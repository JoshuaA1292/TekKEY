#include "program_json.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace bk {

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw Invalid("cannot read " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

json::Value read_json(const std::string& path) { return json::parse(read_file(path)); }

Instruments load_instruments(const json::Value& v) {
  if (!v.is_object()) throw Invalid("instruments.json must be an object");
  Instruments r;
  uint32_t book = 0;
  for (const auto& [name, spec] : v.as_object()) {
    if (name == "contract_version") continue;
    Instrument i;
    i.name = name;
    i.tick_cents = spec.int_or("tick_cents", 100);
    i.lot_desc = spec.str_or("lot_desc", "");
    if (i.tick_cents <= 0) throw Invalid("instrument " + name + ": tick_cents must be positive");
    i.book = book++;
    r.list.push_back(i);
  }
  return r;
}

Instruments find_instruments(const std::string& explicit_path) {
  if (!explicit_path.empty()) return load_instruments(read_json(explicit_path));
  std::vector<std::string> tries;
  if (const char* e = std::getenv("BK_INSTRUMENTS")) tries.push_back(e);
  tries.push_back("instruments.json");
  tries.push_back("fixtures/instruments.json");
  tries.push_back("../fixtures/instruments.json");
  for (const auto& t : tries) {
    std::ifstream f(t);
    if (f) return load_instruments(read_json(t));
  }
  throw Invalid("no instruments.json (pass --instruments or set BK_INSTRUMENTS)");
}

int64_t bps100(double bps) { return static_cast<int64_t>(std::llround(bps * 100.0)); }

Envelope load_envelope(const json::Value& v) {
  Envelope e;
  e.raw = v;
  e.account = v.int_or("account", 0);
  e.max_loss_cents = v.at("max_loss_cents").as_int();
  e.max_fees_cents = v.at("max_fees_cents").as_int();
  e.max_trades = v.at("max_trades").as_int();
  e.max_exposure_lots = v.at("max_exposure_lots").as_int();
  if (const json::Value* is = v.get("instruments"))
    for (const auto& s : is->as_array()) e.instruments.push_back(s.as_string());
  if (const json::Value* r = v.get("require_live_stop")) e.require_live_stop = r->as_bool();
  const json::Value& env = v.at("env");
  e.max_gap_ticks = env.at("max_gap_ticks").as_int();
  e.fill = env.str_or("fill", "full_at_print");
  if (e.fill != "full_at_print") throw Invalid("env.fill: only \"full_at_print\" is implemented");
  if (const json::Value* f = env.get("fee_bps")) {
    e.maker_bps = f->at("maker").as_double();
    e.taker_bps = f->at("taker").as_double();
  }
  e.fees.maker_bps100 = bps100(e.maker_bps);
  e.fees.taker_bps100 = bps100(e.taker_bps);
  e.trigger_ref = env.str_or("trigger_ref", "last");
  if (e.trigger_ref != "last" && e.trigger_ref != "mark") throw Invalid("env.trigger_ref must be \"last\" or \"mark\"");
  if (e.max_gap_ticks < 1) throw Invalid("env.max_gap_ticks must be >= 1");
  if (e.max_loss_cents < 0 || e.max_fees_cents < 0 || e.max_trades < 0 || e.max_exposure_lots < 0)
    throw Invalid("envelope limits must be non-negative");
  return e;
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Limit: return "limit";
    case Kind::StopLimit: return "stop_limit";
    case Kind::StopMarket: return "stop_market";
    case Kind::Halt: return "halt";
  }
  return "?";
}

namespace {
Kind parse_kind(const std::string& s) {
  if (s == "limit") return Kind::Limit;
  if (s == "stop_limit") return Kind::StopLimit;
  if (s == "stop_market") return Kind::StopMarket;
  if (s == "halt") return Kind::Halt;
  throw Invalid("unknown node kind \"" + s + "\"");
}
std::optional<int64_t> opt_int(const json::Value& n, const char* k) {
  const json::Value* v = n.get(k);
  if (!v || v->is_null()) return std::nullopt;
  return v->as_int();
}
std::string opt_str(const json::Value& n, const char* k) {
  const json::Value* v = n.get(k);
  if (!v || v->is_null()) return "";
  return v->as_string();
}
void check_price(const Node& n, int64_t p, const char* what) {
  if (p <= 0 || p > fb::kMaxPrice) throw Invalid("node " + n.id + ": " + what + " out of range");
}
}  // namespace

