// venued against a live exchange (BUILD_A_VENUE.md §3 A5, A7; CONTRACT.md §6; BOUNDED_KEYS.md §8.5
// test 7). Run from minsky/:  build/test_venued [--write-fixture]
//
// Starts build/exchange (tcp 7400) and build/venued (7410), plays the environment itself on book 0
// with bk::print from accounts 0 / 254, and checks deploy, idempotency, the certificate's worst
// path, cancel with flatten, pause, kill -9 + reconcile, and single-run refusals. The scenarios run
// in sequence on one book, each starting after the previous program has halted.
// --write-fixture writes program 7's event stream (plus prints) to ../fixtures/events_v2_worst.jsonl.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <memory>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "env.hpp"
#include "json.hpp"
#include "program_json.hpp"

namespace {

using json::Value;

const char* kStatePath = "/tmp/venued-test.state.json";
const int kExPort = 7400, kExFeed = 7402, kVenuePort = 7410;

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) ++failures;
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
  std::fflush(stdout);
}

uint64_t ms_now() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- processes
pid_t g_exchange = -1, g_venued = -1;

pid_t spawn(const std::vector<std::string>& args) {
  const pid_t p = fork();
  if (p < 0) throw std::runtime_error("fork");
  if (p == 0) {
    std::vector<char*> av;
    for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
    std::perror("execv");
    _exit(127);
  }
  return p;
}
void reap(pid_t& p, int sig) {
  if (p <= 0) return;
  kill(p, sig);
  int st = 0;
  waitpid(p, &st, 0);
  p = -1;
}
void kill_all() {
  reap(g_venued, SIGKILL);
  reap(g_exchange, SIGKILL);
}
void on_fatal(int sig) {
  kill_all();
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

int tcp_connect(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    ::close(fd);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  return fd;
}
bool wait_port(int port, uint64_t ms) {
  const uint64_t t0 = ms_now();
  while (ms_now() - t0 < ms) {
    const int fd = tcp_connect(port);
    if (fd >= 0) {
      ::close(fd);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

pid_t start_venued() {
  const pid_t p = spawn({"./build/venued", "--listen", std::to_string(kVenuePort), "--exchange",
                         "tcp://127.0.0.1:" + std::to_string(kExPort), "--state", kStatePath, "--instruments",
                         "../fixtures/instruments.json", "--quiet"});
  if (!wait_port(kVenuePort, 5000)) throw std::runtime_error("venued did not open its port");
  return p;
}

// ---------------------------------------------------------------- venued clients
// One request -> one response line.
class Conn {
 public:
  Conn() {
    fd_ = tcp_connect(kVenuePort);
    if (fd_ < 0) throw std::runtime_error("cannot connect to venued");
    timeval tv{5, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  ~Conn() {
    if (fd_ >= 0) ::close(fd_);
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  std::string raw(const std::string& line) {
    const std::string s = line + "\n";
    size_t off = 0;
    while (off < s.size()) {
      const ssize_t w = ::send(fd_, s.data() + off, s.size() - off, 0);
      if (w <= 0) throw std::runtime_error("send to venued failed");
      off += static_cast<size_t>(w);
    }
    for (;;) {
      const size_t nl = buf_.find('\n');
      if (nl != std::string::npos) {
        std::string r = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        return r;
      }
      char tmp[65536];
      const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
      if (r <= 0) throw std::runtime_error("no response from venued");
      buf_.append(tmp, static_cast<size_t>(r));
    }
  }
  Value req(const Value& v) { return json::parse(raw(json::compact(v))); }

 private:
  int fd_ = -1;
  std::string buf_;
};

// The event stream.
class Sub {
 public:
  Sub() {
    fd_ = tcp_connect(kVenuePort);
    if (fd_ < 0) throw std::runtime_error("cannot connect to venued");
    const std::string s = "{\"request_id\":\"sub\",\"op\":\"subscribe\"}\n";
    if (::send(fd_, s.data(), s.size(), 0) != static_cast<ssize_t>(s.size())) throw std::runtime_error("subscribe");
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    const uint64_t t0 = ms_now();
    while (!got_ack_ && ms_now() - t0 < 2000) pump();
    if (!got_ack_) throw std::runtime_error("subscribe not acknowledged");
  }
  ~Sub() {
    if (fd_ >= 0) ::close(fd_);
  }
  std::vector<Value> events;
  std::vector<std::string> lines;
  void pump() {
    char tmp[65536];
    for (;;) {
      const ssize_t r = ::recv(fd_, tmp, sizeof tmp, 0);
      if (r <= 0) break;
      buf_.append(tmp, static_cast<size_t>(r));
    }
    size_t nl;
    while ((nl = buf_.find('\n')) != std::string::npos) {
      const std::string line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      const Value v = json::parse(line);
      if (!got_ack_) {
        got_ack_ = v.get("ok") && v.at("ok").as_bool();
        continue;
      }
      events.push_back(v);
      lines.push_back(line);
    }
  }
  // Index of the first event at or after `from` matching `pred`, waiting up to `ms`; -1 if none.
  int wait(size_t from, const std::function<bool(const Value&)>& pred, uint64_t ms = 2000) {
    const uint64_t t0 = ms_now();
    for (;;) {
      pump();
      for (size_t i = from; i < events.size(); ++i)
        if (pred(events[i])) return static_cast<int>(i);
      if (ms_now() - t0 > ms) return -1;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

 private:
  int fd_ = -1;
  std::string buf_;
  bool got_ack_ = false;
};

// ---------------------------------------------------------------- helpers
int64_t I(const Value& v, const char* k) {
  const Value* x = v.get(k);
  return x && x->is_int() ? x->as_int() : INT64_MIN;
}
std::string S(const Value& v, const char* k) {
  const Value* x = v.get(k);
  return x && x->is_string() ? x->as_string() : std::string("<none>");
}
bool ok(const Value& r) { return r.get("ok") && r.at("ok").is_bool() && r.at("ok").as_bool(); }
std::string err(const Value& r) { return S(r, "error"); }
std::vector<std::string> names(const Value& r, const char* k) {
  std::vector<std::string> out;
  if (const Value* a = r.get(k); a && a->is_array())
    for (const auto& x : a->as_array()) out.push_back(x.as_string());
  return out;
}
std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
  return "[" + s + "]";
}
auto is_print(int64_t p) {
  return [p](const Value& e) { return S(e, "type") == "print" && I(e, "price") == p; };
}
auto is_ev(const char* type, int64_t pid, const char* node = nullptr) {
  return [=](const Value& e) {
    return S(e, "type") == type && I(e, "program_id") == pid && (!node || S(e, "node") == node);
  };
}

Value g_compiled;
int g_rid = 0;
std::string rid() { return "r" + std::to_string(++g_rid); }
Value deploy_req(const std::string& id, int64_t pid) {
  return json::obj({{"request_id", id},
                    {"op", "deploy"},
                    {"program_id", static_cast<long long>(pid)},
                    {"account", 1},
                    {"compiled", g_compiled},
                    {"fund_cents", 303734}});
}
Value op_req(const char* op, int64_t pid) {
  return json::obj({{"request_id", rid()}, {"op", op}, {"program_id", static_cast<long long>(pid)}});
}

// The environment: a print on book 0, then wait until venued has reported it.
struct Env {
  bk::WirePort port{net::connect("tcp://127.0.0.1:" + std::to_string(kExPort))};
  void print(int64_t P) { bk::print(port, bk::EnvAccounts{0, 254}, 0, P, false); }
};
void move(Env& env, Sub* sub, int64_t P) {
  const size_t from = sub ? sub->events.size() : 0;
  env.print(P);
  if (sub) check(sub->wait(from, is_print(P)) >= 0, "print " + std::to_string(P) + " event");
}

int run(bool write_fixture) {
  g_compiled = bk::read_json("../fixtures/ex_v2.compiled.json");
  std::remove(kStatePath);
  g_exchange = spawn({"./build/exchange", "--tcp", std::to_string(kExPort), "--feed", std::to_string(kExFeed),
                      "--accounts", "255", "--quiet"});
  if (!wait_port(kExPort, 5000)) throw std::runtime_error("exchange did not open its port");
  g_venued = start_venued();

  auto c = std::make_unique<Conn>();
  auto sub = std::make_unique<Sub>();
  Env env;

  // ---- 1. deploy program 7
  std::printf("-- 1. deploy\n");
  const Value d7req = deploy_req("deploy-7", 7);
  const std::string d7 = c->raw(json::compact(d7req));
  const Value r7 = json::parse(d7);
  check(ok(r7), "deploy 7 ok: " + d7);
  check(I(r7, "sub_account") == 1, "deploy 7 in sub-account 1");
  check(r7.get("order_ids") && r7.at("order_ids").size() == 3 && I(r7.at("order_ids"), "entry") != INT64_MIN &&
            I(r7.at("order_ids"), "sl") != INT64_MIN && I(r7.at("order_ids"), "tp") != INT64_MIN,
        "deploy 7 returns 3 integer order ids");
  check(I(r7, "locked_collateral_cents") == 300000, "locked_collateral_cents 300000");
  {
    const Value s = c->req(op_req("state", 7));
    check(S(s, "status") == "deployed", "state 7: deployed");
    check(join(names(s, "live_nodes")) == "[entry,sl,tp]", "state 7: live_nodes " + join(names(s, "live_nodes")));
    check(I(s, "position") == 0 && I(s, "trades") == 0, "state 7: flat, no trades");
    const Value m = c->req(json::obj({{"request_id", rid()}, {"op", "mark"}, {"instrument", "ETH"}}));
    check(ok(m) && m.get("last_print") && m.at("last_print").is_null(), "mark ETH before any trade: null");
    const Value bad = c->req(json::obj({{"request_id", rid()}, {"op", "mark"}, {"instrument", "DOGE"}}));
    check(!ok(bad) && err(bad) == "bad_request", "mark of an unknown instrument: bad_request");
    const Value u = c->req(op_req("state", 999));
    check(!ok(u) && err(u) == "unknown_program", "state of an unknown program: unknown_program");
  }

  // ---- 2. idempotency
  std::printf("-- 2. idempotency\n");
  check(c->raw(json::compact(d7req)) == d7, "same request_id: identical response");
  {
    const Value again = c->req(deploy_req(rid(), 7));
    check(!ok(again) && err(again) == "already_deployed", "new request_id deploying 7: already_deployed");
  }

  // ---- 3. the certificate's worst path: 3010, gap to 2989, gap to 2964
  std::printf("-- 3. worst path\n");
  const size_t start3 = sub->events.size();
  move(env, sub.get(), 3010);
  {
    const Value m = c->req(json::obj({{"request_id", rid()}, {"op", "mark"}, {"instrument", "ETH"}}));
    check(I(m, "last_print") == 3010, "mark ETH after the first print: 3010");
  }
  move(env, sub.get(), 2989);
  check(sub->wait(start3, is_ev("node.filled", 7, "entry")) >= 0, "entry filled on the 2989 print");
  move(env, sub.get(), 2964);
  const int h7 = sub->wait(start3, is_ev("program.halted", 7));
  check(h7 >= 0, "program 7 halted");
  {
    std::vector<Value> e7;
    for (size_t i = start3; i < sub->events.size() && static_cast<int>(i) <= h7; ++i)
      if (I(sub->events[i], "program_id") == 7) e7.push_back(sub->events[i]);
    std::string seq;
    for (const auto& e : e7) seq += S(e, "type") + "/" + (e.get("node") ? S(e, "node") : "-") + " ";
    check(e7.size() == 4, "program 7: 4 events: " + seq);
    if (e7.size() == 4) {
      const Value &a = e7[0], &b = e7[1], &d = e7[2], &h = e7[3];
      check(S(a, "type") == "node.filled" && S(a, "node") == "entry" && I(a, "qty") == 1 && I(a, "price") == 3000 &&
                I(a, "fee_cents") == 30,
            "1st: node.filled entry 1 @ 3000 fee 30: " + json::compact(a));
      check(S(b, "type") == "node.triggered" && S(b, "node") == "sl" && I(b, "print") == 2964,
            "2nd: node.triggered sl at print 2964: " + json::compact(b));
      check(S(d, "type") == "node.filled" && S(d, "node") == "sl" && I(d, "qty") == 1 && I(d, "price") == 2964 &&
                I(d, "fee_cents") == 104,
            "3rd: node.filled sl 1 @ 2964 fee 104: " + json::compact(d));
      check(S(h, "type") == "program.halted" && I(h, "realized_loss_cents") == 3734 && I(h, "fees_cents") == 134 &&
                I(h, "trades") == 2,
            "4th: program.halted 3734 / 134 / 2: " + json::compact(h));
      for (const auto& e : e7) check(I(e, "contract_version") == 1 && I(e, "t") > 0, "event carries version and t");
    }
  }
  {
    const Value p = c->req(op_req("pause", 7));
    check(!ok(p) && err(p) == "not_live", "pause halted 7: not_live");
    const Value x = c->req(op_req("cancel", 7));
    check(!ok(x) && err(x) == "not_live", "cancel halted 7: not_live");
    const Value d = c->req(deploy_req(rid(), 7));
    check(!ok(d) && err(d) == "already_deployed", "deploy halted 7: already_deployed");
    const Value s = c->req(op_req("state", 7));
    check(ok(s) && S(s, "status") == "halted" && I(s, "realized_loss_cents") == 3734 && I(s, "fees_cents") == 134 &&
              I(s, "trades") == 2 && names(s, "live_nodes").empty() && I(s, "last_print") == 2964,
          "state 7: halted, 3734 / 134 / 2, nothing live, last 2964: " + json::compact(s));
  }
  if (write_fixture && h7 >= 0) {
    std::ofstream f("../fixtures/events_v2_worst.jsonl", std::ios::trunc);
    for (size_t i = start3; static_cast<int>(i) <= h7; ++i) {
      const Value& e = sub->events[i];
      if (S(e, "type") == "print" || I(e, "program_id") == 7) f << sub->lines[i] << "\n";
    }
    std::printf("wrote ../fixtures/events_v2_worst.jsonl\n");
  }

  // ---- 4. cancel with flatten (book at 2964; 7 has halted)
  std::printf("-- 4. cancel\n");
  const size_t start4 = sub->events.size();
  {
    const Value r = c->req(deploy_req(rid(), 8));
    check(ok(r) && I(r, "sub_account") == 1, "deploy 8 in the lowest free sub-account (1): " + json::compact(r));
  }
  move(env, sub.get(), 3020);
  move(env, sub.get(), 3000);
  {
    const int f = sub->wait(start4, is_ev("node.filled", 8, "entry"));
    check(f >= 0 && I(sub->events[static_cast<size_t>(f)], "price") == 3000 &&
              I(sub->events[static_cast<size_t>(f)], "fee_cents") == 30,
          "program 8 entry filled 1 @ 3000 fee 30");
  }
  move(env, sub.get(), 3005);
  {
    const Value x = c->req(op_req("cancel", 8));
    const auto cn = names(x, "cancelled");
    check(ok(x) && cn.size() == 2 && std::find(cn.begin(), cn.end(), "sl") != cn.end() &&
              std::find(cn.begin(), cn.end(), "tp") != cn.end(),
          "cancel 8 cancels sl and tp: " + json::compact(x));
    check(x.get("flatten_fill") && x.at("flatten_fill").is_null(), "cancel 8: flatten_fill null");
    check(x.get("flatten_pending") && x.at("flatten_pending").is_bool() && x.at("flatten_pending").as_bool(),
          "cancel 8: flatten_pending true");
    const Value s = c->req(op_req("state", 8));
    check(S(s, "status") == "cancelled" && join(names(s, "live_nodes")) == "[flatten]" && I(s, "position") == 1,
          "state 8 after cancel: cancelled, flatten live, long 1: " + json::compact(s));
    const Value p = c->req(op_req("pause", 8));
    check(!ok(p) && err(p) == "not_live", "pause cancelled 8: not_live");
  }
  move(env, sub.get(), 2990);
  {
    const int f = sub->wait(start4, is_ev("node.filled", 8, "flatten"));
    check(f >= 0 && I(sub->events[static_cast<size_t>(f)], "price") == 2990 &&
              I(sub->events[static_cast<size_t>(f)], "qty") == 1 &&
              I(sub->events[static_cast<size_t>(f)], "fee_cents") == 105,
          "flatten filled 1 @ 2990 taker fee 105");
    const int h = sub->wait(start4, is_ev("program.halted", 8));
    check(h > f && f >= 0, "program.halted 8 follows the flatten fill");
    if (h >= 0) {
      const Value& e = sub->events[static_cast<size_t>(h)];
      check(I(e, "realized_loss_cents") == 1135 && I(e, "fees_cents") == 135 && I(e, "trades") == 2,
            "program 8 halted 1135 / 135 / 2: " + json::compact(e));
    }
    const Value s = c->req(op_req("state", 8));
    check(S(s, "status") == "cancelled" && names(s, "live_nodes").empty() && I(s, "position") == 0,
          "state 8: still cancelled, flat, nothing live");
    const Value x = c->req(op_req("cancel", 8));
    check(!ok(x) && err(x) == "not_live", "cancel 8 again: not_live");
  }

  // ---- 5. pause before the entry fills (book at 2990)
  std::printf("-- 5. pause\n");
  const size_t start5 = sub->events.size();
  {
    const Value r = c->req(deploy_req(rid(), 9));
    check(ok(r) && I(r, "sub_account") == 1, "deploy 9 in sub-account 1");
    const Value p = c->req(op_req("pause", 9));
    check(ok(p) && join(names(p, "cancelled")) == "[entry]", "pause 9 cancels [entry]: " + json::compact(p));
    const int h = sub->wait(start5, is_ev("program.halted", 9));
    check(h >= 0, "program.halted 9 follows the pause");
    if (h >= 0) {
      const Value& e = sub->events[static_cast<size_t>(h)];
      check(I(e, "realized_loss_cents") == 0 && I(e, "fees_cents") == 0 && I(e, "trades") == 0,
            "program 9 halted 0 / 0 / 0");
    }
    const Value s = c->req(op_req("state", 9));
    check(S(s, "status") == "halted", "state 9: halted");
    const Value x = c->req(op_req("cancel", 9));
    check(!ok(x) && err(x) == "not_live", "cancel 9 after its halt: not_live");
  }

  // ---- 6. kill -9 mid-program, the stop fills while venued is down, then reconcile
  std::printf("-- 6. kill -9\n");
  const size_t start6 = sub->events.size();
  const Value d10req = deploy_req("deploy-10", 10);
  const std::string d10 = c->raw(json::compact(d10req));
  check(ok(json::parse(d10)) && I(json::parse(d10), "sub_account") == 1, "deploy 10 in sub-account 1");
  move(env, sub.get(), 3010);
  move(env, sub.get(), 2989);
  check(sub->wait(start6, is_ev("node.filled", 10, "entry")) >= 0, "program 10 entry filled");
  {
    const Value s = c->req(op_req("state", 10));
    check(I(s, "position") == 1 && join(names(s, "live_nodes")) == "[sl,tp]",
          "state 10 before the kill: long 1, [sl,tp] live");
    // pause after the entry filled: sl and tp are reduce-only (they only close), so both stay
    const Value p = c->req(op_req("pause", 10));
    check(ok(p) && names(p, "cancelled").empty(), "pause 10 after its entry filled cancels nothing: " + json::compact(p));
    const Value s2 = c->req(op_req("state", 10));
    check(S(s2, "status") == "paused" && join(names(s2, "live_nodes")) == "[sl,tp]",
          "state 10 paused: reduce-only sl and tp stay live");
  }
  sub.reset();
  c.reset();
  reap(g_venued, SIGKILL);
  move(env, nullptr, 2964);  // the sl fires and fills on the exchange with venued down
  g_venued = start_venued();
  c = std::make_unique<Conn>();
  {
    const Value r = c->req(json::obj({{"request_id", rid()}, {"op", "reconcile"}}));
    check(ok(r), "reconcile ok");
    const Value* p10 = nullptr;
    if (const Value* ps = r.get("programs"); ps && ps->is_array())
      for (const auto& p : ps->as_array())
        if (I(p, "program_id") == 10) p10 = &p;
    check(p10 != nullptr, "reconcile lists program 10");
    check(r.get("programs") && r.at("programs").size() == 4, "reconcile lists all 4 programs");
    if (p10)
      check(S(*p10, "status") == "halted" && I(*p10, "realized_loss_cents") == 3734 && I(*p10, "fees_cents") == 134 &&
                I(*p10, "trades") == 2 && I(*p10, "position") == 0,
            "reconcile: program 10 halted 3734 / 134 / 2: " + json::compact(*p10));
    const Value s = c->req(op_req("state", 10));
    check(S(s, "status") == "halted" && I(s, "realized_loss_cents") == 3734 && I(s, "fees_cents") == 134 &&
              I(s, "trades") == 2 && names(s, "live_nodes").empty(),
          "state 10 agrees: " + json::compact(s));
    const Value s7 = c->req(op_req("state", 7));
    check(S(s7, "status") == "halted" && I(s7, "realized_loss_cents") == 3734 && I(s7, "trades") == 2,
          "state 7 survives the restart (sub-account reused by 8, 9, 10)");
    const Value s8 = c->req(op_req("state", 8));
    check(S(s8, "status") == "cancelled" && I(s8, "realized_loss_cents") == 1135 && I(s8, "fees_cents") == 135,
          "state 8 survives the restart: cancelled, 1135 / 135");
    check(c->raw(json::compact(d10req)) == d10, "deploy 10's request_id replays its original response after restart");
  }

  // ---- 7. single run at the venue
  std::printf("-- 7. single run\n");
  for (int64_t pid : {7, 8, 9, 10}) {
    const Value d = c->req(deploy_req(rid(), pid));
    check(!ok(d) && err(d) == "already_deployed", "deploy " + std::to_string(pid) + " again: already_deployed");
    for (const char* op : {"pause", "cancel"}) {
      const Value x = c->req(op_req(op, pid));
      check(!ok(x) && err(x) == "not_live", std::string(op) + " " + std::to_string(pid) + ": not_live");
    }
    const Value s = c->req(op_req("state", pid));
    check(ok(s), "state " + std::to_string(pid) + " still answers");
  }
  {
    const Value r = c->req(deploy_req(rid(), 11));
    check(ok(r) && I(r, "sub_account") == 1, "a fresh program 11 deploys into sub-account 1");
    const Value x = c->req(op_req("cancel", 11));
    check(ok(x) && join(names(x, "cancelled")) == "[entry]" && x.at("flatten_pending").is_bool() &&
              !x.at("flatten_pending").as_bool(),
          "cancel 11 before any fill: [entry], nothing to flatten: " + json::compact(x));
    const Value p = c->req(op_req("pause", 11));
    check(!ok(p) && err(p) == "not_live", "pause cancelled 11: not_live");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool write_fixture = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--write-fixture") write_fixture = true;
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, on_fatal);
  std::signal(SIGTERM, on_fatal);
  std::signal(SIGSEGV, on_fatal);
  std::signal(SIGABRT, on_fatal);
  try {
    run(write_fixture);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL: exception: %s\n", e.what());
  }
  kill_all();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
