// Benchmark for the bitboard solver (SortedAlphaBeta and friends, with
// the historical evaluation weights — the engine's default).
//
// Usage: ./bb_bench [depth] [repeats] [algo] [tt_mb]
//   algo: sort (default) | radix | tt
// The transposition table (algo "tt") is cleared before each run; tt_mb
// defaults to 64.

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bb_core.h"
#include "bb_tt.h"

int main(int argc, char** argv) {
  int depth = 3;
  if (argc > 1) depth = std::atoi(argv[1]);
  int repeats = 5;
  if (argc > 2) repeats = std::atoi(argv[2]);
  const char* algoName = (argc > 3) ? argv[3] : "sort";
  int ttMb = 64;
  if (argc > 4) ttMb = std::atoi(argv[4]);

  bb::Algo algo;
  if (std::strcmp(algoName, "sort") == 0)
    algo = bb::Algo::AB_Sort;
  else if (std::strcmp(algoName, "radix") == 0)
    algo = bb::Algo::AB_Radix;
  else if (std::strcmp(algoName, "tt") == 0)
    algo = bb::Algo::AB_TT;
  else {
    std::printf("unknown algo: %s\n", algoName);
    return 1;
  }

  bb::InitTables();
  const bb::Board classical = bb::ClassicalBoard();
  const bb::SearchConfig config(bb::EvalWeights::Default(), false, 1000);
  bb::TranspositionTable tt(static_cast<size_t>(ttMb));

  // Warm up (first run may cold-cache code paths).
  {
    bb::SearchContext ctx;
    ctx.config = &config;
    if (algo == bb::Algo::AB_TT) ctx.tt = &tt;
    bb::Board b = classical;
    bb::Move best;
    const int rootScore = bb::Eval(config.weights, b);
    switch (algo) {
      case bb::Algo::AB_Sort:
        bb::SortedAlphaBeta(best, rootScore, -32000, 32000, b, 0, depth, 0,
                            ctx);
        break;
      default:
        bb::TtAlphaBeta(best, rootScore, -32000, 32000, b, 0, depth, 0, ctx);
        break;
    }
  }

  double best_time = 1e9;
  double total_time = 0;
  for (int i = 0; i < repeats; ++i) {
    bb::SearchContext ctx;
    ctx.config = &config;
    if (algo == bb::Algo::AB_TT) {
      tt.Clear();
      ctx.tt = &tt;
    }
    bb::Board b = classical;
    const int rootScore = bb::Eval(config.weights, b);
    auto t0 = std::chrono::high_resolution_clock::now();
    bb::Move best;
    int score = 0;
    switch (algo) {
      case bb::Algo::AB_Sort:
        score = bb::SortedAlphaBeta(best, rootScore, -32000, 32000, b, 0,
                                    depth, 0, ctx);
        break;
      case bb::Algo::AB_Radix:
        score = bb::RadixSortedAlphaBeta(best, rootScore, -32000, 32000, b, 0,
                                         depth, 0, ctx);
        break;
      case bb::Algo::AB_TT:
        score = bb::TtAlphaBeta(best, rootScore, -32000, 32000, b, 0, depth, 0,
                               ctx);
        break;
      default:
        break;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    total_time += ms;
    if (ms < best_time) best_time = ms;
    if (algo == bb::Algo::AB_TT)
      std::printf(
          "  run %d: %.2f ms  (score=%d nodes=%" PRId64 " leaves=%" PRId64
          " tt: %" PRId64 "/%" PRId64 " hits)\n",
          i, ms, score, ctx.nodeCount, ctx.leafCount, tt.hits, tt.probes);
    else
      std::printf("  run %d: %.2f ms  (score=%d nodes=%" PRId64
                  " leaves=%" PRId64 ")\n",
                  i, ms, score, ctx.nodeCount, ctx.leafCount);
  }

  std::printf("\n");
  std::printf("depth=%d  repeats=%d  algo=%s\n", depth, repeats, algoName);
  std::printf("best  = %.2f ms\n", best_time);
  std::printf("avg   = %.2f ms\n", total_time / repeats);
  std::printf("total = %.2f ms\n", total_time);

  return 0;
}
