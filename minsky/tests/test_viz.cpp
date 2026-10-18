// Tests for the visualiser's feed-state logic against a live exchange. Start one first:
//   ./build/exchange --tcp 7100 --feed 7102 --books 300 --quiet &
//   ./build/test_viz [--orders tcp://127.0.0.1:7100] [--feed tcp://127.0.0.1:7102]
//                    [--layout tests/viz_layout.txt] [--viz ./build/viz]
// Orders are submitted on the order port; the feed is read through viz::FeedState exactly as the
// visualiser reads it; assertions compare what the state decodes with what was put in the books
// and with what the exchange itself reports through control messages.
#define VIZ_NO_MAIN
#include "../src/viz.cpp"

#include <cstdio>
#include <functional>
#include <string>

namespace {

int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (cond) {                                                                  \
      ++g_pass;                                                                  \
    } else {                                                                     \
      ++g_fail;                                                                  \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)
#define CHECK_EQ(a, b)                                                                            \
  do {                                                                                            \
    auto va_ = (a);                                                                               \
    auto vb_ = (b);                                                                               \
    if (va_ == vb_) {                                                                             \
      ++g_pass;                                                                                   \
    } else {                                                                                      \
      ++g_fail;                                                                                   \
      std::printf("  FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,         \
                  (long long)va_, (long long)vb_);                                                \
    }                                                                                             \
  } while (0)

using namespace viz;

// ---- synchronous helpers on the order connection (one message in flight at a time) ----
proto::AckMsg submit(net::Channel& ch, uint8_t account, uint32_t book, uint8_t side, uint8_t otype, int64_t price,
                     int64_t qty) {
  static uint32_t tag = 100;
  proto::NewOrderMsg m;
  m.account = account;
  m.book = book;
  m.side = side;
  m.otype = otype;
  m.price = price;
  m.qty = qty;
  m.tag = ++tag;
  ch.send(m);
  ch.flush();
  proto::Any a;
  while (ch.recv(a))
    if (a.type() == proto::Ack && a.as<proto::AckMsg>().tag == m.tag) return a.as<proto::AckMsg>();
  throw std::runtime_error("order connection died");
}
proto::AckMsg sell_at_P(net::Channel& ch, uint32_t book, int64_t qty, int64_t P = 1000, uint8_t acct = 0) {
  return submit(ch, acct, book, proto::Sell, proto::Limit, P, qty);
}
void trade_once(net::Channel& ch, uint32_t book, int64_t P = 1000) {
  sell_at_P(ch, book, 1, P, 0);
  submit(ch, 1, book, proto::Buy, proto::IOC, P, 1);
}
proto::ControlReplyMsg control(net::Channel& ch, uint8_t op, int64_t arg = 0, int64_t arg2 = 0) {
  static uint32_t tag = 5000;
  proto::ControlMsg c;
  c.op = op;
  c.arg = arg;
  c.arg2 = arg2;
  c.tag = ++tag;
  ch.send(c);
  ch.flush();
  proto::Any a;
  while (ch.recv(a))
    if (a.type() == proto::ControlReply && a.as<proto::ControlReplyMsg>().tag == c.tag)
      return a.as<proto::ControlReplyMsg>();
  throw std::runtime_error("order connection died");
}
void cancel(net::Channel& ch, uint64_t id) {
  proto::CancelMsg c;
  c.order_id = id;
  c.tag = 77;
  ch.send(c);
  ch.flush();
  proto::Any a;
  while (ch.recv(a))
    if (a.type() == proto::Ack && a.as<proto::AckMsg>().tag == 77) return;
}

// Pump the feed through the state until `pred` holds or the timeout passes.
bool pump_until(net::Channel& feed, FeedState& st, const std::function<bool()>& pred,
                uint64_t timeout_ns = 3 * kSecond) {
  const uint64_t t0 = net::now_ns();
  proto::Any a;
  for (;;) {
    const uint64_t now = net::now_ns();
    int n = 0;
    while (n < 4096 && feed.try_recv(a)) {
      st.apply(a, now);
      ++n;
    }
    if (pred()) return true;
    if (now - t0 > timeout_ns) return false;
    if (!n) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}
void pump_for(net::Channel& feed, FeedState& st, uint64_t ns) {
  pump_until(feed, st, [] { return false; }, ns);
}

std::string run_capture(const std::string& cmd, int& status) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    status = -1;
    return out;
  }
  char buf[4096];
  size_t r;
  while ((r = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, r);
  status = pclose(p);
  return out;
}

// ---- pure unit tests (no exchange) ----
void test_sliding() {
  std::printf("sliding window\n");
  Sliding s;
  s.add(0);
  s.add(kSecond / 2);
  s.add(kSecond + 200'000'000);
  CHECK_EQ(s.count(kSecond + 200'000'000), 2u);  // t=0 has fallen out of the ~1 s window
  CHECK_EQ(s.count(kSecond / 2), 2u);              // asking about an earlier time never grows the count
  CHECK_EQ(s.count(5 * kSecond), 0u);
  Sliding e;
  CHECK_EQ(e.count(123), 0u);
}
void test_mutation_names() {
  std::printf("mutation names\n");
  CHECK(mutation_text(0) == "none");
  CHECK(mutation_text(1) == "fifo-after-partial");
  CHECK(mutation_text(2) == "price-inverted");
  CHECK(mutation_text(4) == "stop-wrong-side");
  CHECK(mutation_text(8) == "ioc-rests");
  CHECK(mutation_text(3) == "fifo-after-partial+price-inverted");
}
void test_keys() {
  std::printf("key parser\n");
  KeyParser k;
  k.feed("\x1b[A", 3);
  k.feed("q", 1);
  k.feed("\x1b", 1);  // split escape: the rest arrives in the next read
  KeyEvent e;
  CHECK(k.next(e) && e.key == Key::Up);
  CHECK(k.next(e) && e.key == Key::Char && e.ch == 'q');
  CHECK(!k.next(e));
  k.feed("[D\x1bOC", 5);
  CHECK(k.next(e) && e.key == Key::Left);
  CHECK(k.next(e) && e.key == Key::Right);
  CHECK(!k.next(e));
  k.feed("\x1bx", 2);  // a lone ESC followed by a plain key
  CHECK(k.next(e) && e.key == Key::Char && e.ch == 'x');
}
void test_layout(const std::string& path) {
  std::printf("layout %s\n", path.c_str());
  Layout L = Layout::from(tbl::load_layout(path));
  CHECK_EQ(L.P, 1000);
  CHECK_EQ(L.width, 4);
  CHECK_EQ(L.height, 4);
  CHECK_EQ(L.pixels.size(), 16u);
  CHECK_EQ(L.pixel(1, 2), 9);
  CHECK_EQ(L.pixel(5, 5), -1);
  CHECK_EQ(L.keys.size(), 4u);
  CHECK_EQ(L.frame, 20);
  CHECK_EQ(L.score, 21);
  CHECK_EQ(L.death, 22);
  CHECK_EQ(L.fruit_x.size(), 4u);
  CHECK_EQ(L.cells.size(), 8u);
  CHECK_EQ(L.generation, 58);
  CHECK_EQ(L.key_account, 2);
  CHECK(L.snake());
  Layout none;
  CHECK(!none.snake());
  FeedState empty;
  CHECK(!empty.decode_bits({}).has_value());
}
void test_link_refused() {
  std::printf("link to a closed port\n");
  Link l;
  l.url = "tcp://127.0.0.1:1";
  const uint64_t now = net::now_ns();
  CHECK(!l.maintain(now));
  CHECK(!l.ok());
  CHECK_EQ(l.connects, 0u);
  CHECK(l.next_try > now);
  CHECK(!l.maintain(now + 1));  // retried at most once a second
}

// ---- live tests ----
struct Live {
  std::string orders_url, feed_url, layout_path, viz_bin;
  std::unique_ptr<net::Channel> orders, feed;
  Layout L;
  FeedState st;
  Live(std::string o, std::string f, std::string l, std::string v)
      : orders_url(std::move(o)), feed_url(std::move(f)), layout_path(std::move(l)), viz_bin(std::move(v)),
        L(Layout::from(tbl::load_layout(layout_path))), st(L.P) {}
  uint32_t P() const { return static_cast<uint32_t>(L.P); }
};

void test_connect(Live& t) {
  std::printf("connect + initial snapshot\n");
  t.orders = net::connect(t.orders_url);
  control(*t.orders, proto::ResetBooks);
  control(*t.orders, proto::SetDelayUs, 0);
  control(*t.orders, proto::SetMutation, 0);
  t.feed = net::connect(t.feed_url);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.synced(); }));
  CHECK(t.st.hello().has_value());
  CHECK(t.st.books() >= 60);
  CHECK_EQ(t.st.gaps(), 1u);
  CHECK_EQ(t.st.snapshots_completed(), 1u);
  CHECK_EQ(t.st.resting_orders(), 0u);
  CHECK_EQ(t.st.resting_books(), 0u);
}

void test_pixels(Live& t) {
  std::printf("pixels\n");
  for (uint32_t b : {0u, 5u, 10u, 15u}) sell_at_P(*t.orders, b, 1, t.L.P);
  sell_at_P(*t.orders, 3, 3, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.lit(15) && t.st.lit(3); }));
  for (int y = 0; y < t.L.height; ++y)
    for (int x = 0; x < t.L.width; ++x) {
      const long long b = t.L.pixel(x, y);
      const bool want = b == 0 || b == 3 || b == 5 || b == 10 || b == 15;
      CHECK_EQ(t.st.lit(b), want);
    }
  CHECK_EQ(t.st.sell_qty(3), 3);
  CHECK_EQ(t.st.sell_qty(3, t.L.P + 1), 0);
  CHECK_EQ(t.st.qty(3, proto::Buy, t.L.P), 0);
  CHECK_EQ(t.st.resting_orders(), 5u);
  CHECK_EQ(t.st.resting_books(), 5u);
  CHECK(!t.st.lit(-1));
}

