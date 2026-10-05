// Transposition table tests — phase 6: table mechanics (round trip,
// depth-preferred replacement, clear), exact-score equivalence of the
// TT-aware search against the plain radix search, node reduction,
// warm-table iterative deepening, cross-search pollution, the null-tt
// fallback and the GenMove dispatch.

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

}  // namespace

TEST_CASE("tt: store and probe round trip") {
  InitTables();
  TranspositionTable tt(1);
  TtEntry e;
  CHECK(tt.Probe(11, 22, 0, e) == false);
  CHECK(tt.hits == 0);

  tt.Store(11, 22, 0, 3, 123, kTtExact, 45);
  REQUIRE(tt.Probe(11, 22, 0, e));
  CHECK(e.key0 == 11);
  CHECK(e.key1 == 22);
  CHECK(e.score == 123);
  CHECK(e.depth == 3);
  CHECK(e.flag == kTtExact);
  CHECK(e.move == 45);
  CHECK(e.player == 0);

  // The key includes the side to move and both bitboards.
  CHECK(tt.Probe(11, 22, 1, e) == false);
  CHECK(tt.Probe(22, 11, 0, e) == false);
  CHECK(tt.Probe(11, 23, 0, e) == false);
  CHECK(tt.hits == 1);
  CHECK(tt.stores == 1);
}

TEST_CASE("tt: depth-preferred replacement") {
  InitTables();
  TranspositionTable tt(1);
  TtEntry e;

  tt.Store(1, 2, 0, 5, 50, kTtExact, -1);
  // Same key, shallower search: the deeper entry must survive.
  tt.Store(1, 2, 0, 2, 20, kTtExact, -1);
  REQUIRE(tt.Probe(1, 2, 0, e));
  CHECK(e.depth == 5);
  CHECK(e.score == 50);

  // Same key, deeper search: overwrites.
  tt.Store(1, 2, 0, 7, 70, kTtLower, 3);
  REQUIRE(tt.Probe(1, 2, 0, e));
  CHECK(e.depth == 7);
  CHECK(e.score == 70);
  CHECK(e.flag == kTtLower);
  CHECK(e.move == 3);

  // Same key, same depth: overwrites (re-search of the same position).
  tt.Store(1, 2, 0, 7, 71, kTtExact, 4);
  REQUIRE(tt.Probe(1, 2, 0, e));
  CHECK(e.score == 71);
  CHECK(e.flag == kTtExact);
}

TEST_CASE("tt: clear drops entries and stats") {
  InitTables();
  TranspositionTable tt(1);
  TtEntry e;
  tt.Store(1, 2, 0, 1, 10, kTtExact, -1);
  REQUIRE(tt.Probe(1, 2, 0, e));
  tt.Clear();
  CHECK(tt.probes == 0);
  CHECK(tt.hits == 0);
  CHECK(tt.stores == 0);
  CHECK(tt.Probe(1, 2, 0, e) == false);
}

TEST_CASE("tt: exact scores and node reduction vs radix") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);

  std::vector<Board> positions = {ClassicalBoard()};
  uint64_t seed = 7;
  for (int i = 0; i < 5; ++i) positions.push_back(RandomBoard(seed, 4 + i));

  for (const Board& bIn : positions) {
    for (int player = 0; player < 2; ++player) {
      for (int depth = 1; depth <= 4; ++depth) {
        Board b = bIn;
        const int rootEval = Eval(config.weights, b);

        CtxPtr radix(config);
        Move bestRadix;
        const int scoreRadix = RadixSortedAlphaBeta(
            bestRadix, rootEval, -32000, 32000, b, 0, depth, player,
            *radix.get());

        TranspositionTable tt(2);
        CtxPtr ttc(config);
        ttc.get()->tt = &tt;
        Move bestTt;
        const int scoreTt = TtAlphaBeta(bestTt, rootEval, -32000, 32000, b, 0,
                                        depth, player, *ttc.get());

        INFO("depth=" << depth << " player=" << player);
        CHECK(scoreTt == scoreRadix);
        CHECK(ttc.get()->nodeCount <= radix.get()->nodeCount);
        CHECK(Contains(LegalKeys(b, player), bestTt.key()));
      }
    }
  }

  // From depth 4 on the classical board the search meets real
  // transpositions (commuted own moves), so the table must hit.
  {
    Board b = ClassicalBoard();
    const int rootEval = Eval(config.weights, b);
    TranspositionTable tt(2);
    CtxPtr ttc(config);
    ttc.get()->tt = &tt;
    Move best;
    TtAlphaBeta(best, rootEval, -32000, 32000, b, 0, 4, 0, *ttc.get());
    CHECK(tt.hits > 0);
  }
}

TEST_CASE("tt: warm table across depths (iterative deepening)") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();
  TranspositionTable tt(4);

  for (int player = 0; player < 2; ++player) {
    for (int depth = 1; depth <= 5; ++depth) {
      Board b = classical;
      const int rootEval = Eval(config.weights, b);

      CtxPtr cold(config);
      Move bestCold;
      const int scoreCold = RadixSortedAlphaBeta(
          bestCold, rootEval, -32000, 32000, b, 0, depth, player,
          *cold.get());

      CtxPtr warm(config);
      warm.get()->tt = &tt;  // entries from depths 1..depth-1
      Move bestWarm;
      const int scoreWarm = TtAlphaBeta(bestWarm, rootEval, -32000, 32000, b,
                                        0, depth, player, *warm.get());

      INFO("depth=" << depth << " player=" << player);
      CHECK(scoreWarm == scoreCold);
      CHECK(warm.get()->nodeCount <= cold.get()->nodeCount);
      CHECK(Contains(LegalKeys(b, player), bestWarm.key()));
    }
  }
}