Program load_program(const json::Value& v, const Instruments& ins) {
  Program p;
  if (v.int_or("contract_version", kContractVersion) != kContractVersion) throw Invalid("unsupported contract_version");
  p.backend = v.str_or("backend", "minsky");
  p.source_hash = v.str_or("source_hash", "");
  for (const auto& s : v.at("instruments").as_array()) {
    const std::string& name = s.as_string();
    ins.at(name);
    for (const auto& q : p.instruments)
      if (q == name) throw Invalid("instrument " + name + " listed twice");
    p.instruments.push_back(name);
  }
  if (p.instruments.empty()) throw Invalid("a program needs at least one instrument");
  if (p.instruments.size() > kMaxInstruments) throw Invalid("at most 3 instruments");

  const auto& nodes = v.at("nodes").as_array();
  if (nodes.empty()) throw Invalid("a program needs at least one node");
  if (nodes.size() > kMaxNodes) throw Invalid("at most 64 nodes");
  std::set<std::string> ids;
  for (const auto& jn : nodes) {
    Node n;
    n.index = static_cast<int>(p.nodes.size());
    n.id = jn.at("id").as_string();
    if (n.id.empty() || !ids.insert(n.id).second) throw Invalid("node ids must be unique and non-empty: \"" + n.id + "\"");
    n.kind = parse_kind(jn.at("kind").as_string());
    n.instrument = jn.at("instrument").as_string();
    n.instrument_index = -1;
    for (size_t i = 0; i < p.instruments.size(); ++i)
      if (p.instruments[i] == n.instrument) n.instrument_index = static_cast<int>(i);
    if (n.instrument_index < 0) throw Invalid("node " + n.id + ": instrument not in the program's instruments");
    n.reduce_only = jn.get("reduce_only") && jn.at("reduce_only").as_bool();
    n.terminal = jn.get("terminal") && jn.at("terminal").as_bool();
    n.parent = opt_str(jn, "parent");
    n.oco = opt_str(jn, "oco");
    n.price = opt_int(jn, "price");
    n.trigger = opt_int(jn, "trigger");
    if (n.kind == Kind::Halt) {
      const std::string side = opt_str(jn, "side");
      n.side = side == "sell" ? proto::Sell : proto::Buy;
      n.qty = jn.int_or("qty", 0);
      if (!n.oco.empty()) throw Invalid("node " + n.id + ": a halt node cannot be in an OCO group");
    } else {
      const std::string side = jn.at("side").as_string();
      if (side != "buy" && side != "sell") throw Invalid("node " + n.id + ": side must be buy or sell");
      n.side = side == "buy" ? proto::Buy : proto::Sell;
      n.qty = jn.at("qty").as_int();
      if (n.qty <= 0 || n.qty > fb::kMaxQty) throw Invalid("node " + n.id + ": qty out of range");
      if (n.kind != Kind::StopMarket) {
        if (!n.price) throw Invalid("node " + n.id + ": a " + kind_name(n.kind) + " needs a price");
        check_price(n, *n.price, "price");
      }
      if (n.is_stop()) {
        if (!n.trigger) throw Invalid("node " + n.id + ": a stop needs a trigger");
        check_price(n, *n.trigger, "trigger");
      } else if (n.trigger) {
        throw Invalid("node " + n.id + ": only stops have a trigger");
      }
    }
    p.nodes.push_back(n);
  }
  // parents
  for (auto& n : p.nodes) {
    if (n.parent.empty()) continue;
    n.parent_index = p.find(n.parent);
    if (n.parent_index < 0) throw Invalid("node " + n.id + ": parent \"" + n.parent + "\" does not exist");
    if (n.parent_index == n.index) throw Invalid("node " + n.id + " is its own parent");
    if (p.nodes[n.parent_index].kind == Kind::Halt) throw Invalid("node " + n.id + ": a halt node cannot be a parent");
  }
  // OCO groups, labels in order of first appearance
  std::map<std::string, int> members;
  for (auto& n : p.nodes) {
    if (n.oco.empty()) continue;
    if (!members.count(n.oco)) p.groups.push_back(n.oco);
    ++members[n.oco];
    for (size_t g = 0; g < p.groups.size(); ++g)
      if (p.groups[g] == n.oco) n.group_index = static_cast<int>(g);
  }
  for (const auto& [g, c] : members)
    if (c < 2) throw Invalid("OCO group " + g + " has fewer than 2 members");
  // deploy plan: every native node exactly once, parents in an earlier generation
  std::map<std::string, int> gen_of;
  int g = 0;
  for (const auto& gen : v.at("deploy_plan").as_array()) {
    std::vector<std::string> ids_in;
    for (const auto& s : gen.as_array()) {
      const std::string& id = s.as_string();
      const int i = p.find(id);
      if (i < 0) throw Invalid("deploy_plan names unknown node \"" + id + "\"");
      if (!p.nodes[i].native()) throw Invalid("deploy_plan names halt node \"" + id + "\"");
      if (gen_of.count(id)) throw Invalid("deploy_plan names \"" + id + "\" twice");
      gen_of[id] = g;
      ids_in.push_back(id);
    }
    p.deploy_plan.push_back(ids_in);
    ++g;
  }
  for (const auto& n : p.nodes) {
    if (!n.native()) continue;
    if (!gen_of.count(n.id)) throw Invalid("node " + n.id + " is not in the deploy plan");
    if (n.parent_index >= 0 && !(gen_of[n.parent] < gen_of[n.id]))
      throw Invalid("node " + n.id + ": parent must be deployed in an earlier generation");
  }
  if (const json::Value* r = v.get("report")) {
    p.report.nodes_total = r->int_or("nodes_total", 0);
    p.report.native = r->int_or("native", 0);
    p.report.relayed = r->int_or("relayed", 0);
    p.report.locked_collateral_cents = r->int_or("locked_collateral_cents", 0);
  }
  if (p.backend == "minsky" && p.report.relayed != 0) throw Invalid("backend minsky runs every node natively");
  if (const json::Value* w = v.get("warnings"))
    for (const auto& s : w->as_array()) p.warnings.push_back(s.as_string());
  return p;
}

