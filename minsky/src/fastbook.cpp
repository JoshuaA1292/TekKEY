#include "fastbook.hpp"

#include <algorithm>
#include <stdexcept>

namespace fb {

using proto::Buy;
using proto::Sell;

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
inline bool is_stop(uint8_t t) { return t == proto::StopMarket || t == proto::StopLimit; }
inline int lowest_bit(const uint64_t* occ) {
  for (int w = 0; w < 4; ++w)
    if (occ[w]) return w * 64 + __builtin_ctzll(occ[w]);
  return -1;
}
inline int highest_bit(const uint64_t* occ) {
  for (int w = 3; w >= 0; --w)
    if (occ[w]) return w * 64 + 63 - __builtin_clzll(occ[w]);
  return -1;
}
// highest set bit with index <= hi (hi >= 0)
inline int highest_le(const uint64_t* occ, int hi) {
  for (int w = hi >> 6; w >= 0; --w) {
    uint64_t m = occ[w];
    if (w == (hi >> 6)) {
      int r = hi & 63;
      m &= r == 63 ? ~0ull : ((1ull << (r + 1)) - 1);
    }
    if (m) return w * 64 + 63 - __builtin_clzll(m);
  }
  return -1;
}
inline int lowest_ge(const uint64_t* occ, int lo, int width) {
  for (int w = lo >> 6; w < 4 && w * 64 < width; ++w) {
    uint64_t m = occ[w];
    if (w == (lo >> 6)) m &= ~0ull << (lo & 63);
    if (m) return w * 64 + __builtin_ctzll(m);
  }
  return -1;
}
}  // namespace

const char* mutation_name(uint64_t bit) {
  switch (bit) {
    case FifoAfterPartial: return "fifo-after-partial";
    case PriceInverted: return "price-inverted";
    case StopWrongSide: return "stop-wrong-side";
    case IocRests: return "ioc-rests";
  }
  return "?";
}

Core::Core(const Config& cfg, Sink* sink) : cfg_(cfg), sink_(sink), nbooks_(cfg.books) {
  if (cfg_.width == 0 || cfg_.width > 256) throw std::invalid_argument("width must be 1..256");
  books_.resize(nbooks_);
  for (auto& bk : books_) {
    bk.win[0].resize(cfg_.width);
    bk.win[1].resize(cfg_.width);
  }
  pool_.reserve(cfg_.pool);
  pool_.resize(cfg_.pool);
  for (uint32_t s = 0; s < cfg_.pool; ++s) {
    pool_[s].next = s + 1 < cfg_.pool ? s + 1 : NIL;
    pool_[s].id = static_cast<uint64_t>(s) << 32;  // generation 0: never a valid id
  }
  free_head_ = cfg_.pool ? 0 : NIL;
  pos_.assign(static_cast<size_t>(cfg_.accounts) * nbooks_, 0);
  cash_.assign(cfg_.accounts, 0);
}

uint32_t Core::alloc() {
  if (free_head_ == NIL) {
    const uint32_t old = static_cast<uint32_t>(pool_.size());
    const uint32_t grown = old ? old * 2 : 1024;
    pool_.resize(grown);
    for (uint32_t s = old; s < grown; ++s) {
      pool_[s].next = s + 1 < grown ? s + 1 : NIL;
      pool_[s].id = static_cast<uint64_t>(s) << 32;
      pool_[s].state = State::Free;
    }
    free_head_ = old;
  }
  const uint32_t s = free_head_;
  Order& o = pool_[s];
  free_head_ = o.next;
  uint32_t gen = static_cast<uint32_t>(o.id & 0xffffffffu) + 1;
  if (gen == 0) gen = 1;
  o.id = (static_cast<uint64_t>(s) << 32) | gen;
  o.next = o.prev = NIL;
  return s;
}

void Core::free_slot(uint32_t s) {
  Order& o = pool_[s];
  if (o.state == State::Resting || o.state == State::Pending || o.state == State::Queued || o.state == State::Held) --live_;
  if (o.oco) unregister_group(s);
  o.state = State::Free;
  o.next = free_head_;
  free_head_ = s;
}

