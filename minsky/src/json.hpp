// A small JSON value, parser and deterministic writer for the bounded-key tools (policyc, verify,
// replay, venued). No dependencies. Objects keep insertion order, so output is byte-stable: the
// same value always prints the same bytes, which the fixtures and the determinism tests rely on.
// Integers are kept exact as int64; anything with a fraction or exponent is a double.
#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct ParseError : std::runtime_error {
  int line, col;
  ParseError(const std::string& m, int l, int c) : std::runtime_error(m), line(l), col(c) {}
};

class Value {
 public:
  enum Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };
  using Arr = std::vector<Value>;
  using Obj = std::vector<std::pair<std::string, Value>>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : t_(Bool), i_(b) {}
  Value(int v) : t_(Int), i_(v) {}
  Value(long v) : t_(Int), i_(v) {}
  Value(long long v) : t_(Int), i_(v) {}
  Value(unsigned v) : t_(Int), i_(v) {}
  Value(unsigned long v) : t_(Int), i_(static_cast<int64_t>(v)) {}
  Value(unsigned long long v) : t_(Int), i_(static_cast<int64_t>(v)) {}
  Value(double d) : t_(Double), d_(d) {}
  Value(const char* s) : t_(String), s_(s) {}
  Value(std::string s) : t_(String), s_(std::move(s)) {}
  Value(Arr a) : t_(Array), a_(std::make_shared<Arr>(std::move(a))) {}
  Value(Obj o) : t_(Object), o_(std::make_shared<Obj>(std::move(o))) {}
  static Value array() { return Value(Arr{}); }
  static Value object() { return Value(Obj{}); }

  Type type() const { return t_; }
  bool is_null() const { return t_ == Null; }
  bool is_bool() const { return t_ == Bool; }
  bool is_int() const { return t_ == Int; }
  bool is_number() const { return t_ == Int || t_ == Double; }
  bool is_string() const { return t_ == String; }
  bool is_array() const { return t_ == Array; }
  bool is_object() const { return t_ == Object; }

  bool as_bool() const {
    if (t_ != Bool) throw std::runtime_error("expected a boolean");
    return i_ != 0;
  }
  int64_t as_int() const {
    if (t_ == Int) return i_;
    if (t_ == Double && std::floor(d_) == d_ && std::fabs(d_) < 9.2e18) return static_cast<int64_t>(d_);
    throw std::runtime_error("expected an integer");
  }
  double as_double() const {
    if (t_ == Double) return d_;
    if (t_ == Int) return static_cast<double>(i_);
    throw std::runtime_error("expected a number");
  }
  const std::string& as_string() const {
    if (t_ != String) throw std::runtime_error("expected a string");
    return s_;
  }
  const Arr& as_array() const {
    if (t_ != Array) throw std::runtime_error("expected an array");
    return *a_;
  }
  Arr& as_array() {
    if (t_ != Array) throw std::runtime_error("expected an array");
    own();
    return *a_;
  }
  const Obj& as_object() const {
    if (t_ != Object) throw std::runtime_error("expected an object");
    return *o_;
  }

  // Object access. get() returns nullptr when absent; at() throws with the key's name.
  const Value* get(const std::string& k) const {
    if (t_ != Object) return nullptr;
    for (const auto& [kk, v] : *o_)
      if (kk == k) return &v;
    return nullptr;
  }
  const Value& at(const std::string& k) const {
    const Value* v = get(k);
    if (!v) throw std::runtime_error("missing field \"" + k + "\"");
    return *v;
  }
  bool has(const std::string& k) const { return get(k) != nullptr; }
  // Insert or replace, keeping the original position of an existing key.
  Value& set(const std::string& k, Value v) {
    if (t_ == Null) *this = object();
    if (t_ != Object) throw std::runtime_error("set on a non-object");
    own();
    for (auto& [kk, vv] : *o_)
      if (kk == k) return vv = std::move(v);
    o_->emplace_back(k, std::move(v));
    return o_->back().second;
  }
  void erase(const std::string& k) {
    if (t_ != Object) return;
    own();
    for (auto it = o_->begin(); it != o_->end(); ++it)
      if (it->first == k) {
        o_->erase(it);
        return;
      }
  }
  Value& push(Value v) {
    if (t_ == Null) *this = array();
    own();
    a_->push_back(std::move(v));
    return a_->back();
  }
  size_t size() const { return t_ == Array ? a_->size() : t_ == Object ? o_->size() : 0; }
  const Value& operator[](size_t i) const { return as_array().at(i); }

  // Typed lookups with defaults, for optional fields.
  int64_t int_or(const std::string& k, int64_t d) const {
    const Value* v = get(k);
    return v && !v->is_null() ? v->as_int() : d;
  }
  std::string str_or(const std::string& k, const std::string& d) const {
    const Value* v = get(k);
    return v && !v->is_null() ? v->as_string() : d;
  }

  bool operator==(const Value& o) const;
  bool operator!=(const Value& o) const { return !(*this == o); }

 private:
  void own() {
    if (t_ == Array && a_.use_count() > 1) a_ = std::make_shared<Arr>(*a_);
    if (t_ == Object && o_.use_count() > 1) o_ = std::make_shared<Obj>(*o_);
  }
  Type t_ = Null;
  int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::shared_ptr<Arr> a_;
  std::shared_ptr<Obj> o_;
};

