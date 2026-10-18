// A4: run a path (a counterexample or a worst_path) on an exchange, through the same messages the
// verifier used (env.hpp), and measure the program from the exchange's own fill journal.
// The exchange can be the live one over TCP or an in-process Matcher (ex::InProcChannel): the
// replayer only sees a WirePort, so the two runs are message-for-message identical.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "env.hpp"
#include "program_json.hpp"
#include "verifier.hpp"

namespace bk {

struct ReplayOptions {
  uint8_t account = kReplayAccount;  // the program's sub-account
  std::vector<uint32_t> books;       // per program instrument
  bool deploy = true;                // false: attach to a program venued already deployed
  int64_t journal_base = -1;         // first journal index of this program (-1: count at start)
  uint32_t oco_base = kReplayOcoBase;
  Fees fees;
  bool mark_mode = false;
  EnvAccounts env{0, 254};
};

struct ReplayFill {
  int slot;  // node index, or nodes + 2*inst (+1) for the flatten pair
  int64_t px, q, fee;
  bool maker;
};

class Replayer {
 public:
  Replayer(WirePort& port, const Program& p, const Instruments& ins, ReplayOptions opt);

  // The market before the program: one print per instrument at its mark.
  void open(const std::vector<int64_t>& marks);
  void deploy();
  void apply(const PathMove& m);
  // Runs a whole path: open at the path's first prints, deploy, then every move. Key-holder moves
  // are skipped when attached (they belong to venued); the count is returned.
  int run(const std::vector<PathMove>& moves);

  // Reads the program's fills from the journal (from journal_base) into a fresh ledger.
  Ledger ledger();
  const std::vector<ReplayFill>& fills() const { return fills_; }
  int64_t loss();  // at the last prints
  std::vector<std::optional<int64_t>> marks();
  // Cancels everything the program still has and closes its position against the environment, so
  // the scratch account and book can be used again.
  void cleanup();
  const std::vector<fb::OrderId>& ids() const { return ids_; }

 private:
  int slot_of(fb::OrderId id) const;
  uint8_t side_of(int slot) const;

  WirePort& port_;
  const Program& p_;
  ReplayOptions opt_;
  size_t n_, k_;
  std::vector<fb::OrderId> ids_;
  std::vector<uint8_t> flatten_side_;
  std::vector<int64_t> ticks_;
  std::vector<ReplayFill> fills_;
  uint32_t oco_next_;
};

// A book on the exchange no one has used (no last print, nothing resting, the scratch account
// flat), at or after `from`. Throws if none.
uint32_t fresh_book(WirePort& port, uint8_t account, uint32_t from, uint32_t to);

}  // namespace bk
