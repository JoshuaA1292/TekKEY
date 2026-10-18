#include "router.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace rt {

const char* status_name(Status s) {
  switch (s) {
    case Status::Halted: return "halted";
    case Status::StepLimit: return "step-limit";
    case Status::Idle: return "idle (stalled: nothing left to do and no halt)";
    case Status::Dead: return "exchange connection lost";
    case Status::Timeout: return "timed out waiting for the exchange";
  }
  return "?";
}

proto::NewOrderMsg order_of(const ob::Template& t, uint32_t tag) {
  proto::NewOrderMsg m;
  m.account = static_cast<uint8_t>(t.account);
  m.book = static_cast<uint32_t>(t.book);
  m.side = static_cast<uint8_t>(t.side);
  m.otype = static_cast<uint8_t>(t.type);
  m.reduce_only = t.reduce_only;
  m.price = t.price;
  m.trigger = t.trigger;
  m.qty = t.qty;
  m.tag = tag;
  return m;
}

Router::Router(const tbl::Table& t, net::Channel& ch) : t_(t), ch_(ch) {
  const size_t n = t_.prog.templates.size();
  fill_links_.resize(n);
  accept_links_.resize(n);
  tracked_.assign(n, 0);
  live_.resize(n);
  auto track = [&](const ob::Action& a) {
    if (a.kind != ob::Act::Halt && (a.arg < 0 || a.arg >= static_cast<int>(n)))
      throw std::invalid_argument("action refers to template " + std::to_string(a.arg) + " of " + std::to_string(n));
    if (a.kind == ob::Act::Cancel || a.kind == ob::Act::Reduce) tracked_[a.arg] = 1;
  };
  if (t_.entry_instr.size() != n) throw std::invalid_argument("entry_instr size does not match templates");
  for (const auto& l : t_.prog.links) {
    if (l.src < 0 || l.src >= static_cast<int>(n)) throw std::invalid_argument("link source out of range");
    if (l.on == ob::On::Fill) fill_links_[l.src].push_back({l.at.has_value(), l.at.value_or(0), l.actions});
    else accept_links_[l.src].insert(accept_links_[l.src].end(), l.actions.begin(), l.actions.end());
    for (const auto& a : l.actions) track(a);
  }
  for (const auto& a : t_.prog.initial) track(a);
}

void Router::push(const std::vector<ob::Action>& as, int64_t qty) {
  for (const auto& a : as) q_.push_back({a.kind, a.arg, qty});
}

bool Router::recv_one(const Options& o) {
  proto::Any m;
  if (!ch_.try_recv(m)) {
    const uint64_t t0 = net::now_ns();
    bool got = false;
    while (ch_.alive()) {
      if (ch_.try_recv(m)) {
        got = true;
        break;
      }
      if (o.max_ns_wait && net::now_ns() - t0 > o.max_ns_wait) break;
    }
    st_.ns_waiting += net::now_ns() - t0;
    if (!got) return false;
  }
  handle(m);
  return true;
}

