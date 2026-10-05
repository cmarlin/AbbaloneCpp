// Bindings Python (pybind11) du solveur Abalone bitboard
// (Sources/BitboardAbalone), compilé sans modification du cœur C++.
//
// Expose : Board (2 x uint64), Move, Solver (profondeur fixe ou iterative
// deepening avec budget), la notation ATP et le perft.
//
// La recherche garde le GIL : une recherche profonde bloque l'interpréteur
// — la parallélisation se fait par processus, pas par thread.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "bb_core.h"
#include "bb_tt.h"

namespace py = pybind11;
using namespace bb;

namespace {

// Cap de profondeur commun à tous les points d'entrée. SearchContext::moves
// est dimensionné pour kMaxDepth (64) et indexé par la profondeur courante :
// les recherches pleines écrivent moves[depth] dès l'appel racine et
// l'extension de quiescence de SortedAlphaBeta atteint finalDepth + 1,
// donc 62 est sûr pour tous les algorithmes.
constexpr int kSafeMaxDepth = 62;

bool ParseAlgo(const std::string& name, Algo* out) {
  if (name == "minimax") { *out = Algo::MiniMax; return true; }
  if (name == "abrnd") { *out = Algo::AB_Rnd; return true; }
  if (name == "absort") { *out = Algo::AB_Sort; return true; }
  if (name == "abradix") { *out = Algo::AB_Radix; return true; }
  if (name == "abtt") { *out = Algo::AB_TT; return true; }
  return false;
}

bool ParseWeights(const std::string& name, EvalWeights* out) {
  if (name == "default") { *out = EvalWeights::Default(); return true; }
  if (name == "tweak") { *out = EvalWeights::Tweak(); return true; }
  return false;
}

void CheckPlayer(int player) {
  if (player != 0 && player != 1)
    throw std::invalid_argument("player must be 0 (white) or 1 (black)");
}

void CheckDepth(int depth) {
  if (depth < 1 || depth > kSafeMaxDepth)
    throw std::invalid_argument("depth must be in 1.." +
                                std::to_string(kSafeMaxDepth));
}

std::vector<Move> LegalMoves(const Board& b, int player) {
  MoveList moves;
  ComputeMoveList(moves, b, player);
  const auto& executors = Executors();
  std::vector<Move> out;
  for (int d = 0; d < kNumDirs; ++d) {
    for (int id = Single_1; id < kNumMoveIds; ++id) {
      uint64_t m = moves.masks[d][id];
      while (m) {
        const int cell = __builtin_ctzll(m);
        m &= m - 1;
        out.push_back(executors[ExecutorIndexFast(d, id, cell)].move);
      }
    }
  }
  return out;
}

bool IsLegal(const Board& b, int player, const Move& m) {
  if (ExecutorIndex(m.dir, m.id, m.cell) < 0) return false;
  MoveList moves;
  ComputeMoveList(moves, b, player);
  return (moves.masks[m.dir][m.id] >> m.cell) & 1;
}

// Applique un coup après validation de légalité (ValueError sinon).
void ApplyLegal(Board& b, int player, const Move& m) {
  if (!IsLegal(b, player, m))
    throw std::invalid_argument("illegal move on this board");
  ApplyMove(b, Executors()[ExecutorIndex(m.dir, m.id, m.cell)], player);
}

int Evaluate(const Board& b, const std::string& weights) {
  EvalWeights w;
  if (!ParseWeights(weights, &w))
    throw std::invalid_argument("unknown weights: " + weights);
  return Eval(w, b);
}

std::string BoardRepr(const Board& b) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "Board(p0=0x%016llx, p1=0x%016llx)",
                static_cast<unsigned long long>(b.p[0]),
                static_cast<unsigned long long>(b.p[1]));
  return buf;
}

std::string MoveRepr(const Move& m) {
  return "Move(dir=" + std::to_string(m.dir) + ", id=" +
         std::to_string(m.id) + ", cell=" + CellName(m.cell) + ")";
}

// Un solveur = une config + un contexte de recherche + (option) une TT.
// Non copiable : SearchContext pointe sur la config membre.
class PySolver {
 public:
  PySolver(const std::string& algo, int depth, const std::string& weights,
           bool quiescent, int window, int ttMb, int budgetMs)
      : config_(MakeWeights(weights), quiescent, window) {
    if (!ParseAlgo(algo, &algo_))
      throw std::invalid_argument("unknown algo: " + algo);
    CheckDepth(depth);
    if (ttMb < 0) throw std::invalid_argument("tt_mb must be >= 0");
    depth_ = depth;
    budgetMs_ = budgetMs;
    if ((algo_ == Algo::AB_TT || budgetMs_ > 0) && ttMb > 0)
      tt_ = std::make_unique<TranspositionTable>(static_cast<size_t>(ttMb));
    ctx_.config = &config_;
    ctx_.tt = tt_.get();
  }

  PySolver(const PySolver&) = delete;
  PySolver& operator=(const PySolver&) = delete;

  // Meilleur coup, None si aucun coup légal. budget_ms > 0 : iterative
  // deepening (GenMoveId), depth devient un plafond ; sinon recherche à
  // profondeur fixe (GenMove) avec l'algorithme choisi.
  std::optional<Move> BestMove(const Board& b, int player) {
    CheckPlayer(player);
    ctx_.nodeCount = 0;
    ctx_.leafCount = 0;
    const int idx =
        (budgetMs_ > 0)
            ? GenMoveId(b, player, config_, depth_, budgetMs_, ctx_)
            : GenMove(b, player, config_, algo_, depth_, ctx_);
    if (idx < 0) return std::nullopt;
    return Executors()[idx].move;
  }