void test_unlit(Live& t) {
  std::printf("unlit by cancel and by trade\n");
  auto ack = sell_at_P(*t.orders, 6, 1, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.lit(6); }));
  cancel(*t.orders, ack.order_id);
  CHECK(pump_until(*t.feed, t.st, [&] { return !t.st.lit(6); }));
  CHECK_EQ(t.st.resting_orders(), 5u);
  const uint64_t before = t.st.trades_total();
  submit(*t.orders, 1, 0, proto::Buy, proto::IOC, t.L.P, 1);  // eats the pixel at book 0
  CHECK(pump_until(*t.feed, t.st, [&] { return !t.st.lit(0); }));
  CHECK_EQ(t.st.trades_in(0), 1u);
  CHECK_EQ(t.st.trades_total(), before + 1);
  CHECK_EQ(t.st.resting_orders(), 4u);
  CHECK(t.st.lit(3) && t.st.lit(5) && t.st.lit(10) && t.st.lit(15));
}

void test_score(Live& t) {
  std::printf("score register\n");
  const uint32_t sb = static_cast<uint32_t>(t.L.score);
  sell_at_P(*t.orders, sb, 3, t.L.P);
  sell_at_P(*t.orders, sb, 4, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.sell_qty(sb) == 7; }));
  sell_at_P(*t.orders, sb, 10, t.L.P + 1);  // another price: not the score
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.sell_qty(sb, t.L.P + 1) == 10; }));
  CHECK_EQ(t.st.sell_qty(sb), 7);
  CHECK(t.st.levels().count(LevelKey{sb, proto::Sell, t.L.P + 1}) == 1);
  CHECK_EQ(t.st.levels().at(LevelKey{sb, proto::Sell, t.L.P}).count, 2u);
}

