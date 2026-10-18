// The visualiser and keyboard client. A separate process that subscribes to the exchange's
// market-data feed and draws what the exchange holds: every pixel, the score, the fruit, the
// frame counter are resting quantities in books, and this program knows nothing else. It never
// reads the program, the router's table or its own key presses back: the only input is the feed
// (plus a layout file that says which book means what). Arrow keys become sell orders in the
// "key" books; +/- and m are control messages to the exchange's latency and mutation knobs.
//
//   viz [--feed tcp://127.0.0.1:7002] [--orders tcp://127.0.0.1:7000] [--layout FILE] [--wall]
//       [--fps 30] [--once]
//
// The feed-state logic (FeedState) is kept free of any terminal code so tests can drive it with
// records from a live exchange; tests/test_viz.cpp includes this file with VIZ_NO_MAIN defined.
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "proto.hpp"
#include "table.hpp"
#include "transport.hpp"

namespace viz {

constexpr uint64_t kSecond = 1'000'000'000ull;

// Events in a sliding window of ~1 s: 20 buckets of 50 ms, so a busy book costs 80 bytes.
class Sliding {
 public:
  static constexpr int NB = 20;
  static constexpr uint64_t kBucket = 50'000'000ull;
  void add(uint64_t now) {
    roll(now);
    ++b_[cur_ % NB];
  }
  uint64_t count(uint64_t now) const {
    if (!init_) return 0;
    const uint64_t idx = std::max(now / kBucket, cur_);
    uint64_t n = 0;
    for (int k = 0; k < NB; ++k) {
      const uint64_t i = cur_ - static_cast<uint64_t>(k);
      if (k > 0 && cur_ < static_cast<uint64_t>(k)) break;
      if (i + NB > idx) n += b_[i % NB];
    }
    return n;
  }

 private:
  void roll(uint64_t now) {
    const uint64_t idx = now / kBucket;
    if (!init_) {
      init_ = true;
      cur_ = idx;
      return;
    }
    if (idx <= cur_) return;
    const uint64_t n = idx - cur_;
    if (n >= NB) {
      b_.fill(0);
    } else {
      for (uint64_t k = 1; k <= n; ++k) b_[(cur_ + k) % NB] = 0;
    }
    cur_ = idx;
  }
  std::array<uint32_t, NB> b_{};
  uint64_t cur_ = 0;
  bool init_ = false;
};

struct LevelInfo {
  int64_t total = 0;
  uint32_t count = 0;
  bool operator==(const LevelInfo&) const = default;
};
using LevelKey = std::tuple<uint32_t, uint8_t, int64_t>;  // book, side, price

const char* mutation_name(uint64_t mask) {
  switch (mask) {
    case 0: return "none";
    case 1: return "fifo-after-partial";
    case 2: return "price-inverted";
    case 4: return "stop-wrong-side";
    case 8: return "ioc-rests";
  }
  return "mixed";
}
std::string mutation_text(uint64_t mask) {
  if (mask == 0 || mask == 1 || mask == 2 || mask == 4 || mask == 8) return mutation_name(mask);
  std::string s;
  for (uint64_t bit = 1; bit <= 8; bit <<= 1)
    if (mask & bit) s += (s.empty() ? "" : "+") + std::string(mutation_name(bit));
  if (mask & ~15ull) s += (s.empty() ? "" : "+") + std::string("?");
  return s;
}

// Everything the visualiser knows, rebuilt from feed records alone.
class FeedState {
 public:
  explicit FeedState(int64_t p = 1000) : P_(p) {}

