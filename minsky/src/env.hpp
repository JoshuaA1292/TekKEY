// The environment ("everyone else", BOUNDED_KEYS.md §8.1) as real exchange messages, and the
// program-side bookkeeping, shared by verify (an in-process fb::Core), replay and venued (the live
// exchange over a wire). Both sides drive the engine through the same Port with the same message
// sequence, which is what makes "the search is the real thing" true: a verifier path replayed on
// the exchange produces the same fills, to the cent.
//
// A print at price P (fill model full_at_print) is three or four engine messages from two
// environment accounts E1 and E2 (two, because self-trade prevention stops one account trading
// with itself):
//   down print (P < last):
//     1. n = lots of bids at prices >= P                      (DepthThrough)
//     2. E2 rests BUY  P x 2^30   environment liquidity at the print; it also trades through any
//                                 asks <= P at their own prices
//     3. E1 sends SELL IOC P x n+1  sweeps every bid >= P at the bid's own price (resting orders
//                                 fill at their own price), then trades 1 lot with E2 at P: the
//                                 print, so last = P. Stops fired by these trades and children
//                                 released by them are queued by the engine and processed after
//                                 the sweep, as aggressors against E2's liquidity at P.
//     4. (mark mode) SetMark P, while E2's liquidity is still there.
//     5. E2's remainder is cancelled.
//   up print: the mirror image. P == last: a down pass, then an up pass.
// An order that enters during a print on the side opposite to its direction (a buy released on a
// down print) finds no environment seller at P and rests; it is then swept at its own limit by a
// second round of the same procedure, so it never fills better than the print. Every program
// order therefore satisfies, between moves: bids < last < asks.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fastbook.hpp"
#include "matcher.hpp"
#include "program_json.hpp"
#include "proto.hpp"
#include "transport.hpp"

