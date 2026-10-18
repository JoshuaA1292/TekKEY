// A tiny ISA over one-bit registers, each register being one order book holding 0 or 1 resting
// lot, compiled to Minsky instructions (INC, DECJZ) which stage 1's compiler turns into orders.
// Every macro ends by jumping to the label it is given; a Minsky machine has no fall-through
// jump, so control flow is entirely label-driven. Reads are non-destructive (DEC then INC back
// on the 1 path), so a read costs one instruction for a 0 and two for a 1.
#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "minsky.hpp"

namespace bits {

using Bits = std::vector<int>;  // bit 0 first

class BitAsm {
 public:
  explicit BitAsm(mm::Asm& a) : a_(a) {}
  int label() { return a_.label(); }
  void bind(int l) { a_.bind(l); }
  int here() { return a_.here_label(); }
  mm::Asm& raw() { return a_; }

  // ---- one bit ----
  // if b == 1 go to if1 else if0; b unchanged
  void read(int b, int if1, int if0) {
    int L = a_.label();
    a_.dec(b, L, if0);
    a_.bind(L);
    a_.inc(b, if1);
  }
  // destructive read: b := 0 afterwards
  void readd(int b, int if1, int if0) { a_.dec(b, if1, if0); }
  void clear(int b, int next) { a_.dec(b, next, next); }
  void set(int b, int next) { a_.inc(b, next); }  // requires b == 0
  void write1(int b, int next) {
    int L = a_.label();
    a_.dec(b, L, L);
    a_.bind(L);
    a_.inc(b, next);
  }
  void write(int b, int value, int next) {
    if (value) write1(b, next);
    else clear(b, next);
  }
  // dst := src
  void copy(int src, int dst, int next) {
    if (src == dst) throw std::invalid_argument("copy: a bit onto itself");
    int L = a_.label(), L1 = a_.label();
    a_.dec(dst, L, L);
    a_.bind(L);
    read(src, L1, next);
    a_.bind(L1);
    a_.inc(dst, next);
  }
  // dst := src; src := 0
  void move(int src, int dst, int next) {
    int L = a_.label(), L1 = a_.label();
    a_.dec(dst, L, L);
    a_.bind(L);
    a_.dec(src, L1, next);
    a_.bind(L1);
    a_.inc(dst, next);
  }

