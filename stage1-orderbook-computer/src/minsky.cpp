#include "minsky.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace mm {

namespace {
struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void add(int64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<uint8_t>(v >> (8 * i));
      h *= 1099511628211ull;
    }
  }
};
void hash_entry(Fnv& f, int pc, const Regs& r) {
  f.add(pc);
  for (int64_t v : r) f.add(v);
}
}  // namespace

const char* status_name(Run::Status s) {
  switch (s) {
    case Run::Halted: return "halted";
    case Run::StepLimit: return "step-limit";
    case Run::Stalled: return "STALLED";
  }
  return "?";
}

Run run_reference(const MProg& p, Regs regs, uint64_t max_steps, Sem sem, bool keep_trace) {
  Run out;
  Fnv f;
  int pc = 0;
  std::vector<bool> seen(p.size(), false);
  for (;;) {
    if (out.steps == max_steps) {
      out.status = Run::StepLimit;
      break;
    }
    if (pc < 0 || pc >= static_cast<int>(p.size())) throw std::out_of_range("pc");
    hash_entry(f, pc, regs);
    if (keep_trace) out.trace.emplace_back(pc, regs);
    ++out.steps;
    if (!seen[pc]) seen[pc] = true, ++out.distinct_pcs;
    const Instr& in = p[pc];
    if (in.op == Instr::DEC) ++(regs[in.r] ? out.dec_nonzero : out.dec_zero);
    if (in.op == Instr::HALT) {
      out.status = Run::Halted;
      break;
    }
    if (in.op == Instr::INC) {
      ++regs[in.r];
      pc = in.k;
    } else if (sem == Sem::Standard) {
      if (regs[in.r] == 0) {
        pc = in.l;
      } else {
        --regs[in.r];
        pc = in.k;
      }
    } else {
      if (regs[in.r] > 0) --regs[in.r];
      pc = regs[in.r] == 0 ? in.l : in.k;
    }
  }
  out.pc = pc;
  out.regs = std::move(regs);
  out.trace_hash = f.h;
  return out;
}