namespace bk {

inline constexpr int64_t kEnvDepth = int64_t{1} << 30;

// Accounts on the live exchange (started with --accounts 255):
//   0          the environment's sweeping side (E1)        CONTRACT.md §1: reserved
//   1 .. 252   one per program, allocated by venued
//   253        replay's scratch sub-account
//   254        the environment's liquidity side (E2)
inline constexpr uint8_t kReplayAccount = 253;
inline constexpr uint8_t kMaxProgramAccount = 252;
inline constexpr uint8_t kLiveAccounts = 255;
// OCO group ids are exchange-wide, so each client draws from its own range.
inline constexpr uint32_t kVenuedOcoBase = 0x00100000u;   // venued: persisted counter from here
inline constexpr uint32_t kReplayOcoBase = 0x40000000u;   // replay: base + pid-derived offset

struct EnvAccounts {
  uint8_t e1 = 0;    // the sweeping side of every print
  uint8_t e2 = 254;  // the liquidity side (the live exchange's reserved account)
};

// A synchronous client view of one exchange.
struct Port {
  virtual ~Port() = default;
  virtual fb::Result submit(const proto::NewOrderMsg& m) = 0;
  virtual fb::Result cancel(fb::OrderId id) = 0;
  virtual int64_t depth_through(uint32_t book, uint8_t side, int64_t px) = 0;
  virtual void set_mark(uint32_t book, int64_t px) = 0;
  virtual std::optional<int64_t> last(uint32_t book) = 0;
  virtual uint8_t order_state(fb::OrderId id) = 0;  // proto::OrderState
  virtual int64_t position(uint8_t acct, uint32_t book) = 0;
};

inline int print_one(Port& port, const EnvAccounts& env, uint32_t book, int64_t P, bool mark_mode) {
  const std::optional<int64_t> last = port.last(book);
  auto pass = [&](bool down) {
    // the side being swept: bids on a down print, asks on an up print
    const uint8_t swept = down ? proto::Buy : proto::Sell;
    int rounds = 0;
    for (; rounds < 4; ++rounds) {
      const int64_t n = port.depth_through(book, swept, P);
      if (rounds > 0 && n == 0) break;
      proto::NewOrderMsg liq;
      liq.account = env.e2;
      liq.side = swept;
      liq.otype = proto::Limit;
      liq.book = book;
      liq.price = P;
      liq.qty = kEnvDepth;
      const fb::Result l = port.submit(liq);
      proto::NewOrderMsg sweep;
      sweep.account = env.e1;
      sweep.side = down ? proto::Sell : proto::Buy;
      sweep.otype = proto::IOC;
      sweep.book = book;
      sweep.price = P;
      sweep.qty = n + 1;
      port.submit(sweep);
      if (mark_mode) port.set_mark(book, P);
      if (l.status == proto::Rested) port.cancel(l.id);
    }
    return rounds;
  };
  if (last && P > *last) return pass(false);
  if (last && P < *last) return pass(true);
  const int r = pass(true);
  return r + pass(false);
}

// One print at P on `book`. Returns the number of rounds run (1 normally).
//
// `others` are the program's other books. While this book prints, the environment quotes each of
// them at its last print (bid at last, ask one tick above), so an order this print releases there
// (a hedge after the entry fills, its stop) meets a standing market instead of an empty book:
// sells fill at the last print, buys one tick above it. The quotes go when the print is done.
inline int print(Port& port, const EnvAccounts& env, uint32_t book, int64_t P, bool mark_mode,
                 const std::vector<uint32_t>& others = {}) {
  std::vector<fb::OrderId> quotes;
  for (uint32_t b : others) {
    if (b == book) continue;
    const std::optional<int64_t> lb = port.last(b);
    if (!lb) continue;
    proto::NewOrderMsg q;
    q.otype = proto::Limit;
    q.book = b;
    q.qty = kEnvDepth;
    q.account = env.e2;
    q.side = proto::Buy;
    q.price = *lb;
    fb::Result r = port.submit(q);
    if (r.status == proto::Rested) quotes.push_back(r.id);
    q.account = env.e1;
    q.side = proto::Sell;
    q.price = *lb + 1;
    r = port.submit(q);
    if (r.status == proto::Rested) quotes.push_back(r.id);
  }
  const int rounds = print_one(port, env, book, P, mark_mode);
  for (fb::OrderId id : quotes) port.cancel(id);
  return rounds;
}

// Cancel by the key holder, second half (BOUNDED_KEYS.md §8.2 move 4): the position is closed by
// two native, reduce-only orders in one OCO group, so it closes on the next print in either
// direction with nobody connected: a stop-market one tick through the last print (fires on any
// print against the position) and a limit at the last print (swept by any print in its favour).
struct FlattenIds {
  fb::OrderId stop = 0, limit = 0;
};
inline FlattenIds flatten(Port& port, uint8_t acct, uint32_t book, int64_t pos, int64_t last, uint32_t oco_id,
                          uint32_t tag = 0) {
  FlattenIds ids;
  if (pos == 0) return ids;
  const bool long_pos = pos > 0;
  proto::NewOrderMsg stop;
  stop.account = acct;
  stop.side = long_pos ? proto::Sell : proto::Buy;
  stop.otype = proto::StopMarket;
  stop.reduce_only = 1;
  stop.book = book;
  stop.trigger = long_pos ? last - 1 : last + 1;
  stop.qty = long_pos ? pos : -pos;
  stop.oco = oco_id;
  stop.oco_mode = proto::OcoReduce;
  stop.tag = tag;
  proto::NewOrderMsg lim = stop;
  lim.otype = proto::Limit;
  lim.trigger = 0;
  lim.price = last;
  const fb::Result a = port.submit(stop);
  const fb::Result b = port.submit(lim);
  ids.stop = a.status == proto::Rejected ? 0 : a.id;
  ids.limit = b.status == proto::Rejected ? 0 : b.id;
  return ids;
}

// The program's money, from its fills (one ledger per program, instruments by program index).
struct Ledger {
  int64_t cash_cents = 0;  // + received for sells, - paid for buys
  int64_t fees_cents = 0;
  int64_t trades = 0;
  int64_t max_exposure = 0;
  std::vector<int64_t> pos;   // lots
  std::vector<int64_t> cost;  // cents paid for the open position (signed like pos), average cost

