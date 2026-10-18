// The exchange's matching core: the same rules as stage 1's reference engine (price-time
// priority, limit / market / IOC / stop-market / stop-limit, self-trade prevention, reduce-only,
// stop cascades processed one at a time in trigger order) in a flat layout built for the wire:
//   * a preallocated order pool; order id = slot << 32 | generation, so cancel is an index
//   * per book, a dense window of price levels [base, base + width) with an occupancy bitmap,
//     so the best level is a ctz; prices outside the window fall back to ordered maps
//   * an intrusive doubly linked FIFO per level, so matching and cancel never allocate
//   * no hashing anywhere on the hot path
// Everything the core observes or changes is reported through a Sink, so the server can turn it
// into fill reports and feed events without the core knowing about sockets.
//
// Native chaining (v2, for compiled policies): an order may name a parent, in which case it is
// held and released only when the parent fills completely (cancelled if the parent goes away
// unfilled: cancel, self-trade expiry, an IOC remainder); and it may belong to a one-cancels-other
// group, in which case its fills cancel (OcoCancel, on the complete fill) or reduce (OcoReduce,
// by the filled quantity) the other members. Stops trigger on the last trade or, in mark mode, on
// a mark price the venue publishes. Stop firings, child releases and group effects are queued in
// event order and processed one at a time after the print that caused them, as stage 1 did for
// stops; so one sweep can fill two members of a group before either effect applies.
//
// Canary mutations (deliberate bugs behind flags, for the conformance experiment):
//   FifoAfterPartial  a partially filled resting order loses its place in the queue
//                     (orderbook-rs issue #88)
//   PriceInverted     the taker matches the worst eligible level first, not the best
//   StopWrongSide     a sell stop fires on last >= trigger (and a buy stop on <=)
//   IocRests          an IOC's unfilled remainder rests in the book
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "proto.hpp"

namespace fb {

using Price = int64_t;
using Qty = int64_t;
using OrderId = uint64_t;

enum Mutation : uint64_t {
  FifoAfterPartial = 1,
  PriceInverted = 2,
  StopWrongSide = 4,
  IocRests = 8,
};
const char* mutation_name(uint64_t bit);

enum class STP : uint8_t { None = 0, ExpireMaker = 1, CancelTaker = 2 };

// Prices and quantities from the wire are bounded so that q * px, level totals and cash never
// overflow int64 (2^31 * 2^31 = 2^62, summed over at most 2^31 lots per order).
inline constexpr int64_t kMaxPrice = int64_t{1} << 31;
inline constexpr int64_t kMaxQty = int64_t{1} << 31;

struct Config {
  uint32_t books = 4096;
  Price base = 968;      // first price in the dense window
  uint32_t width = 64;   // window levels (<= 256)
  uint8_t accounts = 4;
  STP stp = STP::ExpireMaker;
  uint64_t mutation = 0;
  uint32_t pool = 1 << 16;  // initial order pool
  uint8_t trigger_ref = 0;  // 0: stops trigger on the last trade; 1: on the mark price
};

enum class State : uint8_t { Free, Resting, Pending, Queued, Matching, Held };

struct Order {
  OrderId id = 0;
  uint32_t tag = 0;
  uint16_t session = 0;
  uint8_t account = 0;
  uint8_t side = proto::Buy;
  uint8_t type = proto::Limit;
  uint8_t reduce_only = 0;
  State state = State::Free;
  uint8_t pad = 0;
  uint32_t book = 0;
  Price price = 0;
  Price trigger = 0;
  Qty qty = 0;     // remaining
  Qty filled = 0;  // cumulative
  uint32_t next = 0, prev = 0;  // level FIFO (NIL = none); `next` doubles as the free list link
  OrderId parent = 0;   // held until this order fills completely
  uint32_t oco = 0;     // one-cancels-other group, 0 = none
  uint8_t oco_mode = 0; // proto::OcoMode: what this order's fills do to the other members
  Qty orig = 0;         // quantity submitted
};

struct Sink {
  virtual ~Sink() = default;
  virtual void fill(const Order& o, bool maker, Price px, Qty q) = 0;
  // A resting order removed without a fill (self-trade prevention, or a reduce-only order whose
  // position vanished). The reference engine drops these silently; a wire owner must be told.
  virtual void cancelled(const Order& o) = 0;
  virtual void trade(uint32_t book, Price px, Qty q, uint8_t buyer, uint8_t seller) = 0;
  virtual void level(uint32_t book, uint8_t side, Price px, Qty total, uint32_t count) = 0;
  virtual void reset(uint32_t book) = 0;
  // Chaining events (v2). Defaults do nothing so the stage 1 comparison harness is untouched.
  virtual void activated(const Order&) {}                       // a held child was released
  virtual void group_cancelled(const Order&, uint8_t /*why*/) {}  // proto::GroupCancelled / ParentCancelled
  virtual void group_reduced(const Order&, Qty /*by*/) {}       // reduce mode; o.qty is the remainder
  virtual void finished(const Order&) {}                        // a fired stop / released child is done
};

struct Result {
  uint8_t status = proto::Rejected;
  OrderId id = 0;
  Qty remaining = 0;
};

class Core {
 public:
  static constexpr uint32_t NIL = 0xffffffffu;

