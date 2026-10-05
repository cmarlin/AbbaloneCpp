#!/usr/bin/env python3
"""Tournoi round-robin de moteurs Abalone via ATP (voir Tournament/ATP.md).

Mode évaluation d'un agent :
  python3 Tournament/tournament.py --config C --evaluate NAME
      [--target-se 20] [--max-rounds 10] [--opponents 3]

  Seul le moteur NAME joue : à chaque tour il rencontre les `--opponents`
  moteurs les plus proches au classement courant (l'information Elo par
  partie est maximale contre un adversaire de niveau égal), couleurs
  alternées entre les tours. Son Elo est ajusté par régression
  logistique (MLE) sur ses résultats — les Elo des adversaires étant
  supposés connus — et l'évaluation s'arrête quand l'erreur standard
  du fit tombe sous `--target-se` (le moteur a trouvé ses adversaires
  de niveau équivalent) ou après `--max-rounds` tours. Le rating ajusté
  est réécrit dans le fichier ELO à la fin.

Usage :
  python3 Tournament/tournament.py --config Tournament/engines.json
  python3 Tournament/tournament.py --config Tournament/engines.json --games 4

Classement ELO :
  - chaque partie met à jour un ELO (facteur K configurable, draws = 0,5) ;
  - les ELO sont chargés puis sauvegardés dans `log/elo.json` à côté du
    config (--ratings pour changer le chemin, --no-ratings pour désactiver) ;
  - un moteur absent du fichier démarre à `initial_elo` (défaut 400).

Format du fichier de configuration (JSON) :
  {
    "board": "classical",
    "games_per_pair": 2,        # total par paire, couleurs alternées
    "move_limit": 300,          # limite de coups -> draw
    "move_timeout": 10.0,       # secondes par genmove -> forfeit
    "random_plies": 10,         # demi-coups d'ouverture aléatoires (optionnel)
    "random_seed": 42,          # seed des ouvertures, reproductible (optionnel)
    "initial_elo": 400,         # ELO de départ d'un moteur inconnu (optionnel)
    "k_factor": 32,             # facteur K ELO (optionnel)
    "engines": [
      {"name": "cpp-d3",
       "cmd": ["/chemin/atp_engine", "--depth", "3"],
       "env": {"MA_VAR": "valeur"}}   // env optionnel
      }
    ]
  }

`random_plies` : les moteurs sont déterministes — sans ouverture aléatoire,
toutes les parties d'une paire (à la couleur près) sont identiques. Les
N premiers demi-coups sont tirés au hasard et joués via `play` dans les deux
moteurs avant de leur rendre la main. `random_seed` fixe le générateur pour
rendre les ouvertures reproductibles (désactivé par défaut : entropie du
système).

L'arbitre est le jeu pyspiel `abalone` : chaque coup renvoyé par genmove est
validé contre les coups légaux de l'arbitre ; coup illégal, timeout, crash ou
`resign` = forfeit (l'adversaire gagne).
"""

import argparse
import json
import math
import os
import random
import select
import subprocess
import time

import pyspiel

COLORS = {0: "white", 1: "black"}

DEFAULT_INITIAL_ELO = 400.0
DEFAULT_K = 32.0
# Pente du logit par point Elo : p = 1 / (1 + 10^((Rb - Ra)/400)).
LOGIT_SLOPE = math.log(10.0) / 400.0


def expected_score(rating_a, rating_b):
    """Score attendu de A contre B (0-1), échelle ELO classique à 400 points."""
    return 1.0 / (1.0 + 10.0 ** ((rating_b - rating_a) / 400.0))


def fit_rating(games):
    """MLE du Elo d'un moteur d'après ses parties, adversaires supposés connus.

    games : liste de (elo_adversaire, score) avec score dans {0.0, 0.5, 1.0}.
    Renvoie (elo, erreur_standard) — l'ESR vient de l'information de Fisher
    du modèle logistique — ou (None, None) sans donnée exploitable (aucune
    partie, ou que des victoires/défaites sans aucun mélange : le MLE
    diverge et l'information est nulle).
    """
    if not games:
        return None, None
    rating = 1000.0
    for _ in range(64):
        probs = [expected_score(rating, opp) for opp, _ in games]
        num = sum(score - p for (_, score), p in zip(games, probs))
        den = LOGIT_SLOPE * sum(p * (1.0 - p) for p in probs)
        if den < 1e-9:
            return None, None
        step = num / den
        rating += max(-400.0, min(400.0, step))
        if abs(step) < 0.01:
            break
    probs = [expected_score(rating, opp) for opp, _ in games]
    info = (LOGIT_SLOPE ** 2) * sum(p * (1.0 - p) for p in probs)
    if info < 1e-12:
        return None, None
    return rating, 1.0 / math.sqrt(info)