// ---- compiler: Weinberger Table 2 and variants ---------------------------------
Compiled compile(const MProg& p, int nregs, const CompileOpts& o_in, int extra_books) {
  using namespace ob;
  CompileOpts o = o_in;
  if (o.stop_branch) o.persistent_backstop = o.inc_clock = o.two_accounts = true;
  if (o.single_book && nregs != 2) throw std::invalid_argument("single_book needs exactly 2 registers");
  Compiled c;
  c.nregs = nregs;
  c.opts = o;
  c.extra_books = extra_books;
  const int n = static_cast<int>(p.size());
  const int a0 = 0, a1 = o.two_accounts ? 1 : 0;
  // Backstop distance from the register level. stop_branch needs a free tick in between so the
  // two prints (reg, back) can be told apart by two stop triggers (<= reg, >= back).
  const Price d = o.stop_branch ? 2 : 1;

  // Geometry.
  c.geo.resize(nregs);
  if (o.single_book) {
    c.reg_books = 1;
    c.geo[0] = {0, Side::Sell, o.P, o.P + d, o.P - 1, a0, a1};
    c.geo[1] = {0, Side::Buy, o.P - 10, o.P - 10 - d, o.P - 10 + 1, a1, a0};
    c.clock_book = 0;
    c.clock_px = o.P - 5;
  } else {
    c.reg_books = nregs;
    for (int r = 0; r < nregs; ++r) c.geo[r] = {r, Side::Sell, o.P, o.P + d, o.P - 1, a0, a1};
    if (o.inc_clock) {
      c.clock_book = nregs;
      c.clock_px = o.P;
    }
  }
  const Qty back_qty = o.stop_branch ? 2 : 1;
  if (o.persistent_backstop) c.fixed_lots = back_qty * nregs;

  auto add = [&](const Template& t, int instr_entry) {
    c.prog.templates.push_back(t);
    c.entry_instr.push_back(instr_entry);
    return static_cast<int>(c.prog.templates.size()) - 1;
  };
  auto other = [](Side s) { return s == Side::Sell ? Side::Buy : Side::Sell; };

  // Per-register persistent backstops.
  std::vector<int> BK(nregs, -1);
  if (o.persistent_backstop)
    for (int r = 0; r < nregs; ++r) {
      const Geo& g = c.geo[r];
      BK[r] = add({g.maker, g.book, g.side, Type::Limit, g.back, 0, back_qty, false}, -1);
      c.prog.initial.push_back({Act::Submit, BK[r]});
    }

  // Templates first, so every instruction's entry action list is known.
  std::vector<int> S(n, -1), B(n, -1), D(n, -1), SIG(n, -1), SS(n, -1), SZ(n, -1), CB(n, -1);
  std::vector<std::vector<Action>> entry(n);
  for (int j = 0; j < n; ++j) {
    const Instr& in = p[j];
    if (in.op != Instr::HALT && (in.r < 0 || in.r >= nregs)) throw std::out_of_range("register");
    const Geo& g = c.geo[in.op == Instr::HALT ? 0 : in.r];
    switch (in.op) {
      case Instr::INC:
        // "Place a resting limit SELL order for one unit of security i at price P_i"
        S[j] = add({g.maker, g.book, g.side, Type::Limit, g.reg, 0, 1, false}, j);
        entry[j] = {{Act::Submit, S[j]}};
        if (o.inc_clock) {
          int cs = add({a0, c.clock_book, Side::Sell, Type::Limit, c.clock_px, 0, 1, false}, -1);
          CB[j] = add({a1, c.clock_book, Side::Buy, Type::IOC, c.clock_px, 0, 1, false}, -1);
          entry[j].push_back({Act::Submit, cs});
          entry[j].push_back({Act::Submit, CB[j]});
        }
        break;
      case Instr::DEC:
        // "Place a resting limit SELL ... at price P_i + theta, then place an IOC BUY ... also
        //  at price P_i + theta"
        if (!o.persistent_backstop) {
          B[j] = add({g.maker, g.book, g.side, Type::Limit, g.back, 0, 1, false}, j);
          entry[j].push_back({Act::Submit, B[j]});
        }
        D[j] = add({g.taker, g.book, other(g.side), Type::IOC, g.back, 0, 1, false}, B[j] < 0 ? j : -1);
        entry[j].push_back({Act::Submit, D[j]});
        if (o.stop_branch) {
          // Transient counterparty for the non-zero stop, one tick inside the register level.
          SIG[j] = add({g.taker, g.book, other(g.side), Type::Limit, g.signal, 0, 1, false}, -1);
          // Non-zero stop: same side as the register, fires when last reaches the register
          // level (sell stop: last <= reg; buy stop: last >= reg), trades at the signal level.
          SS[j] = add({g.maker, g.book, g.side, Type::StopLimit, g.signal, g.reg, 1, false}, -1);
          // Zero stop: opposite side, fires when last reaches the backstop level, trades there.
          SZ[j] = add({g.taker, g.book, other(g.side), Type::StopLimit, g.back, g.back, 1, false}, -1);
          entry[j].push_back({Act::Submit, SIG[j]});
          entry[j].push_back({Act::Submit, SS[j]});
          entry[j].push_back({Act::Submit, SZ[j]});
        }
        break;
      case Instr::HALT:
        entry[j] = {{Act::Halt, j}};
        break;
    }
  }
  for (int j = 0; j < n; ++j) {
    const Instr& in = p[j];
    if (in.op == Instr::INC && o.inc_clock) {
      c.prog.links.push_back({On::Fill, CB[j], o.stop_branch ? std::nullopt : std::optional<Price>(c.clock_px),
                              entry[in.k]});
    } else if (in.op == Instr::INC) {
      // "then place order k when order is executed"
      c.prog.links.push_back(
          {o.inc_waits_for_fill ? On::Fill : On::Accept, S[j], std::nullopt, entry[in.k]});
    } else if (in.op == Instr::DEC && o.stop_branch) {
      std::vector<Action> nz = {{Act::Cancel, SZ[j]}};
      nz.insert(nz.end(), entry[in.k].begin(), entry[in.k].end());
      std::vector<Action> z = {{Act::Cancel, SS[j]}, {Act::Cancel, SIG[j]}, {Act::Submit, BK[in.r]}};
      z.insert(z.end(), entry[in.l].begin(), entry[in.l].end());
      c.prog.links.push_back({On::Fill, SS[j], std::nullopt, nz});
      c.prog.links.push_back({On::Fill, SZ[j], std::nullopt, z});
    } else if (in.op == Instr::DEC) {
      // "If order is executed at price P_i, place order I_k. If ... at P_i + theta, place I_l."
      const Geo& g = c.geo[in.r];
      std::vector<Action> nz, z;
      if (o.gc_backstop && B[j] >= 0) nz.push_back({Act::Cancel, B[j]});
      nz.insert(nz.end(), entry[in.k].begin(), entry[in.k].end());
      if (o.persistent_backstop) z.push_back({Act::Submit, BK[in.r]});
      z.insert(z.end(), entry[in.l].begin(), entry[in.l].end());
      c.prog.links.push_back({On::Fill, D[j], g.reg, nz});
      c.prog.links.push_back({On::Fill, D[j], g.back, z});
    }
  }
  c.prog.initial.insert(c.prog.initial.end(), entry[0].begin(), entry[0].end());
  return c;
}

