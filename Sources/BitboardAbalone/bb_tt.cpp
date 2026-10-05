// Transposition table implementation. See bb_tt.h for the design.

#include "bb_tt.h"

#include <cstring>

namespace bb {

namespace {

// Empty-slot marker: Clear() fills the array with 0xFF, so an unused entry
// has player == 0xFF, which no probe (player 0 or 1) can ever match.
constexpr uint8_t kTtEmpty = 0xFF;

}  // namespace

uint64_t TranspositionTable::Mix(uint64_t x) {
  // splitmix64 finalizer.
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

TranspositionTable::TranspositionTable(size_t sizeMb) {
  Resize(sizeMb);
}

TranspositionTable::~TranspositionTable() {
  delete[] entries_;
}

void TranspositionTable::Resize(size_t sizeMb) {
  delete[] entries_;
  entries_ = nullptr;
  count_ = 0;
  mask_ = 0;

  const size_t maxBytes = sizeMb * 1024 * 1024;
  if (maxBytes < sizeof(TtEntry)) return;
  size_t count = 1;
  while (count * 2 * sizeof(TtEntry) <= maxBytes) count *= 2;
  entries_ = new TtEntry[count];
  count_ = count;
  mask_ = count - 1;
  Clear();
}

void TranspositionTable::Clear() {
  if (entries_ == nullptr) return;
  std::memset(entries_, kTtEmpty, count_ * sizeof(TtEntry));
  probes = hits = stores = 0;
}

bool TranspositionTable::Probe(uint64_t p0, uint64_t p1, int player,
                                TtEntry& out) {
  ++probes;
  if (entries_ == nullptr) return false;
  const TtEntry& e =
      entries_[Mix(p0 ^ Mix(p1 ^ (0x1000ull + static_cast<uint64_t>(player)))) &
               mask_];
  if (e.key0 != p0 || e.key1 != p1 || e.player != static_cast<uint8_t>(player))
    return false;
  ++hits;
  out = e;
  return true;
}

void TranspositionTable::Store(uint64_t p0, uint64_t p1, int player, int depth,
                               int score, int flag, int move) {
  ++stores;
  if (entries_ == nullptr) return;
  TtEntry& e =
      entries_[Mix(p0 ^ Mix(p1 ^ (0x1000ull + static_cast<uint64_t>(player)))) &
               mask_];
  // Depth-preferred: keep the incumbent unless the slot is empty or the new
  // search is at least as deep (this also updates same-key entries, since a
  // re-search of the same position at the same remaining depth overwrites).
  if (e.player != kTtEmpty && depth < e.depth) return;
  e.key0 = p0;
  e.key1 = p1;
  e.score = static_cast<int16_t>(score);
  e.move = static_cast<int16_t>(move);
  e.depth = static_cast<int8_t>(depth);
  e.flag = static_cast<uint8_t>(flag);
  e.player = static_cast<uint8_t>(player);
}

}  // namespace bb