void test_bits(Live& t) {
  std::printf("fruit / head / tail bit registers\n");
  // fruit (1,2): x bit 0 -> book 23; y bit 1 -> book 28 (an unlit cell of the 4x4 board)
  sell_at_P(*t.orders, 23, 1, t.L.P);
  sell_at_P(*t.orders, 28, 1, t.L.P);
  // head (15,0): all x bits, no y bits
  for (uint32_t b = 31; b <= 34; ++b) sell_at_P(*t.orders, b, 2, t.L.P);
  // tail (1,9): x bit 0 -> 39; y bits 0,3 -> 43,46
  sell_at_P(*t.orders, 39, 1, t.L.P);
  sell_at_P(*t.orders, 43, 1, t.L.P);
  sell_at_P(*t.orders, 46, 1, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.lit(46); }));
  CHECK_EQ(t.st.decode_bits(t.L.fruit_x).value_or(-1), 1);
  CHECK_EQ(t.st.decode_bits(t.L.fruit_y).value_or(-1), 2);
  CHECK_EQ(t.st.decode_bits(t.L.head_x).value_or(-1), 15);
  CHECK_EQ(t.st.decode_bits(t.L.head_y).value_or(-1), 0);
  CHECK_EQ(t.st.decode_bits(t.L.tail_x).value_or(-1), 1);
  CHECK_EQ(t.st.decode_bits(t.L.tail_y).value_or(-1), 9);
}

