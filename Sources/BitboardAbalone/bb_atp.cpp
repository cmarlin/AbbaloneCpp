// Cell naming ("a1".."i9") for the ATP notation. Row 0 = 'a', column 0 =
// '1'. Move-level ATP IO (MoveToAtp / TryParseAtp) is added in a later
// phase and will live in this file.

#include "bb_core.h"

#include <cctype>

namespace bb {

std::string CellName(int cell) {
  InitTables();
  if (cell < 0 || cell >= kNumCells) return "";
  std::string s;
  s += static_cast<char>('a' + kRowOf[cell]);
  s += static_cast<char>('1' + kColOf[cell]);
  return s;
}

int CellFromName(const std::string& s) {
  InitTables();
  if (s.size() != 2) return -1;
  char row = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
  char col = s[1];
  if (row < 'a' || row > 'i' || col < '1' || col > '9') return -1;
  return kCellOf[row - 'a'][col - '1'];
}

std::string MoveToAtp(const Board& b, int player, const Move& m) {
  InitTables();
  if (ExecutorIndex(m.dir, m.id, m.cell) < 0) return "";
  MoveList moves;
  ComputeMoveList(moves, b, player);
  if (!((moves.masks[m.dir][m.id] >> m.cell) & 1)) return "";

  // Inline: rear cell + rear + one step in the move direction.
  // Slide: origin + far end of the marble line + origin + one step.
  // Both are the canonical OpenSpiel forms (the sister direction is
  // always (dir+1)%6 or (dir+2)%6, so no start/end swap is needed).
  std::string s = CellName(m.cell);
  if (!IsSlide(m.id)) return s + CellName(kNeighbor[m.cell][m.dir]);
  const int sister = (m.dir + 1 + SisterIndex(m.id)) % kNumDirs;
  int end = m.cell;
  for (int i = 1; i < SlideCount(m.id); ++i) end = kNeighbor[end][sister];
  return s + CellName(end) + CellName(kNeighbor[m.cell][m.dir]);
}

bool TryParseAtp(const Board& b, int player, const std::string& str,
                 Move& m) {
  InitTables();
  m = Move{};
  if (str.size() != 4 && str.size() != 6) return false;
  std::string s = str;
  for (char& ch : s) ch = static_cast<char>(std::tolower(ch));
  const int c0 = CellFromName(s.substr(0, 2));
  const int c1 = CellFromName(s.substr(2, 2));
  if (c0 < 0 || c1 < 0) return false;
  MoveList moves;
  ComputeMoveList(moves, b, player);

  if (s.size() == 4) {
    // Inline move: c1 = c0 + one step in the move direction; the move id
    // follows from the chain content.
    for (int d = 0; d < kNumDirs; ++d) {
      if (kNeighbor[c0][d] != c1) continue;
      for (int id = Single_1; id <= Single_3VS2; ++id) {
        if ((moves.masks[d][id] >> c0) & 1) {
          m.dir = static_cast<uint8_t>(d);
          m.id = static_cast<uint8_t>(id);
          m.cell = static_cast<uint8_t>(c0);
          return true;
        }
      }
      return false;
    }
    return false;
  }

  // Slide move: c2 = c0 + one step in the move direction, the marbles run
  // from c0 to c1 (or c1 to c0 in the non-canonical swapped form).
  const int c2 = CellFromName(s.substr(4, 2));
  if (c2 < 0) return false;
  int dir = -1;
  for (int d = 0; d < kNumDirs; ++d)
    if (kNeighbor[c0][d] == c2) dir = d;
  if (dir < 0) return false;
  for (int side = 0; side < 2; ++side) {
    const int sister = (dir + 1 + side) % kNumDirs;
    for (int count = 2; count <= 3; ++count) {
      const int id = (count == 2) ? Slide_2S0 + side : Slide_3S0 + side;
      int e = c0;
      int e2 = c1;
      for (int i = 1; i < count; ++i) {
        if (e >= 0) e = kNeighbor[e][sister];
        if (e2 >= 0) e2 = kNeighbor[e2][sister];
      }
      if (e == c1 && ((moves.masks[dir][id] >> c0) & 1)) {
        m.dir = static_cast<uint8_t>(dir);
        m.id = static_cast<uint8_t>(id);
        m.cell = static_cast<uint8_t>(c0);
        return true;
      }
      if (e2 == c0 && ((moves.masks[dir][id] >> c1) & 1)) {
        m.dir = static_cast<uint8_t>(dir);
        m.id = static_cast<uint8_t>(id);
        m.cell = static_cast<uint8_t>(c1);
        return true;
      }
    }
  }
  return false;
}

}  // namespace bb
