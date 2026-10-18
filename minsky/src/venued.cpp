// venued: the venue daemon (CONTRACT.md §6, BUILD_A_VENUE.md §3 A5).
//
// One process owns every exchange session for deployed programs. Build B talks to it over TCP,
// newline-delimited JSON, one request -> one response line. A deploy places a CompiledProgram's
// nodes as native exchange orders (parents, OCO groups, stops) in the program's own sub-account;
// from then on the program runs inside the exchange with nobody connected. venued only keeps the
// books: it reads each sub-account's fill journal on the exchange (the single source of truth,
// which also survives a venued restart), turns it into realized loss, fees and trades, and emits
// node / print / halt events to subscribers.
//
// Single threaded: poll() over the listen socket and the clients with a timeout of --poll-ms;
// every --poll-ms a tick syncs every live program from the exchange. Exchange calls are
// synchronous round trips over bk::WirePort (localhost, microseconds).
//
// Sessions: one control session for queries (LastPrice, OrderInfo, FillCount, FillInfo) and one
// session per program sub-account for its submits and cancels. Fills delivered on those sessions
// are ignored; only the journal counts.
//
// State file: rewritten atomically (tmp + rename) after every change. On start it is loaded, and
// every program is rebuilt from the exchange journal by the same code path as `reconcile`.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "env.hpp"
#include "json.hpp"
#include "program_json.hpp"

namespace {

using json::Value;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
int64_t mono_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* kHelp = R"(venued: the venue daemon (CONTRACT.md §6)

usage: venued [--listen PORT] [--exchange URL] [--state PATH] [--instruments PATH]
              [--fees maker=BPS,taker=BPS] [--poll-ms N] [--quiet] [--help]

  --listen PORT        JSON service on 127.0.0.1:PORT                  (default 7010)
  --exchange URL       the live exchange (run it with --accounts 255)  (default tcp://127.0.0.1:7000)
  --state PATH         state file, rewritten atomically after every change (default venued.state.json)
  --instruments PATH   instruments.json (default: $BK_INSTRUMENTS, ./instruments.json,
                       ./fixtures/instruments.json, ../fixtures/instruments.json)
  --fees maker=1.0,taker=3.5   fee bps; fee per fill = ceil(price_cents * qty * bps / 10000)
  --poll-ms N          how often every live program is synced from the exchange (default 5)
  --quiet              no log lines on stderr

Transport: TCP 127.0.0.1:7010, newline-delimited JSON, one request -> one response line.
Every request carries "request_id"; every response is
  {"contract_version":1, "request_id":<echo>, "ok":true|false, ...}
Repeating a request_id returns the original response without re-executing it (responses of
deploy/pause/cancel are kept in the state file; exchange_down answers are not cached, so a retry
runs again).

Requests ("op"):
  deploy     {program_id, account, compiled (CompiledProgram §2.2), fund_cents}
             -> {ok, sub_account, order_ids:{node:id}, locked_collateral_cents}
             Places deploy_plan generation by generation into the lowest free sub-account. A
             rejected order cancels everything already placed and answers exchange_down.
  pause      {program_id}  (status deployed only)
             -> {ok, cancelled:[node]}   every native node that is NOT reduce_only (the nodes
             that can open exposure: entry, scale, hedge) and is resting or held is cancelled
             (its held children go with it); every reduce_only node (stops and takes: they only
             close) stays, armed or held. Status paused.
  cancel     {program_id}  (status deployed or paused)
             -> {ok, cancelled:[node], flatten_fill:{qty,price,fee_cents}|null, flatten_pending}
             Every live node is cancelled; each open position is closed by a native reduce-only
             OCO pair (stop-market one tick through the last print + limit at the last print),
             which fills on the next print in either direction, reported as node "flatten".
             Status cancelled; program.halted follows once flat with nothing live.
  state      {program_id}  (any status)
             -> {ok, program_id, account, sub_account, status, live_nodes:[node], position,
                 positions:{instrument:lots}, realized_loss_cents, mtm_loss_cents, fees_cents,
                 trades, last_print}
  mark       {instrument}  -> {ok, last_print}   (null if the book never traded)
  reconcile  {}            -> {ok, programs:[state...]}   every program rebuilt from the
             exchange's fill journal and order states; halts found here are not emitted as events.
  subscribe  {}            -> {ok:true}, then the connection is a one-way event stream.

status: deployed | paused | halted | cancelled
Errors: {ok:false, error, detail} with error one of
  unknown_program | already_deployed | not_live | exchange_down | bad_request
After program.halted or a successful cancel every op on that program except state is not_live;
a program_id is deployed once only (already_deployed forever after).

Events (subscribe; one JSON object per line, ordered per program; prints before program events):
  {"contract_version":1,"type":"print","instrument":"ETH","price":2964,"t":ms}
  {"contract_version":1,"type":"node.triggered","program_id":7,"node":"sl","print":2964,"t":ms}
  {"contract_version":1,"type":"node.filled","program_id":7,"node":"sl","qty":1,"price":2964,"fee_cents":104,"t":ms}
  {"contract_version":1,"type":"program.halted","program_id":7,"realized_loss_cents":3734,"fees_cents":134,"trades":2,"t":ms}
  realized_loss_cents = max(0, loss) at halt.

Exchange accounts (bk:: constants in env.hpp):
  0          environment, sweeping side (reserved)
  1 .. 252   program sub-accounts, allocated by venued (lowest not used by a live program)
  253        replay's scratch sub-account
  254        environment, liquidity side
OCO group ids are drawn from a persisted counter starting at 0x00100000 (bk::kVenuedOcoBase).
)";

struct Options {
  int listen_port = 7010;
  std::string exchange = "tcp://127.0.0.1:7000";
  std::string state_path = "venued.state.json";
  std::string instruments;
  bk::Fees fees;
  int poll_ms = 5;
  bool quiet = false;
};

// An error answer: {ok:false, error:code, detail}.
struct ReqError {
  std::string code, detail;
};
// The exchange is unreachable or stopped answering.
struct ExDown : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class Status : uint8_t { Deployed, Paused, Halted, Cancelled };
const char* status_name(Status s) {
  switch (s) {
    case Status::Deployed: return "deployed";
    case Status::Paused: return "paused";
    case Status::Halted: return "halted";
    case Status::Cancelled: return "cancelled";
  }
  return "?";
}
Status parse_status(const std::string& s) {
  if (s == "deployed") return Status::Deployed;
  if (s == "paused") return Status::Paused;
  if (s == "halted") return Status::Halted;
  if (s == "cancelled") return Status::Cancelled;
  throw std::runtime_error("bad status \"" + s + "\" in the state file");
}

struct Prog {
  int64_t id = 0, account = 0, fund_cents = 0;
  uint8_t sub = 0;
  Value compiled;
  bk::Program p;
  size_t n = 0, k = 0;              // nodes, instruments
  std::vector<uint32_t> books;      // per program instrument: the exchange book
  std::vector<int64_t> ticks;       // per program instrument: tick_cents
  std::vector<fb::OrderId> ids;     // n nodes, then (flatten stop, flatten limit) per instrument
  std::vector<uint8_t> state;       // last observed proto::OrderState per slot
  std::vector<uint8_t> triggered;   // per node
  std::vector<uint32_t> group_oco;  // per OCO group (Program::groups)
  std::vector<uint32_t> flatten_oco;  // per instrument
  Status status = Status::Deployed;
  int64_t journal_base = 0;  // first journal index that belongs to this program
  int64_t journal_pos = 0;   // next journal index to read
  int64_t journal_end = -1;  // set when the program ended: [base, end) is its whole journal
  bool halted_emitted = false;
  bk::Ledger led;

