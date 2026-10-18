// Stage 1: is an order book a computer? Tests of Weinberger (arXiv 2608.28803), Section 6.3.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "engine.hpp"
#include "minsky.hpp"

using namespace mm;
using ob::Price;
using ob::Side;
using ob::STP;

namespace {

int SCALE = 1;  // --quick divides the random-test sizes by 50

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
std::string regs_str(const Regs& r) {
  std::string s = "[";
  for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + std::to_string(r[i]);
  return s + "]";
}
void header(const char* id, const char* title) { std::printf("\n=== %s  %s\n", id, title); }

MProg random_prog(std::mt19937_64& g, int& nregs) {
  std::uniform_int_distribution<int> len(1, 12), nr(1, 4), pct(0, 99);
  int n = len(g);
  nregs = nr(g);
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

// --- E1 -----------------------------------------------------------------------
void e1_literal_inc() {
  header("E1", "Table 2 INC: 'place order k when order is executed'");
  CompileOpts lit, acc, clk;
  lit.inc_waits_for_fill = true;
  clk.inc_clock = true;
  clk.two_accounts = true;
  auto c_lit = compile(paper_table3(), 2, lit), c_acc = compile(paper_table3(), 2, acc),
       c_clk = compile(paper_table3(), 2, clk);
  std::printf("  paper Table 3 program (2N -> R2):\n");
  std::printf("  %-3s %-22s %-22s %-22s %s\n", "N", "(a) wait for INC fill", "(b) continue on accept",
              "(c) clock trade", "reference");
  for (int N = 0; N <= 6; ++N) {
    auto a = run_on_book(c_lit, {N, 0}, 100000, false);
    auto b = run_on_book(c_acc, {N, 0}, 100000, false);
    auto c = run_on_book(c_clk, {N, 0}, 100000, false, STP::ExpireMaker);
    auto r = run_reference(paper_table3(), {N, 0}, 100000);
    std::printf("  %-3d %-8s R=%-11s %-8s R=%-11s %-8s R=%-11s %s R=%s\n", N, status_name(a.run.status),
                regs_str(a.run.regs).c_str(), status_name(b.run.status), regs_str(b.run.regs).c_str(),
                status_name(c.run.status), regs_str(c.run.regs).c_str(), status_name(r.status),
                regs_str(r.regs).c_str());
  }
  std::printf("  (a) 'executed' = filled, applied to the INC sell itself: that sell is only bought by a\n"
              "      later DECJZ the router never reaches, so the first INC deadlocks.\n"
              "  (b) 'executed' = placed: works unmodified (this is the variant used in E3).\n"
              "  (c) 'executed' = filled, but INC also crosses one lot with itself in a separate clock\n"
              "      book and continues on that fill: on-fill links alone suffice (two accounts, STP on).\n");
}

// --- E2 -----------------------------------------------------------------------
void e2_appendix_semantics() {
  header("E2", "Appendix C definition of DECJZ vs the Table 2 gadget");
  CompileOpts o;
  MProg one = {{Instr::DEC, 0, 1, 2}, {Instr::HALT}, {Instr::HALT}};
  auto c = compile(one, 1, o);
  std::printf("  single DECJZ(R,k=1,l=2), register value v:\n  %-3s %-12s %-14s %-14s\n", "v",
              "gadget ->", "standard ->", "Appendix C ->");
  for (int v = 0; v <= 3; ++v) {
    auto b = run_on_book(c, {v}, 10, false);
    auto s = run_reference(one, {v}, 10, Sem::Standard);
    auto a = run_reference(one, {v}, 10, Sem::AppendixC);
    std::printf("  %-3d I%d R=%-7s I%d R=%-9s I%d R=%s%s\n", v, b.run.pc, regs_str(b.run.regs).c_str(),
                s.pc, regs_str(s.regs).c_str(), a.pc, regs_str(a.regs).c_str(),
                a.pc != b.run.pc ? "   <-- disagree" : "");
  }
  std::printf("  paper Table 3 ('computes 2N'), interpreted under each definition:\n");
  for (int N = 1; N <= 5; ++N) {
    auto s = run_reference(paper_table3(), {N, 0}, 1000, Sem::Standard);
    auto a = run_reference(paper_table3(), {N, 0}, 1000, Sem::AppendixC);
    std::printf("    N=%d  standard R2=%lld   Appendix C R2=%lld\n", N, (long long)s.regs[1],
                (long long)a.regs[1]);
  }
  std::mt19937_64 g(7);
  int n = 20000 / SCALE, agree_std = 0, agree_app = 0;
  for (int i = 0; i < n; ++i) {
    int nr;
    MProg p = random_prog(g, nr);
    Regs r = random_regs(g, nr);
    auto b = run_on_book(compile(p, nr, o), r, 500, false);
    agree_std += same(b.run, run_reference(p, r, 500, Sem::Standard));
    agree_app += same(b.run, run_reference(p, r, 500, Sem::AppendixC));
  }
  std::printf("  random programs: gadget matches standard semantics %d/%d, Appendix C %d/%d\n",
              agree_std, n, agree_app, n);
}

// --- E3 -----------------------------------------------------------------------
void e3_differential() {
  header("E3", "Lock-step differential test: compiled order program vs reference interpreter");
  struct V {
    const char* name;
    CompileOpts o;
    STP stp;
  };
  CompileOpts paper, gc, clk;
  gc.gc_backstop = true;
  clk.gc_backstop = true;
  clk.inc_clock = true;
  clk.two_accounts = true;
  V vs[] = {{"paper gadgets (INC on accept)", paper, STP::None},
            {"+ backstop cancelled on non-zero", gc, STP::None},
            {"fill-only links (clock INC), 2 accts", clk, STP::ExpireMaker}};
  const int N = 100000 / SCALE;
  const uint64_t MAX = 2000;
  for (auto& v : vs) {
    auto t0 = std::chrono::steady_clock::now();
    std::mt19937_64 g(12345);
    long long mism = 0, instrs = 0, halted = 0, limited = 0, dnz = 0, dz = 0, trivial = 0;
    for (int i = 0; i < N; ++i) {
      int nr;
      MProg p = random_prog(g, nr);
      Regs r = random_regs(g, nr);
      auto ref = run_reference(p, r, MAX, Sem::Standard, i < 2000);
      auto b = run_on_book(compile(p, nr, v.o), r, MAX, i < 2000, v.stp);
      bool ok = same(b.run, ref) && (i >= 2000 || b.run.trace == ref.trace);
      if (!ok && mism < 3)
        std::printf("    MISMATCH program %d: ref %s %s  book %s %s\n", i, status_name(ref.status),
                    regs_str(ref.regs).c_str(), status_name(b.run.status), regs_str(b.run.regs).c_str());
      mism += !ok;
      dnz += ref.dec_nonzero;
      dz += ref.dec_zero;
      trivial += ref.distinct_pcs <= 2;
      instrs += ref.steps;
      halted += ref.status == Run::Halted;
      limited += ref.status == Run::StepLimit;
    }
    std::printf("  %-36s %d programs, %lld instructions compared (%lld halted, %lld hit the %llu-step "
                "cap): %lld mismatches  [%.1fs]\n",
                v.name, N, instrs, halted, limited, (unsigned long long)MAX, mism, secs_since(t0));
    if (&v == &vs[0])
      std::printf("    coverage: %lld non-zero DECJZ, %lld zero DECJZ; %lld programs (%.0f%%) visit <= 2 "
                  "distinct instructions\n",
                  dnz, dz, trivial, 100.0 * trivial / N);
  }
  // Filtered corpus: only programs that do real work.
  {
    auto t0 = std::chrono::steady_clock::now();
    std::mt19937_64 g(777);
    const int M = 20000 / SCALE;
    long long tried = 0, mism = 0, instrs = 0, dnz = 0, dz = 0, pcs = 0;
    for (int kept = 0; kept < M;) {
      ++tried;
      int nr;
      MProg p = random_prog(g, nr);
      Regs r = random_regs(g, nr);
      auto ref = run_reference(p, r, MAX);
      if (ref.distinct_pcs < 4 || ref.dec_nonzero < 20 || ref.dec_zero < 3) continue;
      ++kept;
      for (auto& v : vs) mism += !same(run_on_book(compile(p, nr, v.o), r, MAX, false, v.stp).run, ref);
      instrs += ref.steps;
      dnz += ref.dec_nonzero;
      dz += ref.dec_zero;
      pcs += ref.distinct_pcs;
    }
    std::printf("  filtered corpus (>= 4 distinct instructions, >= 20 non-zero and >= 3 zero DECJZ; kept %d of\n"
                "  %lld generated), all 3 variants: %lld instructions each (%lld non-zero / %lld zero DECJZ,\n"
                "  mean %.1f distinct instructions): %lld mismatches  [%.1fs]\n",
                M, tried, instrs, dnz, dz, double(pcs) / M, mism, secs_since(t0));
  }
  std::printf("  (registers are read from the books' resting quantity at P_i before every instruction;\n"
              "   the first 2000 programs also compare the full (pc, registers) trace element by element)\n");
}

// --- E4 -----------------------------------------------------------------------
void e4_exhaustive_gadgets() {
  header("E4", "Exhaustive local gadget check (base case of the simulation proof)");
  long long cases = 0, bad = 0;
  for (bool gc : {false, true}) {
    CompileOpts o;
    o.gc_backstop = gc;
    MProg dec = {{Instr::DEC, 0, 1, 2}, {Instr::HALT}, {Instr::HALT}};
    MProg inc = {{Instr::INC, 0, 1, 0}, {Instr::HALT}};
    auto cd = compile(dec, 1, o), ci = compile(inc, 1, o);
    for (int v = 0; v <= 60; ++v)
      for (int g = 0; g <= (gc ? 0 : 60); ++g)
        for (bool split : {false, true}) {
          // build the book directly: v register lots (one order or v one-lot orders), g garbage lots
          auto run = [&](const Compiled& c) {
            ob::Engine e(2, 3, STP::None, &c.prog);
            e.log_trades = true;
            if (v > 0) {
              if (split)
                for (int i = 0; i < v; ++i) e.submit({0, 0, Side::Sell, ob::Type::Limit, o.P, 0, 1, false});
              else
                e.submit({0, 0, Side::Sell, ob::Type::Limit, o.P, 0, v, false});
            }
            for (int i = 0; i < g; ++i) e.submit({0, 0, Side::Sell, ob::Type::Limit, o.P + 1, 0, 1, false});
            e.submit({2, 1, Side::Buy, ob::Type::Limit, 500, 0, 7, false});  // unrelated book
            uint64_t other = e.book_hash(1);
            e.start();
            e.run();
            struct R {
              int code;
              ob::Qty reg, garbage, bids;
              size_t trades;
              bool other_same;
            };
            return R{e.halt_code(), e.resting_qty(0, Side::Sell, o.P), e.resting_qty(0, Side::Sell, o.P + 1),
                     e.resting_qty(0, Side::Buy, o.P + 1), e.trades.size(), e.book_hash(1) == other};
          };
          auto d = run(cd);
          bool ok_d = d.code == (v > 0 ? 1 : 2) && d.reg == (v > 0 ? v - 1 : 0) &&
                      d.garbage == g + (v > 0 && !gc ? 1 : 0) && d.bids == 0 && d.trades == 1 && d.other_same;
          auto i = run(ci);
          bool ok_i = i.code == 1 && i.reg == v + 1 && i.garbage == g && i.trades == 0 && i.other_same;
          cases += 2;
          bad += !ok_d + !ok_i;
        }
  }
  std::printf("  DECJZ and INC gadgets, register 0..60 (one order or 60 one-lot orders), garbage 0..60,\n"
              "  with and without backstop cleanup: %lld cases, %lld wrong\n", cases, bad);
  std::printf("  checked per case: branch taken, register after, backstop lots after, no stray bids,\n"
              "  exactly one trade for DECJZ / none for INC, every other book bit-identical\n");
}

// --- E5 -----------------------------------------------------------------------
void e5_programs() {
  header("E5", "Real programs, Minsky's 2-counter reduction, and Turing machines on the order book");
  CompileOpts o;
  o.gc_backstop = true;
  struct P {
    const char* name;
    MProg p;
    int nregs;
    Regs in;
  };
  P ps[] = {{"paper Table 3: 2N, N=7", paper_table3(), 2, {7, 0}},
            {"add 13+29", prog_add(), 3, {13, 29, 0}},
            {"absdiff |5-12|", prog_absdiff(), 3, {5, 12, 0}},
            {"absdiff |12-5|", prog_absdiff(), 3, {12, 5, 0}},
            {"mul 17*23", prog_mul(), 4, {17, 23, 0, 0}},
            {"divmod 100/7", prog_divmod(), 5, {100, 7, 0, 0, 0}}};
  for (auto& x : ps) {
    auto ref = run_reference(x.p, x.in, 10'000'000);
    auto b = run_on_book(compile(x.p, x.nregs, o), x.in, 10'000'000, false);
    std::printf("  %-26s -> %-18s %8llu instr, %8zu trades, lock-step %s\n", x.name,
                regs_str(b.run.regs).c_str(), (unsigned long long)b.run.steps, b.trades,
                same(b.run, ref) ? "identical" : "MISMATCH");
  }

  std::printf("  two books only (Minsky 1967: two counters are Turing complete):\n");
  P two[] = {{"2N via 2 counters, N=4", paper_table3(), 2, {4, 0}},
             {"add 3+4 via 2 counters", prog_add(), 3, {3, 4, 0}},
             {"absdiff |2-5| via 2 counters", prog_absdiff(), 3, {2, 5, 0}},
             {"mul 2*2 via 2 counters", prog_mul(), 4, {2, 2, 0, 0}}};
  for (auto& x : two) {
    MProg p2 = to_two_counter(x.p, x.nregs);
    Regs in2 = {godel_encode(x.in), 0};
    auto t0 = std::chrono::steady_clock::now();
    auto ref_k = run_reference(x.p, x.in, 100'000'000);
    auto ref2 = run_reference(p2, in2, 1'000'000'000);
    auto b = run_on_book(compile(p2, 2, o), in2, 1'000'000'000, false);
    Regs dec = godel_decode(b.run.regs[0], x.nregs);
    std::printf("  %-30s %4zu-instr 2-counter program, R0=%lld -> R0=%lld = %s, %9llu instr, "
                "book==ref2 %s, decoded==direct %s [%.1fs]\n",
                x.name, p2.size(), (long long)in2[0], (long long)b.run.regs[0], regs_str(dec).c_str(),
                (unsigned long long)b.run.steps, same(b.run, ref2) ? "yes" : "NO",
                dec == ref_k.regs ? "yes" : "NO", secs_since(t0));
  }

  std::printf("  Turing machines compiled to 3 counters (L, R, temp), then to orders:\n");
  using R = TM::Rule;
  TM bb2{{{{R{1, +1, 1}, R{1, -1, 1}}}, {{R{1, -1, 0}, R{1, +1, -1}}}}};
  // 4-state 2-symbol busy beaver champion (Brady): 107 steps, 13 ones
  TM bb4{{{{R{1, +1, 1}, R{1, -1, 1}}},
          {{R{1, -1, 0}, R{0, -1, 2}}},
          {{R{1, +1, -1}, R{1, -1, 3}}},
          {{R{1, +1, 3}, R{0, +1, 0}}}}};
  struct T {
    const char* name;
    TM tm;
  } tms[] = {{"busy beaver BB(2)", bb2}, {"busy beaver BB(4)", bb4}};
  for (auto& x : tms) {
    auto t0 = std::chrono::steady_clock::now();
    TMResult direct = run_tm(x.tm, 1'000'000);
    MProg p = tm_to_counter(x.tm);
    auto ref = run_reference(p, {0, 0, 0}, 2'000'000'000);
    auto b = run_on_book(compile(p, 3, o), {0, 0, 0}, 2'000'000'000, false);
    // decode tape around the head from L and R
    int64_t L = b.run.regs[0], Rr = b.run.regs[1];
    int ones = __builtin_popcountll(L) + __builtin_popcountll(Rr);
    bool tape_ok = true;
    for (int d = -40; d <= 40; ++d) {
      int idx = direct.head + d;
      int want = (idx >= 0 && idx < (int)direct.tape.size()) ? direct.tape[idx] : 0;
      int got = d >= 0 ? (Rr >> d) & 1 : (L >> (-d - 1)) & 1;
      tape_ok &= want == got;
    }
    std::printf("  %-18s direct: %3llu steps, %2d ones | on the book: %4zu-instr program, %9llu instr, "
                "%9zu trades, %2d ones, tape %s, book==ref %s [%.1fs]\n",
                x.name, (unsigned long long)direct.steps, direct.ones, p.size(),
                (unsigned long long)b.run.steps, b.trades, ones, tape_ok ? "identical" : "DIFFERENT",
                same(b.run, ref) ? "yes" : "NO", secs_since(t0));
  }
}

// --- E6 -----------------------------------------------------------------------
void e6_leak() {
  header("E6", "Backstop leak: book size vs running time for a program whose registers stay in {0,1}");
  MProg loop = {{Instr::INC, 0, 1, 0}, {Instr::DEC, 0, 0, 2}, {Instr::HALT}};
  std::printf("  %-10s %-28s %-28s\n", "instr", "paper gadget: resting orders", "with cleanup: resting orders");
  for (uint64_t n : {1'000ull, 10'000ull, 100'000ull, 1'000'000ull}) {
    CompileOpts paper, gc;
    gc.gc_backstop = true;
    auto a = run_on_book(compile(loop, 1, paper), {0}, n, false);
    auto b = run_on_book(compile(loop, 1, gc), {0}, n, false);
    std::printf("  %-10llu %-28zu %-28zu\n", (unsigned long long)n, a.resting_orders, b.resting_orders);
  }
  std::printf("  -> with the gadget as written the book grows linearly with time even though no\n"
              "     register exceeds 1, so a bounded book cannot run even a bounded-register machine\n"
              "     for long. Cancelling the backstop on the non-zero branch keeps it constant.\n");
}

// --- E7 -----------------------------------------------------------------------
void e7_no_router() {
  header("E7", "Ablation: the book without the router (no links)");
  std::mt19937_64 g(99);
  std::uniform_int_distribution<int> acct(0, 2), book(0, 2), type(0, 4), side(0, 1), qty(1, 5),
      px(95, 105), pct(0, 99), nord(1, 200);
  long long runs = 20000 / SCALE, violations = 0;
  double worst = 0;
  for (int i = 0; i < runs; ++i) {
    ob::Engine e(3, 3, i % 2 ? STP::ExpireMaker : STP::None);
    e.log_trades = false;
    int n = nord(g);
    long long Q = 0, stops = 0;
    for (int j = 0; j < n; ++j) {
      ob::Template t{acct(g), book(g), side(g) ? Side::Buy : Side::Sell, static_cast<ob::Type>(type(g)),
                     px(g), px(g), qty(g), pct(g) < 10};
      Q += t.qty;
      stops += t.type == ob::Type::StopMarket || t.type == ob::Type::StopLimit;
      e.submit(t);
      if (e.run(1'000'000) != ob::Status::Idle) ++violations;
    }
    if ((long long)e.trade_count() * 2 > Q || (long long)e.work_done() > stops) ++violations;
    worst = std::max(worst, 2.0 * e.trade_count() / Q);
  }
  std::printf("  %lld random order sets (1-200 orders of every type, 3 books, stops cascading):\n"
              "  all quiesced; bound violations (trades > Q/2, or triggered-stop work > #stops): %lld;\n"
              "  worst 2*trades/Q = %.2f\n",
              runs, violations, worst);
  std::printf("  -> without links nothing creates quantity, every trade removes >= 2 lots, every stop\n"
              "     fires at most once: a finite order set always halts within Q/2 trades. Unbounded\n"
              "     iteration (loops) exists only through the router re-submitting orders.\n");
}

// --- E8 -----------------------------------------------------------------------
void e8_stp() {
  header("E8", "Self-trade prevention (Hyperliquid documents expire-maker for same-address trades)");
  std::printf("  paper Table 3, N=3 (expect R2=6):\n");
  for (bool two : {false, true})
    for (STP m : {STP::None, STP::ExpireMaker, STP::CancelTaker}) {
      CompileOpts o;
      o.two_accounts = two;
      auto b = run_on_book(compile(paper_table3(), 2, o), {3, 0}, 100000, false, m);
      const char* mn = m == STP::None ? "none" : m == STP::ExpireMaker ? "expire-maker" : "cancel-taker";
      std::printf("    %-11s STP=%-13s -> %-9s R=%-8s after %llu instr\n", two ? "2 accounts" : "1 account",
                  mn, status_name(b.run.status), regs_str(b.run.regs).c_str(),
                  (unsigned long long)b.run.steps);
    }
  std::printf("  single DECJZ on a register holding 5 lots (expect R=4, non-zero branch):\n");
  MProg one = {{Instr::DEC, 0, 1, 2}, {Instr::HALT}, {Instr::HALT}};
  for (STP m : {STP::ExpireMaker, STP::CancelTaker}) {
    auto b = run_on_book(compile(one, 1, CompileOpts{}), {5}, 10, false, m);
    std::printf("    1 account   STP=%-13s -> %-9s R=%-4s backstop lots left=%lld, trades=%zu\n",
                m == STP::ExpireMaker ? "expire-maker" : "cancel-taker", status_name(b.run.status),
                regs_str(b.run.regs).c_str(), (long long)b.garbage, b.trades);
  }
  std::printf("  -> with one account the DECJZ buy can only ever meet the router's own sells (register\n"
              "     lots or its own backstop), so STP blocks every fill: no fill price, no branch, the\n"
              "     router stalls on the first DECJZ. Expire-maker additionally cancels the whole register.\n"
              "     Splitting buyer and seller across two accounts restores the computation.\n");
}

// --- E9 -----------------------------------------------------------------------
void e9_interference() {
  header("E9", "Other market participants");
  CompileOpts o;
  o.gc_backstop = true;
  const int N = 20000 / SCALE;
  // (a) paper: "Turing completeness still obtains if we intersperse arbitrary orders for other
  //     securities anywhere in the queue"
  {
    std::mt19937_64 g(5);
    std::uniform_int_distribution<int> pct(0, 99), px(990, 1010), q(1, 3), side(0, 1), type(0, 2);
    long long mism = 0, injected = 0;
    for (int i = 0; i < N; ++i) {
      int nr;
      MProg p = random_prog(g, nr);
      Regs r = random_regs(g, nr);
      auto c = compile(p, nr, o, 1);
      auto noise = [&](ob::Engine& e, uint64_t) {
        if (pct(g) < 30) {
          e.submit({2, nr, side(g) ? Side::Buy : Side::Sell, static_cast<ob::Type>(type(g)), px(g), 0, q(g), false});
          ++injected;
        }
      };
      mism += !same(run_on_book(c, r, 1000, false, STP::None, noise).run, run_reference(p, r, 1000));
    }
    std::printf("  (a) third-party orders in OTHER securities between router steps: %d programs, %lld\n"
                "      foreign orders injected, %lld mismatches  -> paper's claim holds (by construction:\n"
                "      books share no state and foreign orders fire no links; this is a sanity check)\n",
                N, injected, mism);
  }
  // (b) one third-party 1-lot market buy in a REGISTER security at a random moment
  {
    std::mt19937_64 g(6);
    std::uniform_int_distribution<int> when(0, 60);
    long long fired = 0, diverged = 0, stalled = 0;
    for (int i = 0; i < N; ++i) {
      int nr;
      MProg p = random_prog(g, nr);
      Regs r = random_regs(g, nr);
      auto c = compile(p, nr, o);
      int at = when(g);
      int book = std::uniform_int_distribution<int>(0, nr - 1)(g);
      bool did = false;
      auto noise = [&](ob::Engine& e, uint64_t idx) {
        if ((int)idx == at) {
          e.submit({2, book, Side::Buy, ob::Type::Market, 0, 0, 1, false});
          did = true;
        }
      };
      auto b = run_on_book(c, r, 1000, false, STP::None, noise);
      if (!did) continue;
      ++fired;
      diverged += !same(b.run, run_reference(p, r, 1000));
      stalled += b.run.status == Run::Stalled;
    }
    std::printf("  (b) ONE third-party 1-lot market buy in a register's own book: %lld runs hit, %lld\n"
                "      computed something else (%lld of them stalled the router)\n",
                fired, diverged, stalled);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--quick") SCALE = 50;
  auto t0 = std::chrono::steady_clock::now();
  std::printf("Stage 1: is an order book a computer?  Weinberger, arXiv 2608.28803, Sec. 6.3 Lemma + Table 2\n");
  e1_literal_inc();
  e2_appendix_semantics();
  e3_differential();
  e4_exhaustive_gadgets();
  e5_programs();
  e6_leak();
  e7_no_router();
  e8_stp();
  e9_interference();
  std::printf("\ntotal %.1fs\n", secs_since(t0));
}
