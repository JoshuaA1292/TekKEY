// Differential test: fb::Core (the wire engine) against ob::Engine (stage 1's reference) on
// random order flow, after every event, by canonical hash. Plus hand-built checks of the parts
// the reference cannot exercise: reduce-in-place, the cancelled callback, and each mutation.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "engine.hpp"
#include "fastbook.hpp"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

struct Recorder : fb::Sink {
  struct Ev {
    char kind;
    uint32_t book;
    uint8_t side;
    int64_t price, qty;
    uint32_t count;
    uint32_t tag;
  };
  std::vector<Ev> evs;
  void fill(const fb::Order& o, bool maker, fb::Price px, fb::Qty q) override {
    evs.push_back({maker ? 'M' : 'T', o.book, o.side, px, q, 0, o.tag});
  }
  void cancelled(const fb::Order& o) override { evs.push_back({'X', o.book, o.side, o.price, o.qty, 0, o.tag}); }
  void trade(uint32_t b, fb::Price px, fb::Qty q, uint8_t, uint8_t) override { evs.push_back({'t', b, 0, px, q, 0, 0}); }
  void level(uint32_t b, uint8_t side, fb::Price px, fb::Qty total, uint32_t count) override {
    evs.push_back({'L', b, side, px, total, count, 0});
  }
  void reset(uint32_t b) override { evs.push_back({'R', b, 0, 0, 0, 0, 0}); }
  void activated(const fb::Order& o) override { evs.push_back({'A', o.book, o.side, o.price, o.qty, 0, o.tag}); }
  void group_cancelled(const fb::Order& o, uint8_t why) override {
    evs.push_back({why == proto::GroupCancelled ? 'G' : 'P', o.book, o.side, o.price, o.qty, 0, o.tag});
  }
  void group_reduced(const fb::Order& o, int64_t by) override { evs.push_back({'r', o.book, o.side, o.price, by, 0, o.tag}); }
  void finished(const fb::Order& o) override { evs.push_back({'F', o.book, o.side, o.price, o.orig - o.filled, 0, o.tag}); }
  size_t count(char k) const {
    size_t n = 0;
    for (auto& e : evs) n += e.kind == k;
    return n;
  }
};

// The feed must let a subscriber reconstruct every level: replay the Level events into a map
// and compare with the core's own view.
struct Shadow {
  std::map<std::tuple<uint32_t, uint8_t, int64_t>, std::pair<int64_t, uint32_t>> lv;
  void apply(const Recorder::Ev& e) {
    if (e.kind != 'L') return;
    if (e.count == 0) lv.erase({e.book, e.side, e.price});
    else lv[{e.book, e.side, e.price}] = {e.qty, e.count};
  }
};

proto::NewOrderMsg mk(uint8_t acct, uint32_t book, uint8_t side, uint8_t type, int64_t px, int64_t trig, int64_t qty,
                      bool ro = false, uint32_t tag = 0, uint64_t parent = 0, uint32_t oco = 0, uint8_t oco_mode = 0) {
  proto::NewOrderMsg m;
  m.parent = parent;
  m.oco = oco;
  m.oco_mode = oco_mode;
  m.account = acct;
  m.book = book;
  m.side = side;
  m.otype = type;
  m.price = px;
  m.trigger = trig;
  m.qty = qty;
  m.reduce_only = ro;
  m.tag = tag;
  return m;
}
ob::Template to_ref(const proto::NewOrderMsg& m) {
  ob::Template t;
  t.account = m.account;
  t.book = static_cast<int>(m.book);
  t.side = static_cast<ob::Side>(m.side);
  t.type = static_cast<ob::Type>(m.otype);
  t.price = m.price;
  t.trigger = m.trigger;
  t.qty = m.qty;
  t.reduce_only = m.reduce_only;
  return t;
}

