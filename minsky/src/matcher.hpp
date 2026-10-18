// The exchange's message logic (orders, cancels, reduces, control) over the matching core,
// independent of sockets: the server drives it from its sessions, and the canary / tests drive
// it in-process through an InProcChannel, which is also the zero-latency point on the law.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "fastbook.hpp"
#include "proto.hpp"
#include "transport.hpp"

namespace ex {

struct Out {
  virtual ~Out() = default;
  virtual void to_session(uint16_t sid, const void* msg, size_t n) = 0;
  virtual void to_feed(const proto::Any& a) = 0;
};

// Lots resting on `side` of book `b` that a taker limited at `px` would reach: bids at prices
// >= px, asks at prices <= px. The environment sizes its print sweep with this.
inline fb::Qty depth_through(const fb::Core& core, uint32_t b, uint8_t side, fb::Price px) {
  fb::Qty n = 0;
  core.for_each_level(b, [&](uint8_t s, fb::Price p, fb::Qty total, uint32_t, auto&&) {
    if (s == side && (side == proto::Buy ? p >= px : p <= px)) n += total;
  });
  return n;
}

class Matcher : public fb::Sink {
 public:
  Matcher(const fb::Config& cfg, Out& out) : core_(cfg, this), out_(out) {}
  fb::Core& core() { return core_; }
  const fb::Core& core() const { return core_; }

  // The latency knob: the server holds every reply for this long before sending it (a delay
  // line, so pipelined orders overlap in flight as they do on a real wire). The core itself
  // is never delayed.
  uint64_t delay_ns = 0;
  bool quit = false;
  bool snapshot_wanted = false;

  // Every fill, per account, in the order the core reported it: the exchange's own record of
  // what each sub-account did, so an owner that was disconnected (venued after a kill -9) can
  // rebuild its accounting exactly. Read with FillCount / FillInfo.
  struct JournalFill {
    fb::OrderId id;
    fb::Price px;
    fb::Qty q;
    uint32_t book;
    uint8_t maker;
    uint8_t side;
  };
  const std::vector<JournalFill>& journal(uint8_t acct) const { return journal_[acct]; }

  // ---- Sink ----
  void fill(const fb::Order& o, bool maker, fb::Price px, fb::Qty q) override {
    proto::FillMsg f;
    f.is_maker = maker ? 1 : core_.firing() ? 2 : 0;  // 2: a triggered stop took liquidity
    f.tag = o.tag;
    f.order_id = o.id;
    f.book = o.book;
    f.price = px;
    f.qty = q;
    f.remaining = o.qty;
    journal_[o.account].push_back({o.id, px, q, o.book, static_cast<uint8_t>(maker ? 1 : 0), o.side});
    out_.to_session(o.session, &f, sizeof f);
  }
  void cancelled(const fb::Order& o) override {
    proto::AckMsg a;
    a.status = proto::Expired;
    a.tag = o.tag;
    a.order_id = o.id;
    a.remaining = o.qty;
    out_.to_session(o.session, &a, sizeof a);
  }
  void trade(uint32_t book, fb::Price px, fb::Qty q, uint8_t buyer, uint8_t seller) override {
    proto::TradeMsg t;
    t.book = book;
    t.price = px;
    t.qty = q;
    t.buyer = buyer;
    t.seller = seller;
    out_.to_feed(proto::Any::of(t));
  }
  void level(uint32_t book, uint8_t side, fb::Price px, fb::Qty total, uint32_t count) override {
    proto::LevelMsg l;
    l.book = book;
    l.side = side;
    l.price = px;
    l.total = total;
    l.count = count;
    out_.to_feed(proto::Any::of(l));
  }
  void reset(uint32_t book) override {
    proto::BookResetMsg r;
    r.book = book;
    out_.to_feed(proto::Any::of(r));
  }
  // chaining events: unsolicited acks to the owner
  void activated(const fb::Order& o) override { unsolicited(o, proto::Activated, o.qty); }
  void group_cancelled(const fb::Order& o, uint8_t why) override { unsolicited(o, why, o.qty); }
  void group_reduced(const fb::Order& o, fb::Qty) override { unsolicited(o, proto::GroupReduced, o.qty); }
  void finished(const fb::Order& o) override { unsolicited(o, proto::Finished, o.orig - o.filled); }

