// Bitboard core tests — phase 1: tables, cell mapping, notation, classical
// start, shifts, executors, move generation vs an independent rule-based
// reference, apply/rollback round-trip.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bb_core.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace bb;

namespace {

int Popcount(uint64_t x) { return __builtin_popcountll(x); }

std::vector<std::string> ClassicalP0Names() {
  return {"a1", "a2", "a3", "a4", "a5", "b1", "b2", "b3", "b4", "b5", "b6",
          "c3", "c4", "c5"};
}

std::vector<std::string> ClassicalP1Names() {
  return {"g5", "g6", "g7", "h4", "h5", "h6", "h7", "h8", "h9",
          "i5", "i6", "i7", "i8", "i9"};
}

uint64_t MaskFromNames(const std::vector<std::string>& names) {
  uint64_t m = 0;
  for (const auto& s : names) {
    int c = CellFromName(s);
    REQUIRE(c >= 0);
    m |= kBit[c];
  }
  return m;
}

// Independent rule-based legality predicate, written from the Abalone
// rules and the neighbour table only (no bit-parallel generation, no
// executors): does (dir, id, cell) describe a legal move for `player`?
bool IsLegalRef(const Board& b, int player, int dir, int id, int cell) {
  if (ExecutorIndex(dir, id, cell) < 0) return false;
  const uint64_t own = b.p[player];
  const uint64_t opp = b.p[1 - player];
  const uint64_t occ = own | opp;
  if (!IsSlide(id)) {
    const int len = MoveDepth(id);
    const int ownN = OwnDepth(id);
    int c = cell;
    for (int i = 0; i < len; ++i) {
      const bool isOwn = i < ownN;
      if (isOwn ? !((own >> c) & 1) : !((opp >> c) & 1)) return false;
      c = kNeighbor[c][dir];
    }
    // c = cell after the chain: off-board (ejection) or empty.
    return c < 0 || !((occ >> c) & 1);
  }
  const int s = (dir + 1 + SisterIndex(id)) % kNumDirs;
  int c = cell;
  for (int i = 0; i < SlideCount(id); ++i) {
    if (!((own >> c) & 1)) return false;
    const int dest = kNeighbor[c][dir];
    if (dest < 0 || ((occ >> dest) & 1)) return false;
    c = kNeighbor[c][s];
  }
  return true;
}

// Exhaustive comparison of ComputeMoveList against IsLegalRef.
void CheckMoveGen(const Board& b, int player) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t expected = 0;
      for (int cell = 0; cell < kNumCells; ++cell)
        if (IsLegalRef(b, player, d, id, cell)) expected |= kBit[cell];
      INFO("dir=" << d << " id=" << id);
      CHECK(moves.masks[d][id] == expected);
    }
  }
}

// Apply/rollback every legal move, checking board identity and invariants.
void CheckRoundTrip(const Board& b, int player) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  const auto& executors = Executors();
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        const Executor& e = executors[ExecutorIndex(d, id, cell)];
        Board copy = b;
        ApplyMove(copy, e, player);
        CHECK((copy.p[0] & copy.p[1]) == 0);
        CHECK((copy.p[0] | copy.p[1]) == ((copy.p[0] | copy.p[1]) & kValid));
        RollbackMove(copy, e, player);
        CHECK(copy.p[0] == b.p[0]);
        CHECK(copy.p[1] == b.p[1]);
      }
    }
  }
}

}  // namespace

TEST_CASE("tables: dense cell mapping") {
  InitTables();
  CHECK(Popcount(kValid) == 61);
  const int rowLen[9] = {5, 6, 7, 8, 9, 8, 7, 6, 5};
  int cell = 0;
  for (int r = 0; r < 9; ++r) {
    const int rowStart = cell;
    for (int c = 0; c < 9; ++c) {
      const bool inRow = c >= std::max(0, r - 4) && c <= std::min(8, r + 4);
      CHECK(kCellOf[r][c] == (inRow ? cell : -1));
      if (inRow) {
        CHECK(kRowOf[cell] == r);
        CHECK(kColOf[cell] == c);
        ++cell;
      }
    }
    CHECK(cell - rowStart == rowLen[r]);
  }
  CHECK(cell == 61);
}

