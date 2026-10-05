// ATP notation tests — phase 6: MoveToAtp / TryParseAtp round-trip over
// every legal move, canonical-form parity with the reference core
// (Sources/SpielAbalone Move::ToString), and rejection of bad input.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"
#include "open_spiel/games/abalone/abalone_boards.h"
#include "open_spiel/games/abalone/abalone_core.h"

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

int BbMoveToRefAction(int dir, int id, int cell) {
  int moveType = 0;
  switch (id) {
    case Slide_2S0: moveType = 1; break;
    case Slide_2S1: moveType = 2; break;
    case Slide_3S0: moveType = 3; break;
    case Slide_3S1: moveType = 4; break;
    default: break;
  }
  return ((kRowOf[cell] * 9 + kColOf[cell]) * 6 + dir) * 5 + moveType;
}

// Round-trip every legal move and compare the canonical string with the
// reference core's Move::ToString for the same physical move.
void CheckAtpRoundTrip(const Board& b, int player,
                       const abalone_core::core_state& ref) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        const Move mv{static_cast<uint8_t>(d), static_cast<uint8_t>(id),
                      static_cast<uint8_t>(cell)};
        const std::string atp = MoveToAtp(b, player, mv);
        INFO("dir=" << d << " id=" << id << " cell=" << cell);
        REQUIRE(atp.size() == (IsSlide(id) ? 6u : 4u));

        Move back;
        REQUIRE(TryParseAtp(b, player, atp, back));
        CHECK(back.dir == mv.dir);
        CHECK(back.id == mv.id);
        CHECK(back.cell == mv.cell);

        // Canonical form parity with the reference core.
        const auto refMove = abalone_core::Move::ActionToMove(
            BbMoveToRefAction(d, id, cell));
        REQUIRE(refMove.IsValid(ref));
        CHECK(atp == refMove.ToString());

        // Case-insensitive parse.
        std::string upper = atp;
        for (char& ch : upper)
          ch = static_cast<char>(ch - ((ch >= 'a' && ch <= 'z') ? 32 : 0));
        Move back2;
        REQUIRE(TryParseAtp(b, player, upper, back2));
        CHECK(back2.key() == mv.key());
      }
    }
  }
}

}  // namespace

TEST_CASE("atp: round-trip and canonical form vs reference") {
  InitTables();
  abalone_core::core_state ref;
  ref.Reset(abalone_core::ABALONE_INIT_CLASSIC);
  const Board classical = ClassicalBoard();
  CheckAtpRoundTrip(classical, 0, ref);
  ref.turn_ = 1;
  CheckAtpRoundTrip(classical, 1, ref);
  ref.turn_ = 0;

  // Children of the classical root (board states stay in sync).
  MoveList root;
  ComputeMoveList(root, classical, 0);
  const auto& executors = Executors();
  int taken = 0;
  for (int d = 0; d < kNumDirs && taken < 6; ++d) {
    for (int id = Single_1; id < kNumMoveIds && taken < 6; ++id) {
      uint64_t m = root.masks[d][id];
      while (m && taken < 6) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        Board child = classical;
        ApplyMove(child, executors[ExecutorIndex(d, id, cell)], 0);
        abalone_core::core_state refChild = ref;
        abalone_core::Move::ActionToMove(BbMoveToRefAction(d, id, cell))
            .Apply(refChild);
        CheckAtpRoundTrip(child, 1, refChild);
        ++taken;
      }
    }
  }

  uint64_t seed = 21;
  for (int i = 0; i < 8; ++i) {
    const Board b = RandomBoard(seed, 4 + i);
    // Reference state built from the bitboard.
    abalone_core::core_state rs;
    rs.Reset(abalone_core::ABALONE_INIT_CLASSIC);
    for (int r = 0; r < 9; ++r)
      for (int c = 0; c < 9; ++c) {
        const int cell = kCellOf[r][c];
        if (cell < 0) continue;
        if ((b.p[0] >> cell) & 1) rs.board_[r][c] = abalone_core::CellState::Player0;
        else if ((b.p[1] >> cell) & 1) rs.board_[r][c] = abalone_core::CellState::Player1;
        else rs.board_[r][c] = abalone_core::CellState::Empty;
      }
    rs.turn_ = 0;
    CheckAtpRoundTrip(b, 0, rs);
    rs.turn_ = 1;
    CheckAtpRoundTrip(b, 1, rs);
  }
}