const Order* Core::find(OrderId id) const {
  const uint32_t s = static_cast<uint32_t>(id >> 32);
  if (s >= pool_.size()) return nullptr;
  const Order& o = pool_[s];
  if (o.id != id || o.state == State::Free) return nullptr;
  return &o;
}

Core::Level* Core::level_of(Book& bk, uint8_t side, Price px, bool create) {
  if (px >= cfg_.base && px < cfg_.base + cfg_.width) return &bk.win[side][px - cfg_.base];
  auto& m = px < cfg_.base ? bk.below[side] : bk.above[side];
  if (create) return &m[px];
  auto it = m.find(px);
  return it == m.end() ? nullptr : &it->second;
}

const Core::Level* Core::level_of(const Book& bk, uint8_t side, Price px) const {
  if (px >= cfg_.base && px < cfg_.base + cfg_.width) {
    const Level& lv = bk.win[side][px - cfg_.base];
    return lv.count ? &lv : nullptr;
  }
  const auto& m = px < cfg_.base ? bk.below[side] : bk.above[side];
  auto it = m.find(px);
  return it == m.end() ? nullptr : &it->second;
}

void Core::drop_level_if_empty(Book& bk, uint8_t side, Price px, Level* lv) {
  if (lv->count) return;
  if (px >= cfg_.base && px < cfg_.base + cfg_.width) {
    const uint32_t i = static_cast<uint32_t>(px - cfg_.base);
    bk.occ[side][i >> 6] &= ~(1ull << (i & 63));
    return;
  }
  auto& m = px < cfg_.base ? bk.below[side] : bk.above[side];
  m.erase(px);
}

Core::Level* Core::eligible_level(Book& bk, uint8_t ms, bool any, Price limit, Price& px) {
  const bool ask = ms == Sell;
  auto ok = [&](Price p) { return any || (ask ? p <= limit : p >= limit); };
  if (!(cfg_.mutation & PriceInverted)) {
    // best level only: if the best is not eligible nothing is
    if (ask) {
      if (!bk.below[ms].empty()) {
        px = bk.below[ms].begin()->first;
        return ok(px) ? &bk.below[ms].begin()->second : nullptr;
      }
      int i = lowest_bit(bk.occ[ms]);
      if (i >= 0) {
        px = cfg_.base + i;
        return ok(px) ? &bk.win[ms][i] : nullptr;
      }
      if (!bk.above[ms].empty()) {
        px = bk.above[ms].begin()->first;
        return ok(px) ? &bk.above[ms].begin()->second : nullptr;
      }
      return nullptr;
    }
    if (!bk.above[ms].empty()) {
      auto it = std::prev(bk.above[ms].end());
      px = it->first;
      return ok(px) ? &it->second : nullptr;
    }
    int i = highest_bit(bk.occ[ms]);
    if (i >= 0) {
      px = cfg_.base + i;
      return ok(px) ? &bk.win[ms][i] : nullptr;
    }
    if (!bk.below[ms].empty()) {
      auto it = std::prev(bk.below[ms].end());
      px = it->first;
      return ok(px) ? &it->second : nullptr;
    }
    return nullptr;
  }
  // Mutation: the worst eligible level.
  if (ask) {
    for (auto it = bk.above[ms].rbegin(); it != bk.above[ms].rend(); ++it)
      if (ok(it->first)) {
        px = it->first;
        return &it->second;
      }
    const Price lim_i = any ? Price(cfg_.width) - 1 : std::min<Price>(std::max<Price>(limit - cfg_.base, -1), Price(cfg_.width) - 1);
    const int hi = static_cast<int>(lim_i);
    if (hi >= 0) {
      int i = highest_le(bk.occ[ms], hi);
      if (i >= 0) {
        px = cfg_.base + i;
        return &bk.win[ms][i];
      }
    }
    for (auto it = bk.below[ms].rbegin(); it != bk.below[ms].rend(); ++it)
      if (ok(it->first)) {
        px = it->first;
        return &it->second;
      }
    return nullptr;
  }
  for (auto it = bk.below[ms].begin(); it != bk.below[ms].end(); ++it)
    if (ok(it->first)) {
      px = it->first;
      return &it->second;
    }
  const Price lo_i = any ? 0 : std::min<Price>(std::max<Price>(limit - cfg_.base, 0), Price(cfg_.width));
  const int lo = static_cast<int>(lo_i);
  if (lo < static_cast<int>(cfg_.width)) {
    int i = lowest_ge(bk.occ[ms], lo, static_cast<int>(cfg_.width));
    if (i >= 0) {
      px = cfg_.base + i;
      return &bk.win[ms][i];
    }
  }
  for (auto it = bk.above[ms].begin(); it != bk.above[ms].end(); ++it)
    if (ok(it->first)) {
      px = it->first;
      return &it->second;
    }
  return nullptr;
}