void Router::handle(const proto::Any& m) {
  ++st_.messages_in;
  switch (m.type()) {
    case proto::Fill: {
      const auto& f = m.as<proto::FillMsg>();
      ++st_.fills;
      if (f.is_maker && f.remaining > 0) ++st_.partial_fills;
      const int tmpl = static_cast<int>(f.tag);
      // A stop that fired inside its own submission is the order being processed: its fill
      // actions wait for its ack like any taker's, so accept links (fired at submission in the
      // reference) go first.
      const bool own = f.is_maker == 2 && !inflight_.empty() && inflight_.front().kind == 0 && inflight_.front().tmpl == tmpl;
      if (f.is_maker && !own) {
        // A resting order traded, or a stop fired and took: its links fire now, as in the
        // reference engine, where every print queues its links at once.
        // Orders that are not the router's (registers loaded by the driver, a keyboard) carry
        // tags outside the table and have no links.
        if (tmpl < 0 || tmpl >= static_cast<int>(fill_links_.size())) break;
        for (const auto& L : fill_links_[tmpl])
          if (!L.has_at || L.at == f.price) push(L.actions, f.qty);
        if (f.remaining == 0 && tracked_[tmpl]) {
          auto& ids = live_[tmpl];
          ids.erase(std::remove(ids.begin(), ids.end(), f.order_id), ids.end());
        }
      } else {
        // The order being processed is the oldest unacknowledged one.
        if (inflight_.empty() || inflight_.front().kind != 0 || inflight_.front().tmpl != tmpl)
          throw std::runtime_error("taker fill for an order that is not at the head of the in-flight queue");
        for (const auto& L : fill_links_[tmpl])
          if (!L.has_at || L.at == f.price)
            for (const auto& a : L.actions) inflight_.front().fills.push_back({a.kind, a.arg, f.qty});
      }
      break;
    }
    case proto::Ack: {
      const auto& a = m.as<proto::AckMsg>();
      if (proto::unsolicited(a.status)) {
        // the order is gone (expired, cancelled by its group or parent, or a fired stop finished)
        if (a.status != proto::Activated && a.status != proto::GroupReduced) {
          const int tmpl = static_cast<int>(a.tag);
          if (tmpl >= 0 && tmpl < static_cast<int>(live_.size())) {
            auto& ids = live_[tmpl];
            ids.erase(std::remove(ids.begin(), ids.end(), a.order_id), ids.end());
          }
        }
        break;
      }
      if (inflight_.empty()) throw std::runtime_error("ack with nothing in flight");
      Inflight f = std::move(inflight_.front());
      inflight_.pop_front();
      if (f.kind == 0) {
        if (static_cast<int>(a.tag) != f.tmpl) throw std::runtime_error("ack tag does not match the in-flight order");
        if (a.status == proto::Done && t_.prog.templates[f.tmpl].type == ob::Type::IOC && a.remaining > 0)
          ++st_.ioc_remainders;
        if (a.status != proto::Rejected) {
          push(accept_links_[f.tmpl], 0);  // the reference fires accept links at submission
          if (a.status == proto::Rested && tracked_[f.tmpl]) live_[f.tmpl].push_back(a.order_id);
        }
        for (const auto& x : f.fills) q_.push_back(x);
      }
      break;
    }
    case proto::ControlReply:
      if (!inflight_.empty() && inflight_.front().kind == 3) inflight_.pop_front();
      break;
    case proto::Hello:
      break;
    default:
      throw std::runtime_error("unexpected message type " + std::to_string(m.type()));
  }
}

bool Router::drain(const Options& o) {
  ch_.flush();
  if (!inflight_.empty() && sent) {
    ++st_.waits;
    sent = false;
  }
  while (!inflight_.empty())
    if (!recv_one(o)) return false;
  return true;
}

