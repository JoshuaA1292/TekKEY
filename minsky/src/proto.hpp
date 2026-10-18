// Wire protocol between the exchange, order clients (the router, the keyboard) and feed
// subscribers (the visualiser). Every message is a fixed-size little-endian record whose first
// byte is its type; the size follows from the type. The same records travel over TCP (a byte
// stream of records), UDP (one record per datagram) and the shared-memory rings (one record per
// slot), so the transport is the only thing that changes between the points on the latency law.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace proto {

inline constexpr uint16_t kVersion = 2;

enum Msg : uint8_t {
  // client -> exchange
  NewOrder = 1,
  Cancel = 2,
  Reduce = 3,    // reduce an order's remaining quantity in place (keeps time priority)
  Control = 4,
  // exchange -> client
  Ack = 16,
  Fill = 17,
  ControlReply = 18,
  // exchange -> feed subscribers
  Level = 32,    // (book, side, price) now holds `total` lots in `count` orders
  Trade = 33,
  BookReset = 34,  // all levels of `book` are empty; a snapshot follows
  Stats = 35,
  Gap = 36,        // the feed dropped events for this subscriber; a snapshot follows
  Hello = 37,
};

enum Side : uint8_t { Buy = 0, Sell = 1 };
enum OType : uint8_t { Limit = 0, Market = 1, IOC = 2, StopMarket = 3, StopLimit = 4 };

// Ack.status
enum AckStatus : uint8_t {
  Rested = 0,      // limit / stop order is live
  Filled = 1,      // taker fully filled (or stop fired and filled)
  Done = 2,        // IOC / market remainder cancelled; or stop fired and nothing rested
  Rejected = 3,
  Cancelled = 4,   // Cancel succeeded
  NotFound = 5,    // Cancel / Reduce target not live
  Reduced = 6,
  Expired = 7,     // unsolicited: a resting order was removed (self-trade prevention)
  // v2: native chaining. Everything from Activated on is unsolicited.
  Held = 8,             // a child order is waiting for its parent to fill completely
  Activated = 9,        // unsolicited: a held child was released (its parent filled)
  GroupCancelled = 10,  // unsolicited: removed because another member of its OCO group filled
  GroupReduced = 11,    // unsolicited: reduced by another member's fill (reduce mode); remaining given
  ParentCancelled = 12, // unsolicited: a held child removed because its parent went away unfilled
  Finished = 13,        // unsolicited: a fired stop or released child is done; remaining = unfilled part
};
// Acks the exchange sends without a request: never the reply a client is waiting for.
inline bool unsolicited(uint8_t st) { return st == Expired || st >= Activated; }

enum ControlOp : uint8_t {
  Ping = 0,
  SetDelayUs = 1,     // arg = busy-wait before replying to every order, in microseconds
  SetMutation = 2,    // arg = bitmask of fastbook::Mutation
  ResetBooks = 3,     // clear every book, position and counter
  GetHash = 4,        // reply.arg = canonical hash of all books (ids excluded)
  GetStats = 5,       // reply.arg = orders processed; arg2 = trades
  BookQty = 6,        // arg = book | side<<32, arg2 = price; reply.arg = total resting quantity
  LastPrice = 7,      // arg = book; reply.arg = last trade price, arg2 = 1 if any
  Position = 8,       // arg = account | book<<32; reply.arg = position
  Cash = 9,           // arg = account; reply.arg = cash
  Snapshot = 10,      // (feed only) resend every nonempty level to this subscriber
  BookInfo = 11,      // arg = book; reply.arg = resting orders, arg2 = resting lots
  SetSTP = 12,        // arg = 0 none, 1 expire-maker, 2 cancel-taker
  Quit = 13,
  PeakLots = 14,      // arg = book; reply.arg = most lots ever resting at once in that book
  SetMark = 15,       // arg = book, arg2 = mark price (the venue's oracle; triggers key off it in mark mode)
  SetTriggerRef = 16, // arg = 0 stops trigger on the last trade, 1 on the mark price
  Mark = 17,          // arg = book; reply.arg = mark price, arg2 = 1 if set
  // Bounded keys (venued, replay). Read-only: none of these changes the book.
  OrderInfo = 18,     // arg = order id; reply.arg = OrderState, arg2 = remaining qty
  DepthThrough = 19,  // arg = book | side<<32, arg2 = price; reply.arg = lots on that side a taker
                      // limited at `price` would reach (bids >= price, asks <= price)
  FillCount = 20,     // arg = account; reply.arg = fills in that account's journal
  FillInfo = 21,      // arg = account | index<<8, arg2 = 0: reply (order id, price)
                      //                           arg2 = 1: reply (qty, book | maker<<32 | side<<33)
};

// OrderInfo reply.arg
enum OrderState : uint8_t { OrderGone = 0, OrderResting = 1, OrderPending = 2, OrderQueued = 3, OrderHeld = 4 };

enum OcoMode : uint8_t { OcoCancel = 0, OcoReduce = 1 };