def load_elo(path):
    """Charge le fichier ELO ; renvoie un état vide s'il n'existe pas encore."""
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return {"initial_elo": DEFAULT_INITIAL_ELO, "k": DEFAULT_K, "engines": {}}


def save_elo(path, elo_data):
    """Écriture atomique (tmp + rename) : une interruption ne corrompt pas."""
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(elo_data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


class EngineProcess:
    """Moteur ATP lancé en sous-processus.

    Les pipes sont en mode binaire non bufferisé : la lecture passe par
    select + os.read pour imposer un timeout strict, sans buffer intermédiaire
    qui rendrait select aveugle aux données déjà lues.
    """

    def __init__(self, name, cmd, extra_env=None):
        self.name = name
        self._buf = b""
        env = dict(os.environ)
        env.update(extra_env or {})
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
            env=env,
        )

    def send(self, line):
        try:
            self.proc.stdin.write((line + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    @staticmethod
    def _finish(lines):
        first = lines[0]
        ok = first.startswith("=")
        result = first[1:].strip()
        for extra in lines[1:]:
            result += " " + extra.strip()
        return ok, result

    def read_response(self, timeout):
        """Lit une réponse ATP (en-tête =/? + ligne vide). Timeout en secondes."""
        deadline = time.monotonic() + timeout
        lines = []
        buf = self._buf
        while True:
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                if line == "":
                    if lines:
                        self._buf = buf
                        return self._finish(lines)
                    continue
                lines.append(line)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self._buf = buf
                raise TimeoutError(f"{self.name}: response timeout")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                self._buf = buf
                raise TimeoutError(f"{self.name}: response timeout")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                self._buf = buf
                raise RuntimeError(f"{self.name}: engine died")
            buf += chunk

    def command(self, line, timeout):
        self.send(line)
        return self.read_response(timeout)

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        for stream in (self.proc.stdin, self.proc.stdout):
            try:
                stream.close()
            except Exception:
                pass


def play_game(engine_white, engine_black, board, move_limit, move_timeout,
              random_plies=0, rng=None):
    """Joue une partie complète. Renvoie (winner, reason, moves).

    winner : 0 (white), 1 (black), -1 (draw), None si erreur interne.

    random_plies : nombre de demi-coups d'ouverture joués au hasard (via
    `play`) dans les deux moteurs avant de leur rendre la main. Indispensable
    face à des moteurs déterministes : sans cela, toutes les parties d'une
    même paire (à couleur près) sont identiques.
    """
    game = pyspiel.load_game("abalone", {"board": board})
    state = game.new_initial_state()
    engines = {0: engine_white, 1: engine_black}

    for engine in (engine_white, engine_black):
        ok, _ = engine.command(f"board {board}", move_timeout)
        if not ok:
            return None, f"{engine.name}: board failed", 0

    for _ in range(random_plies):
        if state.is_terminal():
            break
        player = state.current_player()
        action = rng.choice(state.legal_actions())
        move = state.action_to_string(player, action)
        state.apply_action(action)
        for engine in (engine_white, engine_black):
            ok, _ = engine.command(
                f"play {COLORS[player]} {move}", move_timeout
            )
            if not ok:
                return None, f"{engine.name}: rejected opening {move}", 0

    for move_number in range(move_limit):
        player = state.current_player()
        color = COLORS[player]
        opponent = 1 - player

        ok, result = engines[player].command(f"genmove {color}", move_timeout)
        if not ok:
            return opponent, f"{engines[player].name}: genmove error: {result}", move_number
        if result == "resign":
            return opponent, f"{engines[player].name}: resigned", move_number

        legal = {
            state.action_to_string(player, a)
            for a in state.legal_actions()
        }
        if result not in legal:
            return opponent, f"{engines[player].name}: illegal move {result!r}", move_number

        state.apply_action(
            next(
                a
                for a in state.legal_actions()
                if state.action_to_string(player, a) == result
            )
        )
        ok, _ = engines[opponent].command(f"play {color} {result}", move_timeout)
        if not ok:
            return player, f"{engines[opponent].name}: rejected {result}", move_number

        if state.is_terminal():
            returns = state.returns()
            if returns[0] > returns[1]:
                return 0, "ejections", move_number + 1
            if returns[1] > returns[0]:
                return 1, "ejections", move_number + 1
            return -1, "terminal draw", move_number + 1

    return -1, "move limit", move_limit


def play_and_record(cfg_white, cfg_black, board, move_limit, move_timeout,
                    ratings, elo_data, k, initial_elo, ratings_path, stats,
                    note="", random_plies=0, rng=None):
    """Joue une partie, met à jour les stats et les ELO persistants.

    Renvoie (winner, reason, moves) avec winner : 0 (white), 1 (black),
    -1 (draw) ou None (forfeit / erreur interne).
    """
    white = EngineProcess(cfg_white["name"], cfg_white["cmd"], cfg_white.get("env"))
    black = EngineProcess(cfg_black["name"], cfg_black["cmd"], cfg_black.get("env"))
    try:
        winner, reason, moves = play_game(
            white, black, board, move_limit, move_timeout,
            random_plies, rng,
        )
    except (TimeoutError, RuntimeError) as exc:
        winner, reason, moves = None, str(exc), 0
    finally:
        white.close()
        black.close()

    w_name, b_name = cfg_white["name"], cfg_black["name"]
    if winner == 0:
        stats[w_name]["W"] += 1
        stats[b_name]["L"] += 1
        outcome = f"{w_name} (white)"
    elif winner == 1:
        stats[b_name]["W"] += 1
        stats[w_name]["L"] += 1
        outcome = f"{b_name} (black)"
    elif winner == -1:
        stats[w_name]["D"] += 1
        stats[b_name]["D"] += 1
        outcome = "draw"
    else:
        outcome = "void"
    print(
        f"{w_name} vs {b_name}{note} : {outcome} ({reason}, {moves} coups)"
    )

    if winner is not None:
        stored = elo_data.setdefault("engines", {})
        score_white = {0: 1.0, 1: 0.0, -1: 0.5}[winner]
        exp_white = expected_score(ratings[w_name], ratings[b_name])
        ratings[w_name] += k * (score_white - exp_white)
        ratings[b_name] += k * (1.0 - score_white - (1.0 - exp_white))
        for name in (w_name, b_name):
            entry = stored.setdefault(
                name,
                {
                    "elo": initial_elo,
                    "games": 0,
                    "W": 0,
                    "L": 0,
                    "D": 0,
                },
            )
            entry["elo"] = round(ratings[name], 1)
            entry["games"] += 1
        if winner == 0:
            stored[w_name]["W"] += 1
            stored[b_name]["L"] += 1
        elif winner == 1:
            stored[b_name]["W"] += 1
            stored[w_name]["L"] += 1
        else:
            stored[w_name]["D"] += 1
            stored[b_name]["D"] += 1
        elo_data["initial_elo"] = initial_elo
        elo_data["k"] = k
        if ratings_path:
            save_elo(ratings_path, elo_data)
    return winner, reason, moves


def run_tournament(config, games_override=None, ratings_path=None, k_override=None):
    board = config.get("board", "classical")
    games_per_pair = games_override or config.get("games_per_pair", 2)
    move_limit = config.get("move_limit", 300)
    move_timeout = config.get("move_timeout", 10.0)
    random_plies = config.get("random_plies", 0)
    rng = random.Random(config.get("random_seed"))
    engines_cfg = config["engines"]

    stats = {
        e["name"]: {"W": 0, "L": 0, "D": 0} for e in engines_cfg
    }

    # ELO : reprise depuis le fichier s'il existe, sinon départ à initial_elo.
    elo_data = load_elo(ratings_path)
    initial_elo = config.get(
        "initial_elo", elo_data.get("initial_elo", DEFAULT_INITIAL_ELO)
    )
    if k_override is not None:
        k = k_override
    else:
        k = config.get("k_factor", elo_data.get("k", DEFAULT_K))
    stored = elo_data.setdefault("engines", {})
    ratings = {
        e["name"]: float(stored.get(e["name"], {}).get("elo", initial_elo))
        for e in engines_cfg
    }
    start_elo = dict(ratings)

    for i, cfg_a in enumerate(engines_cfg):
        for cfg_b in engines_cfg[i + 1:]:
            for game_idx in range(games_per_pair):
                # Alternance des couleurs entre les deux moitiés de la paire
                if game_idx % 2 == 0:
                    white_cfg, black_cfg = cfg_a, cfg_b
                else:
                    white_cfg, black_cfg = cfg_b, cfg_a

                play_and_record(
                    white_cfg, black_cfg, board, move_limit, move_timeout,
                    ratings, elo_data, k, initial_elo, ratings_path, stats,
                    note=f" [{game_idx + 1}/{games_per_pair}]",
                    random_plies=random_plies, rng=rng,
                )

    print("\nClassement :")
    header = f"{'moteur':<24}{'Elo':>8}{'Δ':>7}{'W':>4}{'L':>4}{'D':>4}"
    print(header)
    print("-" * len(header))
    for name, s in sorted(
        stats.items(), key=lambda kv: ratings[kv[0]], reverse=True
    ):
        delta = ratings[name] - start_elo[name]
        print(
            f"{name:<24}{ratings[name]:>8.0f}{delta:>+7.0f}"
            f"{s['W']:>4}{s['L']:>4}{s['D']:>4}"
        )


def run_evaluation(config, target, ratings_path=None, k_override=None,
                   target_se=20.0, max_rounds=10, n_opponents=3):
    """Mode évaluation : seul `target` joue, contre ses plus proches voisins.

    À chaque tour, `target` rencontre les `n_opponents` moteurs les plus
    proches au classement courant (couleurs alternées entre les tours) —
    c'est contre un adversaire de niveau égal qu'une partie apporte le
    plus d'information Elo. Son rating est ajusté par MLE logistique sur
    ses résultats (fit_rating) ; l'évaluation s'arrête quand l'erreur
    standard tombe sous `target_se` ou après `max_rounds` tours.
    """
    board = config.get("board", "classical")
    move_limit = config.get("move_limit", 300)
    move_timeout = config.get("move_timeout", 10.0)
    random_plies = config.get("random_plies", 0)
    rng = random.Random(config.get("random_seed"))
    engines_cfg = config["engines"]

    cfg_by_name = {e["name"]: e for e in engines_cfg}
    if target not in cfg_by_name:
        print(f"moteur inconnu dans le config : {target}")
        return
    others = [e for e in engines_cfg if e["name"] != target]
    if not others:
        print("aucun adversaire dans le config")
        return

    stats = {e["name"]: {"W": 0, "L": 0, "D": 0} for e in engines_cfg}
    elo_data = load_elo(ratings_path)
    initial_elo = config.get(
        "initial_elo", elo_data.get("initial_elo", DEFAULT_INITIAL_ELO)
    )
    if k_override is not None:
        k = k_override
    else:
        k = config.get("k_factor", elo_data.get("k", DEFAULT_K))
    stored = elo_data.setdefault("engines", {})
    ratings = {
        e["name"]: float(stored.get(e["name"], {}).get("elo", initial_elo))
        for e in engines_cfg
    }

    print(f"Évaluation de {target} (Elo initial {ratings[target]:.0f})")
    print(
        f"arrêt : erreur standard <= {target_se:.0f} ou {max_rounds} tours\n"
    )

    games = []  # (elo adversaire au moment de la partie, score de target)
    void_games = 0
    for round_idx in range(1, max_rounds + 1):
        order = sorted(
            others, key=lambda e: abs(ratings[e["name"]] - ratings[target])
        )
        for opp in order[:n_opponents]:
            if round_idx % 2 == 1:
                white_cfg, black_cfg = cfg_by_name[target], opp
            else:
                white_cfg, black_cfg = opp, cfg_by_name[target]
            opp_elo = ratings[opp["name"]]
            winner, _, _ = play_and_record(
                white_cfg, black_cfg, board, move_limit, move_timeout,
                ratings, elo_data, k, initial_elo, ratings_path, stats,
                note=f" [tour {round_idx}]",
                random_plies=random_plies, rng=rng,
            )
            if winner is None:
                void_games += 1
                continue
            if white_cfg["name"] == target:
                score = {0: 1.0, 1: 0.0, -1: 0.5}[winner]
            else:
                score = {1: 1.0, 0: 0.0, -1: 0.5}[winner]
            games.append((opp_elo, score))

        rating, se = fit_rating(games)
        if rating is None:
            print(
                f"  tour {round_idx} : pas encore d'estimation stable "
                f"({len(games)} parties)\n"
            )
            continue
        print(
            f"  tour {round_idx} : {target} à {rating:.0f} ± {se:.0f} "
            f"({len(games)} parties)\n"
        )
        if se <= target_se:
            break

    rating, se = fit_rating(games)
    print("Résultat de l'évaluation :")
    if rating is None:
        print(
            f"  pas d'estimation ({len(games)} parties exploitables, "
            f"{void_games} forfeits)"
        )
        return
    ci = 1.96 * se
    print(
        f"  {target} : Elo {rating:.0f} ± {se:.0f} "
        f"(IC 95 % : [{rating - ci:.0f}, {rating + ci:.0f}]), "
        f"{len(games)} parties"
    )
    peers = sorted(
        (e["name"], ratings[e["name"]])
        for e in others
        if abs(ratings[e["name"]] - rating) <= ci
    )
    if peers:
        print(
            "  niveau équivalent (dans l'IC 95 %) : "
            + ", ".join(f"{n} ({r:.0f})" for n, r in peers)
        )
    print("\nPar adversaire (W-L-D de l'adversaire) :")
    for e in others:
        s = stats[e["name"]]
        if s["W"] or s["L"] or s["D"]:
            print(f"  {e['name']:<24} {s['W']:>2}-{s['L']}-{s['D']}")
    if ratings_path:
        stored.setdefault(
            target, {"elo": initial_elo, "games": 0, "W": 0, "L": 0, "D": 0}
        )
        stored[target]["elo"] = round(rating, 1)
        save_elo(ratings_path, elo_data)
        print(f"\nElo ajusté écrit dans {ratings_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--games", type=int, default=None)
    parser.add_argument(
        "--ratings",
        default=None,
        help="fichier ELO à charger/sauver (défaut : log/elo.json à côté du config)",
    )
    parser.add_argument(
        "--no-ratings",
        action="store_true",
        help="désactive la sauvegarde/reprise des ELO",
    )
    parser.add_argument(
        "--k",
        type=float,
        default=None,
        help="facteur K ELO (défaut : k_factor du config, sinon 32)",
    )
    parser.add_argument(
        "--evaluate",
        default=None,
        metavar="NAME",
        help="évalue le moteur NAME : il joue seul, contre ses plus proches "
        "voisins au classement, jusqu'à stabilisation de son rating",
    )
    parser.add_argument(
        "--target-se",
        type=float,
        default=20.0,
        help="erreur standard Elo visée en mode évaluation (défaut : 20)",
    )
    parser.add_argument(
        "--max-rounds",
        type=int,
        default=10,
        help="tours max en mode évaluation (défaut : 10)",
    )
    parser.add_argument(
        "--opponents",
        type=int,
        default=3,
        help="adversaires joués par tour, les plus proches (défaut : 3)",
    )
    args = parser.parse_args()

    with open(args.config, encoding="utf-8") as f:
        config = json.load(f)

    if args.no_ratings:
        ratings_path = None
    else:
        ratings_path = args.ratings or os.path.join(
            os.path.dirname(args.config) or ".", "log", "elo.json"
        )
    if args.evaluate:
        run_evaluation(
            config, args.evaluate, ratings_path, args.k,
            args.target_se, args.max_rounds, args.opponents,
        )
    else:
        run_tournament(config, args.games, ratings_path, args.k)


if __name__ == "__main__":
    main()
