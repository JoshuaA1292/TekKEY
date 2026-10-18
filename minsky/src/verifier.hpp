// A3: the verifier. A bounded model checker whose transition function is the matching engine:
// every state is an fb::Core snapshot, every move is real NewOrderMsgs (env.hpp), so fills, stop
// cascades, OCO effects and child releases all come from the engine, never from a model of it.
// BOUNDED_KEYS.md §8; CONTRACT.md §4.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "program_json.hpp"

namespace bk {

struct VerifyOptions {
  std::vector<int64_t> marks;  // per program instrument; one value applies to all
  int64_t timeout_ms = 10000;
  bool shortest = true;  // report the shortest counterexample / worst path (iterative deepening)
};

struct VerifyResult {
  int exit_code = 1;  // 0 certified, 3 rejected, 4 timeout
  json::Value out;
};

VerifyResult verify(const Program& p, const Envelope& e, const Instruments& ins, const VerifyOptions& opt);

// The moves of a path (worst_path / counterexample), parsed back from its JSON form, for replay.
struct PathMove {
  enum Type : uint8_t { Print, Gap, Pause, Cancel } type = Print;
  int inst = 0;  // program instrument index
  int64_t to = 0;
};
std::vector<PathMove> path_moves(const json::Value& path, const Program& p);
// The value a path ends on: the loss_cents of its final {"event":"mark"} entry.
int64_t path_final_loss(const json::Value& path);

// Engine identity printed on certificates.
std::string engine_id();

}  // namespace bk
