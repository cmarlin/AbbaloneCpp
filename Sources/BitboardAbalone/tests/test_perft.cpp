// Cross-validation of the bitboard core against the reference core
// (Sources/SpielAbalone): perft counts, board states along a deterministic
// walk, and per-child perft at the root.
//
// The two engines share the 9x9 coordinate system, the direction numbering
// (E/RIGHT=0 .. SE/DOWN_RIGHT=5) and the move taxonomy, so a bitboard move
// (dir, id, cell) maps to a reference action id directly:
//   action = ((row*9 + col)*6 + dir)*5 + moveType
//   moveType: 0 = single (all Single_* ids), 1 = Slide_2S0, 2 = Slide_2S1,
//             3 = Slide_3S0, 4 = Slide_3S1.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"
#include "open_spiel/games/abalone/abalone_core.h"

#include <vector>

namespace {

long RefPerft(const abalone_core::core_state& s, int depth) {
  if (depth == 0) return 1;
  long count = 0;
  for (int a = 0; a < abalone_core::kActionMax; ++a) {
    const auto mv = abalone_core::Move::ActionToMove(a);
    if (!mv.IsValid(s)) continue;
    abalone_core::core_state child = s;
    mv.Apply(child);
    count += RefPerft(child, depth - 1);
  }
  return count;
}

int BbMoveToRefAction(int dir, int id, int cell) {
  int moveType = 0;
  switch (id) {
    case bb::Slide_2S0: moveType = 1; break;
    case bb::Slide_2S1: moveType = 2; break;
    case bb::Slide_3S0: moveType = 3; break;
    case bb::Slide_3S1: moveType = 4; break;
    default: break;  // all Single_* ids share action type 0
  }
  return ((bb::kRowOf[cell] * 9 + bb::kColOf[cell]) * 6 + dir) * 5 + moveType;
}

bool BoardsEqual(const bb::Board& b, const abalone_core::core_state& s) {
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      const int cell = bb::kCellOf[r][c];
      const uint64_t bit = (cell >= 0) ? bb::kBit[cell] : 0;
      const bool p0 = (b.p[0] & bit) != 0;
      const bool p1 = (b.p[1] & bit) != 0;
      const auto st = s.board_[r][c];
      if (p0 != (st == abalone_core::CellState::Player0)) return false;
      if (p1 != (st == abalone_core::CellState::Player1)) return false;
      if ((st == abalone_core::CellState::Invalid) != (cell < 0)) return false;
    }
  }
  return true;
}

struct BbMove {
  int dir, id, cell;
};

std::vector<BbMove> LegalBbMoves(const bb::Board& b, int player) {
  bb::MoveList moves;
  bb::ComputeMoveList(moves, b, player);
  std::vector<BbMove> out;
  for (int d = 0; d < bb::kNumDirs; ++d)
    for (int id = bb::Single_1; id < bb::kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        out.push_back({d, id, cell});
      }
    }
  return out;
}

}  // namespace

TEST_CASE("perft: classical depths 1-3 match the reference core") {
  bb::InitTables();
  const bb::Board b = bb::ClassicalBoard();
  abalone_core::core_state ref;
  ref.Reset(abalone_core::ABALONE_INIT_CLASSIC);
  REQUIRE(BoardsEqual(b, ref));
  for (int depth = 1; depth <= 3; ++depth) {
    INFO("depth " << depth);
    CHECK(bb::Perft(b, depth, 0) == RefPerft(ref, depth));
  }
}

TEST_CASE("perft: golden values (validated to depth 5 against the reference)") {
  bb::InitTables();
  const bb::Board b = bb::ClassicalBoard();
  // Depth 5 (283320928) was validated once against the reference core in
  // Release; it is too slow to keep in the test run.
  CHECK(bb::Perft(b, 1, 0) == 44);
  CHECK(bb::Perft(b, 2, 0) == 1936);
  CHECK(bb::Perft(b, 3, 0) == 98912);
  CHECK(bb::Perft(b, 4, 0) == 5045110);
}

TEST_CASE("perft: root children match one by one") {
  bb::InitTables();
  const bb::Board b0 = bb::ClassicalBoard();
  abalone_core::core_state ref0;
  ref0.Reset(abalone_core::ABALONE_INIT_CLASSIC);
  const auto& executors = bb::Executors();
  for (const BbMove& m : LegalBbMoves(b0, 0)) {
    bb::Board child = b0;
    bb::ApplyMove(child, executors[bb::ExecutorIndex(m.dir, m.id, m.cell)], 0);
    abalone_core::core_state refChild = ref0;
    const auto refMove =
        abalone_core::Move::ActionToMove(BbMoveToRefAction(m.dir, m.id, m.cell));
    REQUIRE(refMove.IsValid(ref0));
    refMove.Apply(refChild);
    INFO("move dir=" << m.dir << " id=" << m.id << " cell=" << m.cell);
    CHECK(BoardsEqual(child, refChild));
    CHECK(bb::Perft(child, 2, 1) == RefPerft(refChild, 2));
  }
}

TEST_CASE("walk: 24 plies, states and perft(1) stay in sync") {
  bb::InitTables();
  bb::Board b = bb::ClassicalBoard();
  abalone_core::core_state ref;
  ref.Reset(abalone_core::ABALONE_INIT_CLASSIC);
  const auto& executors = bb::Executors();
  int player = 0;
  for (int ply = 0; ply < 24; ++ply) {
    const auto legal = LegalBbMoves(b, player);
    REQUIRE(!legal.empty());
    const BbMove& m = legal[(ply * 7 + 3) % legal.size()];
    const int action = BbMoveToRefAction(m.dir, m.id, m.cell);
    const auto refMove = abalone_core::Move::ActionToMove(action);
    INFO("ply " << ply << " action " << action);
    REQUIRE(refMove.IsValid(ref));
    bb::Board child = b;
    bb::ApplyMove(child, executors[bb::ExecutorIndex(m.dir, m.id, m.cell)],
                  player);
    abalone_core::core_state refChild = ref;
    refMove.Apply(refChild);
    b = child;
    ref = refChild;
    CHECK(BoardsEqual(b, ref));
    CHECK(bb::Perft(b, 1, 1 - player) == RefPerft(ref, 1));
    if (ply % 6 == 5) {
      CHECK(bb::Perft(b, 2, 1 - player) == RefPerft(ref, 2));
    }
    player = 1 - player;
  }
}
