// Search — exact ports of the C# AiAbbaloneTools algorithms:
// MiniMax, AlphaBeta (full eval), AlphaBeta (incremental eval),
// SortedAlphaBeta (move ordering + aspiration window + quiescence) and
// RadixSortedAlphaBeta (bucketed ordering).

#include "bb_core.h"
#include "bb_tt.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bb {

namespace {

// Fill `out` with the executor indices of all legal moves in `moves`,
// in enumeration order (dir, moveId, cell) — same as the C# loops.
int CollectMoves(const MoveList& moves, int* out) {
  Executors();  // one init check per node, not per move
  int n = 0;
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        int cell = __builtin_ctzll(m);
        m &= m - 1;
        out[n++] = ExecutorIndexFast(d, id, cell);
      }
    }
  }
  return n;
}

}  // namespace

int MiniMax(Move& best, Board& b, int depth, int player, SearchContext& ctx) {
  best = Move{};
  if (depth <= 0) {
    ctx.leafCount++;
    return Eval(ctx.config->weights, b);
  }
  ctx.nodeCount++;

  int currentScore = (player == 0) ? -100000 : 100000;
  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);
  const auto& executors = Executors();
  for (int i = 0; i < n; ++i) {
    const Executor& e = executors[list[i]];
    ApplyMove(b, e, player);
    Move child;
    int score = MiniMax(child, b, depth - 1, 1 - player, ctx);
    RollbackMove(b, e, player);
    if ((player == 0 && score > currentScore) ||
        (player != 0 && score < currentScore)) {
      currentScore = score;
      best = e.move;
    }
  }
  return currentScore;
}

int AlphaBeta(Move& best, int alpha, int beta, Board& b, int depth, int player,
              SearchContext& ctx) {
  best = Move{};
  if (depth <= 0) {
    ctx.leafCount++;
    return Eval(ctx.config->weights, b);
  }
  ctx.nodeCount++;

  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);
  const auto& executors = Executors();
  for (int i = 0; i < n; ++i) {
    const Executor& e = executors[list[i]];
    ApplyMove(b, e, player);
    Move child;
    int score = AlphaBeta(child, alpha, beta, b, depth - 1, 1 - player, ctx);
    RollbackMove(b, e, player);

    if (player == 0) {
      if (score > alpha) {
        alpha = score;
        best = e.move;
        if (alpha >= beta) return alpha;
      }
    } else if (score < beta) {
      beta = score;
      best = e.move;
      if (beta <= alpha) return beta;
    }
  }
  return (player == 0) ? alpha : beta;
}

int AlphaBetaInc(Move& best, int eval, int alpha, int beta, Board& b,
                 int depth, int player, SearchContext& ctx) {
  best = Move{};
  if (depth <= 0) {
    ctx.leafCount++;
    return eval;
  }
  ctx.nodeCount++;

  const SearchConfig& config = *ctx.config;
  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);
  const auto& executors = Executors();
  for (int i = 0; i < n; ++i) {
    const int idx = list[i];
    const Executor& e = executors[idx];
    const int staticEval =
        config.costOfExecutor[idx] * ((player == 0) ? 1 : -1);
    ApplyMove(b, e, player);
    Move child;
    int score =
        AlphaBetaInc(child, eval + staticEval, alpha, beta, b, depth - 1,
                     1 - player, ctx);
    RollbackMove(b, e, player);

    if (player == 0) {
      if (score > alpha) {
        alpha = score;
        best = e.move;
        if (alpha >= beta) return alpha;
      }
    } else if (score < beta) {
      beta = score;
      best = e.move;
      if (beta <= alpha) return beta;
    }
  }
  return (player == 0) ? alpha : beta;
}

namespace {

// Private variant of the C# SortedAlphaBeta: quiescent extension (q = 2)
// and per-node aspiration window cuts. Iterates the moves in descending
// cost order (the arrays are sorted ascending, walked backwards).
int SortedAlphaBetaRec(int rootEval, int eval, int alpha, int beta, Board& b,
                       int depth, int finalDepth, int player,
                       SearchContext& ctx) {
  ctx.nodeCount++;
  const SearchConfig& config = *ctx.config;
  constexpr int q = 2;

  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);

