// The search (BOUNDED_KEYS.md §8, BUILD_A_VENUE.md §3 A3).
//
// State: an fb::Core snapshot (the program in account 1, the environment in accounts 0 and 2),
// the program's ledger, the order id of every node (plus two flatten slots per instrument), and
// whether the key holder has paused / cancelled.
//
// Moves, in this fixed order: gap down, cross down, cross up, gap up (each per instrument), pause,
// cancel. Prices only matter at boundaries: every trigger and limit of a live order plus, while a
// position is open, the price at which the loss would pass max_loss_cents (recomputed in every
// state). The landing points are each boundary and its two neighbours (the last price before
// crossing it, the first after). A cross moves to the next landing point; prices in between are
// a walk that changes nothing. A gap jumps up to max_gap_ticks with no prints in between, to every
// landing point that crosses at least one boundary and to the farthest print, nearest first.
// Landing points are complete: between two of them the engine's outcome is fixed and loss and fees
// are monotone in the print, so their extremes are at landing points; and the lowest origin a down
// gap can start from without crossing its first boundary is that boundary's upper neighbour.
//
// Search: depth first over unique states. A state's metrics (loss at the last print, fees, trades,
// exposure, live stop) are functions of the state, so the maxima over paths are maxima over
// reachable states. The memo key is the canonical book hashes (order ids excluded) plus the
// program's own account, ledger, node states and key-holder flags. Core::hash() itself is not the
// key: it hashes the environment's accounts too, whose cash records the path's history and would
// keep a price oscillation from ever closing a cycle. Fees are exact in the key, never bucketed.
//
// Output: the first violation stops the search; the reported counterexample is then the shortest
// violating path (iterative deepening), and a certificate's worst_path is the shortest path that
// attains max_loss, so both read as a story rather than a search trace.
#include "verifier.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "env.hpp"

#ifndef ENGINE_SHA
#define ENGINE_SHA "dev"
#endif

namespace bk {

std::string engine_id() { return std::string("fastbook v2 @ ") + ENGINE_SHA; }

namespace {

constexpr uint8_t kProg = 1;
// Above this many states the presentation pass (the shortest path, by iterative deepening) would
// cost more than the search itself; the depth-first path is reported instead. A state count,
// not a clock, so the output stays deterministic.
constexpr int64_t kShortestStates = 200000;
// The price domain the environment can print in, [1, kTopPrice]: half the engine's limit, so a
// book's shifted prices inside the snapshot core stay within fb::kMaxPrice too.
constexpr int64_t kTopPrice = fb::kMaxPrice / 2;
// Before the exhaustive search, a shallow breadth-first probe for violations: most broken programs
// break within a few moves, and depth-first order can wander a long way before it gets there.
constexpr int kProbeDepth = 6;
constexpr size_t kProbeStates = 40000;
const EnvAccounts kEnv{0, 2};

struct Timeout {};

struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void add(int64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<uint8_t>(v >> (8 * i));
      h *= 1099511628211ull;
    }
  }
};

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// What the program's orders did during one move, in the order the engine reported it.
struct Ev {
  enum T : uint8_t { Fill, Cancel } t;
  fb::OrderId id;
  uint8_t side;
  int64_t px, q;
  bool maker;
};

struct VSink : fb::Sink {
  std::vector<Ev> evs;
  void fill(const fb::Order& o, bool maker, fb::Price px, fb::Qty q) override {
    if (o.account == kProg) evs.push_back({Ev::Fill, o.id, o.side, px, q, maker});
  }
  void cancelled(const fb::Order& o) override {
    if (o.account == kProg) evs.push_back({Ev::Cancel, o.id, o.side, 0, 0, false});
  }
  void group_cancelled(const fb::Order& o, uint8_t) override {
    if (o.account == kProg) evs.push_back({Ev::Cancel, o.id, o.side, 0, 0, false});
  }
  void trade(uint32_t, fb::Price, fb::Qty, uint8_t, uint8_t) override {}
  void level(uint32_t, uint8_t, fb::Price, fb::Qty, uint32_t) override {}
  void reset(uint32_t) override {}
};

struct St {
  fb::Core core;
  Ledger led;
  std::vector<fb::OrderId> ids;  // nodes, then (flatten stop, flatten limit) per instrument
  uint8_t paused = 0, cancelled = 0;
  St(const fb::Config& c, fb::Sink* s, size_t nids, size_t ninst) : core(c, s), led(ninst), ids(nids, 0) {}
  St(const St& o, fb::Sink* s) : core(o.core, s), led(o.led), ids(o.ids), paused(o.paused), cancelled(o.cancelled) {}
};