  // One inbound record from session `sid`; replies go to out_.
  void handle(const proto::Any& m, uint16_t sid) {
    switch (m.type()) {
      case proto::NewOrder: {
        const auto& o = m.as<proto::NewOrderMsg>();
        fb::Result r = core_.submit(o, sid);
        ack(sid, r.status, o.tag, r.id, r.remaining);
        break;
      }
      case proto::Cancel: {
        const auto& c = m.as<proto::CancelMsg>();
        fb::Result r = core_.cancel(c.order_id);
        ack(sid, r.status, c.tag, c.order_id, r.remaining);
        break;
      }
      case proto::Reduce: {
        const auto& c = m.as<proto::ReduceMsg>();
        fb::Result r = core_.reduce(c.order_id, c.by);
        ack(sid, r.status, c.tag, c.order_id, r.remaining);
        break;
      }
      case proto::Control: {
        const auto& c = m.as<proto::ControlMsg>();
        proto::ControlReplyMsg r;
        r.op = c.op;
        r.tag = c.tag;
        control(c, r);
        out_.to_session(sid, &r, sizeof r);
        break;
      }
      default:
        break;
    }
  }

  proto::HelloMsg hello() const {
    proto::HelloMsg h;
    h.books = core_.nbooks();
    h.base_price = core_.config().base;
    h.width = core_.config().width;
    return h;
  }

 private:
  void unsolicited(const fb::Order& o, uint8_t status, fb::Qty rem) {
    proto::AckMsg a;
    a.status = status;
    a.tag = o.tag;
    a.order_id = o.id;
    a.remaining = rem;
    out_.to_session(o.session, &a, sizeof a);
  }
  void ack(uint16_t sid, uint8_t status, uint32_t tag, fb::OrderId id, fb::Qty rem) {
    proto::AckMsg a;
    a.status = status;
    a.tag = tag;
    a.order_id = id;
    a.remaining = rem;
    out_.to_session(sid, &a, sizeof a);
  }
  void control(const proto::ControlMsg& c, proto::ControlReplyMsg& r) {
    const int64_t nb = core_.nbooks();
    switch (c.op) {
      case proto::Ping: break;
      case proto::SetDelayUs: delay_ns = static_cast<uint64_t>(c.arg) * 1000; break;
      case proto::SetMutation: core_.set_mutation(static_cast<uint64_t>(c.arg)); break;
      case proto::ResetBooks: core_.reset_all(); break;
      case proto::GetHash: r.arg = static_cast<int64_t>(core_.hash()); break;
      case proto::GetStats:
        r.arg = static_cast<int64_t>(core_.orders());
        r.arg2 = static_cast<int64_t>(core_.trades());
        break;
      case proto::BookQty: {
        uint32_t b;
        uint8_t side;
        proto::unpack_book_side(c.arg, b, side);
        r.arg = b < nb ? core_.resting_qty(b, side, c.arg2) : -1;
        break;
      }
      case proto::LastPrice: {
        auto lp = c.arg >= 0 && c.arg < nb ? core_.last_price(static_cast<uint32_t>(c.arg)) : std::nullopt;
        r.arg = lp.value_or(0);
        r.arg2 = lp.has_value();
        break;
      }
      case proto::Position: {
        uint8_t acct = static_cast<uint8_t>(c.arg & 0xff);
        uint32_t b = static_cast<uint32_t>(c.arg >> 32);
        r.arg = acct < core_.config().accounts && b < nb ? core_.position(acct, b) : 0;
        break;
      }
      case proto::Cash: r.arg = c.arg >= 0 && c.arg < core_.config().accounts ? core_.cash(static_cast<uint8_t>(c.arg)) : 0; break;
      case proto::BookInfo:
        if (c.arg >= 0 && c.arg < nb) {
          r.arg = static_cast<int64_t>(core_.resting_orders(static_cast<uint32_t>(c.arg)));
          r.arg2 = core_.resting_lots(static_cast<uint32_t>(c.arg));
        }
        break;
      case proto::PeakLots: r.arg = c.arg >= 0 && c.arg < nb ? core_.peak_lots(static_cast<uint32_t>(c.arg)) : 0; break;
      case proto::SetSTP: core_.set_stp(static_cast<fb::STP>(c.arg)); break;
      case proto::SetMark:
        if (c.arg >= 0 && c.arg < nb && c.arg2 <= fb::kMaxPrice && c.arg2 >= -fb::kMaxPrice)
          core_.set_mark(static_cast<uint32_t>(c.arg), c.arg2);
        break;
      case proto::SetTriggerRef: core_.set_trigger_ref(c.arg ? 1 : 0); break;
      case proto::Mark: {
        auto mp = c.arg >= 0 && c.arg < nb ? core_.mark_price(static_cast<uint32_t>(c.arg)) : std::nullopt;
        r.arg = mp.value_or(0);
        r.arg2 = mp.has_value();
        break;
      }
      case proto::OrderInfo: {
        const fb::Order* o = core_.find(static_cast<fb::OrderId>(c.arg));
        r.arg = proto::OrderGone;
        if (o) {
          switch (o->state) {
            case fb::State::Resting: r.arg = proto::OrderResting; break;
            case fb::State::Pending: r.arg = proto::OrderPending; break;
            case fb::State::Queued: r.arg = proto::OrderQueued; break;
            case fb::State::Held: r.arg = proto::OrderHeld; break;
            default: break;
          }
          r.arg2 = o->qty;
        }
        break;
      }
      case proto::DepthThrough: {
        uint32_t b;
        uint8_t side;
        proto::unpack_book_side(c.arg, b, side);
        r.arg = b < nb ? depth_through(core_, b, side, c.arg2) : 0;
        break;
      }
      case proto::FillCount:
        r.arg = static_cast<int64_t>(journal_[static_cast<uint8_t>(c.arg & 0xff)].size());
        break;
      case proto::FillInfo: {
        const auto& j = journal_[static_cast<uint8_t>(c.arg & 0xff)];
        const uint64_t i = static_cast<uint64_t>(c.arg) >> 8;
        if (i < j.size()) {
          const JournalFill& f = j[i];
          if (c.arg2 == 0) {
            r.arg = static_cast<int64_t>(f.id);
            r.arg2 = f.px;
          } else {
            r.arg = f.q;
            r.arg2 = static_cast<int64_t>(f.book) | (static_cast<int64_t>(f.maker) << 32) |
                     (static_cast<int64_t>(f.side) << 33);
          }
        } else {
          r.arg = -1;
        }
        break;
      }
      case proto::Snapshot: snapshot_wanted = true; break;
      case proto::Quit: quit = true; break;
      default: break;
    }
  }

