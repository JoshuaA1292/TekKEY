#include "engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace ob {

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

bool is_stop(Type t) { return t == Type::StopMarket || t == Type::StopLimit; }

bool stop_fires(const Template& t, const std::optional<Price>& last) {
  if (!last) return false;
  return t.side == Side::Sell ? *last <= t.trigger : *last >= t.trigger;
}
}  // namespace

Engine::Engine(int books, int accounts, STP stp, const Program* prog)
    : nbooks_(books), naccts_(accounts), stp_(stp), prog_(prog), books_(books),
      pos_(accounts, std::vector<Qty>(books, 0)), cash_(accounts, 0) {
  if (!prog_) return;
  links_by_src_.assign(prog_->templates.size(), {});
  auto track = [&](const Action& a) {
    if (a.kind == Act::Cancel || a.kind == Act::Reduce) tracked_.insert(a.arg);
  };
  for (size_t i = 0; i < prog_->links.size(); ++i) {
    links_by_src_.at(prog_->links[i].src).push_back(static_cast<int>(i));
    for (const auto& a : prog_->links[i].actions) track(a);
  }
  for (const auto& a : prog_->initial) track(a);
}

void Engine::start() {
  if (!prog_) return;
  for (const auto& a : prog_->initial) work_.push_back({false, 0, a, 0});
}

OrderId Engine::submit_tmpl(const Template& t, int tmpl) {
  if (t.book < 0 || t.book >= nbooks_ || t.account < 0 || t.account >= naccts_ || t.qty <= 0)
    throw std::invalid_argument("bad order");
  Order o{next_id_++, tmpl, t};
  // A reduce-only order with nothing to reduce is rejected outright: it is never accepted.
  if (!is_stop(t.type) && t.reduce_only && reduce_cap(o) == 0) return o.id;
  if (tmpl >= 0 && prog_) {
    for (int li : links_by_src_[tmpl]) {
      const Link& L = prog_->links[li];
      if (L.on == On::Accept)
        for (const auto& a : L.actions) work_.push_back({false, 0, a, 0});
    }
    if (tracked_.count(tmpl)) by_tmpl_[tmpl].push_back(o.id);
  }
  if (is_stop(t.type)) {
    Book& bk = books_[t.book];
    live_.emplace(o.id, o);
    if (stop_fires(t, bk.last))
      work_.push_back({true, o.id, {}, 0});
    else
      bk.stops.push_back(o.id);
  } else {
    activate(o);
  }
  return o.id;
}

Qty Engine::reduce_cap(const Order& o) const {
  Qty p = pos_[o.t.account][o.t.book];
  return o.t.side == Side::Sell ? std::max<Qty>(p, 0) : std::max<Qty>(-p, 0);
}

void Engine::activate(Order& o) {
  if (o.t.reduce_only) o.t.qty = std::min(o.t.qty, reduce_cap(o));
  if (o.t.qty <= 0) return;
  match(o);
  if (o.t.qty > 0 && o.t.type == Type::Limit) rest(o);
}

void Engine::match(Order& o) {
  const int b = o.t.book;
  Book& bk = books_[b];
  const bool buy = o.t.side == Side::Buy;
  const bool any_price = o.t.type == Type::Market;

  auto sweep = [&](auto& side_map) {
    while (o.t.qty > 0 && !side_map.empty()) {
      auto it = side_map.begin();
      const Price px = it->first;
      if (!any_price && (buy ? px > o.t.price : px < o.t.price)) break;
      Level& lv = it->second;
      const OrderId mid = lv.q.front();
      Order& m = live_.at(mid);

      auto drop_maker = [&] {
        lv.total -= m.t.qty;
        lv.q.pop_front();
        if (lv.q.empty()) side_map.erase(it);
        live_.erase(mid);
      };

      if (stp_ != STP::None && m.t.account == o.t.account) {
        if (stp_ == STP::CancelTaker) {
          o.t.qty = 0;
          return;
        }
        drop_maker();  // ExpireMaker: resting order cancelled, no fill, taker continues
        continue;
      }
      Qty q = std::min(o.t.qty, m.t.qty);
      if (o.t.reduce_only) q = std::min(q, reduce_cap(o));
      if (q == 0) {
        o.t.qty = 0;
        return;
      }
      if (m.t.reduce_only) {
        const Qty mc = reduce_cap(m);
        if (mc == 0) {
          drop_maker();
          continue;
        }
        q = std::min(q, mc);
      }

      o.t.qty -= q;
      m.t.qty -= q;
      lv.total -= q;
      const int buyer = buy ? o.t.account : m.t.account;
      const int seller = buy ? m.t.account : o.t.account;
      pos_[buyer][b] += q;
      pos_[seller][b] -= q;
      cash_[buyer] -= q * px;
      cash_[seller] += q * px;
      bk.last = px;
      ++trade_count_;
      if (log_trades) trades.push_back({b, px, q, o.id, mid, buyer, seller});
      const int mtmpl = m.tmpl;
      if (m.t.qty == 0) {
        lv.q.pop_front();
        if (lv.q.empty()) side_map.erase(it);
        live_.erase(mid);
      }
      fill_links(mtmpl, px, q);  // maker first, then taker
      fill_links(o.tmpl, px, q);
      check_stops(b);
    }
  };
  if (buy)
    sweep(bk.asks);
  else
    sweep(bk.bids);
}

