// Perft for the bitboard core (leaf counting), used by the cross-validation
// tests against the reference core (Sources/SpielAbalone).

#include "bb_core.h"

namespace bb {

long Perft(const Board& b, int depth, int player) {
  InitTables();
  if (depth == 0) return 1;
  MoveList moves;
  ComputeMoveList(moves, b, player);
  if (depth == 1) return MoveCount(moves);
  const auto& executors = Executors();
  long count = 0;
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        Board child = b;
        ApplyMove(child, executors[ExecutorIndex(d, id, cell)], player);
        count += Perft(child, depth - 1, 1 - player);
      }
    }
  }
  return count;
}

}  // namespace bb