  explicit Ledger(size_t n = 1) : pos(n, 0), cost(n, 0) {}
  // Returns the fee charged for this fill.
  int64_t fill(int inst, uint8_t side, int64_t px, int64_t q, bool maker, int64_t tick_cents, const Fees& fees) {
    const int64_t fee = fees.fee(px, q, tick_cents, maker);
    const int64_t notional = px * q * tick_cents;
    const int64_t d = side == proto::Buy ? q : -q;
    cash_cents += side == proto::Buy ? -notional : notional;
    fees_cents += fee;
    ++trades;
    int64_t& p = pos[inst];
    int64_t& c = cost[inst];
    if (p == 0 || (p > 0) == (d > 0)) {
      c += d * px * tick_cents;
      p += d;
    } else {
      const int64_t close = std::min(std::abs(d), std::abs(p));
      c -= c * close / std::abs(p);
      p += d;
      if (p != 0 && (p > 0) == (d > 0)) c = p * px * tick_cents;  // flipped through zero
      if (p == 0) c = 0;
    }
    int64_t ex = 0;
    for (int64_t x : pos) ex += std::abs(x);
    max_exposure = std::max(max_exposure, ex);
    return fee;
  }
  int64_t exposure() const {
    int64_t ex = 0;
    for (int64_t x : pos) ex += std::abs(x);
    return ex;
  }
  // Loss at the given marks: -(cash + position value) + fees. A position with no mark counts at cost.
  int64_t loss(const std::vector<std::optional<int64_t>>& marks, const std::vector<int64_t>& ticks) const {
    int64_t v = cash_cents;
    for (size_t i = 0; i < pos.size(); ++i) v += marks[i] ? pos[i] * *marks[i] * ticks[i] : cost[i];
    return -v + fees_cents;
  }
  // Realized: closed lots at average cost, plus every fee.
  int64_t realized_loss() const {
    int64_t v = cash_cents;
    for (int64_t c : cost) v += c;
    return -v + fees_cents;
  }
};

// The core, in process (the verifier's port).
class CorePort : public Port {
 public:
  explicit CorePort(fb::Core& c) : c_(&c) {}
  void bind(fb::Core& c) { c_ = &c; }
  fb::Result submit(const proto::NewOrderMsg& m) override { return c_->submit(m, 0); }
  fb::Result cancel(fb::OrderId id) override { return c_->cancel(id); }
  int64_t depth_through(uint32_t book, uint8_t side, int64_t px) override {
    return ex::depth_through(*c_, book, side, px);
  }
  void set_mark(uint32_t book, int64_t px) override { c_->set_mark(book, px); }
  std::optional<int64_t> last(uint32_t book) override { return c_->last_price(book); }
  uint8_t order_state(fb::OrderId id) override {
    const fb::Order* o = c_->find(id);
    if (!o) return proto::OrderGone;
    switch (o->state) {
      case fb::State::Resting: return proto::OrderResting;
      case fb::State::Pending: return proto::OrderPending;
      case fb::State::Queued: return proto::OrderQueued;
      case fb::State::Held: return proto::OrderHeld;
      default: return proto::OrderGone;
    }
  }
  int64_t position(uint8_t acct, uint32_t book) override { return c_->position(acct, book); }

 private:
  fb::Core* c_;
};

// The live exchange over a channel. Each call is one round trip: send, then wait for the reply
// carrying our tag. Everything else that arrives meanwhile (fills, unsolicited acks) goes to
// `on_other`. Throws if the exchange goes away or does not answer within `timeout_ms`.
class WirePort : public Port {
 public:
  explicit WirePort(std::unique_ptr<net::Channel> ch, uint64_t timeout_ms = 5000)
      : ch_(std::move(ch)), timeout_ns_(timeout_ms * 1'000'000ull) {
    // the exchange greets every session with a Hello; skip it
  }
  std::function<void(const proto::Any&)> on_other;
  net::Channel& channel() { return *ch_; }