void Engine::rest(Order& o) {
  Book& bk = books_[o.t.book];
  Level& lv = o.t.side == Side::Buy ? bk.bids[o.t.price] : bk.asks[o.t.price];
  lv.q.push_back(o.id);
  lv.total += o.t.qty;
  live_.emplace(o.id, o);
  bk.peak_lots = std::max(bk.peak_lots, resting_lots(o.t.book));
}

void Engine::fill_links(int tmpl, Price px, Qty q) {
  if (tmpl < 0 || !prog_) return;
  for (int li : links_by_src_[tmpl]) {
    const Link& L = prog_->links[li];
    if (L.on == On::Fill && (!L.at || *L.at == px))
      for (const auto& a : L.actions) work_.push_back({false, 0, a, q});
  }
}

void Engine::check_stops(int b) {
  Book& bk = books_[b];
  if (bk.stops.empty()) return;
  std::vector<OrderId> keep;
  keep.reserve(bk.stops.size());
  for (OrderId id : bk.stops) {
    if (stop_fires(live_.at(id).t, bk.last))
      work_.push_back({true, id, {}, 0});
    else
      keep.push_back(id);
  }
  bk.stops.swap(keep);
}

bool Engine::unlink_from_book(const Order& o) {
  Book& bk = books_[o.t.book];
  if (is_stop(o.t.type)) {
    auto it = std::find(bk.stops.begin(), bk.stops.end(), o.id);
    if (it != bk.stops.end()) bk.stops.erase(it);
    return true;  // pending, or already queued as triggered
  }
  auto pull = [&](auto& side_map) {
    auto lit = side_map.find(o.t.price);
    if (lit == side_map.end()) return false;
    auto& q = lit->second.q;
    auto qit = std::find(q.begin(), q.end(), o.id);
    if (qit == q.end()) return false;
    q.erase(qit);
    lit->second.total -= o.t.qty;
    if (q.empty()) side_map.erase(lit);
    return true;
  };
  return o.t.side == Side::Buy ? pull(bk.bids) : pull(bk.asks);
}

bool Engine::cancel(OrderId id) {
  auto it = live_.find(id);
  if (it == live_.end()) return false;
  unlink_from_book(it->second);
  live_.erase(it);
  return true;
}

void Engine::exec(const Work& w) {
  if (w.is_stop) {
    auto it = live_.find(w.id);
    if (it == live_.end()) return;  // cancelled while queued
    Order o = it->second;
    live_.erase(it);
    o.t.type = o.t.type == Type::StopMarket ? Type::Market : Type::Limit;
    activate(o);
    return;
  }
  const Action& a = w.a;
  switch (a.kind) {
    case Act::Submit:
      submit_tmpl(prog_->templates.at(a.arg), a.arg);
      break;
    case Act::Cancel: {
      auto& ids = by_tmpl_[a.arg];
      for (OrderId id : ids) cancel(id);
      ids.clear();
      break;
    }
    case Act::Reduce: {
      auto& ids = by_tmpl_[a.arg];
      std::vector<OrderId> keep;
      for (OrderId id : ids) {
        auto it = live_.find(id);
        if (it == live_.end()) continue;
        Order& o = it->second;
        if (o.t.qty <= w.fill_qty) {
          cancel(id);
          continue;
        }
        if (!is_stop(o.t.type)) {
          Book& bk = books_[o.t.book];
          auto& lv = o.t.side == Side::Buy ? bk.bids[o.t.price] : bk.asks[o.t.price];
          lv.total -= w.fill_qty;
        }
        o.t.qty -= w.fill_qty;
        keep.push_back(id);
      }
      ids.swap(keep);
      break;
    }
    case Act::Halt:
      halted_ = true;
      halt_code_ = a.arg;
      work_.clear();
      break;
  }
}

