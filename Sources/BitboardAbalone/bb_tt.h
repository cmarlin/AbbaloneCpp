// Transposition table — exact-key design.
//
// The 2 x uint64 board IS the key: entries store (p[0], p[1], player) in
// full, so a probe can never return a wrong position (a hash collision
// only evicts a stale entry, it never yields a false hit). No Zobrist
// hashing, no incremental key maintenance.
//
// Scores stored by the search are absolute (player 0's perspective) and
// depth-relative: an entry records the remaining depth it was searched
// with, plus a bound flag (exact / lower / upper) in the standard
// alpha-beta sense. The table itself is policy-free: depth gating and
// cutoff decisions belong to the search (TtAlphaBeta).
//
// Replacement is depth-preferred: an entry is replaced only by an empty
// slot, the same key, or a search at least as deep. Single-threaded use
// only — no atomics.

#ifndef BB_TT_H_
#define BB_TT_H_

#include <cstddef>
#include <cstdint>

namespace bb {

enum TtFlag : uint8_t {
  kTtExact = 0,  // score is the true minimax value
  kTtLower = 1,  // true value >= score (fail high)
  kTtUpper = 2,  // true value <= score (fail low)
};

struct TtEntry {
  uint64_t key0;   // p[0] of the stored position
  uint64_t key1;   // p[1]
  int16_t score;   // absolute, player 0's perspective
  int16_t move;    // executor index of the best move, -1 if none
  int8_t depth;    // remaining depth the entry was searched with
  uint8_t flag;    // TtFlag
  uint8_t player;  // side to move
};
static_assert(sizeof(TtEntry) == 24, "TtEntry must stay packed at 24 bytes");

class TranspositionTable {
 public:
  // size_mb is rounded down to a power-of-two entry count.
  explicit TranspositionTable(size_t sizeMb = 64);
  ~TranspositionTable();

  TranspositionTable(const TranspositionTable&) = delete;
  TranspositionTable& operator=(const TranspositionTable&) = delete;

  // Reallocate (drops all entries) and clear.
  void Resize(size_t sizeMb);
  void Clear();

  // Probe a position. Returns true on a full key match and fills `out`.
  // The caller decides whether the entry's depth/flag are usable.
  bool Probe(uint64_t p0, uint64_t p1, int player, TtEntry& out);

  // Depth-preferred replacement (see file header).
  void Store(uint64_t p0, uint64_t p1, int player, int depth, int score,
             int flag, int move);

  // Stats since construction / last Clear.
  long probes = 0;
  long hits = 0;
  long stores = 0;

  size_t EntryCount() const { return count_; }
  size_t SizeBytes() const { return count_ * sizeof(TtEntry); }

 private:
  static uint64_t Mix(uint64_t x);

  TtEntry* entries_ = nullptr;
  size_t count_ = 0;  // power of two
  size_t mask_ = 0;
};

}  // namespace bb

#endif  // BB_TT_H_