struct Move {
  enum T : uint8_t { GapDown, CrossDown, CrossUp, GapUp, Pause, Cancel } t;
  int inst;
  int64_t from, to;
};

struct Metrics {
  int64_t loss = 0, fees = 0, trades = 0, exposure = 0;
  bool live_stop = true;
};

struct Violation {
  std::string what;
  int64_t limit = 0, value = 0;
};

class Verifier {
 public:
  Verifier(const Program& p, const Envelope& e, const Instruments& ins, const VerifyOptions& o)
      : p_(p), e_(e), opt_(o), n_(p.nodes.size()), k_(p.instruments.size()) {
    for (const auto& name : p.instruments) ticks_.push_back(ins.at(name).tick_cents);
    marks_ = o.marks;
    if (marks_.empty()) throw Invalid("verify needs --mark");
    if (marks_.size() == 1) marks_.assign(k_, marks_[0]);
    if (marks_.size() != k_) throw Invalid("one --mark per instrument");
    mark_mode_ = e.trigger_ref == "mark";
    for (size_t i = 0; i < k_; ++i) books_.push_back(static_cast<uint32_t>(i));
    // Inside the snapshot core every book is shifted so its mark sits at marks_[0]: the engine only
    // compares prices within a book, so this changes nothing it decides, and it keeps every
    // instrument inside the core's dense price window (out-of-window levels are std::maps, whose
    // copies dominate the search's cost). Prices leave the core through cpx() / tpx() only.
    for (size_t i = 0; i < k_; ++i) off_.push_back(marks_[i] - marks_[0]);
    t0_ = std::chrono::steady_clock::now();
  }

  // ---------------------------------------------------------------- state inspection
  uint8_t state_of(const St& s, size_t i) const {
    if (!s.ids[i]) return proto::OrderGone;
    const fb::Order* o = s.core.find(s.ids[i]);
    if (!o) return proto::OrderGone;
    switch (o->state) {
      case fb::State::Resting: return proto::OrderResting;
      case fb::State::Pending: return proto::OrderPending;
      case fb::State::Queued: return proto::OrderQueued;
      case fb::State::Held: return proto::OrderHeld;
      default: return proto::OrderGone;
    }
  }
  int inst_of(size_t i) const { return i < n_ ? p_.nodes[i].instrument_index : static_cast<int>((i - n_) / 2); }
  bool exit_slot(size_t i) const { return i >= n_ || p_.nodes[i].is_stop(); }
  std::string name_of(size_t i) const { return i < n_ ? p_.nodes[i].id : "flatten"; }
  int slot_of(const St& s, fb::OrderId id) const {
    for (size_t i = 0; i < s.ids.size(); ++i)
      if (s.ids[i] == id) return static_cast<int>(i);
    return -1;
  }
  std::vector<std::optional<int64_t>> marks(const St& s) const {
    std::vector<std::optional<int64_t>> m(k_);
    for (size_t i = 0; i < k_; ++i) m[i] = last_of(s, static_cast<int>(i));
    return m;
  }
  bool any_live(const St& s) const {
    for (size_t i = 0; i < s.ids.size(); ++i)
      if (state_of(s, i) != proto::OrderGone) return true;
    return false;
  }
  bool flat(const St& s) const {
    for (int64_t x : s.led.pos)
      if (x) return false;
    return true;
  }
  bool halted(const St& s) const { return flat(s) && !any_live(s); }

  Metrics eval(const St& s) const {
    Metrics m;
    m.loss = s.led.loss(marks(s), ticks_);
    m.fees = s.led.fees_cents;
    m.trades = s.led.trades;
    m.exposure = s.led.exposure();
    for (size_t inst = 0; inst < k_; ++inst) {
      if (!s.led.pos[inst]) continue;
      bool ok = false;
      for (size_t i = 0; i < s.ids.size() && !ok; ++i)
        ok = inst_of(i) == static_cast<int>(inst) && exit_slot(i) && state_of(s, i) != proto::OrderGone;
      if (!ok) m.live_stop = false;
    }
    return m;
  }
  std::optional<Violation> violation(const Metrics& m) const {
    if (m.loss > e_.max_loss_cents) return Violation{"max_loss_cents", e_.max_loss_cents, m.loss};
    if (m.fees > e_.max_fees_cents) return Violation{"max_fees_cents", e_.max_fees_cents, m.fees};
    if (m.trades > e_.max_trades) return Violation{"max_trades", e_.max_trades, m.trades};
    if (m.exposure > e_.max_exposure_lots) return Violation{"max_exposure_lots", e_.max_exposure_lots, m.exposure};
    if (e_.require_live_stop && !m.live_stop) return Violation{"require_live_stop", 1, 0};
    return std::nullopt;
  }