  // ---- n bits ----
  // Every macro must emit at least one instruction so that control reaches `next`; an empty
  // vector would silently fall through.
  static void need(const Bits& b, size_t max = 1u << 20) {
    if (b.empty() || b.size() > max) throw std::invalid_argument("bit vector must have 1.." + std::to_string(max) + " bits");
  }
  void clear_n(const Bits& b, int next) {
    need(b);
    for (size_t i = 0; i < b.size(); ++i) {
      int n = i + 1 < b.size() ? a_.label() : next;
      clear(b[i], n);
      if (i + 1 < b.size()) a_.bind(n);
    }
  }
  void copy_n(const Bits& src, const Bits& dst, int next) {
    need(src);
    if (src.size() != dst.size()) throw std::invalid_argument("copy_n: sizes differ");
    for (size_t i = 0; i < src.size(); ++i) {
      int n = i + 1 < src.size() ? a_.label() : next;
      copy(src[i], dst[i], n);
      if (i + 1 < src.size()) a_.bind(n);
    }
  }
  // b := constant
  void write_n(const Bits& b, unsigned value, int next) {
    need(b, 32);
    if (b.size() < 32 && (value >> b.size()) != 0) throw std::invalid_argument("write_n: constant too wide");
    for (size_t i = 0; i < b.size(); ++i) {
      int n = i + 1 < b.size() ? a_.label() : next;
      write(b[i], (value >> i) & 1, n);
      if (i + 1 < b.size()) a_.bind(n);
    }
  }
  // b := b + 1 mod 2^n; on wrap-around go to carry instead of next
  void inc_n(const Bits& b, int next, int carry) {
    need(b);
    for (size_t i = 0; i < b.size(); ++i) {
      int Lset = a_.label();
      int Lnext = i + 1 < b.size() ? a_.label() : carry;
      a_.dec(b[i], Lnext, Lset);  // a 1 becomes 0 and the carry ripples
      a_.bind(Lset);
      a_.inc(b[i], next);
      if (i + 1 < b.size()) a_.bind(Lnext);
    }
  }
  // b := b - 1 mod 2^n; on wrap-around (b was 0) go to borrow
  void dec_n(const Bits& b, int next, int borrow) {
    need(b);
    for (size_t i = 0; i < b.size(); ++i) {
      int Lz = a_.label();
      int Lnext = i + 1 < b.size() ? a_.label() : borrow;
      a_.dec(b[i], next, Lz);
      a_.bind(Lz);
      a_.inc(b[i], Lnext);
      if (i + 1 < b.size()) a_.bind(Lnext);
    }
  }
  // non-destructive compare
  void eq_n(const Bits& x, const Bits& y, int eq, int ne) {
    need(x);
    if (x.size() != y.size()) throw std::invalid_argument("eq_n: sizes differ");
    for (size_t i = 0; i < x.size(); ++i) {
      int Lnext = i + 1 < x.size() ? a_.label() : eq;
      int L1 = a_.label(), L0 = a_.label();
      read(x[i], L1, L0);
      a_.bind(L1);
      read(y[i], Lnext, ne);
      a_.bind(L0);
      read(y[i], ne, Lnext);
      if (i + 1 < x.size()) a_.bind(Lnext);
    }
  }
  // compare with a constant
  void eq_const(const Bits& x, unsigned v, int eq, int ne) {
    need(x, 32);
    if (x.size() < 32 && (v >> x.size()) != 0) throw std::invalid_argument("eq_const: constant too wide");
    for (size_t i = 0; i < x.size(); ++i) {
      int Lnext = i + 1 < x.size() ? a_.label() : eq;
      if ((v >> i) & 1) read(x[i], Lnext, ne);
      else read(x[i], ne, Lnext);
      if (i + 1 < x.size()) a_.bind(Lnext);
    }
  }
  // Decoder: a binary tree over the bits (MSB first, non-destructive reads); leaf(value) emits
  // the code for that value and must end by jumping somewhere.
  void decode(const Bits& b, const std::function<void(unsigned)>& leaf) {
    need(b, 16);
    decode_rec(b, b.size(), 0, leaf);
  }
  // Two-bit branch: targets t[v] for v = b1 b0
  void branch2(int b1, int b0, int t0, int t1, int t2, int t3) {
    int H = a_.label(), L = a_.label();
    read(b1, H, L);
    a_.bind(H);
    read(b0, t3, t2);
    a_.bind(L);
    read(b0, t1, t0);
  }
  // Parity of the given bits into control flow: odd / even. Non-destructive. Two control
  // states (even so far, odd so far) walk the bits.
  void parity(const Bits& b, int odd, int even) {
    need(b);
    int curO = -1;
    for (size_t i = 0; i < b.size(); ++i) {
      const bool last = i + 1 == b.size();
      int nE = last ? even : a_.label();
      int nO = last ? odd : a_.label();
      read(b[i], nO, nE);  // even state (the entry for i == 0)
      if (curO >= 0) {
        a_.bind(curO);
        read(b[i], nE, nO);  // odd state
      }
      if (!last) {
        a_.bind(nE);
        curO = nO;
      }
    }
  }

 private:
  void decode_rec(const Bits& b, size_t depth, unsigned prefix, const std::function<void(unsigned)>& leaf) {
    if (depth == 0) {
      leaf(prefix);
      return;
    }
    int L1 = a_.label(), L0 = a_.label();
    read(b[depth - 1], L1, L0);
    a_.bind(L1);
    decode_rec(b, depth - 1, (prefix << 1) | 1, leaf);
    a_.bind(L0);
    decode_rec(b, depth - 1, prefix << 1, leaf);
  }
  mm::Asm& a_;
};

// Interpreter with a hook at every instruction entry (the stage 1 reference has none). The hook
// may change registers (external input) and returns false to stop.
struct HookRun {
  enum Status { Halted, Stopped, StepLimit } status = StepLimit;
  uint64_t steps = 0;
  int pc = 0;
};
inline HookRun run_with_hook(const mm::MProg& p, mm::Regs& regs, uint64_t max_steps,
                             const std::function<bool(int pc, uint64_t steps)>& hook) {
  HookRun out;
  int pc = 0;
  for (;;) {
    if (out.steps == max_steps) {
      out.status = HookRun::StepLimit;
      break;
    }
    if (hook && !hook(pc, out.steps)) {
      out.status = HookRun::Stopped;
      break;
    }
    ++out.steps;
    const mm::Instr& in = p[pc];
    if (in.op == mm::Instr::HALT) {
      out.status = HookRun::Halted;
      break;
    }
    if (in.op == mm::Instr::INC) {
      ++regs[in.r];
      pc = in.k;
    } else if (regs[in.r] == 0) {
      pc = in.l;
    } else {
      --regs[in.r];
      pc = in.k;
    }
  }
  out.pc = pc;
  return out;
}

}  // namespace bits