void Core::emit_level(uint32_t b, uint8_t side, Price px, const Level* lv) {
  sink_->level(b, side, px, lv ? lv->total : 0, lv ? lv->count : 0);
}

Qty Core::reduce_cap(const Order& o) const {
  const Qty p = pos_[static_cast<size_t>(o.account) * nbooks_ + o.book];
  return o.side == Sell ? std::max<Qty>(p, 0) : std::max<Qty>(-p, 0);
}

bool Core::stop_fires(const Order& o, const std::optional<Price>& ref) const {
  if (!ref) return false;
  bool sell_rule = o.side == Sell;
  if (cfg_.mutation & StopWrongSide) sell_rule = !sell_rule;
  return sell_rule ? *ref <= o.trigger : *ref >= o.trigger;
}

void Core::rest(uint32_t s) {
  Order& o = pool_[s];
  Book& bk = books_[o.book];
  Level* lv = level_of(bk, o.side, o.price, true);
  o.prev = lv->tail;
  o.next = NIL;
  if (lv->tail != NIL) pool_[lv->tail].next = s;
  else lv->head = s;
  lv->tail = s;
  ++lv->count;
  lv->total += o.qty;
  bk.lots += o.qty;
  bk.peak = std::max(bk.peak, bk.lots);
  if (o.price >= cfg_.base && o.price < cfg_.base + cfg_.width) {
    const uint32_t i = static_cast<uint32_t>(o.price - cfg_.base);
    bk.occ[o.side][i >> 6] |= 1ull << (i & 63);
  }
  o.state = State::Resting;
  ++live_;
  emit_level(o.book, o.side, o.price, lv);
}

// Remove a resting order from its level (the order keeps its fields; the caller frees it).
void Core::unlink(uint32_t s) {
  Order& o = pool_[s];
  Book& bk = books_[o.book];
  Level* lv = level_of(bk, o.side, o.price, false);
  if (o.prev != NIL) pool_[o.prev].next = o.next;
  else lv->head = o.next;
  if (o.next != NIL) pool_[o.next].prev = o.prev;
  else lv->tail = o.prev;
  --lv->count;
  lv->total -= o.qty;
  bk.lots -= o.qty;
  const Qty total = lv->total;
  const uint32_t count = lv->count;
  drop_level_if_empty(bk, o.side, o.price, lv);
  sink_->level(o.book, o.side, o.price, total, count);
}

// A resting order removed by the venue mid-match (self-trade expiry, a reduce-only maker whose
// position vanished): its owner is told and its held children go with it.
void Core::drop_maker(uint32_t mslot) {
  Order& m = pool_[mslot];
  sink_->cancelled(m);
  unlink(mslot);
  const OrderId id = m.id;
  free_slot(mslot);
  cancel_children(id, proto::ParentCancelled);
}