  uint64_t key(const St& s) const {
    Fnv f;
    for (uint32_t b = 0; b < k_; ++b) f.add(static_cast<int64_t>(s.core.book_hash(b)));
    f.add(s.led.cash_cents);
    f.add(s.led.fees_cents);
    f.add(s.led.trades);
    for (int64_t x : s.led.pos) f.add(x);
    for (size_t i = 0; i < s.ids.size(); ++i) {
      const uint8_t st = state_of(s, i);
      f.add(st);
      if (st != proto::OrderGone) f.add(s.core.find(s.ids[i])->qty);
    }
    f.add(s.paused);
    f.add(s.cancelled);
    return f.h;
  }

  // ---------------------------------------------------------------- boundaries and moves
  std::vector<int64_t> bounds(const St& s, int inst) const {
    std::vector<int64_t> b;
    for (size_t i = 0; i < s.ids.size(); ++i) {
      if (inst_of(i) != inst || state_of(s, i) == proto::OrderGone) continue;
      const fb::Order* o = s.core.find(s.ids[i]);
      if (o->type == proto::StopMarket || o->type == proto::StopLimit) b.push_back(tpx(inst, o->trigger));
      if (o->type != proto::StopMarket && o->type != proto::Market) b.push_back(tpx(inst, o->price));
    }
    const int64_t pos = s.led.pos[inst];
    if (pos != 0) {
      // the loss at print m, holding every other instrument at its last print
      auto mk = marks(s);
      int64_t K = s.led.cash_cents;
      for (size_t j = 0; j < k_; ++j)
        if (static_cast<int>(j) != inst) K += mk[j] ? s.led.pos[j] * *mk[j] * ticks_[j] : s.led.cost[j];
      K = -K + s.led.fees_cents;  // loss(m) = K - pos * tick * m
      const int64_t L = e_.max_loss_cents;
      const int64_t t = ticks_[inst];
      int64_t m;
      if (pos > 0) m = floor_div(K - L - 1, pos * t);  // largest m with loss(m) > L
      else m = floor_div(L - K, -pos * t) + 1;         // smallest m with loss(m) > L
      if (m >= 1 && m <= fb::kMaxPrice) b.push_back(m);
    }
    std::sort(b.begin(), b.end());
    b.erase(std::unique(b.begin(), b.end()), b.end());
    return b;
  }

  // Landing points that are not boundaries (no neighbours of their own): prices where fees, not
  // any order, change what can happen. A walk up to one changes nothing in the book.
  std::vector<int64_t> fee_points(const St& s, int inst) const {
    std::vector<int64_t> b;
    const int64_t pos = s.led.pos[inst];
    if (pos > 0 && !s.cancelled) {
      // A long can be cancelled at any price, and the flatten pays a fee on that price's notional:
      // the stop-market at x - 1 (taker) or the limit at x (maker). Above the last boundary nothing
      // else stops a rally, so the fee is unbounded there. The fee boundary is the lowest last
      // print x at which cancelling would push fees past max_fees_cents; landing on it lets the
      // search find that path. (A short's flatten fee falls with the price, and its upward walk
      // is bounded by its exits or its loss boundary.)
      const int64_t t = ticks_[inst];
      auto over = [&](int64_t x) {
        const int64_t f = std::max(e_.fees.fee(x, pos, t, true), e_.fees.fee(x - 1, pos, t, false));
        return s.led.fees_cents + f > e_.max_fees_cents;
      };
      int64_t lo = 2, hi = kTopPrice;
      if (over(hi)) {
        while (lo < hi) {
          const int64_t mid = lo + (hi - lo) / 2;
          if (over(mid)) hi = mid;
          else lo = mid + 1;
        }
        b.push_back(lo);
      }
    }
    {
      // The fee sentinel. A fill at a market-determined price (a stop at the print, a child or hedge
      // released into a market already past its limit, a flatten) pays a fee on that price, and
      // above the last boundary nothing stops the price rising, so such fills have no fee bound.
      // The sentinel is the lowest print X at which one lot filled anywhere within a gap of it
      // would exceed max_fees_cents. It is just another landing point: if a market-priced fill
      // can happen up there, the search walks to X, makes it happen, and reports the violation;
      // if every fill up there is at an order's own limit, nothing happens at X.
      // If even the top of the price domain stays within the budget, the sentinel is the top
      // itself, so the certificate's max_fees_cents still covers market fills up there.
      const int64_t t = ticks_[inst], G = e_.max_gap_ticks;
      auto over = [&](int64_t x) { return s.led.fees_cents + e_.fees.fee(x - G - 1, 1, t, true) > e_.max_fees_cents; };
      int64_t lo = G + 2, hi = kTopPrice;
      if (over(hi)) {
        while (lo < hi) {
          const int64_t mid = lo + (hi - lo) / 2;
          if (over(mid)) hi = mid;
          else lo = mid + 1;
        }
      } else {
        lo = kTopPrice;
      }
      b.push_back(lo);
    }
    return b;
  }

