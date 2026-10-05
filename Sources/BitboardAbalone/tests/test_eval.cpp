// Evaluation tests — phase 4: golden values, symmetry, and the central
// invariant ported from the C# test suite (AiAbbaloneToolsTest.cs):
//   Eval(child) == Eval(parent) + DynamicEval(move).cost   (player 0 moves)
//   Eval(child) == Eval(parent) - DynamicEval(move).cost   (player 1 moves)
// plus catchBall == an opponent marble was actually ejected.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"

#include <vector>

using namespace bb;

namespace {

int Popcount(uint64_t x) { return __builtin_popcountll(x); }

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

// Central parity invariant, for one player and one weight set.
void CheckDynamicInvariant(const Board& b, int player, const EvalWeights& w) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  const auto& executors = Executors();
  const int parentEval = Eval(w, b);
  const int sign = (player == 0) ? 1 : -1;
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        const Executor& e = executors[ExecutorIndex(d, id, cell)];
        const DynEval de = DynamicEval(w, e.move);
        Board child = b;
        ApplyMove(child, e, player);
        INFO("dir=" << d << " id=" << id << " cell=" << cell
                     << " player=" << player);
        CHECK(Eval(w, child) == parentEval + sign * de.cost);
        const bool ejected =
            Popcount(child.p[1 - player]) < Popcount(b.p[1 - player]);
        CHECK(de.catchBall == ejected);
      }
    }
  }
}

}  // namespace

TEST_CASE("eval: golden values") {
  InitTables();
  Board b{{0, 0}};
  b.p[0] = kBit[CellFromName("e5")];  // center, dist 0
  // Eval = (score0 - score1) * rfactor + (balls0 - balls1) * bfactor.
  CHECK(Eval(EvalWeights::Default(), b) == 9 * 5 + 1 * 61);
  CHECK(Eval(EvalWeights::Tweak(), b) == 7 * 5 + 1 * 75);
  b.p[1] = kBit[CellFromName("a1")];  // corner, dist 4
  CHECK(Eval(EvalWeights::Default(), b) == (9 - 1) * 5);
  CHECK(Eval(EvalWeights::Tweak(), b) == (7 - 0) * 5);
  b.p[0] |= kBit[CellFromName("a1")];  // both players on dist-4 cells
  CHECK(Eval(EvalWeights::Default(), b) == (9 + 1 - 1) * 5 + 1 * 61);
  CHECK(Eval(EvalWeights::Tweak(), b) == (7 + 0 - 0) * 5 + 1 * 75);
}

TEST_CASE("eval: antisymmetry") {
  InitTables();
  uint64_t seed = 42;
  for (int i = 0; i < 50; ++i) {
    const Board b = RandomBoard(seed, 4 + static_cast<int>(i % 8));
    const Board swapped{{b.p[1], b.p[0]}};
    CHECK(Eval(EvalWeights::Default(), swapped) == -Eval(EvalWeights::Default(), b));
    CHECK(Eval(EvalWeights::Tweak(), swapped) == -Eval(EvalWeights::Tweak(), b));
  }
}

TEST_CASE("dynamic eval: golden values") {
  InitTables();
  const EvalWeights def = EvalWeights::Default();
  const EvalWeights twk = EvalWeights::Tweak();

  // Single marble e5 -> f5 (dist 0 -> 1), player 0 moves east.
  Move m{E, Single_1, static_cast<uint8_t>(CellFromName("e5"))};
  DynEval de = DynamicEval(def, m);
  CHECK(de.cost == (7 - 9) * 5);
  CHECK(de.catchBall == false);
  de = DynamicEval(twk, m);
  CHECK(de.cost == (7 - 7) * 5);

  // Slide of marbles e5,f6 -> f5,g6 (sister NE of the east direction).
  m = Move{E, Slide_2S0, static_cast<uint8_t>(CellFromName("e5"))};
  de = DynamicEval(def, m);
  CHECK(de.cost == (6 - 9) * 5);
  CHECK(de.catchBall == false);

  // Ejection: player 0 pushes g5-h5 north-west, i5 falls off (dist 4 -> 5).
  m = Move{NW, Single_2VS1, static_cast<uint8_t>(CellFromName("g5"))};
  de = DynamicEval(def, m);
  // own: g5 (ring 6 -> 4), h5 (ring 4 -> 1); the pushed i5 loses its ring
  // contribution (ring 1) and costs bfactor on top.
  CHECK(de.cost == (4 - 6) * 5 + (1 - 4) * 5 + 1 * 5 + 61);
  CHECK(de.catchBall == true);
  de = DynamicEval(twk, m);
  CHECK(de.cost == (5 - 7) * 5 + (0 - 5) * 5 + 0 * 5 + 75);
  CHECK(de.catchBall == true);
}

TEST_CASE("dynamic eval: invariant on classical and children") {
  InitTables();
  const EvalWeights weights[] = {EvalWeights::Default(), EvalWeights::Tweak()};
  const Board b0 = ClassicalBoard();
  for (const EvalWeights& w : weights) {
    for (int player = 0; player < 2; ++player)
      CheckDynamicInvariant(b0, player, w);
  }
  // Every child of the classical root, for the opponent.
  MoveList moves;
  ComputeMoveList(moves, b0, 0);
  const auto& executors = Executors();
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        Board child = b0;
        ApplyMove(child, executors[ExecutorIndex(d, id, cell)], 0);
        CheckDynamicInvariant(child, 1, EvalWeights::Default());
      }
    }
  }
}

TEST_CASE("dynamic eval: invariant on random boards") {
  InitTables();
  uint64_t seed = 7;
  for (int i = 0; i < 60; ++i) {
    const Board b = RandomBoard(seed, 3 + static_cast<int>(i % 10));
    CheckDynamicInvariant(b, 0, EvalWeights::Default());
    CheckDynamicInvariant(b, 1, EvalWeights::Default());
    CheckDynamicInvariant(b, 0, EvalWeights::Tweak());
    CheckDynamicInvariant(b, 1, EvalWeights::Tweak());
  }
}

TEST_CASE("search config: costs, ranks and quiescent marker") {
  InitTables();
  const auto& executors = Executors();
  for (bool quiescent : {false, true}) {
    for (const EvalWeights w :
         {EvalWeights::Default(), EvalWeights::Tweak()}) {
      const SearchConfig config(w, quiescent, 1000);
      REQUIRE(config.costOfExecutor.size() == executors.size());
      REQUIRE(config.rankOfExecutor.size() == executors.size());
      REQUIRE(config.costFromRank.size() > 1);
      // Distinct, strictly descending costs.
      for (size_t r = 1; r < config.costFromRank.size(); ++r)
        CHECK(config.costFromRank[r - 1] > config.costFromRank[r]);
      // Rank consistency and 16-bit safety (the C# sbyte cast wraps here).
      int minEjectionCost = 1000;
      for (size_t i = 0; i < executors.size(); ++i) {
        const DynEval de = DynamicEval(w, executors[i].move);
        CHECK(config.costOfExecutor[i] == de.cost);
        CHECK(config.costFromRank[config.rankOfExecutor[i]] == de.cost);
        CHECK(de.cost > -32768);
        CHECK(de.cost < 32768);
        if (de.catchBall && quiescent && de.cost < minEjectionCost)
          minEjectionCost = de.cost;
      }
      CHECK(config.quiescentMarker == (quiescent ? minEjectionCost : 1000));
      CHECK(config.window == 1000);
    }
  }
}