TEST_CASE("tables: hex distance to center") {
  InitTables();
  CHECK(Dist2(4, 4) == 0);
  for (int i = 0; i < kNumCells; ++i) {
    CHECK(kDist[i] == Dist2(kRowOf[i], kColOf[i]));
    CHECK(kDist[i] >= 0);
    CHECK(kDist[i] <= 4);
    // Every off-board neighbour of a valid cell is at distance >= 5:
    // this is what makes the ejection test (newDist >= 5) sound.
    for (int d = 0; d < kNumDirs; ++d) {
      if (kNeighbor[i][d] < 0) {
        const int dr[6] = {0, 1, 1, 0, -1, -1};
        const int dc[6] = {1, 1, 0, -1, -1, 0};
        CHECK(Dist2(kRowOf[i] + dr[d], kColOf[i] + dc[d]) >= 5);
      }
    }
  }
}

TEST_CASE("notation: cell names") {
  InitTables();
  for (int i = 0; i < kNumCells; ++i) {
    const std::string name = CellName(i);
    CHECK(name.size() == 2);
    CHECK(CellFromName(name) == i);
  }
  CHECK(CellName(kCellOf[0][0]) == "a1");
  CHECK(CellName(kCellOf[4][4]) == "e5");
  CHECK(CellName(kCellOf[8][8]) == "i9");
  CHECK(CellFromName("a1") == kCellOf[0][0]);
  CHECK(CellFromName("A1") == kCellOf[0][0]);
  CHECK(CellFromName("e5") == kCellOf[4][4]);
  CHECK(CellFromName("a6") == -1);   // off the hexagon
  CHECK(CellFromName("e9") == kCellOf[4][8]);
  CHECK(CellFromName("a0") == -1);
  CHECK(CellFromName("j5") == -1);
  CHECK(CellFromName("e10") == -1);
  CHECK(CellFromName("") == -1);
  CHECK(CellFromName("5a") == -1);
  CHECK(CellName(-1) == "");
  CHECK(CellName(61) == "");
}

TEST_CASE("classical start position") {
  InitTables();
  const Board b = ClassicalBoard();
  CHECK(Popcount(b.p[0]) == 14);
  CHECK(Popcount(b.p[1]) == 14);
  CHECK((b.p[0] & b.p[1]) == 0);
  CHECK(b.p[0] == MaskFromNames(ClassicalP0Names()));
  CHECK(b.p[1] == MaskFromNames(ClassicalP1Names()));
}

TEST_CASE("shifts match the neighbour table") {
  InitTables();
  for (int i = 0; i < kNumCells; ++i) {
    for (int d = 0; d < kNumDirs; ++d) {
      const int n = kNeighbor[i][d];
      const uint64_t got = Shift(kBit[i], d);
      CHECK(got == (n >= 0 ? kBit[n] : 0u));
    }
  }
  // Shifting the whole board moves exactly the cells that have a
  // neighbour in that direction.
  for (int d = 0; d < kNumDirs; ++d) {
    uint64_t expected = 0;
    for (int i = 0; i < kNumCells; ++i)
      if (kNeighbor[i][d] >= 0) expected |= kBit[i];
    CHECK(Shift(kValid, d) == Shift(expected, d));
    CHECK(Popcount(Shift(kValid, d)) == Popcount(expected));
  }
  // Round trip: shifting d then the opposite direction restores the
  // cells that have both neighbours.
  for (int d = 0; d < kNumDirs; ++d) {
    const int back = (d + 3) % kNumDirs;
    uint64_t both = 0;
    for (int i = 0; i < kNumCells; ++i)
      if (kNeighbor[i][d] >= 0 && kNeighbor[i][back] >= 0) both |= kBit[i];
    CHECK(Shift(Shift(both, d), back) == both);
  }
}