void test_frame_and_death(Live& t) {
  std::printf("frame counter and death flash\n");
  const uint32_t fb = static_cast<uint32_t>(t.L.frame), db = static_cast<uint32_t>(t.L.death);
  const uint64_t before = t.st.trades_total();
  for (int i = 0; i < 5; ++i) trade_once(*t.orders, fb, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.trades_in(fb) == 5; }));
  const uint64_t now = net::now_ns();
  CHECK_EQ(t.st.fps(fb, now), 5.0);
  CHECK(t.st.trades_last_second(now) >= 5);
  CHECK_EQ(t.st.trades_total(), before + 5);
  CHECK_EQ(t.st.fps(fb, now + 2 * kSecond), 0.0);  // the window slides
  CHECK(!t.st.lit(fb));  // a frame trade leaves nothing resting
  CHECK(!t.st.traded_within(db, now, kSecond / 2));
  trade_once(*t.orders, db, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.trades_in(db) == 1; }));
  const uint64_t now2 = net::now_ns();
  CHECK(t.st.traded_within(db, now2, kSecond / 2));
  CHECK(!t.st.traded_within(db, now2 + kSecond, kSecond / 2));
  CHECK(t.st.last_trade_ns(db).has_value());
  CHECK(!t.st.last_trade_ns(99).has_value());
}

void test_stats(Live& t) {
  std::printf("stats: latency knob, mutation, orders/s\n");
  const uint64_t seen = t.st.stats_seen();
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats_seen() > seen; }));
  CHECK(t.st.stats().has_value());
  CHECK_EQ(t.st.stats()->delay_us, 0u);
  CHECK_EQ(t.st.stats()->mutation, 0u);
  control(*t.orders, proto::SetDelayUs, 100);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->delay_us == 100; }));
  control(*t.orders, proto::SetMutation, 2);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->mutation == 2; }));
  CHECK(mutation_text(t.st.stats()->mutation) == "price-inverted");
  control(*t.orders, proto::SetDelayUs, 0);
  control(*t.orders, proto::SetMutation, 0);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->mutation == 0 && t.st.stats()->delay_us == 0; }));
  // orders/s comes from Stats deltas: a burst of orders must show up as a positive rate
  for (int i = 0; i < 40; ++i) {
    sell_at_P(*t.orders, 60, 1, t.L.P);
    submit(*t.orders, 1, 60, proto::Buy, proto::IOC, t.L.P, 1);
  }
  pump_for(*t.feed, t.st, 300'000'000);
  CHECK(t.st.orders_per_sec() > 0);
  CHECK(t.st.stats_trades_per_sec() > 0);
  CHECK(t.st.stats()->trades >= t.st.trades_total());  // exchange counts every trade we saw (and the mirror)
  CHECK_EQ(t.st.stats()->feed_drops, 0u);
}

