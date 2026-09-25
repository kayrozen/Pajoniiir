# Crash au play — findings v200–v203 (2026-09-24)

## État du portage à ce point
- v203 flashée. UI 1024x600 OK, DDJ-400 connectée (profil S3CP, MIDI OK),
  library 52 tracks OK, LOAD track OK (fix RAM v192 : Wi-Fi remote OFF).
- Toggle MAIN OUT (USB DDJ ⇄ PCM5102A) implémenté : app_settings `main_out_usb`
  (NVS `main_usb`, défaut 1), shim `bsp_audio_get_main_i2s_tx()` lit le réglage
  runtime, bouton dans Settings → OUTPUT, `audio_engine_main_sink_refresh()`.
- v193 : ajout `CONFIG_BSP_PCM5102A_MAIN_OUT=y` (Kconfig shim) — avant ça le
  sink MAIN était le stub NOT_SUPPORTED de l'upstream.
- v194 : retry montage SD ×5 (0x107 transitoire rendait ESP_ERROR_CHECK fatal).
- v195/196 : ratés (enable I2S à l'init = hack, bootloop INVALID_STATE).
- v197–v203 : toggle runtime + fixes moteur (détail ci-dessous).

## Symptôme actuel (v203)
Play deck1 → CPU0 saturé par `ae_output` → IDLE0 affamé → task WDT (5 s) →
abort → reboot. Aucun son. Pas de « output sink fault » loggé sous v203.

## Faits établis (vérifiés logs/ELF/code)
1. Le NVS a `main_out_usb=0` (MAIN = PCM5102A) : au boot, `i2s_channel_disable:
   channel has not been enabled yet` à ~2 s ne peut venir que de
   `bsp_audio_main_i2s_set_sample_rate` avec handle non-NULL (en mode USB le
   shim renvoie NULL et sort avant). Probablement togglé dans Settings.
2. `audio_output_service_open_codec` n'a JAMAIS tourné sous v203 : ni
   « PCM5102A main out open » ni « shared codec open » dans les logs (0 hits).
   Donc `s_output_codec_open` devrait rester false → la task devrait rester
   dans la branche d'attente pacée (vTaskDelay 5 ms).
3. CONTRADICTION : le PC du backtrace WDT symbolise en
   `ae_output_task` audio_engine.c:3694 = boucle de mixage 256 frames, qui
   exige `s_output_codec_open == true`. Autres PCs symbolisés (boots
   précédents) : audio_output_mixer_next_prepared (audio_output_mixer.c:218),
   audio_mixer_pcm_from_dsp, audio_mixer_limit_master_float.
4. Backtrace RISC-V = 1 frame seulement (pas de frame pointers) — on ne voit
   pas la chaîne d'appels. Coredump présent sur SD (task "main" = vieux dumps
   ESP_ERROR_CHECK v197/198 ; ne pas se fier sans vérifier la fraîcheur).
