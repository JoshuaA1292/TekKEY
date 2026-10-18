// Rule 110 (a Turing-complete cellular automaton, Cook 2004) running on an exchange.
// Cell i of the current row is the resting ask quantity at P in book i (0 or 1 lot); the next row
// lives in books n..2n-1. Each generation reads three neighbours destructively (a DECJZ tree with
// eight leaves), rewrites them, writes the new cell, then copies the next row back. Every link is
// "on fill at price X -> submit"; no book ever holds more than two one-lot orders.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "minsky.hpp"

using namespace mm;

// Returns the program; gen_marker[g] is the instruction index that starts generation g, and
// gen_marker[gens] the HALT.
static MProg compile_rule110(int n, int gens, std::vector<int>& gen_marker) {
  Asm a;
  auto cur = [](int i) { return i; };
  auto nxt = [&](int i) { return n + i; };
  gen_marker.assign(gens + 1, -1);
  std::vector<int> GEN(gens + 1);
  for (auto& g : GEN) g = a.label();
  for (int g = 0; g < gens; ++g) {
    a.bind(GEN[g]);
    gen_marker[g] = static_cast<int>(a.size());
    // Phase 1: for each cell, read (l, c, r), restore them, write nxt[i].
    int START = GEN[g];
    for (int i = 0; i < n; ++i) {
      int CONT = a.label();
      struct Leaf {
        int label;
        std::vector<int> incs;
      };
      std::vector<Leaf> pending;
      auto target = [&](int l, int c, int r) {
        std::vector<int> incs;
        if (l) incs.push_back(cur(i - 1));
        if (c) incs.push_back(cur(i));
        if (r) incs.push_back(cur(i + 1));
        if ((110 >> (l * 4 + c * 2 + r)) & 1) incs.push_back(nxt(i));
        if (incs.empty()) return CONT;
        int L = a.label();
        pending.push_back({L, incs});
        return L;
      };
      // level c for a fixed l (bound by the caller)
      auto level_c = [&](int l) {
        if (i + 1 < n) {
          int RC1 = a.label(), RC0 = a.label();
          a.dec(cur(i), RC1, RC0);
          a.bind(RC1);
          a.dec(cur(i + 1), target(l, 1, 1), target(l, 1, 0));
          a.bind(RC0);
          a.dec(cur(i + 1), target(l, 0, 1), target(l, 0, 0));
        } else {
          a.dec(cur(i), target(l, 1, 0), target(l, 0, 0));
        }
      };
      if (i > 0) {
        int CL1 = a.label(), CL0 = a.label();
        if (i > 0 && START != GEN[g]) a.bind(START);
        a.dec(cur(i - 1), CL1, CL0);
        a.bind(CL1);
        level_c(1);
        a.bind(CL0);
        level_c(0);
      } else {
        level_c(0);
      }
      for (auto& L : pending) {
        a.bind(L.label);
        for (size_t k = 0; k < L.incs.size(); ++k) {
          if (k + 1 == L.incs.size()) a.inc(L.incs[k], CONT);
          else a.inc_ff(L.incs[k]);
        }
      }
      START = CONT;  // next cell binds it
    }
    a.bind(START);  // copy phase
    // Phase 2: copy nxt -> cur. Last cell jumps to the next generation.
    for (int i = 0; i < n; ++i) {
      int NEXT = i + 1 < n ? a.label() : GEN[g + 1];
      int ONE = a.label(), ZERO = a.label(), K = a.label();
      a.dec(nxt(i), ONE, ZERO);
      a.bind(ONE);
      a.dec(cur(i), K, K);  // clear
      a.bind(K);
      a.inc(cur(i), NEXT);
      a.bind(ZERO);
      a.dec(cur(i), NEXT, NEXT);  // clear
      if (i + 1 < n) a.bind(NEXT);
    }
  }
  a.bind(GEN[gens]);
  gen_marker[gens] = static_cast<int>(a.size());
  a.halt();
  return a.finish();
}

static std::vector<int> step110(const std::vector<int>& row) {
  int n = static_cast<int>(row.size());
  std::vector<int> out(n);
  for (int i = 0; i < n; ++i) {
    int l = i > 0 ? row[i - 1] : 0, c = row[i], r = i + 1 < n ? row[i + 1] : 0;
    out[i] = (110 >> (l * 4 + c * 2 + r)) & 1;
  }
  return out;
}

int main(int argc, char** argv) {
  int n = argc > 1 ? std::atoi(argv[1]) : 48, gens = argc > 2 ? std::atoi(argv[2]) : 24;
  std::vector<int> marker;
  MProg prog = compile_rule110(n, gens, marker);
  CompileOpts o;
  o.persistent_backstop = o.inc_clock = o.two_accounts = true;
  Compiled c = compile(prog, 2 * n, o);
  std::printf("Rule 110 on %d cells for %d generations: %zu-instruction router, %d books (+1 clock)\n\n",
              n, gens, prog.size(), 2 * n);

  ob::Engine eng(c.nbooks(), 3, ob::STP::ExpireMaker, &c.prog);
  eng.log_trades = false;
  std::vector<int> row(n, 0);
  row[n - 1] = 1;
  eng.submit({c.geo[n - 1].maker, c.geo[n - 1].book, c.geo[n - 1].side, ob::Type::Limit, c.geo[n - 1].reg, 0, 1, false});

  std::vector<int> expect = row;
  int shown = 0, wrong = 0;
  uint64_t instrs = 0;
  auto read_row = [&] {
    std::vector<int> v(n);
    for (int i = 0; i < n; ++i) v[i] = static_cast<int>(eng.resting_qty(c.geo[i].book, c.geo[i].side, c.geo[i].reg));
    return v;
  };
  auto show = [&](const std::vector<int>& v, int g) {
    std::string s;
    for (int x : v) s += x ? "\xe2\x96\x88" : "\xc2\xb7";
    std::printf("  gen %2d  %s%s\n", g, s.c_str(), v == expect ? "" : "   <-- DIFFERS from direct Rule 110");
  };
  eng.on_action = [&](const ob::Action& a) {
    int j = -1;
    if (a.kind == ob::Act::Submit) j = c.entry_instr[a.arg];
    if (a.kind == ob::Act::Halt) j = a.arg;
    if (j < 0) return;
    ++instrs;
    if (shown <= gens && j == marker[shown]) {
      auto v = read_row();
      wrong += v != expect;
      show(v, shown);
      expect = step110(expect);
      ++shown;
    }
  };
  auto t0 = std::chrono::steady_clock::now();
  eng.start();
  eng.run();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ob::Qty peak = 0;
  for (int b = 0; b < 2 * n; ++b) peak = std::max(peak, eng.peak_lots(b));
  std::printf("\n  %s. %llu instructions, %llu trades, %llu router work items, peak %lld lots in any book, "
              "%d of %d rows differ from a direct simulation [%.2fs]\n",
              eng.halted() ? "halted" : "STALLED", (unsigned long long)instrs,
              (unsigned long long)eng.trade_count(), (unsigned long long)eng.work_done(), (long long)peak, wrong,
              shown, secs);
  std::printf("  per generation: %.0f trades, %.0f round trips\n", double(eng.trade_count()) / gens,
              double(eng.work_done()) / gens);
  return wrong ? 1 : 0;
}
