// Minsky (counter) machines: a reference interpreter, and a compiler to order
// programs that follows Weinberger (arXiv 2608.28803) Table 2, plus three variants that
// test how little the construction really needs (see CompileOpts).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine.hpp"

namespace mm {

struct Instr {
  enum Op : uint8_t { INC, DEC, HALT } op = HALT;
  int r = 0;  // register
  int k = 0;  // INC: next. DEC: next if register was non-zero (after decrementing)
  int l = 0;  // DEC: next if register was zero
};
using MProg = std::vector<Instr>;
using Regs = std::vector<int64_t>;

// Standard: DECJZ(r,k,l) jumps to l if r==0, else decrements and goes to k. Table 2's gadget
//           and the paper's own Table 3 program both assume this.
// AppendixC: the paper's Appendix C wording: "decrements R_i by one and ... I_k unless the
//           register subsequently holds zero, in which case ... I_l" (saturating at 0).
enum class Sem { Standard, AppendixC };

struct Run {
  enum Status { Halted, StepLimit, Stalled } status = Stalled;
  uint64_t steps = 0;  // instructions entered (HALT included)
  Regs regs;
  int pc = 0;
  uint64_t trace_hash = 0;  // over the (pc, regs) sequence seen at each instruction entry
  uint64_t dec_nonzero = 0, dec_zero = 0, distinct_pcs = 0;  // reference only: coverage
  std::vector<std::pair<int, Regs>> trace;
};
const char* status_name(Run::Status s);

Run run_reference(const MProg& p, Regs regs, uint64_t max_steps, Sem sem = Sem::Standard,
                  bool keep_trace = false);

// ---- compiler ---------------------------------------------------------------
struct CompileOpts {
  // Paper text for INC: "then place order k when order is executed". true = continue only
  // when the INC's resting sell fills (literal reading). false = continue on acceptance.
  bool inc_waits_for_fill = false;
  // Satisfy the strict reading with fill-only links: INC also crosses one lot with itself in a
  // separate clock book (seller account vs buyer account) and continues on that fill.
  bool inc_clock = false;
  // Paper DECJZ submits a fresh backstop sell at P+tick every time and never removes it on the
  // non-zero branch. true = cancel that backstop on the non-zero branch.
  bool gc_backstop = false;
  // false: every router order comes from one account (as in the paper).
  // true: register/backstop sells from account 0, DEC IOC buys from account 1.
  bool two_accounts = false;

  // --- variants added in the stress round ---
  // One backstop per register, placed in the initial set and re-submitted (1 lot) by the zero
  // branch's own fill link. No cancel needed anywhere; garbage is exactly nregs lots.
  bool persistent_backstop = false;
  // No price-keyed links at all. After the probe prints, the router submits two stop-limit
  // orders in the register's book whose triggers straddle the two possible prints; the
  // exchange fires exactly one, and that order's (identity-keyed) fill link continues.
  // Implies persistent_backstop (2 lots), inc_clock, two_accounts.
  bool stop_branch = false;
  // nregs must be 2: register 0 is resting ask quantity at P, register 1 is resting bid quantity
  // at P-10, both in book 0. The clock trade also happens in book 0, at P-5.
  bool single_book = false;

