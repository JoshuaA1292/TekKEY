// A2: the policy language (MPL, BOUNDED_KEYS.md §6) and its compiler to a CompiledProgram (§7).
// The grammar is the guardrail: what MPL cannot express, the venue cannot be asked to run.
#pragma once
#include <optional>
#include <stdexcept>
#include <string>

#include "json.hpp"
#include "program_json.hpp"

namespace bk {

// A violation of the grammar or of a static rule (§6.2): StaticError, exit code 2.
struct StaticError : std::runtime_error {
  int line, col;
  StaticError(const std::string& m, int l, int c) : std::runtime_error(m), line(l), col(c) {}
  json::Value to_json() const;
};

struct CompileOptions {
  std::string backend = "minsky";
  std::optional<int64_t> max_gap_ticks;  // from --env, for the allow < max_gap_ticks warning
  bool unsafe_allow_zero = false;        // tests only: let `allow 0` through to the verifier
};

// Throws StaticError. Deterministic: node order = source order, OCO labels g1, g2, ... in order of
// first appearance, deploy plan = parent generations in source order.
Program compile(const std::string& source, const Instruments& ins, const CompileOptions& opt);

}  // namespace bk
