#!/usr/bin/env python3
"""Moteur ATP (Abalone Text Protocol) basé sur pyspiel.

Bots disponibles :
  --bot random  : coup aléatoire légal
  --bot uct     : MCTS/UCT léger (rollouts aléatoires), --simulations N

Le plateau de départ est choisi via la commande ATP `board <nom>`
(paramètre `board` du jeu pyspiel `abalone`, `classical` par défaut).
"""

import argparse
import math
import random
import sys

import pyspiel

PROTOCOL_VERSION = "1"
ENGINE_NAME = "atp-pyspiel"
ENGINE_VERSION = "1.0"

COLORS = {"white": 0, "black": 1}


class RandomBot:
    def choose(self, state):
        return random.choice(state.legal_actions())


class UctNode:
    __slots__ = ("action", "parent", "children", "visits", "wins")

    def __init__(self, action, parent):
        self.action = action
        self.parent = parent
        self.children = []
        self.visits = 0
        self.wins = 0.0


class UctBot:
    """UCT simple : sélection UC1, expansion d'un noeud, rollout aléatoire."""

    def __init__(self, simulations=1000, seed=None):
        self.simulations = simulations
        self.rng = random.Random(seed)

    def choose(self, state):
        root = UctNode(None, None)
        player = state.current_player()
        for _ in range(self.simulations):
            node = root
            sim = state.clone()
            # Sélection + expansion
            while not sim.is_terminal() and node.children and all(
                c.visits > 0 for c in node.children
            ):
                node = max(
                    node.children,
                    key=lambda c: c.wins / c.visits
                    + math.sqrt(2.0 * math.log(node.visits) / c.visits),
                )
                sim.apply_action(node.action)
            if not sim.is_terminal():
                if not node.children:
                    node.children = [
                        UctNode(a, node) for a in sim.legal_actions()
                    ]
                unvisited = [c for c in node.children if c.visits == 0]
                node = self.rng.choice(unvisited)
                sim.apply_action(node.action)
            # Rollout aléatoire
            while not sim.is_terminal():
                sim.apply_action(self.rng.choice(sim.legal_actions()))
            # Backpropagation (résultat du point de vue de `player`)
            reward = sim.returns()[player]
            while node is not None:
                node.visits += 1
                node.wins += reward
                node = node.parent
                reward = -reward
        best = max(root.children, key=lambda c: c.visits)
        return best.action


class Engine:
    def __init__(self, bot_name, simulations, seed=None):
        if bot_name == "random":
            self.bot = RandomBot()
        elif bot_name == "uct":
            self.bot = UctBot(simulations, seed)
        else:
            raise ValueError(f"unknown bot: {bot_name}")
        self.bot_name = bot_name
        self.board_name = "classical"
        self.game = pyspiel.load_game("abalone")
        self.state = self.game.new_initial_state()

    def _legal_map(self):
        player = self.state.current_player()
        return {
            self.state.action_to_string(player, a): a
            for a in self.state.legal_actions()
        }

    def cmd_board(self, name):
        params = dict(self.game.get_parameters())
        params["board"] = name
        try:
            game = pyspiel.load_game("abalone", params)
        except Exception:
            raise RuntimeError("unsupported board")
        self.game = game
        self.board_name = name
        self.state = game.new_initial_state()

    def cmd_play(self, color, move):
        player = COLORS[color]
        if self.state.is_terminal():
            raise RuntimeError("game over")
        if self.state.current_player() != player:
            raise RuntimeError("not that player's turn")
        legal = self._legal_map()
        if move not in legal:
            raise RuntimeError("invalid move")
        self.state.apply_action(legal[move])

    def cmd_genmove(self, color):
        player = COLORS[color]
        if self.state.is_terminal():
            return "resign"
        if self.state.current_player() != player:
            raise RuntimeError("not that player's turn")
        action = self.bot.choose(self.state)
        move = self.state.action_to_string(player, action)
        self.state.apply_action(action)
        return move

    def cmd_clear_board(self):
        self.state = self.game.new_initial_state()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bot", choices=["random", "uct"], default="random")
    parser.add_argument("--simulations", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    engine = Engine(args.bot, args.simulations, args.seed)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        cmd_id = ""
        if parts[0].isdigit():
            cmd_id = parts[0]
            parts = parts[1:]
        cmd = parts[0] if parts else ""
        args_cmd = parts[1:]
        try:
            if cmd == "protocol_version":
                result = PROTOCOL_VERSION
            elif cmd == "name":
                result = f"{ENGINE_NAME}-{engine.bot_name}"
            elif cmd == "version":
                result = ENGINE_VERSION
            elif cmd == "board":
                engine.cmd_board(args_cmd[0])
                result = ""
            elif cmd == "clear_board":
                engine.cmd_clear_board()
                result = ""
            elif cmd == "play":
                if len(args_cmd) != 2:
                    raise RuntimeError("usage: play <color> <move>")
                engine.cmd_play(args_cmd[0], args_cmd[1])
                result = ""
            elif cmd == "genmove":
                if len(args_cmd) != 1:
                    raise RuntimeError("usage: genmove <color>")
                result = engine.cmd_genmove(args_cmd[0])
            elif cmd == "quit":
                print(f"={cmd_id}\n", flush=True)
                return
            elif cmd == "list_commands":
                result = "protocol_version name version board clear_board play genmove quit list_commands"
            else:
                raise RuntimeError(f"unknown command: {cmd}")
        except (RuntimeError, IndexError, KeyError, ValueError) as exc:
            print(f"?{cmd_id} {exc}\n", flush=True)
            continue
        print(f"={cmd_id} {result}".rstrip() + "\n", flush=True)


if __name__ == "__main__":
    main()
