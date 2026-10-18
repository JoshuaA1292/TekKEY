// Deterministic multi-book matching engine plus an order-router layer.
//
// Engine rules (from the Minsky spec):
//   * integer ticks and lots, no floats, no clock, no randomness
//   * price-time priority, FIFO at each level
//   * order types: limit, market, IOC, stop-market, stop-limit
//   * a sell stop triggers when the last trade price is <= trigger; buy stop mirrors
//   * triggered stops queue in submission order and are processed one at a time
//     (stops are checked after every print, so within one sweep a stop fired by an earlier
//      print queues ahead of one fired by a later print)
//   * several independent books
//
// The router layer is the part Weinberger puts in a broker's smart order router:
// links that fire on a fill (optionally keyed by fill price) or on acceptance and
// submit / cancel / reduce other orders. It is a fixed, finite table: it holds no
// counters of its own, so any unbounded memory must live in the books.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ob {

using Price = int64_t;
using Qty = int64_t;
using OrderId = uint64_t;

enum class Side : uint8_t { Buy, Sell };
enum class Type : uint8_t { Limit, Market, IOC, StopMarket, StopLimit };
// Self-trade prevention when taker and maker share an account.
// ExpireMaker is Hyperliquid's documented behaviour (resting order cancelled, no fill).
enum class STP : uint8_t { None, ExpireMaker, CancelTaker };

struct Template {
  int account = 0;
  int book = 0;
  Side side = Side::Buy;
  Type type = Type::Limit;
  Price price = 0;    // limit price: Limit, IOC, StopLimit
  Price trigger = 0;  // StopMarket, StopLimit
  Qty qty = 0;
  bool reduce_only = false;
};

struct Order {
  OrderId id = 0;
  int tmpl = -1;  // router template that produced it, -1 for external orders
  Template t;     // t.qty is the remaining quantity
};

// ---- router layer -----------------------------------------------------------
enum class On : uint8_t { Fill, Accept };
enum class Act : uint8_t { Submit, Cancel, Reduce, Halt };
struct Action {
  Act kind = Act::Submit;
  int arg = -1;  // Submit: template. Cancel/Reduce: template whose live orders are hit. Halt: code
};
struct Link {
  On on = On::Fill;
  int src = -1;               // template id
  std::optional<Price> at;    // Fill only: fire only at this fill price
  std::vector<Action> actions;
};
struct Program {
  std::vector<Template> templates;
  std::vector<Link> links;
  std::vector<Action> initial;
};

struct Trade {
  int book;
  Price price;
  Qty qty;
  OrderId taker, maker;
  int buyer, seller;  // accounts
};

enum class Status { Idle, Halted, Stopped, Budget };

class Engine {
 public:
  Engine(int books, int accounts, STP stp = STP::None, const Program* prog = nullptr);

  // External events. Their consequences (triggered stops, link actions) are queued;
  // run() drains the queue. step(event) == event + run().
  OrderId submit(const Template& t) { return submit_tmpl(t, -1); }
  bool cancel(OrderId id);
  void start();  // queue the program's initial actions

  // Process queued work items one at a time, at most `budget` of them.
  Status run(uint64_t budget = UINT64_MAX);
  bool idle() const { return work_.empty(); }

  // Observation.
  Qty resting_qty(int book, Side side, Price px) const;
  size_t resting_orders() const;
  size_t resting_orders(int book) const;
  Qty resting_lots(int book) const;       // sum of resting quantity on both sides
  Qty peak_lots(int book) const { return books_[book].peak_lots; }  // max resting_lots ever seen
  size_t live_orders() const { return live_.size(); }
  std::optional<Price> last_price(int book) const { return books_[book].last; }
  Qty position(int acct, int book) const { return pos_[acct][book]; }
  int64_t cash(int acct) const { return cash_[acct]; }
  uint64_t hash() const;      // canonical: independent of order ids
  uint64_t book_hash(int b) const;
  bool halted() const { return halted_; }
  int halt_code() const { return halt_code_; }
  uint64_t work_done() const { return work_done_; }
  uint64_t trade_count() const { return trade_count_; }

  bool log_trades = true;
  std::vector<Trade> trades;
  // Called before each router action executes. May set stop_requested.
  std::function<void(const Action&)> on_action;
  bool stop_requested = false;

 private:
  struct Level {
    std::deque<OrderId> q;
    Qty total = 0;
  };
  struct Book {
    std::map<Price, Level, std::greater<Price>> bids;
    std::map<Price, Level> asks;
    std::optional<Price> last;
    std::vector<OrderId> stops;  // pending, in submission order
    Qty peak_lots = 0;           // updated whenever an order rests
  };
  struct Work {
    bool is_stop;
    OrderId id;   // triggered stop
    Action a;     // router action
    Qty fill_qty; // for Reduce
  };

  OrderId submit_tmpl(const Template& t, int tmpl);
  void activate(Order& o);  // match a live (non-stop) order, rest the remainder if Limit
  void match(Order& o);
  void rest(Order& o);
  void fill_links(int tmpl, Price px, Qty q);
  void check_stops(int book);
  void exec(const Work& w);
  void remove_live(OrderId id);
  bool unlink_from_book(const Order& o);
  Qty reduce_cap(const Order& o) const;

  int nbooks_, naccts_;
  STP stp_;
  const Program* prog_;
  std::vector<std::vector<int>> links_by_src_;
  std::unordered_set<int> tracked_;  // templates that are Cancel/Reduce targets
  std::unordered_map<int, std::vector<OrderId>> by_tmpl_;

  std::vector<Book> books_;
  std::unordered_map<OrderId, Order> live_;  // resting or pending-stop orders
  std::deque<Work> work_;
  std::vector<std::vector<Qty>> pos_;
  std::vector<int64_t> cash_;
  OrderId next_id_ = 1;
  bool halted_ = false;
  int halt_code_ = -1;
  uint64_t work_done_ = 0;
  uint64_t trade_count_ = 0;
};

}  // namespace ob