  // A program is live until it halted (status halted, or cancelled and flattened): its
  // sub-account is in use and the tick syncs it.
  bool live() const { return !halted_emitted; }
  int slot_of(fb::OrderId id) const {
    if (!id) return -1;
    for (size_t i = 0; i < ids.size(); ++i)
      if (ids[i] == id) return static_cast<int>(i);
    return -1;
  }
  int inst_of(size_t s) const { return s < n ? p.nodes[s].instrument_index : static_cast<int>((s - n) / 2); }
  std::string name_of(size_t s) const { return s < n ? p.nodes[s].id : "flatten"; }
  bool is_stop(size_t s) const { return s < n && p.nodes[s].is_stop(); }
};

void init_prog(Prog& g, const bk::Instruments& ins) {
  g.n = g.p.nodes.size();
  g.k = g.p.instruments.size();
  g.books.clear();
  g.ticks.clear();
  for (const auto& name : g.p.instruments) {
    const bk::Instrument& i = ins.at(name);
    g.books.push_back(i.book);
    g.ticks.push_back(i.tick_cents);
  }
  g.ids.assign(g.n + 2 * g.k, 0);
  g.state.assign(g.ids.size(), proto::OrderGone);
  g.triggered.assign(g.n, 0);
  g.group_oco.assign(g.p.groups.size(), 0);
  g.flatten_oco.assign(g.k, 0);
  g.led = bk::Ledger(g.k);
}

Value ledger_json(const bk::Ledger& l) {
  Value pos = Value::array(), cost = Value::array();
  for (int64_t x : l.pos) pos.push(static_cast<long long>(x));
  for (int64_t x : l.cost) cost.push(static_cast<long long>(x));
  return json::obj({{"cash_cents", static_cast<long long>(l.cash_cents)},
                    {"fees_cents", static_cast<long long>(l.fees_cents)},
                    {"trades", static_cast<long long>(l.trades)},
                    {"max_exposure", static_cast<long long>(l.max_exposure)},
                    {"pos", pos},
                    {"cost", cost}});
}
void ledger_from(bk::Ledger& l, const Value& v, size_t k) {
  l = bk::Ledger(k);
  l.cash_cents = v.int_or("cash_cents", 0);
  l.fees_cents = v.int_or("fees_cents", 0);
  l.trades = v.int_or("trades", 0);
  l.max_exposure = v.int_or("max_exposure", 0);
  if (const Value* p = v.get("pos"))
    for (size_t i = 0; i < k && i < p->size(); ++i) l.pos[i] = (*p)[i].as_int();
  if (const Value* c = v.get("cost"))
    for (size_t i = 0; i < k && i < c->size(); ++i) l.cost[i] = (*c)[i].as_int();
}

struct Client {
  int fd = -1;
  std::string in, out;
  size_t out_off = 0;
  bool subscriber = false;
  bool read_closed = false;
  bool dead = false;
};

constexpr size_t kMaxLine = 16u << 20;      // a request line longer than this closes the client
constexpr size_t kMaxBacklog = 64u << 20;   // a subscriber that stops reading is dropped
constexpr size_t kReadOnlyCache = 4096;     // cached responses of non-mutating ops (in memory)

class Venue {
 public:
  Venue(Options o, bk::Instruments ins) : opt_(std::move(o)), ins_(std::move(ins)) {}

  void load_state();
  void listen();
  void run();
  void save();

 private:
  // ------------------------------------------------------------------ logging
  template <class... A>
  void log(const char* fmt, A... a) {
    if (opt_.quiet) return;
    std::fprintf(stderr, "venued: ");
    if constexpr (sizeof...(A) == 0) std::fputs(fmt, stderr);
    else std::fprintf(stderr, fmt, a...);
    std::fputc('\n', stderr);
  }