void test_matches_exchange(Live& t) {
  std::printf("resting orders agree with the exchange's own BookInfo / BookQty\n");
  pump_for(*t.feed, t.st, 100'000'000);
  size_t resting = 0, books = 0;
  for (uint32_t b = 0; b < 64; ++b) {
    const auto r = control(*t.orders, proto::BookInfo, b);
    resting += static_cast<size_t>(r.arg);
    books += r.arg > 0;
  }
  CHECK_EQ(t.st.resting_orders(), resting);
  CHECK_EQ(t.st.resting_books(), books);
  for (uint32_t b : {3u, 5u, 21u, 31u, 46u, 60u}) {
    const auto r = control(*t.orders, proto::BookQty, proto::pack_book_side(b, proto::Sell), t.L.P);
    CHECK_EQ(t.st.sell_qty(b), r.arg);
  }
}

void test_keyboard(Live& t) {
  std::printf("keyboard orders through App (no tty)\n");
  Options o;
  o.orders = t.orders_url;
  o.feed = t.feed_url;
  o.layout = t.layout_path;
  App app(o);
  const uint64_t t0 = net::now_ns();
  while (!(app.orders.ok() && app.feed.ok()) && net::now_ns() - t0 < 2 * kSecond) app.pump(net::now_ns());
  CHECK(app.orders.ok());
  CHECK(app.feed.ok());
  const uint32_t up = static_cast<uint32_t>(t.L.keys[0]), right = static_cast<uint32_t>(t.L.keys[3]);
  CHECK(app.on_key(KeyEvent{Key::Up, 0}));
  CHECK(app.on_key(KeyEvent{Key::Char, 'd'}));
  CHECK(app.on_key(KeyEvent{Key::Char, 'd'}));
  CHECK_EQ(app.keys_sent, 3u);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.sell_qty(right) == 2 && t.st.lit(up); }));
  CHECK_EQ(t.st.sell_qty(up), 1);
  // the App's own feed must see the same thing, and its header must say so
  const uint64_t t1 = net::now_ns();
  while (!(app.st.sell_qty(right) == 2) && net::now_ns() - t1 < 2 * kSecond) app.pump(net::now_ns());
  CHECK_EQ(app.st.sell_qty(right), 2);
  // +/- step the knob from the value the feed reports, never from a local copy
  app.on_key(KeyEvent{Key::Char, '+'});
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->delay_us == 10; }));
  const uint64_t t2 = net::now_ns();
  while (!(app.st.stats() && app.st.stats()->delay_us == 10) && net::now_ns() - t2 < 2 * kSecond) app.pump(net::now_ns());
  app.on_key(KeyEvent{Key::Char, '+'});
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->delay_us == 20; }));
  const uint64_t t3 = net::now_ns();
  while (!(app.st.stats() && app.st.stats()->delay_us == 20) && net::now_ns() - t3 < 2 * kSecond) app.pump(net::now_ns());
  app.on_key(KeyEvent{Key::Char, '-'});
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->delay_us == 10; }));
  const uint64_t t4 = net::now_ns();
  while (!(app.st.stats() && app.st.stats()->delay_us == 10) && net::now_ns() - t4 < 2 * kSecond) app.pump(net::now_ns());
  app.on_key(KeyEvent{Key::Char, 'm'});
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->mutation == 1; }));
  control(*t.orders, proto::SetDelayUs, 0);
  control(*t.orders, proto::SetMutation, 0);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.stats() && t.st.stats()->mutation == 0 && t.st.stats()->delay_us == 0; }));
  CHECK(!app.on_key(KeyEvent{Key::Char, 'q'}));
  // a snapshot request on the App's feed connection yields a fresh snapshot for it
  const uint64_t snaps = app.st.snapshots_completed();
  app.on_key(KeyEvent{Key::Char, 'S'});
  const uint64_t t5 = net::now_ns();
  while (app.st.snapshots_completed() == snaps && net::now_ns() - t5 < 2 * kSecond) app.pump(net::now_ns());
  CHECK_EQ(app.st.snapshots_completed(), snaps + 1);
  // rendering never touches the terminal: it only builds lines
  auto lines = render(app, net::now_ns(), 80);
  CHECK(lines.size() > static_cast<size_t>(t.L.height + 6));
  bool has_board = false;
  for (const auto& l : lines) has_board |= l.find("██") != std::string::npos;
  CHECK(has_board);
}

