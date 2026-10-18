// Programs other than Snake that the compile tool can emit.
#pragma once
#include <vector>

#include "minsky.hpp"

namespace programs {
// Rule 110 on n cells, looping forever (or `gens` generations then HALT if gens > 0). Cell i is
// register i; the next row lives in registers n..2n-1. gen_marker is the instruction that starts
// a generation. From stage 1's rule110.cpp, made to loop.
mm::MProg rule110(int n, int gens, int& gen_marker);
std::vector<int> rule110_step(const std::vector<int>& row);
}  // namespace programs
