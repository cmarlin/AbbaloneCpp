// Search tests — phase 5: exact-value equivalence between MiniMax,
// AlphaBeta (full eval), AlphaBeta (incremental eval) and radix-sorted
// alpha-beta under a full window; depth-1 exactness of the sorted
// alpha-beta; legality of every returned move; the no-legal-move edge
// case; and the GenMove dispatch.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"

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

// Executor keys of all legal moves.
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

void CheckEquivalence(const Board& bIn, int player, const SearchConfig& config,
                      int depth) {
  const EvalWeights& w = config.weights;
  Board b = bIn;
  const int rootEval = Eval(w, b);

  CtxPtr mm(config), ab(config), inc(config), radix(config);
  Move bestMm, bestAb, bestInc, bestRadix;
  const int scoreMm = MiniMax(bestMm, b, depth, player, *mm.get());
  const int scoreAb =
      AlphaBeta(bestAb, -32000, 32000, b, depth, player, *ab.get());
  const int scoreInc = AlphaBetaInc(bestInc, rootEval, -32000, 32000, b, depth,
                                    player, *inc.get());
  const int scoreRadix = RadixSortedAlphaBeta(bestRadix, rootEval, -32000, 32000,
                                              b, 0, depth, player, *radix.get());
  INFO("depth=" << depth << " player=" << player);
  CHECK(scoreAb == scoreMm);
  CHECK(scoreInc == scoreMm);
  CHECK(scoreRadix == scoreMm);

  const auto legal = LegalKeys(b, player);
  REQUIRE(!legal.empty());
  CHECK(Contains(legal, bestMm.key()));
  CHECK(Contains(legal, bestAb.key()));
  CHECK(Contains(legal, bestInc.key()));
  CHECK(Contains(legal, bestRadix.key()));
}

}  // namespace

TEST_CASE("search: minimax == alphabeta == incremental == radix") {
  InitTables();
  const SearchConfig def(EvalWeights::Default(), false, 1000);
  const SearchConfig twk(EvalWeights::Tweak(), false, 1000);

  const Board classical = ClassicalBoard();
  for (int depth = 1; depth <= 3; ++depth) {
    CheckEquivalence(classical, 0, def, depth);
    CheckEquivalence(classical, 1, def, depth);
  }
  CheckEquivalence(classical, 0, twk, 2);
  CheckEquivalence(classical, 1, twk, 2);

  // A few positions after a fixed first move.
  MoveList root;
  ComputeMoveList(root, classical, 0);
  const auto& executors = Executors();
  int taken = 0;
  for (int d = 0; d < kNumDirs && taken < 2; ++d) {
    for (int id = Single_1; id < kNumMoveIds && taken < 2; ++id) {
      uint64_t m = root.masks[d][id];
      while (m && taken < 2) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        Board child = classical;
        ApplyMove(child, executors[ExecutorIndex(d, id, cell)], 0);
        CheckEquivalence(child, 1, def, 2);
        ++taken;
      }
    }
  }

  uint64_t seed = 99;
  for (int i = 0; i < 4; ++i) {
    const Board b = RandomBoard(seed, 4 + i);
    CheckEquivalence(b, 0, def, 2);
    CheckEquivalence(b, 1, twk, 2);
  }
}

TEST_CASE("search: sorted alphabeta is exact at depth 1") {
  InitTables();
  const SearchConfig config(EvalWeights::Default(), false, 1000);

  uint64_t seed = 5;
  std::vector<Board> positions = {ClassicalBoard()};
  for (int i = 0; i < 5; ++i) positions.push_back(RandomBoard(seed, 5 + i));

  for (Board b : positions) {
    for (int player = 0; player < 2; ++player) {
      const int rootEval = Eval(config.weights, b);
      // Brute force: the best move at depth 1 is the best-cost legal move.
      MoveList moves;
      ComputeMoveList(moves, b, player);
      int bestCost = -100000;
      for (int d = 0; d < kNumDirs; ++d)
        for (int id = Single_1; id < kNumMoveIds; ++id) {
          uint64_t m = moves.masks[d][id];
          while (m) {
            const int cell = __builtin_ctzll(m);
            m &= m - 1;
            const int cost = config.costOfExecutor[ExecutorIndex(d, id, cell)];
            if (cost > bestCost) bestCost = cost;
          }
        }
      REQUIRE(bestCost > -100000);
      const int sign = (player == 0) ? 1 : -1;

      CtxPtr ctx(config);
      Move best;
      const int score =
          SortedAlphaBeta(best, rootEval, -32000, 32000, b, 0, 1, player,
                          *ctx.get());
      INFO("player=" << player);
      CHECK(score == rootEval + bestCost * sign);
      CHECK(config.costOfExecutor[ExecutorIndex(best.dir, best.id, best.cell)] ==
            bestCost);
      CHECK(Contains(LegalKeys(b, player), best.key()));
    }
  }
}

TEST_CASE("search: sorted alphabeta returns legal moves at depth 3") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();
  for (int player = 0; player < 2; ++player) {
    CtxPtr ctx(config);
    Board b = classical;
    Move best;
    const int rootEval = Eval(config.weights, b);
    const int score = SortedAlphaBeta(best, rootEval, -32000, 32000, b, 0, 3,
                                      player, *ctx.get());
    CHECK(Contains(LegalKeys(b, player), best.key()));
    // The leaf shortcut approximates the horizon with the best candidate
    // cost, so the score may deviate from the exact minimax value, but
    // only within the cost range.
    CtxPtr exact(config);
    Move dummy;
    const int exactScore =
        MiniMax(dummy, b, 3, player, *exact.get());
    CHECK(score > exactScore - 3000);
    CHECK(score < exactScore + 3000);
  }
}

TEST_CASE("search: quiescent config stays legal") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), true, 1000);
  CHECK(config.quiescentMarker < 1000);
  const Board classical = ClassicalBoard();
  for (int player = 0; player < 2; ++player) {
    CtxPtr ctx(config);
    Board b = classical;
    Move best;
    const int rootEval = Eval(config.weights, b);
    SortedAlphaBeta(best, rootEval, -32000, 32000, b, 0, 3, player, *ctx.get());
    CHECK(Contains(LegalKeys(b, player), best.key()));
  }
}

TEST_CASE("search: no legal move") {
  InitTables();
  // A lone surrounded marble cannot move (1 vs 1 pushes are illegal).
  Board b{{0, 0}};
  b.p[0] = kBit[CellFromName("e5")];
  for (int d = 0; d < kNumDirs; ++d)
    b.p[1] |= kBit[kNeighbor[CellFromName("e5")][d]];
  MoveList moves;
  ComputeMoveList(moves, b, 0);
  CHECK(MoveCount(moves) == 0);

  const SearchConfig config(EvalWeights::Default(), false, 1000);
  for (Algo algo : {Algo::MiniMax, Algo::AB_Rnd, Algo::AB_Sort, Algo::AB_Radix}) {
    CtxPtr ctx(config);
    CHECK(GenMove(b, 0, config, algo, 3, *ctx.get()) == -1);
  }
}

TEST_CASE("search: genmove dispatch") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();
  for (Algo algo : {Algo::MiniMax, Algo::AB_Rnd, Algo::AB_Sort, Algo::AB_Radix}) {
    CtxPtr ctx(config);
    const int idx = GenMove(classical, 0, config, algo, 3, *ctx.get());
    INFO("algo=" << static_cast<int>(algo));
    REQUIRE(idx >= 0);
    const Move& m = Executors()[idx].move;
    CHECK(Contains(LegalKeys(classical, 0), m.key()));
  }
}