  std::vector<Move> moves(const St& s) const {
    std::vector<Move> out;
    if (halted(s)) return out;
    const int64_t G = e_.max_gap_ticks;
    std::vector<std::vector<int64_t>> B(k_), S(k_);
    std::vector<int64_t> X(k_);
    std::vector<std::optional<int64_t>> down(k_), up(k_);
    for (size_t i = 0; i < k_; ++i) {
      B[i] = bounds(s, static_cast<int>(i));
      for (int64_t b : B[i])
        for (int64_t d = -1; d <= 1; ++d)
          if (b + d >= 1) S[i].push_back(b + d);
      for (int64_t f : fee_points(s, static_cast<int>(i))) S[i].push_back(f);
      std::sort(S[i].begin(), S[i].end());
      S[i].erase(std::unique(S[i].begin(), S[i].end()), S[i].end());
      X[i] = last_of(s, static_cast<int>(i)).value_or(marks_[i]);
      auto it = std::lower_bound(S[i].begin(), S[i].end(), X[i]);
      if (it != S[i].begin()) down[i] = *std::prev(it);
      auto jt = std::upper_bound(S[i].begin(), S[i].end(), X[i]);
      if (jt != S[i].end()) up[i] = *jt;
    }
    auto crosses = [](const std::vector<int64_t>& b, int64_t lo, int64_t hi, bool lo_open) {
      // some boundary in (lo, hi] (lo_open) or [lo, hi)
      for (int64_t x : b)
        if (lo_open ? (x > lo && x <= hi) : (x >= lo && x < hi)) return true;
      return false;
    };
    const int I = static_cast<int>(k_);
    for (int i = 0; i < I; ++i) {  // 1. gap down, nearest first
      const int64_t x = X[i], lo = std::max<int64_t>(1, x - G);
      std::vector<int64_t> c;
      for (int64_t y : S[i])
        if (y >= lo && y < x) c.push_back(y);
      c.push_back(lo);
      std::sort(c.begin(), c.end(), std::greater<>());
      c.erase(std::unique(c.begin(), c.end()), c.end());
      for (int64_t y : c)
        if (y < x && (!down[i] || y != *down[i]) && crosses(B[i], y, x, false)) out.push_back({Move::GapDown, i, x, y});
    }
    for (int i = 0; i < I; ++i)  // 2. cross down
      if (down[i]) out.push_back({Move::CrossDown, i, X[i], *down[i]});
    for (int i = 0; i < I; ++i)  // 3. cross up
      if (up[i]) out.push_back({Move::CrossUp, i, X[i], *up[i]});
    for (int i = 0; i < I; ++i) {  // 4. gap up, nearest first
      const int64_t x = X[i], hi = std::min<int64_t>(kTopPrice, x + G);
      std::vector<int64_t> c;
      for (int64_t y : S[i])
        if (y > x && y <= hi) c.push_back(y);
      c.push_back(hi);
      std::sort(c.begin(), c.end());
      c.erase(std::unique(c.begin(), c.end()), c.end());
      for (int64_t y : c)
        if (y > x && (!up[i] || y != *up[i]) && crosses(B[i], x, y, true)) out.push_back({Move::GapUp, i, x, y});
    }
    if (!s.paused && !s.cancelled) {  // 5. pause, if it would cancel something
      for (size_t i = 0; i < n_; ++i) {
        const uint8_t st = state_of(s, i);
        if (p_.nodes[i].native() && !p_.nodes[i].reduce_only && (st == proto::OrderResting || st == proto::OrderHeld)) {
          out.push_back({Move::Pause, 0, 0, 0});
          break;
        }
      }
    }
    if (!s.cancelled) out.push_back({Move::Cancel, 0, 0, 0});  // 6. cancel
    return out;
  }

