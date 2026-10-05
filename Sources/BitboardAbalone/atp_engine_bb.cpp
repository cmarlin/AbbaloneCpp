// ATP (Abalone Text Protocol) engine built on the bitboard core — the C++
// port of the C# solver. See Tournament/ATP.md.
//
// Usage: atp_engine_bb [--depth N] [--window N] [--quiescent]
//                      [--algo minimax|abrnd|absort|abradix|abtt]
//                      [--budget N] [--tt-mb N]
//
// Defaults: historical weights ({9,7,6,4,1}, BFactor 61 — the C# EvalWeights
// .Default, which dominate Tweak in head-to-head matches), depth 3,
// window 1000, quiescence off. Only the classical board is supported.
//
// --algo abtt: fixed-depth search with the transposition table (the table
//   persists across genmove calls within a game, cleared on board /
//   clear_board).
// --budget N: iterative deepening (GenMoveId) with N ms per move; takes
//   precedence over --algo, and --depth becomes a depth cap (default 32
//   when not given explicitly). The budget is soft: a started iteration
//   always completes, so a move can overshoot by one iteration.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "bb_core.h"
#include "bb_tt.h"

namespace {

int g_depth = 3;
bool g_depthSet = false;
int g_window = 1000;
bool g_quiescent = false;
int g_budgetMs = 0;
int g_ttMb = 64;
bb::Algo g_algo = bb::Algo::AB_Sort;
bb::TranspositionTable* g_tt = nullptr;

bb::Board g_board;
int g_player = 0;  // white = player 0, plays first

bool ParseLine(const std::string& line, std::string* command,
               std::vector<std::string>* args) {
  command->clear();
  args->clear();
  size_t i = 0;
  while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) ++i;
  while (i < line.size() && !isspace(static_cast<unsigned char>(line[i])))
    command->push_back(line[i++]);
  while (i < line.size()) {
    while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) ++i;
    size_t start = i;
    while (i < line.size() && !isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i > start) args->push_back(line.substr(start, i - start));
  }
  return !command->empty();
}

bool ColorToPlayer(const std::string& color, int* player) {
  if (color == "white") { *player = 0; return true; }
  if (color == "black") { *player = 1; return true; }
  return false;
}

bool ParseAlgo(const std::string& name) {
  if (name == "minimax") { g_algo = bb::Algo::MiniMax; return true; }
  if (name == "abrnd") { g_algo = bb::Algo::AB_Rnd; return true; }
  if (name == "absort") { g_algo = bb::Algo::AB_Sort; return true; }
  if (name == "abradix") { g_algo = bb::Algo::AB_Radix; return true; }
  if (name == "abtt") { g_algo = bb::Algo::AB_TT; return true; }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
      g_depth = std::atoi(argv[++i]);
      g_depthSet = true;
    } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
      g_window = std::atoi(argv[++i]);
    } else if (strcmp(argv[i], "--quiescent") == 0) {
      g_quiescent = true;
    } else if (strcmp(argv[i], "--algo") == 0 && i + 1 < argc) {
      if (!ParseAlgo(argv[++i])) {
        std::fprintf(stderr, "unknown algo: %s\n", argv[i]);
        return 2;
      }
    } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
      g_budgetMs = std::atoi(argv[++i]);
    } else if (strcmp(argv[i], "--tt-mb") == 0 && i + 1 < argc) {
      g_ttMb = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  bb::InitTables();
  g_board = bb::ClassicalBoard();
  const bb::EvalWeights weights = bb::EvalWeights::Default();
  const bb::SearchConfig config(weights, g_quiescent, g_window);
  if (g_algo == bb::Algo::AB_TT || g_budgetMs > 0)
    g_tt = new bb::TranspositionTable(static_cast<size_t>(g_ttMb));
  // Depth cap for the budget mode when --depth was not given explicitly.
  const int idMaxDepth = g_depthSet ? g_depth : 32;

  std::string line;
  while (std::getline(std::cin, line)) {
    std::string command;
    std::vector<std::string> args;
    if (!ParseLine(line, &command, &args))
      continue;
    bool ok = false;
    std::string result;

    if (command == "protocol_version") {
      result = "1";
      ok = true;
    } else if (command == "name") {
      result = "atp-cpp-bb";
      ok = true;
    } else if (command == "version") {
      result = "1.0";
      ok = true;
    } else if (command == "board") {
      if (args.empty()) {
        result = "missing board name";
      } else if (args[0] != "classical" && args[0] != "classic") {
        result = "unsupported board";
      } else {
        g_board = bb::ClassicalBoard();
        g_player = 0;
        if (g_tt) g_tt->Clear();
        ok = true;
      }
    } else if (command == "clear_board") {
      g_board = bb::ClassicalBoard();
      g_player = 0;
      if (g_tt) g_tt->Clear();
      ok = true;
    } else if (command == "play") {
      int player;
      if (args.size() < 2) {
        result = "usage: play <color> <move>";
      } else if (!ColorToPlayer(args[0], &player)) {
        result = "unknown color";
      } else if (player != g_player) {
        result = "not this player's turn";
      } else {
        bb::Move move;
        if (!bb::TryParseAtp(g_board, player, args[1], move)) {
          result = "illegal move";
        } else {
          bb::ApplyMove(g_board, bb::Executors()[bb::ExecutorIndex(
                                     move.dir, move.id, move.cell)],
                        player);
          g_player = 1 - g_player;
          ok = true;
        }
      }
    } else if (command == "genmove") {
      int player;
      if (args.empty()) {
        result = "usage: genmove <color>";
      } else if (!ColorToPlayer(args[0], &player)) {
        result = "unknown color";
      } else if (player != g_player) {
        result = "not this player's turn";
      } else {
        auto ctx = new bb::SearchContext();
        int idx;
        if (g_budgetMs > 0) {
          ctx->tt = g_tt;
          idx = bb::GenMoveId(g_board, player, config, idMaxDepth, g_budgetMs,
                              *ctx);
        } else {
          if (g_algo == bb::Algo::AB_TT) ctx->tt = g_tt;
          idx = bb::GenMove(g_board, player, config, g_algo, g_depth, *ctx);
        }
        delete ctx;
        if (idx < 0) {
          result = "resign";
        } else {
          const bb::Move& move = bb::Executors()[idx].move;
          result = bb::MoveToAtp(g_board, player, move);
          bb::ApplyMove(g_board, bb::Executors()[idx], player);
          g_player = 1 - g_player;
        }
        ok = true;
      }
    } else if (command == "quit") {
      ok = true;
    } else {
      result = "unknown command";
    }

    if (ok)
      std::printf("=%s\n\n", result.empty() ? "" : (" " + result).c_str());
    else
      std::printf("? %s\n\n", result.c_str());
    std::fflush(stdout);
    if (ok && command == "quit")
      return 0;
  }
  return 0;
}