  // Counting sort by precomputed rank (rank 0 = highest cost), O(n):
  // buckets are filled from their end while scanning moves in enumeration
  // order, so walking ranks forward reproduces exactly the previous
  // packed-key order (cost descending, ties by descending enumeration).
  const int numRanks = static_cast<int>(config.costFromRank.size());
  int* counts = ctx.radixCounts[depth];
  std::memset(counts, 0, sizeof(int) * numRanks);
  int bestRank = numRanks;
  for (int i = 0; i < n; ++i) {
    const int r = config.rankOfExecutor[list[i]];
    ++counts[r];
    if (r < bestRank) bestRank = r;
  }
  const int bestCandidateValue =
      (n > 0) ? config.costFromRank[bestRank] : -1000;
  int offsets[SearchContext::kMaxRanks];
  int total = 0;
  for (int r = 0; r < numRanks; ++r) {
    offsets[r] = total;
    total += counts[r];
  }
  int* buffer = ctx.radixBuffer[depth];
  int cursor[SearchContext::kMaxRanks];
  for (int r = 0; r < numRanks; ++r) cursor[r] = offsets[r] + counts[r];
  for (int i = 0; i < n; ++i)
    buffer[--cursor[config.rankOfExecutor[list[i]]]] = list[i];

  const int quiescentMarker = config.quiescentMarker;
  const int recDepth = depth + 1;
  const int playerIdFactor = (player == 0) ? 1 : -1;
  const auto& executors = Executors();

  if (recDepth < finalDepth ||
      (bestCandidateValue >= quiescentMarker && recDepth < finalDepth + q)) {
    const int windowSize = (finalDepth - depth) * config.window;
    for (int r = 0; r < numRanks; ++r) {
      if (counts[r] == 0) continue;
      const int rawScore = config.costFromRank[r];
      for (int j = offsets[r]; j < offsets[r] + counts[r]; ++j) {
        const int idx = buffer[j];
        int score = eval + rawScore * playerIdFactor;
        if (recDepth < finalDepth ||
            (rawScore >= quiescentMarker && recDepth < finalDepth + q)) {
          if (player == 0) {
            if (score >= rootEval + windowSize) return score;
          } else {
            if (score <= rootEval - windowSize) return score;
          }
          const Executor& e = executors[idx];
          ApplyMove(b, e, player);
          score = SortedAlphaBetaRec(rootEval, score, alpha, beta, b, recDepth,
                                     finalDepth, 1 - player, ctx);
          RollbackMove(b, e, player);
        } else {
          ctx.leafCount++;
        }

        if (player == 0) {
          if (score > alpha) {
            alpha = score;
            if (alpha >= beta) return alpha;
          }
        } else if (score < beta) {
          beta = score;
          if (beta <= alpha) return beta;
        }
      }
    }
  } else {
    ctx.leafCount += n;
    return eval + bestCandidateValue * playerIdFactor;
  }
  return (player == 0) ? alpha : beta;
}

}  // namespace

int SortedAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                    int depth, int finalDepth, int player, SearchContext& ctx) {
  best = Move{};
  ctx.nodeCount++;
  const SearchConfig& config = *ctx.config;

  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);

  // Counting sort by precomputed rank (rank 0 = highest cost), O(n):
  // buckets are filled from their start while scanning moves in
  // enumeration order, so walking ranks forward reproduces exactly the
  // previous stable descending order (ties by ascending enumeration).
  const int numRanks = static_cast<int>(config.costFromRank.size());
  int* counts = ctx.radixCounts[depth];
  std::memset(counts, 0, sizeof(int) * numRanks);
  for (int i = 0; i < n; ++i) ++counts[config.rankOfExecutor[list[i]]];
  int offsets[SearchContext::kMaxRanks];
  int total = 0;
  for (int r = 0; r < numRanks; ++r) {
    offsets[r] = total;
    total += counts[r];
  }
  int* buffer = ctx.radixBuffer[depth];
  int cursor[SearchContext::kMaxRanks];
  std::memcpy(cursor, offsets, sizeof(int) * numRanks);
  for (int i = 0; i < n; ++i)
    buffer[cursor[config.rankOfExecutor[list[i]]]++] = list[i];

  const int recDepth = depth + 1;
  const int windowSize = (finalDepth - depth) * config.window;
  alpha = std::max(alpha, eval - windowSize);
  beta = std::min(beta, eval + windowSize);

  const auto& executors = Executors();
  for (int r = 0; r < numRanks; ++r) {
    for (int j = offsets[r]; j < offsets[r] + counts[r]; ++j) {
      const int idx = buffer[j];
      const Executor& e = executors[idx];
      int score = eval + config.costFromRank[r] * ((player == 0) ? 1 : -1);
      if (recDepth < finalDepth) {
        ApplyMove(b, e, player);
        score = SortedAlphaBetaRec(eval, score, alpha, beta, b, recDepth,
                                   finalDepth, 1 - player, ctx);
        RollbackMove(b, e, player);
      }

      if (player == 0) {
        if (score > alpha) {
          alpha = score;
          best = e.move;
          if (alpha >= beta) return alpha;
        }
      } else if (score < beta) {
        beta = score;
        best = e.move;
        if (beta <= alpha) return beta;
      }
    }
  }
  return (player == 0) ? alpha : beta;
}

int RadixSortedAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                         int depth, int finalDepth, int player,
                         SearchContext& ctx) {
  best = Move{};
  if (depth >= finalDepth) {
    ctx.leafCount++;
    return eval;
  }
  ctx.nodeCount++;

  const SearchConfig& config = *ctx.config;
  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);

  // Counting sort of the moves into per-rank buckets (rank 0 = highest
  // cost), then iterate the buckets in rank order — same behaviour as the
  // C# SortedLists counters.
  const int numRanks = static_cast<int>(config.costFromRank.size());
  int* counts = ctx.radixCounts[depth];
  std::memset(counts, 0, sizeof(int) * SearchContext::kMaxRanks);
  for (int i = 0; i < n; ++i) ++counts[config.rankOfExecutor[list[i]]];
  int offsets[SearchContext::kMaxRanks];
  int total = 0;
  for (int r = 0; r < numRanks; ++r) {
    offsets[r] = total;
    total += counts[r];
  }
  int* buffer = ctx.radixBuffer[depth];
  int cursor[SearchContext::kMaxRanks];
  std::memcpy(cursor, offsets, sizeof(int) * numRanks);
  for (int i = 0; i < n; ++i) {
    int r = config.rankOfExecutor[list[i]];
    buffer[cursor[r]++] = list[i];
  }

  const auto& executors = Executors();
  for (int r = 0; r < numRanks; ++r) {
    const int moveCost = config.costFromRank[r] * ((player == 0) ? 1 : -1);
    for (int i = offsets[r]; i < offsets[r] + counts[r]; ++i) {
      const Executor& e = executors[buffer[i]];
      ApplyMove(b, e, player);
      Move child;
      int score = RadixSortedAlphaBeta(child, eval + moveCost, alpha, beta, b,
                                       depth + 1, finalDepth, 1 - player, ctx);
      RollbackMove(b, e, player);

      if (player == 0) {
        if (score > alpha) {
          alpha = score;
          best = e.move;
          if (alpha >= beta) return alpha;
        }
      } else if (score < beta) {
        beta = score;
        best = e.move;
        if (beta <= alpha) return beta;
      }
    }
  }
  return (player == 0) ? alpha : beta;
}