TEST_CASE("executors: masks are well formed") {
  InitTables();
  const auto& executors = Executors();
  CHECK(executors.size() > 100);
  for (size_t i = 0; i < executors.size(); ++i) {
    const Executor& e = executors[i];
    const Move& m = e.move;
    INFO("executor " << i << " dir=" << m.dir << " id=" << m.id
                     << " cell=" << m.cell);
    CHECK(ExecutorIndex(m.dir, m.id, m.cell) >= 0);
    CHECK((e.fromCur & kValid) == e.fromCur);
    CHECK((e.toCur & kValid) == e.toCur);
    CHECK((e.fromOpp & kValid) == e.fromOpp);
    CHECK((e.toOpp & kValid) == e.toOpp);
    CHECK((e.fromCur & e.fromOpp) == 0);
    CHECK(e.toCur == Shift(e.fromCur, m.dir));
    CHECK(e.toOpp == Shift(e.fromOpp, m.dir));
    CHECK(Popcount(e.toCur) == Popcount(e.fromCur));
    if (!IsSlide(m.id)) {
      CHECK(Popcount(e.fromCur) == OwnDepth(m.id));
      const int pushed = MoveDepth(m.id) - OwnDepth(m.id);
      CHECK(Popcount(e.fromOpp) == pushed);
      // An ejected opponent marble has no destination bit.
      const int pushedTo = Popcount(e.toOpp);
      CHECK(pushedTo >= pushed - 1);
      CHECK(pushedTo <= pushed);
    } else {
      CHECK(Popcount(e.fromCur) == SlideCount(m.id));
      CHECK(e.fromOpp == 0);
      CHECK(e.toOpp == 0);
    }
  }
}

TEST_CASE("movegen: classical position, both players") {
  InitTables();
  const Board b = ClassicalBoard();
  for (int player = 0; player < 2; ++player) {
    CheckMoveGen(b, player);
    CheckRoundTrip(b, player);
    MoveList moves;
    ComputeMoveList(moves, b, player);
    CHECK(MoveCount(moves) > 0);
  }
}

TEST_CASE("movegen: positions after one move") {
  InitTables();
  const Board b0 = ClassicalBoard();
  MoveList moves;
  ComputeMoveList(moves, b0, 0);
  const auto& executors = Executors();
  int checked = 0;
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        Board child = b0;
        ApplyMove(child, executors[ExecutorIndex(d, id, cell)], 0);
        CheckMoveGen(child, 1);
        CheckRoundTrip(child, 1);
        ++checked;
      }
    }
  }
  CHECK(checked == MoveCount(moves));
}

TEST_CASE("apply/rollback: ejection on the NW edge") {
  InitTables();
  // Player 0 pushes g5-h5 north-west into i5, ejecting it (the g5-h5-i5
  // line is the NW diagonal: dr=+1, dc=0).
  Board b{{0, 0}};
  b.p[0] = kBit[CellFromName("g5")] | kBit[CellFromName("h5")];
  b.p[1] = kBit[CellFromName("i5")];
  const int idx = ExecutorIndex(NW, Single_2VS1, CellFromName("g5"));
  REQUIRE(idx >= 0);
  const Executor& e = Executors()[idx];
  Board after = b;
  ApplyMove(after, e, 0);
  CHECK(after.p[0] == (kBit[CellFromName("h5")] | kBit[CellFromName("i5")]));
  CHECK(after.p[1] == 0);
  RollbackMove(after, e, 0);
  CHECK(after.p[0] == b.p[0]);
  CHECK(after.p[1] == b.p[1]);
  // The same template shape one step in is a plain push (no ejection).
  Board b2{{0, 0}};
  b2.p[0] = kBit[CellFromName("e5")] | kBit[CellFromName("f5")];
  b2.p[1] = kBit[CellFromName("g5")];
  const int idx2 = ExecutorIndex(NW, Single_2VS1, CellFromName("e5"));
  REQUIRE(idx2 >= 0);
  Board after2 = b2;
  ApplyMove(after2, Executors()[idx2], 0);
  CHECK(after2.p[0] == (kBit[CellFromName("f5")] | kBit[CellFromName("g5")]));
  CHECK(after2.p[1] == kBit[CellFromName("h5")]);
}

TEST_CASE("eval: classical start is balanced") {
  InitTables();
  const Board b = ClassicalBoard();
  CHECK(Eval(EvalWeights::Default(), b) == 0);
  CHECK(Eval(EvalWeights::Tweak(), b) == 0);
}
