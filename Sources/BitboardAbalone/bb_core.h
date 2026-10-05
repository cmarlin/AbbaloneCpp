// Bitboard Abalone core — 2 x uint64 board representation.
//
// Port of the C# solver (Sources/AiAbbalone) to a dense 61-bit layout:
// one uint64 per player, one bit per cell. Functional parity with the C#
// solver: same move taxonomy (MoveId), same evaluation (ring weights +
// ball count), same dynamic move costs, same search algorithms
// (MiniMax, AlphaBeta, incremental AlphaBeta, sorted AlphaBeta with
// aspiration window + quiescence, radix-sorted AlphaBeta).
//
// Coordinate system (identical to Sources/SpielAbalone/abalone_core.h and
// the C# solver): 9x9 virtual grid, row 0 = 'a' (bottom), column 0 = '1'.
// Valid cells form the hexagon of radius 4 around (4,4): row r spans
// columns [max(0, r-4), min(8, r+4)]. Dense cell index 0..60 numbers rows
// a->i, left to right within each row.
//
// Direction numbering (identical everywhere): E=0, NE=1, NW=2, W=3, SW=4,
// SE=5. Sisters of direction d are (d+1)%6 and (d+2)%6 — the canonical
// broadside ordering of the ATP notation.

#ifndef BB_CORE_H_
#define BB_CORE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace bb {

class TranspositionTable;  // bb_tt.h

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr int kNumCells = 61;
constexpr int kDim = 9;
constexpr int kNumDirs = 6;
constexpr int kNumMoveIds = 11;  // 0 (None) .. 10

enum Dir : int { E = 0, NE = 1, NW = 2, W = 3, SW = 4, SE = 5 };

// Same taxonomy as the C# BigBitboard.MoveId. Single moves are inline
// pushes: the origin is the REAR of the chain, the chain extends in the
// move direction, own marbles first, then pushed opponent marbles.
enum MoveId : int {
  MoveNone = 0,
  Single_1 = 1,      // 1 own marble, empty destination
  Single_2 = 2,      // 2 own, empty destination
  Single_2VS1 = 3,   // 2 own push 1 opponent
  Single_3 = 4,      // 3 own, empty destination
  Single_3VS1 = 5,   // 3 own push 1 opponent
  Single_3VS2 = 6,   // 3 own push 2 opponents
  Slide_2S0 = 7,     // 2-marble broadside, sister side (d+1)%6
  Slide_2S1 = 8,     // 2-marble broadside, sister side (d+2)%6
  Slide_3S0 = 9,     // 3-marble broadside, sister side (d+1)%6
  Slide_3S1 = 10,    // 3-marble broadside, sister side (d+2)%6
};

// Chain length of a single move (own + pushed opponent marbles).
constexpr int MoveDepth(int id) {
  switch (id) {
    case Single_1: return 1;
    case Single_2: return 2;
    case Single_2VS1: return 3;
    case Single_3: return 3;
    case Single_3VS1: return 4;
    case Single_3VS2: return 5;
    default: return 0;
  }
}

// Own marbles in the chain of a single move.
constexpr int OwnDepth(int id) {
  switch (id) {
    case Single_1: return 1;
    case Single_2: return 2;
    case Single_2VS1: return 2;
    case Single_3: return 3;
    case Single_3VS1: return 3;
    case Single_3VS2: return 3;
    default: return 0;
  }
}

// Marbles moved by a slide.
constexpr int SlideCount(int id) {
  switch (id) {
    case Slide_2S0:
    case Slide_2S1: return 2;
    case Slide_3S0:
    case Slide_3S1: return 3;
    default: return 0;
  }
}

inline bool IsSlide(int id) { return id >= Slide_2S0; }
// Sister direction index (0 -> (d+1)%6, 1 -> (d+2)%6) of a slide move id.
inline int SisterIndex(int id) { return (id - Slide_2S0) & 1; }

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

struct Board {
  uint64_t p[2];  // p[0] = player 0 (white), p[1] = player 1 (black)
};

// ---------------------------------------------------------------------------
// Tables (initialised once by InitTables(); safe to call repeatedly)
// ---------------------------------------------------------------------------

extern uint64_t kValid;            // mask of the 61 valid cells
extern uint64_t kBit[kNumCells];    // 1 << index
extern int16_t kCellOf[kDim][kDim];  // (row, col) -> cell index, -1 invalid
extern int8_t kRowOf[kNumCells];      // cell -> row
extern int8_t kColOf[kNumCells];      // cell -> column
extern int8_t kDist[kNumCells];       // hex distance to center (0..4)
extern int16_t kNeighbor[kNumCells][kNumDirs];  // cell -> neighbor, -1 off

// Row masks for direction shifts: shifting the board by direction d is
//   for r in 0..8: out |= (b & kShiftMask[d][r]) << kShiftDelta[d][r]
// kShiftMask[d][r] selects the cells of row r that have a neighbor in
// direction d (empty mask for rows where no cell has one).
extern uint64_t kShiftMask[kNumDirs][kDim];
extern int kShiftDelta[kNumDirs][kDim];