#pragma pack(push, 1)
struct NewOrderMsg {
  uint8_t type = NewOrder;
  uint8_t account = 0;
  uint8_t side = Buy;
  uint8_t otype = Limit;
  uint8_t reduce_only = 0;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t tag = 0;   // echoed on every Ack / Fill for this order
  uint32_t book = 0;
  int64_t price = 0;
  int64_t trigger = 0;
  int64_t qty = 0;
  // v2 native chaining. parent != 0: the order is held and activated only when that order
  // fills completely (cancelled if the parent goes away unfilled). oco != 0: the order belongs
  // to a one-cancels-other group; when it fills, the other members are cancelled (OcoCancel,
  // on its complete fill) or reduced by the filled quantity (OcoReduce, on every fill).
  uint64_t parent = 0;
  uint32_t oco = 0;
  uint8_t oco_mode = OcoCancel;
  uint8_t pad3[3] = {0, 0, 0};
};
struct CancelMsg {
  uint8_t type = Cancel;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t tag = 0;
  uint64_t order_id = 0;
};
struct ReduceMsg {
  uint8_t type = Reduce;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t tag = 0;
  uint64_t order_id = 0;
  int64_t by = 0;  // lots to remove; >= remaining cancels
};
struct ControlMsg {
  uint8_t type = Control;
  uint8_t op = Ping;
  uint8_t pad[2] = {0, 0};
  uint32_t tag = 0;
  int64_t arg = 0;
  int64_t arg2 = 0;
};
struct AckMsg {
  uint8_t type = Ack;
  uint8_t status = Rested;
  uint8_t pad[2] = {0, 0};
  uint32_t tag = 0;
  uint64_t order_id = 0;
  int64_t remaining = 0;
};
struct FillMsg {
  uint8_t type = Fill;
  uint8_t is_maker = 0;  // 0 the order being processed took; 1 a resting order made; 2 a triggered stop took
  uint8_t pad[2] = {0, 0};
  uint32_t tag = 0;
  uint64_t order_id = 0;
  uint32_t book = 0;
  uint32_t pad2 = 0;
  int64_t price = 0;
  int64_t qty = 0;
  int64_t remaining = 0;
};
struct ControlReplyMsg {
  uint8_t type = ControlReply;
  uint8_t op = Ping;
  uint8_t pad[2] = {0, 0};
  uint32_t tag = 0;
  int64_t arg = 0;
  int64_t arg2 = 0;
};
// feed
struct LevelMsg {
  uint8_t type = Level;
  uint8_t side = Buy;
  uint8_t pad[2] = {0, 0};
  uint32_t book = 0;
  int64_t price = 0;
  int64_t total = 0;
  uint32_t count = 0;
  uint32_t pad2 = 0;
};
struct TradeMsg {
  uint8_t type = Trade;
  uint8_t buyer = 0;
  uint8_t seller = 0;
  uint8_t pad = 0;
  uint32_t book = 0;
  int64_t price = 0;
  int64_t qty = 0;
};
struct BookResetMsg {
  uint8_t type = BookReset;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t book = 0;
};
struct StatsMsg {
  uint8_t type = Stats;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t delay_us = 0;
  uint64_t orders = 0;    // inbound order messages processed
  uint64_t trades = 0;
  uint64_t mutation = 0;
  uint64_t feed_drops = 0;
  uint64_t ns = 0;        // server monotonic clock at emission
};
struct GapMsg {
  uint8_t type = Gap;
  uint8_t pad[3] = {0, 0, 0};
  uint32_t dropped = 0;
};
struct HelloMsg {
  uint8_t type = Hello;
  uint8_t pad = 0;
  uint16_t version = kVersion;
  uint32_t books = 0;
  int64_t base_price = 0;
  uint32_t width = 0;
  uint32_t pad2 = 0;
};
#pragma pack(pop)

inline constexpr size_t kMaxMsg = 56;

// Size of the record whose first byte is `type`; 0 for an unknown type.
inline size_t size_of(uint8_t type) {
  switch (type) {
    case NewOrder: return sizeof(NewOrderMsg);
    case Cancel: return sizeof(CancelMsg);
    case Reduce: return sizeof(ReduceMsg);
    case Control: return sizeof(ControlMsg);
    case Ack: return sizeof(AckMsg);
    case Fill: return sizeof(FillMsg);
    case ControlReply: return sizeof(ControlReplyMsg);
    case Level: return sizeof(LevelMsg);
    case Trade: return sizeof(TradeMsg);
    case BookReset: return sizeof(BookResetMsg);
    case Stats: return sizeof(StatsMsg);
    case Gap: return sizeof(GapMsg);
    case Hello: return sizeof(HelloMsg);
  }
  return 0;
}
static_assert(sizeof(NewOrderMsg) == 56);
static_assert(sizeof(FillMsg) == 48);
static_assert(sizeof(FillMsg) <= kMaxMsg && sizeof(NewOrderMsg) <= kMaxMsg && sizeof(StatsMsg) <= kMaxMsg &&
              sizeof(LevelMsg) <= kMaxMsg && sizeof(ControlMsg) <= kMaxMsg);

// A record of any type, with its size.
struct Any {
  alignas(8) uint8_t bytes[kMaxMsg];
  size_t size = 0;
  uint8_t type() const { return bytes[0]; }
  template <class T>
  const T& as() const { return *reinterpret_cast<const T*>(bytes); }
  template <class T>
  static Any of(const T& m) {
    Any a;
    static_assert(sizeof(T) <= kMaxMsg);
    std::memcpy(a.bytes, &m, sizeof(T));
    a.size = sizeof(T);
    return a;
  }
};

// Control argument packing for BookQty: arg = book | side << 32, arg2 = price.
inline int64_t pack_book_side(uint32_t book, uint8_t side) {
  return static_cast<int64_t>(book) | (static_cast<int64_t>(side) << 32);
}
inline void unpack_book_side(int64_t a, uint32_t& book, uint8_t& side) {
  book = static_cast<uint32_t>(a & 0xffffffff);
  side = static_cast<uint8_t>((a >> 32) & 1);
}

}  // namespace proto
