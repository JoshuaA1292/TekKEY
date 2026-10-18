// Engine semantics and determinism tests.
#include <algorithm>
#include <cstdio>
#include <random>

#include "engine.hpp"

using namespace ob;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(cond)) {                                                        \
      ++failures;                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                     \
  } while (0)

static Template lim(int acct, int book, Side s, Price p, Qty q, bool ro = false) {
  return {acct, book, s, Type::Limit, p, 0, q, ro};
}
static Template mkt(int acct, int book, Side s, Qty q) { return {acct, book, s, Type::Market, 0, 0, q, false}; }
static Template ioc(int acct, int book, Side s, Price p, Qty q) { return {acct, book, s, Type::IOC, p, 0, q, false}; }
static Template stopm(int acct, int book, Side s, Price trig, Qty q) {
  return {acct, book, s, Type::StopMarket, 0, trig, q, false};
}
static Template stopl(int acct, int book, Side s, Price trig, Price px, Qty q) {
  return {acct, book, s, Type::StopLimit, px, trig, q, false};
}

static void price_time_priority() {
  std::puts("price-time priority");
  Engine e(1, 3);
  OrderId a = e.submit(lim(0, 0, Side::Sell, 100, 1));
  OrderId b = e.submit(lim(1, 0, Side::Sell, 100, 1));
  OrderId c = e.submit(lim(0, 0, Side::Sell, 99, 1));
  e.submit(mkt(2, 0, Side::Buy, 2));
  e.run();
  CHECK(e.trades.size() == 2);
  CHECK(e.trades[0].maker == c && e.trades[0].price == 99);
  CHECK(e.trades[1].maker == a && e.trades[1].price == 100);
  CHECK(e.resting_qty(0, Side::Sell, 100) == 1);
  (void)b;
}

static void limit_partial_and_ioc() {
  std::puts("limit partial fill rests remainder; IOC remainder cancelled");
  Engine e(1, 3);
  e.submit(lim(1, 0, Side::Sell, 100, 1));
  e.submit(lim(2, 0, Side::Buy, 100, 3));
  CHECK(e.trades.size() == 1 && e.resting_qty(0, Side::Buy, 100) == 2);
  Engine f(1, 3);
  f.submit(lim(1, 0, Side::Sell, 101, 1));
  f.submit(ioc(2, 0, Side::Buy, 101, 5));
  CHECK(f.trades.size() == 1 && f.resting_qty(0, Side::Buy, 101) == 0 && f.resting_orders() == 0);
  // IOC does not cross a worse price
  Engine g(1, 3);
  g.submit(lim(1, 0, Side::Sell, 102, 1));
  g.submit(ioc(2, 0, Side::Buy, 101, 1));
  CHECK(g.trades.empty() && g.resting_qty(0, Side::Sell, 102) == 1);
}

static void stops_cascade_and_order() {
  std::puts("sell stops: trigger at <= trigger, cascade, submission order");
  Engine e(1, 3);
  for (Price p : {99, 98, 97}) e.submit(lim(1, 0, Side::Buy, p, 1));
  OrderId s1 = e.submit(stopm(0, 0, Side::Sell, 99, 1));
  OrderId s2 = e.submit(stopm(0, 0, Side::Sell, 98, 1));
  e.submit(mkt(2, 0, Side::Sell, 1));
  e.run();
  CHECK(e.trades.size() == 3);
  CHECK(e.trades[0].price == 99 && e.trades[1].price == 98 && e.trades[2].price == 97);
  CHECK(e.trades[1].taker == s1 && e.trades[2].taker == s2);

  Engine f(1, 3);
  for (Price p : {95, 94, 93}) f.submit(lim(1, 0, Side::Buy, p, 1));
  OrderId a = f.submit(stopm(0, 0, Side::Sell, 95, 1));  // submitted first
  OrderId b = f.submit(stopm(0, 0, Side::Sell, 96, 1));
  f.submit(mkt(2, 0, Side::Sell, 1));  // prints 95: both fire
  f.run();
  CHECK(f.trades.size() == 3 && f.trades[1].taker == a && f.trades[2].taker == b);

  std::puts("stop-limit rests after trigger");
  Engine g(1, 3);
  g.submit(lim(1, 0, Side::Buy, 99, 1));
  g.submit(lim(1, 0, Side::Buy, 99, 1));
  g.submit(stopl(0, 0, Side::Sell, 99, 99, 3));
  g.submit(mkt(2, 0, Side::Sell, 1));
  g.run();
  CHECK(g.trades.size() == 2 && g.resting_qty(0, Side::Sell, 99) == 2);

  std::puts("buy stop fires on an intermediate print inside a sweep");
  Engine h(1, 3);
  h.submit(lim(1, 0, Side::Buy, 101, 1));
  h.submit(lim(1, 0, Side::Buy, 99, 1));
  h.submit(lim(1, 0, Side::Sell, 102, 1));
  h.submit(stopm(0, 0, Side::Buy, 101, 1));
  h.submit(mkt(2, 0, Side::Sell, 2));  // prints 101 then 99
  h.run();
  CHECK(h.trades.size() == 3 && h.trades[2].price == 102);
}

