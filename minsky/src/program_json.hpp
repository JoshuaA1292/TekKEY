// A1: the CompiledProgram format (CONTRACT.md §2.2) and the files that travel with it:
// instruments.json (§1) and the envelope (BOUNDED_KEYS.md §5.1). One in-memory node table shared
// by verify, replay and venued, so all three deploy exactly the same orders.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "fastbook.hpp"
#include "json.hpp"
#include "proto.hpp"

namespace bk {

inline constexpr int kContractVersion = 1;
inline constexpr size_t kMaxNodes = 64;
inline constexpr size_t kMaxInstruments = 3;

struct Invalid : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ---- instruments.json ----
struct Instrument {
  std::string name;
  int64_t tick_cents = 100;
  std::string lot_desc;
  uint32_t book = 0;  // the live exchange's book for this instrument: its position in the file
};
struct Instruments {
  std::vector<Instrument> list;
  const Instrument* find(const std::string& n) const {
    for (const auto& i : list)
      if (i.name == n) return &i;
    return nullptr;
  }
  const Instrument& at(const std::string& n) const {
    const Instrument* i = find(n);
    if (!i) throw Invalid("unknown instrument \"" + n + "\"");
    return *i;
  }
};
Instruments load_instruments(const json::Value& v);

// ---- fees ----
// fee per fill = ceil(price_cents * qty * bps / 10000) (CONTRACT.md §1). bps is carried in
// hundredths so 3.5 bps is exact: fee = ceil(price_cents * qty * bps100 / 1_000_000).
struct Fees {
  int64_t maker_bps100 = 100;
  int64_t taker_bps100 = 350;
  int64_t fee(int64_t price_ticks, int64_t qty, int64_t tick_cents, bool maker) const {
    const int64_t bps = maker ? maker_bps100 : taker_bps100;
    __int128 n = static_cast<__int128>(price_ticks < 0 ? -price_ticks : price_ticks) * tick_cents * qty * bps;
    return static_cast<int64_t>((n + 999999) / 1000000);
  }
};
int64_t bps100(double bps);

// ---- envelope ----
struct Envelope {
  int64_t account = 0;
  int64_t max_loss_cents = 0;
  int64_t max_fees_cents = 0;
  int64_t max_trades = 0;
  int64_t max_exposure_lots = 0;
  std::vector<std::string> instruments;
  bool require_live_stop = false;
  int64_t max_gap_ticks = 0;
  std::string fill = "full_at_print";
  double maker_bps = 1.0, taker_bps = 3.5;
  Fees fees;
  std::string trigger_ref = "last";
  json::Value raw;
};
Envelope load_envelope(const json::Value& v);

// ---- CompiledProgram ----
enum class Kind : uint8_t { Limit, StopLimit, StopMarket, Halt };
const char* kind_name(Kind k);

struct Node {
  std::string id;
  Kind kind = Kind::Limit;
  std::string instrument;
  uint8_t side = proto::Buy;
  int64_t qty = 0;
  std::optional<int64_t> price;    // limit price (none for stop_market and halt)
  std::optional<int64_t> trigger;  // stops only
  bool reduce_only = false;
  std::string parent;              // "" = none
  std::string oco;                 // "" = none
  bool terminal = false;
  // resolved by load_program
  int index = 0;
  int parent_index = -1;
  int instrument_index = 0;  // position in Program::instruments
  int group_index = -1;      // position in Program::groups
  bool is_stop() const { return kind == Kind::StopLimit || kind == Kind::StopMarket; }
  bool native() const { return kind != Kind::Halt; }
};

struct Report {
  int64_t nodes_total = 0, native = 0, relayed = 0, locked_collateral_cents = 0;
};

struct Program {
  std::string backend = "minsky";
  std::string source_hash;
  std::vector<std::string> instruments;
  std::vector<Node> nodes;
  std::vector<std::vector<std::string>> deploy_plan;
  Report report;
  std::vector<std::string> warnings;
  std::vector<std::string> groups;  // OCO labels, in order of first appearance
  int find(const std::string& id) const {
    for (const auto& n : nodes)
      if (n.id == id) return n.index;
    return -1;
  }
};

// Parses and validates (BUILD_A_VENUE.md §3 A1): kinds and sides are known; parents exist and
// appear in an earlier deploy generation; every native node is deployed exactly once; OCO groups
// have >= 2 members; prices and quantities are within fb::kMaxPrice / fb::kMaxQty; <= 64 nodes;
// <= 3 instruments, each in `ins`. Throws Invalid with the reason.
Program load_program(const json::Value& v, const Instruments& ins);
json::Value to_json(const Program& p);

// The exchange order for a node: `parent_id` is the parent's live order id (0 = none) and
// `oco_id` the venue-wide id of its OCO group (0 = none). Groups always use OcoReduce.
proto::NewOrderMsg order_for(const Node& n, uint8_t account, uint32_t book, fb::OrderId parent_id, uint32_t oco_id,
                             uint32_t tag);

// locked collateral: notional of the limit orders that can open a position (stops lock nothing
// until they trigger; reduce-only orders only close).
int64_t locked_collateral_cents(const Program& p, const Instruments& ins);

// instruments.json for a tool that was not given --instruments: $BK_INSTRUMENTS, then
// ./instruments.json, ./fixtures/instruments.json, ../fixtures/instruments.json.
Instruments find_instruments(const std::string& explicit_path);

std::string read_file(const std::string& path);
json::Value read_json(const std::string& path);

}  // namespace bk