  Core(const Config& cfg, Sink* sink);
  // A snapshot: an exact copy of `other` (books, pool, ids, queues, positions, cash) whose
  // observations go to `sink` instead. The verifier branches on these; nothing is shared.
  Core(const Core& other, Sink* sink) : Core(other) { sink_ = sink; }
  Core(const Core&) = default;
  Core& operator=(const Core&) = default;

  Result submit(const proto::NewOrderMsg& m, uint16_t session);
  Result cancel(OrderId id);
  Result reduce(OrderId id, Qty by);
  void reset_all();
  // The venue's mark price for a book (an oracle: set by whoever plays it). In mark mode stops
  // trigger on it; setting it checks the book's stops and runs the work queue.
  void set_mark(uint32_t book, Price px);
  std::optional<Price> mark_price(uint32_t book) const { return books_[book].mark; }
  void set_trigger_ref(uint8_t r) { cfg_.trigger_ref = r; }

  // Observation (also used by the control messages and the tests).
  Qty resting_qty(uint32_t book, uint8_t side, Price px) const;
  size_t resting_orders(uint32_t book) const;
  Qty resting_lots(uint32_t book) const { return books_[book].lots; }
  Qty peak_lots(uint32_t book) const { return books_[book].peak; }
  std::optional<Price> last_price(uint32_t book) const { return books_[book].last; }
  Qty position(uint8_t acct, uint32_t book) const { return pos_[acct * nbooks_ + book]; }
  int64_t cash(uint8_t acct) const { return cash_[acct]; }
  uint64_t book_hash(uint32_t book) const;
  uint64_t hash() const;  // byte-for-byte the same recipe as ob::Engine::hash with no program
  uint64_t trades() const { return trades_; }
  uint64_t orders() const { return orders_; }
  size_t live_orders() const { return live_; }
  size_t held_orders() const { return held_.size(); }
  const Order* find(OrderId id) const;
  // The orders waiting on a parent, oldest first.
  template <class F>
  void for_each_held(F&& f) const { for (uint32_t s : held_) f(pool_[s]); }
  // The pending (untriggered) stops of a book, submission order.
  template <class F>
  void for_each_stop(uint32_t b, F&& f) const { for (uint32_t s : books_[b].stops) f(pool_[s]); }
  // True while a triggered stop is matching, i.e. the taker in a fill is not the order whose
  // submission is being processed.
  bool firing() const { return firing_; }
  uint32_t nbooks() const { return nbooks_; }
  const Config& config() const { return cfg_; }
  void set_mutation(uint64_t m) { cfg_.mutation = m; }
  void set_stp(STP s) { cfg_.stp = s; }
  // Walk every resting order of a book in price-time order (asks ascending then bids descending).
  template <class F>
  void for_each_level(uint32_t book, F&& f) const;
  void snapshot(uint32_t book) const;  // replay every nonempty level into the sink

