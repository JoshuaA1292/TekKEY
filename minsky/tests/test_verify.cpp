// Soundness of the bounded key's verifier (BOUNDED_KEYS.md §8.5 tests 1-5) plus determinism and
// performance. Every program here is compiled from MPL by the real compiler and certified or
// rejected by the real verifier; every check runs it on an exchange: an in-process Matcher always,
// and the live exchange over TCP with --wire URL (started with --accounts 255).
//
//   1. Fuzz never beats a certificate: random move sequences within the model (any print within
//      max_gap_ticks of the last, not only boundary prices; random pause / cancel), run against
//      certified programs; loss, fees, trades and exposure stay within the bound at every step.
//   2. Every counterexample replays: each rejection's path reproduces value_on_path to the cent,
//      and each certificate's worst_path reproduces max_loss_cents to the cent.
//   3. Compiled equals drawn: the same program and moves on the in-process engine and over the
//      wire give the same fills (node, price, qty, maker, fee), node states and positions.
//   4. The audit fails: each of the audit's zero-allowance stop-limits, written in MPL, is a static
//      error; forced through with allow 0, the verifier finds the gap path (the stop rests); with
//      allow = max_gap_ticks the same program is certified.
//   5. Gaps at the edge: the fuzz of test 1 with every print exactly max_gap_ticks away.
//   D. Determinism: 100 runs of verify give identical output except explored.ms.
//   P. Performance: 3 nodes < 50 ms; 12 nodes / 1 instrument < 1 s; 3 instruments < 10 s.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "env.hpp"
#include "json.hpp"
#include "matcher.hpp"
#include "policy.hpp"
#include "program_json.hpp"
#include "replayer.hpp"
#include "verifier.hpp"

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_fail;
    std::printf("  FAIL %s\n", what.c_str());
  }
}

