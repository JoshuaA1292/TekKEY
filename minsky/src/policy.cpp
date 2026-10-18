#include "policy.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <vector>

#include "sha256.hpp"

namespace bk {

json::Value StaticError::to_json() const {
  return json::obj({{"contract_version", kContractVersion},
                    {"status", "static_error"},
                    {"line", line},
                    {"col", col},
                    {"message", what()}});
}

namespace {

// ------------------------------------------------------------------ lexer

struct Tok {
  enum T : uint8_t { Ident, Int, Sym, End } t = End;
  std::string s;
  int64_t v = 0;
  int line = 1, col = 1;
};

std::vector<Tok> lex(const std::string& src) {
  std::vector<Tok> out;
  int line = 1, col = 1;
  size_t i = 0;
  auto adv = [&](size_t n) {
    for (size_t k = 0; k < n; ++k, ++i) {
      if (src[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
  };
  while (i < src.size()) {
    const char c = src[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      adv(1);
      continue;
    }
    if (c == '#' || (c == '-' && i + 1 < src.size() && src[i + 1] == '-')) {  // comment to end of line
      while (i < src.size() && src[i] != '\n') adv(1);
      continue;
    }
    Tok t;
    t.line = line;
    t.col = col;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
      t.t = Tok::Ident;
      t.s = src.substr(i, j - i);
      adv(j - i);
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
      if (j < src.size() && (src[j] == '.' || std::isalpha(static_cast<unsigned char>(src[j])) || src[j] == '_'))
        throw StaticError("quantities are integer lots and prices integer ticks", line, col);
      t.t = Tok::Int;
      t.s = src.substr(i, j - i);
      if (t.s.size() > 12) throw StaticError("number too large", line, col);
      t.v = std::stoll(t.s);
      adv(j - i);
    } else if ((c == '<' || c == '>') && i + 1 < src.size() && src[i + 1] == '=') {
      t.t = Tok::Sym;
      t.s = src.substr(i, 2);
      adv(2);
    } else if (c == ':' || c == ',' || c == '+' || c == '-') {
      t.t = Tok::Sym;
      t.s = std::string(1, c);
      adv(1);
    } else if (c == '<' || c == '>') {
      throw StaticError("comparisons are <= or >=", line, col);
    } else {
      throw StaticError(std::string("unexpected character '") + c + "'", line, col);
    }
    out.push_back(t);
  }
  Tok end;
  end.line = line;
  end.col = col;
  out.push_back(end);
  return out;
}

// ------------------------------------------------------------------ syntax tree

struct PriceExpr {
  bool rel = false;  // entry +/- offset
  int64_t lit = 0;
  int sign = 1;
  int64_t off = 0;
  bool bps = false;
  int line = 0, col = 0;
};
struct QtyExpr {
  enum K : uint8_t { Lit, All, Half, Position } k = Lit;
  int64_t v = 0;
  int line = 0, col = 0;
};
struct Cond {
  enum K : uint8_t { Price, Fill, Position } k = Price;
  std::string cmp;
  PriceExpr px;
  std::string ref;
  int line = 0, col = 0;
};
struct ANode {
  std::string id;
  int line = 0, col = 0;
  std::string action;  // exit_at exit_market take scale hedge halt
  int aline = 0, acol = 0;
  QtyExpr q;
  PriceExpr px;
  std::optional<int64_t> allow;
  int allow_line = 0, allow_col = 0;
  uint8_t side = proto::Buy;
  std::string hedge;
  int hedge_line = 0, hedge_col = 0;
  std::optional<Cond> when;
  std::string after;
  int after_line = 0, after_col = 0;
  std::vector<Tok> oco;
  std::optional<int64_t> rearm;
  int rearm_line = 0, rearm_col = 0;
};
struct Ast {
  std::vector<Tok> instruments;
  uint8_t side = proto::Buy;
  int64_t qty = 0;
  PriceExpr price;
  int entry_line = 0, entry_col = 0;
  std::vector<ANode> nodes;
};

// ------------------------------------------------------------------ parser

class Parser {
 public:
  explicit Parser(std::vector<Tok> t) : t_(std::move(t)) {}

  Ast parse() {
    Ast a;
    kw("instrument", "a program starts with: instrument NAME");
    a.instruments.push_back(ident("an instrument name"));
    while (sym(",")) a.instruments.push_back(ident("an instrument name"));
    const Tok& e = kw("entry", "expected: entry buy|sell QTY at PRICE");
    a.entry_line = e.line;
    a.entry_col = e.col;
    a.side = side();
    a.qty = integer("the entry quantity");
    kw("at", "expected: at PRICE");
    a.price = price();
    if (a.price.rel) throw StaticError("the entry price must be a number of ticks", a.price.line, a.price.col);
    while (peek().t != Tok::End) a.nodes.push_back(node());
    if (a.nodes.empty()) throw StaticError("a program needs at least one node after the entry", peek().line, peek().col);
    return a;
  }

 private:
  const Tok& peek(size_t k = 0) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
  const Tok& take() { return t_[i_ < t_.size() - 1 ? i_++ : i_]; }
  [[noreturn]] void fail(const std::string& m) const { throw StaticError(m, peek().line, peek().col); }
  bool is_kw(const char* w, size_t k = 0) const { return peek(k).t == Tok::Ident && peek(k).s == w; }
  const Tok& kw(const char* w, const std::string& msg) {
    if (!is_kw(w)) fail(msg);
    return take();
  }
  bool sym(const char* s) {
    if (peek().t == Tok::Sym && peek().s == s) {
      take();
      return true;
    }
    return false;
  }
  Tok ident(const char* what) {
    if (peek().t != Tok::Ident) fail(std::string("expected ") + what);
    return take();
  }
  int64_t integer(const char* what) {
    if (peek().t != Tok::Int) fail(std::string("expected ") + what + " (an integer)");
    return take().v;
  }
  uint8_t side() {
    if (is_kw("buy")) {
      take();
      return proto::Buy;
    }
    if (is_kw("sell")) {
      take();
      return proto::Sell;
    }
    fail("expected buy or sell");
  }
  PriceExpr price() {
    PriceExpr p;
    p.line = peek().line;
    p.col = peek().col;
    if (peek().t == Tok::Int) {
      p.lit = take().v;
      return p;
    }
    if (!is_kw("entry")) fail("expected a price: TICKS, entry + TICKS, entry - TICKS or entry +/- N bps");
    take();
    p.rel = true;
    if (sym("+")) p.sign = 1;
    else if (sym("-")) p.sign = -1;
    else fail("expected + or - after entry");
    p.off = integer("an offset");
    if (is_kw("bps")) {
      take();
      p.bps = true;
    }
    return p;
  }
  QtyExpr qty() {
    QtyExpr q;
    q.line = peek().line;
    q.col = peek().col;
    if (peek().t == Tok::Int) {
      q.v = take().v;
      if (q.v <= 0) throw StaticError("a quantity must be at least 1 lot", q.line, q.col);
      return q;
    }
    if (is_kw("all")) q.k = QtyExpr::All;
    else if (is_kw("half")) q.k = QtyExpr::Half;
    else if (is_kw("position")) q.k = QtyExpr::Position;
    else fail("expected a quantity: LOTS, all, half or position");
    take();
    return q;
  }
  ANode node() {
    ANode n;
    if (peek().t != Tok::Ident || !(peek(1).t == Tok::Sym && peek(1).s == ":"))
      fail("expected a node: NAME: action");
    const Tok& id = take();
    n.id = id.s;
    n.line = id.line;
    n.col = id.col;
    take();  // ':'
    n.aline = peek().line;
    n.acol = peek().col;
    if (is_kw("exit")) {
      take();
      n.q = qty();
      if (is_kw("market")) {
        take();
        n.action = "exit_market";
      } else {
        kw("at", "expected: exit QTY at PRICE allow TICKS, or exit QTY market");
        n.action = "exit_at";
        n.px = price();
        if (is_kw("allow")) {
          const Tok& a = take();
          n.allow_line = a.line;
          n.allow_col = a.col;
          n.allow = integer("the allowance in ticks");
        }
      }
    } else if (is_kw("take")) {
      take();
      n.action = "take";
      n.q = qty();
      kw("at", "expected: take QTY at PRICE");
      n.px = price();
    } else if (is_kw("scale")) {
      take();
      n.action = "scale";
      n.side = side();
      n.q.v = integer("the quantity");
      if (n.q.v <= 0) fail("a quantity must be at least 1 lot");
      kw("at", "expected: scale buy|sell QTY at PRICE");
      n.px = price();
    } else if (is_kw("hedge")) {
      take();
      n.action = "hedge";
      const Tok& h = ident("the hedge instrument");
      n.hedge = h.s;
      n.hedge_line = h.line;
      n.hedge_col = h.col;
      n.side = side();
      n.q.v = integer("the quantity");
      if (n.q.v <= 0) fail("a quantity must be at least 1 lot");
      kw("at", "expected: hedge INSTRUMENT buy|sell QTY at PRICE");
      n.px = price();
    } else if (is_kw("halt")) {
      take();
      n.action = "halt";
    } else {
      fail("expected an action: exit, take, scale, hedge or halt");
    }
    // optional clauses, each at most once
    for (;;) {
      if (is_kw("when")) {
        if (n.when) fail("a node has at most one when");
        take();
        Cond c;
        c.line = peek().line;
        c.col = peek().col;
        if (is_kw("price")) {
          take();
          c.k = Cond::Price;
          c.cmp = cmp();
          c.px = price();
        } else if (is_kw("fill")) {
          take();
          c.k = Cond::Fill;
          c.ref = ident("a node name").s;
        } else if (is_kw("position")) {
          take();
          c.k = Cond::Position;
          c.cmp = cmp();
          qty();
        } else {
          fail("expected: when price <=|>= PRICE, when fill NODE or when position <=|>= QTY");
        }
        n.when = c;
      } else if (is_kw("after")) {
        if (!n.after.empty()) fail("a node has at most one after");
        take();
        const Tok& a = ident("a node name");
        n.after = a.s;
        n.after_line = a.line;
        n.after_col = a.col;
      } else if (is_kw("oco")) {
        if (!n.oco.empty()) fail("a node has at most one oco clause");
        take();
        n.oco.push_back(ident("a node name"));
        while (sym(",")) n.oco.push_back(ident("a node name"));
      } else if (is_kw("rearm")) {
        if (n.rearm) fail("a node has at most one rearm");
        const Tok& r = take();
        n.rearm_line = r.line;
        n.rearm_col = r.col;
        n.rearm = integer("the rearm count");
      } else {
        break;
      }
    }
    if (peek().t != Tok::End && !(peek().t == Tok::Ident && peek(1).t == Tok::Sym && peek(1).s == ":"))
      fail("unexpected '" + peek().s + "'");
    return n;
  }
  std::string cmp() {
    if (sym("<=")) return "<=";
    if (sym(">=")) return ">=";
    fail("expected <= or >=");
  }

  std::vector<Tok> t_;
  size_t i_ = 0;
};

// ------------------------------------------------------------------ compiler

int64_t eval_price(const PriceExpr& p, int64_t entry) {
  int64_t v = p.lit;
  if (p.rel) {
    const int64_t off = p.bps ? (entry * p.off + 5000) / 10000 : p.off;
    v = entry + p.sign * off;
  }
  if (v < 1 || v > fb::kMaxPrice) throw StaticError("price out of range: " + std::to_string(v), p.line, p.col);
  return v;
}

}  // namespace

Program compile(const std::string& source, const Instruments& ins, const CompileOptions& opt) {
  if (opt.backend != "minsky") throw StaticError("backend " + opt.backend + " is not supported (minsky only)", 1, 1);
  Ast a = Parser(lex(source)).parse();

  Program p;
  p.backend = opt.backend;
  p.source_hash = "sha256:" + sha256::hex(source);
  for (const Tok& t : a.instruments) {
    if (!ins.find(t.s)) throw StaticError("unknown instrument " + t.s, t.line, t.col);
    if (std::find(p.instruments.begin(), p.instruments.end(), t.s) != p.instruments.end())
      throw StaticError("instrument " + t.s + " listed twice", t.line, t.col);
    p.instruments.push_back(t.s);
  }
  if (p.instruments.size() > kMaxInstruments)
    throw StaticError("at most 3 instruments: the search is exponential in instruments", a.instruments[3].line,
                      a.instruments[3].col);
  const std::string main = p.instruments[0];
  if (a.qty <= 0) throw StaticError("the entry quantity must be at least 1 lot", a.entry_line, a.entry_col);
  const int64_t entry = eval_price(a.price, 0);

  // names
  std::map<std::string, size_t> idx;
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    const ANode& n = a.nodes[i];
    if (n.id == "entry") throw StaticError("\"entry\" names the header's order; pick another node name", n.line, n.col);
    if (idx.count(n.id)) throw StaticError("node " + n.id + " is defined twice", n.line, n.col);
    idx[n.id] = i;
  }
  // the largest position the program can reach in its main instrument
  int64_t main_pos = a.qty;
  for (const ANode& n : a.nodes)
    if (n.action == "scale" && n.side == a.side) main_pos += n.q.v * (1 + n.rearm.value_or(0));

  // parents: explicit after / when fill, else the entry
  std::vector<std::string> parent(a.nodes.size(), "entry");
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    ANode& n = a.nodes[i];
    std::string par = n.after;
    int pl = n.after_line, pc = n.after_col;
    if (n.when && n.when->k == Cond::Fill) {
      if (!par.empty() && par != n.when->ref)
        throw StaticError("when fill " + n.when->ref + " contradicts after " + par, n.when->line, n.when->col);
      par = n.when->ref;
      pl = n.when->line;
      pc = n.when->col;
    }
    if (n.when && n.when->k == Cond::Position)
      throw StaticError("position conditions are not native orders on backend minsky", n.when->line, n.when->col);
    if (par.empty()) continue;
    if (par != "entry" && !idx.count(par)) throw StaticError("after names an unknown node " + par, pl, pc);
    if (par == n.id) throw StaticError("a node cannot come after itself", pl, pc);
    if (par != "entry" && a.nodes[idx[par]].action == "halt")
      throw StaticError("halt has no order, so nothing can come after it", pl, pc);
    parent[i] = par;
  }
  // every node is reachable from the entry: walk each parent chain
  std::vector<int> depth(a.nodes.size(), -1);
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    size_t cur = i;
    int d = 1;
    std::vector<size_t> seen;
    while (parent[cur] != "entry") {
      seen.push_back(cur);
      cur = idx[parent[cur]];
      ++d;
      if (std::find(seen.begin(), seen.end(), cur) != seen.end())
        throw StaticError("node " + a.nodes[i].id + " is not reachable from the entry (its after chain loops)",
                          a.nodes[i].line, a.nodes[i].col);
    }
    depth[i] = d;
  }