  // ------------------------------------------------------------------ exchange
  std::unique_ptr<bk::WirePort> make_port() {
    auto p = std::make_unique<bk::WirePort>(net::connect(opt_.exchange), 2000);
    p->on_other = [](const proto::Any&) {};
    return p;
  }
  void drop_exchange(const std::string& why) {
    if (ctl_) log("exchange lost: %s", why.c_str());
    ctl_.reset();
    ports_.clear();
    ex_error_ = why;
    next_connect_ms_ = mono_ms() + 250;
  }
  // Connects the control session if needed. `force` ignores the reconnect backoff (requests).
  // Runs the startup rebuild once the exchange is first reachable.
  void ensure(bool force) {
    if (!ctl_) {
      if (!force && mono_ms() < next_connect_ms_) throw ExDown("exchange down: " + ex_error_);
      try {
        ctl_ = make_port();
        ctl_->control(proto::Ping);
      } catch (const std::exception& e) {
        ctl_.reset();
        ex_error_ = e.what();
        next_connect_ms_ = mono_ms() + 250;
        throw ExDown("exchange down: " + ex_error_);
      }
      log("connected to %s", opt_.exchange.c_str());
    }
    if (need_rebuild_) {
      rebuild_all();
      need_rebuild_ = false;
      save();
    }
  }
  template <class F>
  auto call(F&& f) -> decltype(f()) {
    try {
      if (!ctl_) throw ExDown("exchange down: " + ex_error_);
      return f();
    } catch (const ExDown&) {
      throw;
    } catch (const std::exception& e) {
      drop_exchange(e.what());
      throw ExDown(std::string("exchange: ") + e.what());
    }
  }
  bk::WirePort& port_for(uint8_t sub) {
    auto& p = ports_[sub];
    if (!p) p = make_port();
    return *p;
  }
  int64_t x_fill_count(uint8_t a) {
    return call([&] { return ctl_->fill_count(a); });
  }
  bk::WirePort::JFill x_fill_at(uint8_t a, int64_t i) {
    return call([&] { return ctl_->fill_at(a, i); });
  }
  uint8_t x_state(fb::OrderId id) {
    return call([&] { return ctl_->order_state(id); });
  }
  std::optional<int64_t> x_last(uint32_t book) {
    auto r = call([&] { return ctl_->last(book); });
    last_seen_[book] = r;
    return r;
  }
  fb::Result x_submit(uint8_t sub, const proto::NewOrderMsg& m) {
    return call([&] { return port_for(sub).submit(m); });
  }
  fb::Result x_cancel(uint8_t sub, fb::OrderId id) {
    return call([&] { return port_for(sub).cancel(id); });
  }

  // ------------------------------------------------------------------ events
  void emit(const Value& ev) {
    const std::string line = json::compact(ev) + "\n";
    for (auto& c : clients_)
      if (c.subscriber && !c.dead) c.out += line;
  }
  Value event(const char* type) { return json::obj({{"contract_version", bk::kContractVersion}, {"type", type}}); }

  // ------------------------------------------------------------------ accounting
  // Reads journal entries [journal_pos, fill_count) into the ledger. Returns the count read at.
  int64_t read_journal(Prog& g, bool emit_events) {
    const int64_t n = x_fill_count(g.sub);
    for (int64_t i = g.journal_pos; i < n; ++i) {
      const auto f = x_fill_at(g.sub, i);
      apply_fill(g, f, emit_events);
      g.journal_pos = i + 1;
      dirty_ = true;
    }
    return n;
  }
  void apply_fill(Prog& g, const bk::WirePort::JFill& f, bool emit_events) {
    const int s = g.slot_of(f.id);
    if (s < 0) {
      log("program %lld: journal fill for unknown order %llu ignored", (long long)g.id, (unsigned long long)f.id);
      return;
    }
    const size_t slot = static_cast<size_t>(s);
    const int inst = g.inst_of(slot);
    if (g.is_stop(slot) && !g.triggered[slot]) {
      g.triggered[slot] = 1;
      if (emit_events) emit_triggered(g, slot);
    }
    const int64_t fee = g.led.fill(inst, f.side, f.px, f.q, f.maker, g.ticks[inst], opt_.fees);
    if (emit_events) {
      Value ev = event("node.filled");
      ev.set("program_id", static_cast<long long>(g.id));
      ev.set("node", g.name_of(slot));
      ev.set("qty", static_cast<long long>(f.q));
      ev.set("price", static_cast<long long>(f.px));
      ev.set("fee_cents", static_cast<long long>(fee));
      ev.set("t", static_cast<long long>(now_ms()));
      emit(ev);
    }
  }
  void emit_triggered(Prog& g, size_t slot) {
    const auto last = x_last(g.books[g.inst_of(slot)]);
    Value ev = event("node.triggered");
    ev.set("program_id", static_cast<long long>(g.id));
    ev.set("node", g.name_of(slot));
    ev.set("print", last ? Value(static_cast<long long>(*last)) : Value(nullptr));
    ev.set("t", static_cast<long long>(now_ms()));
    emit(ev);
  }
  // Re-reads the order state of every slot not already gone (gone is final for an order id), and
  // reports stops that have fired (now resting, or queued to fire) as triggered.
  void read_states(Prog& g, bool emit_events) {
    for (size_t s = 0; s < g.ids.size(); ++s) {
      if (!g.ids[s]) {
        g.state[s] = proto::OrderGone;
        continue;
      }
      if (g.state[s] == proto::OrderGone) continue;
      const uint8_t prev = g.state[s];
      const uint8_t now = x_state(g.ids[s]);
      g.state[s] = now;
      if (g.is_stop(s) && !g.triggered[s] && (prev == proto::OrderPending || prev == proto::OrderHeld) &&
          (now == proto::OrderResting || now == proto::OrderQueued)) {
        g.triggered[s] = 1;
        dirty_ = true;
        if (emit_events) emit_triggered(g, s);
      }
      if (now != prev) dirty_ = true;
    }
  }
  bool any_live(const Prog& g) const {
    for (size_t s = 0; s < g.ids.size(); ++s)
      if (g.ids[s] && g.state[s] != proto::OrderGone) return true;
    return false;
  }
  static bool flat(const Prog& g) {
    for (int64_t x : g.led.pos)
      if (x) return false;
    return true;
  }
  void check_halt(Prog& g, bool emit_events) {
    if (g.halted_emitted || any_live(g) || !flat(g)) return;
    if (g.status != Status::Cancelled) g.status = Status::Halted;
    g.halted_emitted = true;
    g.journal_end = g.journal_pos;
    dirty_ = true;
    ports_.erase(g.sub);
    log("program %lld halted (%s)", (long long)g.id, status_name(g.status));
    if (emit_events) {
      Value ev = event("program.halted");
      ev.set("program_id", static_cast<long long>(g.id));
      ev.set("realized_loss_cents", static_cast<long long>(std::max<int64_t>(0, g.led.realized_loss())));
      ev.set("fees_cents", static_cast<long long>(g.led.fees_cents));
      ev.set("trades", static_cast<long long>(g.led.trades));
      ev.set("t", static_cast<long long>(now_ms()));
      emit(ev);
    }
  }
  // One sync of a live program: new journal fills, node states, halt. The halt is only decided
  // when no fill arrived while the states were being read, so the ledger and the states agree.
  void sync(Prog& g, bool emit_events) {
    if (!g.live()) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
      const int64_t n = read_journal(g, emit_events);
      read_states(g, emit_events);
      if (x_fill_count(g.sub) == n) {
        check_halt(g, emit_events);
        return;
      }
    }
  }
  // The reconcile path: the ledger and node states rebuilt from the exchange, no events.
  void rebuild(Prog& g) {
    const int64_t count = x_fill_count(g.sub);
    const int64_t end = g.live() ? count : g.journal_end;
    const int64_t need = g.live() ? g.journal_pos : g.journal_end;
    if (count < need) {
      // The exchange lost this account's history (it restarted): keep the persisted ledger.
      log("program %lld: exchange journal shorter than recorded (%lld < %lld); keeping saved ledger", (long long)g.id,
          (long long)count, (long long)need);
      if (g.live()) g.journal_pos = std::min(g.journal_pos, count);
    } else {
      g.led = bk::Ledger(g.k);
      g.journal_pos = g.journal_base;
      for (int64_t i = g.journal_base; i < end; ++i) apply_fill(g, x_fill_at(g.sub, i), false);
      g.journal_pos = end;
    }
    dirty_ = true;
    if (!g.live()) return;
    for (size_t s = 0; s < g.ids.size(); ++s)
      if (g.ids[s]) g.state[s] = proto::OrderPending;  // re-read every slot
    for (int attempt = 0; attempt < 3; ++attempt) {
      const int64_t n = read_journal(g, false);
      for (size_t s = 0; s < g.ids.size(); ++s) {
        if (!g.ids[s] || g.state[s] == proto::OrderGone) continue;
        g.state[s] = x_state(g.ids[s]);
        if (g.is_stop(s) && (g.state[s] == proto::OrderResting || g.state[s] == proto::OrderQueued))
          g.triggered[s] = 1;
      }
      if (x_fill_count(g.sub) == n) {
        check_halt(g, false);
        return;
      }
    }
  }
  void rebuild_all() {
    for (auto& [id, g] : progs_) rebuild(g);
    log("rebuilt %zu program(s) from the exchange", progs_.size());
  }

