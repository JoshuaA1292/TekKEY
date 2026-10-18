#include "programs.hpp"

namespace programs {

using mm::Asm;
using mm::MProg;

MProg rule110(int n, int gens, int& gen_marker) {
  Asm a;
  auto cur = [](int i) { return i; };
  auto nxt = [&](int i) { return n + i; };
  const int GEN = a.label();
  const int HALT = a.label();
  const int ngen = gens > 0 ? gens : 1;
  for (int g = 0; g < ngen; ++g) {
    const int GSTART = g == 0 ? GEN : a.label();
    a.bind(GSTART);
    if (g == 0) gen_marker = static_cast<int>(a.size());
    int START = GSTART;
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
        if (START != GSTART) a.bind(START);
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
      START = CONT;
    }
    a.bind(START);
    // copy nxt -> cur; the last cell jumps to the next generation (or back to the first)
    const int AFTER = gens > 0 ? (g + 1 < ngen ? a.label() : HALT) : GEN;
    for (int i = 0; i < n; ++i) {
      int NEXT = i + 1 < n ? a.label() : AFTER;
      int ONE = a.label(), ZERO = a.label(), K = a.label();
      a.dec(nxt(i), ONE, ZERO);
      a.bind(ONE);
      a.dec(cur(i), K, K);
      a.bind(K);
      a.inc(cur(i), NEXT);
      a.bind(ZERO);
      a.dec(cur(i), NEXT, NEXT);
      if (i + 1 < n) a.bind(NEXT);
    }
    if (gens > 0 && g + 1 < ngen) {
      // AFTER is the next generation's start label; bind it at the top of the next loop body
      // by making the next GSTART that label
      // (handled by binding: the next iteration creates its own label, so alias here)
      a.bind(AFTER);
    }
  }
  a.bind(HALT);
  a.halt();
  return a.finish();
}

std::vector<int> rule110_step(const std::vector<int>& row) {
  int n = static_cast<int>(row.size());
  std::vector<int> out(n);
  for (int i = 0; i < n; ++i) {
    int l = i > 0 ? row[i - 1] : 0, c = row[i], r = i + 1 < n ? row[i + 1] : 0;
    out[i] = (110 >> (l * 4 + c * 2 + r)) & 1;
  }
  return out;
}

}  // namespace programs
