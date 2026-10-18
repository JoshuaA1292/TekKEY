// The exchange server. One thread matches (price-time priority is serial per instrument, and
// that is the honest design); one thread serves the market-data feed from a lock-free ring so a
// slow visualiser never touches the matching core's clock. Listens on any mix of TCP, UDP and
// shared memory at once. The latency knob is a busy-wait before every order, so the law can be
// shown on one machine.
#include <atomic>
#include <csignal>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fastbook.hpp"
#include "matcher.hpp"
#include "proto.hpp"
#include "transport.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }

// Single-producer (matching thread) single-consumer (feed thread) ring of feed records.
class FeedRing {
 public:
  static constexpr size_t N = 1 << 18;
  bool push(const proto::Any& a) {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_.load(std::memory_order_acquire) == N) return false;
    slots_[t % N] = a;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  bool pop(proto::Any& a) {
    const uint64_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_.load(std::memory_order_acquire)) return false;
    a = slots_[h % N];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  size_t size() const { return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire); }

 private:
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  std::vector<proto::Any> slots_ = std::vector<proto::Any>(N);
};

struct Session {
  std::unique_ptr<net::Channel> ch;
  bool dead = false;
  bool dirty = false;  // has unflushed output (a maker fill, an expiry)
  // the delay line: replies held until their release time
  struct Held {
    uint64_t release_ns;
    std::vector<uint8_t> bytes;
  };
  std::deque<Held> held;
};

class Server : private ex::Out {
 public:
  Server(const fb::Config& cfg, bool quiet) : m_(cfg, *this), quiet_(quiet) {}
  ~Server() {
    feed_stop_ = true;
    if (feed_thread_.joinable()) feed_thread_.join();
  }

  void add_acceptor(const std::string& url) {
    acceptors_.push_back(net::listen(url));
    if (!quiet_) std::printf("exchange: orders on %s\n", url.c_str());
  }
  void start_feed(const std::string& url) {
    feed_acceptor_ = net::listen(url);
    if (!quiet_) std::printf("exchange: feed on %s\n", url.c_str());
    feed_thread_ = std::thread([this] { feed_loop(); });
  }
  void set_delay_us(uint64_t us) { m_.delay_ns = us * 1000; }

  void run() {
    uint64_t last_stats = net::now_ns();
    while (!g_stop.load(std::memory_order_relaxed)) {
      for (auto& a : acceptors_)
        while (auto ch = a->try_accept()) add_session(std::move(ch));
      for (size_t i = 0; i < sessions_.size(); ++i) {
        Session& s = sessions_[i];
        if (s.dead) continue;
        proto::Any m;
        int n = 0;
        try {
          while (n < 256 && s.ch->try_recv(m)) {
            m_.handle(m, static_cast<uint16_t>(i));
            ++n;
          }
        } catch (const std::exception& e) {
          if (!quiet_) std::printf("exchange: session %zu: %s; closing it\n", i, e.what());
          s.dead = true;
          s.ch.reset();
          continue;
        }
        if (!s.held.empty()) {
          const uint64_t now = net::now_ns();
          while (!s.held.empty() && s.held.front().release_ns <= now) {
            s.ch->send(s.held.front().bytes.data(), s.held.front().bytes.size());
            s.held.pop_front();
            s.dirty = true;
          }
        }
        if (n || s.dirty) {
          s.ch->flush();
          s.dirty = false;
        }
        if (!s.ch->alive()) {
          if (!quiet_) std::printf("exchange: session %zu (%s) closed\n", i, s.ch->kind());
          s.dead = true;
          s.ch.reset();
        }
      }
      if ((m_.snapshot_wanted || snapshot_wanted_.load(std::memory_order_relaxed)) && feed_.size() < FeedRing::N / 2) {
        m_.snapshot_wanted = false;
        snapshot_wanted_.exchange(false);
        proto::GapMsg g;
        g.dropped = static_cast<uint32_t>(feed_drops_.exchange(0));
        feed_.push(proto::Any::of(g));
        for (uint32_t b = 0; b < m_.core().nbooks(); ++b) {
          proto::BookResetMsg r;
          r.book = b;
          feed_.push(proto::Any::of(r));
          m_.core().snapshot(b);
        }
      }
      const uint64_t now = net::now_ns();
      if (now - last_stats > 50'000'000) {
        last_stats = now;
        proto::StatsMsg st;
        st.delay_us = static_cast<uint32_t>(m_.delay_ns / 1000);
        st.orders = m_.core().orders();
        st.trades = m_.core().trades();
        st.mutation = m_.core().config().mutation;
        st.feed_drops = feed_drops_.load();
        st.ns = now;
        feed_.push(proto::Any::of(st));
      }
      if (m_.quit) break;
    }
    feed_stop_ = true;
    if (feed_thread_.joinable()) feed_thread_.join();
  }

 private:
  // ---- ex::Out ----
  void to_session(uint16_t sid, const void* msg, size_t n) override {
    if (sid >= sessions_.size() || sessions_[sid].dead) return;
    Session& s = sessions_[sid];
    s.dirty = true;
    if (m_.delay_ns == 0 && s.held.empty()) {
      s.ch->send(msg, n);
      return;
    }
    const auto* p = static_cast<const uint8_t*>(msg);
    s.held.push_back({net::now_ns() + m_.delay_ns, std::vector<uint8_t>(p, p + n)});
  }
  void to_feed(const proto::Any& a) override {
    if (!feed_.push(a)) {
      feed_drops_.fetch_add(1, std::memory_order_relaxed);
      snapshot_wanted_ = true;
    }
  }