  // OCO groups: union of each node with the nodes it names
  std::vector<size_t> uf(a.nodes.size());
  for (size_t i = 0; i < uf.size(); ++i) uf[i] = i;
  std::function<size_t(size_t)> root = [&](size_t x) { return uf[x] == x ? x : uf[x] = root(uf[x]); };
  std::vector<bool> in_group(a.nodes.size(), false);
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    const ANode& n = a.nodes[i];
    for (const Tok& t : n.oco) {
      if (t.s == "entry") throw StaticError("the entry cannot be in an OCO group", t.line, t.col);
      if (!idx.count(t.s)) throw StaticError("oco names an unknown node " + t.s, t.line, t.col);
      if (t.s == n.id) throw StaticError("oco names the node itself", t.line, t.col);
      const size_t j = idx[t.s];
      if (a.nodes[j].action == "halt" || n.action == "halt")
        throw StaticError("a halt node has no order to cancel", t.line, t.col);
      uf[root(i)] = root(j);
      in_group[i] = in_group[j] = true;
    }
  }
  std::map<size_t, std::string> label;
  for (size_t i = 0; i < a.nodes.size(); ++i)
    if (in_group[i] && !label.count(root(i))) label[root(i)] = "g" + std::to_string(label.size() + 1);

  // The position an exit or take closes: the main instrument's, unless its after-chain reaches a
  // hedge first, in which case it closes that hedge's instrument.
  struct Leg {
    uint8_t open_side = proto::Buy;
    int64_t max_pos = 0;
  };
  std::map<std::string, Leg> legs;
  legs[main] = {a.side, main_pos};
  for (const ANode& n : a.nodes) {
    if (n.action != "hedge") continue;
    auto it = legs.find(n.hedge);
    if (it == legs.end()) it = legs.emplace(n.hedge, Leg{n.side, 0}).first;
    if (n.side == it->second.open_side) it->second.max_pos += n.q.v * (1 + n.rearm.value_or(0));
  }
  auto leg_of = [&](size_t i) -> std::string {
    size_t cur = i;
    while (parent[cur] != "entry") {
      cur = idx[parent[cur]];
      if (a.nodes[cur].action == "hedge") return a.nodes[cur].hedge;
    }
    return main;
  };

  // nodes
  Node en;
  en.id = "entry";
  en.kind = Kind::Limit;
  en.instrument = main;
  en.side = a.side;
  en.qty = a.qty;
  en.price = entry;
  p.nodes.push_back(en);
  std::vector<int> node_depth{0};
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    const ANode& n = a.nodes[i];
    Node o;
    o.id = n.id;
    o.instrument = main;
    o.parent = parent[i];
    if (in_group[i]) o.oco = label[root(i)];
    const bool closer = n.action == "exit_at" || n.action == "exit_market" || n.action == "take";
    const std::string leg_name = closer ? leg_of(i) : main;
    const Leg& leg = legs[leg_name];
    const int64_t max_pos = leg.max_pos;
    const uint8_t closing = leg.open_side == proto::Buy ? proto::Sell : proto::Buy;
    if (closer && leg_name != main) {
      o.instrument = leg_name;
      auto literal = [&](const PriceExpr& px) {
        if (px.rel) throw StaticError("an exit on hedge " + leg_name + " takes a price in ticks, not entry +/-", px.line, px.col);
      };
      if (n.action != "exit_market") literal(n.px);
      if (n.when && n.when->k == Cond::Price) literal(n.when->px);
    }
    auto resolve_qty = [&]() -> int64_t {
      switch (n.q.k) {
        case QtyExpr::Lit:
          if (n.q.v > max_pos)
            throw StaticError("exit larger than the largest reachable position (" + std::to_string(max_pos) + ")",
                              n.q.line, n.q.col);
          return n.q.v;
        case QtyExpr::Half:
          if (max_pos / 2 == 0) throw StaticError("half of a 1-lot position is 0 lots", n.q.line, n.q.col);
          return max_pos / 2;
        default: return max_pos;
      }
    };
    if (n.when && n.when->k == Cond::Price && n.action != "exit_at" && n.action != "exit_market")
      throw StaticError("only exits trigger on price; a " + n.action + " is a resting limit", n.when->line, n.when->col);
    if (n.action == "exit_at" || n.action == "exit_market") {
      o.side = closing;
      o.qty = resolve_qty();
      o.reduce_only = true;
      std::optional<int64_t> trig;
      if (n.when && n.when->k == Cond::Price) {
        const std::string want = closing == proto::Sell ? "<=" : ">=";
        if (n.when->cmp != want)
          throw StaticError(std::string("a ") + (closing == proto::Sell ? "sell" : "buy") + " exit triggers on price " +
                                want + " x",
                            n.when->line, n.when->col);
        trig = eval_price(n.when->px, entry);
      }
      if (n.action == "exit_market") {
        if (!trig) throw StaticError("exit market needs a trigger: when price <= x (or >= x)", n.aline, n.acol);
        o.kind = Kind::StopMarket;
        o.trigger = trig;
      } else {
        if (!n.allow || (*n.allow < 1 && !opt.unsafe_allow_zero))
          throw StaticError("exit requires allow >= 1 tick", n.aline, n.acol);
        if (*n.allow < 0) throw StaticError("allow must be a non-negative number of ticks", n.allow_line, n.allow_col);
        const int64_t at = eval_price(n.px, entry);
        o.kind = Kind::StopLimit;
        o.trigger = trig.value_or(at);
        o.price = closing == proto::Sell ? at - *n.allow : at + *n.allow;
        if (*o.price < 1) throw StaticError("allow takes the limit below 1 tick", n.allow_line, n.allow_col);
        if (opt.max_gap_ticks && *n.allow < *opt.max_gap_ticks)
          p.warnings.push_back("line " + std::to_string(n.line) + ": allow " + std::to_string(*n.allow) +
                               " < env.max_gap_ticks " + std::to_string(*opt.max_gap_ticks) +
                               ": a gap can jump past the stop limit and leave it resting");
      }
    } else if (n.action == "take") {
      o.kind = Kind::Limit;
      o.side = closing;
      o.qty = resolve_qty();
      o.reduce_only = true;
      o.price = eval_price(n.px, entry);
    } else if (n.action == "scale") {
      o.kind = Kind::Limit;
      o.side = n.side;
      o.qty = n.q.v;
      o.price = eval_price(n.px, entry);
    } else if (n.action == "hedge") {
      if (std::find(p.instruments.begin(), p.instruments.end(), n.hedge) == p.instruments.end())
        throw StaticError("hedge instrument " + n.hedge + " is not in the header", n.hedge_line, n.hedge_col);
      if (n.hedge == main) throw StaticError("a hedge trades another instrument; use scale", n.hedge_line, n.hedge_col);
      o.kind = Kind::Limit;
      o.instrument = n.hedge;
      o.side = n.side;
      o.qty = n.q.v;
      o.price = eval_price(n.px, entry);
    } else {  // halt
      o.kind = Kind::Halt;
      o.terminal = true;
      if (n.rearm) throw StaticError("a halt node cannot rearm", n.rearm_line, n.rearm_col);
    }
    p.nodes.push_back(o);
    node_depth.push_back(depth[i]);
    // rearm K: K chained copies, each a child of the previous copy
    if (n.rearm) {
      if (*n.rearm < 1) throw StaticError("rearm must be at least 1", n.rearm_line, n.rearm_col);
      if (*n.rearm > static_cast<int64_t>(kMaxNodes))
        throw StaticError("rearm expands past 64 nodes", n.rearm_line, n.rearm_col);
      std::string prev = o.id;
      for (int64_t r = 1; r <= *n.rearm; ++r) {
        Node c = o;
        c.id = o.id + ".r" + std::to_string(r);
        c.parent = prev;
        c.oco.clear();
        p.nodes.push_back(c);
        node_depth.push_back(depth[i] + static_cast<int>(r));
        prev = c.id;
      }
    }
  }
  if (p.nodes.size() > kMaxNodes) {
    const ANode& last = a.nodes.back();
    throw StaticError("the program expands to " + std::to_string(p.nodes.size()) + " nodes; at most 64", last.line,
                      last.col);
  }
  // a rearm copy's children are not re-pointed: dependants hang off the original node

  // deploy plan: one generation per parent depth, source order inside a generation
  int maxd = 0;
  for (int d : node_depth) maxd = std::max(maxd, d);
  for (int d = 0; d <= maxd; ++d) {
    std::vector<std::string> gen;
    for (size_t i = 0; i < p.nodes.size(); ++i)
      if (node_depth[i] == d && p.nodes[i].native()) gen.push_back(p.nodes[i].id);
    if (!gen.empty()) p.deploy_plan.push_back(gen);
  }
  // halt nodes carry depth too; a native child of a halt is impossible (checked above)

  p.groups.clear();
  for (const auto& nd : p.nodes)
    if (!nd.oco.empty() && std::find(p.groups.begin(), p.groups.end(), nd.oco) == p.groups.end())
      p.groups.push_back(nd.oco);
  p.report.nodes_total = static_cast<int64_t>(p.nodes.size());
  for (const auto& nd : p.nodes) p.report.native += nd.native() ? 1 : 0;
  p.report.relayed = 0;
  p.report.locked_collateral_cents = locked_collateral_cents(p, ins);

  // the compiled form must load: the same validation verify and venued apply
  return load_program(to_json(p), ins);
}

}  // namespace bk
