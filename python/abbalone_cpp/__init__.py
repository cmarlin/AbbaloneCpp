"""Bindings Python du solveur Abalone bitboard (cœur C++ Sources/BitboardAbalone).

Joueur 0 = blanc (commence), joueur 1 = noir. Notation des coups : ATP
("a1b2" poussée en ligne, "a1b2c3" glissement latéral), identique à celle
du jeu pyspiel `abalone` et du moteur ATP `atp_engine_bb`.

Exemple :

    >>> import abalone_cpp as ab
    >>> board = ab.Board.classical()
    >>> len(board.legal_moves(0))
    44
    >>> solver = ab.Solver(algo="abtt", depth=4)
    >>> move = solver.best_move(board, player=0)
    >>> ab.move_to_atp(board, 0, move)
    'a1b2'
    >>> board.apply(move, 0)

La recherche garde le GIL : une recherche profonde bloque l'interpréteur.
Pour paralléliser, utilisez des processus (un Solver par processus).
"""

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

__version__ = "1.0.0"