  // ---------------------------------------------------------------- transitions
  // Applies `m` to `s` (whose core reports to `sink`), updates the ledger, and, if `ev` is given,
  // appends the move and the program's events to it.
  void apply(St& s, const Move& m, VSink& sink, json::Value* ev) const {
    const size_t nslots = s.ids.size();
    std::vector<uint8_t> before(nslots);
    for (size_t i = 0; i < nslots; ++i) before[i] = state_of(s, i);
    sink.evs.clear();
    CorePort port(s.core);
    int64_t at = 0;
    if (ev) ev->push(move_json(m));
    switch (m.t) {
      case Move::GapDown:
      case Move::CrossDown:
      case Move::CrossUp:
      case Move::GapUp:
        at = m.to;
        print(port, kEnv, static_cast<uint32_t>(m.inst), cpx(m.inst, m.to), mark_mode_, books_);
        break;
      case Move::Pause:
        // the nodes that can open exposure go (entry, scale, hedge); every reduce-only node stays,
        // armed or held: stops and takes only ever close
        for (size_t i = 0; i < n_; ++i) {
          const uint8_t st = state_of(s, i);
          if (!p_.nodes[i].native() || p_.nodes[i].reduce_only) continue;
          if (st != proto::OrderResting && st != proto::OrderHeld) continue;
          sink.evs.push_back({Ev::Cancel, s.ids[i], p_.nodes[i].side, 0, 0, false});
          s.core.cancel(s.ids[i]);
        }
        s.paused = 1;
        break;
      case Move::Cancel:
        for (size_t i = 0; i < nslots; ++i) {
          if (state_of(s, i) == proto::OrderGone) continue;
          sink.evs.push_back({Ev::Cancel, s.ids[i], 0, 0, 0, false});
          s.core.cancel(s.ids[i]);
        }
        for (size_t inst = 0; inst < k_; ++inst) {
          const int64_t pos = s.led.pos[inst];
          const auto last = s.core.last_price(static_cast<uint32_t>(inst));
          if (!pos || !last) continue;
          const FlattenIds f = flatten(port, kProg, static_cast<uint32_t>(inst), pos, *last,
                                       static_cast<uint32_t>(1000 + inst));
          s.ids[n_ + 2 * inst] = f.stop;
          s.ids[n_ + 2 * inst + 1] = f.limit;
        }
        s.cancelled = 1;
        break;
    }
    if (!at) at = last_of(s, 0).value_or(0);
    // stops armed before the move and not armed after it, without being cancelled, triggered
    std::vector<uint8_t> trig(nslots, 0), cancelled(nslots, 0), filled(nslots, 0), told(nslots, 0);
    for (const Ev& e : sink.evs) {
      const int i = slot_of(s, e.id);
      if (i < 0) continue;
      if (e.t == Ev::Cancel) cancelled[i] = 1;
      else filled[i] = 1;
    }
    for (size_t i = 0; i < nslots; ++i) {
      const uint8_t now = state_of(s, i);
      if (before[i] == proto::OrderPending && now != proto::OrderPending && !cancelled[i]) trig[i] = 1;
      // a held stop released by this move into an already crossed market fires inside it too
      if (before[i] == proto::OrderHeld && exit_slot(i) && i < n_ && p_.nodes[i].is_stop() && !cancelled[i] &&
          (filled[i] || (now == proto::OrderResting)))
        trig[i] = 1;
    }
    for (const Ev& e : sink.evs) {
      const int i = slot_of(s, e.id);
      if (i < 0) continue;
      const int inst = inst_of(i);
      if (e.t == Ev::Fill) {
        const int64_t px = tpx(inst, e.px);
        const int64_t fee = s.led.fill(inst, e.side, px, e.q, e.maker, ticks_[inst], e_.fees);
        if (ev) {
          if (trig[i] && !told[i]) {
            ev->push(triggered_json(s, i, at));
            told[i] = 1;
          }
          ev->push(json::obj({{"event", "filled"},
                              {"node", name_of(i)},
                              {"qty", e.q},
                              {"price", px},
                              {"fee_cents", fee}}));
        }
      } else if (ev) {
        ev->push(json::obj({{"event", "cancelled"}, {"node", name_of(i)}}));
      }
    }
    if (ev) {
      for (size_t i = 0; i < nslots; ++i) {
        if (!trig[i]) continue;
        if (!told[i]) ev->push(triggered_json(s, i, at));
        if (state_of(s, i) == proto::OrderResting) {
          const fb::Order* o = s.core.find(s.ids[i]);
          ev->push(json::obj({{"event", "rests"},
                              {"node", name_of(i)},
                              {"reason", o->side == proto::Sell ? "limit above print" : "limit below print"}}));
        }
      }
    }
  }

  json::Value triggered_json(const St& s, size_t i, int64_t at) const {
    json::Value lim = nullptr;
    if (i < n_ && p_.nodes[i].price) lim = static_cast<long long>(*p_.nodes[i].price);
    // the print that triggered it is its own book's last (a hedge's stop can fire on another book's move)
    at = last_of(s, inst_of(i)).value_or(at);
    return json::obj({{"event", "triggered"}, {"node", name_of(i)}, {"print", at}, {"limit", lim}});
  }

