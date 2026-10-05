// Static tables for the bitboard core: dense cell mapping, neighbors,
// direction shifts, classical start.

#include "bb_core.h"

#include <algorithm>
#include <cstring>

namespace bb {

uint64_t kValid = 0;
uint64_t kBit[kNumCells];
int16_t kCellOf[kDim][kDim];
int8_t kRowOf[kNumCells];
int8_t kColOf[kNumCells];
int8_t kDist[kNumCells];
int16_t kNeighbor[kNumCells][kNumDirs];
uint64_t kShiftMask[kNumDirs][kDim];
int kShiftDelta[kNumDirs][kDim];
uint64_t kShiftPairMask[kNumDirs][kDim];
int kShiftPairDelta[kNumDirs][kDim];
int kShiftPairCount[kNumDirs];
uint64_t kPreValid[kNumDirs][3];

namespace {

const int kRowDelta[kNumDirs][2] = {
    {0, 1},   // E
    {1, 1},   // NE
    {1, 0},   // NW
    {0, -1},  // W
    {-1, -1},  // SW
    {-1, 0},  // SE
};

int RowStart(int r) { return std::max(0, r - 4); }
int RowEnd(int r) { return std::min(kDim - 1, r + 4); }

bool gInit = false;

}  // namespace

int Dist2(int row, int col) {
  int dy = row - kDim / 2;
  int dx = col - kDim / 2;
  if (dx * dy >= 0)
    return std::max(std::abs(dx), std::abs(dy));
  return std::abs(dx - dy);
}

void InitTables() {
  if (gInit) return;

  std::memset(kCellOf, -1, sizeof(kCellOf));
  std::memset(kNeighbor, -1, sizeof(kNeighbor));

  int cell = 0;
  for (int r = 0; r < kDim; ++r) {
    for (int c = RowStart(r); c <= RowEnd(r); ++c) {
      kCellOf[r][c] = static_cast<int16_t>(cell);
      kRowOf[cell] = static_cast<int8_t>(r);
      kColOf[cell] = static_cast<int8_t>(c);
      kDist[cell] = static_cast<int8_t>(Dist2(r, c));
      ++cell;
    }
  }

  kValid = 0;
  for (int i = 0; i < kNumCells; ++i) {
    kBit[i] = 1ull << i;
    kValid |= kBit[i];
  }

  for (int i = 0; i < kNumCells; ++i) {
    for (int d = 0; d < kNumDirs; ++d) {
      int r = kRowOf[i] + kRowDelta[d][0];
      int c = kColOf[i] + kRowDelta[d][1];
      if (r >= 0 && r < kDim && c >= 0 && c < kDim && kCellOf[r][c] >= 0)
        kNeighbor[i][d] = kCellOf[r][c];
    }
  }

  // Direction shifts: per source row, the mask of cells having a neighbor
  // in direction d and the dense-index delta to that neighbor.
  for (int d = 0; d < kNumDirs; ++d) {
    for (int r = 0; r < kDim; ++r) {
      kShiftMask[d][r] = 0;
      kShiftDelta[d][r] = 0;
      for (int c = RowStart(r); c <= RowEnd(r); ++c) {
        int i = kCellOf[r][c];
        int n = kNeighbor[i][d];
        if (n >= 0) {
          kShiftMask[d][r] |= kBit[i];
          kShiftDelta[d][r] = n - i;
        }
      }
    }
  }

  // Collapse the per-row masks into (mask, delta) pairs: the delta is
  // constant within a row, and only a few distinct deltas exist per
  // direction, so Shift does one masked shift per pair instead of one per
  // row.
  for (int d = 0; d < kNumDirs; ++d) {
    int n = 0;
    for (int r = 0; r < kDim; ++r) {
      const uint64_t m = kShiftMask[d][r];
      if (m == 0) continue;
      const int delta = kShiftDelta[d][r];
      int i = 0;
      while (i < n && kShiftPairDelta[d][i] != delta) ++i;
      if (i == n) {
        kShiftPairDelta[d][i] = delta;
        kShiftPairMask[d][i] = m;
        ++n;
      } else {
        kShiftPairMask[d][i] |= m;
      }
    }
    kShiftPairCount[d] = n;
  }

  // Pre-image chains of the valid mask: kPreValid[d][k-1] = P^k(kValid).
  // Must run after the shift pair tables above (Shift uses them).
  for (int d = 0; d < kNumDirs; ++d) {
    const int back = (d + 3) % kNumDirs;
    uint64_t v = Shift(kValid, back);
    for (int k = 0; k < 3; ++k) {
      kPreValid[d][k] = v;
      v = Shift(v, back);
    }
  }

  gInit = true;
}

Board ClassicalBoard() {
  InitTables();
  // Same layout as abalone_core ABALONE_INIT_CLASSIC:
  //   player 0: rows a (5), b (6), c (cols 2-4)
  //   player 1: rows i (5), h (6), g (cols 2-4)
  Board b{{0, 0}};
  for (int c = 0; c <= 4; ++c) b.p[0] |= kBit[kCellOf[0][c]];  // a1-a5
  for (int c = 0; c <= 5; ++c) b.p[0] |= kBit[kCellOf[1][c]];  // b1-b6
  for (int c = 2; c <= 4; ++c) b.p[0] |= kBit[kCellOf[2][c]];  // c3-c5
  for (int c = 4; c <= 6; ++c) b.p[1] |= kBit[kCellOf[6][c]];  // g5-g7
  for (int c = 3; c <= 8; ++c) b.p[1] |= kBit[kCellOf[7][c]];  // h4-h9
  for (int c = 4; c <= 8; ++c) b.p[1] |= kBit[kCellOf[8][c]];  // i5-i9
  return b;
}

}  // namespace bb