  void apply(const proto::Any& a) { apply(a, net::now_ns()); }
  void apply(const proto::Any& a, uint64_t now) {
    switch (a.type()) {
      case proto::Hello: {
        hello_ = a.as<proto::HelloMsg>();
        clear_levels();  // a fresh subscription: a full snapshot follows
        break;
      }
      case proto::Gap: {
        const auto& g = a.as<proto::GapMsg>();
        ++gaps_;
        gap_dropped_ += g.dropped;
        snapshot_open_ = true;
        break;
      }
      case proto::BookReset: {
        reset_book(a.as<proto::BookResetMsg>().book);
        break;
      }
      case proto::Level: {
        const auto& l = a.as<proto::LevelMsg>();
        set_level(l.book, l.side, l.price, l.total, l.count);
        break;
      }
      case proto::Trade: {
        const auto& t = a.as<proto::TradeMsg>();
        ++trades_total_;
        trades_window_.add(now);
        auto& pb = per_book_[t.book];
        ++pb.trades;
        pb.last_ns = now;
        pb.window.add(now);
        break;
      }
      case proto::Stats: {
        const auto& s = a.as<proto::StatsMsg>();
        stats_ = s;
        ++stats_seen_;
        if (snapshot_open_) {
          snapshot_open_ = false;
          ++snapshots_completed_;
        }
        if (!hist_.empty() && (s.orders < hist_.back().orders || s.ns < hist_.back().ns)) hist_.clear();
        hist_.push_back(s);
        while (hist_.size() > 2 && s.ns - hist_.front().ns > kSecond) hist_.pop_front();
        break;
      }
      default:
        break;  // order-session records never travel on the feed; ignore anything else
    }
  }

  // ---- queries ----
  int64_t P() const { return P_; }
  int64_t qty(uint32_t book, uint8_t side, int64_t price) const {
    auto it = levels_.find(LevelKey{book, side, price});
    return it == levels_.end() ? 0 : it->second.total;
  }
  int64_t sell_qty(uint32_t book) const { return qty(book, proto::Sell, P_); }
  int64_t sell_qty(uint32_t book, int64_t price) const { return qty(book, proto::Sell, price); }
  bool lit(long long book) const { return book >= 0 && sell_qty(static_cast<uint32_t>(book)) >= 1; }
  // bit i is 1 when books[i] is lit; nullopt when there is no such register in the layout
  std::optional<long long> decode_bits(const std::vector<long long>& books) const {
    if (books.empty()) return std::nullopt;
    long long v = 0;
    for (size_t i = 0; i < books.size() && i < 62; ++i)
      if (lit(books[i])) v |= 1ll << i;
    return v;
  }

  uint64_t trades_total() const { return trades_total_; }
  uint64_t trades_last_second(uint64_t now) const { return trades_window_.count(now); }
  uint64_t trades_in(long long book) const {
    auto it = book < 0 ? per_book_.end() : per_book_.find(static_cast<uint32_t>(book));
    return it == per_book_.end() ? 0 : it->second.trades;
  }
  // trades in `book` over the last second: the frame rate when `book` is the frame counter
  double fps(long long book, uint64_t now) const {
    auto it = book < 0 ? per_book_.end() : per_book_.find(static_cast<uint32_t>(book));
    return it == per_book_.end() ? 0.0 : static_cast<double>(it->second.window.count(now));
  }
  std::optional<uint64_t> last_trade_ns(long long book) const {
    auto it = book < 0 ? per_book_.end() : per_book_.find(static_cast<uint32_t>(book));
    if (it == per_book_.end() || it->second.trades == 0) return std::nullopt;
    return it->second.last_ns;
  }
  bool traded_within(long long book, uint64_t now, uint64_t window_ns) const {
    auto t = last_trade_ns(book);
    return t && now >= *t && now - *t <= window_ns;
  }

  const std::optional<proto::StatsMsg>& stats() const { return stats_; }
  uint64_t stats_seen() const { return stats_seen_; }
  double orders_per_sec() const { return rate([](const proto::StatsMsg& s) { return s.orders; }); }
  double stats_trades_per_sec() const { return rate([](const proto::StatsMsg& s) { return s.trades; }); }

  size_t resting_orders() const { return resting_orders_; }
  size_t resting_books() const { return book_orders_.size(); }
  const std::map<LevelKey, LevelInfo>& levels() const { return levels_; }
  const std::optional<proto::HelloMsg>& hello() const { return hello_; }
  uint32_t books() const { return hello_ ? hello_->books : 0; }
  uint64_t gaps() const { return gaps_; }
  uint64_t gap_dropped() const { return gap_dropped_; }
  uint64_t snapshots_completed() const { return snapshots_completed_; }
  bool synced() const { return hello_ && snapshots_completed_ > 0; }

  void clear_levels() {
    levels_.clear();
    book_orders_.clear();
    resting_orders_ = 0;
  }

