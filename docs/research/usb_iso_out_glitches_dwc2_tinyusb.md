# Recherche web : glitches audio isochronous OUT — DWC2 host / TinyUSB / UAC1 (DDJ-400)

Contexte projet : ESP32-P4 (DWC2 HS via UTMI, TinyUSB vendored) en host USB,
DDJ-400 UAC1, 4 canaux 44,1 kHz, alt setting S16 8 B ou S24 12 B,
pas de feedback EP (cadence fixe = endpoint adaptive côté DAC), pas de
Feature Unit. Host prouvé propre (périodicité iso stable, aucun silence
injecté, segmentation 88 B/microframe testée sans effet). DDJ validé parfait
sur PC Linux.

## 1. Causes possibles classées par plausibilité

### A. Côté device (DAC DDJ-400) — adaptive endpoint sans feedback

1. **FIFO interne du DAC trop court / vidage à cadence fixe inexacte.**
   Un endpoint *adaptive sink* dérive son clock du flux reçu et rejoue ce qui
   arrive ; sans feedback EP, l'hôte envoie exactement le nominal
   (44 100 × 4ch × S16 = 176 400 B/s = 176 B/frame à 1 kHz full-speed).
   Toute dérive d'horloge entre le SOF du host et le xtal/MCLK du DAC se
   traduit par un sample répété/drop localement → clics périodiques.
   Réfs :
   - Apple TN3190 (synchronisation endpoints, limites ADC1) :
     https://developer.apple.com/documentation/technotes/tn3190-usb-audio-device-design-considerations
   - UAC1 spec (adaptive OUT, pas de feedback requis pour sink adaptive) :
     https://www.usb.org/sites/default/files/audio10.pdf
   - Nordic DevZone, drift adaptive : https://devzone.nordicsemi.com/f/nordic-q-a/49513/usb-audio-adaptive-transfer-rate
   - Primer asynchronicité (jitter adaptive) : https://audiophilestyle.com/ca/bits-and-bytes/Asynchronicity-USB-Audio-Primer/

2. **Sensibilité au jitter de départ/espacement des transactions OUT.**
   Les DACs PCM29xx-style (famille Burr-Brown/TI utilisée dans beaucoup de
   surfaces DJ) verrouillent leur PLL/clock recovery sur les SOF ; un espacement
   irrégulier des transactions OUT dans le microframe (burst au lieu de réparti)
   peut faire dériver le PLL et produire des artefacts alors que le débit moyen
   est exact. La datasheet TI PCM2901/2903 décrit explicitement l'adaptive mode
   playback avec buffer interne de 1 ms — un under-buffer de 1 ms est fragile.
   Réfs :
   - TI PCM2901/2903 datasheet (adaptive mode, buffer interne) :
     https://www.ti.com/tw/lit/gpn/pcm2903
   - PCM2902 (même famille) : https://www.ti.com/lit/gpn/pcm2902

3. **La segmentation 88 B/microframe n'était pas la bonne hypothèse.**
   En UAC1 le device attend (généralement) un packet/frame FS de 1 ms — le DDJ
   a été validé sur Linux, dont le hcd envoie typiquement 1 transaction de
   176 B par frame, pas des sous-packets. Si le P4 envoie en HS avec
   1–3 transactions par microframe, le comportement du DAC diffère : tester
   « un seul packet de 176 B par frame de 1 ms » au lieu de 88 B/microframe.

### B. Côté host — DWC2 sur ESP32-P4