json::Value to_json(const Program& p) {
  json::Value nodes = json::Value::array();
  for (const auto& n : p.nodes) {
    json::Value o = json::obj({{"id", n.id}, {"kind", kind_name(n.kind)}, {"instrument", n.instrument}});
    if (n.kind == Kind::Halt) {
      o.set("side", nullptr);
      o.set("qty", 0);
    } else {
      o.set("side", n.side == proto::Buy ? "buy" : "sell");
      o.set("qty", n.qty);
    }
    o.set("price", n.price ? json::Value(static_cast<long long>(*n.price)) : json::Value(nullptr));
    o.set("trigger", n.trigger ? json::Value(static_cast<long long>(*n.trigger)) : json::Value(nullptr));
    o.set("reduce_only", n.reduce_only);
    o.set("parent", n.parent.empty() ? json::Value(nullptr) : json::Value(n.parent));
    o.set("oco", n.oco.empty() ? json::Value(nullptr) : json::Value(n.oco));
    o.set("terminal", n.terminal);
    nodes.push(o);
  }
  json::Value plan = json::Value::array();
  for (const auto& gen : p.deploy_plan) {
    json::Value a = json::Value::array();
    for (const auto& id : gen) a.push(id);
    plan.push(a);
  }
  json::Value ins = json::Value::array();
  for (const auto& i : p.instruments) ins.push(i);
  json::Value warns = json::Value::array();
  for (const auto& w : p.warnings) warns.push(w);
  return json::obj({{"contract_version", kContractVersion},
                    {"backend", p.backend},
                    {"source_hash", p.source_hash},
                    {"instruments", ins},
                    {"nodes", nodes},
                    {"deploy_plan", plan},
                    {"report", json::obj({{"nodes_total", p.report.nodes_total},
                                          {"native", p.report.native},
                                          {"relayed", p.report.relayed},
                                          {"locked_collateral_cents", p.report.locked_collateral_cents}})},
                    {"warnings", warns}});
}

proto::NewOrderMsg order_for(const Node& n, uint8_t account, uint32_t book, fb::OrderId parent_id, uint32_t oco_id,
                             uint32_t tag) {
  if (!n.native()) throw Invalid("node " + n.id + " has no order");
  proto::NewOrderMsg m;
  m.account = account;
  m.side = n.side;
  m.otype = n.kind == Kind::Limit ? proto::Limit : n.kind == Kind::StopLimit ? proto::StopLimit : proto::StopMarket;
  m.reduce_only = n.reduce_only ? 1 : 0;
  m.tag = tag;
  m.book = book;
  m.price = n.price.value_or(0);
  m.trigger = n.trigger.value_or(0);
  m.qty = n.qty;
  m.parent = parent_id;
  m.oco = oco_id;
  m.oco_mode = proto::OcoReduce;
  return m;
}

int64_t locked_collateral_cents(const Program& p, const Instruments& ins) {
  int64_t sum = 0;
  for (const auto& n : p.nodes)
    if (n.kind == Kind::Limit && !n.reduce_only) sum += *n.price * n.qty * ins.at(n.instrument).tick_cents;
  return sum;
}

}  // namespace bk