  void add_session(std::unique_ptr<net::Channel> ch) {
    // Indices are never reused: resting orders remember their session by index, and a fill
    // for a dead session must be dropped, not delivered to whoever came next.
    if (sessions_.size() >= 65000) {
      if (!quiet_) std::puts("exchange: session limit reached; refusing");
      return;
    }
    const size_t i = sessions_.size();
    sessions_.emplace_back();
    sessions_[i].ch = std::move(ch);
    sessions_[i].dead = false;
    sessions_[i].ch->send(m_.hello());
    sessions_[i].ch->flush();
    if (!quiet_) std::printf("exchange: session %zu (%s) opened\n", i, sessions_[i].ch->kind());
  }

  void feed_loop() {
    std::vector<std::unique_ptr<net::Channel>> subs;
    proto::Any a;
    while (!feed_stop_.load(std::memory_order_relaxed)) try {
      while (auto ch = feed_acceptor_->try_accept()) {
        ch->send(m_.hello());
        ch->flush();
        subs.push_back(std::move(ch));
        snapshot_wanted_ = true;
        if (!quiet_) std::printf("exchange: feed subscriber %zu\n", subs.size());
      }
      for (auto& s : subs) {
        proto::Any m;
        while (s->try_recv(m))
          if (m.type() == proto::Control && m.as<proto::ControlMsg>().op == proto::Snapshot) snapshot_wanted_ = true;
      }
      size_t n = 0;
      while (n < 4096 && feed_.pop(a)) {
        ++n;
        for (auto& s : subs) s->send(a.bytes, a.size);
      }
      if (n) {
        for (auto& s : subs) s->flush();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      subs.erase(std::remove_if(subs.begin(), subs.end(), [](auto& s) { return !s->alive(); }), subs.end());
    } catch (const std::exception& e) {
      if (!quiet_) std::printf("exchange: feed: %s\n", e.what());
      subs.clear();
    }
  }

  ex::Matcher m_;
  bool quiet_;
  std::vector<std::unique_ptr<net::Acceptor>> acceptors_;
  std::vector<Session> sessions_;
  FeedRing feed_;
  std::atomic<uint64_t> feed_drops_{0};
  std::atomic<bool> snapshot_wanted_{false};
  std::atomic<bool> feed_stop_{false};
  std::unique_ptr<net::Acceptor> feed_acceptor_;
  std::thread feed_thread_;
};

void usage() {
  std::puts(
      "exchange [--tcp PORT] [--udp PORT] [--shm PATH] [--feed PORT] [--books N] [--base P] [--width W]\n"
      "         [--accounts N] [--stp none|expire-maker|cancel-taker] [--delay-us N] [--mutation MASK] [--quiet]\n"
      "  defaults: --tcp 7000 --feed 7002 --books 4096 --base 968 --width 64 --accounts 4 --stp expire-maker");
}
}  // namespace

int main(int argc, char** argv) {
  fb::Config cfg;
  std::vector<std::string> urls;
  std::string feed = "tcp://0.0.0.0:7002";
  bool quiet = false;
  uint64_t delay_us = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--tcp") urls.push_back("tcp://0.0.0.0:" + next());
    else if (a == "--udp") urls.push_back("udp://0.0.0.0:" + next());
    else if (a == "--shm") urls.push_back("shm://" + next());
    else if (a == "--feed") feed = "tcp://0.0.0.0:" + next();
    else if (a == "--books") cfg.books = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--base") cfg.base = std::stoll(next());
    else if (a == "--width") cfg.width = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--accounts") cfg.accounts = static_cast<uint8_t>(std::stoul(next()));
    else if (a == "--delay-us") delay_us = std::stoull(next());
    else if (a == "--mutation") cfg.mutation = std::stoull(next());
    else if (a == "--quiet") quiet = true;
    else if (a == "--stp") {
      std::string s = next();
      cfg.stp = s == "none" ? fb::STP::None : s == "cancel-taker" ? fb::STP::CancelTaker : fb::STP::ExpireMaker;
    } else {
      usage();
      return 2;
    }
  }
  if (urls.empty()) urls.push_back("tcp://0.0.0.0:7000");
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  std::signal(SIGPIPE, SIG_IGN);
  try {
    Server srv(cfg, quiet);
    for (const auto& u : urls) srv.add_acceptor(u);
    srv.start_feed(feed);
    srv.set_delay_us(delay_us);
    if (!quiet)
      std::printf("exchange: %u books, window [%lld, %lld), %u accounts, stp %d, delay %llu us, mutation %llu\n",
                  cfg.books, (long long)cfg.base, (long long)(cfg.base + cfg.width), cfg.accounts, int(cfg.stp),
                  (unsigned long long)delay_us, (unsigned long long)cfg.mutation);
    std::fflush(stdout);
    srv.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "exchange: %s\n", e.what());
    return 1;
  }
  return 0;
}
