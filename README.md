# Pokémon Jaune — version PC native / native PC port

**Projet de fans, non affilié à Nintendo, Game Freak, The Pokémon Company ni
à [pret](https://pret.github.io/).** C'est un fork du dépôt
[pret/pokeyellow](https://github.com/pret/pokeyellow) (désassemblage du jeu), dont
l'outillage d'origine leur appartient. pret n'accepte pas le code généré par
IA et ne veut pas y être associé : n'envoyez rien d'ici à pret et ne les
sollicitez pas à son sujet. Le port PC (dossier `pc/`) a été réalisé avec des
agents IA.

(English below)

## Français

Pokémon Jaune qui tourne **nativement sur PC** (Windows, Linux), **sans
émulateur**. Le code du jeu (assembleur Game Boy) est **traduit en C** par
recompilation statique, puis compilé ; le matériel Game Boy Color est reproduit
(écran ligne par ligne, son 4 canaux, manette, sauvegarde).

- **Votre ROM est nécessaire** ; l'exécutable ne contient aucune donnée du jeu.
  ROM reconnues :
  - Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe)
  - Pokemon - Version Jaune - Edition Speciale Pikachu (France)
- **Langue** : anglais et français dans le même exécutable, choisis d'après la
  ROM : `--lang en|fr`, ou dans `pokeyellow.ini` à côté du programme :
  ```
  lang=fr
  rom_en=C:\...\Pokemon - Yellow Version (USA, Europe).gb
  rom_fr=C:\...\Pokemon - Version Jaune (France).gb
  ```
  Autres réglages : `fullscreen=1`, `scale=4`, `save=...`.
- **Commandes** : flèches, X/Espace = A, Z/Ctrl = B, Entrée = Start,
  Retour arrière = Select, Tab = avance rapide, F11 = plein écran ; manette
  reconnue automatiquement.
- **Sauvegarde** : `pokemon_yellow.sav` / `pokemon_jaune.sav` à côté du programme, au format des émulateurs
  (`.sav`) : une sauvegarde d'émulateur peut être reprise.

### Compiler

Il faut rgbds 1.0.x et `make` (pour reconstruire la ROM anglaise et ses
symboles, comme le dépôt d'origine), Python 3, CMake, Ninja, Clang et SDL2.
```
python pc/build.py --rom-fr "<ROM française>"
```
Le C généré n'est pas dans le dépôt : il est produit à la compilation. Les
détails (méthode, outils de test, journal) sont dans
[`pc/PORTING_ROADMAP.md`](pc/PORTING_ROADMAP.md).

## English

Pokémon Yellow running **natively on PC** (Windows, Linux) **without an
emulator**: the game's Game Boy assembly is **statically recompiled to C**
and the Game Boy Color hardware is reproduced (scanline renderer, 4-channel sound,
gamepad, save files).

- **Your own ROM is required**; the executable holds no game data.
  Supported ROMs:
  - Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe)
  - Pokemon - Version Jaune - Edition Speciale Pikachu (France)
- **Language**: English and French in one executable, picked from the ROM
  (`--lang en|fr`, or `lang=`, `rom_en=`, `rom_fr=` in `pokeyellow.ini`).
- **Build**: `python pc/build.py --rom-fr "<French ROM>"` (rgbds 1.0.x, make,
  Python 3, CMake, Ninja, Clang, SDL2). The generated C is not in the
  repository; it is produced at build time.
- Saves: `pokemon_yellow.sav` / `pokemon_jaune.sav`, emulator-compatible `.sav` files.

The original disassembly README is at
[pret/pokeyellow](https://github.com/pret/pokeyellow#readme).
