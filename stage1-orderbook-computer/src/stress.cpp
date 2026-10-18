// Stress round: how little does the order-book computer need, and how far does it go?
//   S1  minimal primitives: only "on fill at price X -> submit"; no cancel, no accept, no iceberg
//   S2  stop-branch: the exchange's stop triggers do the branching; router has no price test
//   S3  one security: a single book (ask side + bid side = two counters) is Turing complete
//   S4  bounded books: a space-bounded Turing machine on n books holding <= 2 lots each
//       (the construction behind a corrected Theorem 6.3.1: PSPACE-complete in #securities)
//   S5  the price of computation: trades and round trips per instruction
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "engine.hpp"
#include "minsky.hpp"

using namespace mm;
using ob::Act;
using ob::On;
using ob::Price;
using ob::Side;
using ob::STP;

namespace {

int SCALE = 1;
bool BIG = true;  // --quick turns off the BB(5) run

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
std::string regs_str(const Regs& r) {
  std::string s = "[";
  for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + std::to_string(r[i]);
  return s + "]";
}
void header(const char* id, const char* title) { std::printf("\n=== %s  %s\n", id, title); }

MProg random_prog(std::mt19937_64& g, int& nregs, int fixed_regs = 0) {
  std::uniform_int_distribution<int> len(1, 12), nr(1, 4), pct(0, 99);
  int n = len(g);
  nregs = fixed_regs ? fixed_regs : nr(g);
  std::uniform_int_distribution<int> tgt(0, n - 1), reg(0, nregs - 1);
  MProg p(n);
  for (auto& in : p) {
    int x = pct(g);
    in.op = x < 10 ? Instr::HALT : x < 50 ? Instr::INC : Instr::DEC;
    in.r = reg(g);
    in.k = tgt(g);
    in.l = tgt(g);
  }
  return p;
}
Regs random_regs(std::mt19937_64& g, int nregs) {
  std::uniform_int_distribution<int> v(0, 8);
  Regs r(nregs);
  for (auto& x : r) x = v(g);
  return r;
}
bool same(const Run& a, const Run& b) {
  return a.status == b.status && a.steps == b.steps && a.regs == b.regs &&
         a.trace_hash == b.trace_hash;
}

struct Variant {
  const char* name;
  CompileOpts o;
  STP stp;
};
CompileOpts opts_minimal() {
  CompileOpts o;
  o.persistent_backstop = o.inc_clock = o.two_accounts = true;
  return o;
}
CompileOpts opts_stop() {
  CompileOpts o;
  o.stop_branch = true;
  return o;
}

// What primitives does a compiled program actually use?
struct Census {
  int fill_price = 0, fill_any = 0, accept = 0, submit = 0, cancel = 0, reduce = 0, halt = 0;
  int stops = 0, limits = 0, iocs = 0;
};
Census census(const Compiled& c) {
  Census s;
  auto acts = [&](const std::vector<ob::Action>& v) {
    for (auto& a : v) {
      if (a.kind == Act::Submit) ++s.submit;
      if (a.kind == Act::Cancel) ++s.cancel;
      if (a.kind == Act::Reduce) ++s.reduce;
      if (a.kind == Act::Halt) ++s.halt;
    }
  };
  for (auto& L : c.prog.links) {
    if (L.on == On::Accept) ++s.accept;
    else if (L.at) ++s.fill_price;
    else ++s.fill_any;
    acts(L.actions);
  }
  acts(c.prog.initial);
  for (auto& t : c.prog.templates) {
    if (t.type == ob::Type::StopLimit || t.type == ob::Type::StopMarket) ++s.stops;
    else if (t.type == ob::Type::IOC) ++s.iocs;
    else ++s.limits;
  }
  return s;
}
void print_census(const char* what, const Census& s) {
  std::printf("  %s: links on-fill@price=%d on-fill(any)=%d on-accept=%d | actions submit=%d cancel=%d "
              "reduce=%d halt=%d | templates limit=%d ioc=%d stop=%d\n",
              what, s.fill_price, s.fill_any, s.accept, s.submit, s.cancel, s.reduce, s.halt, s.limits,
              s.iocs, s.stops);
}

// Lock-step differential test of one variant on random programs. Returns mismatches.
struct Diff {
  long long programs = 0, instrs = 0, mism = 0, garbage_bad = 0, halted = 0;
  long long dnz = 0, dz = 0;
  double secs = 0;
};
Diff differential(const Variant& v, int N, uint64_t seed, bool filtered, int fixed_regs = 0) {
  auto t0 = std::chrono::steady_clock::now();
  std::mt19937_64 g(seed);
  Diff d;
  const uint64_t MAX = 2000;
  while (d.programs < N) {
    int nr;
    MProg p = random_prog(g, nr, fixed_regs);
    Regs r = random_regs(g, nr);
    auto ref = run_reference(p, r, MAX, Sem::Standard, d.programs < 500);
    if (filtered && (ref.distinct_pcs < 4 || ref.dec_nonzero < 20 || ref.dec_zero < 3)) continue;
    auto c = compile(p, nr, v.o);
    auto b = run_on_book(c, r, MAX, d.programs < 500, v.stp);
    bool ok = same(b.run, ref) && (d.programs >= 500 || b.run.trace == ref.trace);
    if (!ok && d.mism < 3)
      std::printf("    MISMATCH program %lld: ref %s %s  book %s %s\n", d.programs, status_name(ref.status),
                  regs_str(ref.regs).c_str(), status_name(b.run.status), regs_str(b.run.regs).c_str());
    d.mism += !ok;
    // garbage must be exactly the construction's fixed lots (persistent variants), never more
    if (c.opts.persistent_backstop && b.garbage != c.fixed_lots) ++d.garbage_bad;
    ++d.programs;
    d.instrs += ref.steps;
    d.halted += ref.status == Run::Halted;
    d.dnz += ref.dec_nonzero;
    d.dz += ref.dec_zero;
  }
  d.secs = secs_since(t0);
  return d;
}
void print_diff(const char* name, const Diff& d) {
  std::printf("  %-34s %lld programs, %lld instructions (%lld halted; %lld non-zero / %lld zero DECJZ): "
              "%lld mismatches, %lld garbage violations  [%.1fs]\n",
              name, d.programs, d.instrs, d.halted, d.dnz, d.dz, d.mism, d.garbage_bad, d.secs);
}

// Exhaustive single-gadget check: register 0..60 (one order or one-lot orders), a book the
// gadget must not touch, for one variant. Returns wrong cases.
long long gadget_check(const Variant& v, long long& cases) {
  long long bad = 0;
  MProg dec = {{Instr::DEC, 0, 1, 2}, {Instr::HALT}, {Instr::HALT}};
  MProg inc = {{Instr::INC, 0, 1, 0}, {Instr::HALT}};
  const int nregs = v.o.single_book ? 2 : 1;
  auto cd = compile(dec, nregs, v.o, 1), ci = compile(inc, nregs, v.o, 1);
  for (int v0 = 0; v0 <= 60; ++v0)
    for (int w = 0; w <= (v.o.single_book ? 3 : 0); ++w)  // single book: the other register too
      for (bool split : {false, true}) {
        auto run = [&](const Compiled& c, bool is_dec) {
          ob::Engine e(c.nbooks(), 3, v.stp, &c.prog);
          const Geo& g = c.geo[0];
          if (v0 > 0) {
            if (split)
              for (int i = 0; i < v0; ++i) e.submit({g.maker, g.book, g.side, ob::Type::Limit, g.reg, 0, 1, false});
            else
              e.submit({g.maker, g.book, g.side, ob::Type::Limit, g.reg, 0, v0, false});
          }
          if (c.nregs > 1 && w > 0) {
            const Geo& h = c.geo[1];
            e.submit({h.maker, h.book, h.side, ob::Type::Limit, h.reg, 0, w, false});
          }
          int ob_ = c.nbooks() - 1;  // unrelated book
          e.submit({2, ob_, Side::Buy, ob::Type::Limit, 500, 0, 7, false});
          uint64_t other = e.book_hash(ob_);
          e.start();
          e.run();
          const Geo& g0 = c.geo[0];
          ob::Qty reg = e.resting_qty(g0.book, g0.side, g0.reg);
          ob::Qty reg1 = c.nregs > 1 ? e.resting_qty(c.geo[1].book, c.geo[1].side, c.geo[1].reg) : 0;
          ob::Qty lots = 0;
          for (int b = 0; b < c.reg_books; ++b) lots += e.resting_lots(b);
          ob::Qty garbage = lots - reg - reg1;
          bool ok = e.halted() && e.book_hash(ob_) == other && reg1 == (c.nregs > 1 ? w : 0) &&
                    (!c.opts.persistent_backstop || garbage == c.fixed_lots) &&
                    (c.clock_book <= 0 || e.resting_orders(c.clock_book) == 0) &&
                    e.live_orders() == e.resting_orders();  // no pending stop left behind
          if (is_dec) {
            ok = ok && e.halt_code() == (v0 > 0 ? 1 : 2) && reg == (v0 > 0 ? v0 - 1 : 0);
            size_t want_trades = c.opts.stop_branch ? 2 : 1;  // probe (+ the fired stop)
            ok = ok && e.trade_count() == want_trades;
          } else {
            ok = ok && e.halt_code() == 1 && reg == v0 + 1;
            ok = ok && e.trade_count() == (c.opts.inc_clock ? 1u : 0u);
          }
          return ok;
        };
        cases += 2;
        bad += !run(cd, true) + !run(ci, false);
      }
  return bad;
}

// --- S1 -------------------------------------------------------------------------
void s1_minimal() {
  header("S1", "Minimal primitives: only 'on fill at price X -> submit', no cancel, no accept, no iceberg");
  Variant m{"fill@price->submit only (M)", opts_minimal(), STP::ExpireMaker};
  auto c = compile(paper_table3(), 2, m.o);
  print_census("Table 3 compiled under M", census(c));
  std::printf("  (every link is keyed by fill price; the only actions are submit and halt; one 1-lot\n"
              "   backstop per register placed at start and re-submitted by the zero branch's own link)\n");
  long long cases = 0;
  long long bad = gadget_check(m, cases);
  std::printf("  gadgets in isolation, register 0..60 as one order or one-lot orders: %lld cases, %lld wrong\n",
              cases, bad);
  print_diff("M, random programs", differential(m, 100000 / SCALE, 12345, false));
  print_diff("M, filtered corpus", differential(m, 20000 / SCALE, 777, true));
  // Garbage is constant in time.
  MProg loop = {{Instr::INC, 0, 1, 0}, {Instr::DEC, 0, 0, 2}, {Instr::HALT}};
  for (uint64_t n : {1000ull, 100000ull, 1000000ull}) {
    auto b = run_on_book(compile(loop, 1, m.o), {0}, n, false, m.stp);
    std::printf("  loop with register in {0,1}: %7llu instructions -> %zu resting orders (register + 1 backstop)\n",
                (unsigned long long)n, b.resting_orders);
  }
  std::printf("  -> the Lemma's stated hypotheses (fill-price-conditional routing + unbounded resting\n"
              "     quantity) suffice as written: no cancel, no accept link, no iceberg, book size =\n"
              "     sum of registers + number of registers.\n");
}

// --- S2 -------------------------------------------------------------------------
void s2_stop_branch() {
  header("S2", "Stop-branch: the exchange's stop triggers do the branching; the router never looks at a price");
  Variant s{"stops branch, router chains on fill (S)", opts_stop(), STP::ExpireMaker};
  auto c = compile(paper_table3(), 2, s.o);
  print_census("Table 3 compiled under S", census(c));
  std::printf("  DECJZ: probe IOC prints at P (non-zero) or P+2 (zero). Then submit a sell stop-limit\n"
              "  (trigger P, limit P-1) and a buy stop-limit (trigger P+2, limit P+2): the venue fires\n"
              "  exactly one, it trades, and *that order's* fill link continues. Links carry no price.\n");
  long long cases = 0;
  long long bad = gadget_check(s, cases);
  std::printf("  gadgets in isolation: %lld cases, %lld wrong (checks: branch, register, no pending stop\n"
              "  left behind, garbage == 2 backstop lots per register, other book untouched)\n", cases, bad);
  print_diff("S, random programs", differential(s, 100000 / SCALE, 12345, false));
  print_diff("S, filtered corpus", differential(s, 20000 / SCALE, 777, true));
  struct P {
    const char* name;
    MProg p;
    int nregs;
    Regs in;
  } ps[] = {{"mul 17*23", prog_mul(), 4, {17, 23, 0, 0}}, {"divmod 100/7", prog_divmod(), 5, {100, 7, 0, 0, 0}}};
  for (auto& x : ps) {
    auto ref = run_reference(x.p, x.in, 10'000'000);
    auto b = run_on_book(compile(x.p, x.nregs, s.o), x.in, 10'000'000, false, s.stp);
    std::printf("  %-14s -> %-16s %6llu instr, %6zu trades, lock-step %s\n", x.name, regs_str(b.run.regs).c_str(),
                (unsigned long long)b.run.steps, b.trades, same(b.run, ref) ? "identical" : "MISMATCH");
  }
  using R = TM::Rule;
  TM bb4{{{{R{1, +1, 1}, R{1, -1, 1}}}, {{R{1, -1, 0}, R{0, -1, 2}}}, {{R{1, +1, -1}, R{1, -1, 3}}},
          {{R{1, +1, 3}, R{0, +1, 0}}}}};
  auto t0 = std::chrono::steady_clock::now();
  MProg p = tm_to_counter(bb4);
  auto ref = run_reference(p, {0, 0, 0}, 2'000'000'000);
  auto b = run_on_book(compile(p, 3, s.o), {0, 0, 0}, 2'000'000'000, false, s.stp);
  std::printf("  BB(4) via 3 counters under S: %llu instr, %zu trades, book==ref %s, ones=%d [%.1fs]\n",
              (unsigned long long)b.run.steps, b.trades, same(b.run, ref) ? "yes" : "NO",
              __builtin_popcountll(b.run.regs[0]) + __builtin_popcountll(b.run.regs[1]), secs_since(t0));
  // Ablation: a stop whose trigger is already crossed must fire on submission.
  {
    ob::Engine e(1, 3);
    e.submit({1, 0, Side::Buy, ob::Type::Limit, 99, 0, 1, false});
    e.submit({2, 0, Side::Sell, ob::Type::Market, 0, 0, 1, false});  // prints 99
    e.submit({0, 0, Side::Sell, ob::Type::StopMarket, 0, 100, 1, false});  // trigger 100 >= last: fires now
    e.run();
    std::printf("  engine rule used: a stop submitted with its trigger already crossed fires at once (%s)\n",
                e.live_orders() == 0 ? "confirmed" : "NOT the case");
  }
  std::printf("  -> the conditional moves from the broker's router into the matching engine's stop logic.\n"
              "     Router needs: on-fill(my order) -> submit/cancel fixed templates. Venue needs: stop-limit\n"
              "     orders that trigger on last price, including immediately if already crossed.\n");
}

// --- S3 -------------------------------------------------------------------------
void s3_single_book() {
  header("S3", "One security: a single order book is Turing complete (ask side + bid side = two counters)");
  CompileOpts om = opts_minimal(), os = opts_stop();
  om.single_book = os.single_book = true;
  Variant vm{"single book, fill@price (M)", om, STP::ExpireMaker};
  Variant vs{"single book, stop-branch (S)", os, STP::ExpireMaker};
  std::printf("  layout in book 0: asks at %lld = R0, bids at %lld = R1, backstops at %lld / %lld, clock at %lld\n",
              (long long)om.P, (long long)(om.P - 10), (long long)(om.P + 1), (long long)(om.P - 11),
              (long long)(om.P - 5));
  for (auto* v : {&vm, &vs}) {
    long long cases = 0, bad = gadget_check(*v, cases);
    std::printf("  %-30s gadgets (R0 0..60, R1 0..3): %lld cases, %lld wrong\n", v->name, cases, bad);
    print_diff(v->name, differential(*v, 50000 / SCALE, 4242, false, 2));
  }
  std::printf("  2-counter programs (Minsky 1967) on ONE book:\n");
  struct P {
    const char* name;
    MProg p;
    int nregs;
    Regs in;
  } two[] = {{"2N, N=4", paper_table3(), 2, {4, 0}},
             {"add 3+4", prog_add(), 3, {3, 4, 0}},
             {"absdiff |2-5|", prog_absdiff(), 3, {2, 5, 0}},
             {"mul 2*2", prog_mul(), 4, {2, 2, 0, 0}}};
  for (auto& x : two) {
    MProg p2 = to_two_counter(x.p, x.nregs);
    Regs in2 = {godel_encode(x.in), 0};
    auto t0 = std::chrono::steady_clock::now();
    auto ref_k = run_reference(x.p, x.in, 100'000'000);
    auto ref2 = run_reference(p2, in2, 1'000'000'000);
    auto b = run_on_book(compile(p2, 2, om), in2, 1'000'000'000, false, STP::ExpireMaker);
    Regs dec = godel_decode(b.run.regs[0], x.nregs);
    std::printf("  %-14s R0=%lld -> %lld = %s, %8llu instr, %8zu trades, peak lots in the book %lld, "
                "book==ref %s, decoded==direct %s [%.1fs]\n",
                x.name, (long long)in2[0], (long long)b.run.regs[0], regs_str(dec).c_str(),
                (unsigned long long)b.run.steps, b.trades, (long long)b.peak_book_lots,
                same(b.run, ref2) ? "yes" : "NO", dec == ref_k.regs ? "yes" : "NO", secs_since(t0));
  }
  std::printf("  -> five price levels of one instrument, two accounts, and a fill-price router suffice.\n"
              "     (Price priority means only the best ask and best bid can be probed, so a single book\n"
              "      gives exactly two counters; Minsky showed two are enough.)\n");
}

// --- S4 -------------------------------------------------------------------------
TM bb5() {
  using R = TM::Rule;
  // Marxen-Buntrock 5-state champion: 47,176,870 steps, 4098 ones.
  return TM{{{{R{1, +1, 1}, R{1, -1, 2}}},
             {{R{1, +1, 2}, R{1, +1, 1}}},
             {{R{1, +1, 3}, R{0, -1, 4}}},
             {{R{1, -1, 0}, R{1, -1, 3}}},
             {{R{1, +1, -1}, R{0, -1, 0}}}}};
}

void s4_bounded() {
  header("S4", "Bounded books: a space-bounded Turing machine on n books that never hold more than 2 lots");
  CompileOpts om = opts_minimal();
  // Random TMs: direct bounded simulation vs reference counter machine vs book.
  {
    std::mt19937_64 g(31337);
    std::uniform_int_distribution<int> nq(1, 4), bit(0, 1), mv(0, 1), ncell(4, 9);
    const int N = 3000 / SCALE;
    long long halted = 0, out = 0, limited = 0, mism_ref = 0, mism_book = 0, lots_bad = 0, instrs = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
      TM tm;
      int q = nq(g);
      tm.delta.resize(q);
      std::uniform_int_distribution<int> nxt(-1, q - 1);
      for (auto& row : tm.delta)
        for (auto& r : row) r = {bit(g), mv(g) ? 1 : -1, nxt(g)};
      int n = ncell(g), h0 = n / 2;
      auto direct = run_tm_bounded(tm, n, h0, 400);
      BitProg bp = tm_to_bits(tm, n, h0);
      auto ref = run_reference(bp.prog, Regs(n, 0), 5000);
      auto b = run_on_book(compile(bp.prog, n, om), Regs(n, 0), 5000, false, STP::ExpireMaker);
      instrs += ref.steps;
      if (direct.status == TMBounded::StepLimit) {
        ++limited;
      } else {
        bool want_ok = direct.status == TMBounded::Halted;
        want_ok ? ++halted : ++out;
        bool ok = ref.status == Run::Halted && ref.pc == (want_ok ? bp.halt_ok : bp.halt_out);
        if (ok)
          for (int c = 0; c < n; ++c) ok = ok && ref.regs[c] == direct.tape[c];
        mism_ref += !ok;
      }
      mism_book += !same(b.run, ref);
      lots_bad += b.peak_book_lots > 2;
    }
    std::printf("  %d random 2-symbol TMs (1-4 states) on 4-9 cells: %lld halted, %lld ran off the tape, %lld hit\n"
                "  the step cap. bit-compiler vs direct tape: %lld mismatches; book vs reference: %lld mismatches;\n"
                "  books ever holding > 2 lots: %lld  (%lld instructions) [%.1fs]\n",
                N, halted, out, limited, mism_ref, mism_book, lots_bad, instrs, secs_since(t0));
  }
  using R = TM::Rule;
  TM bb4{{{{R{1, +1, 1}, R{1, -1, 1}}}, {{R{1, -1, 0}, R{0, -1, 2}}}, {{R{1, +1, -1}, R{1, -1, 3}}},
          {{R{1, +1, 3}, R{0, +1, 0}}}}};
  struct T {
    const char* name;
    TM tm;
    bool big;
  } tms[] = {{"BB(4)", bb4, false}, {"BB(5)", bb5(), true}};
  for (auto& x : tms) {
    if (x.big && !BIG) continue;
    auto t0 = std::chrono::steady_clock::now();
    auto probe = run_tm_bounded(x.tm, 1 << 16, 1 << 15, 100'000'000);
    int n = probe.max_head - probe.min_head + 3, h0 = (1 << 15) - probe.min_head + 1;
    auto direct = run_tm_bounded(x.tm, n, h0, 100'000'000);
    int ones = 0;
    for (int v : direct.tape) ones += v;
    BitProg bp = tm_to_bits(x.tm, n, h0);
    std::printf("  %s: %llu steps, %d ones, tape extent %d cells -> %d books + 1 clock book, %zu-instruction router\n",
                x.name, (unsigned long long)direct.steps, ones, n, n, bp.prog.size());
    auto b = run_on_book(compile(bp.prog, n, om), Regs(n, 0), 4'000'000'000ull, false, STP::ExpireMaker, {}, false);
    bool tape_ok = b.run.regs.size() == direct.tape.size();
    for (int c = 0; tape_ok && c < n; ++c) tape_ok = b.run.regs[c] == direct.tape[c];
    int book_ones = 0;
    for (auto v : b.run.regs) book_ones += v;
    std::printf("      on the book: %s at pc %d (halt_ok=%d), %llu instructions, %zu trades, %llu work items, "
                "%d ones, tape %s, peak lots in any register book %lld, final garbage %lld lots [%.1fs]\n",
                status_name(b.run.status), b.run.pc, bp.halt_ok, (unsigned long long)b.run.steps, b.trades,
                (unsigned long long)b.engine_work, book_ones, tape_ok ? "identical" : "DIFFERENT",
                (long long)b.peak_book_lots, (long long)b.garbage, secs_since(t0));
  }
  std::printf("  -> Each security's book holds at most 2 one-lot orders (the bit and its backstop), so the\n"
              "     per-book state is constant; the state that grows is the NUMBER of securities. This is the\n"
              "     reduction behind the corrected theorem: with order counts and sizes bounded, reachability\n"
              "     is PSPACE-complete in the number of securities (and polynomial for a fixed number).\n");
}

// --- S5 -------------------------------------------------------------------------
void s5_cost() {
  header("S5", "The price of computation: trades, round trips and resting orders per instruction");
  Variant vs[] = {{"paper (accept link)", CompileOpts{}, STP::None},
                  {"M fill@price only", opts_minimal(), STP::ExpireMaker},
                  {"S stop-branch", opts_stop(), STP::ExpireMaker}};
  struct P {
    const char* name;
    MProg p;
    int nregs;
    Regs in;
  } ps[] = {{"mul 17*23", prog_mul(), 4, {17, 23, 0, 0}},
            {"divmod 100/7", prog_divmod(), 5, {100, 7, 0, 0, 0}},
            {"BB(4) via 3 ctrs", tm_to_counter(TM{{{{TM::Rule{1, +1, 1}, TM::Rule{1, -1, 1}}},
                                                     {{TM::Rule{1, -1, 0}, TM::Rule{0, -1, 2}}},
                                                     {{TM::Rule{1, +1, -1}, TM::Rule{1, -1, 3}}},
                                                     {{TM::Rule{1, +1, 3}, TM::Rule{0, +1, 0}}}}}),
             3, {0, 0, 0}}};
  std::printf("  %-18s %-20s %10s %10s %10s %8s %8s\n", "program", "variant", "instr", "trades", "orders sent",
              "trd/ins", "ord/ins");
  for (auto& x : ps)
    for (auto& v : vs) {
      auto b = run_on_book(compile(x.p, x.nregs, v.o), x.in, 2'000'000'000, false, v.stp);
      // orders sent = submit actions executed = work items minus cancels/halts (approximation: work items)
      std::printf("  %-18s %-20s %10llu %10zu %10llu %8.2f %8.2f\n", x.name, v.name,
                  (unsigned long long)b.run.steps, b.trades, (unsigned long long)b.engine_work,
                  double(b.trades) / b.run.steps, double(b.engine_work) / b.run.steps);
    }
  std::printf("  Every DECJZ is a wash trade between the router's two accounts and pays a taker fee; every\n"
              "  router action is one venue round trip. At 1 round trip per 10 ms, BB(4) via 3 counters is\n"
              "  roughly %.0f minutes of exchange time; the same machine via S4's bit layout is seconds.\n",
              run_on_book(compile(ps[2].p, 3, opts_minimal()), {0, 0, 0}, 2'000'000'000, false, STP::ExpireMaker)
                      .engine_work *
                  0.010 / 60.0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--quick") SCALE = 50, BIG = false;
  if (argc > 1 && std::string(argv[1]) == "--no-bb5") BIG = false;
  auto t0 = std::chrono::steady_clock::now();
  std::printf("Stress round: what does the order-book computer actually need?\n");
  s1_minimal();
  s2_stop_branch();
  s3_single_book();
  s4_bounded();
  s5_cost();
  std::printf("\ntotal %.1fs\n", secs_since(t0));
}