4. **HCD DWC2/TinyUSB : planning périodique iso OUT.**
   Sur DWC2, les canaux iso sont programmés via la Host Frame List ; bInterval
   iso HS doit être 1..16 (1 = chaque microframe). Les notes Espressif
   (« USB Host Maintainers Notes (DWC_OTG Controller) ») décrivent le modèle
   channel/frame-list : erreurs classiques = FIFO périodique trop petit,
   re-programmation du channel manquée à chaque (micro)frame → packets
   silencieusement jetés (iso n'a pas de retry).
   Réfs :
   - Notes Espressif DWC_OTG (bInterval via frame list, §6.5) :
     https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32s2/api-reference/peripherals/usb_host/usb_host_notes_dwc_otg.html
   - Goodpenguin, « Why USB Isochronous Bandwidth Errors Occur » (règle des
     80 % de microframe, bInterval, scheduling) :
     https://www.thegoodpenguin.co.uk/blog/understanding-why-usb-isochronous-bandwidth-errors-occur/
   - TinyUSB USB Concepts : https://docs.tinyusb.org/en/latest/reference/usb_concepts.html

5. **Errata/limitations DWC2 & TinyUSB — iso reste le parent pauvre.**
   - Synopsys OTG 2.65a : troncature occasionnelle de transfers iso (DATA0 au
     lieu de DATA2, reste non envoyé) — même famille de core :
     https://stackoverflow.com/questions/10337174/synopsys-usb-otg-controller-2-65a-occasionally-truncates-isochronous-in-in-usb
   - TinyUSB #1618 « My successes and failures with TinyUSB (DWC2 mostly) » :
     problèmes de data toggle/seq rx-tx-rx vs tx-tx :
     https://github.com/hathach/tinyusb/issues/1618
   - TinyUSB discussion #2433 : perte de packets iso **par paires** (HS) —
     cause finale : hub/analyseur intermédiaire (signal integrity), pas le code :
     https://github.com/hathach/tinyusb/discussions/2433
   - TinyUSB discussion #2620 : taille de buffer EP OUT vs IN en UAC
     (192 vs 196 B) — piège d'alignement/padding :
     https://github.com/hathach/tinyusb/discussions/2620
   - TinyUSB #3635 : limites iso UAC2 et réponses err/0 byte quand le buffer
     EP est mal dimensionné :
     https://github.com/hathach/tinyusb/discussions/3635
   - Device-specific known issues (page officielle TinyUSB, core très buggy
     listé avec 17 errata dont iso IN) :
     https://docs.tinyusb.org/en/latest/reference/device_issues.html
   - ChibiOS STM32 : iso OUT exige transfer size/packet count = nb exact de
     packets max-size — même exigence côté DWC2 host :
     https://forum.chibios.org/viewtopic.php?t=926

6. **Priorité arbitrage microframe / contention interne.**
   Si d'autres transfers (MIDI interrupt IN, hub) tournent dans le même
   microframe, l'arbitre DWC2 peut décaler l'OUT iso ; à bInterval=1 ça passe
   ou se perd sans trace côté hcd sauf si on lit les bits d'état du channel
   (HCINT xacterr/frmovrun). Vérifier que le EP MIDI du DDJ et l'EP iso ne
   se marchent pas dessus.

7. **Signal integrity / alimentation VBUS.**
   #2433 ci-dessus s'est résolu en rebranchant direct : à garder en tête pour
   le P4 (câble, hub, 5 V VBUS instable → eye diagram dégradé à 480 Mbps).
   https://forums.raspberrypi.com/viewtopic.php?t=348117 (iso OUT host, mêmes
   symptômes sur RP2040).

### C. Analogues Linux (débogage conceptuel)

8. **XRUNs ALSA** = exactement underrun/overrun de buffer ; les causes
   récurrentes (période trop courte, IRQ partagée, DMA latency) se traduisent
   en embedded par : tâche de remplissage FIFO trop lente, priorité insuffisante,
   memcpy en fin de buffer. Réfs :
   - https://discourse.zynthian.org/t/solved-xrun-every-110-seconds-with-usb-audio-interface/6854
   - libusb iso perf thread : https://sourceforge.net/p/libusb/mailman/libusb-devel/thread/ff234d23-ec5c-797f-d290-54f42aa786a6@probo.com/
   - Raspberry Pi forums iso perf : https://forums.raspberrypi.com/viewtopic.php?t=29226
   - TotalPhase (interprétation des erreurs iso, packet loss « réel ») :
     https://www.totalphase.com/blog/2021/07/how-do-i-interpret-usb-data-errors-for-isochronous-endpoints/

## 2. Références Espressif / exemples

- USB Host ESP32-P4 (docs esp-usb) : https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_host.html
- Errata ESP32-P4 (index officiel) : https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32p4/resources.html
- Composant `usb_stream` (UVC+UAC host, UAC1, S2/S3 — bon code de référence pour le polling UAC1) :
  https://components.espressif.com/components/espressif/usb_stream/versions/1.0.5
