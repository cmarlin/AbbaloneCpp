// Iterative deepening tests — phase 7: exactness of GenMoveId against the
// plain radix search at every fixed depth (aspiration must never change
// the value, only the cost), the forced re-search path (aspiration = 0),
// the soft budget, a persistent TT across a short game, and the
// no-legal-move edge case.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"
#include "bb_tt.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace bb;

namespace {

uint64_t NextRand(uint64_t& seed) {
  seed = seed * 6364136223846793005ull + 1442695040888963407ull;
  return seed >> 33;
}

Board RandomBoard(uint64_t& seed, int pairs) {
  Board b{{0, 0}};
  for (int i = 0; i < pairs; ++i) {
    for (int p = 0; p < 2; ++p) {
      int cell;
      do {
        cell = static_cast<int>(NextRand(seed) % kNumCells);
      } while (((b.p[0] | b.p[1]) >> cell) & 1);
      b.p[p] |= kBit[cell];
    }
  }
  return b;
}

struct CtxPtr {
  std::unique_ptr<SearchContext> ctx;
  explicit CtxPtr(const SearchConfig& cfg) : ctx(new SearchContext()) {
    ctx->config = &cfg;
  }
  SearchContext* get() const { return ctx.get(); }
};

std::vector<uint16_t> LegalKeys(const Board& b, int player) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  const auto& executors = Executors();
  std::vector<uint16_t> keys;
  for (int d = 0; d < kNumDirs; ++d)
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        keys.push_back(executors[ExecutorIndex(d, id, cell)].move.key());
      }
    }
  return keys;
}

bool Contains(const std::vector<uint16_t>& v, uint16_t k) {
  return std::find(v.begin(), v.end(), k) != v.end();
}

// GenMoveId at maxDepth = depth, unlimited budget, must reproduce the
// radix full-window value and report the depth it reached.
void CheckIdExact(const Board& bIn, int player, const SearchConfig& config,
                  int depth) {
  Board b = bIn;
  const int rootEval = Eval(config.weights, b);

  CtxPtr radix(config);
  Move bestRadix;
  const int scoreRadix = RadixSortedAlphaBeta(bestRadix, rootEval, -32000,
                                              32000, b, 0, depth, player,
                                              *radix.get());

  TranspositionTable tt(2);
  CtxPtr ctx(config);
  ctx.get()->tt = &tt;
  const int idx = GenMoveId(b, player, config, depth, 0, *ctx.get());

  INFO("depth=" << depth << " player=" << player);
  REQUIRE(idx >= 0);
  CHECK(ctx.get()->idDepth == depth);
  CHECK(ctx.get()->idScore == scoreRadix);
  CHECK(Contains(LegalKeys(b, player), Executors()[idx].move.key()));
}

}  // namespace

TEST_CASE("id: exact score and depth at fixed max depth") {
  InitTables();
  const SearchConfig def(EvalWeights::Default(), false, 1000);
  const SearchConfig twk(EvalWeights::Tweak(), false, 1000);

  const Board classical = ClassicalBoard();
  for (int depth = 1; depth <= 5; ++depth) {
    CheckIdExact(classical, 0, twk, depth);
    CheckIdExact(classical, 1, twk, depth);
  }

  uint64_t seed = 11;
  for (int i = 0; i < 4; ++i) {
    const Board b = RandomBoard(seed, 4 + i);
    for (int depth = 1; depth <= 4; ++depth) {
      CheckIdExact(b, 0, def, depth);
      CheckIdExact(b, 1, twk, depth);
    }
  }
}

TEST_CASE("id: forced aspiration fails stay exact") {
  InitTables();
  // aspiration = 0: clamped to a minimal [p-1, p+1] window, so every
  // iteration d >= 2 fails and takes the full-window re-search path. The
  // value must be unaffected.
  SearchConfig config(EvalWeights::Tweak(), false, 1000);
  config.aspiration = 0;

  const Board classical = ClassicalBoard();
  for (int depth = 1; depth <= 4; ++depth) {
    CheckIdExact(classical, 0, config, depth);
    CheckIdExact(classical, 1, config, depth);
  }
  uint64_t seed = 23;
  for (int i = 0; i < 3; ++i) {
    const Board b = RandomBoard(seed, 5 + i);
    CheckIdExact(b, 0, config, 3);
    CheckIdExact(b, 1, config, 3);
  }
}

TEST_CASE("id: soft budget stops between iterations") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();

  // Unlimited budget: deterministic, reaches maxDepth.
  {
    TranspositionTable tt(2);
    CtxPtr ctx(config);
    ctx.get()->tt = &tt;
    const int idx = GenMoveId(classical, 0, config, 6, 0, *ctx.get());
    REQUIRE(idx >= 0);
    CHECK(ctx.get()->idDepth == 6);
    CHECK(Contains(LegalKeys(classical, 0), Executors()[idx].move.key()));
  }

  // Tiny budget: depth 1 always completes (the check runs between
  // iterations), a valid move is returned, and the search stops early.
  {
    TranspositionTable tt(2);
    CtxPtr ctx(config);
    ctx.get()->tt = &tt;
    const int idx = GenMoveId(classical, 0, config, 12, 1, *ctx.get());
    REQUIRE(idx >= 0);
    CHECK(ctx.get()->idDepth >= 1);
    CHECK(ctx.get()->idDepth < 12);
    CHECK(Contains(LegalKeys(classical, 0), Executors()[idx].move.key()));
  }
}

TEST_CASE("id: persistent TT across a short game") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  TranspositionTable tt(4);

  Board b = ClassicalBoard();
  int player = 0;
  for (int ply = 0; ply < 10; ++ply) {
    CtxPtr ctx(config);
    ctx.get()->tt = &tt;
    const int idx = GenMoveId(b, player, config, 4, 0, *ctx.get());
    INFO("ply=" << ply << " player=" << player);
    REQUIRE(idx >= 0);
    REQUIRE(ctx.get()->idDepth == 4);
    CHECK(Contains(LegalKeys(b, player), Executors()[idx].move.key()));
    ApplyMove(b, Executors()[idx], player);
    player = 1 - player;
  }
  CHECK(tt.hits > 0);
}

TEST_CASE("id: no legal move") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  Board blocked{{0, 0}};
  blocked.p[0] = kBit[CellFromName("e5")];
  for (int d = 0; d < kNumDirs; ++d)
    blocked.p[1] |= kBit[kNeighbor[CellFromName("e5")][d]];

  TranspositionTable tt(2);
  CtxPtr ctx(config);
  ctx.get()->tt = &tt;
  CHECK(GenMoveId(blocked, 0, config, 4, 0, *ctx.get()) == -1);
  CHECK(ctx.get()->idDepth == 0);
}