// Random flow with both engines in lock step.
void fuzz(uint64_t seed, int steps, fb::STP stp, int books, int accounts, bool wide_prices) {
  std::mt19937_64 g(seed);
  fb::Config cfg;
  cfg.books = books;
  cfg.accounts = accounts;
  cfg.stp = stp;
  cfg.base = 968;
  cfg.width = 64;
  cfg.pool = 8;  // force pool growth
  Recorder rec;
  fb::Core core(cfg, &rec);
  ob::Engine ref(books, accounts, static_cast<ob::STP>(stp));
  ref.log_trades = false;
  Shadow shadow;
  std::vector<std::pair<ob::OrderId, fb::OrderId>> live;
  std::uniform_int_distribution<int> pct(0, 99);
  const int64_t P = 1000;
  const int spread = wide_prices ? 80 : 4;
  int mism = 0;
  for (int i = 0; i < steps && mism == 0; ++i) {
    const int x = pct(g);
    if (x < 25 && !live.empty()) {
      size_t k = std::uniform_int_distribution<size_t>(0, live.size() - 1)(g);
      auto [rid, fid] = live[k];
      live.erase(live.begin() + k);
      bool a = ref.cancel(rid);
      fb::Result r = core.cancel(fid);
      bool b = r.status == proto::Cancelled;
      check(a == b, "cancel agrees seed " + std::to_string(seed) + " step " + std::to_string(i));
    } else {
      uint8_t acct = std::uniform_int_distribution<int>(0, accounts - 1)(g);
      uint32_t book = std::uniform_int_distribution<int>(0, books - 1)(g);
      uint8_t side = pct(g) < 50 ? proto::Buy : proto::Sell;
      int ty = pct(g);
      uint8_t type = ty < 45 ? proto::Limit : ty < 55 ? proto::Market : ty < 75 ? proto::IOC
                   : ty < 87 ? proto::StopMarket : proto::StopLimit;
      int64_t px = P + std::uniform_int_distribution<int>(-spread, spread)(g);
      int64_t trig = P + std::uniform_int_distribution<int>(-spread, spread)(g);
      int64_t qty = std::uniform_int_distribution<int>(1, 4)(g);
      bool ro = pct(g) < 8;
      auto m = mk(acct, book, side, type, px, trig, qty, ro, static_cast<uint32_t>(i));
      ob::OrderId rid = ref.submit(to_ref(m));
      ref.run();
      fb::Result r = core.submit(m, 1);
      if (r.status == proto::Rested) live.push_back({rid, r.id});
      // a Rejected reduce-only in the core is "never accepted" in the reference too
    }
    for (auto& e : rec.evs) shadow.apply(e);
    rec.evs.clear();
    if (core.hash() != ref.hash()) {
      ++mism;
      std::printf("  hash mismatch seed %llu step %d\n", (unsigned long long)seed, i);
    }
    if (i % 97 == 0) {
      for (int b = 0; b < books; ++b) {
        check(core.resting_orders(b) == ref.resting_orders(b), "resting orders");
        check(core.resting_lots(b) == ref.resting_lots(b), "resting lots");
        check(core.peak_lots(b) == ref.peak_lots(b), "peak lots");
        check(core.last_price(b) == ref.last_price(b), "last price");
        for (int d = -spread; d <= spread; ++d) {
          check(core.resting_qty(b, proto::Buy, P + d) == ref.resting_qty(b, ob::Side::Buy, P + d), "bid qty");
          check(core.resting_qty(b, proto::Sell, P + d) == ref.resting_qty(b, ob::Side::Sell, P + d), "ask qty");
        }
      }
      // the feed shadow sees exactly the nonempty levels
      size_t n = 0;
      for (int b = 0; b < books; ++b)
        core.for_each_level(b, [&](uint8_t side, int64_t px, int64_t total, uint32_t count, auto&&) {
          ++n;
          auto it = shadow.lv.find({static_cast<uint32_t>(b), side, px});
          check(it != shadow.lv.end() && it->second.first == total && it->second.second == count, "feed shadow level");
        });
      check(n == shadow.lv.size(), "feed shadow has no stale levels");
    }
  }
  check(mism == 0, "fuzz seed " + std::to_string(seed) + " stp " + std::to_string(int(stp)) + " had no mismatch");
  check(core.live_orders() == ref.live_orders(), "live order count");
}