void Core::match(uint32_t s) {
  Order& o = pool_[s];
  Book& bk = books_[o.book];
  const bool buy = o.side == Buy;
  const uint8_t ms = buy ? Sell : Buy;
  const bool any = o.type == proto::Market;
  while (o.qty > 0) {
    Price px;
    Level* lv = eligible_level(bk, ms, any, o.price, px);
    if (!lv) break;
    const uint32_t mslot = lv->head;
    Order& m = pool_[mslot];
    if (cfg_.stp != STP::None && m.account == o.account) {
      if (cfg_.stp == STP::CancelTaker) {
        o.qty = 0;
        return;
      }
      // ExpireMaker: the resting order is cancelled, no fill, the taker continues
      drop_maker(mslot);
      continue;
    }
    Qty q = std::min(o.qty, m.qty);
    if (o.reduce_only) q = std::min(q, reduce_cap(o));
    if (q == 0) {
      o.qty = 0;
      return;
    }
    if (m.reduce_only) {
      const Qty mc = reduce_cap(m);
      if (mc == 0) {
        drop_maker(mslot);
        continue;
      }
      q = std::min(q, mc);
    }
    o.qty -= q;
    m.qty -= q;
    o.filled += q;
    m.filled += q;
    lv->total -= q;
    bk.lots -= q;
    const uint8_t buyer = buy ? o.account : m.account;
    const uint8_t seller = buy ? m.account : o.account;
    pos_[static_cast<size_t>(buyer) * nbooks_ + o.book] += q;
    pos_[static_cast<size_t>(seller) * nbooks_ + o.book] -= q;
    cash_[buyer] -= q * px;
    cash_[seller] += q * px;
    bk.last = px;
    ++trades_;
    sink_->trade(o.book, px, q, buyer, seller);
    sink_->fill(m, true, px, q);
    sink_->fill(o, false, px, q);
    if (o.oco) work_.push_back({Work::GroupFill, o.qty == 0, o.oco_mode, s, o.oco, o.id, q});
    if (m.oco) work_.push_back({Work::GroupFill, m.qty == 0, m.oco_mode, mslot, m.oco, m.id, q});
    if (m.qty == 0) work_.push_back({Work::ParentDone, 0, 0, mslot, 0, m.id, 0});
    if (m.qty == 0) {
      // pop the head (its total is already subtracted; unlink subtracts the remaining 0)
      unlink(mslot);
      free_slot(mslot);
    } else {
      if ((cfg_.mutation & FifoAfterPartial) && lv->count > 1) {
        // the partially filled maker goes to the back of its level
        lv->head = m.next;
        pool_[m.next].prev = NIL;
        m.prev = lv->tail;
        m.next = NIL;
        pool_[lv->tail].next = mslot;
        lv->tail = mslot;
      }
      emit_level(o.book, ms, px, lv);
    }
    check_stops(bk, o.book);
  }
}

void Core::activate(uint32_t s) {
  Order& o = pool_[s];
  if (o.reduce_only) o.qty = std::min(o.qty, reduce_cap(o));
  if (o.qty <= 0) {
    free_slot(s);
    return;
  }
  match(s);
  Order& o2 = pool_[s];
  if (o2.qty > 0 && (o2.type == proto::Limit || (o2.type == proto::IOC && (cfg_.mutation & IocRests))))
    rest(s);
  else
    free_slot(s);
}

void Core::check_stops(Book& bk, uint32_t) {
  if (bk.stops.empty()) return;
  const std::optional<Price>& ref = trigger_ref(bk);
  size_t keep = 0;
  for (size_t i = 0; i < bk.stops.size(); ++i) {
    const uint32_t s = bk.stops[i];
    if (stop_fires(pool_[s], ref)) {
      pool_[s].state = State::Queued;
      work_.push_back({Work::FireStop, 0, 0, s, 0, pool_[s].id, 0});
    } else {
      bk.stops[keep++] = s;
    }
  }
  bk.stops.resize(keep);
}

// What happens once a non-stop order has matched: it rested, or it is done. For orders that
// had no submit of their own (fired stops, released children) the owner is told it finished,
// and a parent that filled completely releases its children; one that is gone unfilled drops them.
void Core::after_activate(uint32_t s, bool report) {
  Order& o = pool_[s];
  if (o.state == State::Resting) return;
  // activate() freed the slot; its fields are still readable until the next alloc
  if (report) sink_->finished(o);
  // complete: nothing remains and something traded (a reduce-only order the venue cut to the
  // position and then filled counts; one cut to nothing, or an IOC remainder, does not)
  if (o.qty == 0 && o.filled > 0) work_.push_back({Work::ParentDone, 0, 0, s, 0, o.id, 0});
  else cancel_children(o.id, proto::ParentCancelled);
}