static void reduce_only() {
  std::puts("reduce-only is cut to the position");
  Engine e(1, 3);
  e.submit(lim(1, 0, Side::Sell, 100, 2));
  e.submit(lim(0, 0, Side::Buy, 100, 2));
  CHECK(e.position(0, 0) == 2);
  e.submit(lim(2, 0, Side::Buy, 99, 10));
  e.submit(lim(0, 0, Side::Sell, 99, 5, true));
  CHECK(e.position(0, 0) == 0 && e.resting_qty(0, Side::Sell, 99) == 0);
  e.submit(lim(0, 0, Side::Sell, 99, 1, true));  // flat: rejected
  CHECK(e.position(0, 0) == 0 && e.trades.size() == 2);
}

static void oco_cancel_vs_reduce() {
  std::puts("bracket: one-cancels-other in cancel mode vs reduce mode on a partial target fill");
  for (Act mode : {Act::Cancel, Act::Reduce}) {
    Program p;
    p.templates.push_back(lim(0, 0, Side::Sell, 105, 2));       // 0: target
    p.templates.push_back(stopm(0, 0, Side::Sell, 95, 2));      // 1: stop
    p.links.push_back({On::Fill, 0, std::nullopt, {{mode, 1}}});
    p.initial = {{Act::Submit, 0}, {Act::Submit, 1}};
    Engine e(1, 3, STP::None, &p);
    e.submit(lim(1, 0, Side::Sell, 100, 2));
    e.submit(lim(0, 0, Side::Buy, 100, 2));  // long 2
    e.start();
    e.run();
    e.submit(lim(2, 0, Side::Buy, 105, 1));  // partial target fill
    e.run();
    CHECK(e.position(0, 0) == 1);
    for (Price px = 94; px >= 90; --px) e.submit(lim(2, 0, Side::Buy, px, 5));
    e.submit(mkt(1, 0, Side::Sell, 1));  // prints 94: through the stop
    e.run();
    if (mode == Act::Cancel)
      CHECK(e.position(0, 0) == 1);  // the bug: remaining lot unprotected
    else
      CHECK(e.position(0, 0) == 0);  // stop reduced to 1 and closed the rest
  }
}

static void self_trade_prevention() {
  std::puts("self-trade prevention modes");
  for (STP m : {STP::None, STP::ExpireMaker, STP::CancelTaker}) {
    Engine e(1, 3, m);
    e.submit(lim(0, 0, Side::Sell, 100, 1));
    e.submit(lim(0, 0, Side::Sell, 101, 1));
    e.submit(ioc(0, 0, Side::Buy, 101, 1));
    if (m == STP::None) CHECK(e.trades.size() == 1 && e.trades[0].price == 100);
    if (m == STP::ExpireMaker) CHECK(e.trades.empty() && e.resting_orders() == 0);
    if (m == STP::CancelTaker) CHECK(e.trades.empty() && e.resting_orders() == 2);
  }
}

static Template random_order(std::mt19937_64& g) {
  std::uniform_int_distribution<int> acct(0, 2), book(0, 1), type(0, 4), side(0, 1), qty(1, 4),
      px(95, 105), pct(0, 99);
  Template t;
  t.account = acct(g);
  t.book = book(g);
  t.side = side(g) ? Side::Buy : Side::Sell;
  t.type = static_cast<Type>(type(g));
  t.price = px(g);
  t.trigger = px(g);
  t.qty = qty(g);
  t.reduce_only = pct(g) < 10;
  return t;
}

