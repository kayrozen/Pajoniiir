# UI JC1060P470 — Layout 1024×600 et conventions

Statut : layout 1024×600 fonctionnel et validé par les gates simulateur (2026-09).
Branch de travail : `codex/ddj400-jc1060-integration`. Tout le travail UI se fait
uniquement via le simulateur — pas de flash (l'hardware est l'affaire d'un autre
flux de travail).

## Deux layouts authorés

Le code UI supporte **deux layouts authorés** sélectionnés au compile-time :

| Target | Résolution | Topbar | Content H | Activation |
|--------|-----------|--------|-----------|------------|
| P4 (JC4880P443C) | 800×480 | 46 | 434 | défaut |
| JC1060P470 | 1024×600 | 54 | 546 | `-DUI_TARGET_JC1060=1` |

- Les macros de résolution vivent dans `firmware/main-deck-p4/components/ui/ui.c`
  (`UI_HOR_RES`, `UI_VER_RES`, `UI_TOPBAR_H`, `UI_CONTENT_Y`, `UI_CONTENT_H`).
  Le bloc `#ifdef WIN32` (lignes ~51-56) est pour le firmware/PPA ; le simulateur
  passe par `#ifndef UI_HOR_RES` — ne jamais passer `-DUI_HOR_RES` pour le P4
  (ça court-circuite le bloc `#ifndef` et casse `UI_TOPBAR_H`).
- Les positions d'éléments UI sont **relatives au deck panel**, qui commence à
  `UI_TOPBAR_H` (46 ou 54). Une valeur de macro `Y=86` se rend à 86+54=140 absolu
  sur JC1060. C'est LE piège principal : concevoir les valeurs comme absolues
  produit des chevauchements invisibles dans le code.
- Le texte d'un `ui_fx_panel_label` (h=22) se rend ~4px sous le `y` de l'objet.

## Où est le layout

| Écran | Fichier | Mécanisme |
|-------|---------|-----------|
| Overview (waves, VU, info, FX rail) | `ui_overview.c` lignes ~137-310 | bloc de macros `OVERVIEW_*` en double branche `#ifdef UI_TARGET_JC1060` / `#else` |
| Library (table, colonnes, rail) | `ui_library.c` `ui_library_create()` | ternaires `lib_wide = hor_res >= 1000` |
| Hot Cues (grille, status strip) | `ui_performance_tabs.c` `ui_performance_tabs_create_hot_cues()` | ternaires `hc_wide = hor_res >= 1000` |
| Settings (colonnes, mixer bar) | `ui_settings.c` `ui_settings_create()` | variables dérivées de `s_config.hor_res` |
| Header/footer | `ui.c` `create_header`/`create_footer` | dérivés de `UI_HOR_RES`/`UI_TAB_COUNT` |

Valeurs clés JC1060 (relatives au deck panel) :

- Overview : vagues 860×175 par deck (`OVERVIEW_CV_W/H`), mini-vague 502×59,
  colonne info 512 par deck (`OVERVIEW_DECK_INFO_W` — 2×512 = 1024 pile),
  rail FX 64px à x=950, jauge depth 150px.
- `info_x` (deck 2) = `OVERVIEW_DECK_INFO_W` ; `top_y` (deck 2) =
  `OVERVIEW_DECK2_WAVE_Y + 16` — dérivés, ne pas re-hardcoder.
- Library : table 850×430 à (10,46), colonnes TITLE 430 / ARTIST 230 /
  KEY 70 / BPM 55 / TIME 65 (total = largeur table), rail x=880,
  pagination y=484.
- Hot Cues : pads 226×160, status strip 964×62 à y=464, target selector x=410.
- Settings : left 460 / right 474 (gouttière 30), mixer bar 964.

## Gates simulateur

```bash
# P4 800×480 (non-régression — baselines bit-exactes)
./tests/ui_simulator/run_ui_simulator_e2e.sh

# JC1060 1024×600
./tests/ui_simulator/run_ui_simulator_e2e_jc1060.sh              # check
./tests/ui_simulator/run_ui_simulator_e2e_jc1060.sh -UpdateBaselines
```

- Cible JC1060 : `ui_simulator_e2e_jc1060` dans `tests/ui_simulator/CMakeLists.txt`
  (macros `-DUI_TARGET_JC1060 -DUI_HOR_RES=1024 -DUI_VER_RES=600 -DUI_TOPBAR_H=54
  -DUI_CONTENT_Y=54 -DUI_CONTENT_H=546`). Toute nouvelle macro de layout dérivée
  doit être passée ici aussi.
- Baselines : `baselines.json` (800) et `baselines_jc1060.json` (1024).
  Après UN changement visuel assumé : `-UpdateBaselines` puis inspection des PPM.
  Ne jamais baseline-er un rendu non inspecté.
- Les captures PPM sortent dans `.cache/ui_simulator/screenshots_jc1060/`.
  Conversion pour inspection : `python3 -c "from PIL import Image; Image.open('x.ppm').save('x.png')"`.
- Mesure pixel objective (fiable, la vision seule se trompe sur les petites
  zones) : scanner les rows du PPM converti en L avec un seuil ~25.

## Debug

- `UI_SIM_DEBUG=1 <binaire> <dir>` : dump des labels visibles + chaîne
  d'ancêtres du label manquant (hidden, pos, size).
- `UI_SIM_DUMP_FX=1 <binaire> <dir>` : coords absolues du label BEAT + scroll
  du FX panel / deck panel (diagnostic décalages de rendu).
- Erreur type « Missing visible label: X » : un ancêtre est caché ou l'objet
  sort du root clippé (`lv_obj_is_visible` échoue). Voir le fix historique :
  `ui.c` root container hardcodé 800×480 (corrigé en `UI_HOR_RES/VER_RES`).

## Règles pour fonctionnalités futures

1. **Jamais de position en dur hors du bloc de macros** — toute nouvelle
   constante de layout Overview va dans le bloc double-branche (les deux
   branches), avec `_Static_assert` de non-chevauchement si deux éléments
   partagent un axe.
2. **Dériver, ne pas dupliquer** : `OVERVIEW_DECK2_WAVE_Y = CV_H + 1`,
   `PITCH_Y = INFO_ROW_Y`, `WAVE_CENTER_X`, `BEAT_STRIP_TOP_Y` sont dérivés.
   Les macros dérivées restent hors du `#ifdef`.
3. **Vérifier la cohérence horizontale par deck** : `DECK_INFO_W × 2 <=
   UI_HOR_RES` et le rail FX doit rester à droite sans chevaucher.
4. **Static asserts = filet de sécurité** : le build échoue avant tout rendu
   cassé. En ajouter un pour chaque nouvelle contrainte de chevauchement.
5. **Tester les DEUX gates** après chaque changement : le P4 doit rester
   bit-identique (hashes inchangés) sauf si le changement est volontaire.
6. **Inspection visuelle obligatoire** après `-UpdateBaselines` : le gate
   valide navigation + déterminisme, pas l'esthétique. Convertir les PPM et
   vérifier chevauchements/clipping à l'œil + mesure pixel.
7. **Reste connu** : la library UI n'affiche pas plus de rows à 600px de haut
   (hauteur de page fixe) — une future passe pourrait paginer selon la hauteur
   réelle. Les `ui_fx_panel_label` 800-branch gardent leur chevauchement
   historique caption/chip d'1px (design assumé, non corrigé).
