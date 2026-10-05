# ATP — Abalone Text Protocol (v1)

Protocole texte inspiré de GTP (*Go Text Protocol*) pour connecter des moteurs
Abalone hétérogènes (C++, Python/OpenSpiel) à un arbitre de tournoi.

## Transport

- Entrée standard / sortie standard, une commande par ligne (UTF-8).
- Réponses sur deux lignes : en-tête `=` (succès) ou `?` (erreur), puis ligne vide.

```
<command> [id]        →  =[id] [résultat]
                          [ligne vide]
                      →  ?[id] [message d'erreur]
                          [ligne vide]
```

L'identifiant `id` est optionnel : s'il est présent dans la commande, il est
répété dans la réponse.

## Couleurs et coups

- Deux couleurs : `white` (joue en premier, = joueur 0) et `black` (= joueur 1).
- Notation des coups : celle du projet, partagée par les trois écosystèmes —
  `a1b2` (lettre de ligne a-i, numéro de colonne 1-9), ex. `b1a1b2`, `i5h5`.
- Un moteur peut recevoir `genmove` pour n'importe quelle couleur ; il maintient
  l'état complet de la partie et applique lui-même les coups qu'il joue.

## Commandes

| Commande | Réponse | Description |
|---|---|---|
| `protocol_version` | `1` | |
| `name` | nom du moteur | |
| `version` | version | |
| `board <nom>` | vide | Choix du plateau de départ. `classical` partout ; les autres layouts (`belgian-daisy`, ...) selon le moteur — erreur `unsupported board` sinon. Réinitialise la partie. |
| `clear_board` | vide | Réinitialise la partie sur le plateau courant. |
| `play <color> <move>` | vide | Joue un coup (typiquement celui de l'adversaire). Erreur si illégal. |
| `genmove <color>` | coup | Calcule et joue le coup pour `<color>`, renvoie la notation. `resign` si aucun coup. |
| `quit` | vide | Le moteur se termine. |

## Cycle d'une partie (arbitre → moteurs)

```
arbitre → moteur A : board classical
arbitre → moteur A : genmove white        → A joue w1 (appliqué chez A)
arbitre → moteur B : board classical
arbitre → moteur B : play white w1        → B applique w1
arbitre → moteur B : genmove black        → B joue b1
arbitre → moteur A : play black b1
... (alternance)
```

L'arbitre (pyspiel `abalone`) valide chaque coup reçu ; un coup illégal ou une
réponse invalide vaut forfeit. Fin de partie : 6 billes éjectées ou limite de
coups (draw).

## Moteurs fournis

| Moteur | Commande | IA |
|---|---|---|
| `atp-cpp-bb` | `Sources/BitboardAbalone/build/atp_engine_bb [--depth N] [--window N] [--quiescent] [--algo minimax\|abrnd\|absort\|abradix\|abtt] [--budget N] [--tt-mb N]` | solveur bitboard 2 x uint64 : minimax, alpha-beta (ordres aléatoire/trié/radix), alpha-beta avec table de transposition, iterative deepening (`--budget`, ms par coup) ; plateau classical |
| `atp-pyspiel` | `python3 Tournament/atp_pyspiel.py --bot random\|uct [--simulations N]` | bots Python : aléatoire, UCT (MCTS léger, rollouts aléatoires) |

## Tournois

`Tournament/tournament.py` est l'arbitre round-robin (voir
`Tournament/engines_cpp.json`) :

```
python3 Tournament/tournament.py --config Tournament/engines_cpp.json [--games 4]
```

- Chaque paire joue `games_per_pair` parties, couleurs alternées.
- Timeout par coup (`move_timeout` s), coup illégal / `resign` / crash = forfeit.
- Fin de partie : 6 billes éjectées, ou draw sur limite de coups (`move_limit`)
  ou limite interne du jeu pyspiel (200 coups).
- Classement ELO : chaque partie met à jour un ELO (facteur K via `k_factor`
  dans le config ou `--k`, défaut 32 ; draw = 0,5). Les ELO sont chargés puis
  resauvés après chaque partie dans `log/elo.json` à côté du config
  (`--ratings` pour changer le chemin, `--no-ratings` pour désactiver) : un
  tournoi reprend le classement du précédent, un moteur inconnu démarre à
  `initial_elo` (défaut 400). Les parties void (crash des deux côtés) ne
  comptent pas.
- Ouvertures : `random_plies` joue N demi-coups aléatoires via `play` dans
  les deux moteurs avant de leur rendre la main (sinon des moteurs
  déterministes rejouent la même partie) ; `random_seed` rend le tirage
  reproductible.
- Prérequis : `cmake -S Sources/BitboardAbalone -B
  Sources/BitboardAbalone/build && cmake --build
  Sources/BitboardAbalone/build -j`, et `PYTHONPATH` pointant sur le build
  OpenSpiel (clé `env` du moteur Python).

## Notation détaillée

- Poussée en ligne (1 à 3 billes) : `a1b2` = cellule arrière + arrière + un
  offset de direction ; la longueur de la chaîne se déduit du plateau.
- Glissement latéral (broadside) : `a1b2c3` = extrémité + autre extrémité +
  offset depuis la première cellule, la première cellule étant celle dont la
  sœur est à direction+1 ou direction+2 (ordre canonique, seul accepté par
  l'arbitre).
