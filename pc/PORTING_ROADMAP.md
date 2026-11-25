# Pokémon Jaune — port PC natif par recompilation statique

Même méthode et mêmes outils que le port de Cristal
(`../pokecrystal/pc/PORTING_ROADMAP.md`). Rien n'est modifié hors de `pc/`
(sauf `/build/` et `/dist/` dans `.gitignore`) ; `make compare` reste OK.

Particularités : jeu compatible Game Boy Color (tourne en mode couleur avec ses
propres palettes), cartouche **MBC5** (gérée par le runtime), sauvegarde 32 Kio.

## Jalons

| Jalon | Contenu | État |
|---|---|---|
| Outils | ROM Jaune reconstruite identique à celle du joueur (USA/Europe) | ✅ |
| Traducteur | 68 444 instructions, 36 banques, routine DMA en HRAM traduite | ✅ |
| Compilation | Windows | ✅ |
| Démarrage | intro (Pikachu surfeur), titre, NEW GAME, discours de Chen en couleurs | ✅ |
| Contrôle | `GB_DIFF=1` : 187 201 blocs, 0 écart | ✅ |
| Jeu | capture de Pikachu par Chen, Pikachu reçu au labo, combat contre le rival (Évoli) gagné, Pikachu qui suit, sauvegarde, CONTINUE au même endroit | ✅ |
| Français | ROM Jaune (France) traduite directement (7 942 fonctions retrouvées) ; `pokeyellow.exe` anglais + français. Pikachu, « YELLOW a vaincu BLUE ! », sauvegarde, CONTINUER ✅ | ✅ |
| Paquet, Linux | comme Cristal | — |

## Commandes

```
PATH=/d/LUC/pokemonall/tools/rgbds-1.0.4:$PATH mingw32-make -j8 yellow
python pc/tools/recomp/recomp.py --rom pokeyellow.gbc --sym pokeyellow.sym --src . \
       --out build/rc_yellow --name yellow --prefix yellow --lang en
cmake -S pc -B build/pc -G Ninja -DCMAKE_C_COMPILER=clang -DRC_DIRS=build/rc_yellow \
      -DGAME_NAME=pokeyellow -DSDL2_PREBUILT_DIR=<SDL2 statique>
cmake --build build/pc
```

## Journal

- 2026-11-25 : parcours scripté jusqu'à la sauvegarde/rechargement.
- 2026-11-25 : version française ajoutée et testée jusqu'à la sauvegarde/rechargement.