int TtAlphaBeta(Move& best, int eval, int alpha, int beta, Board& b,
                int depth, int finalDepth, int player, SearchContext& ctx) {
  best = Move{};
  if (depth >= finalDepth) {
    ctx.leafCount++;
    return eval;
  }
  ctx.nodeCount++;
  const SearchConfig& config = *ctx.config;
  TranspositionTable* tt = ctx.tt;

  // Probe. The root (depth == 0) never takes a score cutoff so that a best
  // move is always produced for the caller; it can still use the TT move.
  int ttMove = -1;
  if (tt != nullptr) {
    TtEntry e;
    if (tt->Probe(b.p[0], b.p[1], player, e)) {
      ttMove = e.move;
      if (depth > 0 && e.depth >= finalDepth - depth) {
        if (e.flag == kTtExact) return e.score;
        if (e.flag == kTtLower && e.score >= beta) return e.score;
        if (e.flag == kTtUpper && e.score <= alpha) return e.score;
      }
    }
  }

  ComputeMoveList(ctx.moves[depth], b, player);
  int list[SearchContext::kMaxMoves];
  int n = CollectMoves(ctx.moves[depth], list);

  // Counting sort of the moves into per-rank buckets (rank 0 = highest
  // cost), then iterate the buckets in rank order — same as the radix
  // search.
  const int numRanks = static_cast<int>(config.costFromRank.size());
  int* counts = ctx.radixCounts[depth];
  std::memset(counts, 0, sizeof(int) * SearchContext::kMaxRanks);
  for (int i = 0; i < n; ++i) ++counts[config.rankOfExecutor[list[i]]];
  int offsets[SearchContext::kMaxRanks];
  int total = 0;
  for (int r = 0; r < numRanks; ++r) {
    offsets[r] = total;
    total += counts[r];
  }
  int* buffer = ctx.radixBuffer[depth];
  int cursor[SearchContext::kMaxRanks];
  std::memcpy(cursor, offsets, sizeof(int) * numRanks);
  for (int i = 0; i < n; ++i) {
    int r = config.rankOfExecutor[list[i]];
    buffer[cursor[r]++] = list[i];
  }

  // TT move hint: the entry key-matches this exact position and player,
  // so the stored move is legal here. It is searched first, before the
  // buckets — with the cost of its OWN rank, because the bucket loop
  // associates costs by bucket index (swapping the hint into buffer[0]
  // would silently re-cost it and corrupt the incremental eval).
  int hintPos = -1;
  if (ttMove >= 0) {
    for (int i = 0; i < n; ++i) {
      if (buffer[i] == ttMove) {
        hintPos = i;
        break;
      }
    }
  }

  // Store the node value on every exit. Flag semantics (value V of the
  // node, window [alphaOrig, betaOrig], scores from player 0's view):
  //   max node (player 0): v >= betaOrig -> V >= v (lower bound);
  //                        v <= alphaOrig -> V <= v (upper bound); else exact.
  //   min node (player 1): v <= alphaOrig -> V <= v (upper bound);
  //                        v >= betaOrig -> V >= v (lower bound); else exact.
  // Nodes with no legal move are not stored: their returned window value is
  // not a bound on anything.
  const int alphaOrig = alpha;
  const int betaOrig = beta;
  int bestIdx = -1;
  auto storeTT = [&]() {
    if (tt == nullptr || n == 0) return;
    const int v = (player == 0) ? alpha : beta;
    int flag;
    if (player == 0)
      flag = (v >= betaOrig) ? kTtLower
                             : ((v <= alphaOrig) ? kTtUpper : kTtExact);
    else
      flag = (v <= alphaOrig) ? kTtUpper
                             : ((v >= betaOrig) ? kTtLower : kTtExact);
    tt->Store(b.p[0], b.p[1], player, finalDepth - depth, v, flag, bestIdx);
  };

  const auto& executors = Executors();
  if (hintPos >= 0) {
    const Executor& e = executors[ttMove];
    const int hintCost =
        config.costFromRank[config.rankOfExecutor[ttMove]] *
        ((player == 0) ? 1 : -1);
    ApplyMove(b, e, player);
    Move child;
    int score = TtAlphaBeta(child, eval + hintCost, alpha, beta, b, depth + 1,
                            finalDepth, 1 - player, ctx);
    RollbackMove(b, e, player);
    if (player == 0) {
      if (score > alpha) {
        alpha = score;
        best = e.move;
        bestIdx = ttMove;
        if (alpha >= beta) {
          storeTT();
          return alpha;
        }
      }
    } else if (score < beta) {
      beta = score;
      best = e.move;
      bestIdx = ttMove;
      if (beta <= alpha) {
        storeTT();
        return beta;
      }
    }
  }
  for (int r = 0; r < numRanks; ++r) {
    const int moveCost = config.costFromRank[r] * ((player == 0) ? 1 : -1);
    for (int i = offsets[r]; i < offsets[r] + counts[r]; ++i) {
      if (i == hintPos) continue;  // TT move already searched
      const Executor& e = executors[buffer[i]];
      ApplyMove(b, e, player);
      Move child;
      int score = TtAlphaBeta(child, eval + moveCost, alpha, beta, b,
                              depth + 1, finalDepth, 1 - player, ctx);
      RollbackMove(b, e, player);

      if (player == 0) {
        if (score > alpha) {
          alpha = score;
          best = e.move;
          bestIdx = buffer[i];
          if (alpha >= beta) {
            storeTT();
            return alpha;
          }
        }
      } else if (score < beta) {
        beta = score;
        best = e.move;
        bestIdx = buffer[i];
        if (beta <= alpha) {
          storeTT();
          return beta;
        }
      }
    }
  }
  storeTT();
  return (player == 0) ? alpha : beta;
}

