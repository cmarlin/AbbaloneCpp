// Cell naming ("a1".."i9") for the ATP notation. Row 0 = 'a', column 0 =
// '1'. Move-level ATP IO (MoveToAtp / TryParseAtp) is added in a later
// phase and will live in this file.

#include "bb_core.h"

#include <cctype>
#include <sstream>

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

bool ParseBoardText(const std::string& text, Board* out, std::string* error) {
  InitTables();
  Board b{{0, 0}};
  bool seen[kDim] = {};
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream tokens(line);
    std::string label;
    if (!(tokens >> label) || label.size() != 3 || label[0] != '<' ||
        label[2] != '>' || label[1] < 'a' || label[1] > 'i')
      continue;
    const int row = label[1] - 'a';
    if (seen[row]) {
      *error = "row " + label + " given twice";
      return false;
    }
    seen[row] = true;
    // the row's cells, left to right, start at column max(0, row - 4)
    int col = row > 4 ? row - 4 : 0;
    std::string token;
    while (tokens >> token && token[0] != '<') {
      if (token != "." && token != "1" && token != "2") {
        *error = "unexpected token '" + token + "' in row " + label;
        return false;
      }
      const int cell = col < kDim ? kCellOf[row][col] : -1;
      if (cell < 0) {
        *error = "too many cells in row " + label;
        return false;
      }
      if (token != ".") b.p[token[0] - '1'] |= kBit[cell];
      ++col;
    }
    if (col < kDim && kCellOf[row][col] >= 0) {
      *error = "missing cells in row " + label;
      return false;
    }
  }
  for (int row = 0; row < kDim; ++row)
    if (!seen[row]) {
      *error = std::string("row <") + static_cast<char>('a' + row) + "> missing";
      return false;
    }
  *out = b;
  return true;
}

Board BelgianDaisyBoard() {
  // ABALONE_INIT_BELGIAN_DAISY, as OpenSpiel displays it
  static const char kLayout[] =
      "<i>           2   2   .   1   1\n"
      "<h>         2   2   2   1   1   1\n"
      "<g>       .   2   2   .   1   1   .\n"
      "<f>     .   .   .   .   .   .   .   .\n"
      "<e>   .   .   .   .   .   .   .   .   .\n"
      "<d>     .   .   .   .   .   .   .   .\n"
      "<c>       .   1   1   .   2   2   .\n"
      "<b>         1   1   1   2   2   2\n"
      "<a>           1   1   .   2   2\n";
  Board b;
  std::string error;
  ParseBoardText(kLayout, &b, &error);
  return b;
}

bool LayoutBoard(const std::string& name, Board* out) {
  if (name == "classical" || name == "classic") {
    *out = ClassicalBoard();
    return true;
  }
  if (name == "belgian-daisy") {
    *out = BelgianDaisyBoard();
    return true;
  }
  return false;
}

}  // namespace bb