TEST_CASE("atp: known moves and rejections") {
  InitTables();
  const Board b = ClassicalBoard();

  // Hand-written strings: a1b2 moves the whole a1-b2-c3 chain north-east
  // (d4 is empty), c5c6 is a single-marble move.
  Move m;
  CHECK(TryParseAtp(b, 0, "a1b2", m));
  CHECK(m.dir == NE);
  CHECK(m.id == Single_3);
  CHECK(m.cell == CellFromName("a1"));

  CHECK(TryParseAtp(b, 0, "c5c6", m));
  CHECK(m.dir == E);
  CHECK(m.id == Single_1);
  CHECK(m.cell == CellFromName("c5"));

  CHECK(TryParseAtp(b, 0, "A1B2", m));  // case-insensitive
  CHECK(m.cell == CellFromName("a1"));

  // Illegal / malformed input.
  CHECK_FALSE(TryParseAtp(b, 0, "a1a2", m));   // own marble ahead (a1-a5 line)
  CHECK_FALSE(TryParseAtp(b, 0, "e5e6", m));  // empty source cell
  CHECK_FALSE(TryParseAtp(b, 0, "a1b2c3", m));  // third cell is not one step
  CHECK_FALSE(TryParseAtp(b, 0, "a1", m));
  CHECK_FALSE(TryParseAtp(b, 0, "a1b2c3d4", m));
  CHECK_FALSE(TryParseAtp(b, 0, "z9z9", m));
  CHECK_FALSE(TryParseAtp(b, 1, "a1b2", m));  // not black's marble

  // MoveToAtp rejects illegal moves.
  Move bad{E, Single_1, static_cast<uint8_t>(CellFromName("e5"))};
  CHECK(MoveToAtp(b, 0, bad).empty());
  Move badChain{NE, Single_1, static_cast<uint8_t>(CellFromName("a1"))};
  CHECK(MoveToAtp(b, 0, badChain).empty());  // a1b2 is a Single_3 here
  Move good{E, Single_1, static_cast<uint8_t>(CellFromName("c5"))};
  CHECK(MoveToAtp(b, 0, good) == "c5c6");
}

// Bitboard of a reference starting table (row 0 = 'a', as kCellOf).
Board BoardFromRefTable(
    const abalone_core::CellState (&table)[abalone_core::kNumRows]
                                          [abalone_core::kNumCols]) {
  Board b{{0, 0}};
  for (int row = 0; row < kDim; ++row)
    for (int col = 0; col < kDim; ++col) {
      const int cell = kCellOf[row][col];
      if (cell < 0) continue;
      if (table[row][col] == abalone_core::Player0) b.p[0] |= kBit[cell];
      if (table[row][col] == abalone_core::Player1) b.p[1] |= kBit[cell];
    }
  return b;
}

TEST_CASE("layouts: belgian daisy matches the reference table") {
  InitTables();
  const Board ref = BoardFromRefTable(abalone_core::ABALONE_INIT_BELGIAN_DAISY);
  const Board b = BelgianDaisyBoard();
  CHECK(b.p[0] == ref.p[0]);
  CHECK(b.p[1] == ref.p[1]);
  CHECK(__builtin_popcountll(b.p[0]) == 14);
  CHECK(__builtin_popcountll(b.p[1]) == 14);

  Board named{{0, 0}};
  CHECK(LayoutBoard("belgian-daisy", &named));
  CHECK(named.p[0] == b.p[0]);
  CHECK(LayoutBoard("classic", &named));
  CHECK(named.p[0] == ClassicalBoard().p[0]);
  CHECK_FALSE(LayoutBoard("snake", &named));
}

TEST_CASE("layouts: board text round-trip and rejections") {
  InitTables();
  // OpenSpiel display of the classical start, labels included
  const std::string classical =
      "board = \n"
      "<i>           2   2   2   2   2\n"
      "<h>         2   2   2   2   2   2\n"
      "<g>       .   .   2   2   2   .   .\n"
      "<f>     .   .   .   .   .   .   .   .\n"
      "<e>   .   .   .   .   .   .   .   .   .\n"
      "<d>     .   .   .   .   .   .   .   .  <9>\n"
      "<c>       .   .   1   1   1   .   .  <8>\n"
      "<b>         1   1   1   1   1   1  <7>\n"
      "<a>           1   1   1   1   1  <6>\n"
      "               <1> <2> <3> <4> <5>\n"
      "move_number_ = 0\n";
  Board b{{0, 0}};
  std::string error;
  REQUIRE(ParseBoardText(classical, &b, &error));
  CHECK(b.p[0] == ClassicalBoard().p[0]);
  CHECK(b.p[1] == ClassicalBoard().p[1]);

  std::string missing = classical;
  missing.erase(missing.find("<e>"), missing.find("<d>") - missing.find("<e>"));
  CHECK_FALSE(ParseBoardText(missing, &b, &error));
  CHECK(error == "row <e> missing");

  std::string bad = classical;
  bad.replace(bad.find("<a>           1"), 16, "<a>           x");
  CHECK_FALSE(ParseBoardText(bad, &b, &error));

  std::string short_row = classical;
  short_row.replace(short_row.find("1   1   1   1   1  <6>"), 22, "1   1   1   1  <6>");
  CHECK_FALSE(ParseBoardText(short_row, &b, &error));
  CHECK(error == "missing cells in row <a>");
}