Status Router::run(const Options& o) {
  const uint64_t t0 = net::now_ns();
  push(t_.prog.initial, 0);
  Status status = Status::Idle;
  uint64_t next_frame_ns = 0;
  const uint64_t frame_ns = o.max_fps > 0 ? static_cast<uint64_t>(1e9 / o.max_fps) : 0;
  auto finish = [&](Status s) {
    drain(o);
    st_.ns_total = net::now_ns() - t0;
    return s;
  };
  for (;;) {
    while (!q_.empty()) {
      const QAction a = q_.front();
      q_.pop_front();
      switch (a.kind) {
        case ob::Act::Submit: {
          const int j = t_.entry_instr[a.arg];
          if (j >= 0) {
            if (st_.instructions >= o.max_instr) return finish(Status::StepLimit);
            if (on_instr && !on_instr(j, st_.instructions)) return finish(Status::StepLimit);
            ++st_.instructions;
            const int mk = t_.marker_of(j);
            if (mk >= 0) {
              if (mk == 0) {
                ++st_.frames;
                if (frame_ns) {
                  // pace: flush what is pending, then wait for the frame deadline
                  ch_.flush();
                  const uint64_t now = net::now_ns();
                  if (next_frame_ns > now) {
                    const uint64_t wait = next_frame_ns - now;
                    if (wait > 2'000'000) std::this_thread::sleep_for(std::chrono::nanoseconds(wait - 1'000'000));
                    net::spin_for_ns(next_frame_ns - net::now_ns());
                  }
                  next_frame_ns = std::max(next_frame_ns, now) + frame_ns;
                }
              }
              if (on_marker) on_marker(mk);
            }
          }
          ch_.send(order_of(t_.prog.templates[a.arg], static_cast<uint32_t>(a.arg)));
          inflight_.push_back({0, a.arg, {}});
          ++st_.orders_sent;
          if (t_.prog.templates[a.arg].type == ob::Type::StopMarket || t_.prog.templates[a.arg].type == ob::Type::StopLimit)
            ++st_.stops_sent;
          sent = true;
          if (inflight_.size() % 256 == 0) {
            // a long unconditional burst: push it out and take whatever replies are ready,
            // so a full socket in either direction can never wedge the two processes
            ch_.flush();
            proto::Any peek;
            while (!inflight_.empty() && ch_.try_recv(peek)) handle(peek);
          }
          break;
        }
        case ob::Act::Cancel: {
          if (!drain(o)) return finish(ch_.alive() ? Status::Timeout : Status::Dead);
          for (uint64_t id : live_[a.arg]) {
            proto::CancelMsg c;
            c.tag = static_cast<uint32_t>(a.arg);
            c.order_id = id;
            ch_.send(c);
            inflight_.push_back({1, a.arg, {}});
            ++st_.cancels_sent;
            sent = true;
          }
          live_[a.arg].clear();
          break;
        }
        case ob::Act::Reduce: {
          if (!drain(o)) return finish(ch_.alive() ? Status::Timeout : Status::Dead);
          for (uint64_t id : live_[a.arg]) {
            proto::ReduceMsg r;
            r.tag = static_cast<uint32_t>(a.arg);
            r.order_id = id;
            r.by = a.qty;
            ch_.send(r);
            inflight_.push_back({2, a.arg, {}});
            sent = true;
          }
          break;
        }
        case ob::Act::Halt:
          if (st_.instructions >= o.max_instr) return finish(Status::StepLimit);
          if (on_instr && !on_instr(a.arg, st_.instructions)) return finish(Status::StepLimit);
          ++st_.instructions;
          st_.halt_code = a.arg;
          q_.clear();
          return finish(Status::Halted);
      }
    }
    ch_.flush();
    if (inflight_.empty()) return finish(Status::Idle);
    if (sent) {
      // The next action depends on a reply: one dependency round trip.
      ++st_.waits;
      sent = false;
    }
    if (!recv_one(o)) {
      status = ch_.alive() ? Status::Timeout : Status::Dead;
      st_.ns_total = net::now_ns() - t0;
      return status;
    }
  }
}

proto::AckMsg submit_sync(net::Channel& ch, const proto::NewOrderMsg& m) {
  ch.send(m);
  ch.flush();
  proto::Any a;
  for (;;) {
    if (!ch.recv(a)) throw std::runtime_error("exchange connection lost");
    if (a.type() == proto::Ack && !proto::unsolicited(a.as<proto::AckMsg>().status)) return a.as<proto::AckMsg>();
  }
}

proto::ControlReplyMsg control(net::Channel& ch, uint8_t op, int64_t arg, int64_t arg2) {
  proto::ControlMsg c;
  c.op = op;
  c.arg = arg;
  c.arg2 = arg2;
  ch.send(c);
  ch.flush();
  proto::Any a;
  for (;;) {
    if (!ch.recv(a)) throw std::runtime_error("exchange connection lost");
    if (a.type() == proto::ControlReply) return a.as<proto::ControlReplyMsg>();
  }
}

int64_t book_qty(net::Channel& ch, uint32_t book, uint8_t side, int64_t price) {
  return control(ch, proto::BookQty, proto::pack_book_side(book, side), price).arg;
}

}  // namespace rt