 private:
  struct Level {
    uint32_t head = NIL, tail = NIL;
    uint32_t count = 0;
    Qty total = 0;
  };
  struct Book {
    std::vector<Level> win[2];          // [side][price - base]
    uint64_t occ[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    std::map<Price, Level> below[2];    // prices < base
    std::map<Price, Level> above[2];    // prices >= base + width
    std::optional<Price> last;
    std::optional<Price> mark;
    std::vector<uint32_t> stops;        // pending stops (slots), submission order
    Qty lots = 0, peak = 0;
  };
  // Deferred effects, processed one at a time in event order (see the header comment).
  struct Work {
    enum Kind : uint8_t { FireStop, ParentDone, GroupFill };
    uint8_t kind;
    uint8_t full;      // GroupFill: the member filled completely
    uint8_t mode;      // GroupFill: the member's oco_mode
    uint32_t slot;     // FireStop: the stop
    uint32_t group;    // GroupFill
    OrderId id;        // ParentDone: the parent; GroupFill: the member (excluded from the effect)
    Qty qty;           // GroupFill: lots filled
  };

  uint32_t alloc();
  void free_slot(uint32_t s);
  Level* level_of(Book& bk, uint8_t side, Price px, bool create);
  const Level* level_of(const Book& bk, uint8_t side, Price px) const;
  void drop_level_if_empty(Book& bk, uint8_t side, Price px, Level* lv);
  // Best (or, under PriceInverted, worst) level on `side` of `bk` a taker with this limit may
  // hit. Returns nullptr if none.
  Level* eligible_level(Book& bk, uint8_t side, bool any_price, Price limit, Price& px_out);
  void activate(uint32_t s);
  void match(uint32_t s);
  void rest(uint32_t s);
  void unlink(uint32_t s);
  bool stop_fires(const Order& o, const std::optional<Price>& ref) const;
  const std::optional<Price>& trigger_ref(const Book& bk) const { return cfg_.trigger_ref ? bk.mark : bk.last; }
  void check_stops(Book& bk, uint32_t b);
  void drain();
  void release(uint32_t s);
  void after_activate(uint32_t s, bool report);
  void remove_live(uint32_t s);            // take a non-Free order out of wherever it lives
  void cancel_children(OrderId parent, uint8_t why);
  void drop_maker(uint32_t mslot);
  void group_effect(const Work& w);
  void unregister_group(uint32_t s);
  Qty reduce_cap(const Order& o) const;
  void emit_level(uint32_t b, uint8_t side, Price px, const Level* lv);

  Config cfg_;
  Sink* sink_;
  uint32_t nbooks_;
  std::vector<Book> books_;
  std::vector<Order> pool_;
  uint32_t free_head_ = NIL;
  std::vector<Work> work_;      // FIFO
  size_t work_head_ = 0;
  std::vector<uint32_t> held_;  // State::Held slots, submission order
  std::map<uint32_t, std::vector<uint32_t>> groups_;  // oco group -> member slots
  std::vector<Qty> pos_;
  std::vector<int64_t> cash_;
  uint64_t trades_ = 0, orders_ = 0;
  size_t live_ = 0;
  bool firing_ = false;
};

template <class F>
void Core::for_each_level(uint32_t b, F&& f) const {
  const Book& bk = books_[b];
  auto walk = [&](uint8_t side, Price px, const Level& lv) {
    f(side, px, lv.total, lv.count, [&](auto&& g) {
      for (uint32_t s = lv.head; s != NIL; s = pool_[s].next) g(pool_[s]);
    });
  };
  // asks ascending
  for (const auto& [px, lv] : bk.below[proto::Sell]) walk(proto::Sell, px, lv);
  for (uint32_t i = 0; i < cfg_.width; ++i)
    if (bk.win[proto::Sell][i].count) walk(proto::Sell, cfg_.base + i, bk.win[proto::Sell][i]);
  for (const auto& [px, lv] : bk.above[proto::Sell]) walk(proto::Sell, px, lv);
  // bids descending
  for (auto it = bk.above[proto::Buy].rbegin(); it != bk.above[proto::Buy].rend(); ++it)
    walk(proto::Buy, it->first, it->second);
  for (uint32_t i = cfg_.width; i-- > 0;)
    if (bk.win[proto::Buy][i].count) walk(proto::Buy, cfg_.base + i, bk.win[proto::Buy][i]);
  for (auto it = bk.below[proto::Buy].rbegin(); it != bk.below[proto::Buy].rend(); ++it)
    walk(proto::Buy, it->first, it->second);
}

}  // namespace fb
