# Tester et déboguer les ports Game Boy (Cristal, Or/Argent, Rouge/Bleu, Jaune)

Ce guide est commun aux quatre dépôts (`pokecrystal`, `pokegold`, `pokered`,
`pokeyellow`) : même traducteur, même runtime, mêmes outils (`pc/`). Les
exemples utilisent Cristal ; remplacer l'exécutable, la ROM et le `.sym`.

## 1. Lancer le jeu

```
build/pc/pokecrystal.exe [--lang fr|en] [--rom-dir DOSSIER] [--fullscreen] [--scale N] [ROM]
```

Recherche de la ROM : chemin donné → dossier du programme → `rom_dir` (du
`.ini`, de `--rom-dir` ou de `GB_ROM_DIR`). Le fichier est reconnu à son
contenu (CRC de toute la ROM : version et langue), jamais à son nom.
Fichier `<jeu>.ini` à côté du programme : `rom_dir`, `lang`, `rom_en`,
`rom_fr`, `fullscreen`, `scale`, `save`, `palette` (1re génération :
`gbc`, `green`, `gray`). Les commentaires (`#` ou `;` après un espace) et les
guillemets sont acceptés.

## 2. Variables d'environnement (tests)

| Variable | Effet |
|---|---|
| `GB_HEADLESS=1` | sans fenêtre ni son (tests automatiques) |
| `GB_FAST=1` | sans limitation de vitesse |
| `GB_MUTE=1` | muet |
| `GB_FRAMES=n` | quitter après n images |
| `GB_SHOT=f:a.bmp,f2:b.bmp` | captures aux images données |
| `GB_INPUT=f=touches;f2=…` | boutons tenus à partir de l'image f (`a b s t u d l r` ; vide = relâcher) |
| `GB_SAVE=fichier.sav` | fichier de sauvegarde |
| `GB_STATE_SAVE=f:état.bin` | état complet de la machine à l'image f |
| `GB_STATE_LOAD=état.bin` | repartir d'un état |
| `GB_STATE_WHEN=ADDR:VAL:état.bin` | état + arrêt dès que mémoire[ADDR] = VAL (ex. début de combat) |
| `GB_POKE=f:ADDR=octets;…` | écrire en mémoire à l'image f |
| `GB_POKE_KEEP=ADDR=octets;…` | réécrire à chaque image (« code de triche » : PV, PP, statut) |
| `GB_DIFF=1` | chaque bloc traduit est rejoué par l'interpréteur et comparé (vérifie le traducteur) |
| `GB_INTERP=1` | tout dans l'interpréteur (référence ; interruptions servies à chaque instruction) |
| `GB_ITRACE=de:à` | trace instruction par instruction (mode interprété) |
| `GB_WATCHPC=hex` | registres quand l'aiguillage passe par cette adresse |
| `GB_PCLOG=1` | pc, banque, LCDC, IE/IF à chaque image |
| `GB_WAV=son.wav` | enregistrer le son |