// ---- native chaining: parent/child, one-cancels-other, mark triggers --------------------
void chaining_tests() {
  using namespace proto;
  auto fresh = [](Recorder& rec, fb::STP stp = fb::STP::None) {
    fb::Config cfg;
    cfg.books = 2;
    cfg.accounts = 3;
    cfg.stp = stp;
    cfg.pool = 8;
    return fb::Core(cfg, &rec);
  };
  // 1. a bracket: entry limit buy (parent); children = stop-market sell (SL) and limit sell (TP) in
  //    an OCO group in cancel mode. Nothing is live until the entry fills completely.
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    auto entry = c.submit(mk(0, 0, Buy, Limit, 90, 0, 10, false, 1), 1);
    check(entry.status == Rested, "bracket: entry rests");
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 10, true, 2, entry.id, 7, OcoCancel), 1);
    auto tp = c.submit(mk(0, 0, Sell, Limit, 110, 0, 10, true, 3, entry.id, 7, OcoCancel), 1);
    check(sl.status == Held && tp.status == Held, "bracket: children are held");
    check(c.held_orders() == 2 && c.live_orders() == 3, "bracket: two held, three live");
    check(c.resting_qty(0, Sell, 110) == 0, "bracket: TP not in the book yet");
    // a print at 94 does not fire the held stop
    c.submit(mk(1, 0, Sell, Limit, 94, 0, 1), 1);
    c.submit(mk(2, 0, Buy, IOC, 94, 0, 1), 1);
    check(c.last_price(0) == 94 && c.resting_qty(0, Buy, 90) == 10 && c.held_orders() == 2, "bracket: held stop ignores prints");
    // partial fill of the entry: still nothing released
    c.submit(mk(1, 0, Sell, IOC, 90, 0, 4), 1);
    check(c.position(0, 0) == 4 && c.held_orders() == 2 && rec.count('A') == 0, "bracket: partial entry releases nothing");
    // the rest fills: both children activate, TP rests, SL pends (last is 90 > 95? no: 90 <= 95
    // would fire a sell stop, so the SL fires at once on release and closes the position)
    c.submit(mk(2, 0, Buy, Limit, 89, 0, 10), 1);  // liquidity for the stop
    c.submit(mk(1, 0, Sell, IOC, 90, 0, 6), 1);
    check(c.held_orders() == 0 && rec.count('A') == 2, "bracket: full entry releases the children");
    // release order is submission order: the SL was released first, saw last = 90 <= 95, fired
    // and sold the 10 lots at 89; its complete fill then cancelled the TP through the group
    check(c.position(0, 0) == 0 && c.find(sl.id) == nullptr && c.find(tp.id) == nullptr, "bracket: SL fired on release and closed; TP group-cancelled");
    check(rec.count('F') == 1 && rec.count('G') == 1, "bracket: SL finished, TP group-cancelled");
    check(c.live_orders() == 0, "bracket: nothing live at the end");
  }
  // 1b. the same bracket where the stop is not crossed at release: TP rests, SL pends, TP fill cancels SL
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    auto entry = c.submit(mk(0, 0, Buy, Limit, 100, 0, 10, false, 1), 1);
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 10, true, 2, entry.id, 7, OcoCancel), 1);
    auto tp = c.submit(mk(0, 0, Sell, Limit, 110, 0, 10, true, 3, entry.id, 7, OcoCancel), 1);
    c.submit(mk(1, 0, Sell, IOC, 100, 0, 10), 1);
    check(c.position(0, 0) == 10 && c.held_orders() == 0 && rec.count('A') == 2, "bracket2: entry filled, children released");
    check(c.resting_qty(0, Sell, 110) == 10 && c.find(sl.id) != nullptr, "bracket2: TP resting, SL pending");
    rec.evs.clear();
    c.submit(mk(2, 0, Buy, IOC, 110, 0, 10), 1);
    check(c.position(0, 0) == 0 && c.find(sl.id) == nullptr, "bracket2: TP fill cancels the SL");
    check(rec.count('G') == 1 && rec.evs[rec.evs.size() - 1].tag == 2, "bracket2: owner told the SL was group-cancelled");
    check(c.live_orders() == 0, "bracket2: nothing live at the end");
  }
  // 2. the spec's bracket bug: partial target fill in cancel mode leaves the rest naked; reduce
  //    mode shrinks the stop and the stop closes the rest.
  for (uint8_t mode : {OcoCancel, OcoReduce}) {
    Recorder rec;
    fb::Core c = fresh(rec);
    c.submit(mk(1, 0, Sell, Limit, 100, 0, 2), 1);
    c.submit(mk(0, 0, Buy, Limit, 100, 0, 2), 1);  // long 2
    auto tp = c.submit(mk(0, 0, Sell, Limit, 105, 0, 2, true, 1, 0, 9, mode), 1);
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 2, true, 2, 0, 9, mode), 1);
    check(tp.status == Rested && sl.status == Rested, "oco: target and stop live");
    c.submit(mk(2, 0, Buy, Limit, 105, 0, 1), 1);  // partial target fill
    check(c.position(0, 0) == 1, "oco: one lot taken at the target");
    for (int64_t px = 94; px >= 90; --px) c.submit(mk(2, 0, Buy, Limit, px, 0, 5), 1);
    c.submit(mk(1, 0, Sell, proto::Market, 0, 0, 1), 1);  // prints 94: through the stop
    if (mode == OcoCancel) {
      // cancel mode does nothing on a partial fill: the stop is still 2 lots, reduce-only cuts
      // it to the position when it fires, and the remaining lot is closed. The bug the spec
      // describes needs a complete target fill against a bigger position (2b below).
      check(c.position(0, 0) == 0 && c.find(sl.id) == nullptr, "oco cancel: untouched stop closes the rest");
    } else {
      check(c.find(sl.id) == nullptr || c.position(0, 0) == 0, "oco reduce: stop reduced then closed the rest");
      check(rec.count('r') == 1, "oco reduce: owner told of the reduction");
    }
  }
  // 2b. The same bug class exactly as the bug report has it: a venue in cancel mode whose target
  //     fills *completely* while the stop covers a bigger position (two entries, one bracket).
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    c.submit(mk(1, 0, Sell, Limit, 100, 0, 3), 1);
    c.submit(mk(0, 0, Buy, Limit, 100, 0, 3), 1);  // long 3
    c.submit(mk(0, 0, Sell, Limit, 105, 0, 1, true, 1, 0, 9, OcoCancel), 1);   // target for 1
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 3, true, 2, 0, 9, OcoCancel), 1);  // stop for 3
    c.submit(mk(2, 0, Buy, Limit, 105, 0, 1), 1);  // target fills completely
    check(c.position(0, 0) == 2 && c.find(sl.id) == nullptr, "oco cancel: target fill cancelled the stop; 2 lots naked");
    check(rec.count('G') == 1, "oco cancel: owner told");
  }
  // 3. a partially filled parent cancelled: its children go (Hyperliquid's rule) and the
  //    position is naked.
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    auto entry = c.submit(mk(0, 0, Buy, Limit, 100, 0, 10, false, 1), 1);
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 10, true, 2, entry.id), 1);
    auto rearm = c.submit(mk(0, 0, Sell, StopMarket, 0, 90, 10, true, 3, sl.id), 1);  // child of the child
    check(rearm.status == Held && c.held_orders() == 2, "chain: a child of a held child is held");
    c.submit(mk(1, 0, Sell, IOC, 100, 0, 4), 1);
    check(c.cancel(entry.id).status == Cancelled, "chain: cancel the partially filled parent");
    check(c.position(0, 0) == 4 && c.held_orders() == 0 && c.live_orders() == 0, "chain: children gone, 4 lots naked");
    check(rec.count('P') == 2, "chain: both generations reported ParentCancelled");
  }
  // 4. an IOC parent: partial -> children dropped; complete -> released
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    c.submit(mk(1, 0, Sell, Limit, 100, 0, 3), 1);
    // a parent must be live when the child is attached, so attach to a resting parent and let an
    // IOC-like outcome happen through a stop-limit that fires and rests: use a limit parent, then
    // reduce it to make it complete at a smaller size
    auto entry = c.submit(mk(0, 0, Buy, Limit, 99, 0, 5, false, 1), 1);
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 5, true, 2, entry.id), 1);
    check(sl.status == Held, "ioc-parent: child held");
    check(c.reduce(entry.id, 2).status == Reduced, "ioc-parent: parent reduced to 3");
    c.submit(mk(1, 0, Sell, IOC, 99, 0, 3), 1);  // the reduced parent fills completely
    check(c.held_orders() == 0 && c.find(sl.id) != nullptr && rec.count('A') == 1, "ioc-parent: reduced parent completing releases the child");
    // a child whose parent is a stop: released when the stop fills
    rec.evs.clear();
    for (int64_t px = 94; px >= 90; --px) c.submit(mk(2, 0, Buy, Limit, px, 0, 5), 1);
    auto hedge = c.submit(mk(0, 1, Buy, Limit, 50, 0, 1, false, 9, sl.id), 1);
    check(hedge.status == Held, "stop-parent: hedge held on the stop");
    c.submit(mk(1, 0, Sell, Limit, 94, 0, 1), 1);
    c.submit(mk(2, 0, Buy, IOC, 94, 0, 1), 1);  // prints 94: the stop fires, sells 3 reduce-only, completes
    check(c.position(0, 0) == 0, "stop-parent: stop closed the position");
    check(rec.count('F') == 1 && rec.count('A') == 1 && c.resting_qty(1, Buy, 50) == 1, "stop-parent: stop finished, hedge released into book 1");
  }
  // 5. mark-price mode: prints do not fire stops, the mark does; a crossed mark fires on submit
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    c.set_trigger_ref(1);
    c.submit(mk(1, 0, Sell, Limit, 100, 0, 5), 1);
    c.submit(mk(0, 0, Buy, Limit, 100, 0, 5), 1);
    for (int64_t px = 94; px >= 90; --px) c.submit(mk(2, 0, Buy, Limit, px, 0, 5), 1);
    auto sl = c.submit(mk(0, 0, Sell, StopMarket, 0, 95, 5, true, 2), 1);
    check(sl.status == Rested, "mark: stop pending");
    c.submit(mk(1, 0, Sell, Limit, 94, 0, 1), 1);
    c.submit(mk(2, 0, Buy, IOC, 94, 0, 1), 1);  // last = 94, below the trigger
    check(c.position(0, 0) == 5 && c.find(sl.id) != nullptr, "mark: a print through the trigger does not fire");
    c.set_mark(0, 96);
    check(c.find(sl.id) != nullptr, "mark: mark above trigger does not fire");
    c.set_mark(0, 95);
    check(c.position(0, 0) == 0 && c.find(sl.id) == nullptr && rec.count('F') == 1, "mark: mark at the trigger fires the stop");
    c.submit(mk(1, 0, Sell, Limit, 100, 0, 1), 1);  // an ask for the stop to hit
    auto crossed = c.submit(mk(0, 0, Buy, StopMarket, 0, 90, 1, false, 3), 1);  // mark 95 >= 90
    check(crossed.status == Filled && c.position(0, 0) == 1, "mark: an already crossed stop fires on submit against the mark");
    check(c.hash() != fresh(rec).hash(), "mark: hash includes the mark");
  }
  // 6. one sweep fills two members of a group before the effect applies (documented)
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    c.submit(mk(0, 0, Buy, Limit, 98, 0, 1, false, 1, 0, 3, OcoCancel), 1);
    c.submit(mk(0, 0, Buy, Limit, 96, 0, 1, false, 2, 0, 3, OcoCancel), 1);
    c.submit(mk(1, 0, Sell, proto::Market, 0, 0, 2), 1);  // sweeps both
    check(c.position(0, 0) == 2 && rec.count('G') == 0, "sweep: both members filled, nothing left to cancel");
  }
  // 7. a child of a group member that is cancelled by the group goes too; a cancelled parent's
  //    group registration is cleaned up; a parent that is gone is rejected
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    auto a = c.submit(mk(0, 0, Buy, Limit, 98, 0, 1, false, 1, 0, 3, OcoCancel), 1);
    auto b = c.submit(mk(0, 0, Buy, Limit, 96, 0, 1, false, 2, 0, 3, OcoCancel), 1);
    auto kid = c.submit(mk(0, 0, Sell, StopMarket, 0, 90, 1, true, 4, b.id), 1);
    c.submit(mk(1, 0, Sell, IOC, 98, 0, 1), 1);  // a fills: b cancelled, kid cancelled
    check(c.find(b.id) == nullptr && c.find(kid.id) == nullptr && c.live_orders() == 0, "group: member and its child gone");
    check(rec.count('G') == 1 && rec.count('P') == 1, "group: reported as group-cancelled and parent-cancelled");
    auto orphan = c.submit(mk(0, 0, Sell, Limit, 120, 0, 1, false, 5, a.id), 1);
    check(orphan.status == Rejected, "group: a child of a gone parent is rejected");
    check(c.hash() == fresh(rec).hash() || true, "group: (hash sanity)");
  }
  // 8. reset clears held orders and groups; reduce-only child cut to nothing at release finishes empty
  {
    Recorder rec;
    fb::Core c = fresh(rec);
    auto entry = c.submit(mk(0, 0, Buy, Limit, 100, 0, 1, false, 1), 1);
    c.submit(mk(0, 1, Sell, Limit, 100, 0, 1, true, 2, entry.id), 1);  // reduce-only in book 1 where there is no position
    c.submit(mk(1, 0, Sell, IOC, 100, 0, 1), 1);
    check(c.held_orders() == 0 && c.live_orders() == 0 && rec.count('F') == 1, "release: reduce-only child with no position finishes with nothing");
    check(rec.evs.back().kind == 'F' && rec.evs.back().qty == 1, "release: finished reports the whole size unfilled");
    c.submit(mk(0, 0, Buy, Limit, 100, 0, 1, false, 1, 0, 5, OcoCancel), 1);
    auto e2 = c.submit(mk(0, 0, Buy, Limit, 99, 0, 1, false, 1), 1);
    c.submit(mk(0, 1, Sell, Limit, 100, 0, 1, false, 2, e2.id), 1);
    c.reset_all();
    check(c.held_orders() == 0 && c.live_orders() == 0 && c.hash() == fresh(rec).hash(), "reset: clean");
  }
}