std::string fixtures() {
  for (const char* d : {"../fixtures/", "fixtures/", "../../fixtures/"}) {
    FILE* f = std::fopen((std::string(d) + "instruments.json").c_str(), "r");
    if (f) {
      std::fclose(f);
      return d;
    }
  }
  throw std::runtime_error("fixtures/ not found");
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// An exchange to run on, with a scratch book allocator.
struct Exch {
  std::string name;
  std::unique_ptr<bk::WirePort> port;
  uint32_t next_book = 0, max_book = 0;
  uint32_t book() {
    const uint32_t b = bk::fresh_book(*port, bk::kReplayAccount, next_book, max_book);
    next_book = b + 1;
    return b;
  }
};

std::unique_ptr<Exch> inproc() {
  fb::Config c;
  c.books = 512;
  c.accounts = bk::kLiveAccounts;
  c.pool = 4096;
  auto e = std::make_unique<Exch>();
  e->name = "inproc";
  e->port = std::make_unique<bk::WirePort>(std::make_unique<ex::InProcChannel>(c));
  e->next_book = 1;
  e->max_book = 512;
  return e;
}

std::unique_ptr<Exch> wire(const std::string& url) {
  auto e = std::make_unique<Exch>();
  e->name = "wire";
  e->port = std::make_unique<bk::WirePort>(net::connect(url));
  e->next_book = 2048;
  e->max_book = 4096;
  return e;
}

bk::Instruments eth() {
  return bk::load_instruments(json::parse(R"({"ETH": {"tick_cents": 100}, "BTC": {"tick_cents": 100}, "SOL": {"tick_cents": 10}})"));
}

json::Value envelope_json(int64_t max_loss, int64_t gap, bool live_stop, std::vector<std::string> ins = {"ETH"}) {
  json::Value is = json::Value::array();
  for (const auto& i : ins) is.push(i);
  return json::obj({{"contract_version", 1},
                    {"account", 1},
                    {"max_loss_cents", max_loss},
                    {"max_fees_cents", 100000},
                    {"max_trades", 60},
                    {"max_exposure_lots", 20},
                    {"instruments", is},
                    {"require_live_stop", live_stop},
                    {"env", json::obj({{"max_gap_ticks", gap},
                                       {"fill", "full_at_print"},
                                       {"fee_bps", json::obj({{"maker", 1.0}, {"taker", 3.5}})},
                                       {"trigger_ref", "last"}})}});
}

struct Case {
  std::string mpl;
  int64_t mark;
  bk::Envelope env;
};

// A random bracket in MPL: entry, a stop (stop-limit with a random allowance, or stop-market),
// usually a take-profit in its OCO group, sometimes a scale-in and a second take.
Case random_case(std::mt19937_64& rng) {
  auto r = [&](int64_t lo, int64_t hi) { return lo + static_cast<int64_t>(rng() % static_cast<uint64_t>(hi - lo + 1)); };
  const bool buy = rng() % 2;
  const int64_t qty = r(1, 3);
  const int64_t mark = r(2900, 3100);
  const int64_t entry = buy ? mark - r(0, 15) : mark + r(0, 15);
  const int64_t a = r(3, 28), c = r(5, 50);
  const std::string cmp = buy ? "<=" : ">=", away = buy ? "-" : "+", toward = buy ? "+" : "-";
  const bool tp = rng() % 5 != 0, tp2 = tp && qty >= 2 && rng() % 3 == 0, scale = rng() % 3 == 0;
  std::string m = "instrument ETH\nentry " + std::string(buy ? "buy" : "sell") + " " + std::to_string(qty) + " at " +
                  std::to_string(entry) + "\n";
  if (scale) m += "sc: scale " + std::string(buy ? "buy" : "sell") + " 1 at entry " + away + " " + std::to_string(r(1, a - 1)) + "\n";
  std::string oco = tp ? (tp2 ? " oco tp, tp2" : " oco tp") : "";
  if (rng() % 5 == 0)
    m += "sl: exit all market when price " + cmp + " entry " + away + " " + std::to_string(a) + oco + "\n";
  else
    m += "sl: exit all at entry " + away + " " + std::to_string(a) + " allow " + std::to_string(r(1, 40)) +
         " when price " + cmp + " entry " + away + " " + std::to_string(a) + oco + "\n";
  if (tp) m += "tp: take all at entry " + toward + " " + std::to_string(c) + " oco sl\n";
  if (tp2) m += "tp2: take half at entry " + toward + " " + std::to_string(c / 2 + 1) + " oco sl\n";
  Case k;
  k.mpl = m;
  k.mark = mark;
  k.env = bk::load_envelope(envelope_json(r(500, 9000), r(3, 30), rng() % 2));
  return k;
}

struct Bound {
  int64_t loss, fees, trades, exposure;
};
Bound bound_of(const json::Value& cert) {
  const json::Value& b = cert.at("bound");
  return {b.at("max_loss_cents").as_int(), b.at("max_fees_cents").as_int(), b.at("halts_within_trades").as_int(),
          b.at("max_exposure_lots").as_int()};
}

bk::ReplayOptions ropts(const bk::Envelope& e, uint32_t book) {
  bk::ReplayOptions o;
  o.books = {book};
  o.fees = e.fees;
  o.mark_mode = e.trigger_ref == "mark";
  return o;
}

std::vector<int64_t> ticks_of(const bk::Program& p, const bk::Instruments& ins) {
  std::vector<int64_t> t;
  for (const auto& n : p.instruments) t.push_back(ins.at(n).tick_cents);
  return t;
}

// Replays a verifier path on an exchange and returns the loss at its end.
int64_t replay_loss(Exch& x, const bk::Program& p, const bk::Instruments& ins, const bk::Envelope& e,
                    const json::Value& path) {
  bk::Replayer r(*x.port, p, ins, ropts(e, x.book()));
  r.run(bk::path_moves(path, p));
  const int64_t loss = r.ledger().loss(r.marks(), ticks_of(p, ins));
  r.cleanup();
  return loss;
}

// One fuzz sequence. Returns false (and says why) if any step leaves the bound.
struct FuzzStats {
  int64_t sequences = 0, steps = 0, worst_loss = INT64_MIN, fills = 0, filled_seqs = 0;
  int64_t tight = 0;  // sequences that reached the certified max_loss exactly
};
bool fuzz_one(Exch& x, std::vector<uint32_t> books, const bk::Program& p, const bk::Instruments& ins,
              const bk::Envelope& e, const Bound& b, std::vector<int64_t> marks, std::mt19937_64& rng, bool edge,
              FuzzStats& st, std::string& why, std::vector<bk::PathMove>* record = nullptr,
              std::vector<bk::ReplayFill>* fills = nullptr) {
  bk::ReplayOptions o = ropts(e, books[0]);
  o.books = books;
  bk::Replayer r(*x.port, p, ins, o);
  r.open(marks);
  r.deploy();
  std::vector<bk::PathMove> moves;
  for (size_t i = 0; i < books.size(); ++i) {
    moves.push_back({bk::PathMove::Print, static_cast<int>(i), marks[i]});
    r.apply(moves.back());
  }
  bool paused = false, cancelled = false;
  const int64_t G = e.max_gap_ticks;
  const auto ticks = ticks_of(p, ins);
  bool ok = true;
  for (int step = 0; step < 14 && ok; ++step) {
    bk::PathMove m;
    const int u = static_cast<int>(rng() % 100);
    if (u < 4 && !paused && !cancelled) {
      m.type = bk::PathMove::Pause;
      paused = true;
    } else if (u < 7 && !cancelled) {
      m.type = bk::PathMove::Cancel;
      cancelled = true;
    } else {
      m.inst = static_cast<int>(rng() % books.size());
      const int64_t last = x.port->last(books[m.inst]).value_or(marks[m.inst]);
      int64_t d = edge ? G : 1 + static_cast<int64_t>(rng() % static_cast<uint64_t>(G));
      if (rng() % 2) d = -d;
      m.type = bk::PathMove::Gap;
      m.to = std::max<int64_t>(1, last + d);
    }
    r.apply(m);
    moves.push_back(m);
    const bk::Ledger led = r.ledger();
    const int64_t loss = led.loss(r.marks(), ticks);
    ++st.steps;
    st.worst_loss = std::max(st.worst_loss, loss);
    if (loss == b.loss && b.loss > 0) ++st.tight;
    if (loss > b.loss || led.fees_cents > b.fees || led.trades > b.trades || led.max_exposure > b.exposure) {
      why = "loss " + std::to_string(loss) + "/" + std::to_string(b.loss) + " fees " + std::to_string(led.fees_cents) +
            "/" + std::to_string(b.fees) + " trades " + std::to_string(led.trades) + "/" + std::to_string(b.trades) +
            " exposure " + std::to_string(led.max_exposure) + "/" + std::to_string(b.exposure);
      ok = false;
    }
  }
  if (record) *record = moves;
  if (fills) *fills = r.fills();
  st.fills += static_cast<int64_t>(r.fills().size());
  st.filled_seqs += !r.fills().empty();
  r.cleanup();
  ++st.sequences;
  return ok;
}

bk::VerifyResult run_verify(const bk::Program& p, const bk::Envelope& e, const bk::Instruments& ins,
                            std::vector<int64_t> marks, int64_t timeout_ms = 20000) {
  bk::VerifyOptions o;
  o.marks = std::move(marks);
  o.timeout_ms = timeout_ms;
  return bk::verify(p, e, ins, o);
}

std::string strip_ms(json::Value v) {
  if (v.has("explored")) {
    json::Value x = v.at("explored");
    x.set("ms", 0);
    v.set("explored", x);
  }
  return json::compact(v);
}

}  // namespace

