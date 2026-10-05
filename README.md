# AbbaloneCpp

Solveur Abalone en C++ et infrastructure de tournois pour évaluer des IA
hétérogènes entre elles.

Le plateau est représenté par deux `uint64_t` (un par joueur, un bit par
bille). Le projet fournit :

- **6 algorithmes de recherche** : minimax, alpha-beta (ordre aléatoire,
  trié, radix), alpha-beta avec table de transposition (`abtt`), et
  iterative deepening avec fenêtre d'aspiration et budget temps (`--budget`,
  ms par coup) ;
- **un moteur ATP** (*Abalone Text Protocol*, inspiré du GTP du Go) qui
  permet de brancher n'importe quel solveur sur l'arbitre de tournoi ;
- **un framework de tournoi** : round-robin Elo persistant, mode évaluation
  d'un agent (matchmaking par proximité, arrêt sur stabilisation du rating),
  ouvertures aléatoires pour jouer des parties réellement distinctes entre
  moteurs déterministes.

L'arbitre des tournois est le jeu `abalone` de
[OpenSpiel](https://github.com/google-deepmind/open_spiel) : chaque coup
renvoyé par un moteur est validé contre les coups légaux de l'arbitre.

## Prérequis

- Compilateur C++17, CMake >= 3.16 ;
- Python 3 avec le module `pyspiel` d'OpenSpiel (pour les tournois uniquement).

## Build

```
cmake -S Sources/BitboardAbalone -B Sources/BitboardAbalone/build
cmake --build Sources/BitboardAbalone/build -j
```

Produit notamment `Sources/BitboardAbalone/build/atp_engine_bb` (le moteur)
et `bb_bench` (benchmark des algorithmes).

## Tests

Sept suites [doctest](https://github.com/doctest/doctest) (plateau et
génération des coups, évaluation, recherche, table de transposition,
iterative deepening, protocole ATP, perft validé croisé contre le cœur de
référence `Sources/SpielAbalone`) :

```
ctest --test-dir Sources/BitboardAbalone/build --output-on-failure
```

## Utiliser le moteur

Le moteur parle l'ATP sur l'entrée/sortie standard (voir
[`Tournament/ATP.md`](Tournament/ATP.md) pour le protocole complet) :

```
echo -e "board classical\ngenmove white\nquit" \
  | Sources/BitboardAbalone/build/atp_engine_bb --depth 4 --algo abtt
```

Options principales : `--depth N`, `--algo minimax|abrnd|absort|abradix|abtt`,
`--budget N` (iterative deepening, N ms par coup), `--tt-mb N` (taille de la
table de transposition).

## Utilisation depuis Python

Le solveur est packagé comme module Python (`abbalone-cpp`, bindings pybind11
compilant le cœur bitboard sans le dupliquer) :

```
pip install /chemin/vers/AbbaloneCpp
```

Depuis le `requirements.txt` d'un autre projet :

```
abbalone-cpp @ file:///chemin/vers/AbbaloneCpp
```

(ou `-e /chemin/vers/AbbaloneCpp` pour une installation éditable, ou
`abbalone-cpp @ git+https://.../AbbaloneCpp.git` pour un dépôt distant ;
prérequis : compilateur C++17 et CMake, la compilation se fait à
l'installation).

```python
import abbalone_cpp as ab

board = ab.Board.classical()          # joueur 0 = blanc, 1 = noir
len(board.legal_moves(0))             # 44

solver = ab.Solver(algo="abtt", depth=4)          # profondeur fixe
# ou : ab.Solver(budget_ms=200, depth=20)         # iterative deepening
move = solver.best_move(board, player=0)          # Move ou None
ab.move_to_atp(board, 0, move)                   # 'a1b2' (notation ATP)
board.apply(move, 0)                             # sur place, lève ValueError si illégal
```

API : `Board` (`classical()`, `legal_moves`, `is_legal`, `apply`, `evaluate`,
`copy`, masques `p0`/`p1`), `Solver` (`best_move`, compteurs `nodes`/`leaves`,
`last_depth`/`last_score` du mode budget), `parse_atp`/`move_to_atp`
(notation ATP, identique à celle du jeu pyspiel `abalone`), `perft`,
`cell_name`/`cell_from_name`. La recherche garde le GIL : pour paralléliser,
utilisez des processus (un `Solver` par processus).

## Tournois

Le tournoi fait s'affronter les moteurs d'un config JSON
(`Tournament/engines_cpp.json` : les variantes cpp-bb et les bots pyspiel),
met à jour un classement Elo persistant et l'affiche :

```
PYTHONPATH=<chemin du build OpenSpiel> \
  python3 Tournament/tournament.py --config Tournament/engines_cpp.json
```

- `--games N` : parties par paire (couleurs alternées) ;
- `--evaluate NAME` : évalue un seul agent contre ses plus proches voisins
  au classement, jusqu'à ce que son rating se stabilise
  (`--target-se`, `--max-rounds`, `--opponents`) ;
- `random_plies` (config) : demi-coups d'ouverture aléatoires — sans cela,
  des moteurs déterministes rejouent la même partie ; `random_seed` rend le
  tirage reproductible ;
- les Elo sont sauvegardés après chaque partie dans
  `Tournament/log/elo.json` : un tournoi reprend le classement du précédent.

## Organisation

```
Sources/BitboardAbalone/   solveur bitboard (cœur, recherche, TT, ID, ATP)
Sources/SpielAbalone/      cœur de référence (validation croisée du perft)
python/                    bindings pybind11 (package abbalone-cpp)
Tournament/                arbitre de tournoi, moteur ATP pyspiel, configs
```