 private:
  struct PerBook {
    uint64_t trades = 0;
    uint64_t last_ns = 0;
    Sliding window;
  };
  template <class F>
  double rate(F field) const {
    if (hist_.size() < 2) return 0.0;
    const auto& a = hist_.front();
    const auto& b = hist_.back();
    if (b.ns <= a.ns || field(b) < field(a)) return 0.0;
    return static_cast<double>(field(b) - field(a)) * 1e9 / static_cast<double>(b.ns - a.ns);
  }
  void set_level(uint32_t book, uint8_t side, int64_t price, int64_t total, uint32_t count) {
    const LevelKey k{book, side, price};
    auto it = levels_.find(k);
    const uint32_t old = it == levels_.end() ? 0 : it->second.count;
    if (count == 0) {
      if (it != levels_.end()) levels_.erase(it);
    } else if (it == levels_.end()) {
      levels_.emplace(k, LevelInfo{total, count});
    } else {
      it->second = LevelInfo{total, count};
    }
    resting_orders_ = resting_orders_ + count - old;
    auto& bo = book_orders_[book];
    bo = bo + count - old;
    if (bo == 0) book_orders_.erase(book);
  }
  void reset_book(uint32_t book) {
    auto lo = levels_.lower_bound(LevelKey{book, 0, INT64_MIN});
    auto hi = levels_.lower_bound(LevelKey{book + 1, 0, INT64_MIN});
    size_t n = 0;
    for (auto it = lo; it != hi; ++it) n += it->second.count;
    levels_.erase(lo, hi);
    resting_orders_ -= n;
    book_orders_.erase(book);
  }

  int64_t P_;
  std::map<LevelKey, LevelInfo> levels_;
  std::map<uint32_t, uint64_t> book_orders_;
  size_t resting_orders_ = 0;
  std::map<uint32_t, PerBook> per_book_;
  Sliding trades_window_;
  uint64_t trades_total_ = 0;
  std::optional<proto::StatsMsg> stats_;
  std::deque<proto::StatsMsg> hist_;
  uint64_t stats_seen_ = 0;
  std::optional<proto::HelloMsg> hello_;
  uint64_t gaps_ = 0, gap_dropped_ = 0, snapshots_completed_ = 0;
  bool snapshot_open_ = false;
};

// ---- layout -------------------------------------------------------------------------------
struct Layout {
  int64_t P = 1000;
  int width = 16, height = 16;
  std::vector<long long> pixels, keys, fruit_x, fruit_y, head_x, head_y, tail_x, tail_y, cells;
  long long frame = -1, score = -1, death = -1, generation = -1;
  uint8_t key_account = 2;
  bool loaded = false;