  json::Value move_json(const Move& m) const {
    json::Value o;
    switch (m.t) {
      case Move::GapDown:
      case Move::GapUp: o = json::obj({{"env", "gap"}, {"from", m.from}, {"to", m.to}}); break;
      case Move::CrossDown:
      case Move::CrossUp: o = json::obj({{"env", "print"}, {"at", m.to}}); break;
      case Move::Pause: return json::obj({{"keyholder", "pause"}});
      case Move::Cancel: return json::obj({{"keyholder", "cancel"}});
    }
    if (k_ > 1) o.set("instrument", p_.instruments[m.inst]);
    return o;
  }

  // ---------------------------------------------------------------- root
  fb::Config config() const {
    fb::Config c;
    c.books = static_cast<uint32_t>(k_);
    c.base = marks_[0] - 32;
    c.width = 64;
    c.accounts = 3;
    c.stp = fb::STP::ExpireMaker;
    c.pool = 64;
    c.trigger_ref = mark_mode_ ? 1 : 0;
    return c;
  }
  // The market before the program exists: one print at each mark, so the program is deployed
  // into a book with a last price, as venued deploys into the live book. Then the program is
  // deployed generation by generation. The settle print at each mark that follows (the first
  // entries of every path) is applied by the caller like any other move.
  void deploy(St& s) const {
    CorePort port(s.core);
    for (size_t i = 0; i < k_; ++i) print(port, kEnv, static_cast<uint32_t>(i), marks_[0], mark_mode_, books_);
    for (const auto& gen : p_.deploy_plan)
      for (const auto& id : gen) {
        const Node& nd = p_.nodes[p_.find(id)];
        const fb::OrderId pid = nd.parent_index >= 0 ? s.ids[nd.parent_index] : 0;
        if (nd.parent_index >= 0 && !pid) throw Invalid("deploy: parent of " + nd.id + " is gone");
        auto m = order_for(nd, kProg, static_cast<uint32_t>(nd.instrument_index), pid,
                           nd.group_index >= 0 ? static_cast<uint32_t>(nd.group_index + 1) : 0, 0);
        if (nd.price) m.price = cpx(nd.instrument_index, m.price);
        if (nd.trigger) m.trigger = cpx(nd.instrument_index, m.trigger);
        const fb::Result r = port.submit(m);
        if (r.status == proto::Rejected) throw Invalid("deploy: the engine rejected node " + nd.id);
        s.ids[nd.index] = r.id;
      }
  }
  std::vector<Move> opening() const {
    std::vector<Move> v;
    for (int i = 0; i < static_cast<int>(k_); ++i) v.push_back({Move::CrossDown, i, marks_[i], marks_[i]});
    return v;
  }

  void tick() {
    if ((++ticks_count_ & 255) == 0) {
      const auto ms = elapsed_ms();
      if (ms > opt_.timeout_ms) throw Timeout{};
    }
  }
  int64_t elapsed_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count();
  }

  const Program& p_;
  const Envelope& e_;
  const VerifyOptions& opt_;
  size_t n_, k_;
  std::vector<int64_t> ticks_, marks_;
  std::vector<uint32_t> books_;
  std::vector<int64_t> off_;
  int64_t cpx(int inst, int64_t true_px) const { return true_px - off_[inst]; }  // into the core
  int64_t tpx(int inst, int64_t core_px) const { return core_px + off_[inst]; }  // out of the core
  std::optional<int64_t> last_of(const St& s, int inst) const {
    auto l = s.core.last_price(static_cast<uint32_t>(inst));
    if (l) *l = tpx(inst, *l);
    return l;
  }
  bool mark_mode_ = false;
  std::chrono::steady_clock::time_point t0_;
  uint64_t ticks_count_ = 0;
};

// ------------------------------------------------------------------ the searches

struct Exhaustive {
  explicit Exhaustive(Verifier& vv) : v(vv) {}
  Verifier& v;
  std::unordered_set<uint64_t> seen;
  std::vector<Move> stack;
  Metrics max;
  std::vector<Move> worst;
  int64_t states = 0, paths = 0, depth = 0;
  std::optional<Violation> viol;
  std::vector<Move> viol_path;