void test_snapshot_no_stale(Live& t) {
  std::printf("snapshot after BookReset leaves no stale levels\n");
  auto before = t.st.levels();
  const size_t resting = t.st.resting_orders();
  const uint64_t trades = t.st.trades_total();
  // Inject a level the exchange does not hold: a stale entry that only a reset can remove.
  proto::LevelMsg stale;
  stale.book = 1;
  stale.side = proto::Sell;
  stale.price = t.L.P;
  stale.total = 5;
  stale.count = 1;
  t.st.apply(proto::Any::of(stale));
  CHECK(t.st.lit(1));
  CHECK_EQ(t.st.resting_orders(), resting + 1);
  const uint64_t snaps = t.st.snapshots_completed(), gaps = t.st.gaps();
  proto::ControlMsg c;
  c.op = proto::Snapshot;
  t.feed->send(c);
  t.feed->flush();
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.snapshots_completed() > snaps; }));
  CHECK(!t.st.lit(1));
  CHECK(t.st.levels() == before);
  CHECK_EQ(t.st.resting_orders(), resting);
  CHECK_EQ(t.st.trades_total(), trades);  // a snapshot carries levels, never trades
  CHECK(t.st.gaps() > gaps);  // every snapshot is announced by a Gap (other subscribers' snapshots reach us too)
}

void test_once(Live& t) {
  std::printf("viz --once renders a frame to a pipe\n");
  int status = 0;
  std::string cmd = t.viz_bin + " --feed " + t.feed_url + " --orders " + t.orders_url + " --layout " + t.layout_path + " --once";
  std::string out = run_capture(cmd, status);
  CHECK_EQ(status, 0);
  CHECK(out.find("MINSKY snake") != std::string::npos);
  CHECK(out.find("fps") != std::string::npos);
  CHECK(out.find("score 7") != std::string::npos);
  CHECK(out.find("fruit (1,2)") != std::string::npos);
  CHECK(out.find("head (15,0)") != std::string::npos);
  CHECK(out.find("tail (1,9)") != std::string::npos);
  CHECK(out.find("██") != std::string::npos);
  CHECK(out.find("◆") != std::string::npos);
  CHECK(out.find("latency 0 us") != std::string::npos);
  CHECK(out.find("mutation none") != std::string::npos);
  CHECK(out.find("orders resting in") != std::string::npos);
  CHECK(out.find("feed disconnected") == std::string::npos);
  size_t rows = 0;
  for (size_t p = out.find("│"); p != std::string::npos; p = out.find("│", p + 1)) ++rows;
  CHECK_EQ(rows, 2u * static_cast<size_t>(t.L.height));
  if (g_fail) std::printf("%s\n", out.c_str());

  std::string wcmd = t.viz_bin + " --feed " + t.feed_url + " --orders " + t.orders_url + " --layout " + t.layout_path + " --wall --once";
  std::string wout = run_capture(wcmd, status);
  CHECK_EQ(status, 0);
  CHECK(wout.find("MINSKY wall") != std::string::npos);
  CHECK(wout.find("generations/s") != std::string::npos);
  CHECK(wout.find("8 cells") != std::string::npos);

  std::string ncmd = t.viz_bin + " --feed " + t.feed_url + " --orders " + t.orders_url + " --once";  // no layout at all
  std::string nout = run_capture(ncmd, status);
  CHECK_EQ(status, 0);
  CHECK(nout.find("MINSKY wall") != std::string::npos);
  CHECK(nout.find("300 cells") != std::string::npos);
  CHECK(nout.find("█") != std::string::npos);

  std::string dcmd = t.viz_bin + " --feed tcp://127.0.0.1:1 --orders tcp://127.0.0.1:1 --layout " + t.layout_path + " --once";
  std::string dout = run_capture(dcmd, status);
  CHECK(status != 0);
  CHECK(dout.find("feed disconnected") != std::string::npos);
  CHECK(dout.find("orders disconnected") != std::string::npos);
}