int main(int argc, char** argv) {
  std::string url;
  int64_t fuzz_total = 10000;
  int programs = 80;
  uint64_t seed = 20261016;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--wire" && i + 1 < argc) url = argv[++i];
    else if (a == "--fuzz" && i + 1 < argc) fuzz_total = std::stoll(argv[++i]);
    else if (a == "--programs" && i + 1 < argc) programs = std::stoi(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
    else if (a == "--quick") {
      fuzz_total = 1000;
      programs = 30;
    } else {
      std::printf("test_verify [--wire tcp://127.0.0.1:PORT] [--fuzz N] [--programs N] [--seed S] [--quick]\n");
      return 2;
    }
  }
  const std::string fx = fixtures();
  const bk::Instruments ins = eth();
  std::vector<std::unique_ptr<Exch>> exchanges;
  exchanges.push_back(inproc());
  if (!url.empty()) exchanges.push_back(wire(url));
  else std::printf("(no --wire: the over-TCP halves of tests 1, 2, 3, 5 are skipped)\n");

  // ---------------------------------------------------------------- A6: snapshots and canonical hashes
  {
    std::printf("A6: Core snapshots are independent; book hashes ignore order ids\n");
    struct Count : fb::Sink {
      int fills = 0;
      void fill(const fb::Order&, bool, fb::Price, fb::Qty) override { ++fills; }
      void cancelled(const fb::Order&) override {}
      void trade(uint32_t, fb::Price, fb::Qty, uint8_t, uint8_t) override {}
      void level(uint32_t, uint8_t, fb::Price, fb::Qty, uint32_t) override {}
      void reset(uint32_t) override {}
    } sa, sb;
    fb::Config c;
    c.books = 2;
    c.accounts = 3;
    c.pool = 8;
    auto order = [](uint8_t acct, uint8_t side, int64_t px, int64_t q) {
      proto::NewOrderMsg m;
      m.account = acct;
      m.side = side;
      m.price = px;
      m.qty = q;
      return m;
    };
    // the same book reached through different id histories (slots and generations differ)
    fb::Core a(c, &sa), b(c, &sb);
    for (int i = 0; i < 5; ++i) b.cancel(b.submit(order(2, proto::Buy, 900 + i, 1), 0).id);
    for (fb::Core* x : {&a, &b}) {
      x->submit(order(1, proto::Buy, 1000, 2), 0);
      x->submit(order(1, proto::Sell, 1010, 3), 0);
      x->submit(order(2, proto::Sell, 1000, 1), 0);  // trades 1 lot
    }
    check(a.book_hash(0) == b.book_hash(0) && a.hash() == b.hash(), "same book via different order ids: same hashes");
    // a snapshot reports to its own sink and never touches the original
    fb::Core snap(a, &sb);
    const uint64_t before = a.hash();
    const int fa = sa.fills, fbn = sb.fills;
    snap.submit(order(2, proto::Buy, 1010, 3), 0);
    check(a.hash() == before && sa.fills == fa && sb.fills == fbn + 2 && snap.hash() != before,
          "a snapshot diverges on its own sink; the original is untouched");
  }

  // ---------------------------------------------------------------- the fixtures themselves
  {
    std::printf("fixtures: ex_v1 / ex_v2 through policyc + verify + replay\n");
    const bk::Instruments fins = bk::load_instruments(bk::read_json(fx + "instruments.json"));
    const bk::Envelope env = bk::load_envelope(bk::read_json(fx + "envelope_40.json"));
    bk::CompileOptions co;
    co.max_gap_ticks = env.max_gap_ticks;
    for (int v = 1; v <= 2; ++v) {
      const bk::Program p = bk::compile(bk::read_file(fx + "ex_v" + std::to_string(v) + ".mpl"), fins, co);
      json::Value want = bk::read_json(fx + "ex_v" + std::to_string(v) + ".compiled.json");
      json::Value got = bk::to_json(p);
      want.erase("source_hash");
      got.erase("source_hash");
      check(want == got, "policyc ex_v" + std::to_string(v) + " == fixture");
      const auto r = run_verify(p, env, fins, {3010});
      const json::Value fixture = bk::read_json(fx + "ex_v" + std::to_string(v) + ".result.json");
      check(strip_ms(r.out) == strip_ms(fixture), "verify ex_v" + std::to_string(v) + " == fixture");
      for (auto& x : exchanges) {
        const json::Value& path = v == 1 ? r.out.at("counterexample") : r.out.at("worst_path");
        const int64_t loss = replay_loss(*x, p, fins, env, path);
        check(loss == bk::path_final_loss(path), x->name + ": ex_v" + std::to_string(v) + " path replays to " +
                                                     std::to_string(loss) + " (verifier " +
                                                     std::to_string(bk::path_final_loss(path)) + ")");
      }
    }
    try {
      bk::compile(bk::read_file(fx + "ex_noallow.mpl"), fins, co);
      check(false, "ex_noallow compiles");
    } catch (const bk::StaticError& e) {
      const json::Value want = bk::read_json(fx + "ex_noallow.result.json");
      check(e.to_json() == want, "ex_noallow StaticError == fixture (line " + std::to_string(e.line) + ")");
    }
  }

  // ---------------------------------------------------------------- random programs: tests 1, 2, 5
  std::mt19937_64 rng(seed);
  struct Certified {
    std::string src;
    bk::Program p;
    bk::Envelope e;
    int64_t mark;
    Bound b;
  };
  std::vector<Certified> certified;
  int rejected = 0, static_errors = 0, worst_replayed = 0, cx_replayed = 0;
  {
    std::printf("test 2: every counterexample and every worst_path replays to the cent (%d random programs)\n", programs);
    for (int i = 0; i < programs; ++i) {
      Case k = random_case(rng);
      bk::CompileOptions co;
      co.max_gap_ticks = k.env.max_gap_ticks;
      bk::Program p;
      try {
        p = bk::compile(k.mpl, ins, co);
      } catch (const bk::StaticError& e) {
        ++static_errors;
        continue;
      }
      const auto r = run_verify(p, k.env, ins, {k.mark});
      check(r.exit_code == 0 || r.exit_code == 3, "verify verdict for case " + std::to_string(i));
      const bool cert = r.exit_code == 0;
      const json::Value& path = cert ? r.out.at("worst_path") : r.out.at("counterexample");
      const int64_t expect = bk::path_final_loss(path);
      if (cert) {
        check(expect == std::max<int64_t>(0, r.out.at("bound").at("max_loss_cents").as_int()) ||
                  r.out.at("bound").at("max_loss_cents").as_int() == 0,
              "case " + std::to_string(i) + ": worst_path ends on max_loss");
        certified.push_back({k.mpl, p, k.env, k.mark, bound_of(r.out)});
        ++worst_replayed;
      } else {
        ++rejected;
        ++cx_replayed;
        if (r.out.at("violates").as_string() == "max_loss_cents")
          check(expect == r.out.at("value_on_path").as_int() && expect > r.out.at("limit").as_int(),
                "case " + std::to_string(i) + ": counterexample ends on value_on_path");
      }
      for (auto& x : exchanges) {
        const int64_t loss = replay_loss(*x, p, ins, k.env, path);
        if (loss != expect) {
          std::printf("    %s case %d: replay %lld, verifier %lld\n%s", x->name.c_str(), i, (long long)loss,
                      (long long)expect, k.mpl.c_str());
        }
        check(loss == expect, x->name + ": case " + std::to_string(i) + " path replays");
      }
    }
    std::printf("  %zu certified, %d rejected, %d static errors; %d worst paths and %d counterexamples replayed on %zu exchange(s)\n",
                certified.size(), rejected, static_errors, worst_replayed, cx_replayed, exchanges.size());
    check(certified.size() >= 5 && rejected >= 5, "the random mix has both verdicts");
  }
  for (int edge = 0; edge <= 1; ++edge) {
    const int64_t total = edge ? std::max<int64_t>(fuzz_total / 4, 500) : fuzz_total;
    std::printf("test %d: %lld random sequences%s never beat a certificate\n", edge ? 5 : 1, (long long)total,
                edge ? " with every gap exactly max_gap_ticks" : "");
    for (auto& x : exchanges) {
      FuzzStats st;
      int64_t bad = 0;
      const auto t0 = std::chrono::steady_clock::now();
      size_t ci = 0;
      std::vector<uint32_t> books(certified.size(), 0);
      while (st.sequences < total && !certified.empty()) {
        const size_t i = ci++ % certified.size();
        if (!books[i]) books[i] = x->book();
        std::string why;
        std::vector<bk::PathMove> rec;
        std::vector<bk::ReplayFill> fl;
        if (!fuzz_one(*x, {books[i]}, certified[i].p, ins, certified[i].e, certified[i].b, {certified[i].mark}, rng, edge,
                      st, why, &rec, &fl)) {
          if (++bad <= 5) {
            std::printf("    %s: program %zu: %s\n      mark %lld gap %lld\n%s      moves:", x->name.c_str(), i, why.c_str(),
                        (long long)certified[i].mark, (long long)certified[i].e.max_gap_ticks, certified[i].src.c_str());
            for (const auto& m : rec)
              std::printf(" %s", m.type == bk::PathMove::Pause ? "PAUSE" : m.type == bk::PathMove::Cancel ? "CANCEL" : std::to_string(m.to).c_str());
            std::printf("\n      fills:");
            for (const auto& f : fl) std::printf(" [slot %d %lldx%lld %s fee %lld]", f.slot, (long long)f.q, (long long)f.px, f.maker ? "maker" : "taker", (long long)f.fee);
            std::printf("\n");
          }
        }
      }
      check(bad == 0, x->name + ": fuzz within the bound");
      std::printf("  %s: %lld sequences, %lld steps, %lld program fills (%lld sequences traded), %lld steps at\n"
                  "    exactly the certified max_loss, %lld beat the bound, %.0f ms\n",
                  x->name.c_str(), (long long)st.sequences, (long long)st.steps, (long long)st.fills,
                  (long long)st.filled_seqs, (long long)st.tight, (long long)bad, ms_since(t0));
    }
  }

  // ---------------------------------------------------------------- test 3: compiled equals drawn
  if (exchanges.size() == 2 && !certified.empty()) {
    std::printf("test 3: compiled programs on the in-process engine and over the wire give the same fills\n");
    int same = 0, n = 0;
    for (int t = 0; t < 200; ++t) {
      const Certified& c = certified[static_cast<size_t>(t) % certified.size()];
      const uint64_t s = rng();
      std::vector<bk::ReplayFill> fa, fb_;
      std::vector<bk::PathMove> ma, mb;
      FuzzStats st;
      std::string why;
      std::mt19937_64 ra(s), rb(s);
      const uint32_t ba = exchanges[0]->book(), bb = exchanges[1]->book();
      fuzz_one(*exchanges[0], {ba}, c.p, ins, c.e, c.b, {c.mark}, ra, false, st, why, &ma, &fa);
      fuzz_one(*exchanges[1], {bb}, c.p, ins, c.e, c.b, {c.mark}, rb, false, st, why, &mb, &fb_);
      bool eq = fa.size() == fb_.size() && ma.size() == mb.size();
      for (size_t i = 0; eq && i < fa.size(); ++i)
        eq = fa[i].slot == fb_[i].slot && fa[i].px == fb_[i].px && fa[i].q == fb_[i].q && fa[i].maker == fb_[i].maker &&
             fa[i].fee == fb_[i].fee;
      for (size_t i = 0; eq && i < ma.size(); ++i)
        eq = ma[i].type == mb[i].type && ma[i].to == mb[i].to && ma[i].inst == mb[i].inst;
      same += eq;
      ++n;
    }
    check(same == n, "in-process and wire agree on every run");
    std::printf("  %d / %d runs identical (fills: node, price, qty, maker, fee)\n", same, n);
  }

  // ---------------------------------------------------------------- test 4: the audit fails
  {
    const json::Value audit = bk::read_json(fx + "audit_stoplimits.json");
    int n = 0, zero = 0, static_rejected = 0, gap_found = 0, repaired = 0;
    const bk::Instruments xins = bk::load_instruments(json::parse(R"({"X": {"tick_cents": 1}})"));
    for (const auto& o : audit.at("orders").as_array()) {
      ++n;
      if (!o.at("zero_allowance").as_bool()) continue;
      ++zero;
      const double px = std::stod(o.at("trigger").as_string());
      const double scale = std::pow(10.0, 4 - std::floor(std::log10(px)));
      const int64_t T = std::llround(px * scale);
      const bool sell = o.at("side").as_string() == "sell";  // a sell stop protects a long
      const int64_t d = std::max<int64_t>(5, T / 200), G = std::max<int64_t>(2, T / 1000);
      const int64_t E = sell ? T + d : T - d;
      // the audit's stop on a 1-lot position, in a bracket with a take-profit (so the only thing that
      // differs between the forced and the repaired program is the stop's allowance)
      auto mpl = [&](int64_t allow) {
        return "instrument X\nentry " + std::string(sell ? "buy" : "sell") + " 1 at " + std::to_string(E) +
               "\nsl: exit all at " + std::to_string(T) + " allow " + std::to_string(allow) + " when price " +
               (sell ? "<= " : ">= ") + std::to_string(T) + " oco tp\ntp: take all at " +
               std::to_string(sell ? E + d : E - d) + " oco sl\n";
      };
      const bk::Envelope env = bk::load_envelope(envelope_json(d + 2 * G + 100, G, true, {"X"}));
      bk::CompileOptions co;
      co.max_gap_ticks = G;
      try {
        bk::compile(mpl(0), xins, co);
      } catch (const bk::StaticError& e) {
        static_rejected += e.line == 3 && std::string(e.what()) == "exit requires allow >= 1 tick";
      }
      co.unsafe_allow_zero = true;
      const bk::Program forced = bk::compile(mpl(0), xins, co);
      const auto r = run_verify(forced, env, xins, {E});
      bool rests = false;
      if (r.exit_code == 3)
        for (const auto& ev : r.out.at("counterexample").as_array())
          rests |= ev.str_or("event", "") == "rests" && ev.str_or("node", "") == "sl";
      gap_found += r.exit_code == 3 && rests;
      co.unsafe_allow_zero = false;
      // the repair: an allowance of max_gap_ticks
      const auto fixed = run_verify(bk::compile(mpl(G), xins, co), env, xins, {E});
      repaired += fixed.exit_code == 0;
    }
    std::printf("test 4: the audit's %d stop-limits, %d with zero allowance (BOUNDED_KEYS.md says 273; the audit data has %d)\n",
                n, zero, zero);
    std::printf("  static check rejects %d / %d; forced through with allow 0, the verifier finds the gap path for %d / %d;\n"
                "  repaired with allow = max_gap_ticks, %d / %d are certified\n",
                static_rejected, zero, gap_found, zero, repaired, zero);
    check(static_rejected == zero && gap_found == zero && repaired == zero && zero > 0, "the audit fails, and the fix certifies");
  }

  // ---------------------------------------------------------------- determinism
  {
    std::printf("determinism: 100 runs of verify, identical output except explored.ms\n");
    const bk::Instruments fins = bk::load_instruments(bk::read_json(fx + "instruments.json"));
    const bk::Envelope env = bk::load_envelope(bk::read_json(fx + "envelope_40.json"));
    bool same = true;
    for (int v = 1; v <= 2; ++v) {
      const bk::Program p = bk::load_program(bk::read_json(fx + "ex_v" + std::to_string(v) + ".compiled.json"), fins);
      const std::string first = strip_ms(run_verify(p, env, fins, {3010}).out);
      for (int i = 0; i < 100; ++i) same &= strip_ms(run_verify(p, env, fins, {3010}).out) == first;
    }
    for (size_t c = 0; c < std::min<size_t>(5, certified.size()); ++c) {
      const std::string first = strip_ms(run_verify(certified[c].p, certified[c].e, ins, {certified[c].mark}).out);
      for (int i = 0; i < 20; ++i)
        same &= strip_ms(run_verify(certified[c].p, certified[c].e, ins, {certified[c].mark}).out) == first;
    }
    check(same, "verify is deterministic");
  }

  // ---------------------------------------------------------------- performance
  {
    std::printf("performance\n");
    const bk::Instruments fins = bk::load_instruments(bk::read_json(fx + "instruments.json"));
    const bk::Envelope env = bk::load_envelope(bk::read_json(fx + "envelope_40.json"));
    const bk::Program v2 = bk::load_program(bk::read_json(fx + "ex_v2.compiled.json"), fins);
    auto t0 = std::chrono::steady_clock::now();
    run_verify(v2, env, fins, {3010});
    const double ms3 = ms_since(t0);
    std::printf("  3 nodes (ex_v2): %.1f ms\n", ms3);
    check(ms3 < 50, "3 nodes < 50 ms");

    // a 12-node bracket: six scaled take-profits in one OCO group with the stop (reduce mode: each
    // take shrinks the stop, the stop cancels the takes), two early partial exits, a market
    // backstop and a runner
    const std::string twelve =
        "instrument ETH\n"
        "entry buy 6 at 3000\n"
        "sl: exit all at 2985 allow 25 when price <= 2985 oco tp1, tp2, tp3, tp4, tp5, tp6\n"
        "tp1: take 1 at 3010 oco sl\n"
        "tp2: take 1 at 3015 oco sl\n"
        "tp3: take 1 at 3020 oco sl\n"
        "tp4: take 1 at 3025 oco sl\n"
        "tp5: take 1 at 3030 oco sl\n"
        "tp6: take 1 at 3040 oco sl\n"
        "xe: exit 2 at 2995 allow 25 when price <= 2995\n"
        "xf: exit 1 at 2990 allow 25 when price <= 2990\n"
        "bk: exit all market when price <= 2970\n"
        "run: take all at 3080\n";
    const bk::Envelope big = bk::load_envelope(envelope_json(1000000, 25, true));
    const bk::Program p12 = bk::compile(twelve, ins, {});
    t0 = std::chrono::steady_clock::now();
    const auto r12 = run_verify(p12, big, ins, {3010}, 60000);
    const double ms12 = ms_since(t0);
    std::printf("  %zu nodes, 1 instrument: %s, %lld states, %.0f ms\n", p12.nodes.size(),
                r12.out.at("status").as_string().c_str(), (long long)r12.out.at("explored").at("states").as_int(), ms12);
    check(p12.nodes.size() == 12 && r12.exit_code == 0 && ms12 < 1000, "12 nodes, 1 instrument: certified < 1 s");

    // three legs, each bracketed: the hedges' stops are stop-markets, because a hedge is released by
    // the ETH entry while its own book may already be past any fixed trigger (a stop-limit there
    // can be jumped; the verifier rejects that variant)
    const std::string three =
        "instrument ETH, BTC, SOL\n"
        "entry buy 1 at 3000\n"
        "sl: exit all at 2985 allow 25 when price <= 2985 oco tp\n"
        "tp: take all at 3030 oco sl\n"
        "hb: hedge BTC sell 1 at 6000\n"
        "hbs: exit all market when price >= 6020 after hb oco hbt\n"
        "hbt: take all at 5960 after hb oco hbs\n"
        "hs: hedge SOL sell 2 at 1500\n"
        "hss: exit all market when price >= 1510 after hs oco hst\n"
        "hst: take all at 1480 after hs oco hss\n";
    // A hedge released by the ETH entry sells at wherever its own book is, and its stop-market
    // buys back a tick above: fills at a market price, so their fees grow with that price. Under a
    // $1,000 fee cap the verifier must reject it; with the cap lifted it certifies, with a fee
    // bound that covers the whole price domain.
    const bk::Program p3 = bk::compile(three, ins, {});
    bk::VerifyOptions o3;
    o3.marks = {3010, 6010, 1505};
    o3.timeout_ms = 120000;
    const bk::Envelope env3cap = bk::load_envelope(envelope_json(1000000, 25, false, {"ETH", "BTC", "SOL"}));
    t0 = std::chrono::steady_clock::now();
    const auto r3c = bk::verify(p3, env3cap, ins, o3);
    const double ms3c = ms_since(t0);
    std::printf("  3 instruments, %zu nodes, $1,000 fee cap: %s (%s), %.0f ms\n", p3.nodes.size(),
                r3c.out.at("status").as_string().c_str(), r3c.out.str_or("violates", "-").c_str(), ms3c);
    check(r3c.exit_code == 3 && r3c.out.at("violates").as_string() == "max_fees_cents" && ms3c < 10000,
          "3 instruments: the hedge's unbounded fee exposure is found < 10 s");
    for (auto& x : exchanges) {
      bk::ReplayOptions o = ropts(env3cap, 0);
      o.books = {x->book(), x->book(), x->book()};
      bk::Replayer rr(*x->port, p3, ins, o);
      rr.run(bk::path_moves(r3c.out.at("counterexample"), p3));
      const bk::Ledger led = rr.ledger();
      rr.cleanup();
      check(led.fees_cents == r3c.out.at("value_on_path").as_int(),
            x->name + ": the 3-instrument counterexample replays to its fees (" + std::to_string(led.fees_cents) + ")");
    }
    json::Value uncapped = envelope_json(1000000000000LL, 25, false, {"ETH", "BTC", "SOL"});
    uncapped.set("max_fees_cents", 1000000000000LL);
    const bk::Envelope env3 = bk::load_envelope(uncapped);
    t0 = std::chrono::steady_clock::now();
    const auto r3 = bk::verify(p3, env3, ins, o3);
    const double ms3i = ms_since(t0);
    std::printf("  3 instruments, %zu nodes, no fee cap: %s, %lld states, %.0f ms (every book can walk to the top of\n"
                "    the price domain; the BUILD_A target of 10 s is for this worst case)\n",
                p3.nodes.size(), r3.out.at("status").as_string().c_str(),
                (long long)(r3.out.has("explored") ? r3.out.at("explored").at("states").as_int() : 0), ms3i);
    check(r3.exit_code == 0, "3 instruments, no fee cap: certified");

    // test 1 for three instruments: prints on random books, random pause / cancel
    if (r3.exit_code == 0) {
      const Bound b3 = bound_of(r3.out);
      const int64_t total = std::max<int64_t>(fuzz_total / 5, 400);
      std::printf("test 1, three instruments: %lld random sequences across ETH / BTC / SOL\n", (long long)total);
      std::printf("  worst path replays: ");
      for (auto& x : exchanges) {
        bk::ReplayOptions o = ropts(env3, 0);
        o.books = {x->book(), x->book(), x->book()};
        bk::Replayer rr(*x->port, p3, ins, o);
        rr.run(bk::path_moves(r3.out.at("worst_path"), p3));
        const int64_t loss = rr.ledger().loss(rr.marks(), ticks_of(p3, ins));
        rr.cleanup();
        std::printf("%s %lld (verifier %lld)  ", x->name.c_str(), (long long)loss,
                    (long long)bk::path_final_loss(r3.out.at("worst_path")));
        check(loss == bk::path_final_loss(r3.out.at("worst_path")), x->name + ": 3-instrument worst path replays");
      }
      std::printf("\n");
      for (auto& x : exchanges) {
        FuzzStats st;
        int64_t bad = 0;
        const std::vector<uint32_t> bs{x->book(), x->book(), x->book()};
        const auto tf = std::chrono::steady_clock::now();
        while (st.sequences < total) {
          std::string why;
          std::vector<bk::PathMove> rec;
          std::vector<bk::ReplayFill> fl;
          if (!fuzz_one(*x, bs, p3, ins, env3, b3, o3.marks, rng, rng() % 4 == 0, st, why, &rec, &fl) && ++bad <= 3) {
            std::printf("    %s: %s\n      moves:", x->name.c_str(), why.c_str());
            for (const auto& m : rec)
              std::printf(" %s", m.type == bk::PathMove::Pause    ? "PAUSE"
                                 : m.type == bk::PathMove::Cancel ? "CANCEL"
                                                                  : (p3.instruments[m.inst] + "@" + std::to_string(m.to)).c_str());
            std::printf("\n      fills:");
            for (const auto& f : fl)
              std::printf(" [%s %lldx%lld %s fee %lld]", f.slot < (int)p3.nodes.size() ? p3.nodes[f.slot].id.c_str() : "flatten",
                          (long long)f.q, (long long)f.px, f.maker ? "maker" : "taker", (long long)f.fee);
            std::printf("\n");
          }
        }
        check(bad == 0, x->name + ": 3-instrument fuzz within the bound");
        std::printf("  %s: %lld sequences, %lld program fills, %lld beat the bound, %.0f ms\n", x->name.c_str(),
                    (long long)st.sequences, (long long)st.fills, (long long)bad, ms_since(tf));
      }
    }
  }

  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