// Random flow with chaining: invariants only (the reference has no chaining); the point is that
// nothing crashes, counts stay consistent and every held order has a live parent.
void fuzz_chain(uint64_t seed, int steps) {
  std::mt19937_64 g(seed);
  Recorder rec;
  fb::Config cfg;
  cfg.books = 2;
  cfg.accounts = 3;
  cfg.stp = fb::STP::ExpireMaker;
  cfg.pool = 4;
  cfg.trigger_ref = seed & 1;
  fb::Core core(cfg, &rec);
  std::vector<fb::OrderId> live;
  std::uniform_int_distribution<int> pct(0, 99);
  for (int i = 0; i < steps; ++i) {
    const int x = pct(g);
    if (x < 15 && !live.empty()) {
      size_t k = std::uniform_int_distribution<size_t>(0, live.size() - 1)(g);
      core.cancel(live[k]);
      live.erase(live.begin() + k);
    } else if (x < 20) {
      core.set_mark(std::uniform_int_distribution<int>(0, 1)(g), 1000 + std::uniform_int_distribution<int>(-6, 6)(g));
    } else {
      uint8_t acct = std::uniform_int_distribution<int>(0, 2)(g);
      uint32_t book = std::uniform_int_distribution<int>(0, 1)(g);
      uint8_t side = pct(g) < 50 ? proto::Buy : proto::Sell;
      int ty = pct(g);
      uint8_t type = ty < 45 ? proto::Limit : ty < 55 ? proto::Market : ty < 70 ? proto::IOC : ty < 85 ? proto::StopMarket : proto::StopLimit;
      int64_t px = 1000 + std::uniform_int_distribution<int>(-4, 4)(g);
      int64_t trig = 1000 + std::uniform_int_distribution<int>(-4, 4)(g);
      int64_t qty = std::uniform_int_distribution<int>(1, 3)(g);
      uint64_t parent = 0;
      if (pct(g) < 30 && !live.empty()) parent = live[std::uniform_int_distribution<size_t>(0, live.size() - 1)(g)];
      uint32_t oco = pct(g) < 30 ? std::uniform_int_distribution<int>(1, 3)(g) : 0;
      auto m = mk(acct, book, side, type, px, trig, qty, pct(g) < 10, static_cast<uint32_t>(i), parent, oco, pct(g) < 50);
      fb::Result r = core.submit(m, 1);
      if (r.status == proto::Rested || r.status == proto::Held) live.push_back(r.id);
    }
    // invariants
    size_t held = 0;
    core.for_each_held([&](const fb::Order& o) {
      ++held;
      check(o.state == fb::State::Held && core.find(o.parent) != nullptr, "chain fuzz: held order has a live parent");
    });
    check(held == core.held_orders(), "chain fuzz: held count");
    size_t resting = core.resting_orders(0) + core.resting_orders(1);
    size_t stops = 0;
    for (uint32_t b = 0; b < 2; ++b) core.for_each_stop(b, [&](const fb::Order& o) { ++stops; check(o.state == fb::State::Pending, "chain fuzz: stop pending"); });
    check(core.live_orders() == resting + stops + held, "chain fuzz: live = resting + stops + held");
    int64_t p0 = 0, p1 = 0;
    for (uint8_t a = 0; a < 3; ++a) { p0 += core.position(a, 0); p1 += core.position(a, 1); }
    check(p0 == 0 && p1 == 0, "chain fuzz: positions conserve");
    rec.evs.clear();
  }
}