// A held child whose parent filled completely enters the book as if just submitted.
void Core::release(uint32_t s) {
  Order& o = pool_[s];
  held_.erase(std::find(held_.begin(), held_.end(), s));
  o.parent = 0;
  sink_->activated(o);
  if (is_stop(o.type)) {
    Book& bk = books_[o.book];
    if (stop_fires(o, trigger_ref(bk))) {
      o.state = State::Queued;
      work_.push_back({Work::FireStop, 0, 0, s, 0, o.id, 0});
    } else {
      o.state = State::Pending;
      bk.stops.push_back(s);
    }
    return;
  }
  o.state = State::Matching;
  --live_;
  activate(s);
  after_activate(s, true);
}

void Core::drain() {
  while (work_head_ < work_.size()) {
    const Work w = work_[work_head_++];
    switch (w.kind) {
      case Work::FireStop: {
        Order& o = pool_[w.slot];
        if (o.state != State::Queued || o.id != w.id) continue;  // cancelled while queued
        o.type = o.type == proto::StopMarket ? proto::Market : proto::Limit;
        o.state = State::Matching;
        --live_;
        firing_ = true;
        activate(w.slot);
        firing_ = false;
        after_activate(w.slot, true);
        break;
      }
      case Work::ParentDone: {
        // release every child of this parent, oldest first (held_ is in submission order)
        std::vector<uint32_t> kids;
        for (uint32_t s : held_)
          if (pool_[s].parent == w.id) kids.push_back(s);
        for (uint32_t s : kids)
          if (pool_[s].state == State::Held) release(s);
        break;
      }
      case Work::GroupFill:
        group_effect(w);
        break;
    }
  }
  work_.clear();
  work_head_ = 0;
}

void Core::unregister_group(uint32_t s) {
  auto it = groups_.find(pool_[s].oco);
  if (it == groups_.end()) return;
  auto& v = it->second;
  v.erase(std::remove(v.begin(), v.end(), s), v.end());
  if (v.empty()) groups_.erase(it);
}

// Take a live order out of its book / stop list / held list without freeing it.
void Core::remove_live(uint32_t s) {
  Order& o = pool_[s];
  switch (o.state) {
    case State::Resting: unlink(s); break;
    case State::Pending: {
      auto& st = books_[o.book].stops;
      st.erase(std::find(st.begin(), st.end(), s));
      break;
    }
    case State::Held: held_.erase(std::find(held_.begin(), held_.end(), s)); break;
    default: break;  // Queued: the fire loop skips it
  }
}

void Core::cancel_children(OrderId parent, uint8_t why) {
  std::vector<uint32_t> kids;
  for (uint32_t s : held_)
    if (pool_[s].parent == parent) kids.push_back(s);
  for (uint32_t s : kids) {
    Order& o = pool_[s];
    if (o.state != State::Held) continue;
    sink_->group_cancelled(o, why);
    remove_live(s);
    const OrderId id = o.id;
    free_slot(s);
    cancel_children(id, proto::ParentCancelled);
  }
}

void Core::group_effect(const Work& w) {
  auto it = groups_.find(w.group);
  if (it == groups_.end()) return;
  if (w.mode == proto::OcoCancel && !w.full) return;
  std::vector<uint32_t> members = it->second;  // copy: the effects edit the group
  for (uint32_t s : members) {
    Order& o = pool_[s];
    if (o.id == w.id || o.state == State::Free || o.state == State::Matching) continue;
    if (w.mode == proto::OcoCancel || w.qty >= o.qty) {
      sink_->group_cancelled(o, proto::GroupCancelled);
      remove_live(s);
      const OrderId id = o.id;
      free_slot(s);
      cancel_children(id, proto::ParentCancelled);
      continue;
    }
    // reduce mode: shrink in place, keeping time priority
    if (o.state == State::Resting) {
      Book& bk = books_[o.book];
      Level* lv = level_of(bk, o.side, o.price, false);
      lv->total -= w.qty;
      bk.lots -= w.qty;
      o.qty -= w.qty;
      emit_level(o.book, o.side, o.price, lv);
    } else {
      o.qty -= w.qty;
    }
    sink_->group_reduced(o, w.qty);
  }
}

void Core::set_mark(uint32_t b, Price px) {
  if (b >= nbooks_) return;
  Book& bk = books_[b];
  bk.mark = px;
  check_stops(bk, b);
  drain();
}