  // ------------------------------------------------------------------ tick
  void tick() {
    bool any = need_rebuild_;
    for (auto& [id, g] : progs_)
      if (g.live()) any = true;
    if (!any) return;
    try {
      ensure(false);
      for (auto& [sub, port] : ports_) call([&] { port->poll(); });
      std::set<uint32_t> books;
      for (auto& [id, g] : progs_)
        if (g.live())
          for (uint32_t b : g.books) books.insert(b);
      for (uint32_t b : books) {
        const auto prev = printed_.find(b);
        const auto last = x_last(b);
        if (!last) continue;
        if (prev != printed_.end() && prev->second == *last) continue;
        printed_[b] = *last;
        Value ev = event("print");
        ev.set("instrument", ins_.list.at(b).name);
        ev.set("price", static_cast<long long>(*last));
        ev.set("t", static_cast<long long>(now_ms()));
        emit(ev);
      }
      for (auto& [id, g] : progs_)
        if (g.live()) sync(g, true);
    } catch (const ExDown&) {
    }
    if (dirty_) save();
  }

  // ------------------------------------------------------------------ requests
  Value respond_ok(const Value& rid) {
    return json::obj({{"contract_version", bk::kContractVersion}, {"request_id", rid}, {"ok", true}});
  }
  Value respond_err(const Value& rid, const ReqError& e) {
    return json::obj({{"contract_version", bk::kContractVersion},
                      {"request_id", rid},
                      {"ok", false},
                      {"error", e.code},
                      {"detail", e.detail}});
  }
  static int64_t req_int(const Value& r, const char* k) {
    const Value* v = r.get(k);
    if (!v || !v->is_int()) throw ReqError{"bad_request", std::string("\"") + k + "\" must be an integer"};
    return v->as_int();
  }
  Prog& find_prog(const Value& r) {
    const int64_t id = req_int(r, "program_id");
    auto it = progs_.find(id);
    if (it == progs_.end()) throw ReqError{"unknown_program", "no program " + std::to_string(id)};
    return it->second;
  }

  // Returns the response line for one request line, and whether the client now subscribes.
  std::string handle_line(const std::string& line, bool& subscribe) {
    Value req;
    try {
      req = json::parse(line);
    } catch (const std::exception& e) {
      return json::compact(respond_err(nullptr, {"bad_request", std::string("bad JSON: ") + e.what()})) + "\n";
    }
    const Value rid = req.is_object() && req.get("request_id") ? req.at("request_id") : Value(nullptr);
    if (!req.is_object() || !req.get("request_id") || rid.is_null())
      return json::compact(respond_err(rid, {"bad_request", "every request needs a request_id"})) + "\n";
    const std::string op = req.str_or("op", "");
    if (op == "subscribe") {
      subscribe = true;
      return json::compact(respond_ok(rid)) + "\n";
    }
    const std::string key = json::compact(rid);
    if (auto it = cached_.find(key); it != cached_.end()) return json::compact(it->second) + "\n";
    if (auto it = ro_cached_.find(key); it != ro_cached_.end()) return json::compact(it->second) + "\n";
    const bool mutating = op == "deploy" || op == "pause" || op == "cancel";
    Value resp;
    bool cache = true;
    try {
      Value r = respond_ok(rid);
      if (op == "deploy") op_deploy(req, r);
      else if (op == "pause") op_pause(req, r);
      else if (op == "cancel") op_cancel(req, r);
      else if (op == "state") op_state(req, r);
      else if (op == "mark") op_mark(req, r);
      else if (op == "reconcile") op_reconcile(r);
      else throw ReqError{"bad_request", "unknown op \"" + op + "\""};
      resp = r;
    } catch (const ReqError& e) {
      resp = respond_err(rid, e);
    } catch (const ExDown& e) {
      resp = respond_err(rid, {"exchange_down", e.what()});
      cache = false;
    } catch (const std::exception& e) {
      resp = respond_err(rid, {"bad_request", e.what()});
    }
    if (cache) {
      if (mutating) {
        cached_[key] = resp;
        cached_order_.push_back(key);
        dirty_ = true;
      } else {
        ro_cached_[key] = resp;
        ro_order_.push_back(key);
        if (ro_order_.size() > kReadOnlyCache) {
          ro_cached_.erase(ro_order_.front());
          ro_order_.pop_front();
        }
      }
    }
    if (dirty_) save();
    return json::compact(resp) + "\n";
  }