  static Layout from(const tbl::Layout& l) {
    Layout o;
    o.loaded = true;
    o.P = l.get("P", 1000);
    o.width = static_cast<int>(l.get("width", 16));
    o.height = static_cast<int>(l.get("height", 16));
    if (o.width < 1) o.width = 1;
    if (o.height < 1) o.height = 1;
    o.pixels = l.vec("pixels");
    o.keys = l.vec("keys");
    o.fruit_x = l.vec("fruit_x");
    o.fruit_y = l.vec("fruit_y");
    o.head_x = l.vec("head_x");
    o.head_y = l.vec("head_y");
    o.tail_x = l.vec("tail_x");
    o.tail_y = l.vec("tail_y");
    o.cells = l.vec("cells");
    o.frame = l.get("frame", -1);
    o.score = l.get("score", -1);
    o.death = l.get("death", -1);
    o.generation = l.get("generation", -1);
    o.key_account = static_cast<uint8_t>(l.get("key_account", 2));
    return o;
  }
  bool snake() const { return !pixels.empty(); }
  long long pixel(int x, int y) const {
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    return i < pixels.size() ? pixels[i] : -1;
  }
};

// ---- connections --------------------------------------------------------------------------
struct Link {
  std::string url;
  std::unique_ptr<net::Channel> ch;
  uint64_t next_try = 0;
  uint64_t connects = 0;
  bool ok() const { return ch && ch->alive(); }
  // Reconnect at most once a second; returns true when a new channel was just opened.
  bool maintain(uint64_t now) {
    if (ok()) return false;
    ch.reset();
    if (now < next_try) return false;
    next_try = now + kSecond;
    try {
      ch = net::connect(url);
      ++connects;
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
};

// ---- terminal -----------------------------------------------------------------------------
struct Terminal {
  termios orig{};
  bool raw = false;
  int orig_flags = 0;
  void enter() {
    if (raw || !isatty(0)) return;
    if (tcgetattr(0, &orig) != 0) return;
    termios t = orig;
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | IEXTEN);  // keep ISIG: ^C -> SIGINT -> clean exit
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    orig_flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, orig_flags | O_NONBLOCK);
    const char* s = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
    (void)!write(1, s, std::strlen(s));
    raw = true;
  }
  void leave() {
    if (!raw) return;
    const char* s = "\x1b[0m\x1b[?25h\x1b[?1049l";
    (void)!write(1, s, std::strlen(s));
    fcntl(0, F_SETFL, orig_flags);
    tcsetattr(0, TCSANOW, &orig);
    raw = false;
  }
  static int columns() {
    winsize w{};
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    return 80;
  }
};

enum class Key { None, Up, Down, Left, Right, Char };
struct KeyEvent {
  Key key = Key::None;
  char ch = 0;
};

// Parses bytes from a non-blocking stdin into key events: arrows are ESC [ A..D (or ESC O A..D).
class KeyParser {
 public:
  void feed(const char* p, size_t n) { buf_.append(p, n); }
  bool next(KeyEvent& ev) {
    while (!buf_.empty()) {
      const unsigned char c = static_cast<unsigned char>(buf_[0]);
      if (c != 0x1b) {
        buf_.erase(0, 1);
        ev = KeyEvent{Key::Char, static_cast<char>(c)};
        return true;
      }
      if (buf_.size() < 2) return false;  // ESC alone so far: wait for the rest
      if (buf_[1] != '[' && buf_[1] != 'O') {
        buf_.erase(0, 1);  // a lone escape: drop it
        continue;
      }
      if (buf_.size() < 3) return false;
      const char d = buf_[2];
      buf_.erase(0, 3);
      switch (d) {
        case 'A': ev = KeyEvent{Key::Up, 0}; return true;
        case 'B': ev = KeyEvent{Key::Down, 0}; return true;
        case 'C': ev = KeyEvent{Key::Right, 0}; return true;
        case 'D': ev = KeyEvent{Key::Left, 0}; return true;
        default: continue;
      }
    }
    return false;
  }
  void drop_stale_escape() {
    if (buf_ == "\x1b") buf_.clear();
  }

 private:
  std::string buf_;
};

// ---- the application ----------------------------------------------------------------------
constexpr std::array<int64_t, 10> kDelaySteps = {0, 10, 20, 50, 100, 200, 300, 500, 1000, 2000};
constexpr std::array<uint64_t, 5> kMutationCycle = {0, 1, 2, 4, 8};

struct Options {
  std::string feed = "tcp://127.0.0.1:7002";
  std::string orders = "tcp://127.0.0.1:7000";
  std::string layout;
  bool wall = false;
  bool once = false;
  double fps = 30;
};

struct App {
  Options opt;
  Layout lay;
  FeedState st;
  Link feed, orders;
  uint32_t tag = 0;
  uint64_t keys_sent = 0;
  std::string last_action;
  std::string note;  // transient status line (e.g. "no key books in layout")

  explicit App(const Options& o) : opt(o), st(1000) {
    if (!o.layout.empty()) {
      lay = Layout::from(tbl::load_layout(o.layout));
      st = FeedState(lay.P);
    }
    feed.url = o.feed;
    orders.url = o.orders;
  }
  bool snake_mode() const { return !opt.wall && lay.snake(); }

  // Drain both connections; never blocks. Returns the number of feed records applied.
  size_t pump(uint64_t now, size_t max_records = 1 << 16) {
    feed.maintain(now);
    orders.maintain(now);
    size_t n = 0;
    proto::Any a;
    if (feed.ch) {
      while (n < max_records && feed.ch->try_recv(a)) {
        st.apply(a, now);
        ++n;
      }
    }
    if (orders.ch) {
      int d = 0;
      while (d < 4096 && orders.ch->try_recv(a)) ++d;  // Hello / Ack / ControlReply: discarded
    }
    return n;
  }