Result Core::submit(const proto::NewOrderMsg& m, uint16_t session) {
  if (m.book >= nbooks_ || m.account >= cfg_.accounts || m.qty <= 0 || m.qty > kMaxQty || m.otype > proto::StopLimit ||
      m.side > Sell || m.price > kMaxPrice || m.price < -kMaxPrice || m.trigger > kMaxPrice || m.trigger < -kMaxPrice)
    return {proto::Rejected, 0, m.qty};
  ++orders_;
  const uint32_t s = alloc();
  Order& o = pool_[s];
  o.tag = m.tag;
  o.session = session;
  o.account = m.account;
  o.side = m.side;
  o.type = m.otype;
  o.reduce_only = m.reduce_only ? 1 : 0;
  o.book = m.book;
  o.price = m.price;
  o.trigger = m.trigger;
  o.qty = m.qty;
  o.orig = m.qty;
  o.filled = 0;
  o.parent = 0;
  o.oco = 0;
  o.oco_mode = 0;
  const OrderId id = o.id;
  if (m.parent != 0) {
    const Order* par = find(m.parent);
    // a parent must be live and able to fill: resting, a pending/queued stop, or itself held
    if (!par || par->state == State::Matching) {
      o.state = State::Free;
      free_slot(s);
      return {proto::Rejected, id, m.qty};
    }
  }
  if (!is_stop(o.type) && o.reduce_only && m.parent == 0 && reduce_cap(o) == 0) {
    o.state = State::Free;
    free_slot(s);
    return {proto::Rejected, id, m.qty};
  }
  if (m.oco != 0) {
    o.oco = m.oco;
    o.oco_mode = m.oco_mode == proto::OcoReduce ? proto::OcoReduce : proto::OcoCancel;
    groups_[m.oco].push_back(s);
  }
  if (m.parent != 0) {
    o.parent = m.parent;
    o.state = State::Held;
    ++live_;
    held_.push_back(s);
    drain();
    return {proto::Held, id, m.qty};
  }
  if (is_stop(o.type)) {
    Book& bk = books_[o.book];
    ++live_;
    if (stop_fires(o, trigger_ref(bk))) {
      o.state = State::Queued;
      work_.push_back({Work::FireStop, 0, 0, s, 0, o.id, 0});
    } else {
      o.state = State::Pending;
      bk.stops.push_back(s);
    }
  } else {
    o.state = State::Matching;
    activate(s);
    after_activate(s, false);
  }
  drain();
  // The slot cannot have been reused: nothing allocates between here and the submit.
  const Order& f = pool_[s];
  Result r;
  r.id = id;
  if (f.state == State::Resting || f.state == State::Pending) {
    r.status = proto::Rested;
    r.remaining = f.qty;
  } else {
    // Filled only if every lot traded; a taker cut short by self-trade prevention or a
    // reduce-only cap is Done with its real remainder.
    r.remaining = m.qty - f.filled;
    r.status = r.remaining == 0 ? proto::Filled : proto::Done;
  }
  return r;
}

Result Core::cancel(OrderId id) {
  const Order* c = find(id);
  if (!c) return {proto::NotFound, id, 0};
  const uint32_t s = static_cast<uint32_t>(id >> 32);
  Order& o = pool_[s];
  const Qty rem = o.qty;
  if (o.state == State::Free || o.state == State::Matching) return {proto::NotFound, id, 0};
  remove_live(s);
  free_slot(s);
  cancel_children(id, proto::ParentCancelled);
  return {proto::Cancelled, id, rem};
}

Result Core::reduce(OrderId id, Qty by) {
  const Order* c = find(id);
  if (!c) return {proto::NotFound, id, 0};
  const uint32_t s = static_cast<uint32_t>(id >> 32);
  Order& o = pool_[s];
  if (by >= o.qty) return cancel(id);
  if (by <= 0) return {proto::Reduced, id, o.qty};
  if (o.state == State::Resting) {
    Book& bk = books_[o.book];
    Level* lv = level_of(bk, o.side, o.price, false);
    lv->total -= by;
    bk.lots -= by;
    o.qty -= by;
    emit_level(o.book, o.side, o.price, lv);
  } else {
    o.qty -= by;
  }
  return {proto::Reduced, id, o.qty};
}

