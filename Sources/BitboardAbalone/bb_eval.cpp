// Evaluation — exact port of the C# AiAbbaloneTools.Eval / DynamicEval /
// SearchConfig cost construction.

#include "bb_core.h"

#include <algorithm>
#include <map>
#include <set>

namespace bb {

int Eval(const EvalWeights& w, const Board& b) {
  InitTables();
  int score[2] = {0, 0};
  int balls[2] = {0, 0};
  for (int p = 0; p < 2; ++p) {
    uint64_t m = b.p[p];
    while (m) {
      int i = __builtin_ctzll(m);
      m &= m - 1;
      balls[p]++;
      score[p] += w.ring[kDist[i]];
    }
  }
  return (score[0] - score[1]) * w.rfactor + (balls[0] - balls[1]) * w.bfactor;
}

namespace {

// Offset (dr, dc) of a direction.
void DirOffset(int dir, int& dr, int& dc) {
  static const int kOffsets[kNumDirs][2] = {
      {0, 1}, {1, 1}, {1, 0}, {0, -1}, {-1, -1}, {-1, 0},
  };
  dr = kOffsets[dir][0];
  dc = kOffsets[dir][1];
}

// Port of the C# DynamicEvalSingle: ring-weight deltas along the chain,
// ejection (new distance >= 5) costs BFactor and marks catchBall.
DynEval DynamicEvalSingle(const EvalWeights& w, const Move& m) {
  const int len = MoveDepth(m.id);
  const int own = OwnDepth(m.id);
  // boardLine bit j = 1 when chain cell (len-1-j) belongs to the opponent
  // (front cells are the pushed ones) — same encoding as the C#.
  const int boardLine = (1 << (len - own)) - 1;

  int dr, dc;
  DirOffset(m.dir, dr, dc);

  int scoreCount[2] = {0, 0};
  bool catchBall = false;
  int depth = len;
  int row = kRowOf[m.cell];
  int col = kColOf[m.cell];
  while (depth != 0) {
    depth--;
    const int player = (boardLine >> depth) & 0x1;
    const int dist = Dist2(row, col);
    const int newDist = Dist2(row + dr, col + dc);
    scoreCount[player] -= w.ring[dist] * w.rfactor;
    if (newDist < 5) {
      scoreCount[player] += w.ring[newDist] * w.rfactor;
    } else {
      scoreCount[player] -= w.bfactor;
      catchBall = true;
    }
    row += dr;
    col += dc;
  }
  return {scoreCount[0] - scoreCount[1], catchBall};
}

// Port of the C# DynamicEvalSlide: all moved marbles belong to the mover,
// no ejection possible.
int DynamicEvalSlide(const EvalWeights& w, const Move& m) {
  const int count = SlideCount(m.id);
  const int side = SisterIndex(m.id);
  const int s = (m.dir + 1 + side) % kNumDirs;
  int dr, dc, sr, sc;
  DirOffset(m.dir, dr, dc);
  DirOffset(s, sr, sc);

  int scoreCount[2] = {0, 0};
  int row = kRowOf[m.cell];
  int col = kColOf[m.cell];
  for (int i = 0; i < count; ++i) {
    const int dist = Dist2(row, col);
    const int newDist = Dist2(row + dr, col + dc);
    // Raw ring deltas here; the RFactor is applied once at the end (same
    // as the C# DynamicEvalSlide).
    scoreCount[0] -= w.ring[dist];
    scoreCount[0] += w.ring[newDist];
    row += sr;
    col += sc;
  }
  return (scoreCount[0] - scoreCount[1]) * w.rfactor;
}

}  // namespace

DynEval DynamicEval(const EvalWeights& w, const Move& m) {
  if (!IsSlide(m.id)) return DynamicEvalSingle(w, m);
  return {DynamicEvalSlide(w, m), false};
}

SearchConfig::SearchConfig(const EvalWeights& w, bool quiescent, int window)
    : weights(w), quiescent(quiescent), window(window) {
  const auto& executors = Executors();
  costOfExecutor.assign(executors.size(), 0);
  rankOfExecutor.assign(executors.size(), 0);

  // Distinct costs, descending (same as the C# costList).
  std::map<int, int> catchCount;  // cost -> number of ejection moves
  for (size_t i = 0; i < executors.size(); ++i) {
    DynEval de = DynamicEval(weights, executors[i].move);
    costOfExecutor[i] = static_cast<int16_t>(de.cost);
    if (de.catchBall) catchCount[de.cost]++;
  }
  std::set<int> costs(costOfExecutor.begin(), costOfExecutor.end());
  for (auto it = costs.rbegin(); it != costs.rend(); ++it)
    costFromRank.push_back(static_cast<int16_t>(*it));
  for (size_t i = 0; i < executors.size(); ++i) {
    rankOfExecutor[i] = static_cast<int16_t>(
        std::find(costFromRank.begin(), costFromRank.end(),
                  costOfExecutor[i]) - costFromRank.begin());
  }

  // Quiescent marker: minimum cost among ejection moves (1000 = disabled).
  quiescentMarker = 1000;
  if (quiescent) {
    for (const auto& kv : catchCount) {
      if (kv.second > 0) {
        quiescentMarker = kv.first;  // minimum of the (sorted) map keys
        break;
      }
    }
  }
}

}  // namespace bb