// Collapsed form of the row masks: rows sharing the same delta are merged,
// so a shift needs one masked shift per distinct delta (1-3 per direction)
// instead of one per row. Only the first kShiftPairCount[d] entries are
// valid for direction d.
extern uint64_t kShiftPairMask[kNumDirs][kDim];
extern int kShiftPairDelta[kNumDirs][kDim];
extern int kShiftPairCount[kNumDirs];

// kPreValid[d][k-1] = P^k(kValid) for direction d, where
// P(x) = Shift(x, (d + 3) % kNumDirs) = {origins whose next cell in
// direction d exists and is in x}. Combined with the linearity of Shift
// this gives P^k(empty) = kPreValid[d][k-1] & ~P^k(occupied), so the
// empty pre-images need no shifts at all.
extern uint64_t kPreValid[kNumDirs][3];

// Shift a bitboard one step in direction d (bits without a neighbor in
// direction d are dropped).
inline uint64_t Shift(uint64_t b, int d) {
  uint64_t out = 0;
  const int n = kShiftPairCount[d];
  for (int i = 0; i < n; ++i) {
    const uint64_t m = b & kShiftPairMask[d][i];
    const int delta = kShiftPairDelta[d][i];
    out |= (delta >= 0) ? (m << delta) : (m >> -delta);
  }
  return out & kValid;
}

// Hex distance to center for any (row, col), even off-board cells.
// Off-board neighbors of valid cells always give a distance >= 5.
int Dist2(int row, int col);

void InitTables();

// Classical starting position (identical to abalone_core ABALONE_INIT_CLASSIC:
// player 0 rows a,b,c ; player 1 rows g,h,i).
Board ClassicalBoard();

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

struct Move {
  uint8_t dir;
  uint8_t id;    // MoveId
  uint8_t cell;  // origin (rear for singles, canonical end for slides)
  uint16_t key() const {
    return static_cast<uint16_t>(dir | (id << 3) | (cell << 7));
  }
};

// Precomputed executor for one geometrically valid move: from/to masks for
// the moving player and the opponent (an ejected opponent marble simply has
// no 'to' bit). Apply/rollback are two AND/OR pairs.
struct Executor {
  Move move;
  uint64_t fromCur, toCur;
  uint64_t fromOpp, toOpp;
};

// All geometrically valid moves, in the same enumeration order as the C#
// MoveItem.AllMoves() (dir, moveId, row, column). Index into executors().
const std::vector<Executor>& Executors();

// Executor index for (dir, id, cell), -1 if geometrically invalid.
int ExecutorIndex(int dir, int id, int cell);

// Raw (dir, id, cell) -> executor index table, filled by Executors().
extern int16_t gExecutorIndex[kNumDirs][kNumMoveIds][kNumCells];

// Unchecked table lookup — valid only once Executors() has run (it performs
// the initialisation). Use this in hot loops instead of ExecutorIndex.
inline int ExecutorIndexFast(int dir, int id, int cell) {
  return gExecutorIndex[dir][id][cell];
}

inline void ApplyMove(Board& b, const Executor& e, int player) {
  b.p[player] = (b.p[player] & ~e.fromCur) | e.toCur;
  b.p[1 - player] = (b.p[1 - player] & ~e.fromOpp) | e.toOpp;
}

inline void RollbackMove(Board& b, const Executor& e, int player) {
  b.p[player] = (b.p[player] & ~e.toCur) | e.fromCur;
  b.p[1 - player] = (b.p[1 - player] & ~e.toOpp) | e.fromOpp;
}

// Per-direction, per-moveId bitmasks of legal move origins for a player.
// Same content as the C# ComputeMoveList: bitmaps[dir][id] has a bit set
// at the origin cell of each legal move.
struct MoveList {
  uint64_t masks[kNumDirs][kNumMoveIds];
};

void ComputeMoveList(MoveList& out, const Board& b, int player);

// Number of legal moves (popcount of the masks).
int MoveCount(const MoveList& moves);

// ---------------------------------------------------------------------------
// Evaluation (exact port of the C# EvalWeights / Eval / DynamicEval)
// ---------------------------------------------------------------------------

struct EvalWeights {
  int bfactor = 61;
  int rfactor = 5;
  int ring[5] = {9, 7, 6, 4, 1};
  static EvalWeights Default() { return EvalWeights(); }
  static EvalWeights Tweak() {
    EvalWeights w;
    w.bfactor = 75;
    w.rfactor = 5;
    int ring[5] = {7, 7, 7, 5, 0};
    for (int i = 0; i < 5; ++i) w.ring[i] = ring[i];
    return w;
  }
};

// Static evaluation from player 0's perspective.
int Eval(const EvalWeights& w, const Board& b);

// Dynamic (incremental) cost of a move, from player 0's perspective, and
// whether it ejects an opponent marble. Exact port of the C# DynamicEval.
struct DynEval {
  int cost;
  bool catchBall;
};
DynEval DynamicEval(const EvalWeights& w, const Move& m);