void Core::reset_all() {
  for (uint32_t b = 0; b < nbooks_; ++b) {
    Book& bk = books_[b];
    for (int side = 0; side < 2; ++side) {
      for (auto& lv : bk.win[side]) lv = Level{};
      for (auto& w : bk.occ[side]) w = 0;
      bk.below[side].clear();
      bk.above[side].clear();
    }
    bk.last.reset();
    bk.mark.reset();
    bk.stops.clear();
    bk.lots = bk.peak = 0;
    sink_->reset(b);
  }
  held_.clear();
  groups_.clear();
  for (uint32_t s = 0; s < pool_.size(); ++s) {
    pool_[s].state = State::Free;
    pool_[s].next = s + 1 < pool_.size() ? s + 1 : NIL;
  }
  free_head_ = pool_.empty() ? NIL : 0;
  work_.clear();
  work_head_ = 0;
  std::fill(pos_.begin(), pos_.end(), 0);
  std::fill(cash_.begin(), cash_.end(), 0);
  trades_ = orders_ = 0;
  live_ = 0;
}

Qty Core::resting_qty(uint32_t b, uint8_t side, Price px) const {
  const Level* lv = level_of(books_[b], side, px);
  return lv ? lv->total : 0;
}

size_t Core::resting_orders(uint32_t b) const {
  size_t n = 0;
  for_each_level(b, [&](uint8_t, Price, Qty, uint32_t count, auto&&) { n += count; });
  return n;
}

void Core::snapshot(uint32_t b) const {
  for_each_level(b, [&](uint8_t side, Price px, Qty total, uint32_t count, auto&&) {
    sink_->level(b, side, px, total, count);
  });
}

uint64_t Core::book_hash(uint32_t b) const {
  // Same recipe as ob::Engine::book_hash: [ask levels] -2 [bid levels] -2 [stops] last.
  Fnv f;
  const Book& bk = books_[b];
  bool asks_closed = false;
  for_each_level(b, [&](uint8_t side, Price px, Qty total, uint32_t, auto&& each) {
    if (side == Buy && !asks_closed) {
      f.add(-2);
      asks_closed = true;
    }
    f.add(px);
    f.add(total);
    each([&](const Order& o) {
      f.add(o.account);
      f.add(o.qty);
      f.add(-1);  // template id: the wire engine has none
      f.add(o.reduce_only);
    });
    f.add(-1);
  });
  if (!asks_closed) f.add(-2);
  f.add(-2);
  for (uint32_t s : bk.stops) {
    const Order& o = pool_[s];
    f.add(o.account);
    f.add(o.side);
    f.add(o.type);
    f.add(o.trigger);
    f.add(o.price);
    f.add(o.qty);
    f.add(-1);
    f.add(o.reduce_only);
  }
  f.add(bk.last.has_value());
  f.add(bk.last.value_or(0));
  if (bk.mark) {
    f.add(-4);
    f.add(*bk.mark);
  }
  return f.h;
}

uint64_t Core::hash() const {
  Fnv f;
  for (uint32_t b = 0; b < nbooks_; ++b) f.add(static_cast<int64_t>(book_hash(b)));
  for (uint8_t a = 0; a < cfg_.accounts; ++a) {
    for (uint32_t b = 0; b < nbooks_; ++b) f.add(pos_[static_cast<size_t>(a) * nbooks_ + b]);
    f.add(cash_[a]);
  }
  f.add(0);   // halted
  f.add(-1);  // halt code
  if (!held_.empty()) {
    f.add(-3);
    for (uint32_t s : held_) {
      const Order& o = pool_[s];
      f.add(o.account);
      f.add(o.book);
      f.add(o.side);
      f.add(o.type);
      f.add(o.price);
      f.add(o.trigger);
      f.add(o.qty);
      f.add(o.reduce_only);
      // the parent by its position among live orders would be canonical; its id is not, but
      // every verifier state derives from one submission order, so ids are reproducible there
      f.add(static_cast<int64_t>(o.parent));
      f.add(o.oco);
      f.add(o.oco_mode);
    }
  }
  if (!groups_.empty()) {
    f.add(-5);
    for (const auto& [g, v] : groups_) {
      f.add(g);
      f.add(static_cast<int64_t>(v.size()));
    }
  }
  return f.h;
}

}  // namespace fb