inline bool Value::operator==(const Value& o) const {
  if (is_number() && o.is_number()) {
    if (t_ == Int && o.t_ == Int) return i_ == o.i_;
    return as_double() == o.as_double();
  }
  if (t_ != o.t_) return false;
  switch (t_) {
    case Null: return true;
    case Bool: return i_ == o.i_;
    case String: return s_ == o.s_;
    case Array: return *a_ == *o.a_;
    case Object: {
      if (o_->size() != o.o_->size()) return false;
      for (const auto& [k, v] : *o_) {
        const Value* w = o.get(k);
        if (!w || *w != v) return false;
      }
      return true;
    }
    default: return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Parser

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}
  Value parse() {
    ws();
    Value v = value(0);
    ws();
    if (p_ != s_.size()) fail("trailing characters");
    return v;
  }

 private:
  [[noreturn]] void fail(const std::string& m) const {
    int line = 1, col = 1;
    for (size_t i = 0; i < p_ && i < s_.size(); ++i) {
      if (s_[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    throw ParseError("json: " + m + " at line " + std::to_string(line) + " col " + std::to_string(col), line, col);
  }
  void ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
  }
  char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }
  void expect(char c) {
    if (peek() != c) fail(std::string("expected '") + c + "'");
    ++p_;
  }
  bool lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if (s_.compare(p_, n, w) == 0) {
      p_ += n;
      return true;
    }
    return false;
  }
  Value value(int depth) {
    if (depth > 200) fail("nesting too deep");
    char c = peek();
    if (c == '{') return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') return Value(str());
    if (c == 't' && lit("true")) return Value(true);
    if (c == 'f' && lit("false")) return Value(false);
    if (c == 'n' && lit("null")) return Value(nullptr);
    if (c == '-' || (c >= '0' && c <= '9')) return number();
    fail("unexpected character");
  }
  Value object(int depth) {
    expect('{');
    Value::Obj o;
    ws();
    if (peek() == '}') {
      ++p_;
      return Value(std::move(o));
    }
    for (;;) {
      ws();
      if (peek() != '"') fail("expected a key");
      std::string k = str();
      ws();
      expect(':');
      ws();
      o.emplace_back(std::move(k), value(depth + 1));
      ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      expect('}');
      return Value(std::move(o));
    }
  }
  Value array(int depth) {
    expect('[');
    Value::Arr a;
    ws();
    if (peek() == ']') {
      ++p_;
      return Value(std::move(a));
    }
    for (;;) {
      ws();
      a.push_back(value(depth + 1));
      ws();
      if (peek() == ',') {
        ++p_;
        continue;
      }
      expect(']');
      return Value(std::move(a));
    }
  }
  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  uint32_t hex4() {
    if (p_ + 4 > s_.size()) fail("short \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[p_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else fail("bad \\u escape");
    }
    return v;
  }
  std::string str() {
    expect('"');
    std::string out;
    for (;;) {
      if (p_ >= s_.size()) fail("unterminated string");
      char c = s_[p_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= s_.size()) fail("unterminated escape");
      char e = s_[p_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xd800 && cp < 0xdc00 && p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
            p_ += 2;
            uint32_t lo = hex4();
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          }
          utf8(out, cp);
          break;
        }
        default: fail("bad escape");
      }
    }
  }
  Value number() {
    size_t st = p_;
    if (peek() == '-') ++p_;
    bool frac = false;
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c >= '0' && c <= '9') {
        ++p_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || (c == '-' && p_ > st)) {
        frac = true;
        ++p_;
      } else {
        break;
      }
    }
    const char* b = s_.data() + st;
    const char* e = s_.data() + p_;
    if (!frac) {
      int64_t v = 0;
      auto r = std::from_chars(b, e, v);
      if (r.ec == std::errc() && r.ptr == e) return Value(static_cast<long long>(v));
    }
    std::string tmp(b, e);
    char* end = nullptr;
    double d = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size()) fail("bad number");
    return Value(d);
  }

  const std::string& s_;
  size_t p_ = 0;
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