BookRun run_on_book(const Compiled& c, const Regs& regs, uint64_t max_steps, bool keep_trace,
                    ob::STP stp, const Interleave& noise, bool read_regs) {
  using namespace ob;
  BookRun out;
  Engine eng(c.nbooks(), 3, stp, &c.prog);
  eng.log_trades = false;
  for (int r = 0; r < c.nregs; ++r) {
    const Geo& g = c.geo[r];
    if (regs.at(r) > 0) eng.submit({g.maker, g.book, g.side, Type::Limit, g.reg, 0, regs[r], false});
  }

  auto read = [&] {
    Regs v(c.nregs);
    for (int r = 0; r < c.nregs; ++r) v[r] = eng.resting_qty(c.geo[r].book, c.geo[r].side, c.geo[r].reg);
    return v;
  };
  Fnv f;
  Run& run = out.run;
  bool halted = false;
  eng.on_action = [&](const Action& a) {
    int j = -1;
    if (a.kind == Act::Submit) j = c.entry_instr[a.arg];
    if (a.kind == Act::Halt) j = a.arg;
    if (j < 0) return;
    if (run.steps == max_steps) {
      eng.stop_requested = true;
      return;
    }
    if (read_regs) {
      Regs v = read();
      hash_entry(f, j, v);
      if (keep_trace) run.trace.emplace_back(j, v);
    } else {
      f.add(j);
    }
    ++run.steps;
    run.pc = j;
    if (a.kind == Act::Halt) halted = true;
  };
  eng.start();
  Status st;
  uint64_t idx = 0;
  for (;;) {
    if (noise) noise(eng, idx++);
    st = eng.run(noise ? 1 : UINT64_MAX);
    if (st != Status::Budget) break;
  }
  run.status = st == Status::Halted && halted ? Run::Halted
               : st == Status::Stopped        ? Run::StepLimit
                                              : Run::Stalled;
  run.regs = read();
  run.trace_hash = f.h;
  out.engine_work = eng.work_done();
  out.resting_orders = eng.resting_orders();
  out.trades = eng.trade_count();
  Qty lots = 0, regsum = 0;
  for (int b = 0; b < c.reg_books; ++b) {
    lots += eng.resting_lots(b);
    out.peak_book_lots = std::max(out.peak_book_lots, eng.peak_lots(b));
  }
  for (Qty v : run.regs) regsum += v;
  out.garbage = lots - regsum;
  return out;
}

// ---- assembler & programs ---------------------------------------------------
MProg Asm::finish() const {
  MProg p = code_;
  auto res = [&](int lab) {
    if (lab < 0 || lab >= static_cast<int>(addr_.size()) || addr_[lab] < 0)
      throw std::logic_error("unbound label");
    return addr_[lab];
  };
  for (auto& in : p) {
    if (in.op == Instr::INC) in.k = res(in.k);
    if (in.op == Instr::DEC) {
      in.k = res(in.k);
      in.l = res(in.l);
    }
  }
  return p;
}