Status Engine::run(uint64_t budget) {
  if (halted_) return Status::Halted;
  for (uint64_t n = 0; n < budget && !work_.empty(); ++n) {
    Work w = work_.front();
    work_.pop_front();
    ++work_done_;
    if (!w.is_stop && on_action) {
      on_action(w.a);
      if (stop_requested) return Status::Stopped;
    }
    exec(w);
    if (halted_) return Status::Halted;
  }
  return work_.empty() ? Status::Idle : Status::Budget;
}

Qty Engine::resting_qty(int book, Side side, Price px) const {
  const Book& bk = books_[book];
  if (side == Side::Buy) {
    auto it = bk.bids.find(px);
    return it == bk.bids.end() ? 0 : it->second.total;
  }
  auto it = bk.asks.find(px);
  return it == bk.asks.end() ? 0 : it->second.total;
}

size_t Engine::resting_orders(int b) const {
  size_t n = 0;
  for (const auto& [p, lv] : books_[b].bids) n += lv.q.size();
  for (const auto& [p, lv] : books_[b].asks) n += lv.q.size();
  return n;
}

Qty Engine::resting_lots(int b) const {
  Qty n = 0;
  for (const auto& [p, lv] : books_[b].bids) n += lv.total;
  for (const auto& [p, lv] : books_[b].asks) n += lv.total;
  return n;
}

size_t Engine::resting_orders() const {
  size_t n = 0;
  for (const auto& bk : books_) {
    for (const auto& [p, lv] : bk.bids) n += lv.q.size();
    for (const auto& [p, lv] : bk.asks) n += lv.q.size();
  }
  return n;
}

uint64_t Engine::book_hash(int b) const {
  Fnv f;
  const Book& bk = books_[b];
  auto orders = [&](const auto& side_map) {
    for (const auto& [px, lv] : side_map) {
      f.add(px);
      f.add(lv.total);
      for (OrderId id : lv.q) {
        const Order& o = live_.at(id);
        f.add(o.t.account);
        f.add(o.t.qty);
        f.add(o.tmpl);
        f.add(o.t.reduce_only);
      }
      f.add(-1);
    }
    f.add(-2);
  };
  orders(bk.asks);
  orders(bk.bids);
  for (OrderId id : bk.stops) {
    const Order& o = live_.at(id);
    f.add(o.t.account);
    f.add(static_cast<int>(o.t.side));
    f.add(static_cast<int>(o.t.type));
    f.add(o.t.trigger);
    f.add(o.t.price);
    f.add(o.t.qty);
    f.add(o.tmpl);
    f.add(o.t.reduce_only);
  }
  f.add(bk.last.has_value());
  f.add(bk.last.value_or(0));
  return f.h;
}

uint64_t Engine::hash() const {
  Fnv f;
  for (int b = 0; b < nbooks_; ++b) f.add(static_cast<int64_t>(book_hash(b)));
  for (int a = 0; a < naccts_; ++a) {
    for (int b = 0; b < nbooks_; ++b) f.add(pos_[a][b]);
    f.add(cash_[a]);
  }
  // queued work: triggered stops (by content, not id) and router actions
  for (const Work& w : work_) {
    f.add(w.is_stop);
    if (w.is_stop) {
      auto it = live_.find(w.id);
      if (it == live_.end()) {
        f.add(-3);
        continue;
      }
      const Order& o = it->second;
      f.add(o.t.account);
      f.add(o.t.book);
      f.add(static_cast<int>(o.t.side));
      f.add(static_cast<int>(o.t.type));
      f.add(o.t.price);
      f.add(o.t.qty);
      f.add(o.t.reduce_only);
      f.add(o.tmpl);
    } else {
      f.add(static_cast<int>(w.a.kind));
      f.add(w.a.arg);
      f.add(w.fill_qty);
    }
  }
  f.add(halted_);
  f.add(halt_code_);
  return f.h;
}

}  // namespace ob