5. Le log série devient silencieux entre ~8 s et le WDT (~22 s/43 s) —
   cohérent avec un CPU0 étouffé (même l'UART souffre).
6. Toutes les boucles candidates examinées sont BORNÉES :
   audio_output_sink_write_all (≤3 appels, written==0 → ERROR),
   audio_resampler_next (while source_frames-- borné), audio_keylock.c (aucun
   while), pas de continue entre write_main et mon pacing, pas d'autre
   utilisateur du canal I2S (seuls shim set_sample_rate/abort_write).
7. v198 : « output sink fault main=ESP_ERR_INVALID_STATE » = canal I2S
   désactivé au moment du write (vieux boots, contract : absent = NOT_SUPPORTED
   désormais, retourné par write_main quand s_main_i2s_tx==NULL, toléré par les
   2 call sites idle (vTaskDelay 5) et actif (main_ok = !s_main_i2s_tx || OK)).
8. Le pacing que j'ai ajouté (fin de boucle active, `!s_main_i2s_tx && !s_codec`,
   sommeil 1 ms jusqu'à la frontière du bloc 5.3 ms, fallback 48 kHz) est dans
   v203 mais la contradiction (2)/(3) montre qu'on ne comprend pas encore quel
   chemin tourne réellement.

## Hypothèses classées
H1. Le flag/état réel diffère de ce que les logs laissent croire : ouvrir
    output SANS logger (un autre chemin appelle open_codec ?), ou un second
    `ae_output` créé par play (ligne 4268, task_ctx) alors que le service
    (3874) existe déjà. Vérifier qui appelle quoi au play.
H2. i2s_channel_write retourne instantanément OK (DMA vidée anormalement vite,
    horloge I2S délirante après reconfig ?) → boucle à vide sans fault.
H3. Spin réel dans le mix (resampler/keylock/pop) — contredit par code borné,
    sauf pop_source (deck ring pop) non vérifiée en profondeur.
H4. Deux tasks ae_output se battent (service + per-engine) — vérifier.

## Prochain pas (v204 — instrumentation)
- Heartbeat printf direct dans ae_output_task : toutes les N itérations,
  printer s_output_codec_open, s_main_i2s_tx!=NULL, s_codec!=NULL,
  s_output_sample_rate, compteur de blocs + phase ae_wdt_trace courante.
- Printer au PLAY (audio_engine_play) : mêmes champs + qui appelle
  audio_output_service_open_codec (ajouter log à l'entrée).
- Vérifier si play crée un 2e ae_output (ligne 4268) alors que le service
  tourne déjà → si oui, gate pour n'en avoir qu'un.
- Option : CONFIG pour frame pointers (RISC-V) afin d'avoir des backtraces
  complets au WDT.

## Divers à ne pas oublier
- Kconfig `PAJ_MAIN_OUT_USB` du shim : plus utilisé par le code (remplacé par
  app_settings main_out_usb) — à retirer/nettoyer.
- Bloc « v185c TEMP DEBUG » GPIO37/38 dans sdkconfig.defaults : à revert.
- La clé USB de test contient Latitude_5X10_*.exe etc. — export rekordbox
  propre attendu (export.pdb absent → « pdb: Cannot open »).
- heap interne : 36 Ko libres / bloc max 11 Ko au load (avant fix Wi-Fi) —
  le toggle Wi-Fi remote OFF est maintenant requis pour charger une track.

## Revue code (2026-09-24) — contradiction résolue, cause probable

Analyse statique du code + du build flashé ; rien de modifié, rien testé sur
carte.

### A. La contradiction (2)/(3) est un artefact de log
- `build/config/sdkconfig.h` : `CONFIG_LOG_DEFAULT_LEVEL 2` (WARN).
- `CONFIG_LOG_DEFAULT_LEVEL=3` (`sdkconfig.defaults:143`, bloc « v190 INFO »)
  est IGNORÉ : symbole dérivé du choix `CONFIG_LOG_DEFAULT_LEVEL_WARN=y`
  (lignes 13 et 114). Aucun `esp_log_level_set("audio", …)` dans le code.
- Donc les `ESP_LOGI` « PCM5102A main out open » / « shared codec open »
  (audio_engine.c:3272/3290) et « audio_engine_init: output ready » sont
  filtrés → les « 0 hits » du fait 2 sont normaux.
- ⇒ `open_codec` A tourné, `s_output_codec_open == true`, le PC à
  audio_engine.c:3694 (boucle de mix) est cohérent. **H1 tombe.**
- **H4 exclue** : `audio_fw_task_plan.c` renvoie toujours
  `start_output = false` → la ligne 4268 ne crée jamais de 2e `ae_output`.
- Fait 1 non fiable : le message `i2s_channel_disable: channel has not been
  enabled yet` n'identifie pas l'appelant ; `esp_codec_dev_open` (ES8311, dans
  `bsp_audio_init_cfg`) manipule aussi ce canal au boot. Ne pas en déduire
  `main_out_usb=0`.

### B. Mécanisme du WDT : surcharge CPU0, pas un spin
- `ae_output` = prio 6, core 0 ; `ae_decode` / `ae_loader` = prio 5, core 0.
- Si un bloc de mix (256 frames) coûte ≥ sa période (~5,8 ms @ 44,1 kHz),
  plus rien ne dort :
  - mode PCM : DMA I2S jamais plein → `i2s_channel_write` ne bloque pas ;
  - mode USB : le pacing v203 (audio_engine.c:3799) voit
    `spent_us >= pace_period_us` et saute le sommeil.
- Seul relâchement restant : `vTaskDelay(1)` tous les 64 blocs / 100 ms
  (`audio_output_timing.h`). Ce tick est pris par `ae_decode` (prio 5, ring
  qui se vide, toujours prêt) → IDLE0 ne tourne jamais → TWDT 5 s.
- PCs répartis sur `mixer_next_prepared` / `pcm_from_dsp` /
  `limit_master_float` = profil d'un CPU saturé, pas d'un spin → cohérent avec
  le fait 6 (toutes les boucles bornées). H3 « spin » peu probable ; H3
  « mix trop lent » = cause probable.
- Pourquoi plus lent que sur JC4880 (HYPOTHÈSE, non mesurée) : options perf
  identiques à l'upstream (XIP PSRAM, 360 MHz, L2 128 KB), mais écran
  1024×600 RGB888 (`BSP_LCD_COLOR_BITS 24`) vs 480×800 RGB565 upstream →
  ~2,4× plus de bande passante PSRAM consommée par le DSI, en concurrence avec
  le code XIP PSRAM et les lectures timeline. Aussi
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=64` (upstream 256) → plus de petites
  allocations en PSRAM.

### C. Bug distinct : aucun son vers la DDJ (mode USB)
- `controller_usb_audio_stream.c` : `STREAM_RATE_HZ` passé 44100 → 48000,
  mais `controller_usb_audio_stream_write` rejette
  `source_sample_rate < STREAM_RATE_HZ`.
- `audio_output_select_sample_rate()` renvoie 44100 pour une piste 44,1 kHz
  → chaque bloc renvoie `ESP_ERR_INVALID_ARG`, ignoré par le `(void)`
  (audio_engine.c:3622 et 3734) → la DDJ ne reçoit que du silence.
- Fix : accepter 44,1–48 kHz en entrée ET agrandir `RESAMPLE_OUTPUT_FRAMES`
  (129), trop petit pour suréchantillonner 44,1 → 48 kHz.

### D. Plan v204 (remplace « Prochain pas » ci-dessus)
1. Logs visibles : remplacer le bloc v190 par `CONFIG_LOG_DEFAULT_LEVEL_INFO=y`
   + `# CONFIG_LOG_DEFAULT_LEVEL_WARN is not set`, ou
   `esp_log_level_set("audio", ESP_LOG_INFO)` dans `app_main`.
2. Mesurer : logger tous les ~500 blocs `mix` / `block_elapsed_us` vs
   `block_period_us` (`s_phase.mix_max_us` existe déjà, exposé aussi dans le
   diagnostic `web_server`). Si mix ≥ ~5,8 ms → surcharge confirmée.
   Test décisif : couper le rendu LVGL pendant le play ou passer
   temporairement en RGB565 → le WDT disparaît-il ?
3. Journal WDT sur SD : après chaque reboot WDT, événement `AUDIO_WDT_TRACE`
   dans `/sd/logs/system.log` (phase, n° de bloc, groupe de mix, masque
   decks). Attendu ~172 blocs/s ; un n° de bloc anormalement élevé = free-run.
4. Appliquer le fix C (taux UAC) indépendamment du crash.

## v205–v208 (2026-09-24)

### Mix à 30 ms/bloc : résolu (v205 sonde, v206/v207 fix)
- Sonde v205 (4 groupes de 16 frames : normal / scheduler suspendu / IRQ
  masquées / normal) : `norm=1802 susp=2037 irqoff=1683 us`, `cyc/us=358`.
  CPU à la bonne fréquence, pas de vol par tâche ni IRQ : ~40k cycles/frame
  de stall mémoire.
- Cause : `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` → tout `.text` (dont le chemin
  mix par frame) est fetché depuis la PSRAM via le L2 partagé.
- v206 : `components/audio_engine/linker.lf` (objets DSP par frame en
  `noflash`) + `AE_RT_ATTR` (IRAM) sur `ae_output_task` et ses callbacks
  par frame. HIL v207 : `norm=71 us`, mix avg 1290 us, plus de WDT, 2 decks.
- v207 : le +31 Ko IRAM a cassé `ui_load` (pile 16 Ko interne) → pile en
  PSRAM (`xTaskCreateWithCaps`/`vTaskDeleteWithCaps`), comme `ae_decode`.
  Sûr : `SPIRAM_FETCH_INSTRUCTIONS`+`SPIRAM_RODATA` ⇒ le cache n'est jamais
  coupé pendant une écriture flash.

### A. Aucun son master/cue en mode USB (v208)
- `main_rc=0x106` est normal en mode USB (pas de sink I2S). MAIN et cue sortent
  uniquement par `controller_usb_host_write_audio()` →
  `controller_usb_audio_stream_write()`, dont le code retour était ignoré.
- Le fix « C » ci-dessus partait d'une prémisse fausse : `STREAM_RATE_HZ`
  avait été passé à 48000 dans l'arbre de travail, alors que la DDJ-400 est
  44,1 kHz seulement (DAC 44,1 kHz ; alt 1 S16 / alt 2 S24_3LE listent
  44,1 kHz, `docs/AUDIO_ENGINE_USB_SPEC.md` D4/D6). `select_stream_format()`
  exige ce taux → aucun candidat → `ESP_ERR_NOT_SUPPORTED` → stream jamais
  démarré (« UAC unavailable ») → chaque write rejeté en `INVALID_STATE`.
- v208 : `STREAM_RATE_HZ` revient à 44100 (upstream) ; l'entrée 44,1–48 kHz
  reste acceptée (48 → 44,1 kHz via `controller_audio_resampler`, testé
  hôte : ratio 0,91875) ; SET_CUR toujours sauté (NAK DDJ-400 v183).
- v208 diag : ligne `HB UAC rc=… fail=… claimed cfg streaming faulted epoch
  cfg_fail xfer_fail pkt_fail sub drop ring under over` à chaque heartbeat.
  Attendu : `rc=0x0 fail=0 streaming=1`, `under` stable.

### B. Skips : mix max 11–13 ms, 1–2 overruns / 500 blocs (à mesurer)
- Corrélé aux pics `decode max=8–10 ms` (lectures USB MSC).
- `ae_decode` (prio 5, core 0) ne peut pas préempter `ae_output` (prio 6,
  core 0), et la phase mix est du calcul pur : un mix de 11 ms signifie que
  `ae_output` a été préempté (tâches prio ≥ 6 ou ISR, p. ex. DWC/MSC,
  `controller_usb` prio 6 non épinglée) ou ralenti par contention L2/PSRAM
  (DMA MSC, core 1).
- Mesure à faire avant fix : déclencher la sonde PROBE sur le pire bloc (ou
  sur `mix > period`) plutôt qu'une fois par fenêtre ; si `susp`/`irqoff`
  restent rapides pendant un pic → préemption (épingler `usb_hostd`/
  `usb_lib`/`usb_store`/`ae_decode` sur core 1) ; si tous lents → contention
  mémoire (buffers MSC en interne, prebuffer plus grand).

### B. Sonde v209 (statistique, tous les blocs actifs)
- Chaque bloc actif chronomètre ses 16 groupes de 16 frames ; un groupe
  tourne scheduler suspendu (`susp`), un autre IRQ masquées (`irqoff`),
  décalés de 8 et tournant d'un groupe à chaque bloc. Bloc « spike » =
  somme des groupes > période de sortie.
- Deux lignes par heartbeat (500 blocs) :
  `PROBE spikes=N/M avg us/16fr spike norm= susp= irqoff= | all norm= susp= irqoff=`
  `PROBE worst mix= us susp@g irqoff@g modes D0= D1= grp=<16 valeurs>`
- Lecture (colonnes `spike`) :
  - `norm` ≫ `susp` ≈ `irqoff` → préemption par une tâche sur core 0 ;
  - `norm` ≈ `susp` ≫ `irqoff` → charge ISR sur core 0 ;
  - les trois ≈ lents → contention mémoire (L2/PSRAM).
  `grp=` du pire bloc montre si le pic est concentré sur 1–2 groupes
  (préemption/ISR ponctuelle) ou étalé (contention).
- Coût : ~70 µs IRQ masquées + ~70 µs scheduler suspendu par bloc de
  5,8 ms en régime normal ; plus long seulement pendant un pic. Diagnostic
  à retirer une fois B tranché.

## v210 : UAC jamais démarré au boot v209 (2026-09-24)
- Symptôme v209 : `HB UAC rc=0x103 fail=500/500 cfg=0 streaming=0 cfg_fail=0`,
  aucune ligne `controller_uac` (`UAC formats parsed`, `cand`, `UAC ready`),
  MIDI OK.
- Cause : `probe_device()` (controller_usb_host.c) ne démarre l'UAC que si
  `direct_root_child && parent_port == 1` (hérité d'upstream). Dans le fork
  esp-usb, `parent.port_num` vaut l'index root **0-based** pour un device
  direct (HS = 0, cf. `controller_bootstrap.c`) et le port 1-based d'un hub
  sinon ; `parent_dev_hdl` est toujours NULL. DDJ sur le root HS (layout A)
  → `parent_port=0` → UAC sauté sans aucun log (cfg_fail=0 le confirme :
  `controller_usb_audio_stream_start()` n'est jamais appelé). Les boots
  v205/206 avaient vraisemblablement la DDJ vue en `parent_port=1` (root FS
  ou port 1 du hub) ; non confirmé, la ligne INFO n'était pas compilée.
- Fix v210 : gate accepte `parent_port` 0 (root HS) ou 1 (cas v205/206) ;
  ligne WARN `UAC gate PID= parent_port= direct_root= -> start|skip` à
  chaque probe Pioneer. FIFO port 0 : retombe sur `fifo_settings_custom`
  (ptx 180 lignes = 720 o ≥ MPS iso 576).

## v211 : famine du ring UAC, 164 blocs/s au lieu de 187,5 (2026-09-24)
- HIL v210 (DDJ-400, MAIN sur USB, pas d'I2S ni ES8311) : `ring=122-300/2048`,
  `under` +~1700/HB, `rate=164` pour `exp=187` (période 5334 µs à 48 kHz).
- Cause : le pacing sans sink attendait `block_start + period` par pas de
  `vTaskDelay(1)` (tick 1 ms). Chaque bloc dépassait donc de l'arrondi au tick
  plus le travail après le sleep (yield, bookkeeping), et le dépassement ne se
  rattrapait jamais : ~6,1 ms/bloc. Simulation hôte (mix 1,3–1,7 ms, tick
  1 ms) : ancien schéma 166,7 blk/s, échéance absolue 187,5 blk/s.
- Le chemin idle (`idle_main_rc == NOT_SUPPORTED`) dormait 5 ms fixes, donc
  plus vite que la période : c'est probablement l'origine de `over`/`drop`,
  qui n'augmentent plus pendant le play.
- Fix v211 : `ae_output_pace_sinkless()` avec une échéance absolue
  (`deadline += period`), réancrée à `now` au-delà de 4 périodes de retard
  (pas de rafale de rattrapage). Même helper dans les chemins actif et idle ;
  l'échéance est remise à zéro dès qu'un sink bloquant existe.
- Resampling : `s_output_sample_rate` = taux du morceau (48 kHz ici). Le
  48 → 44,1 kHz se fait dans `controller_usb_audio_stream_write()`
  (`controller_audio_resampler`, sur la tâche `ae_output`) vers le ring
  44,1 kHz. esp_timer et les SOF de l'hôte viennent du même quartz P4, donc
  un débit producteur exact suffit ; le trim ±1 frame du ring
  (`write_clocked`, bande 5/8..7/8) remonte le niveau d'environ 187 frames/s.
- Format DDJ-400 : `select_stream_format()` retient le premier candidat
  4ch 44,1 kHz 16 ou 24 bits dont le MPS couvre 45 frames, **tel que décrit
  par le device**. Taille de paquet et conversion 16→24 bits suivent
  `s_format.bytes_per_sample`. v210 : alt 1 annoncé 16 bits, pkt 360 ≤ MPS 576.
  Le « bits 24 » cité pour cand 0 en v205 contredit ce résultat : à trancher
  avec les lignes `cand` du boot v211.
- Logs : `FLX4 UAC ready` devient `UAC ready ... mps= ... (B/sample, pkt)`,
  le libellé de recovery devient `UAC stream`, et les logs LED/connexion de
  deck_core passent de « FLX4 » à « controller ».

## v212 : DDJ-400, son faible/distordu, master absent, boutons morts (2026-09-24)
- HIL v211 : pacing OK (rate 185/187, ring 1230–1296 stable, drop/over 0).
  Retour utilisateur : son très bas et très distordu, rien au master alors que
  les phones sortent, aucun bouton MIDI (seuls MASTER/HEADPHONE LEVEL, qui
  sont analogiques sur la DDJ-400, réagissent).
- Packing : `prepare_and_submit()` suit `s_format.bytes_per_sample` du
  candidat choisi. Avec bps=2, les int16 du ring sont copiés tels quels (S16LE
  4ch, ordre MASTER L/R puis PHONES L/R) ; la conversion 24 bits ne tourne que
  si bps=3. Le code est cohérent avec le format déclaré.
  **Incohérence ouverte** : cand 0 en v205 = `alt 1 bits 24 bps 3 rates 2
  pkt 540` (conforme à ddj400_re 03_USB_SYSTEM : alt 1 PCM 4ch 24 bits
  44,1/48 kHz), contre alt 1 S16 pkt 360 annoncé en v210/v211. Si le device
  consomme réellement du 24 bits alors que l'hôte envoie du S16, les trames
  sont désalignées : bruit sur MASTER, signal décalé sur PHONES, ce qui
  correspond au symptôme. v212 journalise la liste des taux dans les lignes
  `cand` et `UAC ready` affiche bps/pkt : à comparer au boot v212.
- Deuxième piste si la distorsion persiste : « rates 2 » ⇒ la DDJ accepte
  probablement 44,1 et 48 kHz, or v208 saute SET_CUR. Si son taux par défaut
  est 48 kHz, elle manque ~3 frames/ms, ce qui produit un son haché.
- Niveau : l'atténuation upstream `>> 2` (−12 dB, sans justification)
  devient `STREAM_GAIN_SHIFT 0`.
- MIDI : le profil SD `controllers/pioneer_ddj_400` vient de
  `MIDI_MAP_RESEARCH.md`, qui est faux (VID 0x0853/PID 0x0504, PLAY 0x0E,
  CUE 0x0F, 6 pads au lieu de 8). Une fois actif, il passait avant la map
  intégrée dans `controller_runtime_handle_midi()`. Or la map Mixxx FLX4
  (`docs/reference/Pioneer-DDJ-FLX4.midi.xml`) est « based on DDJ-400
  mapping » (PLAY 0x90/0x0B). Fix : PID 0x0026 utilise la map/LED FLX4
  intégrée et n'est plus soumis à la résolution de profil SD. palmarci/ddj400_re
  ne contient pas de table MIDI (listée « next step »). Note : la DDJ-400 a une
  auth SysEx logicielle (07_MIDI_AUTH), non requise d'après Mixxx pour les
  messages de contrôle.
- Diagnostic : les 48 premiers messages MIDI de chaque boot sont loggés
  (`MIDI ss d1 d2 -> mapped|unmapped`).

## v213 : profil SD DDJ-400 rétabli, canaux MASTER/PHONES échangés (2026-09-24)
- HIL v212 : la map intégrée fonctionne sur la DDJ-400 (`MIDI B6 0D 3F ->
  mapped`, etc.). La disposition MIDI de la DDJ-400 est donc bien celle de la
  map Mixxx FLX4.
- Décision opérateur : pas de contrôleur codé en dur, seul le profil SD fait
  foi. `p4_local_controller.c` : `builtin_map_supports()` redevient
  FLX4-only, et la DDJ-400 repasse par
  `controller_profile_manager_on_descriptor_report()`. Le profil SD actif est
  prioritaire dans `controller_runtime_handle_midi()`. Sans profil SD, la
  DDJ-400 est `unsupported`. Le log `MIDI .. -> mapped|unmapped` (48
  premiers messages) est conservé.
- `controllers/pioneer_ddj_400/profile.json` est régénéré depuis
  `controllers/pioneer_ddj_flx4/profile.json` (Mixxx XML), avec nom et PID
  0x0026, sans Smart CFX / Smart Fader. Le champ `source` documente
  l'origine. `profile.s3bin` : 6148 B, CRC 0xE0BB089B, 287 inputs,
  127 outputs, 19 pair slots, recompilation déterministe.
  L'ancien profil erroné est archivé dans le scratchpad de session. README et
  `MIDI_MAP_RESEARCH.md` portent désormais un avertissement.
- Audio : l'opérateur confirme que MASTER et cue sont inversés. Dans
  `controller_usb_audio_stream.c`, `STREAM_PHONES_SLOT 0` et
  `STREAM_MASTER_SLOT 2` placent PHONES sur ch 1-2 et MAIN sur ch 3-4. Le
  swap est fait lors de l'entrelacement avant le resampler, seul point
  d'écriture du ring. `prepare_and_submit()` garde ainsi sa copie directe en
  S16 et son expansion par sous-trame en 24 bits, et le résultat sur le bus
  est identique.

## v214 : flux DDJ-400 en 24 bits, parseur UAC durci (2026-09-24)
- Constat opérateur : v205 donnait `cand 0: ifc 1 alt 1 ... bits 24 bps 3
  rates 2 pcm24 1 pkt 540`, v213 `bits 16 bps 2 pcm16 1 pkt 360`. Avec 16
  bits envoyés à un device qui lit des sous-trames de 3 octets, les trames
  sont décalées, ce qui donne la distorsion et l'inversion apparente des
  canaux.
- Vérification : `git diff HEAD` montre que les lectures du descripteur Type I
  (@4 bNrChannels, @5 bSubframeSize, @6 bBitResolution, @7 bSamFreqType)
  n'ont pas changé depuis v169. v211 n'a élargi que le filtre de
  packetisation. Un test hôte sur un descripteur synthétique (alt 1 24 bits
  44,1/48 + alt 2 16 bits) donne bien `alt 1 bits 24 bps 3 rates 2`. Le
  « S16 » de v210 venait de la chaîne upstream figée « 4ch/16-bit » du log
  ready, pas du parseur. La différence v205/v213 n'est donc **pas expliquée
  par le code** ; le dump brut ci-dessous doit trancher.
- Correctifs :
  - Parseur (`flx4_uac_descriptors.c`) : seul le **premier** descripteur
    FORMAT_TYPE de **type I** (bFormatType 1) d'un alt compte. Une largeur
    incohérente (bSubframeSize hors 1..4, ou bBitResolution > 8×subframe)
    laisse l'alt incomplet au lieu d'être devinée. bSamFreqType 0 (plage
    continue) est géré (`sample_rate_continuous`). Le contrôle de
    packetisation vérifie 44,1/48 kHz via `format_has_rate`.
  - Sélection (`select_stream_format`) : tous les candidats sont loggés et un
    pcm24 bat un pcm16. Nouveau log `UAC selected cand N (bits, B/sample)`.
  - Packetizer initialisé avec `s_format.bytes_per_sample` au lieu du 2 figé.
    `prepare_and_submit()` étend déjà l'int16 en S24_3LE (`s << 8`) quand
    bps=3. En 24 bits, pkt = 45×4×3 = 540 ≤ MPS 576.
  - Le swap v213 est annulé : MASTER sur ch 1-2, PHONES sur ch 3-4, dans
    l'ordre des string descriptors.
  - `controller_usb_host.c` : `dump_uac_descriptors()` s'exécute à **chaque**
    démarrage UAC et affiche chaque descripteur interface/endpoint/CS en hex
    complet. Avant, le dump ne sortait qu'en cas d'échec, et le sous-type
    affiché lisait le mauvais octet.
- Taux : la ligne `cand` affiche `rates N [r0 r1 r2]`, avec `cont` pour une
  plage continue. SET_CUR reste sauté à 44,1 kHz (NAK infini en v183). Si le
  device annonce 2 taux et démarre par défaut à 48 kHz, ce sera visible au
  HIL (son accéléré/haché) ; ce serait alors le prochain chantier.
- Note : `tests/flx4_usb_audio` vise la copie S3 et attend un rejet du 24
  bits. Il n'est pas concerné par la copie JC1060.

## v215 : packing S24_3LE innocenté, diagnostic de la distorsion (2026-09-24)
- HIL v214 : la vitesse est bonne (donc 44,1 kHz correct et SET_CUR inutile
  pour l'instant), mais le son reste distordu.
- Packing vérifié par un test hôte gcc qui extrait la boucle réelle de
  `prepare_and_submit()` (sed des lignes du firmware) :
  - Exhaustif sur les 65 536 int16 : les octets sont `00, lo, hi`, soit
    `s << 8` en little-endian. L'octet de poids fort est l'octet haut de
    l'int16 et porte donc le signe ; le dépack S24LE signé donne exactement
    `s × 256`. 0 erreur, pas d'inversion d'octets, pas de décalage.
  - Chaîne complète : sine 48 kHz, entrelacement 4ch, resampler, ring,
    packetizer 44/45, pack, dépack. Aucun saut ni désalignement de canal sur
    88 200 frames.
  - Côté HCD (esp-usb fork) : paquets contigus (`bytes_filled +=
    num_bytes`), tailles 528/540 multiples de 4, et cache msync C2M sur les
    URB OUT.
- Qualité du resampler linéaire 48→44,1 du flux USB (le moteur tourne à
  48 kHz, 187 blocs/s). Résidu de distorsion/alias sous le signal : 64 dB à
  1 kHz, 45 dB à 3 kHz, 32 dB à 6 kHz, 22 dB à 10 kHz, 13 dB à 15 kHz. Les
  aigus sonnent granuleux, mais le résampler de deck est lui aussi linéaire
  et le chemin I2S JC4880 a été accepté : cause secondaire probable, pas
  « crade » sur tout le spectre.
- Diagnostics v215 :
  - `HB UAC` ajoute `trim=` et `dup=` (glissements d'horloge du ring),
    `lost=` (frames de paquets iso perdus), `lim=` (échantillons MAIN
    écrêtés par le soft limiter, genou 30000) et `peak=` (pic d'entrée du
    limiter). Compteurs cumulés.
  - `STREAM_TEST_TONE_HZ` (0 par défaut) : s'il est non nul, un sinus à
    −6 dBFS remplace l'audio moteur sur les 4 canaux, après le resampler. Un
    sinus propre disculpe ring, packing et USB ; un sinus sale accuse le
    transport.

## v216 — le consommateur USB cadence le moteur

- Compteurs v215 : `dup` ≈ 2 × `sub`, `trim` = 0, `under` ≈ +500/s, `lim` = 0,
  ring entre 1173 et 1343. La zone morte clocked (dup ≤ 1280) du ring tourne
  donc en continu : le moteur, cadencé à l'horloge murale sans sink local,
  produit trop peu face au drain SOF de 44 100 trames/s. Des trames sont
  répétées en permanence, d'où la distorsion. Le packing est hors de cause
  (validé en v215).
- Correctif, fidèle à l'upstream : dans l'upstream, le moteur bloque sur le
  DMA I2S du PCM5102A, donc le sink cadence la production. Sur la JC1060 sans
  sink local, `ae_output_pace_sinkless()` appelle d'abord
  `controller_usb_host_audio_pace_ready()`. On rend un bloc dès que
  `queued + incoming ≤ STREAM_PACE_CEILING_FRAMES` (5/8 du ring, soit 1280).
  Sinon, attente d'un tick de 1 ms, au plus `AE_PACE_MAX_LAG` périodes, puis
  repli sur l'échéance murale.
- En mode cadencé par le consommateur (`s_consumer_paced`), l'écriture passe
  par `controller_audio_ring_write()` et non plus `_write_clocked()`. Il n'y a
  plus de dup/trim trame par trame, car la dérive est absorbée par le rythme
  de production.
- Sortie USB seule : `audio_output_service_open_codec()` force la chaîne à
  `AE_USB_SINK_RATE_HZ` (44 100 Hz) et logue
  `USB-only output: <src> Hz source -> 44100 Hz chain`. Les resamplers par
  deck absorbent le taux source, et le resampler linéaire du stream devient un
  bypass (plus de résidu d'alias).
- Simulation hôte (vrai ring + packetizer, rendu de 1 à 5 ms avec pic de
  12 ms sur 1/200, 60 s) : `dup` = 0, `trim` = 0, `over` = 0. Les underruns
  n'apparaissent qu'à l'amorçage (529 trames = les 12 premiers paquets sur
  ring vide) ; ring entre 319 et 1104 au moment des drains.
- Le sinus de test reste désactivé (`STREAM_TEST_TONE_HZ 0u`). S'il est
  activé, le log `UAC TEST TONE <n> Hz replaces engine audio` le confirme.

## v217 — packing prouvé bit-exact, récupération après débranchement

- Données v216 : transport sain (`dup`=2, `trim`=0, `lost`=0, `lim`=0, ring
  stable à 1169/2048), mais la distorsion persiste.
- Test hôte gcc : la boucle de packing est extraite telle quelle de
  `prepare_and_submit()` (awk sur le source, pas de recopie à la main).
  - Les 65 536 valeurs int16 → S24_3LE → int16 donnent 0 erreur.
  - Un sinus 1 kHz sur 4 canaux (176 400 échantillons) est bit-exact.
  - Octets d'un paquet :
    `00 87 1A | 00 34 33 | 00 57 3F | 00 F8 3B | 00 87 22 ...` pour 6791,
    13108, 16215, 15352, 8839. Ordre b0 = LSB (0, justifié à gauche),
    b1 = milieu, b2 = MSB avec le signe, conforme à UAC Type I
    bSubframeSize 3 / bBitResolution 24.
  - Cas limites : −1 → `00 FF FF`, −32768 → `00 00 80`, 32767 → `00 FF 7F`.
- Le reste du chemin est lui aussi transparent :
  - le resampler du stream à 44,1 → 44,1 kHz est un `memcpy` ;
  - le HCD place les paquets iso de façon contiguë (`bytes_filled += xfer_len`),
    comme les remplit `prepare_and_submit()`.
- La distorsion est donc en amont du stream (moteur) ou côté appareil.
  Nouveaux logs pour trancher :
  - `UAC ep 0x.. bmAttributes 0x.. sync=async|adaptive|sync bSynchAddress in_ep` :
    un endpoint async attend que l'hôte suive son feedback, ce que le stream
    ne fait pas ;
  - `UAC pkt24 src a b c d e -> 16 octets` : premier paquet réel non
    silencieux, capturé dans le chemin iso et logué depuis `get_stats` ;
  - test décisif : `STREAM_TEST_TONE_HZ 1000u`. Un sinus propre accuse le
    moteur, un sinus sale accuse la synchronisation avec l'appareil.
- Reconnexion : le Host Library ne réarme le root qu'une fois le device
  fermé par tous les clients. Un `close_step()` bloqué laisse donc le root
  mort : ni NEW_DEV, ni MIDI, ni UAC. Correctifs :
  - `routed_queue_reset()` attendait les producteurs MIDI OUT en
    `taskYIELD()`, qui ne cède qu'aux tâches de priorité égale ou supérieure.
    Or le producteur LED (dispatch, prio 4) peut être préempté dans la gate
    pendant que la tâche contrôleur (prio 5/6) boucle. Remplacé par
    `vTaskDelay(1)`, avec un log au-delà de 500 ms.
  - Watchdog : si la fermeture dure plus de 1 s, halt/flush forcé des
    endpoints UAC (`controller_usb_audio_stream_force_flush()`) et MIDI,
    même pour un device parti.
  - Récupération sur faute : l'upstream demandait en dur le power-cycle de
    `USB1` (index 1). Sur la JC1060, c'est le root du stockage. On utilise
    maintenant le root où le contrôleur a été sondé (`recovery_root`, 0 en
    layout A). Une faute signalée après DEV_GONE ne remet plus `device_gone`
    à false et ne déclenche plus de power-cycle.
  - Réarmement : après une fermeture sur DEV_GONE sans NEW_DEV, le root du
    contrôleur est power-cyclé via le manager après 5 s, puis toutes les
    15 s. Le manager refuse tant qu'un attach ou une énumération est en cours.
  - Le compteur de log MIDI (48 messages) est remis à zéro à chaque connexion.
  - Logs `recovery:` : DEV_GONE, closing, close waiting on `<étape>` (chaque
    seconde, avec `uac_blockers`), forced halt/flush, closed in X ms, NEW_DEV,
    re-arm #n, re-attached after X ms.

## v218 — dump pkt24 analysé : packing correct, pas de changement

- HIL v217 : `sync=sync`, donc l'endpoint est synchrone et le feedback est
  inutile.
- Dump : src −1138 (0xFB8E) → `00 8E FB`. C'est bien S24_3LE : la valeur
  24 bits est −1138 × 256 = 0xFB8E00, et b2 = 0xFB porte le signe. Décodé :
  −291328.
- `8E FB FF` serait −1138 en 24 bits, soit 48 dB sous la pleine échelle.
  La formule proposée (`(int32_t)s16 << 8`, puis b0/b1/b2) produit elle aussi
  `00 8E FB` : identique au firmware sur les 65 536 valeurs (test hôte).
- Une seule boucle de packing 24 bits existe ; le chemin 16 bits est une
  copie int16 native LE. Aucun changement de code.
- Il reste le moteur. Prochaine étape : un build avec
  `STREAM_TEST_TONE_HZ 1000u`.

## v218 (applied on operator request)

- `STREAM_PCM24_LEFT_JUSTIFY 0`: 24-bit subframes now carry the sign-extended
  int16 unshifted (`-1138 -> 8E FB FF`). Previous upstream packing (`s << 8`,
  `-1138 -> 00 8E FB`) kept behind `STREAM_PCM24_LEFT_JUSTIFY 1`.
- Host test (scratchpad `t_v218.c`, loop extracted from the source): `8E FB FF`
  for -1138, exhaustive 65536-value round trip 0 errors, sine bit-exact.
- Expected effect: the same waveform 48 dB below full scale (24-bit container).
  If the distortion disappears only because the level is ~48 dB lower, it is
  not a proof of packing; compare with the tone build.
- `STREAM_TEST_TONE_HZ 1000u` enabled in the same build (-6 dBFS int16, so
  ~-54 dBFS on the wire with the unshifted packing). Set back to `0u` after HIL.

## v219 : taux d'échantillonnage réel du device (2026-09-24)

- Cadencement vérifié dans le code :
  - `pkt 540` = octets = 45 frames × 4 canaux × 3 octets (paquet max à
    44,1 kHz), pas 180 frames.
  - `sub` du HB = blocs moteur de 256 frames (HB tous les 500 blocs), soit
    172/s × 256 ≈ 44 100 frames/s, pas des paquets iso.
  - Packetizer : un paquet par trame de 1 ms, 9×44 + 1×45 frames par 10 ms.
    HCD DWC : un qTD par trame, buffers chaînés (`next_start_idx`), et un
    paquet manqué remonterait SKIPPED (compté dans `lost`, qui vaut 0).
  - `ddj400_re/docs/03_USB_SYSTEM.md` ne donne que alt 1 PCM 4ch 24 bits
    44,1/48 kHz : ni bInterval, ni vitesse, ni type de synchro.
- Faille trouvée : SET_CUR était sauté (v208), en supposant 44,1 kHz par
  défaut. La conclusion de v214 (« vitesse bonne ⇒ 44,1 kHz ») ne tient pas.
  Sur un endpoint synchrone, l'hôte borne le débit : un device cadencé à
  48 kHz garde le tempo, mais joue chaque paquet 8,8 % trop aigu et manque
  ~3,9 frames par ms, d'où un bourdonnement à 1 kHz.
- v219 :
  - `STREAM_RATE_CONTROL 1` : SET_INTERFACE → GET_CUR → SET_CUR 44100 →
    GET_CUR, puis priming. Logs `UAC GET_CUR rate before/after SET_CUR` et
    `UAC SET_CUR 44100 Hz completed`. Un STALL sur une étape de taux est
    journalisé puis ignoré.
  - Si une étape reste en attente plus d'1 s (NAK, comme en v183), le HB
    journalise `UAC control step N pending > 1 s` : repasser alors
    `STREAM_RATE_CONTROL` à 0.
  - Buffer de contrôle agrandi à 8 + 64 octets (étage IN).
  - `STREAM_TEST_TONE_HZ` revient à 0u (demande opérateur).
  - `STREAM_PCM24_LEFT_JUSTIFY` reste à 0 (v218, niveau −48 dB).
- Vérification : `gcc -fsyntax-only -Wall -Wextra` avec des stubs ESP-IDF,
  0 warning. Pas de build firmware.

## v220 : left-justify + pacing 44,1 kHz, traçage MIDI OUT (2026-09-24)

- HIL v219 : `UAC control step 2 pending > 1 s`. Le NAK porte sur le
  GET_CUR (étape 2), avant tout SET_CUR : la DDJ ne répond à aucune requête
  SAMPLING_FREQ sur l'endpoint. Le taux ne peut pas être piloté par contrôle.
- Switches v220 : `STREAM_RATE_CONTROL 0` (séquence v218), `STREAM_PCM24_LEFT_JUSTIFY 1`
  (`s << 8`), `STREAM_TEST_TONE_HZ 0u`. La chaîne 44,1 kHz et le pacing USB
  v216 sont inchangés : c'est la première fois que left-justify et pacing
  correct tournent ensemble.
- LED / MIDI OUT :
  - `07_MIDI_AUTH.md` décrit un SysEx signé (vendor `00 40 05`, device 0x206,
    TLV vendor/produit, FNV-1a, jeton TEA à clé firmware), mais ne donne
    aucune séquence exploitable. Mixxx pilote les LEDs de la DDJ-400 sans ce
    chiffrement : l'auth ne conditionne donc probablement pas le feedback
    LED. Piste non retenue pour l'instant.
  - Sur la DDJ-400 (PID 0x0026), la map intégrée FLX4 est désactivée : les
    LEDs passent uniquement par le profil SD (`outputs` : vu_meter B0/B1 02,
    notes, banques de pads). Si le profil n'est pas actif, aucune LED ne part.
  - Traçage v220 :
    - `controller_led` : 24 premières requêtes LED par connexion, avec les
      octets MIDI ou `no mapping (profile_active=… builtin=…)` ;
    - `controller_usb_host` : 16 premiers `MIDI OUT ep … submit` et
      `MIDI OUT done status=… actual=…` par connexion.
- Vérification : `gcc -fsyntax-only` avec des stubs sur `controller_led_runtime.c`
  et `controller_usb_audio_stream.c`, 0 warning. Pas de build firmware.

## v221 : isolation des canaux, traçage LED après activation (2026-09-24)

- LEDs : aucun bug de liaison trouvé. `controller_profile_runtime` n'existe
  qu'en un exemplaire, avec un seul `s_active`, lu par le runtime MIDI IN
  et par `controller_led`. Seuls `clear()` (déconnexion, profil non trouvé)
  et `init()` (bootstrap) le remettent à false. En revanche, le profil
  s'active de façon asynchrone (tâche `local_profile_task`, lecture SD)
  après le connect. Or le connect ré-arme la trace LED v220 : les 24
  lignes peuvent être épuisées (CUE, VU) avant l'activation, et c'est
  compatible avec `profile_active=0` suivi de `profile=local`.
- Traces v221 :
  - `ctrl_profile_rt` en WARN : `dynamic profile active … outputs=N` et
    `dynamic profile cleared` (l'ancien log INFO était compilé hors build) ;
  - `controller_led` : `LED path sees profile_active=0/1` à chaque
    transition, et la fenêtre de 24 lignes est ré-armée à ce moment.
- Audio : le seul document de topologie (`03_USB_SYSTEM.md`, chaînes 4 à 7)
  donne l'ordre MASTER L, MASTER R, PHONES L, PHONES R, qui est celui des
  slots 0 à 3. `06_HARDWARE.md` ne dit que « 4 channels: 2 stereo pairs ».
  Interface 2 alt 1 est l'IN (non utilisée) ; la sortie est interface 1
  alt 1, 4 canaux. Rien ne contredit l'entrelacement actuel.
- `STREAM_CHANNEL_MASK 0x3u` : seuls MASTER L/R portent du signal, PHONES
  est en silence numérique. Log `UAC CHANNEL MASK 0x3`. Remettre 0xFu ensuite.
- Vérification : `gcc -fsyntax-only` avec stubs sur les trois fichiers
  touchés, 0 warning. Pas de build firmware.

## v222 : chaîne 48 kHz, traçage LED par deck (2026-09-24)

- HIL v221 : distorsion identique avec `STREAM_CHANNEL_MASK 0x3`, donc
  l'entrelacement est hors de cause. Masque remis à `0xFu`.
- 48 kHz :
  - `STREAM_RATE_HZ 48000u` et `AE_USB_SINK_RATE_HZ 48000u` (à garder
    égaux). La chaîne USB-only tourne à 48 kHz (256 frames/bloc, 187,5
    blocs/s) et le packetizer émet 48 frames/ms = 576 octets = MPS.
  - `RESAMPLE_OUTPUT_FRAMES` = 141 (static assert OK). Aucune requête de
    taux (`STREAM_RATE_CONTROL 0`). Left-justify reste à 1.
  - Hypothèse testée : la DDJ tourne à 48 kHz malgré l'absence de SET_CUR.
    Prédiction : son propre si c'est le cas, sinon tempo accéléré ou
    débordements.
- LEDs deck 1 :
  - Profil SD (`--dump` du `.s3bin` committé) : 127 outputs, deck 0 =
    0x90 / pads 0x97 / VU B0, deck 1 = 0x91 / 0x99 / B1. C'est identique
    à la map FLX4 intégrée (`flx4_led_midi.c`) et `CTRL_DECK_1 = 0`. Aucune
    erreur de compilation ni d'adressage dans le profil committé.
  - Non vérifié : la copie présente sur la SD (le runtime lit celle-là).
  - Traces v222 : une fenêtre de 24 lignes par deck, le VU limité à 2
    lignes par deck, et chaque paquet des transferts MIDI OUT loggés
    (`MIDI OUT [i] ..`) avec le compteur de drops de la file.

## v223 : 48 kHz forcé, LED sans mapping (2026-09-24)

- HIL v222 : aucun son, aucun `UAC ready`. Les descripteurs DDJ-400
  n'annoncent que `rates 1 [44100 0 0]`, donc le filtre 48 kHz n'a trouvé
  aucun candidat et le flux n'a jamais démarré (le HB affichait pourtant
  `out_rate=48000`).
- `STREAM_FORCE_RATE_HZ 48000u` :
  - `STREAM_RATE_HZ` en découle (44100 si le switch vaut 0) ;
  - le filtre des taux annoncés est contourné ; canaux, format et MPS sont
    toujours vérifiés, et l'alt 24 bits reste préféré ;
  - le packetizer émet 48 frames/ms = 576 octets ;
  - log `UAC FORCED RATE 48000 Hz (descriptor announces 44100 Hz)` ;
  - `AE_USB_SINK_RATE_HZ` reste à 48000.
- `STREAM_RATE_CONTROL 1`, séquence réordonnée :
  - SET_INTERFACE, **amorçage du flux**, puis SET_CUR 48000 (étape 3) et,
    seulement si SET_CUR aboutit, GET_CUR (étape 4) ;
  - le GET_CUR d'avant SET_CUR est supprimé (NAK permanent en v219) ;
  - un NAK ne bloque plus la lecture et ne met jamais le flux en faute ;
  - logs : `UAC SET_CUR 48000 Hz completed`, `UAC GET_CUR rate after
    SET_CUR`, `UAC rate step N status=..`, `UAC control step 3 pending > 1 s` ;
  - à noter : en v183, le SET_CUR avait un wIndex faux. Avec le wIndex
    correct, il n'a encore jamais été émis (v219 bloquait à l'étape 2).
  - risque connu : un SET_CUR NAKé en permanence garde `s_control_active`
    et retarde le teardown sans débranchement, comme en v219.
- LEDs :
  - v222 confirme que MIDI OUT fonctionne (`09 90 0C 7F done status=0
    actual=4/4`, donc deck 0 CUE part bien sur 0x90) ;
  - `LED 42` = `LED_SMART_FADER` ; `LED 41` = `LED_SMART_CFX`. Ce sont
    des LED FLX4 (`96 01` / `96 00`) sans équivalent sur la DDJ-400.
    `LED_BEAT` (2) et `LED_END` (3) n'ont de sortie ni FLX4 ni DDJ-400,
    et seule l'UI les utilise. Tableau dans
    `controllers/pioneer_ddj_400/README.md` ;
  - les LED sans mapping sont maintenant loggées une fois par ID, hors de
    la fenêtre de 24 lignes par deck. Ces envois globaux partent sur deck 0,
    donc ils mangeaient la fenêtre du deck 0 ;
  - code relu : deck_core publie les deux decks de façon symétrique
    (snapshot deck 0 puis deck 1, compat = `CTRL_DECK_1`). Seules les
    LED globales SMART_CFX/SMART_FADER/BEAT_FX_ON/MASTER_CUE sont
    limitées au deck 0.
- Vérification : `gcc -fsyntax-only -Wall -Wextra` sur
  `controller_usb_audio_stream.c` et `controller_led_runtime.c`, 0
  warning. Pas de build firmware.

## v224 : référence Mixxx quirx, SysEx d'init, échelle VU (2026-09-24)

- 48 kHz forcé de v223 conservé (`STREAM_FORCE_RATE_HZ 48000u`,
  `STREAM_RATE_CONTROL 1` après amorçage).
- Comparaison profil DDJ-400 vs `Pioneer-DDJ-400-quirx-script.js` : PLAY/CUE
  90/91 0B/0C, pads 97/99 (shift 98/9A), VU B0/B1 02, trackLoaded 9F 00/01.
  **Aucun écart**, le profil n'est pas modifié. Le `lights.deck2.vuMeter=0xB0`
  du script est un bug de ce script ; sa fonction réelle envoie bien B1, comme
  notre profil.
- VU : le script envoie `value*150` (VU Mixxx 0..1). Un octet de données MIDI
  plafonne à 127, donc le vumètre est plein à 85 % de l'entrée. Sur DDJ-400,
  `controller_led_runtime.c` applique `niveau*150/127` borné à 127
  (`LED_DDJ400_VU_SCALE 150u`, 0 = brut). On ne fait pas d'envoi > 0x7F :
  un octet de données ≥ 0x80 n'est pas du MIDI valide.
- SysEx d'init `F0 00 40 05 00 00 02 06 00 03 01 F7` : 4 paquets USB-MIDI
  (04 F0 00 40 / 04 05 00 00 / 04 02 06 00 / 07 03 01 F7), envoyés depuis
  `local_profile_task` quand le profil DDJ-400 s'active, juste avant
  `controller_runtime_request_snapshot()`. Switch `LOCAL_DDJ400_INIT_SYSEX`,
  log `DDJ-400 init SysEx … queued: ESP_OK`.
- `controller_runtime_handle_midi` ignore désormais les CIN 0x4-0x7 (fragments
  SysEx), pour qu'une réponse SysEx ne soit jamais passée aux maps.
- Vérification : `gcc -fsyntax-only` sans erreur sur controller_led_runtime.c,
  controller_runtime.c et controller_usb_audio_stream.c. Pour
  p4_local_controller.c, seules restent des erreurs de stubs FreeRTOS
  (portMAX_DELAY, pdPASS), hors des lignes modifiées. Pas de build firmware.

## v225 : SysEx d'init porté par le profil (2026-09-24)

- Correction architecturale demandée par l'opérateur : le SysEx d'init
  n'est plus codé en dur dans `p4_local_controller.c` (retrait de
  `LOCAL_DDJ400_PID`, `LOCAL_DDJ400_INIT_SYSEX` et
  `send_ddj400_init_sysex`). Le fichier redevient générique : à
  l'activation de n'importe quel profil, il appelle
  `controller_led_runtime_send_profile_init()` avant
  `controller_runtime_request_snapshot()`.
- Format : champ optionnel `init_sysex` dans `profile.json` (F0..F7,
  ≤ 64 octets). `compile_profile.py` le valide et le place après les
  outputs ; sa longueur va dans le u16 d'en-tête à l'offset 30 (ex-réservé).
  Les profils sans SysEx restent identiques à l'octet près : les fixtures
  FLX4, generic et Hercules recompilées sont identiques aux `.s3bin`
  committés. Le `--dump` affiche `init_sysex[n]`.
- DDJ-400 : `profile.json` porte
  `[240,0,64,5,0,0,2,6,0,3,1,247]`, et `profile.s3bin` fait maintenant
  6160 octets (CRC 0xF4E6D7FD). **Recopier le `.s3bin` sur la SD.**
- JC1060 :
  - `cp_profile_parse` : lecture de la longueur, taille attendue + n,
    validation F0/F7/payload < 0x80 et copie dans
    `cp_profile_t.init_sysex` ;
  - `controller_profile_manager` : taille attendue + n ;
  - `controller_profile_runtime_init_sysex_packets()` découpe le message en
    paquets USB-MIDI (CIN 0x4, puis 0x5, 0x6 ou 0x7 pour le dernier) ;
  - `controller_led_runtime_send_profile_init()` les envoie via
    `controller_usb_host_send_packet` et logge `profile init SysEx: N
    USB-MIDI packets queued`. Il est placé là et pas dans
    `controller_runtime`, qui reste un mappeur sans sortie USB testable
    sur PC.
- Non modifiés : les parsers/managers S3 et main-deck-p4. Ils rejettent un
  profil avec `init_sysex` (taille), ce qui ne concerne aujourd'hui que le
  DDJ-400 sur JC1060.
- Erreur de build `TAG` : `static const char *TAG` est maintenant défini
  juste après les includes de `p4_local_controller.c`.
- Reste codé en dur, même type d'écart : `LED_DDJ400_VU_SCALE` /
  `LED_DDJ400_PID` dans `controller_led_runtime.c` (échelle VU v224).
- Vérification :
  - test PC du scratchpad, parser + runtime JC1060 : DDJ-400 parse avec
    12 octets et produit `04 F0 00 40 / 04 05 00 00 / 04 02 06 00 /
    07 03 01 F7` ; un F7 corrompu est rejeté ; les trois fixtures
    parsent avec 0 octet de SysEx et ne produisent aucun paquet ;
  - `test_convert_web_profile` OK ;
  - `gcc -fsyntax-only` : seules restent des erreurs dues aux stubs
    FreeRTOS. Pas de build firmware.

## v226 : chasse aux spikes de playback, échelle VU dans le profil (2026-09-24)

Spikes audibles alors que le ring UAC reste stable (`ring=1280`, `dup=2`,
`drop=0`, `lim=0`). Le HB montre des pics de mix (~9,6 ms) et de décodage
(~10 ms pendant les lectures MSC).

- Analyse :
  - le ring UAC oscille entre ~670 et 1280 trames (14 à 27 ms), donc un bloc
    de mix à 9,6 ms (période 5,3 ms) ne s'entend pas tant que le ring ne
    tombe pas à 0 ;
  - le décodage lit via un cache compressé et devance la tête de lecture
    d'environ 2 s dans la timeline PCM. Un `fread` MSC de 10 ms est surtout
    de l'attente I/O : il ne coûte pas de CPU0 à `ae_output` ;
  - trois tâches USB tournaient sur le cœur audio ou pouvaient y tomber :
    - `msc_host` : sa config était initialisée à zéro, donc `core_id = 0`,
      et il tournait épinglé sur CPU0 en prio 5, comme `ae_decode` ;
    - `controller_usb` : non épinglé, prio 6 pendant le streaming comme
      `ae_output` ; il remplit les paquets isochrones et pouvait se
      partager le temps CPU0 avec le mix ;
    - `usb_hostd` : non épinglé ; `usb_host_install()` alloue les
      interruptions DWC sur le cœur de cette tâche.
- Instrumentation, nouvelle ligne `HB spk` après les deux lignes HB
  (deltas calculés sur la fenêtre du HB) :
  - `over` : blocs dont le rendu dépasse la période ;
  - `behind` : blocs où le drain attendait déjà, sans sommeil de pacing ;
  - `pace_wait max` : plus longue attente de pacing ;
  - `uac_low` : niveau minimal du ring vu par `pace_ready()`, juste avant
    chaque remplissage (`controller_usb_host_audio_take_pace_low_water()`) ;
  - `u_under`, `u_dup`, `u_trim`, `u_lost` : deltas des compteurs UAC ;
  - `pcm_under D1/D2` : pops PCM ratés par deck, c'est-à-dire un deck à
    sec ;
  - `runway_min D1/D2` : minimum de PCM décodé en avance, sur les decks
    actifs ;
  - `dec_max D1/D2` : pire appel de décodage de la fenêtre.

  Lecture : un clic avec `u_under > 0` vient d'un ring UAC vide (timing).
  Avec `pcm_under > 0`, un deck est tombé à sec (décodage/I/O). Si les deux
  sont à 0, le timing est hors de cause et il faut chercher dans le contenu
  DSP : `lim`, `peak`, modes PROBE.
- Correctifs :
  - `usb_storage.c` : `MSC_TASK_CORE 1` ;
  - `usb_storage_shared.c` : `daemon_core_id = 1`, ce qui place aussi les
    ISR USB sur CPU1 ;
  - `p4_local_controller.c` : `task_core_id = 1` pour `controller_usb`.

  Les priorités ne changent pas. Sur CPU1, `controller_usb` (5 ou 6) et
  `msc_host` (5) passent avant LVGL (4), et `usb_hostd` (4) partage le
  round-robin avec LVGL. `usb_store` (3) et `usb_lib` (4) restent non
  épinglés : ils sont sous la prio de l'audio et hors du chemin temps réel.
- PROBE : nouveau `AE_MIX_PROBE_ISOLATE`, laissé à 1 pour ce build de
  diagnostic. À 0, plus de groupe à scheduler suspendu ni à IRQ masquées ;
  c'est la valeur à prendre en production une fois la source connue.
- Lecture fichier : inchangée. Rien n'indique pour l'instant qu'elle affame
  l'audio ; `runway_min` et `pcm_under` le diront.
- Échelle VU dans le profil :
  - l'output `cc_value` accepte un `"scale"` optionnel, compilé dans le u16
    à l'offset 10 de l'entrée output (ex-réservé) ;
  - `cp_profile_map_led` envoie `min(127, state * scale / 127)` ;
  - un `scale` non nul sur une sortie note est rejeté par le compilateur et
    par le parser JC1060 ;
  - `LED_DDJ400_VU_SCALE` et `LED_DDJ400_PID` sont supprimés de
    `controller_led_runtime.c`.

  Le `profile.s3bin` DDJ-400 fait toujours 6160 octets ; seuls deux octets
  d'échelle et le CRC changent (CRC 0xB51FE77F). **Recopier le `.s3bin` sur
  la SD.** Les fixtures FLX4, generic et Hercules ne changent pas.
- `STREAM_TEST_TONE_HZ` reste à `0u`.
- Vérification :
  - test PC du scratchpad, parser JC1060 :
    - VU DDJ-400 B0/B1 02 : 64 → 75, 85 → 100, 100 → 118, 127 → 127 ;
    - la sortie note PLAY ne change pas ;
    - une sortie note avec un scale est rejetée (rc -6) ;
    - les trois fixtures ont `value_scale = 0` et un VU brut ;
  - `test_convert_web_profile` OK ;
  - `gcc -fsyntax-only` : il ne reste que des erreurs de stubs ESP-IDF, en
    dehors des zones modifiées.

  Pas de build firmware.

## v228 — load NO_MEM : tâche `ae_output` persistante

Symptôme v226/v227 : aucun load ne passe, `failed to start shared output
task` (`ESP_ERR_NO_MEM`) sur les deux decks. v225 chargeait normalement.

- Diff v225 → v227 : l'`audio_engine` n'a ajouté aucune allocation heap.
  Il n'y a que ~84 o de `.bss`/`.sdata` (`s_hb_base`, champs ajoutés à
  `s_hb`, `s_hb_decode_max_us`, `s_pace_low_water`). Le code HB est en flash.
  `AE_MIX_PROBE_ISOLATE` n'alloue rien. Le `value_scale` du profil ajoute
  +320 o à `s_profile` (`.dram1.bss`, région heap 2) et +320 o au malloc
  interne transitoire du parse.
- Région `0x4ff27b80` (78912 o) à 147/147 blocs au boot : c'est le premier
  morceau de heap interne (juste après `.dram0.bss`), rempli en premier par
  first-fit. Ce n'est pas une anomalie en soi. Il faudrait un dump v225 au
  même moment pour comparer.
- Cause réelle : `ae_output` était détruite au dernier unload
  (`vTaskDelete`), puis recréée à chaque premier load avec 8 Ko de stack +
  TCB contigus en RAM interne. Ce create tombe après la fragmentation du
  heap (USB, profil, UI), et parfois avant que l'idle ait libéré l'ancienne
  stack. Quelques centaines d'octets en moins et un ordre d'allocation
  différent (tâches repinnées en v226) suffisent à le faire échouer.
- Correctif :
  - `ae_output` est créée une seule fois dans `audio_engine_init`, heap
    encore propre ;
  - entre deux sessions, elle reste parquée sur `ulTaskNotifyTake` ;
  - `ensure_started` ne fait plus que `run=true` + `xTaskNotifyGive`, avec
    une création lazy en secours si l'init a échoué ;
  - `s_output_active` appartient au côté contrôle. Chaque run donne un seul
    `s_output_done`, consommé soit par `stop`, soit par `ensure_started`
    (après une sink fault) ;
  - si le create échoue, le log donne `internal free` / `largest` ;
  - `controller_profile_runtime_activate` alloue le scratch de parse en PSRAM
    (repli sur `malloc`).
- Vérification : `gcc -fsyntax-only` ne signale que des trous de stubs
  ESP-IDF ; le test PC du runtime compile avec `-Werror`. Pas de build
  firmware.

## v229 — waveform qui recule de quelques frames

Symptôme : chaque deck, indépendamment de l'autre, voit parfois sa waveform
reculer de quelques frames puis repartir. Le playback est sain (v228 :
`pcm_under` 0/0, runway ~91 ms, mix max ~4,4 ms). Des pics `dec_max` jusqu'à
9,7 ms apparaissent (lectures MSC).

- Cause : `ui_position_interpolator` extrapolait depuis une ancre à
  l'horloge locale et ignorait le snapshot tant que l'écart restait sous
  120 ms. La dérive entre l'horloge UI et la position audio (permille
  entier, commit de position retardé par un pic) s'accumulait sans être
  corrigée. Au franchissement du seuil, un snap arrière d'un coup ramenait
  la waveform plusieurs frames en arrière. Test PC avec 0,3 % de dérive sur
  3 min : l'ancien algo fait 4 reculs, jusqu'à 87 ms.
- Correctif, côté UI uniquement (copie JC1060) :
  - la position affichée est en µs et ne décroît jamais pendant la lecture ;
  - un snapshot neuf ne fait que moduler la vitesse d'affichage
    (constante de temps `UI_POSITION_INTERPOLATOR_SLEW_TAU_MS` = 500 ms) ;
  - un snapshot inchangé ne corrige rien : on suit l'horloge ;
  - un affichage en avance de plus de 120 ms attend l'audio au lieu de
    reculer ;
  - un snapshot qui recule (seek, cue, hot cue, loop wrap) ou qui saute de
    plus de 120 ms en avant (beat jump) est suivi immédiatement ;
  - pause, premier sample et scratch (`speed_permille == 0`) restent
    autoritatifs.
- Chemin audio intact : aucun changement de mix, de pacing, de priorité de
  tâche, ni d'allocation.
- Vérification :
  - `tests/ui_position_interpolator` (7 cas existants) passe contre la copie
    JC1060 ;
  - test scratchpad : commit retardé de 15 ms, pas entre 32 et 34 ms, jamais
    de recul ; dérive de 0,3 % : écart max 1 ms, sans recul ; stall de
    300 ms : l'affichage attend ; cue arrière de 50 ms et seek avant
    immédiats.
  - La copie `main-deck-p4` n'est pas modifiée.

## v230 — micro-coupure audio toutes les ~2,7 s, synchrone sur les 2 decks

Symptôme : le son se coupe un instant puis repart, toutes les ~3 s, sur les
deux decks en même temps. La période est celle du heartbeat
(`AE_HB_BLOCKS` = 500 blocs, soit 2662 ms à 187,5 blocs/s).

- Cause confirmée dans le code :
  - `ae_output_heartbeat_note()` appelait `ESP_LOGI` depuis `ae_output`
    (prio 6, core 0). En fin de fenêtre, il imprimait 5 lignes : HB UAC,
    HB blk, HB spk et les deux lignes PROBE, soit ~1 000 caractères.
  - Chaque `ESP_LOG` traverse la chaîne de sortie de façon synchrone :
    1. `console_vprintf` : `send()` TCP bloquant si un client est connecté ;
    2. `ls_vprintf` : `uart_write_bytes(UART0)`, driver installé avec
       `tx_buffer_size = 0`, donc bloquant jusqu'à ce que tout soit entré
       dans la FIFO ;
    3. vprintf libc vers la console, UART0 elle aussi, en polling.
  - À 115200 bauds, c'est ~87 µs par caractère, écrit deux fois : environ
    170 ms de mix bloqué, contre un ring UAC de ~14-27 ms.
  - Les lignes blk/UAC/PROBE existaient avant ; v226 a ajouté HB spk
    (~220 caractères), d'où les « petits spikes » déjà entendus.
- Correctif, sans toucher au `sdkconfig` :
  - `ae_output` ne formate et n'imprime plus rien. En fin de fenêtre, il
    copie `s_hb` et `s_mix_probe` dans un `ae_hb_report_t` en PSRAM, mais
    seulement si le slot est libre, puis appelle `xTaskNotifyGive`. Si le
    slot est occupé, la fenêtre est comptée comme perdue, et l'audio
    n'attend jamais.
  - Nouvelle tâche `ae_log` : prio 1, core 1, stack de 6 Ko en PSRAM, créée
    dans `audio_engine_init`. Elle lit les stats UAC et limiter, prend
    `uac_low`, `pcm_under` et `dec_max`, puis imprime les 5 lignes.
  - Les messages ponctuels du chemin de sortie passent par un ring de 8
    événements : `EOF drain complete`, `startup gate released` et
    `output sink fault`. Il est protégé par un spinlock de quelques
    instructions. Ring plein : le message est perdu et compté.
  - Si `ae_log` ne peut pas être créée, les logs du chemin de sortie sont
    perdus ; ils ne sont jamais imprimés en ligne.
  - `controller_usb_host_get_audio_stats()` peut lui-même logguer (control
    stall, dump pkt24). Il n'est plus appelé depuis `ae_output`.
- Restent à surveiller, sans changement dans v230 :
  - `AE_MIX_PROBE_ISOLATE` est passé de 1 à 0 dans ce même build v230 :
    plus de groupe à scheduler suspendu ni à IRQ masquées par bloc. Les
    lignes PROBE restent, avec susp/irqoff lus comme norm ; remettre 1 pour
    rediagnostiquer.
  - Les logs d'erreur `isochronous status/resubmit` restent synchrones dans
    le callback USB.
  - La tee UART0 de `log_screen.c` reste sans buffer TX.
- Vérification : un scan des fonctions appelées par `ae_output_task` ne
  trouve plus aucun `ESP_LOG`. `gcc -fsyntax-only` ne signale que des trous
  de stubs ESP-IDF. Pas de build firmware.

## v231 — waveform : monotone strict, sans lissage lent

Retour HIL v230 : la micro-coupure de ~3 s a disparu. En revanche, le blend
v229 (constante de temps de 500 ms) laissait l'affichage décrocher
visiblement de la position réelle.

- `ui_overview` lit la position moteur en direct à chaque frame
  (`deck_core_get_deck_state()` → `audio_engine_deck_position_ms()`, résolution
  d'un bloc de sortie, ~5 ms). Il n'y a donc rien à lisser : il suffit
  d'afficher cette position sans jamais reculer.
- Nouvel algorithme de `ui_position_interpolator`, copie JC1060 :
  - ancre = dernière position moteur, horodatée quand l'UI la voit changer ;
  - affiché = max(affiché précédent, ancre + min(écoulé × vitesse, 33 ms)) ;
  - une position moteur en avance sur l'affichage est prise à l'instant
    (snap avant, sans blend) ;
  - l'avance au-delà du moteur est bornée par
    `UI_POSITION_INTERPOLATOR_MAX_LEAD_MS` = 33 ms, soit 1 refresh LVGL
    (`CONFIG_LV_DEF_REFR_PERIOD` = 33). Si le moteur stagne, l'affichage
    s'arrête là et attend ;
  - un recul du moteur (seek, cue, hot cue, loop wrap) est suivi
    immédiatement. Pause, premier sample et scratch (`speed_permille == 0`)
    restent autoritatifs ;
  - `UI_POSITION_INTERPOLATOR_REBASE_THRESHOLD_MS` et
    `UI_POSITION_INTERPOLATOR_SLEW_TAU_MS` sont supprimés.
- Audio : aucun changement.
- Vérification, test scratchpad `t_v231` :
  - lecture en direct, blocs de 5 ms, un commit en retard de 10 ms toutes les
    40 frames : jamais de recul, jamais derrière le moteur, avance max 0 ms ;
  - moteur figé : l'affichage plafonne à +33 ms puis reprend sans recul ;
  - snap avant immédiat ;
  - cue arrière et seek avant immédiats ;
  - dérive de 0,3 % : avance max 0 ms.
- `tests/ui_position_interpolator` (copie P4) suppose une extrapolation
  illimitée avec un snapshot figé. Il échoue donc contre la copie JC1060 ;
  il ne la vise pas, et la copie P4 n'est pas modifiée.

## v232 : FILTER de voie et sélecteur Beat FX sur DDJ-400 (2026-09-24)

- Symptôme : les potards FILTER des deux voies n'ont aucun effet.
- Adresses MIDI : correctes. `B6 17/37` → `mixer.ch1_filter` et `B6 18/38` →
  `mixer.ch2_filter`, identiques au XML Mixxx FLX4 et au XML Mixxx DDJ-400.
  Test hôte : `B6 17 40` + `B6 37 00` → `CTRL_ID_CH1_FILTER` = 8192. Ensuite
  deck_core → `audio_engine_set_filter()`.
- Cause : dans `ae_output`, `.filter_enabled = smart_cfx_enabled`. Le DSP
  filtre de voie ne tourne que si Smart CFX est actif. C'est un bouton FLX4
  (`96 00`) que la DDJ-400 n'a pas, et `s_smart_cfx_enabled` démarre à
  false.
- Correctif, dérivé du profil (sans VID/PID) :
  - `controller_profile_runtime` gagne un callback de changement (activate /
    clear) et `controller_profile_runtime_has_input(type, id)` ;
  - `app_main` enregistre `on_controller_profile_change()`. Il fixe
    `audio_engine_set_channel_filter_needs_smart_cfx(!active || has_input(BUTTON, SMART_CFX))`
    et logue une ligne WARN `channel filter gated by Smart CFX` /
    `always live` ;
  - `ae_output` : `filter_enabled = smart_cfx || !needs_smart_cfx`, soit une
    lecture atomique de bool par bloc, sans allocation et sans changement de
    pacing ni de priorité. FILTER centré = bypass (zone morte ±96 raw). Coût
    CPU quand le filtre est tourné : le même biquad que Smart CFX sur FLX4 ;
  - FLX4 intégré et profil FLX4 SD : inchangés, toujours derrière Smart CFX.
- Beat FX CH SELECT (signalé en cours de v232, « FX sur le master ») :
  - la DDJ-400 envoie `94 10` (CH1), `94 11` (CH2) et `94 14` (MASTER) :
    XML Mixxx `Pioneer-DDJ-400.midi.xml`, quirx `beatFxChannel`. Le
    `state_pair` hérité de la FLX4 attendait `95 11`, donc seul CH1
    fonctionnait ;
  - nouveau type d'entrée `note_select` (raw 8, parseur JC1060 seulement) :
    émet `value` à l'appui, rien au relâchement ;
  - profil : CH1 → 0, CH2 → 1, MASTER → 2 (CH1&CH2). Le P4 n'a pas de
    Beat FX sur le bus master : l'effet est appliqué par deck, avant le
    fader. Écho/Delay suivent donc les faders de voie ;
  - `profile.s3bin` régénéré : 6176 octets, SHA-256
    `b1b4ea2194a75bde750e122e6cad1fc865214116d92e9b89f70f4a0c9b696906`.
    Les fixtures FLX4, generic et Hercules sont identiques octet pour octet.
- Vérification :
  - test scratchpad `t_v232` (parseur et runtime JC1060, PC_TEST) : filtre
    CH1 mappé ; target 0/1/2 à l'appui, rien au relâchement ; `has_input`
    SMART_CFX = false pour la DDJ-400 et true pour la FLX4 ; callback appelé
    à activate et à clear, pas sur un clear redondant ;
  - `audio_engine.c` : même nombre de diagnostics stubs avant et après ;
  - callback `app_main` : `-fsyntax-only -Werror` OK ;
  - `git diff --check` OK. Pas de build firmware.

## v233 — profil SD rejeté en silence, reboot au débranchement de la DDJ

- A, profil SD ignoré (`uses built-in map`, aucun log d'ouverture) :
  - cause : régression v232. Le type raw 8 (`note_select`) a été ajouté au
    parseur, mais pas au garde-fou du manager SD
    (`CPM_MAX_RAW_TYPE` restait 7). `controller_profile_meta_parse` rejetait
    donc le `profile.s3bin` de 6176 octets au scan de boot, sans aucun log ;
  - chemin et moment corrects : `/sd/controllers/<id>/profile.s3bin`, scanné
    par `controller_profile_manager_scan_storage()` juste après
    `bsp_sd_init()` (boot ~2 s), bien avant la résolution du descripteur
    (6,6 s) ;
  - le champ `scale` réutilise les octets réservés 10-11 de l'entrée de
    sortie : aucune taille ne change. L'ancien fichier de 6160 octets était
    valide et le reste ;
  - fix : `CPM_MAX_RAW_TYPE` passe à 8. Chaque rejet est maintenant logué
    (`ctrl_profile: profile rejected: ...`) : taille hors bornes, magic,
    version/header, octets lus vs taille d'en-tête, CRC calculé vs attendu,
    compteurs, taille attendue, type raw trop récent (avec l'entrée MIDI),
    slot de paire, type de sortie. S'y ajoutent l'échec d'ouverture (chemin +
    errno), l'échec d'`opendir`, un résumé WARN
    `N controller profile(s) in /sd/controllers, M valid` et, pour chaque
    fichier, `OK` ou `INVALID`. `uses built-in map` indique aussi le nombre
    de profils du registre et le nombre d'invalides.
- B, débrancher la DDJ reboote la tablette :
  - backtrace (ELF v232, SHA `e44a6054e`) : `controller_task` →
    `close_step` (controller_usb_host.c:504) → `usb_host_device_close` →
    `usbh_dev_close` usbh.c:1334
    `assert(num_ctrl_xfers_inflight == 0)`. Vu deux fois (lignes ~54956 et
    ~55753 de `serial_all.log`), après `USBH: Dev 1 EP 0 Error`, avec l'étape
    de contrôle UAC 4 encore en attente (`uac_blockers=0x2f`) ;
  - cause : course dans esp-usb. `handle_ep0_dequeue()` appelle le callback
    du transfert de contrôle, ce qui réveille `controller_usb` (priorité 6,
    CPU1), avant de décrémenter `num_ctrl_xfers_inflight`. Le daemon
    `usb_hostd` est en priorité 4 sur CPU1. Le callback UAC libère
    `s_control_active`, `poll_cleanup` réussit, puis `close_step` ferme le
    device avant que le daemon ait terminé la décrémentation ;
  - fix, sans toucher au fork : `close_step` note le tick où le cleanup UAC
    (seul utilisateur de EP0) se termine. `usb_host_device_close()` n'est
    appelé qu'après `CONTROLLER_CLOSE_EP0_SETTLE_MS` (20 ms), en rendant la
    main à la boucle client entre-temps, bloquée dans
    `usb_host_client_handle_events`. Le chemin audio n'est pas touché ;
- C, abort au boot sans SD (ligne 54593, boot POWERON) :
  - symptôme : SD attempt 1 en `ESP_ERR_TIMEOUT` (CMD6 high-speed switch,
    0x107), attempts 2-5 en `slot is not available`, puis
    `ESP_ERROR_CHECK(bsp_sd_init())` fait l'abort ;
  - cause : ESP-Hosted (SDIO, slot 1) et la microSD (slot 0) partagent un
    seul contrôleur SDMMC, et `bsp_sd_mount` utilise un init factice.
    En cas d'échec, le helper FATFS appelle `sdmmc_host_deinit_slot(0)`, qui
    retire le slot puis supprime le contrôleur s'il ne reste aucun slot.
    Le pointeur `s_ctlr` du driver legacy n'est pas remis à NULL : chaque
    retry ajoute alors le slot à un contrôleur libéré. Un modèle hôte du
    driver IDF 6.0.2 reproduit exactement le log terrain quand le slot
    ESP-Hosted est absent ;
  - fix `bsp_sd.c` :
    - `host.deinit_p` = `sdmmc_host_release_slot` : ne libère que si un slot
      est enregistré, et suit la suppression du contrôleur au dernier slot
      (`s_ctlr_live`, sans jamais relire le pointeur pendant) ;
    - `host.init` = `sdmmc_host_init_shared` : factice tant qu'ESP-Hosted
      possède le contrôleur, puis vrai `sdmmc_host_init()` s'il a disparu ;
    - retries à `SDMMC_FREQ_DEFAULT`, ce qui évite le CMD6 ;
  - `bsp_sd_init` : 5 tentatives, délai 200/400/600/800 ms ;
  - `app_main` : plus d'`ESP_ERROR_CHECK`. Log E
    `microSD unavailable (...): running without SD - built-in controller map, USB library only`
    et le boot continue. Tous les usages de `/sd` (journal, recorder,
    trackcache, profils, web) testent déjà la présence de la carte ;
  - vérifié sur le modèle hôte : v232 reproduit l'échec, v233 monte à la
    tentative 2 sans use-after-free, le slot ESP-Hosted reste intact quand
    il est présent, et une carte toujours en échec rend 0x107 proprement ;
  - si le slot ESP-Hosted était vraiment absent, le Wi-Fi SDIO de ce boot ne
    fonctionnait déjà pas. v233 recrée le contrôleur pour la SD
    uniquement ; à surveiller.
- Vérification :
  - test scratchpad `t_v233` : le manager v233 accepte les fichiers de 6176
    et 6160 octets, alors que le manager v232 marquait celui de 6176 octets
    invalide. Une copie avec logs activés affiche les bons messages (CRC,
    dossier vide, nom invalide, racine absente). Compilé en
    `-Wformat=2 -Werror` ;
  - `controller_usb_host.c` : diagnostics stubs identiques avant et après ;
  - boucle `bsp_sd_init` et bloc `app_main` compilés isolément en
    `-Wall -Wextra -Werror` et exécutés (montage à la tentative 3, puis
    échec total → boot continue) ;
  - pas de build firmware ni de test matériel.

## v234 — la DDJ ne revient pas au rebranchement (2026-09-24)

- Symptôme (HIL v233) : débranchement propre à 175,5 s (`DEV_GONE`, fermé en
  57 ms), re-arm #1 à 180,6 s, `NEW_DEV addr=5` à 182,5 s, puis
  `recovery: closing controller (gone=0 uac=0 uac_blockers=0x40)` et plus
  aucun `NEW_DEV` ; re-arms #2 à #31 toutes les 15 s sans effet.
- Cause, d'après le log complet :
  - la fermeture ne vient pas d'une requête en attente. `probe_device`
    échoue au claim de l'interface MIDI :
    `USBH: EP Alloc error: ESP_ERR_NO_MEM` →
    `Claiming interface error: ESP_ERR_NO_MEM`. Son chemin d'erreur ferme
    notre handle (`close_step`), d'où le log « closing controller » ;
  - le `NO_MEM` vient de la liste qTD du pipe
    (`heap_caps_aligned_calloc(512, …, MALLOC_CAP_DMA | INTERNAL)`,
    `hcd_dwc.c`) : la RAM interne DMA est épuisée. Le diag preload donne
    environ 18 Ko de RAM interne libre en lecture sur v229 à v233, contre
    91 Ko au premier probe du boot ;
  - le périphérique reste énuméré sur root 0 : pas de nouveau `NEW_DEV`.
    Le manager ne coupe qu'un root déconnecté
    (`pajoniiir_hcd_port_power_off_if_disconnected` → `NOT_FINISHED`), donc
    chaque re-arm est refusé. Le log « recovery suppressed » était en INFO,
    filtré, et le refus ressemblait à un succès silencieux ;
  - `0x40` = `CONTROLLER_UAC_BLOCK_DEVICE_GONE` (`controller_usb_audio`).
    Le `DEV_GONE` est arrivé après la fin du cleanup UAC
    (`uac_blockers=0x00`) : `request_stop(true)` a mémorisé un flag que
    seul le chemin « stopping » efface. Il est cosmétique et ne ferme rien.
- Fix :
  - `controller_usb_host.c` : un probe qui échoue après énumération (claim,
    alloc URB, submit) arme un re-probe de la même adresse, avec un backoff
    de 250 ms doublé jusqu'à 5 s. Il s'arrête sur succès, ou quand l'open
    rend `NOT_FOUND`/`INVALID_STATE` (appareil parti). Pendant le re-probe,
    `rearm_root_if_silent` ne demande plus de power-cycle (il serait
    refusé). Chaque échec logue l'étape, la RAM interne et DMA
    free/largest (tentatives 1-3 puis une sur 12). Un succès logue
    `probe addr=N succeeded on retry K` ;
  - `controller_usb_audio_stream.c` : `request_stop` ne mémorise
    `device_gone` que s'il reste quelque chose à arrêter (plus de 0x40
    périmé) ;
  - `usb_host_manager.c` : « recovery suppressed » passe en WARN.
- Hors fix : la RAM interne DMA reste le vrai goulot. Si le re-probe logue
  `DMA free` proche de 0 pendant longtemps, il faudra trouver le
  consommateur interne. Aucun changement audio, pacing ou priorité ici.
- Vérification : les trois fichiers modifiés passent un contrôle de syntaxe
  hôte `-Wall -Wextra` (vrais headers `usb`, stubs FreeRTOS/esp_err/heap) :
  - `controller_usb_host.c` et `controller_usb_audio_stream.c` : propres,
    `-Werror` ;
  - `usb_host_manager.c` : seule erreur préexistante,
    `fifo_settings_per_port`, ajouté au build par le patch cmake du fork.

  Pas de build firmware ni de test matériel.

## v235 — pas d'audio après rebranchement (2026-09-24)

- Symptôme (HIL v234) : débranchement propre à 46,4 s, puis au
  rebranchement `NEW_DEV addr=5`, MIDI OK et
  `re-attached after 5574 ms (... uac=0)`. Le HB UAC reste ensuite en
  `rc=0x103 claimed=0 cfg_fail=1` : pas d'audio jusqu'au reboot.
- Cause, d'après le log : ce n'est pas une course avec l'ancien handle, qui
  était fermé depuis 5 s (`closed in 44 ms`). Le claim UAC (ifc 1 alt 2)
  échoue sur la même allocation qu'en v234 :
  `USBH: EP Alloc error: ESP_ERR_NO_MEM` → `Claiming interface error` →
  `DDJ-400 UAC unavailable; MIDI remains active: ESP_ERR_NO_MEM`. Le pipe
  isochrone demande deux listes qTD de 512 octets, alignées sur 512, en RAM
  interne DMA. Cette fois le MIDI est passé et l'UAC non. Le start n'était
  jamais retenté.
- Fix :
  - `controller_usb_host.c` : un start UAC qui échoue (sauf
    `NOT_SUPPORTED`/`INVALID_ARG`, aucun format utilisable) est retenté
    avec le même backoff que le re-probe v234 : 250 ms doublé jusqu'à 5 s.
    Il s'arrête sur succès, ou à la fermeture du contrôleur (DEV_GONE,
    faute) ; un nouveau probe repart de zéro ;
  - si un start a échoué après son claim, on attend que `poll_cleanup` ait
    libéré l'interface (quiesced) avant de retenter, sans compter
    d'essai ;
  - chaque échec logue `UAC start addr=N: <err> ... retry #K in D ms`,
    avec la RAM interne et DMA free/largest (essais 1-3 puis un sur 12).
    Une réussite logue `recovery: UAC start addr=N succeeded on retry K` ;
  - `controller_usb_audio_stream.c` : un claim refusé logue
    `UAC claim ifc I alt A ep 0xEE mps M: <err>`. Les échecs de
    SET_INTERFACE et des étapes de débit loguaient déjà
    `control step N status=S`.
- Limite : le retry ne crée pas de RAM. Si les logs montrent `DMA largest`
  durablement sous ~1 Ko, il faudra une réserve DMA gardée à la
  déconnexion, ou trouver le consommateur interne.
- Vérification :
  - `controller_usb_host.c` et `controller_usb_audio_stream.c` passent un
    contrôle de syntaxe hôte `-Wall -Wextra -Wformat=2 -Werror` ;
  - test scratchpad `t_v235`, qui inclut `controller_usb_host.c` avec
    stubs : 6 échecs `NO_MEM` donnent les délais 250/500/1000/2000/4000/5000
    ms, puis succès au 7e start avec `usb_audio_active`. Pendant l'attente
    quiesced, aucun start n'est lancé ni compté. `NOT_SUPPORTED` n'est pas
    retenté ;
  - pas de build firmware ni de test matériel.

## v236 — réserve DMA interne pour le rebranchement (2026-09-24)

- Symptôme (HIL v235) : au rebranchement, le probe échoue en boucle au claim
  MIDI (stage 7 = `INTERFACE_CLAIM`) :
  `probe addr=5: ESP_ERR_NO_MEM ... internal free=37679 largest=7168, DMA free=23779 largest=2304`.
  Ni MIDI ni audio. Le retry UAC v235 n'est jamais atteint.
- Cause : la RAM interne DMA est fragmentée. Chaque buffer d'endpoint
  réserve une liste qTD alignée sur 512 (`heap_caps_aligned_calloc`,
  `hcd_dwc.c`), soit environ 0,8 Ko contigu en INTR et 1 Ko en ISOC.
  Un bloc de 2304 octets ne tient pas les quatre listes du claim MIDI, et
  les URB isoc UAC font 2304 octets chacun. Au boot, les allocations USB se
  sont dispersées entre celles du reste du système ; libérées à l'unplug,
  elles laissent des trous trop petits.
- Choix : réserve prise une fois au boot, découpée en morceaux.
  - Un pool statique dédié demanderait de modifier le fork esp-usb, hors
    périmètre : ses allocateurs appellent `heap_caps_*` avec des caps
    fixes.
  - Une réserve prise seulement au DEV_GONE arriverait trop tard : le heap
    est déjà fragmenté à ce moment-là.
- Fix (`controller_usb_host.c`) :
  - `controller_usb_host_init` prend 6 morceaux de 2560 octets en
    `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`, environ 15 Ko. Un morceau
    contient un URB isoc de 2304 octets ou deux listes qTD alignées dans le
    pire cas. Six couvrent les environ 12 Ko de la DDJ-400 : 4 listes INTR,
    2 ISOC, 3 URB isoc et les petits URB ;
  - la réserve est libérée juste avant le claim MIDI, première allocation
    DMA du probe. Les échecs antérieurs (open, descripteurs, appareil non
    MIDI, clé USB) ne la touchent pas ;
  - elle est reprise à la fin de chaque fermeture (unplug, faute, échec de
    probe) : les buffers du contrôleur, placés dans ces trous, s'y
    recoalescent. Si elle est incomplète, elle est complétée toutes les
    1 s tant qu'aucun contrôleur n'est ouvert (log seulement si le compte
    change) ;
  - logs : `USB DMA reserve N/6 chunks ...` (boot, fermeture, complément),
    `USB DMA reserve: N chunks released for the claim`, et
    `controller closed ... DMA freed ~X B`, qui mesure ce que tenait le
    contrôleur et sert à caler la taille.
- Empreinte : contrôleur branché, la réserve est remplacée par ses propres
  allocations, donc le régime permanent ne change pas. Contrôleur absent,
  environ 15 Ko restent tenus, ce qui correspond au régime « branché »
  validé (environ 18 Ko libres). Pas de libération après X s : la réserve
  servirait justement au rebranchement suivant.
- Risques : au boot, il y a 15 Ko de RAM interne DMA en moins entre
  `controller_usb_host_init` et le premier claim ; à surveiller.
  L'énumération elle-même (EP0 et URB d'énumération) n'est pas couverte ;
  elle est passée en v234 et v235. Si `DMA freed` dépasse nettement 15 Ko,
  augmenter `CONTROLLER_DMA_RESERVE_CHUNKS`.
- Vérification :
  - `controller_usb_host.c` passe un contrôle de syntaxe hôte
    `-Wall -Wextra -Wformat=2 -Werror` ;
  - test scratchpad `t_v236` : remplissage partiel au boot (4/6),
    complément silencieux s'il ne change rien, complément à 6/6, libération
    totale et idempotente, reprise après fermeture ;
  - `t_v235` repasse ;
  - pas de build firmware ni de test matériel.

## v237 — faute UAC au rebranchement, port root mort ensuite (2026-09-24)

- Constat HIL v236 :
  - la réserve libère 2 morceaux, le probe passe, `re-attached uac=1`, puis
    `control step 1 complete` ;
  - 360 ms plus tard : `failed to prime UAC isochronous queue` →
    `UAC stream fault (ESP_FAIL)` → fermeture (blockers 0x23) ;
  - `fault power-cycle request root 0: ESP_OK`, puis
    `USB0 recovery suppressed: attach/enumeration is active`, et plus aucun
    NEW_DEV.
- Boot et rebranchement ont la même séquence : claim MIDI, claim UAC,
  SET_INTERFACE (step 1), prime, SET_CUR/GET_CUR (steps 3/4). La différence
  est la mémoire :
  - `prime_and_ready()` alloue les 3 URB isoc (3 × 2304 octets DMA interne)
    depuis le callback du step 1, donc après la fin du burst du probe ;
  - au boot, le plus grand bloc DMA faisait 11-15 Ko ; au rebranchement,
    les morceaux restants de la réserve libérée avaient déjà été pris par de
    petites allocations (`SPIRAM_MALLOC_ALWAYSINTERNAL`) ;
  - l'allocation échoue avec NO_MEM, et le prime appelle `mark_fault(true)`.
  - Ni le step 4 ni le claim sur un appareil déjà configuré ne sont en cause :
    le step 1 se termine, et le step 3 n'est jamais soumis.
- Port mort : la recovery du manager n'éteint qu'un root déconnecté
  (`usb_host_lib_power_off_root_port_if_idle_by_index`). Après la faute, la
  DDJ reste énumérée et plus aucun client ne la possède : requête refusée,
  pas de déconnexion, donc pas de NEW_DEV. C'est le cas que la recovery
  upstream de `usb_storage` couvre (cycle forcé, répété).
- Fix (`controller_usb_audio_stream.c`) :
  - les URB isoc sont alloués dans `controller_usb_audio_stream_start()`,
    juste après le claim et donc dans le burst du probe ou du retry. Un échec
    y est synchrone (`UAC isoc URB i alloc (2304 B): ...`) et passe par le
    retry UAC v235, pas par une faute de stream ;
  - `prime_and_ready()` ne fait plus que soumettre. L'allocation y reste
    comme repli, et l'échec est journalisé avec l'URB, l'étape et le rc.
- Fix (`controller_usb_host.c`), réserve DMA juste-à-temps :
  - `dma_reserve_release(why)` ouvre un burst ;
  - `dma_reserve_settle(why)` le ferme à la fin du probe (après le start
    UAC) et reprend `(libéré − consommé) / 2560` morceaux ;
  - le retry UAC libère ce qui est tenu, puis reprend le reste ;
  - l'empreinte totale ne dépasse pas celle de la réserve au boot ;
  - `USB DMA reserve: probe took ~N B, k/6 chunks re-taken` montre la
    consommation réelle.
- Fix (`controller_usb_host.c`), recovery du root selon le pattern upstream
  `usb_storage` :
  - power-off forcé (`usb_host_manager_set_root_power_by_index(root,false)`),
    150 ms, puis power-on. INVALID_STATE est réessayé toutes les 20 ms
    pendant 1 s maximum, le temps que le daemon traite la déconnexion ;
  - répété toutes les 900 ms, puis toutes les 30 s après 8 cycles, jusqu'à
    ce qu'un appareil soit probé sur ce root. Un NEW_DEV sur le root
    stockage ne l'arrête pas ;
  - armé immédiatement après la fermeture sur faute, et 900 ms après un
    débranchement. Il remplace la requête one-shot et le re-arm idle-only
    5 s/15 s (`rearm_root_if_silent` supprimé) ;
  - exécuté après la file des probes, contrôleur fermé uniquement ;
  - logs : `power-cycling root N`, `power-cycle #k off=... on=...` et
    `power-cycle loop stopped after k cycles (NEW_DEV probed on it)`.
- Risques :
  - un cycle forcé pendant une énumération lente (plus de 900 ms) la coupe.
    La cadence lente de 30 s laisse de toute façon la place ;
  - le task contrôleur bloque environ 150-1150 ms par cycle, seulement quand
    il est fermé.
- Correctif structurel non appliqué (sdkconfig interdit ici) :
  `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y` sortirait ces buffers de
  la RAM interne fragmentée.
- Vérification :
  - syntaxe hôte `-Werror` OK sur `controller_usb_host.c`,
    `controller_usb_host_routed.c` et `controller_usb_audio_stream.c` ;
  - test scratchpad `t_v237` : settle partiel, puis nul, puis limité par un
    malloc qui échoue ; cadence 900 ms ×8 puis 30 s ; retry du POWER_ON
    borné ; root inconnu ignoré ; arrêt uniquement sur un probe du bon root ;
    faute armée immédiatement ;
  - test `t_v237u` sur le stream : NO_MEM synchrone sans soumission ni
    fuite, prime sans allocation ;
  - `t_v235` et `t_v236` repassent ;
  - pas de build firmware ni de test matériel.

## v238 — redémarrage complet du host USB (2026-09-24)

- Résultat HIL v237 : le power-cycle forcé du root ne marche pas sur ce
  fork. Les cycles #1-4 donnent off=OK on=OK, puis #5-#9 donnent
  on=ESP_ERR_INVALID_STATE en boucle. Aucun NEW_DEV ne suit le
  rebranchement : après un power-off forcé, ce root n'émet plus
  d'événement de connexion. Piste abandonnée.
- Fix : escalade vers un cycle complet du Host Library, comme au boot. Le
  même `usb_host_config_t` est réinstallé, ce qui recrée aussi le hub
  virtuel ; c'est la seule voie prouvée pour réénumérer ici.
- `usb_host_manager` :
  - API `usb_host_manager_request_host_restart(why)`, non bloquante.
    Participants : `..._restart_participant_register(name)`,
    `..._host_restart_pending()`, `..._host_generation()`,
    `..._host_restart_release(id, gen)` et `..._wait_host_restart(gen)` ;
  - la requête passe l'état READY -> RESTARTING et réveille le daemon par
    `usb_host_lib_unblock()`. Le restart s'exécute sur le daemon (CPU1,
    prio 4), là où `usb_host_install` doit allouer les interruptions DWC ;
  - séquence :
    1. attente bornée (5 s), avec `handle_events` qui continue de tourner,
       que chaque participant libère son client. Le client topologie se
       désinscrit seul ;
    2. log `released=0x../0x.. clients= devices=`, plus le nom de chaque
       participant qui n'a pas libéré ;
    3. `usb_host_device_free_all()`, puis attente d'ALL_FREE (2 s) ;
    4. `usb_host_uninstall()`, réessayé pendant 1 s tant qu'il reste des
       flags en attente ;
    5. réinstallation (3 tentatives) et nouvelle tâche topologie. La
       génération est incrémentée en dernier ;
  - un restart ne peut pas être forcé si un client est encore inscrit :
    `usb_host_uninstall()` renverrait INVALID_STATE, et le handle du client
    pointerait ensuite vers de la mémoire libérée. Dans ce cas, le détail
    est journalisé, l'ancien stack est gardé et la topologie est relancée
    dessus. Les participants se réinscrivent sur ce qui existe ;
  - `host_call_enter/exit` compte les appels Host Library faits hors du
    daemon (power root, info, recovery). Le daemon attend zéro avant
    l'uninstall. La recovery par root est suspendue tant que l'état n'est
    pas READY.
- Contrôleur (`controller_usb_host.c`) :
  - après 3 power-cycles sans probe sur le root, un restart host est
    demandé (`full USB host restart k/3`), au plus 3 fois par épisode. Les
    power-cycles, la réserve DMA et les retries v234-v237 restent ;
  - `host_restart_participate()` ferme le contrôleur s'il est ouvert, puis
    désinscrit le client, libère, attend, se réinscrit, vide la file des
    probes (anciennes adresses) et rallume
    `config.root_port_index` (nouveau champ, `LOCAL_USB1_ROOT_INDEX`) ;
  - le prochain power-cycle attend 5 s, le temps que le stack réinstallé
    énumère.
- Stockage (`usb_storage.c`, hooks `USB_STORAGE_HOST_RESTART_*`, no-op hors
  adaptateur partagé) :
  - `storage_task`, seul propriétaire, démonte et libère la clé ;
  - `usb_lib_task` fait `msc_host_uninstall()`, puis libère et attend ;
  - ensuite `msc_host_install()`, reset de la session et recovery du root 1
    (`host restart`), exactement comme au boot. Remontage de la
    bibliothèque : environ 5 s ;
  - si la libération dépasse 4 s, le stockage garde tout (le manager
    abandonne le restart).
- Audio : aucun changement dans le mix, ni en pacing ni en priorité. Le
  restart ne tourne que sur CPU1 (daemon prio 4, tâche contrôleur).
- Risques :
  - un morceau lu depuis la clé USB est coupé par le démontage ;
  - la pile du daemon (4096) porte maintenant aussi uninstall et
    réinstallation, à surveiller au premier HIL ;
  - avec un client non libéré, pas de restart (voir ci-dessus).
- Vérification :
  - syntaxe hôte `-Werror` OK sur `usb_host_manager.c` (hors champ
    `fifo_settings_per_port` ajouté par le patch du fork),
    `controller_usb_host_routed.c` et `usb_storage_shared.c` ;
    `usb_storage.c` compile aussi seul, avec les macros par défaut ;
  - test `t_v238` (contrôleur) : escalade après 3 cycles, puis
    participation complète (timeout d'attente et échecs de register
    réessayés), désinscription refusée, budget de 3 restarts, restart
    refusé qui retombe sur le power-cycle ;
  - test `t_v238m` (manager, faux Host Library) :
    - restart nominal : ALL_FREE attendu, uninstall réessayé, réinstallation
      et génération +1 ;
    - requête en double refusée ;
    - power root refusé pendant le restart ;
    - release d'une génération passée ignoré ;
    - participant absent : abandon, ancien stack gardé ;
    - échec de réinstallation : FAILED ;
  - `t_v235`, `t_v236` et `t_v237` repassent (t_v237 avec restart refusé) ;
  - pas de build firmware ni de test matériel.

## v239 — retour à v237 + réserve DMA, ré-ouverture du stream UAC (2026-09-24)

- Rejet v238 (opérateur) : le restart complet du host démonte la clé MSC et
  la bibliothèque ne revient pas. Le morceau en lecture est perdu :
  inacceptable. L'escalade est désactivée, le code reste en place :
  - `CONTROLLER_HOST_RESTART_ENABLED 0` : aucune requête
    `usb_host_manager_request_host_restart()`, pas de participant
    contrôleur ;
  - le code manager et stockage v238 reste compilé, mais personne ne
    demande de restart. Le stockage reste inscrit comme participant, sans
    effet.
- Relecture de la trace v236 : après l'unplug, la réserve DMA est libérée,
  le probe réussit (`uac=1`), puis `control step 1 complete, priming
  isochronous queue` à 28,6 s, et 360 ms plus tard `UAC stream fault
  (ESP_FAIL)`, ce qui ferme tout le contrôleur. L'énumération n'est donc pas
  en cause : c'est la (ré)ouverture du stream.
  - `cfg_fail=1` : faute de configuration. Après le step 1, seul le prime
    (alloc ou submit des URB isoc) peut la produire. Les steps 3/4 (rate)
    sont ignorés en cas d'échec et ne font jamais de faute. Un claim
    refusé est synchrone et aurait donné `uac=0`.
  - Le log du prime v236 n'avait pas de code retour, donc la cause exacte
    (NO_MEM ou INVALID_STATE/INVALID_SIZE du HCD) n'est pas prouvée.
  - `usb_host_client_handle_events` traite les événements puis rend la
    main : la faute est détectée quasi immédiatement. Les 360 ms sont
    compatibles avec le retard de la console UART bloquante (7-10 ms par
    ligne).
  - Le HIL v237 n'a probablement jamais testé un re-attach UAC : le
    power-cycle forcé, armé 900 ms après l'unplug, tuait le root avant.
  - Après un unplug physique, la DDJ-400 repart hors tension, sans état
    d'interface résiduel. Cet état ne compte que pour un close sans unplug
    (retry in-place, re-probe). SET_CONFIGURATION 0 est exclu : usbh
    possède la configuration.
- Changements (`controller_usb_audio_stream.c`) :
  - chaque start porte un numéro `UAC seq N`. Chaque étape est loggée avec
    son rc et un horodatage relatif :
    - `start dev=` (premier start ou répétition) ;
    - claim `ifc/alt/ep/mps` ;
    - alloc des 3 URB isoc et de l'URB control ;
    - submit du step 5 (`SET_INTERFACE alt 0`) et du step 1
      (`SET_INTERFACE alt N`), avec leur status ;
    - `control step 1 complete (alt N)` ;
    - prime, avec URB, alloc/submit et rc ;
    - `UAC ready ... seq N +ms` ;
  - `FAULT at <site>: <rc> (... streaming N ms, N isoc URBs completed)`.
    Sites : isoc URB status, isoc resubmit, control step 1 submit/status,
    prime submit/alloc, ring init, isoc URB alloc, configuration start. Le
    rc est exposé dans `stats.fault_rc` et `stats.start_seq` ;
  - `isoc_callback` capture sans logger le premier URB terminé et le
    premier paquet en échec (index, status, actual/wanted).
    `controller_usb_audio_stream_log_trace()` les imprime une fois, depuis
    la tâche contrôleur, et seulement quand le stream ne tourne pas : aucun
    log UART ne retarde les resubmits ;
  - `stop: gone/faulted/fault/blockers`, `stop: ep halt/flush` et
    `stop done: URBs freed, ifc N released|was not claimed` montrent si
    l'ancien handle a vraiment libéré l'interface. Un échec de release est
    loggé une seule fois ;
  - `STREAM_RESET_ALT_ON_RESTART 1` : à partir du 2e start depuis le boot,
    la séquence commence par `SET_INTERFACE alt 0` (step 5) puis
    `alt N`. Un STALL sur alt 0 est ignoré. Le premier start du boot
    (chemin qui marche) est inchangé : alt N directement ;
  - on garde de v237 l'alloc des URB dans `stream_start` (réserve DMA).
- Changements (`controller_usb_host.c`) :
  - `CONTROLLER_ROOT_CYCLE_ENABLED 0` : plus de power-cycle forcé du root
    (il le tuait en v237). Après un unplug, on attend NEW_DEV
    (`root power-cycle disabled (v239), waiting for NEW_DEV`) ;
  - une faute UAC ne ferme plus le contrôleur. `restart_uac_in_place()`
    arrête seulement le stream, puis le retry v235 le relance quand il est
    quiesced, avec libération et settle de la réserve DMA. Log : `UAC
    stream fault (rc, seq N) ... restarting the stream in place in N ms,
    MIDI stays up` ;
  - une faute MIDI/contrôleur sans unplug ferme puis re-probe la même
    adresse (`re-probing addr=N in N ms (device still enumerated)`) ;
  - backoff par série de fautes : 250 ms, doublé jusqu'à 5 s, remis à zéro
    après 30 s sans faute ou à l'unplug. Compteurs séparés pour UAC et
    contrôleur.
- Audio : rien ne change dans le mix, le pacing ou les priorités. Les
  nouveaux logs tournent hors streaming, sauf les fautes (stream déjà mort).
- À chercher dans le log HIL : `UAC seq`, `FAULT at`, `stop:`, `first isoc
  URB`, `first failed isoc packet`, `restarting the stream in place`,
  `re-probing addr`, `SET_INTERFACE alt 0`.
- Risques :
  - si la faute est structurelle (DMA ou HCD), le stream redémarre en
    boucle, avec au plus un essai toutes les 5 s. MIDI et la clé MSC ne sont
    pas touchés ;
  - sans power-cycle, un root réellement bloqué n'a plus de recovery
    automatique ;
  - alt 0 avant alt N n'est pas encore validé sur la DDJ-400.
- Vérification :
  - syntaxe hôte `-Werror` OK sur `controller_usb_host_routed.c` (macros à
    0, et variante à 1) et sur `controller_usb_audio_stream.c` ;
  - `t_v239u` (stream) : premier start en alt N direct, trace muette
    pendant le streaming puis imprimée après stop, start répété alt 0 ->
    alt N -> prime, STALL sur alt 0 ignoré, STALL sur alt N en faute avec
    rc, échec de submit au prime avec `fault_rc=INVALID_STATE`, start
    suivant propre ;
  - `t_v239c` (contrôleur) : backoff 250 ms -> 5 s puis reset, restart UAC
    in-place sans close (ignoré si gone ou closing), re-probe de la même
    adresse, pas de power-cycle armé à l'unplug ;
  - `t_v235` et `t_v236` repassent tels quels. `t_v237` repasse avec
    `ROOT_CYCLE_ENABLED=1` ; `t_v238` repasse avec les deux macros à 1.
    `t_v237u` échoue par construction (son 2e start attend l'ancien alt N
    direct), couvert par `t_v239u` ;
  - pas de build firmware ni de test matériel.

## v240 — buffers DMA USB en PSRAM (2026-09-25)

- Résultat HIL v239 : diagnostic net. Après le replug, la boucle de restart
  in-place tourne (seq 13-16). À chaque essai, `claim ifc 1 alt 2 ep 0x01
  mps 576: ESP_OK` puis `FAULT at isoc URB alloc: ESP_ERR_NO_MEM`, stop
  propre, retry 5 s, indéfiniment. MIDI et clé MSC restent sains. Cause :
  heap DMA interne trop fragmenté après l'unplug pour les URB isoc, même
  avec la réserve (6 x 2560 B).
- Fix (approuvé par l'opérateur) : `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`
  dans `sdkconfig.defaults`.
  - Le flag existe dans le fork épinglé (`esp-usb` `cc65dc26`,
    `host/usb/Kconfig`, `depends on IDF_TARGET_ESP32P4 && SPIRAM`). Il
    bascule `DATA_BUFFER_CAPS` (`usb_private.c`, données de tous les URB)
    et `XFER_DESC_LIST_CAPS` (`hcd_dwc.c`, listes qTD des pipes) sur
    `MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM`. La
    frame list par port reste interne ; elle est allouée une seule fois,
    à l'installation.
  - `esp_heap_adjust_alignment_to_hw()` retire `MALLOC_CAP_DMA` quand
    `MALLOC_CAP_SPIRAM` est demandé (aucune région n'a les deux caps) et
    aligne taille et adresse sur la ligne de cache. Vérifié dans les
    sources IDF 5.5.4 disponibles localement ; 6.0.2 non présent sur la
    machine de revue.
  - La cohérence de cache est déjà gérée : sur P4
    (`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`), le HCD fait `esp_cache_msync`
    sur chaque buffer, en interne comme en PSRAM.
  - Portée globale : MIDI, UAC et MSC (bulk) passent en PSRAM, ce qui
    enlève aussi les buffers MSC du heap DMA interne.
- `controller_usb_host.c` : la réserve DMA interne n'a plus rien à
  protéger. Avec le flag, `CONTROLLER_DMA_RESERVE_ENABLED 0`, ce qui rend
  environ 15 KB de RAM interne. Au boot, un log l'indique : `USB DMA
  reserve off: USB-DWC buffers in PSRAM`. Sans le flag (sdkconfig non
  régénéré), la réserve v236/v237 reste active telle quelle.
- `controller_usb_audio_stream.c` : le log d'alloc indique où sont les
  buffers : `3 isoc URBs (2304 B, PSRAM|internal)`.
- Impact isochrone :
  - le flux OUT fait 576 B/ms, soit environ 0,6 MB/s, négligeable face au
    débit PSRAM à 200 MHz ;
  - le DWC lit chaque paquet via AHB pendant la frame. Les pics de latence
    PSRAM (DSI/PPA/LVGL) sont de l'ordre de la µs, contre une frame de
    1 ms ;
  - la file garde 3 URB x 4 paquets d'avance ;
  - côté CPU, l'écriture des échantillons dans le buffer et le msync sont
    du même ordre qu'en interne (P4 passe aussi par le cache pour
    l'interne) ;
  - aucun changement de mix, de pacing ou de priorité.
- Risques :
  - le sdkconfig local contient `# CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM
    is not set`, que les defaults ne remplacent pas. Il faut supprimer
    `sdkconfig` avant le build, sinon le flag est ignoré. Le log de boot et
    `PSRAM|internal` le montrent ;
  - Kconfig le signale comme « minor performance degradation ». Le débit
    de lecture MSC depuis la clé est à vérifier pendant la lecture ;
  - l'underrun isoc sous forte charge PSRAM (UI) reste à valider en HIL.
- Vérification :
  - syntaxe hôte `-Werror` OK, `controller_usb_host_routed.c` avec et sans
    le flag, et `controller_usb_audio_stream.c` ;
  - `t_v240` : réserve active sans le flag (6 chunks, release/settle),
    aucune allocation avec le flag ;
  - `t_v239u` (avec `PSRAM` dans le log), `t_v239c`, `t_v235` et `t_v236`
    repassent ;
  - pas de build firmware ni de test matériel.