  void send_order(int dir) {
    static const char* names[4] = {"up", "down", "left", "right"};
    if (lay.keys.size() < 4 || lay.keys[static_cast<size_t>(dir)] < 0) {
      note = "no key books in layout";
      return;
    }
    if (!orders.ok()) {
      note = "orders disconnected: key dropped";
      return;
    }
    proto::NewOrderMsg m;
    m.account = lay.key_account;
    m.book = static_cast<uint32_t>(lay.keys[static_cast<size_t>(dir)]);
    m.side = proto::Sell;
    m.otype = proto::Limit;
    m.price = lay.P;
    m.qty = 1;
    m.tag = ++tag;
    orders.ch->send(m);
    orders.ch->flush();
    ++keys_sent;
    last_action = std::string(names[dir]) + " -> sell 1 @" + std::to_string(lay.P) + " in book " + std::to_string(m.book);
    note.clear();
  }
  void send_control(uint8_t op, int64_t arg, const std::string& what) {
    if (!orders.ok()) {
      note = "orders disconnected: " + what + " dropped";
      return;
    }
    proto::ControlMsg c;
    c.op = op;
    c.arg = arg;
    c.tag = ++tag;
    orders.ch->send(c);
    orders.ch->flush();
    last_action = what;
    note.clear();
  }
  void step_delay(int dir) {
    const int64_t cur = st.stats() ? st.stats()->delay_us : 0;
    int64_t next = cur;
    if (dir > 0) {
      next = kDelaySteps.back();
      for (int64_t d : kDelaySteps)
        if (d > cur) {
          next = d;
          break;
        }
    } else {
      next = 0;
      for (auto it = kDelaySteps.rbegin(); it != kDelaySteps.rend(); ++it)
        if (*it < cur) {
          next = *it;
          break;
        }
    }
    send_control(proto::SetDelayUs, next, "latency -> " + std::to_string(next) + " us");
  }
  void cycle_mutation() {
    const uint64_t cur = st.stats() ? st.stats()->mutation : 0;
    uint64_t next = 0;
    for (size_t i = 0; i < kMutationCycle.size(); ++i)
      if (kMutationCycle[i] == cur) next = kMutationCycle[(i + 1) % kMutationCycle.size()];
    send_control(proto::SetMutation, static_cast<int64_t>(next), "mutation -> " + mutation_text(next));
  }
  void request_snapshot() {
    if (!feed.ok()) {
      note = "feed disconnected: snapshot dropped";
      return;
    }
    proto::ControlMsg c;
    c.op = proto::Snapshot;
    c.tag = ++tag;
    feed.ch->send(c);
    feed.ch->flush();
    last_action = "snapshot requested";
    note.clear();
  }
  // Returns false when the user asked to quit.
  bool on_key(const KeyEvent& e) {
    switch (e.key) {
      case Key::Up: send_order(0); return true;
      case Key::Down: send_order(1); return true;
      case Key::Left: send_order(2); return true;
      case Key::Right: send_order(3); return true;
      case Key::Char: break;
      case Key::None: return true;
    }
    switch (e.ch) {
      case 'w': case 'W': send_order(0); break;
      case 's': send_order(1); break;  // lower-case s moves (snake); S requests a snapshot too
      case 'a': case 'A': send_order(2); break;
      case 'd': case 'D': send_order(3); break;
      case '+': case '=': step_delay(+1); break;
      case '-': case '_': step_delay(-1); break;
      case 'm': case 'M': cycle_mutation(); break;
      case 'S': case 'r': case 'R': request_snapshot(); break;
      case 'q': case 'Q': case 3: return false;
      default: break;
    }
    return true;
  }
};

// ---- rendering ----------------------------------------------------------------------------
namespace ansi {
constexpr const char* reset = "\x1b[0m";
constexpr const char* dim = "\x1b[2m";
constexpr const char* bold = "\x1b[1m";
constexpr const char* lit = "\x1b[92m";
constexpr const char* fruit = "\x1b[95m";
constexpr const char* red = "\x1b[1;91m";
constexpr const char* yellow = "\x1b[93m";
}  // namespace ansi

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}
std::string coord(const std::optional<long long>& x, const std::optional<long long>& y) {
  if (!x && !y) return "-";
  return "(" + (x ? std::to_string(*x) : "?") + "," + (y ? std::to_string(*y) : "?") + ")";
}