  uint8_t free_sub() const {
    std::set<uint8_t> used;
    for (const auto& [id, g] : progs_)
      if (g.live()) used.insert(g.sub);
    for (unsigned a = 1; a <= bk::kMaxProgramAccount; ++a)
      if (!used.count(static_cast<uint8_t>(a))) return static_cast<uint8_t>(a);
    return 0;
  }

  void op_deploy(const Value& req, Value& r) {
    const int64_t pid = req_int(req, "program_id");
    const int64_t account = req_int(req, "account");
    const int64_t fund = req.get("fund_cents") ? req_int(req, "fund_cents") : 0;
    if (progs_.count(pid)) throw ReqError{"already_deployed", "program " + std::to_string(pid) + " was deployed before"};
    const Value* compiled = req.get("compiled");
    if (!compiled || !compiled->is_object()) throw ReqError{"bad_request", "\"compiled\" must be a CompiledProgram"};
    Prog g;
    try {
      g.p = bk::load_program(*compiled, ins_);
    } catch (const std::exception& e) {
      throw ReqError{"bad_request", std::string("compiled: ") + e.what()};
    }
    if (g.p.backend != "minsky") throw ReqError{"bad_request", "backend \"" + g.p.backend + "\" is not served here"};
    g.id = pid;
    g.account = account;
    g.fund_cents = fund;
    g.compiled = *compiled;
    init_prog(g, ins_);
    ensure(true);
    g.sub = free_sub();
    if (!g.sub) throw ReqError{"exchange_down", "no free sub-account (all 252 hold live programs)"};
    for (auto& o : g.group_oco) o = next_oco_++;
    for (auto& o : g.flatten_oco) o = next_oco_++;
    dirty_ = true;
    g.journal_base = x_fill_count(g.sub);
    g.journal_pos = g.journal_base;
    std::vector<size_t> placed;
    auto rollback = [&] {
      for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        try {
          x_cancel(g.sub, g.ids[*it]);
        } catch (const std::exception&) {
        }
      }
    };
    for (const auto& gen : g.p.deploy_plan) {
      for (const auto& name : gen) {
        const size_t i = static_cast<size_t>(g.p.find(name));
        const bk::Node& nd = g.p.nodes[i];
        const fb::OrderId parent = nd.parent_index >= 0 ? g.ids[static_cast<size_t>(nd.parent_index)] : 0;
        const uint32_t oco = nd.group_index >= 0 ? g.group_oco[static_cast<size_t>(nd.group_index)] : 0;
        const auto m = bk::order_for(nd, g.sub, g.books[static_cast<size_t>(nd.instrument_index)], parent, oco, 0);
        fb::Result res;
        try {
          res = x_submit(g.sub, m);
        } catch (const ExDown&) {
          rollback();
          throw;
        }
        if (res.status == proto::Rejected) {
          rollback();
          throw ReqError{"exchange_down", "the exchange rejected node \"" + name + "\"; nothing was left placed"};
        }
        g.ids[i] = res.id;
        g.state[i] = nd.is_stop() ? proto::OrderPending : proto::OrderResting;  // re-read on the next sync
        placed.push_back(i);
      }
    }
    g.status = Status::Deployed;
    Value ids = Value::object();
    for (size_t i = 0; i < g.n; ++i)
      if (g.ids[i]) ids.set(g.p.nodes[i].id, static_cast<long long>(g.ids[i]));
    r.set("program_id", static_cast<long long>(pid));
    r.set("sub_account", static_cast<int>(g.sub));
    r.set("order_ids", ids);
    r.set("locked_collateral_cents", static_cast<long long>(bk::locked_collateral_cents(g.p, ins_)));
    log("program %lld deployed in sub-account %d", (long long)pid, int(g.sub));
    auto& stored = progs_[pid] = std::move(g);
    dirty_ = true;
    try {
      read_states(stored, false);
    } catch (const ExDown&) {
    }
  }

  void op_pause(const Value& req, Value& r) {
    Prog& g = find_prog(req);
    if (g.status != Status::Deployed)
      throw ReqError{"not_live", "program " + std::to_string(g.id) + " is " + status_name(g.status)};
    ensure(true);
    sync(g, true);
    if (!g.live()) throw ReqError{"not_live", "program " + std::to_string(g.id) + " has halted"};
    Value cancelled = Value::array();
    for (size_t i = 0; i < g.n; ++i) {
      const bk::Node& nd = g.p.nodes[i];
      // only nodes that can open exposure go; reduce-only nodes (stops and takes) only close
      if (!nd.native() || nd.reduce_only || !g.ids[i]) continue;
      const uint8_t st = x_state(g.ids[i]);
      g.state[i] = st;
      if (st != proto::OrderResting && st != proto::OrderHeld) continue;
      if (x_cancel(g.sub, g.ids[i]).status == proto::Cancelled) cancelled.push(nd.id);
      g.state[i] = proto::OrderGone;
    }
    g.status = Status::Paused;
    dirty_ = true;
    r.set("cancelled", cancelled);
  }

  void op_cancel(const Value& req, Value& r) {
    Prog& g = find_prog(req);
    if (g.status != Status::Deployed && g.status != Status::Paused)
      throw ReqError{"not_live", "program " + std::to_string(g.id) + " is " + status_name(g.status)};
    ensure(true);
    sync(g, true);
    if (!g.live()) throw ReqError{"not_live", "program " + std::to_string(g.id) + " has halted"};
    Value cancelled = Value::array();
    for (size_t i = 0; i < g.n; ++i) {
      if (!g.ids[i] || g.state[i] == proto::OrderGone) continue;
      const uint8_t st = x_state(g.ids[i]);
      g.state[i] = st;
      if (st == proto::OrderGone) continue;
      if (x_cancel(g.sub, g.ids[i]).status == proto::Cancelled) cancelled.push(g.p.nodes[i].id);
      g.state[i] = proto::OrderGone;
    }
    read_journal(g, true);  // fills that raced the cancels
    for (size_t inst = 0; inst < g.k; ++inst) {
      const int64_t pos = g.led.pos[inst];
      const size_t s = g.n + 2 * inst;
      if (pos == 0) continue;
      if ((g.ids[s] && x_state(g.ids[s]) != proto::OrderGone) ||
          (g.ids[s + 1] && x_state(g.ids[s + 1]) != proto::OrderGone))
        continue;  // a flatten from an earlier, interrupted attempt is still working
      const auto last = x_last(g.books[inst]);
      if (!last) continue;  // a position without a print cannot happen: a fill is a trade
      auto& port = call([&]() -> bk::WirePort& { return port_for(g.sub); });
      const bk::FlattenIds f =
          call([&] { return bk::flatten(port, g.sub, g.books[inst], pos, *last, g.flatten_oco[inst]); });
      g.ids[s] = f.stop;
      g.ids[s + 1] = f.limit;
      g.state[s] = f.stop ? proto::OrderPending : proto::OrderGone;
      g.state[s + 1] = f.limit ? proto::OrderResting : proto::OrderGone;
      dirty_ = true;
    }
    g.status = Status::Cancelled;
    dirty_ = true;
    // a flatten that crossed on placement
    const int64_t led_fees = g.led.fees_cents;
    const int64_t n0 = g.journal_pos;
    int64_t fq = 0, fpx = 0;
    {
      const int64_t n = x_fill_count(g.sub);
      for (int64_t i = n0; i < n; ++i) {
        const auto f = x_fill_at(g.sub, i);
        const int s = g.slot_of(f.id);
        if (s >= 0 && static_cast<size_t>(s) >= g.n) {
          fq += f.q;
          fpx = f.px;
        }
        apply_fill(g, f, true);
        g.journal_pos = i + 1;
      }
    }
    read_states(g, true);
    bool pending = false;
    for (size_t s = g.n; s < g.ids.size(); ++s)
      if (g.ids[s] && g.state[s] != proto::OrderGone) pending = true;
    r.set("cancelled", cancelled);
    if (fq)
      r.set("flatten_fill", json::obj({{"qty", static_cast<long long>(fq)},
                                       {"price", static_cast<long long>(fpx)},
                                       {"fee_cents", static_cast<long long>(g.led.fees_cents - led_fees)}}));
    else
      r.set("flatten_fill", nullptr);
    r.set("flatten_pending", pending);
  }

  Value state_json(const Prog& g, const std::vector<std::optional<int64_t>>& marks) const {
    Value live = Value::array();
    for (size_t s = 0; s < g.ids.size(); ++s) {
      if (!g.ids[s] || g.state[s] == proto::OrderGone) continue;
      const std::string nm = g.name_of(s);
      bool dup = false;
      for (const auto& x : live.as_array()) dup = dup || x.as_string() == nm;
      if (!dup) live.push(nm);
    }
    Value positions = Value::object();
    for (size_t i = 0; i < g.k; ++i) positions.set(g.p.instruments[i], static_cast<long long>(g.led.pos[i]));
    return json::obj({{"program_id", static_cast<long long>(g.id)},
                      {"account", static_cast<long long>(g.account)},
                      {"sub_account", static_cast<int>(g.sub)},
                      {"status", status_name(g.status)},
                      {"live_nodes", live},
                      {"position", static_cast<long long>(g.led.pos.empty() ? 0 : g.led.pos[0])},
                      {"positions", positions},
                      {"realized_loss_cents", static_cast<long long>(g.led.realized_loss())},
                      {"mtm_loss_cents", static_cast<long long>(g.led.loss(marks, g.ticks))},
                      {"fees_cents", static_cast<long long>(g.led.fees_cents)},
                      {"trades", static_cast<long long>(g.led.trades)},
                      {"last_print", marks.empty() || !marks[0] ? Value(nullptr) : Value(static_cast<long long>(*marks[0]))}});
  }
  // Last prints of a program's books: from the exchange, or (exchange down) the last ones seen.
  std::vector<std::optional<int64_t>> marks_of(const Prog& g, bool live_read) {
    std::vector<std::optional<int64_t>> m(g.k);
    for (size_t i = 0; i < g.k; ++i) {
      if (live_read) m[i] = x_last(g.books[i]);
      else if (auto it = last_seen_.find(g.books[i]); it != last_seen_.end()) m[i] = it->second;
    }
    return m;
  }

  void op_state(const Value& req, Value& r) {
    Prog& g = find_prog(req);
    std::vector<std::optional<int64_t>> marks;
    try {
      ensure(true);
      sync(g, true);
      marks = marks_of(g, true);
    } catch (const ExDown&) {
      if (g.live()) throw;  // a live program's state cannot be known without the exchange
      marks = marks_of(g, false);
    }
    const Value st = state_json(g, marks);
    for (const auto& [k, v] : st.as_object()) r.set(k, v);
  }

  void op_mark(const Value& req, Value& r) {
    const Value* v = req.get("instrument");
    if (!v || !v->is_string()) throw ReqError{"bad_request", "\"instrument\" must be a string"};
    const bk::Instrument* i = ins_.find(v->as_string());
    if (!i) throw ReqError{"bad_request", "unknown instrument \"" + v->as_string() + "\""};
    ensure(true);
    const auto last = x_last(i->book);
    r.set("instrument", i->name);
    r.set("last_print", last ? Value(static_cast<long long>(*last)) : Value(nullptr));
  }

  void op_reconcile(Value& r) {
    ensure(true);
    rebuild_all();
    Value progs = Value::array();
    for (auto& [id, g] : progs_) progs.push(state_json(g, marks_of(g, true)));
    r.set("programs", progs);
  }

  // ------------------------------------------------------------------ clients
  void accept_clients();
  void read_client(Client& c);
  void flush_client(Client& c);

  Options opt_;
  bk::Instruments ins_;
  std::map<int64_t, Prog> progs_;
  uint32_t next_oco_ = bk::kVenuedOcoBase;
  std::map<std::string, Value> cached_;  // mutating ops, persisted
  std::vector<std::string> cached_order_;
  std::map<std::string, Value> ro_cached_;  // read-only ops, bounded, memory only
  std::deque<std::string> ro_order_;
  std::map<uint32_t, int64_t> printed_;                  // last print event per book
  std::map<uint32_t, std::optional<int64_t>> last_seen_; // last LastPrice read per book
  std::unique_ptr<bk::WirePort> ctl_;
  std::map<uint8_t, std::unique_ptr<bk::WirePort>> ports_;
  int64_t next_connect_ms_ = 0;
  std::string ex_error_ = "not connected";
  bool need_rebuild_ = true;
  bool dirty_ = false;
  int listen_fd_ = -1;
  std::vector<Client> clients_;
};