TEST_CASE("tt: interleaved searches do not pollute") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();
  MoveList root;
  ComputeMoveList(root, classical, 0);
  int firstIdx = -1;
  for (int d = 0; d < kNumDirs && firstIdx < 0; ++d)
    for (int id = Single_1; id < kNumMoveIds && firstIdx < 0; ++id) {
      const uint64_t m = root.masks[d][id];
      if (m) firstIdx = ExecutorIndex(d, id, __builtin_ctzll(m));
    }
  REQUIRE(firstIdx >= 0);
  Board other = classical;
  ApplyMove(other, Executors()[firstIdx], 0);

  TranspositionTable tt(2);
  int scoreA1 = 0, scoreA2 = 0;
  const Board* boards[] = {&classical, &other};
  for (int round = 0; round < 2; ++round) {
    for (const Board* pb : boards) {
      Board b = *pb;
      const int rootEval = Eval(config.weights, b);
      CtxPtr ctx(config);
      ctx.get()->tt = &tt;
      Move best;
      const int score = TtAlphaBeta(best, rootEval, -32000, 32000, b, 0, 4, 0,
                                    *ctx.get());
      if (round == 0 && pb == &classical) scoreA1 = score;
      if (round == 1 && pb == &classical) scoreA2 = score;
    }
  }
  CHECK(scoreA1 == scoreA2);
}

TEST_CASE("tt: null table falls back to plain radix behaviour") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();
  Board b0 = classical;
  const int rootEval = Eval(config.weights, b0);

  CtxPtr radix(config);
  Move bestRadix;
  const int scoreRadix = RadixSortedAlphaBeta(bestRadix, rootEval, -32000,
                                              32000, b0, 0, 4, 0,
                                              *radix.get());

  CtxPtr ctx(config);  // ctx->tt stays null
  Move bestTt;
  const int scoreTt = TtAlphaBeta(bestTt, rootEval, -32000, 32000, b0,
                                  0, 4, 0, *ctx.get());
  CHECK(scoreTt == scoreRadix);
  CHECK(ctx.get()->nodeCount == radix.get()->nodeCount);
}

TEST_CASE("tt: warm iterative deepening matches cold on a played game") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);

  // Deterministic self-play at d4 AB_Sort (same setup as the reference
  // auto-game) down to a mid-game position.
  Board b = ClassicalBoard();
  int player = 0;
  for (int ply = 0; ply < 40; ++ply) {
    CtxPtr ctx(config);
    const int idx = GenMove(b, player, config, Algo::AB_Sort, 4, *ctx.get());
    REQUIRE(idx >= 0);
    ApplyMove(b, Executors()[idx], player);
    player = 1 - player;
  }

  const int rootEval = Eval(config.weights, b);
  CtxPtr cold(config);
  Move bestCold;
  Board bc = b;
  const int scoreCold = RadixSortedAlphaBeta(bestCold, rootEval, -32000, 32000,
                                             bc, 0, 5, player, *cold.get());

  // Iterative deepening d1..d5 with a persistent TT: cross-iteration
  // entries fail the depth gate and act as move hints, so this exercises
  // the hint path on real mid-game trees.
  TranspositionTable tt(4);
  CtxPtr warm(config);
  warm.get()->tt = &tt;
  int scoreWarm = 0;
  for (int d = 1; d <= 5; ++d) {
    Board bw = b;
    const int ev = Eval(config.weights, bw);
    Move bestWarm;
    scoreWarm = TtAlphaBeta(bestWarm, ev, -32000, 32000, bw, 0, d, player,
                            *warm.get());
  }
  CHECK(scoreWarm == scoreCold);
}

TEST_CASE("tt: genmove dispatch") {
  InitTables();
  const SearchConfig config(EvalWeights::Tweak(), false, 1000);
  const Board classical = ClassicalBoard();

  TranspositionTable tt(2);
  CtxPtr ctx(config);
  ctx.get()->tt = &tt;
  const int idx = GenMove(classical, 0, config, Algo::AB_TT, 4, *ctx.get());
  REQUIRE(idx >= 0);
  CHECK(Contains(LegalKeys(classical, 0), Executors()[idx].move.key()));

  // Null table: AB_TT must still answer (plain radix path).
  CtxPtr noTt(config);
  const int idx2 = GenMove(classical, 0, config, Algo::AB_TT, 4, *noTt.get());
  REQUIRE(idx2 >= 0);
  CHECK(Contains(LegalKeys(classical, 0), Executors()[idx2].move.key()));

  // No legal move: same answer as the other algorithms.
  Board blocked{{0, 0}};
  blocked.p[0] = kBit[CellFromName("e5")];
  for (int d = 0; d < kNumDirs; ++d)
    blocked.p[1] |= kBit[kNeighbor[CellFromName("e5")][d]];
  CtxPtr stuck(config);
  stuck.get()->tt = &tt;
  CHECK(GenMove(blocked, 0, config, Algo::AB_TT, 3, *stuck.get()) == -1);
}