// Search configuration: precomputed per-move costs and ranks (same
// construction as the C# SearchConfig). Costs are stored on 16 bits:
// the C# casts them to sbyte, which silently wraps for extreme moves
// (3-marble ejections reach ~275 with the Tweak weights) — this port does
// NOT replicate that overflow.
struct SearchConfig {
  EvalWeights weights;
  bool quiescent = false;
  int window = 1000;
  int quiescentMarker = 1000;
  // Aspiration half-width used by GenMoveId around the previous iteration
  // score, clamped to >= 1 (a degenerate alpha == beta window is unsound
  // here). Small values force the full-window re-search path.
  int aspiration = 150;
  std::vector<int16_t> costOfExecutor;  // cost per executor index
  std::vector<int16_t> rankOfExecutor;   // rank per executor index
  std::vector<int16_t> costFromRank;     // distinct costs, descending

  explicit SearchConfig(const EvalWeights& w, bool quiescent = false,
                        int window = 1000);
};

// ---------------------------------------------------------------------------
// Search (exact ports of the C# algorithms)
// ---------------------------------------------------------------------------

enum class Algo { MiniMax, AB_Rnd, AB_Sort, AB_Radix, AB_TT };

struct SearchContext {
  static constexpr int kMaxDepth = 64;
  static constexpr int kMaxMoves = 512;   // upper bound of legal move count
  static constexpr int kMaxRanks = 256;   // upper bound of distinct costs
  const SearchConfig* config = nullptr;
  // Optional transposition table (bb_tt.h). Null: the TT-aware algorithms
  // behave exactly like their plain counterparts.
  TranspositionTable* tt = nullptr;
  MoveList moves[kMaxDepth];
  // Radix scratch, per depth: move counts per rank and the sorted buffer.
  int32_t radixCounts[kMaxDepth][kMaxRanks];
  int32_t radixBuffer[kMaxDepth][kMaxMoves];
  long nodeCount = 0;
  long leafCount = 0;
  // Filled by GenMoveId: depth and score of the last completed iteration.
  int idDepth = 0;
  int idScore = 0;
};

int MiniMax(Move& best, Board& b, int depth, int player, SearchContext& ctx);
int AlphaBeta(Move& best, int alpha, int beta, Board& b, int depth, int player,
              SearchContext& ctx);
int AlphaBetaInc(Move& best, int eval, int alpha, int beta, Board& b, int depth,
                int player, SearchContext& ctx);
int SortedAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                    int depth, int finalDepth, int player, SearchContext& ctx);
int RadixSortedAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                         int depth, int finalDepth, int player,
                         SearchContext& ctx);
// RadixSortedAlphaBeta + transposition table (ctx.tt). Exact semantics: the
// radix search is a pure alpha-beta, so TT cutoffs never change the returned
// score, only the node count. With ctx.tt == null it degrades gracefully to
// the plain radix behaviour.
int TtAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                int depth, int finalDepth, int player, SearchContext& ctx);

// Top-level genmove: picks the algorithm from the config, mirrors the C#
// AiAbbalonePlayer.NextMove. Returns the executor index of the best move,
// or -1 if no legal move.
int GenMove(const Board& b, int player, const SearchConfig& config,
            Algo algo, int depth, SearchContext& ctx);

// Iterative deepening driver over TtAlphaBeta: searches depth 1..maxDepth
// with an aspiration window around the previous score (config.aspiration)
// and a full-window re-search on fail. budgetMs > 0 stops between
// iterations when the budget is spent or predicted to be exceeded by the
// next one — the budget is SOFT: a started iteration always runs to
// completion, so the wall time can overshoot by one iteration. With
// ctx.tt set the table persists across iterations (move hints + bounds).
// Returns the executor index of the best move (ctx.idDepth / ctx.idScore
// hold the last completed iteration), or -1 if there is no legal move.
int GenMoveId(const Board& b, int player, const SearchConfig& config,
              int maxDepth, int budgetMs, SearchContext& ctx);

// ---------------------------------------------------------------------------
// Perft (for cross-validation against the reference core)
// ---------------------------------------------------------------------------

long Perft(const Board& b, int depth, int player);

// ---------------------------------------------------------------------------
// ATP notation (canonical OpenSpiel/pyspiel strings)
// ---------------------------------------------------------------------------

// "a1b2" (inline, rear + rear + one offset) or "a1b2c3" (broadside,
// canonical ordering). Returns "" if the move is not representable.
std::string MoveToAtp(const Board& b, int player, const Move& m);

// Parse an ATP move in the context of a board/player, resolving the chain
// length / slide count from the board. Returns false if illegal.
bool TryParseAtp(const Board& b, int player, const std::string& s, Move& m);

// Cell name ("a1") helpers.
std::string CellName(int cell);
int CellFromName(const std::string& s);  // -1 if invalid

}  // namespace bb

#endif  // BB_CORE_H_