// ---------------------------------------------------------------------------- persistence

void Venue::save() {
  Value progs = Value::array();
  for (const auto& [id, g] : progs_) {
    Value ids = Value::object();
    for (size_t i = 0; i < g.n; ++i)
      if (g.ids[i]) ids.set(g.p.nodes[i].id, static_cast<long long>(g.ids[i]));
    Value groups = Value::object();
    for (size_t i = 0; i < g.group_oco.size(); ++i) groups.set(g.p.groups[i], static_cast<long long>(g.group_oco[i]));
    Value foco = Value::array(), fl = Value::array(), trig = Value::array();
    for (uint32_t o : g.flatten_oco) foco.push(static_cast<long long>(o));
    for (size_t i = 0; i < g.k; ++i)
      fl.push(json::obj({{"stop", static_cast<long long>(g.ids[g.n + 2 * i])},
                         {"limit", static_cast<long long>(g.ids[g.n + 2 * i + 1])}}));
    for (size_t i = 0; i < g.n; ++i)
      if (g.triggered[i]) trig.push(g.p.nodes[i].id);
    progs.push(json::obj({{"program_id", static_cast<long long>(g.id)},
                          {"account", static_cast<long long>(g.account)},
                          {"sub_account", static_cast<int>(g.sub)},
                          {"fund_cents", static_cast<long long>(g.fund_cents)},
                          {"status", status_name(g.status)},
                          {"order_ids", ids},
                          {"oco_ids", groups},
                          {"flatten_oco", foco},
                          {"flatten", fl},
                          {"journal_base", static_cast<long long>(g.journal_base)},
                          {"journal_pos", static_cast<long long>(g.journal_pos)},
                          {"journal_end", static_cast<long long>(g.journal_end)},
                          {"halted_emitted", g.halted_emitted},
                          {"triggered", trig},
                          {"ledger", ledger_json(g.led)},
                          {"compiled", g.compiled}}));
  }
  Value resp = Value::array();
  for (const auto& k : cached_order_) resp.push(json::obj({{"key", k}, {"response", cached_.at(k)}}));
  const Value doc = json::obj({{"contract_version", bk::kContractVersion},
                               {"next_oco", static_cast<long long>(next_oco_)},
                               {"programs", progs},
                               {"responses", resp}});
  const std::string tmp = opt_.state_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      log("cannot write %s", tmp.c_str());
      return;
    }
    f << json::compact(doc) << '\n';
    f.flush();
    if (!f) {
      log("write to %s failed", tmp.c_str());
      return;
    }
  }
  if (std::rename(tmp.c_str(), opt_.state_path.c_str()) != 0) {
    log("rename %s: %s", tmp.c_str(), std::strerror(errno));
    return;
  }
  dirty_ = false;
}