std::vector<std::string> header(const App& app, uint64_t now) {
  const FeedState& st = app.st;
  std::vector<std::string> out;
  const auto& s = st.stats();
  std::string l1 = std::string(ansi::bold) + "MINSKY " + (app.snake_mode() ? "snake" : "wall") + ansi::reset;
  if (app.snake_mode()) {
    const long long score = app.lay.score >= 0 ? st.sell_qty(static_cast<uint32_t>(app.lay.score)) : -1;
    l1 += fmt("   fps %5.1f", st.fps(app.lay.frame, now));
    l1 += score >= 0 ? fmt("   score %lld", score) : "   score -";
  } else {
    l1 += fmt("   generations/s %5.1f", st.fps(app.lay.generation, now));
  }
  l1 += fmt("   trades %llu (%llu/s)", (unsigned long long)st.trades_total(), (unsigned long long)st.trades_last_second(now));
  l1 += fmt("   orders %.0f/s", st.orders_per_sec());
  out.push_back(l1);

  std::string l2;
  if (s) {
    l2 += fmt("latency %u us   mutation %s   feed drops %llu", s->delay_us, mutation_text(s->mutation).c_str(),
              (unsigned long long)s->feed_drops);
    l2 += fmt("   exchange orders %llu trades %llu", (unsigned long long)s->orders, (unsigned long long)s->trades);
  } else {
    l2 += "latency -   mutation -   feed drops -   (no Stats yet)";
  }
  out.push_back(l2);

  std::string l3 = fmt("exchange: %zu orders resting in %zu books", st.resting_orders(), st.resting_books());
  if (st.hello()) l3 += fmt("   (%u books, window [%lld,%lld))", st.hello()->books, (long long)st.hello()->base_price,
                            (long long)(st.hello()->base_price + st.hello()->width));
  out.push_back(l3);

  std::string l4 = "feed " + app.feed.url + " ";
  l4 += app.feed.ok() ? std::string(ansi::lit) + "ok" + ansi::reset : std::string(ansi::red) + "feed disconnected" + ansi::reset;
  if (st.gaps()) l4 += fmt(" (gaps %llu, dropped %llu)", (unsigned long long)st.gaps(), (unsigned long long)st.gap_dropped());
  l4 += "   orders " + app.orders.url + " ";
  l4 += app.orders.ok() ? std::string(ansi::lit) + "ok" + ansi::reset : std::string(ansi::red) + "orders disconnected" + ansi::reset;
  out.push_back(l4);
  return out;
}

std::vector<std::string> render_snake(const App& app, uint64_t now) {
  const FeedState& st = app.st;
  const Layout& L = app.lay;
  std::vector<std::string> out = header(app, now);
  const auto fx = st.decode_bits(L.fruit_x), fy = st.decode_bits(L.fruit_y);
  const auto hx = st.decode_bits(L.head_x), hy = st.decode_bits(L.head_y);
  const auto tx = st.decode_bits(L.tail_x), ty = st.decode_bits(L.tail_y);
  out.push_back("head " + coord(hx, hy) + "   tail " + coord(tx, ty) + "   fruit " + coord(fx, fy) +
                fmt("   P=%lld   frame book %lld   death book %lld", (long long)L.P, L.frame, L.death));
  const bool dead = st.traded_within(L.death, now, kSecond / 2);
  const std::string border = dead ? ansi::red : ansi::dim;
  std::string top = border + "┌";
  for (int x = 0; x < L.width; ++x) top += "──";
  top += "┐";
  top += ansi::reset;
  if (dead) top += std::string("  ") + ansi::red + "GAME OVER" + ansi::reset;
  out.push_back(top);
  for (int y = 0; y < L.height; ++y) {
    std::string row = border + "│" + ansi::reset;
    for (int x = 0; x < L.width; ++x) {
      const long long b = L.pixel(x, y);
      const bool on = b >= 0 && st.lit(b);
      const bool is_fruit = fx && fy && *fx == x && *fy == y;
      if (on) row += std::string(ansi::lit) + "██" + ansi::reset;
      else if (is_fruit) row += std::string(ansi::fruit) + "◆ " + ansi::reset;
      else row += std::string(ansi::dim) + "· " + ansi::reset;
    }
    row += border + "│" + ansi::reset;
    out.push_back(row);
  }
  std::string bot = border + "└";
  for (int x = 0; x < L.width; ++x) bot += "──";
  bot += "┘";
  bot += ansi::reset;
  out.push_back(bot);
  std::string keys = std::string(ansi::dim) + "keys: arrows/wasd move   +/- latency   m mutation   S snapshot   q quit" + ansi::reset;
  out.push_back(keys);
  std::string act = fmt("sent %llu key orders", (unsigned long long)app.keys_sent);
  if (!app.last_action.empty()) act += "   last: " + app.last_action;
  if (!app.note.empty()) act += std::string("   ") + ansi::yellow + app.note + ansi::reset;
  out.push_back(act);
  return out;
}