void test_wall_cells(Live& t) {
  std::printf("wall cells and generation counter\n");
  for (uint32_t b : {50u, 52u, 57u}) sell_at_P(*t.orders, b, 1, t.L.P);
  const uint32_t gb = static_cast<uint32_t>(t.L.generation);
  for (int i = 0; i < 3; ++i) trade_once(*t.orders, gb, t.L.P);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.lit(57) && t.st.trades_in(gb) == 3; }));
  std::string pattern;
  for (long long b : t.L.cells) pattern += t.st.lit(b) ? '#' : '.';
  CHECK(pattern == "#.#....#");
  CHECK_EQ(t.st.fps(gb, net::now_ns()), 3.0);
}

void test_reconnect(Live& t) {
  std::printf("feed reconnect rebuilds the state from a fresh snapshot\n");
  Link l;
  l.url = t.feed_url;
  CHECK(l.maintain(net::now_ns()));
  FeedState st(t.L.P);
  CHECK(pump_until(*l.ch, st, [&] { return st.synced(); }));
  CHECK(st.levels() == t.st.levels());
  l.ch.reset();  // the connection drops
  CHECK(!l.ok());
  CHECK(!l.maintain(net::now_ns()));  // too soon
  l.next_try = 0;
  CHECK(l.maintain(net::now_ns()));
  CHECK_EQ(l.connects, 2u);
  sell_at_P(*t.orders, 61, 1, t.L.P);
  CHECK(pump_until(*l.ch, st, [&] { return st.snapshots_completed() == 2 && st.lit(61); }));
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.lit(61); }));
  CHECK(st.levels() == t.st.levels());
}

void test_reset(Live& t) {
  std::printf("ResetBooks clears everything\n");
  control(*t.orders, proto::ResetBooks);
  CHECK(pump_until(*t.feed, t.st, [&] { return t.st.resting_orders() == 0; }));
  CHECK(t.st.levels().empty());
  CHECK_EQ(t.st.resting_books(), 0u);
  for (long long b : t.L.pixels) CHECK(!t.st.lit(b));
  CHECK(!t.st.decode_bits(t.L.fruit_x).value_or(-1));
}

}  // namespace

int main(int argc, char** argv) {
  std::string orders = "tcp://127.0.0.1:7100", feed = "tcp://127.0.0.1:7102";
  std::string layout = "tests/viz_layout.txt", vizbin = "./build/viz";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--orders" && i + 1 < argc) orders = argv[++i];
    else if (a == "--feed" && i + 1 < argc) feed = argv[++i];
    else if (a == "--layout" && i + 1 < argc) layout = argv[++i];
    else if (a == "--viz" && i + 1 < argc) vizbin = argv[++i];
  }
  signal(SIGPIPE, SIG_IGN);
  test_sliding();
  test_mutation_names();
  test_keys();
  test_layout(layout);
  test_link_refused();
  try {
    Live t(orders, feed, layout, vizbin);
    test_connect(t);
    test_pixels(t);
    test_unlit(t);
    test_score(t);
    test_bits(t);
    test_frame_and_death(t);
    test_stats(t);
    test_matches_exchange(t);
    test_keyboard(t);
    test_snapshot_no_stale(t);
    test_wall_cells(t);
    test_once(t);
    test_reconnect(t);
    test_reset(t);
  } catch (const std::exception& e) {
    std::printf("  FAIL exception: %s\n", e.what());
    ++g_fail;
  }
  std::printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