  fb::Result submit(const proto::NewOrderMsg& m0) override {
    proto::NewOrderMsg m = m0;
    m.tag = next_tag();
    ch_->send(m);
    ch_->flush();
    const proto::AckMsg a = wait_ack(m.tag);
    return {a.status, a.order_id, a.remaining};
  }
  fb::Result cancel(fb::OrderId id) override {
    proto::CancelMsg c;
    c.tag = next_tag();
    c.order_id = id;
    ch_->send(c);
    ch_->flush();
    const proto::AckMsg a = wait_ack(c.tag);
    return {a.status, a.order_id, a.remaining};
  }
  proto::ControlReplyMsg control(uint8_t op, int64_t arg = 0, int64_t arg2 = 0) {
    proto::ControlMsg c;
    c.op = op;
    c.tag = next_tag();
    c.arg = arg;
    c.arg2 = arg2;
    ch_->send(c);
    ch_->flush();
    const uint64_t t0 = net::now_ns();
    proto::Any a;
    for (;;) {
      if (!ch_->alive()) throw std::runtime_error("exchange connection lost");
      if (ch_->try_recv(a)) {
        if (a.type() == proto::ControlReply && a.as<proto::ControlReplyMsg>().tag == c.tag)
          return a.as<proto::ControlReplyMsg>();
        other(a);
        continue;
      }
      if (net::now_ns() - t0 > timeout_ns_) throw std::runtime_error("exchange did not answer");
    }
  }
  int64_t depth_through(uint32_t book, uint8_t side, int64_t px) override {
    return control(proto::DepthThrough, proto::pack_book_side(book, side), px).arg;
  }
  void set_mark(uint32_t book, int64_t px) override { control(proto::SetMark, book, px); }
  std::optional<int64_t> last(uint32_t book) override {
    const auto r = control(proto::LastPrice, book);
    if (!r.arg2) return std::nullopt;
    return r.arg;
  }
  uint8_t order_state(fb::OrderId id) override {
    return static_cast<uint8_t>(control(proto::OrderInfo, static_cast<int64_t>(id)).arg);
  }
  int64_t position(uint8_t acct, uint32_t book) override {
    return control(proto::Position, static_cast<int64_t>(acct) | (static_cast<int64_t>(book) << 32)).arg;
  }
  int64_t fill_count(uint8_t acct) { return control(proto::FillCount, acct).arg; }
  struct JFill {
    fb::OrderId id;
    int64_t px, q;
    uint32_t book;
    bool maker;
    uint8_t side;  // proto::Side of the order that filled
  };
  JFill fill_at(uint8_t acct, int64_t i) {
    const auto a = control(proto::FillInfo, static_cast<int64_t>(acct) | (i << 8), 0);
    const auto b = control(proto::FillInfo, static_cast<int64_t>(acct) | (i << 8), 1);
    return {static_cast<fb::OrderId>(a.arg), a.arg2, b.arg, static_cast<uint32_t>(b.arg2 & 0xffffffff),
            ((b.arg2 >> 32) & 1) != 0, static_cast<uint8_t>((b.arg2 >> 33) & 1)};
  }
  // Drain anything already queued (fills, unsolicited acks) without blocking.
  void poll() {
    proto::Any a;
    while (ch_->try_recv(a)) other(a);
  }

 private:
  uint32_t next_tag() {
    if (++tag_ == 0) tag_ = 1;
    return tag_;
  }
  void other(const proto::Any& a) {
    if (on_other) on_other(a);
  }
  proto::AckMsg wait_ack(uint32_t tag) {
    const uint64_t t0 = net::now_ns();
    proto::Any a;
    for (;;) {
      if (!ch_->alive()) throw std::runtime_error("exchange connection lost");
      if (ch_->try_recv(a)) {
        if (a.type() == proto::Ack) {
          const auto& k = a.as<proto::AckMsg>();
          if (k.tag == tag && !proto::unsolicited(k.status)) return k;
        }
        other(a);
        continue;
      }
      if (net::now_ns() - t0 > timeout_ns_) throw std::runtime_error("exchange did not answer");
    }
  }
  std::unique_ptr<net::Channel> ch_;
  uint64_t timeout_ns_;
  uint32_t tag_ = 0;
};

}  // namespace bk