  bool dfs(const St& s, int d) {
    for (const Move& m : v.moves(s)) {
      v.tick();
      VSink sink;
      St t(s, &sink);
      v.apply(t, m, sink, nullptr);
      ++paths;
      stack.push_back(m);
      if (seen.insert(v.key(t)).second) {
        ++states;
        depth = std::max<int64_t>(depth, d + 1);
        const Metrics mt = v.eval(t);
        if (auto vi = v.violation(mt)) {
          viol = vi;
          viol_path = stack;
          return true;
        }
        if (mt.loss > max.loss) {
          max.loss = mt.loss;
          worst = stack;
        }
        max.fees = std::max(max.fees, mt.fees);
        max.trades = std::max(max.trades, mt.trades);
        max.exposure = std::max(max.exposure, mt.exposure);
        if (dfs(t, d + 1)) return true;
      }
      stack.pop_back();
    }
    return false;
  }
};

// Shortest path from `root` to a state satisfying `pred`, by iterative deepening up to `max_depth`.
template <class Pred>
std::optional<std::vector<Move>> shortest(Verifier& v, const St& root, int max_depth, Pred pred,
                                          size_t max_states = SIZE_MAX, int64_t* states = nullptr,
                                          int64_t* paths = nullptr) {
  std::unordered_map<uint64_t, int> memo;
  std::vector<Move> stack;
  std::function<bool(const St&, int, int)> go = [&](const St& s, int d, int D) -> bool {
    if (d == D) return false;
    for (const Move& m : v.moves(s)) {
      v.tick();
      VSink sink;
      St t(s, &sink);
      v.apply(t, m, sink, nullptr);
      if (paths) ++*paths;
      const uint64_t k = v.key(t);
      auto it = memo.find(k);
      if (it != memo.end() && it->second <= d + 1) continue;
      if (memo.size() >= max_states) return false;
      memo[k] = d + 1;
      stack.push_back(m);
      if (pred(t) || go(t, d + 1, D)) return true;
      stack.pop_back();
    }
    return false;
  };
  for (int D = 1; D <= max_depth; ++D) {
    memo.clear();
    stack.clear();
    const bool found = go(root, 0, D);
    if (states) *states = std::max<int64_t>(*states, static_cast<int64_t>(memo.size()) + 1);
    if (found) return stack;
  }
  return std::nullopt;
}

}  // namespace

