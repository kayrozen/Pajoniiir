# Plan de migration UI : `dj_ui` sur le JC1060

Statut : phases 0 à 6 réalisées (v250–v293, 2026-09-25 → 2026-09-27).
`dj_ui` est la seule couche de présentation du JC1060 ; l'ancien code de
widgets et le flag `CONFIG_UI_PRESENTATION_DJUI` sont retirés (phase 6).
L'acceptance HIL complète reste ouverte (voir phase 6).
Cible : `firmware/main-deck-jc1060` (ESP32-P4, 1024x600, LVGL 9.6.0~1 selon
`dependencies.lock`). La base de départ est v249 (NFS peer fetch).

Sources analysées :

- `docs/recherche/lvgl/dj_ui.h`, `dj_ui.c` (935 lignes), `dj_ui_demo.c` et
  `README.md`. Le README décrit un LVGL v9, 1024x600, 4 onglets et une API
  `dj_ui_set_*` avec des callbacks.
- `firmware/main-deck-jc1060/components/ui` : 41 034 lignes au total. Sur ce
  total, 26 941 sont les polices générées `Musieer_48.c` et `Musieer_80.c`
  (splash), il reste donc environ 14 100 lignes écrites à la main.
- `tests/ui_simulator/run_ui_simulator_e2e.sh` et
  `run_ui_simulator_e2e_jc1060.sh`, `CMakeLists.txt`, `baselines_jc1060.json`.
- Les suites hôte `tests/ui_*`, dont `ui_overview_wave_cache`,
  `ui_position_interpolator`, `ui_load_gate`, `ui_idle` et `ui_library`.

---

## 0. Constat préalable sur le simulateur

`tests/ui_simulator/CMakeLists.txt` fixe
`UI_DIR = firmware/main-deck-p4/components/ui` pour **les deux** cibles. La
cible `ui_simulator_e2e_jc1060` compile donc les sources **P4** avec les
défines JC1060 (`UI_HOR_RES=1024`, `UI_VER_RES=600`, `UI_TOPBAR_H=54`…). Elle
ne compile pas le code réellement flashé sur le JC1060.

Pourtant les deux arbres divergent : `diff` compte 758 lignes sur
`ui_library.c`, 588 sur `ui_settings.c`, 203 sur `ui_overview.c`, 96 sur
`ui_overview_renderer.c`, 83 sur `ui_position_interpolator.c` et 83 sur
`ui_overview_wave_cache.c`. `ui_hot_cue_view.c` n'existe que côté JC1060.

Conséquence : `baselines_jc1060.json` fige des captures d'une UI qui n'est pas
celle du JC1060. Le gate peut passer alors que l'UI JC1060 réelle a régressé.
Il faut corriger ça **avant** la migration (phase 0). Sinon aucune phase ne
peut être validée par le simulateur.

Deuxième écart : le simulateur est épinglé sur le commit LVGL `263ae5e…`,
alors que le firmware résout `lvgl/lvgl 9.6.0~1`. Des différences de rendu
(anti-aliasing, kerning, `LV_LABEL_LONG_MODE_DOTS` à partir de 9.3) peuvent
faire diverger les captures du simulateur de ce qu'affiche le panel. `dj_ui.c`
gère déjà ce cas à la compilation avec `#if LVGL_VERSION_MINOR >= 3`.

---

## 1. Inventaire fonctionnel

Légende :

- **OK** : `dj_ui` le couvre de façon équivalente.
- **Partiel** : `dj_ui` a le widget, mais pas la donnée ou le comportement.
- **Absent** : il faut l'ajouter à `dj_ui`, ou garder notre code.
- **Doublon** : les deux le font ; il faut en choisir un.

### 1.1 Overview (onglet 1)