void Venue::load_state() {
  std::ifstream f(opt_.state_path, std::ios::binary);
  if (!f) {
    log("no state file %s; starting empty", opt_.state_path.c_str());
    return;
  }
  const Value doc = bk::read_json(opt_.state_path);
  next_oco_ = static_cast<uint32_t>(doc.int_or("next_oco", bk::kVenuedOcoBase));
  if (const Value* ps = doc.get("programs")) {
    for (const auto& v : ps->as_array()) {
      Prog g;
      g.compiled = v.at("compiled");
      g.p = bk::load_program(g.compiled, ins_);
      init_prog(g, ins_);
      g.id = v.at("program_id").as_int();
      g.account = v.int_or("account", 0);
      g.sub = static_cast<uint8_t>(v.at("sub_account").as_int());
      g.fund_cents = v.int_or("fund_cents", 0);
      g.status = parse_status(v.at("status").as_string());
      for (const auto& [name, id] : v.at("order_ids").as_object()) {
        const int i = g.p.find(name);
        if (i >= 0) g.ids[static_cast<size_t>(i)] = static_cast<fb::OrderId>(id.as_int());
      }
      for (const auto& [label, id] : v.at("oco_ids").as_object())
        for (size_t i = 0; i < g.p.groups.size(); ++i)
          if (g.p.groups[i] == label) g.group_oco[i] = static_cast<uint32_t>(id.as_int());
      const Value& foco = v.at("flatten_oco");
      for (size_t i = 0; i < g.k && i < foco.size(); ++i) g.flatten_oco[i] = static_cast<uint32_t>(foco[i].as_int());
      const Value& fl = v.at("flatten");
      for (size_t i = 0; i < g.k && i < fl.size(); ++i) {
        g.ids[g.n + 2 * i] = static_cast<fb::OrderId>(fl[i].int_or("stop", 0));
        g.ids[g.n + 2 * i + 1] = static_cast<fb::OrderId>(fl[i].int_or("limit", 0));
      }
      g.journal_base = v.int_or("journal_base", 0);
      g.journal_pos = v.int_or("journal_pos", g.journal_base);
      g.journal_end = v.int_or("journal_end", -1);
      g.halted_emitted = v.at("halted_emitted").as_bool();
      for (const auto& t : v.at("triggered").as_array()) {
        const int i = g.p.find(t.as_string());
        if (i >= 0) g.triggered[static_cast<size_t>(i)] = 1;
      }
      if (const Value* l = v.get("ledger")) ledger_from(g.led, *l, g.k);
      for (size_t s = 0; s < g.ids.size(); ++s)
        g.state[s] = g.ids[s] && g.live() ? proto::OrderPending : proto::OrderGone;
      progs_[g.id] = std::move(g);
    }
  }
  if (const Value* rs = doc.get("responses")) {
    for (const auto& e : rs->as_array()) {
      const std::string k = e.at("key").as_string();
      if (!cached_.count(k)) cached_order_.push_back(k);
      cached_[k] = e.at("response");
    }
  }
  log("loaded %zu program(s) and %zu cached response(s) from %s", progs_.size(), cached_.size(),
      opt_.state_path.c_str());
}

