// The router table on disk: an ob::Program (templates, fill/accept links, initial actions) plus
// the instrumentation the router needs to count instructions and frames, and a small text
// "layout" file the visualiser uses to know which books mean what. The exchange never sees
// either file.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "engine.hpp"

namespace tbl {

struct Table {
  std::string name;
  ob::Program prog;
  std::vector<int> entry_instr;   // template id -> instruction it enters, or -1
  std::vector<int> markers;       // instruction indices that start a frame / generation
  int books = 0;                  // books the program touches (the server must have at least this many)
  int ninstr = 0;
  int marker_of(int instr) const;  // index in markers, or -1
};

void save(const Table& t, const std::string& path);
Table load(const std::string& path);

// name = v1,v2,...   or   name = v
struct Layout {
  std::map<std::string, std::vector<long long>> v;
  long long get(const std::string& k, long long dflt = 0) const {
    auto it = v.find(k);
    return it == v.end() || it->second.empty() ? dflt : it->second[0];
  }
  const std::vector<long long>& vec(const std::string& k) const {
    static const std::vector<long long> none;
    auto it = v.find(k);
    return it == v.end() ? none : it->second;
  }
};
void save(const Layout& l, const std::string& path);
Layout load_layout(const std::string& path);

}  // namespace tbl