  ob::Price P = 1000;  // register price level P_i (same value in each register's own book)
};

// Where a register lives and the prices its gadgets use.
struct Geo {
  int book = 0;
  ob::Side side = ob::Side::Sell;  // side of the register's resting orders
  ob::Price reg = 0;     // register level
  ob::Price back = 0;    // backstop level (= probe limit)
  ob::Price signal = 0;  // stop_branch: level of the transient order the non-zero stop trades with
  int maker = 0, taker = 0;  // accounts: resting side / aggressive side
};

struct Compiled {
  ob::Program prog;
  std::vector<int> entry_instr;  // template id -> instruction whose entry it marks, else -1
  int nregs = 0;
  CompileOpts opts;
  std::vector<Geo> geo;  // per register
  int extra_books = 0;   // books after the register (and clock) books, for third-party noise
  int clock_book = -1;
  ob::Price clock_px = 0;
  int reg_books = 0;     // books holding registers (nregs, or 1 if single_book)
  ob::Qty fixed_lots = 0;  // lots the construction keeps resting besides register content
  int nbooks() const { return reg_books + (clock_book >= 0 && clock_book >= reg_books) + extra_books; }
};

Compiled compile(const MProg& p, int nregs, const CompileOpts& o, int extra_books = 0);

struct BookRun {
  Run run;
  uint64_t engine_work = 0;
  size_t resting_orders = 0;
  size_t trades = 0;
  ob::Qty garbage = 0;  // resting lots in register books that are not register content
  ob::Qty peak_book_lots = 0;  // max over register books of the most lots ever resting at once
};

// Optional hook called between router work items (for third-party interference).
using Interleave = std::function<void(ob::Engine&, uint64_t work_index)>;

// Load registers as resting orders, start the program and run it to halt / stall / limit.
// Registers are read back from the books (resting quantity at the register level), never from
// shadow state. read_regs=false skips the per-instruction register read (trace_hash then covers
// only the pc sequence) for very large runs.
BookRun run_on_book(const Compiled& c, const Regs& regs, uint64_t max_steps, bool keep_trace,
                    ob::STP stp = ob::STP::None, const Interleave& noise = {},
                    bool read_regs = true);

// ---- program construction ---------------------------------------------------
// Assembler with labels. Targets are label ids, resolved by finish().
class Asm {
 public:
  int label() {
    addr_.push_back(-1);
    return static_cast<int>(addr_.size()) - 1;
  }
  void bind(int lab) { addr_[lab] = static_cast<int>(code_.size()); }
  int here_label() {
    int l = label();
    bind(l);
    return l;
  }
  void inc(int r, int next) { code_.push_back({Instr::INC, r, next, 0}); }
  void dec(int r, int nz, int z) { code_.push_back({Instr::DEC, r, nz, z}); }
  void halt() { code_.push_back({Instr::HALT, 0, 0, 0}); }
  // inc then fall through
  void inc_ff(int r) {
    int n = label();
    inc(r, n);
    bind(n);
  }
  size_t size() const { return code_.size(); }
  MProg finish() const;

 private:
  MProg code_;
  std::vector<int> addr_;
};

// Weinberger Table 3: 2N into R2 (1-indexed in the paper; here R1->0, R2->1, instr i -> i-1).
MProg paper_table3();
MProg prog_add();      // r0 + r1 -> r2
MProg prog_absdiff();  // |r0 - r1| -> r2
MProg prog_mul();      // r0 * r1 -> r2 (uses r3)
MProg prog_divmod();   // r0 / r1 -> r2, r0 % r1 -> r3 (r1 > 0; uses r4)

// Minsky's reduction: any k-register machine -> 2-register machine, with
// R0 = prod p_i^{r_i} (p = 2,3,5,7,...), R1 scratch.
MProg to_two_counter(const MProg& p, int k);
int64_t godel_encode(const Regs& r);
Regs godel_decode(int64_t a, int k);

// Turing machine (2 symbols) -> 3-register machine. L, R hold the tape left/right of the head
// in binary (nearest cell = least significant bit); R's lowest bit is the cell under the head.
struct TM {
  // delta[q][s] = {write, move(+1 right/-1 left), next}; next == -1 is the halt state
  struct Rule {
    int write, move, next;
  };
  std::vector<std::array<Rule, 2>> delta;
};
MProg tm_to_counter(const TM& tm);
struct TMResult {
  uint64_t steps;
  std::vector<int> tape;  // dense, from leftmost visited cell
  int head;               // index into tape
  int ones;
};
TMResult run_tm(const TM& tm, uint64_t max_steps);

// Space-bounded Turing machine -> n one-bit registers (tape cell i = register i, value 0/1).
// The head position is unrolled into the control state, so the program has O(|Q| * n)
// instructions and every register stays in {0, 1}. Instruction index halt_ok() is reached
// when the TM halts; halt_out() if the head leaves cells [0, n).
struct BitProg {
  MProg prog;
  int halt_ok = -1, halt_out = -1;
};
BitProg tm_to_bits(const TM& tm, int n, int head0);
struct TMBounded {
  enum Status { Halted, OutOfTape, StepLimit } status;
  uint64_t steps;
  std::vector<int> tape;  // n cells
  int head;
  int min_head, max_head;  // extent visited (unbounded run)
};
TMBounded run_tm_bounded(const TM& tm, int n, int head0, uint64_t max_steps);

}  // namespace mm
