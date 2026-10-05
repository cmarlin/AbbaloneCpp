"""Bindings Python du solveur Abalone bitboard (cœur C++ Sources/BitboardAbalone).

Joueur 0 = blanc (commence), joueur 1 = noir. Notation des coups : ATP
("a1b2" poussée en ligne, "a1b2c3" glissement latéral), identique à celle
du jeu pyspiel `abalone` et du moteur ATP `atp_engine_bb`.

Exemple :

    >>> import abbalone_cpp as ab
    >>> board = ab.Board.classical()
    >>> len(board.legal_moves(0))
    44
    >>> solver = ab.Solver(algo="abtt", depth=4)
    >>> move = solver.best_move(board, player=0)
    >>> ab.move_to_atp(board, 0, move)
    'a1b2'
    >>> board.apply(move, 0)

La recherche et le perft relâchent le GIL : plusieurs Solver peuvent
chercher en parallèle dans des threads (une même instance sérialise ses
appels). Board et Move sont hashables et picklables.
"""

from importlib.metadata import PackageNotFoundError, version

from ._engine import (
    ALGORITHMS,
    WEIGHTS,
    Board,
    Move,
    Solver,
    cell_from_name,
    cell_name,
    move_to_atp,
    parse_atp,
    perft,
)

__all__ = [
    "ALGORITHMS",
    "WEIGHTS",
    "Board",
    "Move",
    "Solver",
    "cell_from_name",
    "cell_name",
    "move_to_atp",
    "parse_atp",
    "perft",
]

try:
    __version__ = version("abbalone-cpp")
except PackageNotFoundError:  # module importé hors d'une installation
    __version__ = "0+unknown"