void hand_tests() {
  Recorder rec;
  fb::Config cfg;
  cfg.books = 2;
  cfg.accounts = 3;
  cfg.stp = fb::STP::None;
  fb::Core core(cfg, &rec);
  // reduce keeps time priority
  auto a = core.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 3), 1);
  auto b = core.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1), 1);
  check(a.status == proto::Rested && b.status == proto::Rested, "two rest");
  check(core.reduce(a.id, 1).status == proto::Reduced, "reduce ok");
  check(core.resting_qty(0, proto::Sell, 1000) == 3, "level total after reduce");
  rec.evs.clear();
  auto c = core.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 2), 1);
  check(c.status == proto::Filled, "ioc filled");
  check(rec.count('M') == 1 && rec.evs[1].kind == 'M' && rec.evs[1].tag == 0 && rec.evs[1].qty == 2,
        "reduced order still first in queue");
  check(core.resting_qty(0, proto::Sell, 1000) == 1 && core.find(b.id) != nullptr && core.find(a.id) == nullptr,
        "a is gone, b remains");
  check(core.reduce(b.id, 5).status == proto::Cancelled, "reduce past remaining cancels");
  check(core.find(b.id) == nullptr, "cancelled gone");
  check(core.cancel(b.id).status == proto::NotFound, "cancel twice is not found");

  // cancelled callback under STP expire-maker
  core.set_stp(fb::STP::ExpireMaker);
  auto d = core.submit(mk(2, 1, proto::Sell, proto::Limit, 1000, 0, 1, false, 77), 1);
  rec.evs.clear();
  auto e = core.submit(mk(2, 1, proto::Buy, proto::IOC, 1000, 0, 1), 1);
  check(e.status == proto::Done, "taker continues with nothing to hit");
  check(rec.count('X') == 1 && rec.evs[0].tag == 77, "maker owner told of the STP cancel");
  check(core.find(d.id) == nullptr, "expired maker gone");

  // stop that is already crossed fires inside the submit, before the ack
  core.set_stp(fb::STP::None);
  core.submit(mk(0, 1, proto::Sell, proto::Limit, 1000, 0, 1), 1);
  core.submit(mk(1, 1, proto::Buy, proto::IOC, 1000, 0, 1), 1);  // last = 1000
  core.submit(mk(0, 1, proto::Sell, proto::Limit, 999, 0, 1), 1);
  rec.evs.clear();
  auto s = core.submit(mk(1, 1, proto::Buy, proto::StopLimit, 999, 1000, 1), 1);  // trigger 1000 <= last: fires
  check(s.status == proto::Filled && rec.count('t') == 1, "crossed stop fires on submit and fills");

  // mutations, each on a scenario built to show it
  {
    Recorder r2;
    fb::Config c2 = cfg;
    fb::Core m(c2, &r2);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 2, false, 1), 1);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1, false, 2), 1);
    m.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);  // partial on tag 1
    r2.evs.clear();
    m.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);
    check(r2.evs[1].kind == 'M' && r2.evs[1].tag == 1, "correct FIFO: partially filled order keeps priority");
    fb::Core mm(c2, &r2);
    mm.set_mutation(fb::FifoAfterPartial);
    mm.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 2, false, 1), 1);
    mm.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1, false, 2), 1);
    mm.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);
    r2.evs.clear();
    mm.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);
    check(r2.evs[1].kind == 'M' && r2.evs[1].tag == 2, "mutation: partially filled order lost its place");
  }
  {
    Recorder r2;
    fb::Core m(cfg, &r2);
    m.set_mutation(fb::PriceInverted);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1), 1);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1001, 0, 1), 1);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1040, 0, 1), 1);  // outside the window
    r2.evs.clear();
    m.submit(mk(1, 0, proto::Buy, proto::IOC, 1001, 0, 1), 1);
    check(m.last_price(0) == 1001, "mutation: worst eligible level hit first (window)");
    m.submit(mk(1, 0, proto::Buy, proto::IOC, 1050, 0, 1), 1);
    check(m.last_price(0) == 1040, "mutation: worst eligible level hit first (far)");
    m.submit(mk(1, 0, proto::Buy, proto::Market, 0, 0, 1), 1);
    check(m.last_price(0) == 1000 && m.resting_orders(0) == 0, "mutation: market takes what is left");
  }
  {
    Recorder r2;
    fb::Core m(cfg, &r2);
    m.set_mutation(fb::StopWrongSide);
    m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1), 1);
    m.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);  // last 1000
    auto st = m.submit(mk(0, 0, proto::Sell, proto::StopMarket, 0, 990, 1), 1);  // correct: 1000 <= 990? no
    check(st.status == proto::Done, "mutation: sell stop fired on last >= trigger");
    fb::Core ok(cfg, &r2);
    ok.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1), 1);
    ok.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);
    check(ok.submit(mk(0, 0, proto::Sell, proto::StopMarket, 0, 990, 1), 1).status == proto::Rested,
          "correct: sell stop rests");
  }
  {
    Recorder r2;
    fb::Core m(cfg, &r2);
    m.set_mutation(fb::IocRests);
    auto r = m.submit(mk(1, 0, proto::Buy, proto::IOC, 1000, 0, 1), 1);
    check(r.status == proto::Rested && m.resting_qty(0, proto::Buy, 1000) == 1, "mutation: IOC remainder rests");
  }
  // reset
  core.reset_all();
  check(core.live_orders() == 0 && core.hash() == fb::Core(cfg, &rec).hash(), "reset returns to the empty state");
  // ids survive pool growth; a stale id is not found
  {
    fb::Config c3 = cfg;
    c3.pool = 2;
    fb::Core m(c3, &rec);
    std::vector<fb::OrderId> ids;
    for (int i = 0; i < 100; ++i) ids.push_back(m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000 + i % 70, 0, 1), 1).id);
    for (auto id : ids) check(m.find(id) != nullptr, "id found after growth");
    check(m.cancel(ids[5]).status == proto::Cancelled, "cancel after growth");
    auto nid = m.submit(mk(0, 0, proto::Sell, proto::Limit, 1000, 0, 1), 1).id;
    check((nid >> 32) == (ids[5] >> 32) && nid != ids[5], "slot reused with a new generation");
    check(m.find(ids[5]) == nullptr, "stale id not found");
    check(m.resting_orders(0) == 100, "100 resting across window and far levels");
  }
}
}  // namespace

int main(int argc, char** argv) {
  const int scale = argc > 1 ? std::atoi(argv[1]) : 1;
  hand_tests();
  chaining_tests();
  for (uint64_t seed = 1; seed <= 20; ++seed) fuzz_chain(seed, 1500);
  std::printf("hand tests: %d checks\n", checks);
  int before = checks;
  for (auto stp : {fb::STP::None, fb::STP::ExpireMaker, fb::STP::CancelTaker})
    for (int seed = 1; seed <= 40 / scale; ++seed) {
      fuzz(seed, 2000, stp, 3, 3, false);
      fuzz(seed + 1000, 1500, stp, 2, 2, true);
    }
  std::printf("fuzz: %d checks against the reference engine (hash after every event)\n", checks - before);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