  fb::Core core_;
  Out& out_;
  std::vector<JournalFill> journal_[256];
};

// A channel whose far end is a Matcher in the same process: send() is a function call, replies
// queue in an inbox. The feed goes to an optional callback.
class InProcChannel : public net::Channel, private Out {
 public:
  explicit InProcChannel(const fb::Config& cfg) : m_(cfg, *this) {}
  Matcher& matcher() { return m_; }
  std::function<void(const proto::Any&)> on_feed;
  using net::Channel::send;

  void send(const void* msg, size_t n) override { send_as(0, msg, n); }
  // Another session on the same exchange (a keyboard, a driver); its replies are discarded.
  template <class T>
  void send_as(uint16_t sid, const T& m) { send_as(sid, &m, sizeof(T)); }
  void send_as(uint16_t sid, const void* msg, size_t n) {
    proto::Any a;
    std::memcpy(a.bytes, msg, n);
    a.size = n;
    m_.handle(a, sid);
  }
  void flush() override {}
  bool try_recv(proto::Any& out) override {
    if (inbox_.empty()) return false;
    out = inbox_.front();
    inbox_.pop_front();
    return true;
  }
  bool alive() const override { return true; }
  const char* kind() const override { return "inproc"; }

 private:
  void to_session(uint16_t sid, const void* msg, size_t n) override {
    if (sid != 0) return;
    proto::Any a;
    std::memcpy(a.bytes, msg, n);
    a.size = n;
    inbox_.push_back(a);
  }
  void to_feed(const proto::Any& a) override {
    if (on_feed) on_feed(a);
  }
  Matcher m_;
  std::deque<proto::Any> inbox_;
};

}  // namespace ex
