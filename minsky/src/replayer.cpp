#include "replayer.hpp"

#include <stdexcept>

namespace bk {

Replayer::Replayer(WirePort& port, const Program& p, const Instruments& ins, ReplayOptions opt)
    : port_(port), p_(p), opt_(std::move(opt)), n_(p.nodes.size()), k_(p.instruments.size()) {
  if (opt_.books.size() != k_) throw Invalid("replay: one book per instrument");
  ids_.assign(n_ + 2 * k_, 0);
  flatten_side_.assign(2 * k_, proto::Sell);
  for (const auto& name : p.instruments) ticks_.push_back(ins.at(name).tick_cents);
  if (opt_.journal_base < 0) opt_.journal_base = port_.fill_count(opt_.account);
  oco_next_ = opt_.oco_base;
}

void Replayer::open(const std::vector<int64_t>& marks) {
  for (size_t i = 0; i < k_; ++i) print(port_, opt_.env, opt_.books[i], marks[i], opt_.mark_mode, opt_.books);
}

void Replayer::deploy() {
  std::vector<uint32_t> group_ids(p_.groups.size());
  for (auto& g : group_ids) g = oco_next_++;
  for (const auto& gen : p_.deploy_plan)
    for (const auto& id : gen) {
      const Node& nd = p_.nodes[p_.find(id)];
      const fb::OrderId pid = nd.parent_index >= 0 ? ids_[nd.parent_index] : 0;
      if (nd.parent_index >= 0 && !pid) throw Invalid("deploy: parent of " + nd.id + " is gone");
      const auto m = order_for(nd, opt_.account, opt_.books[nd.instrument_index], pid,
                               nd.group_index >= 0 ? group_ids[nd.group_index] : 0, 0);
      const fb::Result r = port_.submit(m);
      if (r.status == proto::Rejected) throw Invalid("deploy: the exchange rejected node " + nd.id);
      ids_[nd.index] = r.id;
    }
}

void Replayer::apply(const PathMove& m) {
  switch (m.type) {
    case PathMove::Print:
    case PathMove::Gap: print(port_, opt_.env, opt_.books[m.inst], m.to, opt_.mark_mode, opt_.books); break;
    case PathMove::Pause:
      for (size_t i = 0; i < n_; ++i) {
        if (!ids_[i] || !p_.nodes[i].native() || p_.nodes[i].reduce_only) continue;
        const uint8_t st = port_.order_state(ids_[i]);
        if (st == proto::OrderResting || st == proto::OrderHeld) port_.cancel(ids_[i]);
      }
      break;
    case PathMove::Cancel:
      for (size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i] && port_.order_state(ids_[i]) != proto::OrderGone) port_.cancel(ids_[i]);
      for (size_t inst = 0; inst < k_; ++inst) {
        const int64_t pos = port_.position(opt_.account, opt_.books[inst]);
        const auto last = port_.last(opt_.books[inst]);
        if (!pos || !last) continue;
        const FlattenIds f = flatten(port_, opt_.account, opt_.books[inst], pos, *last, oco_next_++);
        ids_[n_ + 2 * inst] = f.stop;
        ids_[n_ + 2 * inst + 1] = f.limit;
        flatten_side_[2 * inst] = flatten_side_[2 * inst + 1] = pos > 0 ? proto::Sell : proto::Buy;
      }
      break;
  }
}

int Replayer::run(const std::vector<PathMove>& moves) {
  // the path opens with one print per instrument at its mark
  std::vector<int64_t> marks(k_, 0);
  for (size_t i = 0; i < k_; ++i) {
    if (i >= moves.size() || moves[i].type != PathMove::Print || moves[i].inst != static_cast<int>(i))
      throw Invalid("a path opens with one print per instrument at its mark");
    marks[i] = moves[i].to;
  }
  int skipped = 0;
  if (opt_.deploy) {
    open(marks);
    deploy();
  }
  for (const PathMove& m : moves) {
    if (!opt_.deploy && (m.type == PathMove::Pause || m.type == PathMove::Cancel)) {
      ++skipped;
      continue;
    }
    apply(m);
  }
  return skipped;
}

int Replayer::slot_of(fb::OrderId id) const {
  for (size_t i = 0; i < ids_.size(); ++i)
    if (ids_[i] == id) return static_cast<int>(i);
  return -1;
}

uint8_t Replayer::side_of(int slot) const {
  return slot < static_cast<int>(n_) ? p_.nodes[slot].side : flatten_side_[slot - n_];
}

Ledger Replayer::ledger() {
  Ledger led(k_);
  fills_.clear();
  const int64_t n = port_.fill_count(opt_.account);
  for (int64_t i = opt_.journal_base; i < n; ++i) {
    const WirePort::JFill f = port_.fill_at(opt_.account, i);
    int inst = -1;
    for (size_t j = 0; j < k_; ++j)
      if (opt_.books[j] == f.book) inst = static_cast<int>(j);
    if (inst < 0) continue;  // another book of this account: not this program
    const int64_t fee = led.fill(inst, f.side, f.px, f.q, f.maker, ticks_[inst], opt_.fees);
    fills_.push_back({slot_of(f.id), f.px, f.q, fee, f.maker});
  }
  return led;
}

std::vector<std::optional<int64_t>> Replayer::marks() {
  std::vector<std::optional<int64_t>> m(k_);
  for (size_t i = 0; i < k_; ++i) m[i] = port_.last(opt_.books[i]);
  return m;
}

int64_t Replayer::loss() { return ledger().loss(marks(), ticks_); }

void Replayer::cleanup() {
  for (size_t i = 0; i < ids_.size(); ++i)
    if (ids_[i] && port_.order_state(ids_[i]) != proto::OrderGone) port_.cancel(ids_[i]);
  for (size_t inst = 0; inst < k_; ++inst) {
    const uint32_t b = opt_.books[inst];
    const int64_t pos = port_.position(opt_.account, b);
    const auto last = port_.last(b);
    if (!pos || !last) continue;
    proto::NewOrderMsg liq;
    liq.account = opt_.env.e2;
    liq.side = pos > 0 ? proto::Buy : proto::Sell;
    liq.otype = proto::Limit;
    liq.book = b;
    liq.price = *last;
    liq.qty = kEnvDepth;
    const fb::Result l = port_.submit(liq);
    proto::NewOrderMsg out;
    out.account = opt_.account;
    out.side = pos > 0 ? proto::Sell : proto::Buy;
    out.otype = proto::Market;
    out.reduce_only = 1;
    out.book = b;
    out.qty = pos > 0 ? pos : -pos;
    port_.submit(out);
    if (l.status == proto::Rested) port_.cancel(l.id);
  }
}

uint32_t fresh_book(WirePort& port, uint8_t account, uint32_t from, uint32_t to) {
  for (uint32_t b = from; b < to; ++b) {
    if (port.last(b)) continue;
    if (port.control(proto::BookInfo, b).arg != 0) continue;
    if (port.position(account, b) != 0) continue;
    return b;
  }
  throw Invalid("no fresh book on the exchange in [" + std::to_string(from) + ", " + std::to_string(to) + ")");
}

}  // namespace bk