Les adresses non traduites (passages par l'interpréteur) sont écrites dans
`rc_missing.txt` ; on les ajoute à `pc/roots_*_runtime.txt` puis on relance
`recomp.py`.

## 3. Parcours scriptés : `pc/tools/play.py`

```
python pc/tools/play.py --exe build/pc/pokecrystal.exe --rom "<ROM>" --sym pokecrystal.sym \
       --dir build/run [--load état.bin --start IMAGE] [--save test.sav] "étapes"
```

Étapes séparées par `;` :

| Étape | Effet |
|---|---|
| `wait N` | laisser passer N images |
| `press T [N]` / `hold T N` / `mash T N` | appuyer / tenir / marteler une touche |
| `shot NOM` / `save NOM` | capture NOM.bmp / état NOM.bin |
| `poke ÉTIQUETTE=octets,…` | écrire en mémoire (étiquettes du `.sym`) |
| `keep ÉTIQUETTE=octets,…` | réécrire à chaque image pendant tout le parcours |
| `warp G N X Y` | 2e génération : entrer sur la carte G:N en X,Y comme la commande de script `warp` |

Les captures sont assemblées dans `<dir>/sheet.png` (`pc/tools/shots.py`).

## 4. Lire et modifier l'état du jeu

- `pc/tools/peek.py état.bin GAME.sym wMapGroup wXCoord wPartyMon1:48` : lire
  des variables dans un état.
- `pc/tools/savedit.py SAVE.sav GAME.sym get|set …` : lire / modifier une
  sauvegarde, checksum recalculé (2e gén. : somme 16 bits ; 1re gén. : somme
  8 bits complémentée). `teleport1` (1re gén.) existe mais la reprise ne
  recharge pas tout l'état de la carte : préférer le vol ci-dessous.

### Se téléporter

- **2e génération** (Cristal, Or, Argent) : `warp G N X Y` dans `play.py`
  (écrit `wMapGroup`, `wMapNumber`, `wXCoord`, `wYCoord`,
  `wDefaultSpawnpoint = FF`, `hMapEntryMethod = MAPSETUP_WARP`,
  `wMapStatus = MAPSTATUS_ENTER`, comme le fait le jeu). Ligue : salle de
  Clément = `warp 0x10 3 5 17`.
- **1re génération** (Rouge, Bleu, Jaune) : le vol. `poke
  wDestinationMap=09,wStatusFlags6=09` (Plateau Indigo, bit BIT_FLY_WARP).
  Dans le hall du Plateau, chemin praticable calculé depuis les données du
  jeu : depuis (7,7) gauche jusqu'à x=3, haut jusqu'à y=2, droite jusqu'à
  x=8, haut.

### ROM françaises

- 2e génération : la mémoire de travail est disposée comme en anglais ; le
  `.sym` anglais sert tel quel.
- 1re génération : décalage de **+5 octets** à partir de `wPlayerName`
  (D158 en Rouge, D157 en Jaune) jusqu'à la fin de la WRAM. Fichier de
  symboles français : décaler de 5 les étiquettes de banque 00 dans cette
  zone (`build/pokered_fr_wram.sym`). Les sauvegardes françaises ont aussi
  une autre disposition : ne pas utiliser `savedit.py` dessus.

### Équipe de test

Niveau 100 cohérent : `wPartyMon1Level=64`, l'**expérience du niveau 100**
(`wPartyMon1Exp`, sinon le jeu recalcule le niveau après un K.O.), PV/stats
à 03E7 (999), attaques et PP (`3f`). En combat, `keep` sur `wBattleMonHP`,
`wBattleMonPP`, `wBattleMonStatus` (et `wPlayerSubStatus1..5` en 2e gén.)
évite de perdre contre le sommeil, la confusion, etc. Attention aux types
(Pikachu contre l'Onix d'Aldo) et aux attaques qui bloquent (Claquoir).

## 5. Résultats (2026-11-26)

| Jeu | Anglais | Français |
|---|---|---|
| Cristal | début → Route 29, combat, sauvegarde/CONTINUE ; Ligue → Panthéon → générique → retour à Bourg Geon ; Pokédex, PC, salle de connexion, échange PNJ (Kyle), horloge jour/nuit | début ; Ligue → Panthéon → « FIN » |
| Or | début → combat → sauvegarde/CONTINUE ; Ligue → « THE END » | début → combat ; Ligue → « FIN » |
| Argent | jusqu'à la chambre | Ligue jusqu'à Marion (3 membres battus) |
| Rouge | début → rival → sauvegarde/CONTINUE ; Ligue → Maître → « THE END » | rival → SAUVER/CONTINUER ; Ligue → Maître → « FIN » |
| Bleu | vol vers le Plateau, combat contre Olga | jusqu'à la chambre |
| Jaune | début → rival → sauvegarde/CONTINUE ; Ligue → Maître → « THE END » | rival → sauvegarde/CONTINUER ; Ligue → Maître → générique |

`GB_DIFF=1` : 0 écart entre code traduit et interpréteur sur les 6 jeux.
Linux (WSL) : images identiques pixel pour pixel à Windows.