std::vector<std::string> render_wall(const App& app, uint64_t now, int cols) {
  const FeedState& st = app.st;
  std::vector<std::string> out = header(app, now);
  std::vector<long long> cells = app.lay.cells;
  if (cells.empty()) {
    const uint32_t n = std::min<uint32_t>(st.books() ? st.books() : 512, 512);
    for (uint32_t b = 0; b < n; ++b) cells.push_back(b);
  }
  out.push_back(fmt("%zu cells, P=%lld, generation book %lld", cells.size(), (long long)app.lay.P, app.lay.generation));
  const int w = std::max(8, cols);
  std::string row;
  int n = 0;
  for (long long b : cells) {
    row += st.lit(b) ? std::string(ansi::lit) + "█" + ansi::reset : std::string(ansi::dim) + "·" + ansi::reset;
    if (++n == w) {
      out.push_back(row);
      row.clear();
      n = 0;
    }
  }
  if (n) out.push_back(row);
  out.push_back(std::string(ansi::dim) + "keys: +/- latency   m mutation   S snapshot   q quit" + ansi::reset);
  std::string act;
  if (!app.last_action.empty()) act += "last: " + app.last_action;
  if (!app.note.empty()) act += std::string("   ") + ansi::yellow + app.note + ansi::reset;
  if (!act.empty()) out.push_back(act);
  return out;
}

std::vector<std::string> render(const App& app, uint64_t now, int cols) {
  return app.snake_mode() ? render_snake(app, now) : render_wall(app, now, cols);
}

void draw(const std::vector<std::string>& lines, bool raw) {
  std::string s;
  if (raw) s += "\x1b[H";
  for (const auto& l : lines) {
    s += l;
    s += raw ? "\x1b[K\r\n" : "\n";
  }
  if (raw) s += "\x1b[J";
  size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::write(1, s.data() + off, s.size() - off);
    if (w <= 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    off += static_cast<size_t>(w);
  }
}

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
  std::fputs(
      "viz [--feed tcp://127.0.0.1:7002] [--orders tcp://127.0.0.1:7000] [--layout FILE] [--wall] [--fps 30] [--once]\n"
      "  --once   connect, wait for the snapshot, print one frame to stdout and exit (no raw mode)\n",
      stderr);
}

int run(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--feed") o.feed = next();
    else if (a == "--orders") o.orders = next();
    else if (a == "--layout") o.layout = next();
    else if (a == "--wall") o.wall = true;
    else if (a == "--once") o.once = true;
    else if (a == "--fps") o.fps = std::stod(next());
    else {
      usage();
      return 2;
    }
  }
  if (o.fps <= 0) o.fps = 30;
  std::unique_ptr<App> app;
  try {
    app = std::make_unique<App>(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "viz: %s\n", e.what());
    return 1;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  if (o.once) {
    const uint64_t t0 = net::now_ns();
    app->feed.next_try = 0;
    app->orders.next_try = 0;
    for (;;) {
      const uint64_t now = net::now_ns();
      app->pump(now);
      if (app->st.synced() && app->st.stats_seen() > 0) break;
      if (now - t0 > 2 * kSecond || g_stop) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    draw(render(*app, net::now_ns(), Terminal::columns()), false);
    return app->feed.ok() ? 0 : 1;
  }

  Terminal term;
  term.enter();
  KeyParser keys;
  const uint64_t frame_ns = static_cast<uint64_t>(1e9 / o.fps);
  uint64_t next_frame = 0;
  bool running = true;
  while (running && !g_stop) {
    const uint64_t now = net::now_ns();
    const size_t got = app->pump(now);
    if (term.raw) {
      char buf[256];
      ssize_t r = ::read(0, buf, sizeof buf);
      if (r > 0) keys.feed(buf, static_cast<size_t>(r));
      else keys.drop_stale_escape();
      KeyEvent e;
      while (running && keys.next(e)) running = app->on_key(e);
    }
    if (now >= next_frame) {
      next_frame = now + frame_ns;
      draw(render(*app, now, Terminal::columns()), term.raw);
    }
    if (!got) std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  term.leave();
  return 0;
}

}  // namespace viz

#ifndef VIZ_NO_MAIN
int main(int argc, char** argv) { return viz::run(argc, argv); }
#endif
