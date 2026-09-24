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