VerifyResult verify(const Program& p, const Envelope& e, const Instruments& ins, const VerifyOptions& opt) {
  VerifyResult res;
  for (const auto& i : p.instruments)
    if (std::find(e.instruments.begin(), e.instruments.end(), i) == e.instruments.end()) {
      json::Value allowed = json::Value::array();
      for (const auto& a : e.instruments) allowed.push(a);
      res.exit_code = 3;
      res.out = json::obj({{"contract_version", kContractVersion},
                           {"status", "rejected"},
                           {"violates", "instruments"},
                           {"limit", allowed},
                           {"value_on_path", i},
                           {"counterexample", json::Value::array()}});
      return res;
    }
  if (p.report.relayed > 0) throw Invalid("backend minsky: relayed nodes are not supported");

  Verifier v(p, e, ins, opt);
  const fb::Config cfg = v.config();
  const size_t nslots = p.nodes.size() + 2 * p.instruments.size();
  VSink root_sink;
  St root(cfg, &root_sink, nslots, p.instruments.size());
  v.deploy(root);
  const std::vector<Move> open = v.opening();
  for (const Move& m : open) v.apply(root, m, root_sink, nullptr);

  auto render = [&](const std::vector<Move>& path) {
    json::Value out = json::Value::array();
    VSink s0;
    auto cur = std::make_unique<St>(cfg, &s0, nslots, p.instruments.size());
    v.deploy(*cur);
    std::vector<Move> all = open;
    all.insert(all.end(), path.begin(), path.end());
    for (const Move& m : all) {
      VSink sink;
      auto next = std::make_unique<St>(*cur, &sink);
      v.apply(*next, m, sink, &out);
      cur = std::move(next);
    }
    const Metrics mt = v.eval(*cur);
    json::Value mark = json::obj({{"event", "mark"}, {"position", cur->led.pos[0]}, {"loss_cents", mt.loss}});
    if (p.instruments.size() > 1) {
      json::Value ps = json::Value::object();
      for (size_t i = 0; i < p.instruments.size(); ++i) ps.set(p.instruments[i], cur->led.pos[i]);
      mark.set("positions", ps);
    }
    out.push(mark);
    return std::make_pair(out, mt);
  };

  Exhaustive x{v};
  bool from_probe = false;
  try {
    x.seen.insert(v.key(root));
    x.states = 1;
    const Metrics m0 = v.eval(root);
    x.max = m0;
    if (auto vi = v.violation(m0)) {
      x.viol = vi;
    } else {
      int64_t ps = 0, pp = 0;
      auto probe = shortest(
          v, root, kProbeDepth, [&](const St& s) { return v.violation(v.eval(s)).has_value(); }, kProbeStates, &ps,
          &pp);
      if (probe) {  // already the shortest violating path; the rendered path says which bound
        x.viol = Violation{};
        x.viol_path = *probe;
        x.states = ps;
        x.paths = pp;
        x.depth = static_cast<int64_t>(probe->size());
        from_probe = true;
      } else {
        x.dfs(root, 0);
      }
    }
    const auto explored = [&] {
      return json::obj({{"states", x.states}, {"paths", x.paths}, {"depth", x.depth}, {"ms", v.elapsed_ms()}});
    };
    if (x.viol) {
      std::vector<Move> path = x.viol_path;
      if (opt.shortest && !from_probe && !path.empty() && x.states <= kShortestStates) {
        auto sp = shortest(v, root, static_cast<int>(path.size()), [&](const St& s) {
          return v.violation(v.eval(s)).has_value();
        });
        if (sp) path = *sp;
      }
      auto [cx, mt] = render(path);
      const Violation vi = *v.violation(mt);
      res.exit_code = 3;
      res.out = json::obj({{"contract_version", kContractVersion},
                           {"status", "rejected"},
                           {"violates", vi.what},
                           {"limit", vi.limit},
                           {"value_on_path", vi.value},
                           {"counterexample", cx},
                           {"explored", explored()}});
      return res;
    }
    std::vector<Move> path = x.worst;
    if (opt.shortest && !path.empty() && x.states <= kShortestStates) {
      const int64_t target = x.max.loss;
      auto sp = shortest(v, root, static_cast<int>(path.size()), [&](const St& s) { return v.eval(s).loss == target; });
      if (sp) path = *sp;
    }
    auto [wp, mt] = render(path);
    (void)mt;
    int64_t native = 0;
    for (const auto& nd : p.nodes) native += nd.native() ? 1 : 0;
    res.exit_code = 0;
    res.out = json::obj(
        {{"contract_version", kContractVersion},
         {"status", "certified"},
         {"bound", json::obj({{"max_loss_cents", std::max<int64_t>(0, x.max.loss)},
                              {"max_fees_cents", x.max.fees},
                              {"max_exposure_lots", x.max.exposure},
                              {"halts_within_trades", x.max.trades}})},
         {"assumes", json::obj({{"max_gap_ticks", e.max_gap_ticks},
                                {"fill", e.fill},
                                {"fee_bps", json::obj({{"maker", e.maker_bps}, {"taker", e.taker_bps}})},
                                {"trigger_ref", e.trigger_ref},
                                {"margin", "isolated_per_program"}})},
         {"envelope_checked", json::obj({{"max_loss_cents", e.max_loss_cents},
                                         {"max_fees_cents", e.max_fees_cents},
                                         {"max_trades", e.max_trades},
                                         {"max_exposure_lots", e.max_exposure_lots}})},
         {"worst_path", wp},
         {"explored", explored()},
         {"nodes", json::obj({{"total", static_cast<int64_t>(p.nodes.size())}, {"native", native}, {"relayed", 0}})},
         {"locked_collateral_cents", p.report.locked_collateral_cents},
         {"engine", engine_id()}});
    return res;
  } catch (const Timeout&) {
    res.exit_code = 4;
    res.out = json::obj({{"contract_version", kContractVersion}, {"status", "timeout"}});
    return res;
  }
}

std::vector<PathMove> path_moves(const json::Value& path, const Program& p) {
  std::vector<PathMove> out;
  for (const auto& e : path.as_array()) {
    PathMove m;
    if (const json::Value* env = e.get("env")) {
      const std::string& t = env->as_string();
      if (t == "print") {
        m.type = PathMove::Print;
        m.to = e.at("at").as_int();
      } else if (t == "gap") {
        m.type = PathMove::Gap;
        m.to = e.at("to").as_int();
      } else {
        throw Invalid("unknown env move \"" + t + "\"");
      }
      const std::string ins = e.str_or("instrument", p.instruments[0]);
      m.inst = -1;
      for (size_t i = 0; i < p.instruments.size(); ++i)
        if (p.instruments[i] == ins) m.inst = static_cast<int>(i);
      if (m.inst < 0) throw Invalid("path names unknown instrument " + ins);
      out.push_back(m);
    } else if (const json::Value* kh = e.get("keyholder")) {
      m.type = kh->as_string() == "pause" ? PathMove::Pause : PathMove::Cancel;
      out.push_back(m);
    }
  }
  return out;
}

int64_t path_final_loss(const json::Value& path) {
  int64_t v = 0;
  for (const auto& e : path.as_array())
    if (e.str_or("event", "") == "mark") v = e.at("loss_cents").as_int();
  return v;
}

}  // namespace bk