MProg paper_table3() {
  // 1 DECJZ(R2,1,2)  2 DECJZ(R1,3,5)  3 INC(R2,4)  4 INC(R2,2)  5 HALT
  return {{Instr::DEC, 1, 0, 1}, {Instr::DEC, 0, 2, 4}, {Instr::INC, 1, 3, 0},
          {Instr::INC, 1, 1, 0}, {Instr::HALT, 0, 0, 0}};
}

MProg prog_add() {
  Asm a;
  int L0 = a.label(), A1 = a.label(), L1 = a.label(), B1 = a.label(), H = a.label();
  a.bind(L0); a.dec(0, A1, L1);
  a.bind(A1); a.inc(2, L0);
  a.bind(L1); a.dec(1, B1, H);
  a.bind(B1); a.inc(2, L1);
  a.bind(H);  a.halt();
  return a.finish();
}

MProg prog_absdiff() {
  Asm a;
  int LOOP = a.label(), T = a.label(), REST = a.label(), MV0 = a.label(), MV0b = a.label(),
      MV1 = a.label(), MV1b = a.label(), H = a.label();
  a.bind(LOOP); a.dec(0, T, MV1);      // r0 exhausted first -> answer is what is left of r1
  a.bind(T);    a.dec(1, LOOP, REST);  // r1 exhausted first -> r0 had one more, put it back
  a.bind(REST); a.inc(2, MV0);
  a.bind(MV0);  a.dec(0, MV0b, H);
  a.bind(MV0b); a.inc(2, MV0);
  a.bind(MV1);  a.dec(1, MV1b, H);
  a.bind(MV1b); a.inc(2, MV1);
  a.bind(H);    a.halt();
  return a.finish();
}

MProg prog_mul() {
  Asm a;
  int OUT = a.label(), IN = a.label(), I1 = a.label(), I2 = a.label(), RS = a.label(),
      RS1 = a.label(), H = a.label();
  a.bind(OUT); a.dec(0, IN, H);
  a.bind(IN);  a.dec(1, I1, RS);
  a.bind(I1);  a.inc(2, I2);
  a.bind(I2);  a.inc(3, IN);
  a.bind(RS);  a.dec(3, RS1, OUT);
  a.bind(RS1); a.inc(1, RS);
  a.bind(H);   a.halt();
  return a.finish();
}

MProg prog_divmod() {
  Asm a;
  int ROUND = a.label(), S = a.label(), S2 = a.label(), FULL = a.label(), MV = a.label(),
      MV1 = a.label(), PART = a.label(), P1 = a.label(), H = a.label();
  a.bind(ROUND); a.dec(1, S, FULL);
  a.bind(S);     a.dec(0, S2, PART);
  a.bind(S2);    a.inc(4, ROUND);
  a.bind(FULL);  a.inc(2, MV);
  a.bind(MV);    a.dec(4, MV1, ROUND);
  a.bind(MV1);   a.inc(1, MV);
  a.bind(PART);  a.dec(4, P1, H);
  a.bind(P1);    a.inc(3, PART);
  a.bind(H);     a.halt();
  return a.finish();
}

// ---- Minsky's two-counter reduction -----------------------------------------
static const int64_t kPrimes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};