  long nodes() const { return ctx_.nodeCount; }
  long leaves() const { return ctx_.leafCount; }
  // Dernière itération complétée (mode budget uniquement ; 0 sinon).
  int lastDepth() const { return ctx_.idDepth; }
  int lastScore() const { return ctx_.idScore; }

 private:
  static EvalWeights MakeWeights(const std::string& name) {
    EvalWeights w;
    if (!ParseWeights(name, &w))
      throw std::invalid_argument("unknown weights: " + name);
    return w;
  }

  Algo algo_ = Algo::AB_Sort;
  int depth_ = 3;
  int budgetMs_ = 0;
  SearchConfig config_;
  std::unique_ptr<TranspositionTable> tt_;
  SearchContext ctx_;
};

}  // namespace

PYBIND11_MODULE(_engine, m) {
  m.doc() =
      "Bindings Python du solveur Abalone bitboard (cœur C++ "
      "Sources/BitboardAbalone). Joueur 0 = blanc, joueur 1 = noir.";

  Executors();  // init des tables globales dès l'import

  py::class_<Move>(m, "Move")
      .def(py::init<int, int, int>(), py::arg("dir"), py::arg("id"),
           py::arg("cell"))
      .def(py::init<>())
      .def_property(
          "dir",
          [](const Move& m) { return static_cast<int>(m.dir); },
          [](Move& m, int v) { m.dir = static_cast<uint8_t>(v); })
      .def_property(
          "id",
          [](const Move& m) { return static_cast<int>(m.id); },
          [](Move& m, int v) { m.id = static_cast<uint8_t>(v); })
      .def_property(
          "cell",
          [](const Move& m) { return static_cast<int>(m.cell); },
          [](Move& m, int v) { m.cell = static_cast<uint8_t>(v); })
      .def("key", &Move::key)
      .def("__eq__",
           [](const Move& a, const Move& b) {
             return a.dir == b.dir && a.id == b.id && a.cell == b.cell;
           })
      .def("__repr__", &MoveRepr);

  py::class_<Board>(m, "Board")
      .def(py::init<>())
      .def(py::init<uint64_t, uint64_t>(), py::arg("p0"), py::arg("p1"))
      .def_static("classical", []() { return ClassicalBoard(); })
      .def_property(
          "p0",
          [](const Board& b) { return b.p[0]; },
          [](Board& b, uint64_t v) { b.p[0] = v; })
      .def_property(
          "p1",
          [](const Board& b) { return b.p[1]; },
          [](Board& b, uint64_t v) { b.p[1] = v; })
      .def("copy", [](const Board& b) { return b; })
      .def("is_legal",
           [](const Board& b, const Move& m, int player) {
             return IsLegal(b, player, m);
           },
           py::arg("move"), py::arg("player"))
      .def("legal_moves", &LegalMoves, py::arg("player"))
      .def("apply",
           [](Board& b, const Move& m, int player) {
             ApplyLegal(b, player, m);
           },
           py::arg("move"), py::arg("player"))
      .def("evaluate", &Evaluate, py::arg("weights") = "default")
      .def("__eq__",
           [](const Board& a, const Board& b) {
             return a.p[0] == b.p[0] && a.p[1] == b.p[1];
           })
      .def("__repr__", &BoardRepr);

  py::class_<PySolver>(m, "Solver")
      .def(py::init<const std::string&, int, const std::string&, bool, int,
                    int, int>(),
           py::arg("algo") = "abtt", py::arg("depth") = 3,
           py::arg("weights") = "default", py::arg("quiescent") = false,
           py::arg("window") = 1000, py::arg("tt_mb") = 64,
           py::arg("budget_ms") = 0)
      .def("best_move", &PySolver::BestMove, py::arg("board"),
           py::arg("player"))
      .def_property_readonly("nodes", &PySolver::nodes)
      .def_property_readonly("leaves", &PySolver::leaves)
      .def_property_readonly("last_depth", &PySolver::lastDepth)
      .def_property_readonly("last_score", &PySolver::lastScore);

  m.def(
      "parse_atp",
      [](const Board& b, int player, const std::string& s) {
        Move mv;
        std::optional<Move> out;
        if (TryParseAtp(b, player, s, mv)) out = mv;
        return out;
      },
      py::arg("board"), py::arg("player"), py::arg("move"));

  m.def(
      "move_to_atp",
      [](const Board& b, int player, const Move& m) {
        const std::string s = MoveToAtp(b, player, m);
        if (s.empty())
          throw std::invalid_argument("move is not legal on this board");
        return s;
      },
      py::arg("board"), py::arg("player"), py::arg("move"));

  m.def("cell_name", &CellName, py::arg("cell"));
  m.def("cell_from_name", &CellFromName, py::arg("name"));
  m.def("perft", &Perft, py::arg("board"), py::arg("depth"),
        py::arg("player"));

  m.attr("ALGORITHMS") =
      py::make_tuple("minimax", "abrnd", "absort", "abradix", "abtt");
  m.attr("WEIGHTS") = py::make_tuple("default", "tweak");
}
