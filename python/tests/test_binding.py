"""Tests du binding Python abbalone_cpp (pytest)."""

import pickle
import threading

import pytest

import abbalone_cpp as ab

OUTSIDE_CELLS = 1 << 63


@pytest.fixture
def board():
    return ab.Board.classical()


# --- Plateau et coups --------------------------------------------------------


def test_classical_moves(board):
    assert len(board.legal_moves(0)) == 44
    assert len(board.legal_moves(1)) == 44


@pytest.mark.parametrize("depth,count", [(0, 1), (1, 44), (2, 1936), (3, 98912)])
def test_perft(board, depth, count):
    assert ab.perft(board, depth, 0) == count


def test_apply_and_atp_roundtrip(board):
    for move in board.legal_moves(0):
        s = ab.move_to_atp(board, 0, move)
        assert ab.parse_atp(board, 0, s) == move
    move = board.legal_moves(0)[0]
    before = board.copy()
    board.apply(move, 0)
    assert board != before
    assert board.p1 == before.p1


def test_illegal_apply_raises(board):
    move = ab.Move(0, 1, 30)  # case vide au centre
    assert not board.is_legal(move, 0)
    with pytest.raises(ValueError):
        board.apply(move, 0)


def test_cells():
    assert ab.cell_name(0) == "a1"
    assert ab.cell_from_name("A1") == 0
    with pytest.raises(ValueError):
        ab.cell_name(61)
    with pytest.raises(ValueError):
        ab.cell_from_name("z9")


# --- Validation des entrées ---------------------------------------------------


@pytest.mark.parametrize("player", [-1, 2, 100])
def test_invalid_player(board, player):
    move = board.legal_moves(0)[0]
    calls = [
        lambda: board.legal_moves(player),
        lambda: board.is_legal(move, player),
        lambda: board.apply(move, player),
        lambda: ab.perft(board, 1, player),
        lambda: ab.parse_atp(board, player, "a1b2"),
        lambda: ab.move_to_atp(board, player, move),
        lambda: ab.Solver(depth=1).best_move(board, player),
    ]
    for call in calls:
        with pytest.raises(ValueError):
            call()


def test_negative_perft_depth(board):
    with pytest.raises(ValueError):
        ab.perft(board, -1, 0)


@pytest.mark.parametrize(
    "p0,p1", [(OUTSIDE_CELLS, 0), (0, OUTSIDE_CELLS), (0xFF, 0x01)]
)
def test_invalid_board(p0, p1):
    with pytest.raises(ValueError):
        ab.Board(p0, p1)


def test_invalid_board_setters(board):
    with pytest.raises(ValueError):
        board.p0 = OUTSIDE_CELLS
    with pytest.raises(ValueError):
        board.p1 = board.p0
    assert board == ab.Board.classical()


@pytest.mark.parametrize("args", [(6, 1, 0), (0, 11, 0), (0, 1, 61), (300, 1, 0), (-1, 1, 0)])
def test_invalid_move(args):
    with pytest.raises(ValueError):
        ab.Move(*args)


def test_invalid_move_setter():
    move = ab.Move(0, 1, 0)
    with pytest.raises(ValueError):
        move.dir = 300
    assert move.dir == 0


@pytest.mark.parametrize(
    "kwargs",
    [
        {"algo": "nope"},
        {"weights": "nope"},
        {"depth": 0},
        {"depth": 63},
        {"budget_ms": -1},
        {"tt_mb": -1},
        {"window": 0},
    ],
)
def test_invalid_solver(kwargs):
    with pytest.raises(ValueError):
        ab.Solver(**kwargs)


# --- Protocole Python ---------------------------------------------------------


def test_hash_and_eq(board):
    moves = board.legal_moves(0)
    assert len(set(moves)) == len(moves)
    assert {board: 1}[ab.Board.classical()] == 1
    assert board != None  # noqa: E711
    assert ab.Move(0, 1, 0) != "a1"


def test_pickle(board):
    assert pickle.loads(pickle.dumps(board)) == board
    move = board.legal_moves(0)[3]
    assert pickle.loads(pickle.dumps(move)) == move


# --- Solveur -------------------------------------------------------------------


@pytest.mark.parametrize("algo", ab.ALGORITHMS)
def test_solver_algorithms(board, algo):
    solver = ab.Solver(algo=algo, depth=2)
    move = solver.best_move(board, 0)
    assert board.is_legal(move, 0)
    assert solver.nodes > 0


def test_solver_depth_defaults():
    assert ab.Solver().depth == 3
    assert ab.Solver(budget_ms=50).depth == 32
    assert ab.Solver(budget_ms=50, depth=5).depth == 5


def test_solver_budget(board):
    solver = ab.Solver(budget_ms=50)
    move = solver.best_move(board, 0)
    assert board.is_legal(move, 0)
    assert solver.last_depth >= 1


def test_no_legal_move():
    assert ab.Solver(depth=2).best_move(ab.Board(), 0) is None


def test_solver_threads(board):
    # GIL relâché : plusieurs solveurs (et un solveur partagé) en parallèle.
    shared = ab.Solver(algo="abtt", depth=3)
    expected = ab.Solver(algo="abtt", depth=3).best_move(board, 0)
    results = []

    def run(solver):
        results.append(solver.best_move(board, 0))

    threads = [threading.Thread(target=run, args=(shared,)) for _ in range(4)]
    threads += [
        threading.Thread(target=run, args=(ab.Solver(algo="abtt", depth=3),))
        for _ in range(4)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert results == [expected] * len(threads)