// ---------------------------------------------------------------------------------------------
// Writer. compact(): no whitespace, one line (the venued wire format, the event stream).
// pretty(): two-space indent, but any array or object whose compact form fits in `width`
// columns at its indent is printed on one line. Both are pure functions of the value.

inline void escape(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

inline void number(std::string& out, double d) {
  if (!std::isfinite(d)) {
    out += "null";
    return;
  }
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, d);
  std::string s(buf, r.ptr);
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  out += s;
}

inline void compact(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Value::Int: out += std::to_string(v.as_int()); break;
    case Value::Double: number(out, v.as_double()); break;
    case Value::String: escape(out, v.as_string()); break;
    case Value::Array: {
      out += '[';
      bool first = true;
      for (const auto& e : v.as_array()) {
        if (!first) out += ',';
        first = false;
        compact(out, e);
      }
      out += ']';
      break;
    }
    case Value::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, e] : v.as_object()) {
        if (!first) out += ',';
        first = false;
        escape(out, k);
        out += ':';
        compact(out, e);
      }
      out += '}';
      break;
    }
  }
}

inline std::string compact(const Value& v) {
  std::string s;
  compact(s, v);
  return s;
}

// The one-line form used inside pretty output: a space after ':' and ',' for readability.
inline void spaced(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Array: {
      out += '[';
      bool first = true;
      for (const auto& e : v.as_array()) {
        if (!first) out += ", ";
        first = false;
        spaced(out, e);
      }
      out += ']';
      break;
    }
    case Value::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, e] : v.as_object()) {
        if (!first) out += ", ";
        first = false;
        escape(out, k);
        out += ": ";
        spaced(out, e);
      }
      out += '}';
      break;
    }
    default: compact(out, v);
  }
}

// `indent` is the block indent; `col` is the column the value starts at (after "key": ).
inline void pretty(std::string& out, const Value& v, int indent, int col, int width) {
  if (!v.is_array() && !v.is_object()) {
    compact(out, v);
    return;
  }
  std::string one;
  spaced(one, v);
  if (static_cast<int>(one.size()) + col <= width || v.size() == 0) {
    out += one;
    return;
  }
  const std::string pad(indent + 2, ' ');
  if (v.is_array()) {
    out += "[\n";
    bool first = true;
    for (const auto& e : v.as_array()) {
      if (!first) out += ",\n";
      first = false;
      out += pad;
      pretty(out, e, indent + 2, indent + 2, width);
    }
    out += "\n" + std::string(indent, ' ') + "]";
    return;
  }
  out += "{\n";
  bool first = true;
  for (const auto& [k, e] : v.as_object()) {
    if (!first) out += ",\n";
    first = false;
    out += pad;
    escape(out, k);
    out += ": ";
    pretty(out, e, indent + 2, indent + 2 + static_cast<int>(k.size()) + 4, width);
  }
  out += "\n" + std::string(indent, ' ') + "}";
}

inline std::string pretty(const Value& v, int width = 110) {
  std::string s;
  pretty(s, v, 0, 0, width);
  s += '\n';
  return s;
}

// Builder helper: json::obj({{"a", 1}, {"b", "x"}}) keeps the order given.
inline Value obj(std::initializer_list<std::pair<std::string, Value>> kv) {
  Value::Obj o(kv.begin(), kv.end());
  return Value(std::move(o));
}
inline Value arr(std::initializer_list<Value> xs) { return Value(Value::Arr(xs.begin(), xs.end())); }

}  // namespace json
