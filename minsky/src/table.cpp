#include "table.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tbl {

namespace {
struct W {
  std::vector<unsigned char> b;
  template <class T>
  void put(T v) {
    unsigned char tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    b.insert(b.end(), tmp, tmp + sizeof(T));
  }
  void str(const std::string& s) {
    put<uint32_t>(static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
};
struct R {
  const unsigned char* p;
  const unsigned char* end;
  // a count from the file, which must be plausible for the bytes that remain
  uint32_t count(size_t min_bytes_each) {
    uint32_t n = get<uint32_t>();
    if (static_cast<size_t>(end - p) < n * min_bytes_each) throw std::runtime_error("table file truncated");
    return n;
  }
  template <class T>
  T get() {
    if (end - p < static_cast<long>(sizeof(T))) throw std::runtime_error("table file truncated");
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  std::string str() {
    uint32_t n = get<uint32_t>();
    if (end - p < static_cast<long>(n)) throw std::runtime_error("table file truncated");
    std::string s(reinterpret_cast<const char*>(p), n);
    p += n;
    return s;
  }
};
void put_actions(W& w, const std::vector<ob::Action>& as) {
  w.put<uint32_t>(static_cast<uint32_t>(as.size()));
  for (const auto& a : as) {
    w.put<uint8_t>(static_cast<uint8_t>(a.kind));
    w.put<int32_t>(a.arg);
  }
}
std::vector<ob::Action> get_actions(R& r) {
  uint32_t n = r.count(5);
  std::vector<ob::Action> as(n);
  for (auto& a : as) {
    a.kind = static_cast<ob::Act>(r.get<uint8_t>());
    a.arg = r.get<int32_t>();
  }
  return as;
}
}  // namespace

int Table::marker_of(int instr) const {
  for (size_t i = 0; i < markers.size(); ++i)
    if (markers[i] == instr) return static_cast<int>(i);
  return -1;
}

void save(const Table& t, const std::string& path) {
  W w;
  w.b.insert(w.b.end(), {'M', 'N', 'S', 'K'});
  w.put<uint32_t>(1);
  w.str(t.name);
  w.put<int32_t>(t.books);
  w.put<int32_t>(t.ninstr);
  w.put<uint32_t>(static_cast<uint32_t>(t.prog.templates.size()));
  for (size_t i = 0; i < t.prog.templates.size(); ++i) {
    const auto& x = t.prog.templates[i];
    w.put<int32_t>(x.account);
    w.put<int32_t>(x.book);
    w.put<uint8_t>(static_cast<uint8_t>(x.side));
    w.put<uint8_t>(static_cast<uint8_t>(x.type));
    w.put<uint8_t>(x.reduce_only);
    w.put<int64_t>(x.price);
    w.put<int64_t>(x.trigger);
    w.put<int64_t>(x.qty);
    w.put<int32_t>(i < t.entry_instr.size() ? t.entry_instr[i] : -1);
  }
  w.put<uint32_t>(static_cast<uint32_t>(t.prog.links.size()));
  for (const auto& l : t.prog.links) {
    w.put<uint8_t>(static_cast<uint8_t>(l.on));
    w.put<int32_t>(l.src);
    w.put<uint8_t>(l.at.has_value());
    w.put<int64_t>(l.at.value_or(0));
    put_actions(w, l.actions);
  }
  put_actions(w, t.prog.initial);
  w.put<uint32_t>(static_cast<uint32_t>(t.markers.size()));
  for (int m : t.markers) w.put<int32_t>(m);
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fwrite(w.b.data(), 1, w.b.size(), f);
  std::fclose(f);
}

Table load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  R r{b.data(), b.data() + b.size()};
  if (b.size() < 8 || std::memcmp(b.data(), "MNSK", 4) != 0) throw std::runtime_error("not a table file");
  r.p += 4;
  if (r.get<uint32_t>() != 1) throw std::runtime_error("table version");
  Table t;
  t.name = r.str();
  t.books = r.get<int32_t>();
  t.ninstr = r.get<int32_t>();
  uint32_t nt = r.count(39);
  t.prog.templates.resize(nt);
  t.entry_instr.resize(nt);
  for (uint32_t i = 0; i < nt; ++i) {
    auto& x = t.prog.templates[i];
    x.account = r.get<int32_t>();
    x.book = r.get<int32_t>();
    x.side = static_cast<ob::Side>(r.get<uint8_t>());
    x.type = static_cast<ob::Type>(r.get<uint8_t>());
    x.reduce_only = r.get<uint8_t>();
    x.price = r.get<int64_t>();
    x.trigger = r.get<int64_t>();
    x.qty = r.get<int64_t>();
    t.entry_instr[i] = r.get<int32_t>();
  }
  uint32_t nl = r.count(18);
  t.prog.links.resize(nl);
  for (auto& l : t.prog.links) {
    l.on = static_cast<ob::On>(r.get<uint8_t>());
    l.src = r.get<int32_t>();
    bool has = r.get<uint8_t>();
    int64_t at = r.get<int64_t>();
    if (has) l.at = at;
    l.actions = get_actions(r);
  }
  t.prog.initial = get_actions(r);
  uint32_t nm = r.count(4);
  t.markers.resize(nm);
  for (auto& m : t.markers) m = r.get<int32_t>();
  return t;
}

void save(const Layout& l, const std::string& path) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write " + path);
  for (const auto& [k, vs] : l.v) {
    out << k << " =";
    for (size_t i = 0; i < vs.size(); ++i) out << (i ? "," : " ") << vs[i];
    out << "\n";
  }
}

Layout load_layout(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  Layout l;
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq);
    while (!k.empty() && k.back() == ' ') k.pop_back();
    std::string rest = line.substr(eq + 1);
    std::vector<long long> vs;
    std::stringstream ss(rest);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      if (tok.find_first_not_of(" \t") == std::string::npos) continue;
      vs.push_back(std::stoll(tok));
    }
    l.v[k] = vs;
  }
  return l;
}

}  // namespace tbl
