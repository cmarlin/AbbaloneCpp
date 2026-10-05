// Move enumeration, executor masks and bit-parallel move generation.
//
// Geometric moves are enumerated once (same order as the C#
// MoveItem.AllMoves(): dir, moveId, row, column) and each gets a
// precomputed Executor with from/to masks for O(1) apply/rollback.
//
// ComputeMoveList is the port of the C# ComputeMoveList: per direction and
// move id, a bitmask of legal origins, computed with board shifts.

#include "bb_core.h"

#include <cstddef>

namespace bb {

int16_t gExecutorIndex[kNumDirs][kNumMoveIds][kNumCells];

namespace {

std::vector<Executor> gExecutors;
bool gInit = false;

bool ChainFits(int cell, int dir, int len) {
  int c = cell;
  for (int i = 1; i < len; ++i) {
    int n = kNeighbor[c][dir];
    if (n < 0) return false;
    c = n;
  }
  return true;
}

// Build the from/to masks of a geometrically valid move.
Executor BuildExecutor(int dir, int id, int cell) {
  Executor e{};
  e.move.dir = static_cast<uint8_t>(dir);
  e.move.id = static_cast<uint8_t>(id);
  e.move.cell = static_cast<uint8_t>(cell);

  if (!IsSlide(id)) {
    // Inline push: chain of MoveDepth(id) cells from the origin.
    int len = MoveDepth(id);
    int own = OwnDepth(id);
    int cells[6];
    int c = cell;
    for (int i = 0; i < len; ++i) {
      cells[i] = c;
      c = kNeighbor[c][dir];
    }
    // Own marbles cells[0..own-1] shift one step; opponent marbles
    // cells[own..len-1] shift one step; the last opponent marble is
    // ejected when its destination is off-board (no 'to' bit).
    for (int i = 0; i < own; ++i) {
      e.fromCur |= kBit[cells[i]];
      e.toCur |= kBit[kNeighbor[cells[i]][dir]];
    }
    for (int i = own; i < len; ++i) {
      e.fromOpp |= kBit[cells[i]];
      int n = kNeighbor[cells[i]][dir];
      if (n >= 0) e.toOpp |= kBit[n];
    }
  } else {
    // Broadside slide: marbles at cell, cell+s, ... (+s twice), each moves
    // one step in direction dir. No ejection possible.
    int s = (dir + 1 + SisterIndex(id)) % kNumDirs;
    int count = SlideCount(id);
    int c = cell;
    for (int i = 0; i < count; ++i) {
      e.fromCur |= kBit[c];
      e.toCur |= kBit[kNeighbor[c][dir]];
      c = kNeighbor[c][s];
    }
  }
  return e;
}

}  // namespace

const std::vector<Executor>& Executors() {
  InitTables();
  if (!gInit) {
    for (int i = 0; i < kNumDirs; ++i)
      for (int j = 0; j < kNumMoveIds; ++j)
        for (int c = 0; c < kNumCells; ++c) gExecutorIndex[i][j][c] = -1;

    for (int dir = 0; dir < kNumDirs; ++dir) {
      for (int id = Single_1; id < kNumMoveIds; ++id) {
        for (int cell = 0; cell < kNumCells; ++cell) {
          bool fits;
          if (!IsSlide(id)) {
            // The chain cells must exist; when the whole chain is made of
            // own marbles (pure move) the destination of the last one must
            // exist too (own marbles are never ejected). For pushes the
            // destination of the last opponent marble may be off-board
            // (ejection), decided by the board at generation time.
            const int len = MoveDepth(id);
            fits = ChainFits(cell, dir, len);
            if (fits && OwnDepth(id) == len) {
              int end = cell;
              for (int i = 1; i < len; ++i) end = kNeighbor[end][dir];
              fits = kNeighbor[end][dir] >= 0;
            }
          } else {
            // Every moved marble must have an on-board destination.
            int s = (dir + 1 + SisterIndex(id)) % kNumDirs;
            int c = cell;
            fits = true;
            for (int i = 0; i < SlideCount(id); ++i) {
              if (c < 0 || kNeighbor[c][dir] < 0) {
                fits = false;
                break;
              }
              c = kNeighbor[c][s];
            }
          }
          if (!fits) continue;
          gExecutorIndex[dir][id][cell] = static_cast<int16_t>(gExecutors.size());
          gExecutors.push_back(BuildExecutor(dir, id, cell));
        }
      }
    }
    gInit = true;
  }
  return gExecutors;
}

int ExecutorIndex(int dir, int id, int cell) {
  Executors();
  if (dir < 0 || dir >= kNumDirs || id < 0 || id >= kNumMoveIds ||
      cell < 0 || cell >= kNumCells)
    return -1;
  return gExecutorIndex[dir][id][cell];
}

void ComputeMoveList(MoveList& out, const Board& b, int player) {
  InitTables();
  const uint64_t own = b.p[player];
  const uint64_t opp = b.p[1 - player];

  // One-step shifts of each player board in every direction: the first
  // level of every per-direction chain below is a lookup instead of a
  // shift (P and PS first levels hit all 6 directions over the loop).
  uint64_t ownS[kNumDirs], oppS[kNumDirs];
  for (int k = 0; k < kNumDirs; ++k) {
    ownS[k] = Shift(own, k);
    oppS[k] = Shift(opp, k);
  }

  for (int d = 0; d < kNumDirs; ++d) {
    const int back = (d + 3) % kNumDirs;  // pre-image direction
    // P(x) = {origins c whose next cell in direction d is in x};
    // P^k(x) = {origins whose k-th cell exists and is in x}.
    // Shift is linear (P(a|b) == P(a) | P(b)) and P^k(empty) =
    // kPreValid[d][k-1] & ~P^k(occupied), so only the occupied chains
    // need shifts, at the exact depths the move masks use.
    const uint64_t pOwn0 = ownS[back];
    const uint64_t pOpp0 = oppS[back];
    const uint64_t pOcc0 = pOwn0 | pOpp0;
    const uint64_t pEmpty1 = kPreValid[d][0] & ~pOcc0;
    const uint64_t pOwn1 = Shift(pOwn0, back);
    const uint64_t pOpp1 = Shift(pOpp0, back);
    const uint64_t pOcc1 = pOwn1 | pOpp1;
    const uint64_t pEmpty2 = kPreValid[d][1] & ~pOcc1;
    const uint64_t pOpp2 = Shift(pOpp1, back);
    const uint64_t pOcc2 = Shift(pOcc1, back);
    const uint64_t pEmpty3 = kPreValid[d][2] & ~pOcc2;
    const uint64_t pOpp3 = Shift(pOpp2, back);
    const uint64_t pOcc3 = Shift(pOcc2, back);
    const uint64_t pOcc4 = Shift(pOcc3, back);

    // Single moves: origin = rear of the chain. The cell after the chain
    // must be empty, or off-board for pushes (~pOcc drops both cases of
    // an occupied next cell and keeps off-board origins).
    out.masks[d][Single_1] = own & pEmpty1;
    out.masks[d][Single_2] = own & pOwn0 & pEmpty2;
    out.masks[d][Single_2VS1] = own & pOwn0 & pOpp1 & ~pOcc2;
    out.masks[d][Single_3] = own & pOwn0 & pOwn1 & pEmpty3;
    out.masks[d][Single_3VS1] = own & pOwn0 & pOwn1 & pOpp2 & ~pOcc3;
    out.masks[d][Single_3VS2] =
        own & pOwn0 & pOwn1 & pOpp2 & pOpp3 & ~pOcc4;

    // Slide moves: for each sister side, the moved marbles and all their
    // destinations must be own / empty respectively.
    for (int side = 0; side < 2; ++side) {
      const int s = (d + 1 + side) % kNumDirs;
      const int sBack = (s + 3) % kNumDirs;
      const uint64_t psOwn1 = ownS[sBack];
      const uint64_t psOwn2 = Shift(psOwn1, sBack);
      const uint64_t psEmpty1 = Shift(pEmpty1, sBack);
      const uint64_t psEmpty2 = Shift(psEmpty1, sBack);
      out.masks[d][Slide_2S0 + side] =
          own & psOwn1 & pEmpty1 & psEmpty1;
      out.masks[d][Slide_3S0 + side] =
          own & psOwn1 & psOwn2 & pEmpty1 & psEmpty1 & psEmpty2;
    }
  }
}

int MoveCount(const MoveList& moves) {
  int count = 0;
  for (int d = 0; d < kNumDirs; ++d)
    for (int id = Single_1; id < kNumMoveIds; ++id)
      count += __builtin_popcountll(moves.masks[d][id]);
  return count;
}

}  // namespace bb
