// The router: Weinberger's "smart order router" made literal. It holds a fixed table
// (template -> order; (template, fill price) -> actions) and nothing else about the program:
// no counters, no program counter. Every bit of program state is resting quantity in the
// exchange. The router's only runtime state is bookkeeping about messages in flight and the
// ids of live orders it may later have to cancel.
//
// Semantics match stage 1's ob::Engine work queue: an action list is a FIFO; a Submit sends an
// order; the actions a fill triggers are appended to the back of the queue. The router pipelines:
// it keeps sending while the queue is non-empty and only waits when the next action depends on
// a reply. For the S1 gadgets that is exactly one wait per instruction.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "table.hpp"
#include "transport.hpp"

namespace rt {

struct Options {
  uint64_t max_instr = UINT64_MAX;
  double max_fps = 0;        // 0 = unlimited; otherwise the router paces frame markers
  uint64_t max_ns_wait = 0;  // 0 = forever; otherwise give up if a reply takes longer (UDP loss)
};

struct Stats {
  uint64_t instructions = 0, orders_sent = 0, cancels_sent = 0, fills = 0, messages_in = 0;
  uint64_t waits = 0;        // dependency round trips: times the next action needed a reply
  uint64_t frames = 0;       // marker 0 entries
  // Which matching rules the run exercised (for the canary's coverage column).
  uint64_t partial_fills = 0;    // maker fills that left the maker with remaining quantity
  uint64_t ioc_remainders = 0;   // IOC orders that did not fill completely
  uint64_t stops_sent = 0;       // stop orders submitted
  uint64_t ns_total = 0, ns_waiting = 0;
  int halt_code = -1;
};

enum class Status { Halted, StepLimit, Idle, Dead, Timeout };
const char* status_name(Status s);

class Router {
 public:
  Router(const tbl::Table& t, net::Channel& ch);
  Status run(const Options& o);
  // Called when an instruction is entered (before its order is sent). Return false to stop.
  std::function<bool(int instr, uint64_t count)> on_instr;
  // Called when a marker instruction is entered (marker index).
  std::function<void(int marker)> on_marker;
  const Stats& stats() const { return st_; }

 private:
  struct QAction {
    ob::Act kind;
    int arg;
    int64_t qty;
  };
  struct FillLink {
    bool has_at;
    int64_t at;
    std::vector<ob::Action> actions;
  };
  struct Inflight {
    uint8_t kind;  // 0 order, 1 cancel, 2 reduce, 3 control
    int tmpl;
    std::vector<QAction> fills;
  };
  bool recv_one(const Options& o);
  void handle(const proto::Any& m);
  void push(const std::vector<ob::Action>& as, int64_t qty);
  bool drain(const Options& o);

  const tbl::Table& t_;
  net::Channel& ch_;
  std::vector<std::vector<FillLink>> fill_links_;
  std::vector<std::vector<ob::Action>> accept_links_;
  std::vector<uint8_t> tracked_;
  std::vector<std::vector<uint64_t>> live_;
  std::deque<QAction> q_;
  std::deque<Inflight> inflight_;
  bool sent = false;  // something was sent since the last wait
  Stats st_;
};

// Synchronous helpers for drivers and tests (nothing else may be in flight on the channel).
proto::AckMsg submit_sync(net::Channel& ch, const proto::NewOrderMsg& m);
proto::ControlReplyMsg control(net::Channel& ch, uint8_t op, int64_t arg = 0, int64_t arg2 = 0);
int64_t book_qty(net::Channel& ch, uint32_t book, uint8_t side, int64_t price);
proto::NewOrderMsg order_of(const ob::Template& t, uint32_t tag);

}  // namespace rt