// ---------------------------------------------------------------------------- sockets

void set_nonblock(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void Venue::listen() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket");
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(opt_.listen_port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
    throw std::runtime_error("bind 127.0.0.1:" + std::to_string(opt_.listen_port) + ": " + std::strerror(errno));
  if (::listen(listen_fd_, 64) < 0) throw std::runtime_error("listen");
  set_nonblock(listen_fd_);
  log("listening on 127.0.0.1:%d, exchange %s", opt_.listen_port, opt_.exchange.c_str());
}

void Venue::accept_clients() {
  for (;;) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    Client c;
    c.fd = fd;
    clients_.push_back(std::move(c));
  }
}

void Venue::read_client(Client& c) {
  char buf[65536];
  for (;;) {
    const ssize_t r = ::recv(c.fd, buf, sizeof buf, 0);
    if (r > 0) {
      if (!c.subscriber) c.in.append(buf, static_cast<size_t>(r));
      continue;
    }
    if (r == 0) {
      c.read_closed = true;
      break;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
    break;
  }
  size_t start = 0;
  while (!c.subscriber) {
    const size_t nl = c.in.find('\n', start);
    if (nl == std::string::npos) break;
    std::string line = c.in.substr(start, nl - start);
    start = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    bool sub = false;
    c.out += handle_line(line, sub);
    if (sub) c.subscriber = true;
  }
  if (c.subscriber) c.in.clear();
  else c.in.erase(0, start);
  if (c.in.size() > kMaxLine) {
    c.out += json::compact(respond_err(nullptr, {"bad_request", "request line too long"})) + "\n";
    c.in.clear();
    c.read_closed = true;
  }
}

void Venue::flush_client(Client& c) {
  while (c.out_off < c.out.size()) {
    const ssize_t w = ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, 0);
    if (w > 0) {
      c.out_off += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    c.dead = true;
    return;
  }
  if (c.out_off == c.out.size()) {
    c.out.clear();
    c.out_off = 0;
  } else if (c.out_off > (1u << 20)) {
    c.out.erase(0, c.out_off);
    c.out_off = 0;
  }
  if (c.out.size() - c.out_off > kMaxBacklog) c.dead = true;
}

void Venue::run() {
  int64_t next_tick = mono_ms();
  try {
    ensure(true);
  } catch (const ExDown& e) {
    log("%s; will retry", e.what());
  }
  while (!g_stop) {
    std::vector<pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& c : clients_) {
      short ev = 0;
      if (!c.read_closed) ev |= POLLIN;
      if (c.out_off < c.out.size()) ev |= POLLOUT;
      fds.push_back({c.fd, ev, 0});
    }
    const int timeout = static_cast<int>(std::max<int64_t>(0, next_tick - mono_ms()));
    const int nr = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout);
    if (nr < 0 && errno != EINTR) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
    if (nr > 0) {
      const size_t nclients = clients_.size();
      for (size_t i = 0; i < nclients; ++i) {
        const short re = fds[i + 1].revents;
        Client& c = clients_[i];
        if (re & (POLLIN | POLLHUP)) read_client(c);
        if (re & POLLERR) c.dead = true;
      }
      if (fds[0].revents & POLLIN) accept_clients();
    }
    if (mono_ms() >= next_tick) {
      tick();
      next_tick = mono_ms() + opt_.poll_ms;
    }
    for (auto& c : clients_)
      if (!c.dead && c.out_off < c.out.size()) flush_client(c);
    for (auto& c : clients_)
      if (c.read_closed && c.out_off >= c.out.size()) c.dead = true;
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [](Client& c) {
                                    if (c.dead) ::close(c.fd);
                                    return c.dead;
                                  }),
                   clients_.end());
  }
  if (dirty_) save();
  log("stopped");
}

bk::Fees parse_fees(const std::string& s) {
  bk::Fees f;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    const std::string kv = s.substr(p, q - p);
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) throw std::runtime_error("--fees wants maker=BPS,taker=BPS");
    const std::string k = kv.substr(0, eq);
    const double v = std::stod(kv.substr(eq + 1));
    if (k == "maker") f.maker_bps100 = bk::bps100(v);
    else if (k == "taker") f.taker_bps100 = bk::bps100(v);
    else throw std::runtime_error("--fees: unknown key " + k);
    p = q + 1;
  }
  return f;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
      };
      if (a == "--help" || a == "-h") {
        std::fputs(kHelp, stdout);
        return 0;
      } else if (a == "--listen") o.listen_port = std::stoi(next());
      else if (a == "--exchange") o.exchange = next();
      else if (a == "--state") o.state_path = next();
      else if (a == "--instruments") o.instruments = next();
      else if (a == "--fees") o.fees = parse_fees(next());
      else if (a == "--poll-ms") o.poll_ms = std::max(1, std::stoi(next()));
      else if (a == "--quiet") o.quiet = true;
      else throw std::runtime_error("unknown argument " + a + " (see --help)");
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "venued: %s\n", e.what());
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  try {
    Venue v(o, bk::find_instruments(o.instruments));
    v.load_state();
    v.listen();
    v.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "venued: %s\n", e.what());
    return 1;
  }
  return 0;
}