MProg to_two_counter(const MProg& p, int k) {
  if (k > 10) throw std::out_of_range("too many registers");
  const int A = 0, B = 1;
  Asm a;
  std::vector<int> start(p.size());
  for (auto& s : start) s = a.label();
  for (size_t j = 0; j < p.size(); ++j) {
    a.bind(start[j]);
    const Instr& in = p[j];
    const int q = static_cast<int>(kPrimes[in.r]);
    if (in.op == Instr::HALT) {
      a.halt();
    } else if (in.op == Instr::INC) {
      // A := A * q   (A -> B times q, then B -> A)
      int L1 = a.here_label(), BODY = a.label(), L2 = a.label(), M = a.label();
      a.dec(A, BODY, L2);
      a.bind(BODY);
      for (int i = 0; i < q - 1; ++i) a.inc_ff(B);
      a.inc(B, L1);
      a.bind(L2);
      a.dec(B, M, start[in.k]);
      a.bind(M);
      a.inc(A, L2);
    } else {
      // Divide A by q into B, remainder in control state.
      std::vector<int> Dl(q), R(q);
      for (int i = 0; i < q; ++i) Dl[i] = a.label(), R[i] = a.label();
      int Q = a.label();
      for (int i = 0; i < q; ++i) {
        a.bind(Dl[i]);
        a.dec(A, i < q - 1 ? Dl[i + 1] : Q, R[i]);
      }
      a.bind(Q);
      a.inc(B, Dl[0]);
      // remainder 0: register was non-zero; A := A/q is the decrement. Move B back.
      a.bind(R[0]);
      {
        int M = a.here_label(), MB = a.label();
        a.dec(B, MB, start[in.k]);
        a.bind(MB);
        a.inc(A, M);
      }
      // remainder i > 0: register was zero; restore A := B*q + i and take the zero branch.
      for (int i = 1; i < q; ++i) {
        a.bind(R[i]);
        for (int t = 0; t < i; ++t) a.inc_ff(A);
        int N = a.here_label(), NB = a.label();
        a.dec(B, NB, start[in.l]);
        a.bind(NB);
        for (int t = 0; t < q - 1; ++t) a.inc_ff(A);
        a.inc(A, N);
      }
    }
  }
  return a.finish();
}

int64_t godel_encode(const Regs& r) {
  int64_t a = 1;
  for (size_t i = 0; i < r.size(); ++i)
    for (int64_t t = 0; t < r[i]; ++t) {
      if (a > INT64_MAX / kPrimes[i]) throw std::overflow_error("godel");
      a *= kPrimes[i];
    }
  return a;
}

Regs godel_decode(int64_t a, int k) {
  Regs r(k, 0);
  for (int i = 0; i < k; ++i)
    while (a % kPrimes[i] == 0) a /= kPrimes[i], ++r[i];
  return r;
}

// ---- Turing machine -> 3 counters -------------------------------------------
MProg tm_to_counter(const TM& tm) {
  const int L = 0, R = 1, T = 2;
  Asm a;
  const int nq = static_cast<int>(tm.delta.size());
  std::vector<int> READ(nq);
  for (auto& x : READ) x = a.label();
  int HL = a.label();

  // X := 2X + bit, then go to cont
  auto double_plus = [&](int X, int bit, int cont) {
    int D = a.here_label(), DB = a.label(), DM = a.label(), MB = a.label();
    int after = bit ? a.label() : cont;
    a.dec(X, DB, DM);
    a.bind(DB); a.inc_ff(T); a.inc(T, D);
    a.bind(DM); a.dec(T, MB, after);
    a.bind(MB); a.inc(X, DM);
    if (bit) { a.bind(after); a.inc(X, cont); }
  };
  // X := X / 2, branch on X % 2
  auto divmod2 = [&](int X, int cont0, int cont1) {
    int RD = a.here_label(), RD2 = a.label(), RDQ = a.label(), Z0 = a.label(), Z1 = a.label(),
        M0 = a.label(), M1 = a.label();
    a.dec(X, RD2, Z0);
    a.bind(RD2); a.dec(X, RDQ, Z1);
    a.bind(RDQ); a.inc(T, RD);
    a.bind(Z0);  a.dec(T, M0, cont0);
    a.bind(M0);  a.inc(X, Z0);
    a.bind(Z1);  a.dec(T, M1, cont1);
    a.bind(M1);  a.inc(X, Z1);
  };
  for (int q = 0; q < nq; ++q) {
    int ACT[2] = {a.label(), a.label()};
    a.bind(READ[q]);
    divmod2(R, ACT[0], ACT[1]);  // pop the head cell into the control state
    for (int s = 0; s < 2; ++s) {
      const auto& rule = tm.delta[q][s];
      int target = rule.next < 0 ? HL : READ[rule.next];
      a.bind(ACT[s]);
      if (rule.move > 0) {
        double_plus(L, rule.write, target);  // written cell is now left of the head
      } else {
        int POP = a.label(), P0 = a.label(), P1 = a.label();
        double_plus(R, rule.write, POP);  // written cell is now right of the head
        a.bind(POP);
        divmod2(L, P0, P1);  // new head cell comes off L ...
        a.bind(P0);
        double_plus(R, 0, target);  // ... and goes onto R
        a.bind(P1);
        double_plus(R, 1, target);
      }
    }
  }
  a.bind(HL);
  a.halt();
  return a.finish();
}