- Guide TinyUSB esp-iot-solution (notes iso/UAC) : https://docs.espressif.com/projects/esp-iot-solution/en/release-v2.0/usb/usb_overview/tinyusb_guide.html
- TinyUSB HS OTG P4 merged (0.15) : https://blog.adafruit.com/2024/10/01/usb-otg-high-speed-support-for-esp32-p4-merged-into-tinyusb/
- Discussion UAC2 multichannel sur P4 : https://github.com/hathach/tinyusb/discussions/3562
- Esp32.com P4 host mode (stack size du host task !) : https://esp32.com/viewtopic.php?t=45502&start=10

## 3. DDJ-400 spécifiquement

- Mixxx : DDJ-400 class compliant (audio+MIDI) : https://manual.mixxx.org/2.3/id/hardware/controllers/pioneer_ddj_400
- Reverse engineering interfaces audio Pioneer DJM (méthodologie applicable,
  Olivia Mackintosh) : https://base.nu/musings/2021-09-19-Reverse-engineering-audio-gear/
- Wiki Mixxx « Reverse Engineering » (usbmon/Wireshark, méthode) :
  https://github.com/mixxxdj/mixxx/wiki/Reverse-Engineering
- Reverse engineering d'une soundcard USB+MIDI sous Linux (kicherer) :
  https://kicherer.org/joomla/index.php/en/blog/bloglist/38-reverse-engineering-a-usb-sound-card-with-midi-interface-for-linux
- Mesures ASR du DDJ-400 (confirme chaîne DAC correcte quand host correct) :
  https://www.audiosciencereview.com/forum/index.php?threads/ddj-400-measurments.29480/
- Note : le repo « palmarci/ddj400_re » n'a pas été retrouvé par recherche web
  (peut-être privé ou renommé) ; les sources ci-dessus couvrent la méthode.
  Le codec du DDJ-400 n'est pas documenté officiellement ; famille
  PCM29xx (adaptive playback, buffer 1 ms) est le modèle mental le plus proche.

## 4. Tests recommandés (ordre de coût croissant)

1. **Capter ce que Linux envoie** au DDJ-400 sur PC (usbmon + Wireshark,
   méthode wiki Mixxx) : exactement combien de transactions OUT par
   frame/microframe, taille par transaction, espacement, et est-ce que
   Linux envoie 176 B/frame FS ou autre. Reproduire ce pattern exact sur P4.
2. **Forcer l'alt setting S24 12 B** (ou l'inverse) : si le glitch change de
   caractère, la marge de buffer du DAC est en cause (12 B = 2× plus de
   marge temporelle par transaction).
3. **Pattern de test sur le bus** : envoyer un motifconstant type rampe 8-bit
   et logger côté P4 (HCINT bits, compteur de packets soumis vs complétés)
   + côté DAC (si ligne I2S observable, ou mesure du glitch audio périodique :
   calculer la période en frames — une période fixe de N ms pointe vers
   underrun de buffer predictible ; aléatoire pointe vers perte iso/PHY).
4. **Répartir les transactions** dans le microframe (1 transaction de 176 B
   par frame de 1 ms, ou espacement manuel) au lieu de bursts — imiter Linux.
5. **Désactiver/re-prioriser le canal MIDI** pendant l'iso OUT pour tester la
   contention ; Augmenter la taille de la FIFO périodique DWC2
   (GDFIFOCFG / HPTXFSIZ) au-delà du minimum.
6. **Test hub alimenté / câble court blindé / autre câble** (éliminer PHY/
   signal integrity ; UTMI HS PHY est sensible, cf. #2433).
7. **Vérifier les errata ESP32-P4** officiels pour l'USB OTG HS avant tout
   changement logiciel : https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32p4/resources.html
8. **Si tout échoue** : envisager d'émuler le comportement « async avec
   feedback » impossible ici — la seule marge restante est le dimensionnement
   du buffer d'envoi côté P4 (pré-buffer ≥ 2-3 frames pour absorber le jitter
   de scheduling, au prix de ~3 ms de latence).

## 5. Synthèse

Hypothèses les plus probables étant donné « host périodique propre, segmentation
sans effet, PC Linux parfait » :
1. le DAC adaptive du DDJ-400 est sensible au *pattern* de transactions
   (espacement/jitter intra-microframe), pas au débit moyen → reproduire
   exactement le pattern Linux (usbmon capture) ;
2. pertes iso silencieuses côté DWC2 (FIFO périodique / arbitrage / errata)
   invisibles sans lecture des registres HCINT ;
3. signal integrity / VBUS sur le port P4.