int GenMove(const Board& bIn, int player, const SearchConfig& config,
            Algo algo, int depth, SearchContext& ctx) {
  ctx.config = &config;
  Board b = bIn;
  Move best;
  const int rootScore = Eval(config.weights, b);
  const int alpha = -32000;
  const int beta = 32000;

  switch (algo) {
    case Algo::MiniMax:
      MiniMax(best, b, depth, player, ctx);
      break;
    case Algo::AB_Rnd:
      AlphaBetaInc(best, rootScore, alpha, beta, b, depth, player, ctx);
      break;
    case Algo::AB_Sort:
      SortedAlphaBeta(best, rootScore, alpha, beta, b, 0, depth, player, ctx);
      break;
    case Algo::AB_Radix:
      RadixSortedAlphaBeta(best, rootScore, alpha, beta, b, 0, depth, player,
                           ctx);
      break;
    case Algo::AB_TT:
      TtAlphaBeta(best, rootScore, alpha, beta, b, 0, depth, player, ctx);
      break;
  }
  if (best.id == MoveNone) return -1;
  return ExecutorIndex(best.dir, best.id, best.cell);
}

int GenMoveId(const Board& bIn, int player, const SearchConfig& config,
              int maxDepth, int budgetMs, SearchContext& ctx) {
  ctx.idDepth = 0;
  ctx.idScore = 0;
  if (maxDepth < 1) return -1;
  ctx.config = &config;
  Board b = bIn;
  const int rootEval = Eval(config.weights, b);

  const auto start = std::chrono::steady_clock::now();
  auto elapsedMs = [&]() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
  };

  Move best;
  int bestIdx = -1;
  double lastIterMs = 0.0;
  int prevScore = rootEval;

  for (int depth = 1; depth <= maxDepth; ++depth) {
    if (budgetMs > 0 && depth > 1) {
      const double now = elapsedMs();
      // Soft budget, checked between iterations only: stop when spent, or
      // when the next iteration — predicted from the last iteration's
      // cost, which grows roughly 6x per ply — would not fit.
      if (now >= budgetMs) break;
      if (lastIterMs > 0.0 && now + 6.0 * lastIterMs >= budgetMs) break;
    }

    const double t0 = elapsedMs();
    int alpha = -32000;
    int beta = 32000;
    if (depth > 1) {
      // Clamp to a strictly positive half-width: a degenerate alpha == beta
      // window is unsound with this search's bound semantics (a fail-low
      // return equals alpha, which the parent then misreads as >= beta and
      // stores as a false lower bound).
      const int half = config.aspiration > 0 ? config.aspiration : 1;
      alpha = prevScore - half;
      beta = prevScore + half;
    }
    int score =
        TtAlphaBeta(best, rootEval, alpha, beta, b, 0, depth, player, ctx);
    if (depth > 1 && (score <= alpha || score >= beta)) {
      // Aspiration fail. The narrow search stored sound bounds in the TT;
      // the full-window re-search returns the exact value. A fail low
      // leaves `best` empty, the re-search fills it back.
      score = TtAlphaBeta(best, rootEval, -32000, 32000, b, 0, depth, player,
                          ctx);
    }
    lastIterMs = elapsedMs() - t0;

    if (best.id == MoveNone) break;  // no legal move at all
    bestIdx = ExecutorIndex(best.dir, best.id, best.cell);
    ctx.idDepth = depth;
    ctx.idScore = score;
    prevScore = score;
  }
  return bestIdx;
}

}  // namespace bb