TMResult run_tm(const TM& tm, uint64_t max_steps) {
  std::map<int, int> tape;
  int head = 0, q = 0;
  uint64_t steps = 0;
  while (q >= 0 && steps < max_steps) {
    int s = tape[head];
    const auto& r = tm.delta[q][s];
    tape[head] = r.write;
    head += r.move;
    q = r.next;
    ++steps;
  }
  tape[head];  // make sure the head cell is in range
  TMResult out{steps, {}, 0, 0};
  int lo = tape.begin()->first;
  for (auto& [pos, v] : tape) {
    while (lo + static_cast<int>(out.tape.size()) < pos) out.tape.push_back(0);
    out.tape.push_back(v);
    out.ones += v;
  }
  out.head = head - lo;
  return out;
}

// ---- space-bounded Turing machine -> one bit per register ----------------------
BitProg tm_to_bits(const TM& tm, int n, int head0) {
  if (n <= 0 || head0 < 0 || head0 >= n) throw std::invalid_argument("tape");
  Asm a;
  const int nq = static_cast<int>(tm.delta.size());
  // READ[q][i]: control state q with the head on cell i. One DECJZ reads cell i destructively
  // (a 1 becomes 0), so when the rule writes 0 the branch jumps straight to the next
  // (state, cell); when it writes 1 the branch goes through one INC. No scratch register.
  std::vector<std::vector<int>> READ(nq, std::vector<int>(n));
  for (auto& row : READ)
    for (auto& x : row) x = a.label();
  int HOK = a.label(), HOUT = a.label();
  auto target = [&](int q, int i) {
    if (q < 0) return HOK;
    if (i < 0 || i >= n) return HOUT;
    return READ[q][i];
  };
  auto emit = [&](int q, int i) {
    a.bind(READ[q][i]);
    int br[2];
    for (int s = 0; s < 2; ++s) {
      const auto& rule = tm.delta[q][s];
      int t = target(rule.next, i + rule.move);
      br[s] = rule.write ? a.label() : t;
    }
    a.dec(i, br[1], br[0]);
    for (int s = 0; s < 2; ++s)
      if (tm.delta[q][s].write) {
        a.bind(br[s]);
        a.inc(i, target(tm.delta[q][s].next, i + tm.delta[q][s].move));
      }
  };
  emit(0, head0);  // instruction 0 is the entry point
  for (int q = 0; q < nq; ++q)
    for (int i = 0; i < n; ++i)
      if (!(q == 0 && i == head0)) emit(q, i);
  BitProg out;
  a.bind(HOK);
  out.halt_ok = static_cast<int>(a.size());
  a.halt();
  a.bind(HOUT);
  out.halt_out = static_cast<int>(a.size());
  a.halt();
  out.prog = a.finish();
  return out;
}

TMBounded run_tm_bounded(const TM& tm, int n, int head0, uint64_t max_steps) {
  TMBounded out{TMBounded::StepLimit, 0, std::vector<int>(n, 0), head0, head0, head0};
  int q = 0;
  while (out.steps < max_steps) {
    if (q < 0) {
      out.status = TMBounded::Halted;
      return out;
    }
    if (out.head < 0 || out.head >= n) {
      out.status = TMBounded::OutOfTape;
      return out;
    }
    const auto& r = tm.delta[q][out.tape[out.head]];
    out.tape[out.head] = r.write;
    out.head += r.move;
    out.min_head = std::min(out.min_head, out.head);
    out.max_head = std::max(out.max_head, out.head);
    q = r.next;
    ++out.steps;
  }
  if (q < 0) out.status = TMBounded::Halted;
  else if (out.head < 0 || out.head >= n) out.status = TMBounded::OutOfTape;
  return out;
}

}  // namespace mm