static void determinism_replay() {
  std::puts("replay: same book + events -> same trades and state hash (500 seeds x 300 events)");
  Program prog;  // a router program too, so links are covered
  prog.templates.push_back(lim(0, 0, Side::Buy, 98, 1));
  prog.templates.push_back(stopm(0, 0, Side::Sell, 96, 1));
  prog.links.push_back({On::Fill, 0, std::nullopt, {{Act::Submit, 1}}});
  prog.links.push_back({On::Fill, 1, std::nullopt, {{Act::Submit, 0}}});
  prog.initial = {{Act::Submit, 0}};
  int mismatches = 0;
  for (int seed = 0; seed < 500; ++seed) {
    std::vector<uint64_t> hashes[2];
    std::vector<Trade> trades[2];
    for (int rep = 0; rep < 2; ++rep) {
      std::mt19937_64 g(seed);
      Engine e(2, 3, seed % 3 == 0 ? STP::ExpireMaker : STP::None, &prog);
      e.start();
      e.run(1000);
      for (int i = 0; i < 300; ++i) {
        e.submit(random_order(g));
        e.run(1000);
        hashes[rep].push_back(e.hash());
      }
      trades[rep] = e.trades;
    }
    bool same = hashes[0] == hashes[1] && trades[0].size() == trades[1].size();
    for (size_t i = 0; same && i < trades[0].size(); ++i) {
      const Trade &x = trades[0][i], &y = trades[1][i];
      same = x.book == y.book && x.price == y.price && x.qty == y.qty && x.taker == y.taker &&
             x.maker == y.maker && x.buyer == y.buyer && x.seller == y.seller;
    }
    mismatches += !same;
  }
  CHECK(mismatches == 0);
}

static void conservation() {
  std::puts("conservation: positions sum to zero and cash sums to zero after random flow");
  for (int seed = 0; seed < 200; ++seed) {
    std::mt19937_64 g(seed + 1000);
    Engine e(2, 3);
    for (int i = 0; i < 300; ++i) {
      e.submit(random_order(g));
      e.run();
    }
    int64_t cash = 0;
    for (int b = 0; b < 2; ++b) {
      Qty p = 0;
      for (int a = 0; a < 3; ++a) p += e.position(a, b);
      CHECK(p == 0);
    }
    for (int a = 0; a < 3; ++a) cash += e.cash(a);
    CHECK(cash == 0);
  }
}

static void lots_tracking() {
  std::puts("resting_lots equals the sum of level totals; peak_lots >= current and never decreases");
  for (int seed = 0; seed < 100; ++seed) {
    std::mt19937_64 g(seed + 5000);
    Engine e(2, 3, seed % 2 ? STP::ExpireMaker : STP::None);
    Qty peak[2] = {0, 0};
    for (int i = 0; i < 200; ++i) {
      e.submit(random_order(g));
      e.run();
      for (int b = 0; b < 2; ++b) {
        Qty sum = 0;
        for (Price p = 90; p <= 110; ++p) sum += e.resting_qty(b, Side::Buy, p) + e.resting_qty(b, Side::Sell, p);
        CHECK(e.resting_lots(b) == sum);
        CHECK(e.peak_lots(b) >= sum && e.peak_lots(b) >= peak[b]);
        peak[b] = e.peak_lots(b);
      }
    }
  }
}

static void review_regressions() {
  std::puts("rejected reduce-only order fires no accept link; hash sees queued work");
  Program p;
  p.templates.push_back(lim(0, 0, Side::Sell, 100, 1, true));  // 0: reduce-only, account flat
  p.templates.push_back(lim(0, 0, Side::Buy, 90, 1));          // 1: must not appear
  p.links.push_back({On::Accept, 0, std::nullopt, {{Act::Submit, 1}}});
  p.initial = {{Act::Submit, 0}};
  Engine e(1, 3, STP::None, &p);
  e.start();
  e.run();
  CHECK(e.resting_orders() == 0);

  Engine a(1, 3), b(1, 3);
  for (Engine* x : {&a, &b}) x->submit(lim(1, 0, Side::Buy, 99, 1));
  a.submit(stopm(0, 0, Side::Sell, 101, 1));   // fires at once on the next print: queued
  a.submit(mkt(2, 0, Side::Sell, 1));          // prints 99, stop queued but not run
  b.submit(mkt(2, 0, Side::Sell, 1));
  CHECK(!a.idle() && a.hash() != b.hash());
}

int main() {
  price_time_priority();
  limit_partial_and_ioc();
  stops_cascade_and_order();
  reduce_only();
  oco_cancel_vs_reduce();
  self_trade_prevention();
  determinism_replay();
  conservation();
  review_regressions();
  lots_tracking();
  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