| Fonction | Notre UI (fichiers) | `dj_ui` | Verdict |
|---|---|---|---|
| Waveform zoomée | `ui_overview_wave_cache.c` : cache en bande RGB565 avec marge de 128 px, scroll et colonnes de bord seulement, stats. Le renderer (`ui_overview_renderer.c`) applique la palette ANLZ PWV couleur. Blit PPA direct (`ui_lvgl_backend_blit_rgb565_ppa270_region`, rotation 0 sur JC1060) qui contourne LVGL. | `zoom_draw` en `LV_EVENT_DRAW_MAIN_END` : environ 175 barres × 2 `lv_draw_rect` par deck **à chaque frame**, fenêtre fixe de 8 s, deux couleurs (peak/core). | **Doublon, on garde le nôtre**. Le rendu `dj_ui` est incompatible avec notre budget (voir R2). |
| Zoom de la waveform | `ui_overview_zoom_delta()`, pilotable depuis le contrôleur. `ui_overview_window.c`. | Absent (`ZOOM_WINDOW_MS` en dur). | Absent : on garde le nôtre. |
| Couleurs ANLZ (PWV / PWV3-5), fallback LOW | `ui_waveform_model.c` (source LOW/HIGH, palette par sample) | Absent : `peaks`/`cores` 0..255 uniquement. | Absent. |
| Beat grid | `ui_overview_grid.c` : grille ANLZ réelle (tempo variable), cap de beat en bas. | Grille calculée à partir de `bpm` + `first_beat_ms` (tempo constant). | Partiel. Faux pour les pistes à tempo variable, on garde le nôtre. |
| Marqueurs de hot-cue sur la zoomée | Liste fusionnée pads locaux + ANLZ (v244), incrustée dans la bande. | Absent (seulement sur la mini). | Absent. |
| Cue point CDJ (triangle jaune) et memory cue rekordbox | v241/v242, `cue_point_burn_*`. | Absent. | Absent. |
| Région de loop active (ambre) et loop armée | `ui_overview_wave_cache_set_loop`, `armed_loop_burn_*`. | Absent. | Absent. |
| Playhead | Incrusté dans la bande avant le blit PPA (sinon LVGL l'efface). | Barre blanche `fill()` redessinée. | Doublon, on garde le nôtre. |
| Mini-waveform (piste entière) | `ui_overview_renderer_draw_mini` dans un canvas, invalidation limitée à une plage (`invalidate_mini_wave_range`). | `overview_draw` : max par bin, **invalidation de tout l'objet à chaque `set_position`**. | Doublon. On garde le nôtre ; on reprend le style `dj_ui` (fond, épaisseur des barres). |
| Seek tactile sur la zoomée et la mini | `waveform_seek_event_cb`, `mini_waveform_seek_event_cb`. | Absent. | Absent. |
| Interpolateur de position | `ui_position_interpolator.c` : ancre + vitesse (pitch), monotone pendant la lecture, avance max 33 ms. | Absent : `dj_ui_set_position(pos_ms)` est brut. | Absent. Il **alimente** `dj_ui`. |
| Scheduler et motion | `ui_overview_scheduler.c` (budget de redessin selon les decks en lecture, alternance), `ui_overview_motion.c` (snap au pixel, pas de redessin inutile). | Absent. | Absent : on garde. |
| Compteurs de perf | `ui_overview_perf.c` (cache, PPA, msync). | Absent. | Absent : on garde. C'est l'outil de mesure de la phase 5. |
| Infos deck : titre, artiste | Oui | Oui (footer), avec points de suspension (DOTS). | OK. Le style `dj_ui` est meilleur. |
| Carte info : source, `n/N`, KEY, TEMPO, BPM | Partiel : pas de key au niveau du deck, pas de `n/N`. | Oui. | **Gain `dj_ui`**. La key vient de `rekordbox_pdb` (`key[8]`) / `media_catalog` (`key[16]`). |
| Temps écoulé et restant | Oui (`format_elapsed/remaining`) | Oui (restant, et `00:00.0` dans la zoomée) | Doublon mineur. On garde notre formatage. |
| BPM effectif et pitch % | Oui (centi-percent, `speed_permille`) | Oui (float) | Doublon. `dj_ui_set_tempo` est alimenté par notre calcul. |
| Master Tempo | Bouton + label | Absent | Absent. |
| Boutons PLAY/PAUSE et CUE, sélection du deck actif | Oui | Absent | Absent (ajouter à `dj_ui` ou garder). |
| 8 pads de hot-cue par deck | Via Hot Cues ou Performance | Oui (A–H, couleur) | Partiel. `dj_ui` a une palette de 8 index ; rekordbox donne du RGB (voir 1.3). |
| Panneau FX | Effet, couleur de l'effet, cible (pills), beat, temps, niveau | Nom, CH, beat 1/4–2, temps, niveau, ON | Doublon. Il faut mapper nos 4 effets (Filter/Echo/Flanger/Delay) et les pills de cible. |
| Indicateur de beat / beat strip | `ui_beat_indicator.c`, beat strip | Absent | Absent. |
| VU et état du mixer | `ui_overview_update_vu_meter`, `ui_mixer_view.c` | Absent (seulement des chips de texte dans Settings) | Absent. |
| Progression de chargement moteur (`ae_loading`, `ae_load_pct`) | Oui | Absent | Absent. |
| Pochette 34x34 | Absente (aucun pipeline d'artwork) | Oui (`dj_ui_set_artwork`) | Gain possible plus tard. Le placeholder « ART » suffit en attendant. |
| Récupération après le retour d'onglet (réarmement du reblit PPA) | `ui_overview_note_screen_restored`, `arm_all_wave_reblits` | N/A | Il faut garder ce mécanisme. |

### 1.2 Library (onglet 2)

| Fonction | Notre UI (`ui_library.c`, 2 596 lignes) | `dj_ui` | Verdict |
|---|---|---|---|
| Table TITLE / ARTIST / KEY / BPM / TIME | `lv_table` avec entête séparé | 8 lignes fixes × 5 labels, surlignage des pistes chargées D1/D2 | Doublon. On garde le rendu `dj_ui`. |
| Pagination | Oui | PREV / NEXT et `set_info(source,total,page,pages)` | OK. |
| Tri ARTIST / NAME / BPM / KEY | Tri local, catalogue | Boutons + callback `on_lib_sort` (le tri reste chez l'appelant) | OK côté vue. On garde notre tri. |
| Navigation au contrôleur (`ui_library_select_delta`, `load_selected[_for_deck]`) | Oui | Tactile seulement | Absent. `dj_ui` a besoin d'un `set_selected(row)` piloté par l'extérieur (le `g.sel` interne existe déjà). |
| Load gate (id, annulation, un chargement à la fois) | `ui_load_gate.c`, worker `ui_submit_track_load` | Absent | Absent. Logique à garder telle quelle. |
| Messages temporisés (`status_hold`, `D1 LOADED…`, refus) | Oui | `set_deck_status(deck, text)` | Partiel. Le texte passe, pas la couleur ni la durée. |
| Source LOCAL / `USB <nom> #n` (DJ Link, v248) | Bouton SOURCE en cycle | `set_info(source,…)` en texte seulement | Absent : il faut un bouton SOURCE et son callback. |
| Badges de ligne `NET`, `META` (ambre), `DB`, `NN%` (v249) | Oui | Absent | Absent : `dj_ui_library_set_badge(row, text, tone)`. |
| Téléchargement NFS : progression, `CANCEL` (v249) | Oui | Absent | Absent. Le bouton SOURCE devient CANCEL. |
| Deck actif et cible de chargement | `s_active_deck_indicator` | Carte DECK n + `LOAD DECK 1/2` | Doublon. On garde le rendu `dj_ui`. |
| Rafraîchissement après remontage USB, notification de retrait | `ui_refresh_library`, `ui_notify_usb_removed` | Absent | Absent : on garde. |

### 1.3 Hot Cues (onglet 3)

| Fonction | Notre UI (`ui_performance_tabs.c`, `ui_hot_cue_view.c`) | `dj_ui` | Verdict |
|---|---|---|---|
| Grille de 8 cartes, cible D1/D2 | Oui | Oui (grandes cartes 244x212) | Doublon. On garde le rendu `dj_ui`. |
| Couleur des cues | RGB ANLZ / rekordbox | Index 0..7 (`CUE_COL`) | Partiel. Il faut `dj_ui_set_hotcue_rgb()` ou une quantification vers 8 couleurs. |
| Loop cues, loop shadow, beat jump (`calculate_jump_target`) | Oui | Chip de texte « LOOP CUES » seulement | Absent : on garde notre logique. |
| Seeding des hot-cues rekordbox et cues locales (v243–v245) | Oui | N/A (donnée) | On garde. `dj_ui` ne fait qu'afficher. |
| Chips d'état (CUES, LOOPS, ANLZ, TARGET) | Partiel | Oui (`set_field`) | Gain `dj_ui`. |

### 1.4 Settings (onglet 4)

| Fonction | Notre UI (`ui_settings.c`, 1 048 lignes) | `dj_ui` | Verdict |
|---|---|---|---|
| Luminosité | `app_settings.backlight_pct` (10..100), NVS avec debounce | Slider 5..100 + `on_brightness` | Doublon. Ramener le minimum du slider à 10. |
| Master trim (presets en cycle) | `ui_settings_master_trim_*` | Champ de texte `DJ_F_MASTER` sans action | Partiel. Il manque le bouton ou le callback. |
| Wi-Fi remote | Callback `ui_settings_set_wifi_toggle_cb` | Switch + `on_wireless` | OK. |
| DJ Link on/off (v246) | Callback toggle | Absent | Absent. |
| Enregistrement master | Callback toggle | Absent | Absent. |
| Libellés contrôleur dynamiques selon le profil (v245) | Oui | Champ `DJ_F_SYS_CONTROLLER` | OK en texte ; nous gardons la source. |
| SD : montée, espace libre, SD log, dernier reset, FW / OTA | Oui (`refresh_storage`, poll) | Champs `DJ_F_SYS_*` | OK en texte. La logique de poll reste chez nous. |
| MAIN / CUE output, statut mixer | Oui | Champs + chips | OK. |
| Timeout du screensaver | Oui (`ui_idle_set_timeout`) | Absent | Absent. |

### 1.5 Transverse

| Fonction | Nous | `dj_ui` | Verdict |
|---|---|---|---|
| Barre de navigation | Top bar de 54 px (`ui_status.c` : état contrôleur, SD, etc.) + boutons d'onglet | 4 boutons d'onglet en haut (y 8..40) | Doublon. Il faut décider où va le statut de `ui_status` (proposition : à droite de la rangée d'onglets, ou dans des chips). **Décidé (phase 6, v293)** : la rangée d'onglets `dj_ui` de 40 px et sa ligne de statut remplacent la top bar ; `ui_status.c` est supprimé. |
| Screensaver | `ui_idle.c` (logique testable en hôte, inhibée pendant lecture et enregistrement, le réveil consomme l'événement) | Absent | On garde. |
| Splash | `splash_screen.c` + polices Musieer | Absent | On garde. |
| Backend LVGL (tâche, lock, callback par frame, PPA, DMA) | `ui_lvgl_backend.c` | N/A (`lv_lock()` générique) | On garde. Tous les appels `dj_ui_*` se font depuis `ui_update()`. |
| Snapshot par frame | `ui_frame_context_t` | N/A | On garde. C'est l'entrée du pont. |
| LEDs du deck actif, performance target | `ui_active_deck_leds.c`, `ui_performance_target.c` | N/A | On garde. |
| Polices | Montserrat 12–28 (ASCII) | Montserrat 12–32 (ASCII) ; ajouter `LV_FONT_MONTSERRAT_32` | Même problème des deux côtés (voir R1). |

### 1.6 Bilan

- **Ce que `dj_ui` apporte vraiment** : la mise en page 1024x600, la hiérarchie
  visuelle, les tokens de couleur/style, une API `set` + callbacks
  propre, la tonalité au niveau du deck, le compteur `n/N`, la pochette, les
  chips d'état.
- **Ce que `dj_ui` n'a pas**, soit une vingtaine de fonctions dont la plupart sont
  critiques :
  - tout le pipeline waveform performant (cache, PPA, zoom, ANLZ couleur,
    grille réelle, loops, cues sur la zoomée, cue jaune, seek) ;
  - l'interpolateur, le scheduler, le load gate ;
  - la navigation au contrôleur ;
  - DJ Link (source, badges, fetch, CANCEL) ;
  - les toggles DJ Link et enregistrement ;
  - screensaver, splash, VU, beat indicator, PLAY/CUE/MT ;
  - la progression de chargement.
- **Doublons à trancher** : waveform zoomée, mini-waveform, playhead, carte
  info, panneau FX, table library, grille hot-cues, tabs et top bar, formatage
  du temps et du BPM.

---

## 2. Stratégie

### Options

1. **Remplacement complet** (brancher `dj_ui_create()` et supprimer `ui_*`) :
   rejetée.
   - On perd immédiatement la vingtaine de fonctions de 1.6, dont le rendu
     waveform qui tient le budget P4.
   - Les suites hôte (`ui_overview_wave_cache`, interpolateur, load gate, idle,
     library) ne testeraient plus rien de ce qui tourne.
   - Il faudrait tout reconstruire dans `dj_ui`, c'est-à-dire réécrire nos
     14 k lignes dans un autre style, sans gain.
2. **Garder notre UI et la restyler façon `dj_ui`** (reprendre couleurs et
   géométrie à la main dans `ui_overview.c` etc.) : possible, mais on garde
   des fichiers de 2,6 k lignes qui mélangent widgets, état et logique. On
   n'obtient pas la séparation « présentation / données » qui est l'intérêt
   de `dj_ui`.
3. **Recommandée : nos pipelines de données restent, `dj_ui` devient la couche
   de présentation**, adoptée onglet par onglet.
   - `dj_ui` est copié dans le firmware comme **notre** composant, et on le
     modifie librement.
   - Un pont `ui_djui_bridge.c` traduit `ui_frame_context_t` en appels
     `dj_ui_set_*`, et les callbacks `dj_ui` en nos actions existantes
     (deck_core, `ui_submit_track_load`, `dj_link_fetch_*`, hot_cue_store,
     app_settings).
   - La waveform zoomée n'est **pas** dessinée par `dj_ui`. Son objet devient
     un emplacement (rectangle) dont les coordonnées alimentent
     `ui_overview_wave_overlay_rect()`. Notre cache et le blit PPA y écrivent
     comme aujourd'hui.

### Justification de l'option 3

- **Performance.** Le goulot est la waveform. Notre cache ne rend que les
  colonnes de bord et fait 1 à 2 blits PPA hors de LVGL. `dj_ui` invalide
  deux zones par deck à chaque position (≈ 698×143 + 479×90 px, soit
  ≈ 143 k px par deck et ≈ 286 k px par frame pour 2 decks). À 30 Hz, LVGL
  doit alors rasteriser en logiciel ≈ 8,6 Mpx/s avec ≈ 700 draw tasks par
  frame. Le README de `dj_ui` affirme que ça passe sur le P4 sans l'avoir
  mesuré ; nos compteurs `ui_overview_perf` montrent pourquoi on a quitté ce
  modèle.
- **Testabilité.** Les modules purs (cache, renderer, interpolateur, motion,
  scheduler, load gate, idle, pdb, dj_link) gardent leurs tests hôte. `dj_ui`
  s'ajoute comme vue, testée au simulateur par capture.
- **Risque incrémental.** Chaque onglet bascule seul derrière une option
  Kconfig, avec retour arrière immédiat. L'Overview, le plus risqué, passe
  en dernier.
- **Pourquoi pas « l'inverse »** (`dj_ui` pilote, nos modules en plugins) :
  `dj_ui` n'a ni cycle de frame, ni snapshot, ni notion de tâche. C'est
  `ui_update()` qui porte l'ordonnancement (règle AGENTS : les commandes de
  browse/load s'exécutent dans `ui_update()`). Le pilote doit rester chez
  nous.

### Règles de la couche `dj_ui`

1. **Emplacement.** Copier `dj_ui.[ch]` dans
   `firmware/main-deck-jc1060/components/ui/` (ou dans un composant
   `ui_djui`). `docs/recherche/lvgl/` reste une référence figée, jamais
   modifiée. `dj_ui_demo.c` n'est pas compilé en firmware ; il sert
   seulement de cible de démo au simulateur.
2. **Découpage par onglet.** Découper `dj_ui_create()` en
   `dj_ui_create_overview/library/hotcues/settings(parent)`, pour que notre
   `ui.c` (tab host, screensaver, splash) héberge chaque page pendant la
   transition.
3. **Données non copiées.** Aucun buffer de données n'est copié par `dj_ui` :
   les textes passent par les labels LVGL, les waveforms ne passent pas par
   `dj_ui` (point 5).
4. **Thread.** Tous les appels se font depuis `ui_update()` (tâche LVGL). On
   n'utilise pas `lv_lock()` depuis d'autres tâches ; les autres tâches
   passent par les flags et files existants.
5. **Waveforms.** `dj_ui_set_waveform()` est retirée en firmware (ou réservée
   au simulateur et à la démo). La zoomée et la mini sont rendues par nos
   modules.
6. **Positions.** `dj_ui_set_position()` ne met plus à jour que les labels de
   temps (au dixième de seconde) et n'invalide plus la zoomée ni la mini.
7. **Polices.** Les macros `F12`…`F32` pointent vers notre jeu de polices
   (voir R1).

---

## 3. Phases

Chaque phase se termine par trois gates :

- **(a) Build JC1060.** `idf.py build` en ESP-IDF 6.0.2, avec `PROJECT_VER`
  incrémenté par l'opérateur. Le build ne doit pas modifier
  `dependencies.lock`.
- **(b) Simulateur.** `./tests/ui_simulator/run_ui_simulator_e2e_jc1060.sh`
  vert, avec les baselines régénérées seulement après revue visuelle
  (`-UpdateBaselines -KeepArtifacts`). Les suites hôte `tests/ui_*` doivent
  rester vertes.
- **(c) HIL.** Checklist sur le JC1060 avec la DDJ-400, deux decks en lecture,
  pour vérifier l'absence de dropout audio et la fluidité. Les compteurs
  `ui_overview_perf` sont relevés à partir de la phase 5.

Les efforts sont en jours-personne, tests compris ; le temps de HIL et
d'attente matérielle n'est pas compté.

### Phase 0 : fiabiliser le simulateur JC1060 (2–3 j)

- `tests/ui_simulator/CMakeLists.txt` : la cible JC1060 compile
  `firmware/main-deck-jc1060/components/ui` (même liste que le `CMakeLists` du
  composant) au lieu des sources P4.
- Ajouter les stubs manquants :
  - `dj_link` : browse et fetch (statuts scriptables) ;
  - `media_io_gate`, `sd_io_gate`, `controller_profile_manager`, `bsp_jc1060p470` ;
  - `app_settings` JC1060.
- Aligner le commit LVGL du simulateur sur le tag correspondant à `9.6.0`
  (celui que résout le firmware), ou documenter l'écart s'il est impossible.
- Ajouter des captures :
  - `library_peer` (source DJ Link, badges `NET` / `META` / `NN%`) ;
  - `library_fetch` (bouton CANCEL) ;
  - `overview_loaded` (fixtures ANLZ, avec hot-cues, cue jaune et loop).
- Ajouter une cible `dj_ui_demo_sim` qui capture les 4 onglets de `dj_ui` avec
  `dj_ui_demo.c`. C'est une référence visuelle, sans hash bloquant.
- Régénérer `baselines_jc1060.json` après revue.
- Pas de changement firmware. Gate (a) : build inchangé. Pas de HIL.

### Phase 1 : intégrer `dj_ui` comme composant, sans effet visible (2 j)

- Copier `dj_ui.[ch]` et le découper par onglet (règle 2).
- Extraire les tokens (couleurs `C_*`, `DECK_COL`, rayons) dans un
  `dj_ui_theme.h` partagé avec nos modules. `ui_overview.c` en profite, même
  avant sa migration.
- Brancher les polices (R1) et activer `LV_FONT_MONTSERRAT_32` ou son
  équivalent.
- Squelette de `ui_djui_bridge.c` et options Kconfig
  `PAJONIIIR_UI_DJUI_{SETTINGS,HOTCUES,LIBRARY,OVERVIEW}` à `n` par défaut.
- Gates :
  - (a) build avec toutes les options à `n` et avec toutes à `y` (onglets vides
    tolérés) ;
  - (b) baselines inchangées quand tout est à `n` ;
  - (c) boot et smoke rapide.

**Réalisé (v250, 2026-09-25)** — écart assumé avec le plan ci-dessus :

- Un seul flag `CONFIG_UI_PRESENTATION_DJUI` (`components/ui/Kconfig`, `n` par
  défaut) au lieu des options par onglet. Avec `y`, `ui_init()` ne construit
  aucun widget legacy (pas de double allocation) ; catalogue, pipeline de
  chargement (`ui_library_update()` sans table), commandes UI de `deck_core`,
  idle, control_link et audio tournent à l'identique. Les options par onglet
  arrivent avec les phases 2 à 5.
- `dj_ui.[ch]` copiés tels quels dans `components/ui` (pas encore découpés, ni
  `dj_ui_theme.h`) ; les sources de référence restent dans
  `docs/recherche/lvgl/`.
- `ui_djui_bridge.c` branche titre, artiste, BPM, tempo, position, transport,
  mini-waveform (surface IMAGE, rendue depuis `waveform_low` dans un buffer
  PSRAM alloué une fois) et ligne de statut. Zoom en EXTERNAL (vide). Le LOAD
  contrôleur charge la ligne sélectionnée du catalogue (index 0).
- `layer->_clip_area` compile avec le LVGL d'ESP-IDF 6.0.2 (champ public de
  `lv_draw.h`). La perf IMAGE sur P4 reste à mesurer sur banc via les logs
  `lvgl render` / `refr total` du backend (`ui_diagnostics`).
- Gates passés : builds `n` et `y`, simulateur 13 captures inchangées + scénario
  bridge non capturé. Non faits : (c) boot et smoke.

### Phase 2 : Settings (2–3 j)

- Remplacer la construction des widgets de `ui_settings.c` par
  `dj_ui_create_settings`. On garde le poll, les presets de master trim, les
  callbacks (Wi-Fi, enregistrement, DJ Link) et `refresh_storage`.
- Extensions de `dj_ui` :
  - switches DJ LINK et RECORD ;
  - bouton MASTER TRIM en cycle ;
  - timeout du screensaver ;
  - plage de luminosité 10..100 ;
  - callbacks `on_dj_link`, `on_record`, `on_master_trim`, `on_idle_timeout`.
- Gate (b) : `settings`, `settings_restored` et `screensaver`.
- Gate (c) :
  - persistance NVS de la luminosité ;
  - toggle DJ Link on/off avec Ethernet ;
  - enregistrement start/stop ;
  - libellés contrôleur après hotplug.

**Statut : hardware-verified (flag ON)** — v257, 2026-09-25, sur le JC1060
avec la DDJ-400 : luminosité (slider et persistance au redémarrage) validée
par l'opérateur. Le slider pilote enfin le rétroéclairage depuis le correctif
partagé du shim BSP (v256, qui touche aussi le flag OFF).

### Phase 3 : Hot Cues (2 j)

- Utiliser `dj_ui_create_hotcues`. Les couleurs RGB passent par un nouveau
  `dj_ui_set_hotcue_rgb(deck, i, set, pos_ms, rgb)` ; l'index 0..7 reste pour
  la démo.
- On garde `ui_performance_tabs` (loops, beat jump, loop shadow) et
  `hot_cue_store`. Les loop cues s'affichent sur les cartes (badge LOOP +
  longueur).
- Gate (b) : `hot_cues`, D1 et D2.
- Gate (c) :
  - tap sur une carte ⇒ seek/cue ;
  - cues rekordbox seedées ;
  - cue locale posée depuis la DDJ ;
  - loop cue.

**Statut : hardware-verified (flag ON)** — v257, 2026-09-25 : hot cues, cue
play et persistance au redémarrage validés par l'opérateur, LOAD D1/D2 depuis
la Library dj_ui compris (file de résultats de chargement en PSRAM depuis
v257, traces `ui_library` au niveau WARN à chaque maillon du LOAD).

### Phase 4 : Library (4–5 j)

- La vue `dj_ui` (8 lignes par page, en-têtes, PREV/NEXT, LOAD D1/D2, tris)
  remplace les `lv_table`.
- `ui_library.c` garde le catalogue, le tri, le load gate, le worker de
  chargement, la navigation au contrôleur, le peer browse et le fetch NFS.
- Extensions de `dj_ui` :
  - `set_selected(row)` piloté par l'encodeur ;
  - `set_badge(row, text, tone)` ;
  - bouton SOURCE avec texte variable (SOURCE / CANCEL) et `on_lib_source` ;
  - `set_deck_status(deck, text, tone)` avec couleur ;
  - colonne KEY alimentée par la clé du catalogue.
- Gate (b) : `library`, `library_peer`, `library_fetch`, tri par colonne,
  pagination.
- Gate (c) :
  - navigation encodeur ;
  - LOAD D1/D2 pendant la lecture de l'autre deck ;
  - retrait et remontage de l'USB ;
  - peer DJ Link (quand un CDJ est disponible, sinon avec un autre Pajoniiir) ;
  - CANCEL en cours de fetch.

**Réalisé (v258, 2026-09-25)** — validé sur hardware (flag ON) par
l'opérateur : tri, pagination, accents, encodeur, LOAD pendant la lecture.
Le peer DJ Link n'a pas pu être testé (pas de CDJ disponible) :

- Le modèle reste `ui_library.c` (flag ON, `UI_LIBRARY_HEADLESS`) ; `dj_ui`
  n'affiche que l'état publié par `ui_library_djui_publish()` via le pont.
- Tri : un appui sur une nouvelle colonne trie en ascendant, un 2e appui sur
  la colonne allumée inverse. `dj_ui` ne s'allume plus tout seul
  (`dj_ui_library_set_sort(sort, desc)`, flèche haut/bas), donc un refus
  (liste pair « SORT: LOCAL ONLY », « LOAD BUSY ») laisse les boutons
  inchangés. Liste pair : aucune colonne allumée. Un rechargement du catalogue
  (nouvelle génération, ordre de chargement) éteint le tri. Les boutons legacy
  (flag OFF) gardent leurs bascules par colonne.
- Pagination : PREV / NEXT grisés (opacité 40 %) en première / dernière page ;
  la sélection garde sa ligne d'une page à l'autre (inchangé).
- Source DJ Link : même chemin que le legacy (SOURCE fait défiler LOCAL puis
  chaque joueur, LOAD télécharge par NFS, SOURCE devient CANCEL). Lignes pair :
  BPM / TIME en « ... » tant que le détail n'est pas arrivé, « -- » quand il
  vaut 0 ; badges colorés (téléchargement en vert, META en gris, NET en bleu).
- Accents (R1, avant la police Latin-1) : `ui_djui_text_fit()`
  (`components/ui/ui_djui_text.c`, testé en hôte dans `tests/ui_djui_text`)
  replie l'UTF-8 en ASCII (« Été » → « Ete », « – » → « - », « œ » → « oe »,
  le reste en « ? ») sans jamais couper une séquence. Appliqué aux titres et
  artistes des lignes (passés en entier, `dj_ui` ellipse à la largeur), au
  nom du joueur, aux statuts temporaires et aux titres de l'Overview.
  `UI_DJUI_FONT_LATIN=1` laissera passer U+00A0..U+017F et U+2013..U+2026 le
  jour où la famille Latin-1 sera branchée. Le legacy (flag OFF) n'est pas
  replié.
- Gates : simulateur 19 captures (nouvelle `library_bridge_sort_desc`,
  `library*` régénérées après revue : PREV grisé, flèche de tri, fixture FR) ;
  suites hôte `ui_djui_text` (ASCII et Latin) et `ui_library` vertes. Builds
  OFF/ON faits pour le HIL ; HIL OK (sauf DJ Link).

### Phase 5 : Overview (7–9 j, phase à risque)

- Mise en page `dj_ui` : carte info, zone zoomée, pads, footer, panneau FX.
- Pour les deux waveforms :
  - la zone zoomée devient un emplacement : son rectangle écran remplace le
    rect actuel dans `ui_overview_wave_overlay_rect()` ;
  - le cache, le PPA, le zoom, la grille ANLZ, les cues, le cue jaune, la loop
    et le playhead incrusté ne changent pas ;
  - la mini est rendue par `ui_overview_renderer_draw_mini` dans le canvas du
    footer, avec invalidation par plage.
- Déplacer **hors de la zone PPA** les labels que `dj_ui` place dedans (numéro
  du deck, `zoom_time`), ou les incruster dans la bande. Sinon, LVGL et le
  blit PPA s'écrasent mutuellement (voir R2).
- Éléments à ajouter à `dj_ui` : PLAY/CUE/MT, VU, beat indicator, progression
  de chargement moteur, seek tactile (callbacks `on_seek(deck, ms)`), et la
  tonalité du deck (nouveau champ du pont).
- Le pont appelle `dj_ui_set_position()` avec la sortie de
  `ui_position_interpolator_update()`, et le scheduler garde le budget de
  redessin.
- Gate (b) : `overview_deck1`, `overview_deck2`, `overview_loaded`.
- Gate (c), perf :
  - relever les compteurs `ui_overview_perf` (µs du cache, `ppa_us`, fps
    effectifs), deux decks en lecture, zoom min et max ;
  - comparer à v249 : pas plus de 10 % de dégradation, et zéro
    underrun audio sur 30 min.
- Gate (c), fonctionnel :
  - retour d'onglet (réarmement des reblits) ;
  - screensaver puis réveil ;
  - hotplug du contrôleur.

**Réalisé (v259, 2026-09-25)** — pas encore de HIL. Écart assumé avec le plan
ci-dessus, par décision de l'opérateur : on garde **notre** waveform, sans PPA
ni emplacement `EXTERNAL`, et `dj_ui` n'utilise pas son moteur waveform
interne.

- Zoom : le pont possède, par deck, un `ui_overview_wave_cache` lié à une
  bande RGB565 en PSRAM (`(largeur zoom + 2×128) × hauteur`, allouée une fois).
  Le rendu, le scroll incrémental, la grille ANLZ, les hot cues et la loop
  active sont ceux du legacy (même palette, via le nouveau
  `ui_overview_palette.h`). `dj_ui` dessine cette bande en IMAGE « anneau »
  (`dj_ui_wave_set_strip(deck, img, src_x, center_ms)`, deux blits quand
  l'anneau boucle). LVGL la repeint lui-même au retour d'onglet, après le
  screensaver et après le blackout : aucun réarmement de reblit.
- Marqueurs `dj_ui` par-dessus, par surface (`dj_ui_wave_set_marks`) pour ne
  rien dessiner deux fois. Sur le zoom : playhead vert de 3 px, triangle cue
  jaune et trail de loop armée (teinte + trait loop-in blanc,
  `dj_ui_set_loop_armed`). Sur la mini : tous les marqueurs, dont la
  progression jouée.
- Données (`ui_djui_update`) :
  - position : `ui_position_interpolator_update` (vitesse moteur, figée
    pendant le scratch) ;
  - BPM de base : grille ANLZ, sinon deck ; `dj_ui` applique le tempo
    (`pitch_centipercent`) ;
  - MT, cue point, loop (`deck_core_get_loop_display`), beat indicator, VU
    (`deck_peak_display`, mapping et décroissance legacy) ;
  - hot cues fusionnés (`ui_hot_cue_view_merge`) ;
  - fenêtre de zoom via `ui_overview_zoom_window_ms()`, donc le zoom
    contrôleur est partagé avec le legacy ;
  - le centre est calé au pixel par le pont.
- Callbacks : PLAY, CUE, MT et seek tactile appellent les actions legacy
  `ui_overview_action_*`.
- Snapshots : `ui.c` garde une référence ANLZ par deck (retain, puis release
  de l'ancien), car le cache conserve `meta`. Le pont réinitialise le cache
  à chaque frame où `meta` change, même quand l'Overview est masqué.
- Budget : `UI_DJUI_FRAME_BUDGET_PX` vaut 286 000 px de bande par update.
  Au-delà, un deck attend la frame suivante, en alternant l'ordre ; une bande
  passe toujours. Pas de rendu hors de l'onglet Overview, sous le screensaver
  ou sous le blackout. Compteurs : `ui_djui_bridge_get_perf()`, loggés toutes
  les 5 s avec `ui_diagnostics_enabled()`.
- Mesure simulateur (`UI_SIM_DEBUG`, deux decks en lecture à 60 Hz, zoom
  696×119) : 165 648 px de bande par frame et 172 417 px rendus par refresh
  au maximum, sous 286 000.
- Tonalité : champ `key` du pont prêt, mais aucune source de tonalité côté
  deck pour l'instant, donc « -- ». Artwork : toujours le placeholder (R8).
- Gates :
  - simulateur 21 captures, les 19 anciennes inchangées ; nouvelles captures
    `overview_bridge` et `overview_bridge_scroll` ;
  - le simulateur teste aussi le budget réduit (report, une bande par frame,
    aucun deck affamé), l'absence de rendu hors Overview et sous screensaver,
    et la reprise.
- Builds OFF/ON et HIL (perf deux decks, zoom min/max, retour d'onglet,
  screensaver, blackout, seek, PLAY/CUE/MT, hotplug) en attente.

### Phase 6 : barre de statut, nettoyage, documentation (2–3 j)

- Décider où va `ui_status` : la rangée d'onglets `dj_ui` fait 40 px, contre
  54 px pour notre top bar. Recaler `UI_TOPBAR_H` et `UI_CONTENT_Y` (le
  simulateur les passe en défines).
- Retirer les options Kconfig et l'ancien code de widgets (le gain estimé est
  de 3 à 4 k lignes dans `ui_overview.c`, `ui_library.c` et
  `ui_settings.c`).
- Mettre à jour `README.md`, `docs/DEVELOPMENT_PLAN.md` et
  `docs/STARTUP_CHECKLIST.md` (règle AGENTS sur les phases).
- Régénérer toutes les baselines, puis faire l'acceptance HIL complète.

**Réalisé (v292/v293, 2026-09-27)** — retrait du legacy, sans changement de
rendu.

- Barre de statut : décision prise de fait. Sous `dj_ui`, `ui_status.c` (top
  bar de 54 px) n'était plus appelé du tout. La rangée d'onglets `dj_ui` de
  40 px et sa ligne de statut la remplacent. `UI_TOPBAR_H`, `UI_CONTENT_Y` et
  `UI_CONTENT_H` sont retirés de `components/ui/CMakeLists.txt` et de `ui.c` ;
  seuls `UI_HOR_RES`/`UI_VER_RES` restent.
- Kconfig : `UI_PRESENTATION_DJUI` est retiré, `dj_ui` est toujours construit.
  `UI_DJUI_DIRECT_STRIPS` reste (repli perf PPA → images LVGL, pas un flag de
  migration), sans `depends on`.
- Code retiré (environ 6 600 lignes) :
  - `ui_status.c`, `ui_performance_tabs.c`, `ui_mixer_view.c`,
    `ui_overview_scheduler.c`, `ui_active_deck_leds.c` et leurs headers ;
  - `ui_overview.c` réduit au pas de zoom contrôleur
    (`ui_overview_zoom_delta`, `ui_overview_zoom_window_ms`) ;
  - `ui_library.c` : table, pagination, boutons, indicateur de deck actif et
    `UI_LIBRARY_HEADLESS` ; le modèle et le pipeline de chargement restent ;
  - `ui_settings.c` : widgets et caches legacy ; restent la page `dj_ui`, le
    trim master et les callbacks Wi-Fi/REC/DJ Link ;
  - `ui.c`, `ui_controls.c` et `splash_screen.c` : header/footer, styles,
    sélecteur de performance target, screensaver legacy.
- Laissés exprès : les helpers morts des modules purs partagés avec le
  simulateur et le P4 amont (renderer `draw_main*`/`draw_mini`, stats du wave
  cache, motion, grid, setters `ui_idle`). `--gc-sections` les retire du
  binaire.
- Diagnostics : `UI_DIAGNOSTICS_ENABLED` revient à 0. La sonde lvgl STALL
  (échantillonnage et marqueurs de phase) et l'enregistrement du hook
  `controller_usb` dans `app_main` ne tournent que si
  `ui_diagnostics_enabled()`. Les compteurs `controller_usb_host_get_work()`
  restent des incréments atomiques, sans changement sur le chemin USB/audio.
- Gates : simulateur JC1060 vert, 22 captures inchangées, aucune baseline
  régénérée. Build firmware, flash et acceptance HIL complète en attente.

**Total estimé : 21 à 27 jours.** La phase 5 porte l'essentiel de
l'incertitude.

---

## 4. Risques

### R1 : polices Montserrat ASCII et accents

- Les Montserrat intégrées à LVGL (et `Musieer_*`, générées en
  `--range 0x20-0x7F`) ne couvrent que l'ASCII. Les titres FR (« Été »,
  « Garçon »), les tags ID3/rekordbox et les noms venus de DJ Link (UTF-16LE
  converti en UTF-8) s'affichent **déjà aujourd'hui** avec des glyphes
  manquants. `dj_ui` ne crée pas ce problème et ne le règle pas non plus.
- Plan :
  - générer avec `lv_font_conv` une famille sur `0x20-0x7E,0xA0-0x17F`
    (Latin-1 + Latin Extended-A, ≈ 350 glyphes), plus `0x2013-0x2026`
    (tirets, guillemets typographiques, points de suspension) ;
  - le faire pour les tailles réellement utilisées (12/14/16/20/24/28/32),
    en bpp 4.
- Coût estimé : de l'ordre de 3,5 fois la taille ASCII, soit quelques
  centaines de Ko de flash au total. Acceptable, mais à vérifier contre la
  table de partitions OTA (taille de l'app) **avant** de s'y engager.
- Chaîner `lv_font_t.fallback` vers la Montserrat ASCII de même taille. Pour
  les codepoints encore absents (CJK, emoji), ajouter une translittération de
  secours (é→e, ß→ss…) dans le pont, jamais dans le chemin audio.
- Test : une fixture simulateur « Été à Paris – Garçon » dans la library et
  dans l'overview, avec capture dédiée.

### R2 : performance P4 du redraw waveform à 30 Hz

- Voir le calcul de §2 : le modèle `dj_ui` invalide ≈ 286 k px par frame. Le
  nôtre rend quelques colonnes de bord et fait des blits PPA hors de LVGL.
- La mitigation, c'est la stratégie elle-même : `dj_ui` ne dessine jamais les
  waveforms en firmware.
- Risque secondaire propre à notre modèle : tout objet LVGL qui chevauche la
  zone PPA (labels, bordure, rayon de la boîte `dj_ui`) est redessiné par
  dessus le blit, ou l'efface. Il faut une géométrie stricte (la boîte zoomée
  sans enfant, bordure en dehors du rect PPA) et une capture de contrôle au
  HIL, puisque le simulateur ne voit pas le PPA.
- Règle audio inchangée : aucune modification de priorité ni de pacing, et
  aucune allocation dans le mix. Les buffers UI sont alloués en PSRAM au
  chargement d'une piste, jamais par frame. L'UI perd toujours.

### R3 : baselines du simulateur à régénérer

- Le manifeste stocke le SHA-256 du framebuffer complet : chaque phase
  invalide les captures qu'elle touche.
- Processus :
  1. lancer `-KeepArtifacts` et relire les PPM ;
  2. lancer `-UpdateBaselines` ;
  3. commiter les baselines **séparément** du code, en indiquant quelles
     captures changent et pourquoi.
- La phase 0 est un prérequis : tant que la cible JC1060 compile les sources
  P4, régénérer les baselines ne prouve rien (§0).
- Le simulateur ne couvre ni le PPA, ni le DSI, ni le toucher réel, ni le
  timing du panel. Le gate (c) reste obligatoire à chaque phase.
- Le PowerShell `run_ui_simulator_e2e.ps1` n'a pas de variante JC1060. Il faut
  soit en ajouter une, soit documenter que le gate JC1060 est bash-only.

### R4 : règle upstream

- `dj_ui` est **notre** couche. Il n'existe pas d'upstream à suivre ou à
  alimenter, donc rien à reporter. On peut le modifier librement (découpage,
  API, thème).
- La règle AGENTS « ne pas nettoyer en masse une baseline importée » ne
  s'applique pas à `dj_ui`. Elle continue de s'appliquer à LVGL, au fork
  esp-usb et aux scripts cmake de fork-patch, qui restent intouchés.
- `docs/recherche/lvgl/` garde la version d'origine comme référence de
  design. Toute divergence vit dans la copie firmware.

### R5 : régressions de navigation au contrôleur

- `dj_ui` est pensé pour le tactile. Les commandes DDJ (sélection library,
  zoom, load, deck actif) passent par `ui_update()` et doivent piloter la vue
  via le pont.
- Chaque phase ajoute ces commandes à sa checklist HIL.

### R6 : mémoire LVGL

- `dj_ui` crée ses 4 pages au démarrage, soit quelques centaines d'objets.
  Pendant la transition (phases 1 à 5), l'ancienne et la nouvelle vue d'un
  même onglet ne doivent jamais coexister : le choix est fait par Kconfig à la
  compilation, pas à l'exécution.
- Mesurer le heap LVGL après le boot à chaque phase.

### R7 : écarts de modèle de données

- Couleurs de cue : RGB contre 8 index.
- `float` BPM et tempo contre nos entiers (centi-%, ×100).
- `uint8_t` pour le nombre de lignes et `uint16_t` pour le total de la
  library : le total dépasse 65 535 sur de grosses clés, il faut passer à
  `uint32_t`.
- Toutes les conversions vivent dans le pont, testé en hôte comme fonctions
  pures.

### R8 : pas d'artwork (traité côté jc1060, 2026-09-27, pas encore testé sur hardware)

- Fait : table Artwork (0x0D) de la PDB, décodage TJpgDec dans un worker
  hors audio, miniatures 40x40 (rows Library) et 34x34 (en-tête deck, via
  `dj_ui_set_artwork_pixels`), cache PSRAM borné. Détails dans
  `docs/rekordbox-format-analysis.md` (Artwork rows). Les notes ci-dessous
  décrivent l'état d'avant.

- `dj_ui_set_artwork` n'a pas de source aujourd'hui (il faudrait la table
  artwork de la PDB, un décodage JPEG et une mise à l'échelle 34x34).
- Placeholder « ART » jusqu'à une phase dédiée, hors de ce plan.

---

## 5. Ce qui n'est pas dans ce plan

- Export NFS de notre USB (v250+, voir `docs/DJ_LINK_SPEC.md`).
- Pochettes et polices CJK.
- Portage vers la cible P4 800x480 (`firmware/main-deck-p4`). `dj_ui` est
  conçu pour du 1024x600 et n'est pas prévu pour la cible P4.
