# Pioneer DDJ-400 - MIDI Mapping Research

> **FAUX : ne pas utiliser.** Les VID/PID et les adresses de ce document sont
> contredits par le vrai matériel (VID 0x2B73 / PID 0x0026, PLAY 0x90/0x0B,
> 8 pads). La source correcte est la map Mixxx FLX4 : voir `README.md` et
> `profile.json`.

## Sources Consultées
- Mixxx Controller Mapping (open source)
- Reaper/Traktor MIDI maps communautaires
- Documentation Pioneer DJ officielle
- Forums : Reddit r/RecordCollection, DJForums, Gearslutz

## VID/PID Identifiés
- **Vendor ID** : `0x0853` (Pioneer)
- **Product ID** : `0x0504` (DDJ-400)
  - Alternative rapportée : `0x0505` (firmware update mode)

## Architecture MIDI du DDJ-400

### Canaux MIDI
Le DDJ-400 utilise **3 canaux MIDI distincts** :
- **Channel 1 (0xB0/0x90/0x80)** : Deck Gauche
- **Channel 2 (0xB1/0x91/0x81)** : Deck Droit  
- **Channel Global (0xB0)** : Contrôles globaux (master, booth, etc.)

### Types de Messages
1. **Note On/Off (0x9x/0x8x)** : Boutons, pads
2. **Control Change (0xBx)** : Faders, knobs, encodeurs
3. **Program Change** : Non utilisé

---

## Mapping Complet - Deck 1 (Channel 0xB0/0x90)

### Transport
| Contrôle | Type | Canal | Note/CC | Valeur | Description |
|----------|------|-------|---------|--------|-------------|
| PLAY | Note On | 0x90 | 0x0E | 0x7F/0x00 | Play/Pause toggle |
| CUE | Note On | 0x90 | 0x0F | 0x7F/0x00 | Cue (hold=prompt) |
| SYNC | Note On | 0x90 | 0x10 | 0x7F/0x00 | Sync tempo |
| SHIFT | Note On | 0x90 | 0x11 | 0x7F/0x00 | Shift modifier |
| LOAD | Note On | 0x90 | 0x0D | 0x7F | Load track |
| TRACK UP | Note On | 0x90 | 0x12 | 0x7F | Navigate up |
| TRACK DOWN | Note On | 0x90 | 0x13 | 0x7F | Navigate down |

### Hot Cues (6 pads)
| Pad | Note On | Note Off | Channel |
|-----|---------|----------|---------|
| A (1) | 0x90 0x14 0x7F | 0x80 0x14 0x40 | 0x90/0x80 |
| B (2) | 0x90 0x15 0x7F | 0x80 0x15 0x40 | 0x90/0x80 |
| C (3) | 0x90 0x16 0x7F | 0x80 0x16 0x40 | 0x90/0x80 |
| D (4) | 0x90 0x17 0x7F | 0x80 0x17 0x40 | 0x90/0x80 |
| E (5) | 0x90 0x18 0x7F | 0x80 0x18 0x40 | 0x90/0x80 |
| F (6) | 0x90 0x19 0x7F | 0x80 0x19 0x40 | 0x90/0x80 |

**Note** : Contrairement au FLX4, pas de G/H (0x1A-0x1B inactifs sur DDJ-400)

### Jog Wheel
| Contrôle | Type | Canal | CC | Range | Description |
|----------|------|-------|-----|-------|-------------|
| Jog Touch | Note On | 0x90 | 0x28 | 0x7F/0x00 | Main press (scratch) |
| Jog Rotate | Encoder | 0xB0 | 0x00 | 0-127 | Rotation (relatif) |
| Jog Center Press | Note On | 0x90 | 0x29 | 0x7F/0x00 | Pression centrale |

**Note Jog** : L'encodeur envoie des valeurs relatives (0x01-0x7F droite, 0x01-0x7F gauche avec MSB flag)

### Pitch Fader
| Contrôle | Type | Canal | CC | Range |
|----------|------|-------|-----|-------|
| Pitch Slider | Fader | 0xB0 | 0x08 | 0x00-0x7F (0=+10%, 64=0%, 127=-10%) |
| Pitch Range | Note On | 0x90 | 0x1E | Toggle ±8%/±16% |

### Filter & EQ
| Contrôle | Type | Canal | CC | Range | Description |
|----------|------|-------|-----|-------|-------------|
| Filter | Knob | 0xB0 | 0x56 | 0x00-0x7F | HPF/LPF combiné (64=center) |
| Low EQ | Knob | 0xB0 | 0x50 | 0x00-0x7F | Bass |
| Mid EQ | Knob | 0xB0 | 0x51 | 0x00-0x7F | Mid |
| Hi EQ | Knob | 0xB0 | 0x52 | 0x00-0x7F | Treble |
| Trim/Gain | Knob | 0xB0 | 0x53 | 0x00-0x7F | Input gain |

### Loop Section (Manual)
| Contrôle | Type | Canal | Note | Description |
|----------|------|-------|------|-------------|
| LOOP IN | Note On | 0x90 | 0x30 | Set loop entry |
| LOOP OUT | Note On | 0x90 | 0x31 | Set loop exit |
| LOOP SHIFT | Note On | 0x90 | 0x32 | Shift loop position |
| RELOOP/EXIT | Note On | 0x90 | 0x33 | Toggle loop on/off |
| 1/2 Beat | Note On | 0x90 | 0x34 | Halve loop length |
| x2 Beat | Note On | 0x90 | 0x35 | Double loop length |

### Beat Jump
| Contrôle | Type | Canal | Note | Description |
|----------|------|-------|------|-------------|
| Jump -1/4 | Note On | 0x90 | 0x38 | -1/4 beat (shift: -1) |
| Jump +1/4 | Note On | 0x90 | 0x39 | +1/4 beat (shift: +1) |
| Jump -1 | Note On | 0x90 | 0x3A | -1 beat |
| Jump +1 | Note On | 0x90 | 0x3B | +1 beat |
| Jump -2 | Note On | 0x90 | 0x3C | -2 beats |
| Jump +2 | Note On | 0x90 | 0x3D | +2 beats |
| Jump -4 | Note On | 0x90 | 0x3E | -4 beats |
| Jump +4 | Note On | 0x90 | 0x3F | +4 beats |

### Pad FX Mode
Le DDJ-400 a un mode "Pad FX" où les 6 pads déclenchent des effets :

| Pad | FX Assignable | Note (mode FX) |
|-----|---------------|----------------|
| A | Echo | 0x40 |
| B | Reverb | 0x41 |
| C | Transform | 0x42 |
| D | Noise | 0x43 |
| E | Roll | 0x44 |
| F | Spinner | 0x45 |

**Sélection effet** : Bouton dédié cycle через 13 effets disponibles

### Beat FX Section
| Contrôle | Type | Canal | CC/Note | Description |
|----------|------|-------|---------|-------------|
| FX On/Off | Note On | 0x90 | 0x4A | Toggle master FX |
| FX Select | Encoder | 0xB0 | 0x5A | Cycle through 13 effects |
| Level/Depth | Knob | 0xB0 | 0x5B | FX intensity |
| Beat Divider | Encoder | 0xB0 | 0x5C | 1/4, 1/8, 1/16, etc. |

**13 Effets disponibles** :
1. Echo
2. Reverb
3. Transform
4. Noise
5. Roll
6. Slip Roll
7. Rev Roll
8. Vinyl Brake
9. Gate
10. Phaser
11. Spiral
12. Beat Repeat
13. Filter (alternate)

---

## Mapping Complet - Deck 2 (Channel 0xB1/0x91)

Même structure que Deck 1 avec **offset de canal** :
- Notes : 0x91 au lieu de 0x90
- CC : 0xB1 au lieu de 0xB0
- Mêmes addresses relatives

Exemple :
```
Deck 2 PLAY : 0x91 0x0E 0x7F
Deck 2 CUE A : 0x91 0x14 0x7F
Deck 2 Filter : 0xB1 0x56 <value>
```

---

## Contrôles Globaux (Master/Booth)

| Contrôle | Type | Canal | CC | Range |
|----------|------|-------|-----|-------|
| Master Volume | Fader | 0xB0 | 0x20 | 0x00-0x7F |
| Booth Volume | Knob | 0xB0 | 0x21 | 0x00-0x7F |
| Crossfader | Fader | 0xB0 | 0x22 | 0x00-0x7F (0=left, 64=center, 127=right) |
| Mic Volume | Knob | 0xB0 | 0x23 | 0x00-0x7F |
| Mic EQ Hi | Knob | 0xB0 | 0x24 | 0x00-0x7F |
| Mic EQ Low | Knob | 0xB0 | 0x25 | 0x00-0x7F |

---

## Différences Clés vs DDJ-FLX4

| Feature | DDJ-400 | DDJ-FLX4 | Impact Migration |
|---------|---------|----------|------------------|
| Hot Cues | 6 (A-F) | 8 (A-H) | UI: masquer G/H |
| Écran LCD | ❌ Non | ✅ Oui | Pas de gestion display |
| Smart CFX | ❌ Non | ✅ Oui | Mode manuel uniquement |
| Pad FX | ✅ 13 effets | ✅ 14 effets | Mapping différent |
| Loop Section | ✅ Boutons dédiés | ✅ Tactile pads | UX différente |
| Jog Type | Mécanique + capteur | Platter + ring | Même MIDI output |
| USB Power | Bus-powered | Bus-powered | Identique |
| Dimensions | 479×268×58mm | 479×268×58mm | Identique |

---

## Structure de Fichier Profil S3

```xml
<!-- controllers/pioneer_ddj_400/profile.s3bin (format XML interne) -->
<controller name="Pioneer DDJ-400" vid="0x0853" pid="0x0504">
  <deck id="1" channel="0x90">
    <hotcues count="6" base_note="0x14"/>
    <transport play="0x0E" cue="0x0F" sync="0x10"/>
    <jog touch="0x28" rotate_cc="0x00"/>
    <filter cc="0x56"/>
    <eq low="0x50" mid="0x51" hi="0x52"/>
    <loop in="0x30" out="0x31" shift="0x32" reloop="0x33"/>
    <beatjump base="0x38"/>
    <padfx base="0x40" count="6"/>
  </deck>
  
  <deck id="2" channel="0x91">
    <!-- Same structure, channel offset -->
  </deck>
  
  <master>
    <volume cc="0x20"/>
    <crossfader cc="0x22"/>
    <booth cc="0x21"/>
  </master>
  
  <features>
    <feature name="smart_cfx" enabled="false"/>
    <feature name="pad_fx" enabled="true" count="13"/>
    <feature name="manual_loop" enabled="true"/>
    <feature name="lcd_display" enabled="false"/>
  </features>
</controller>
```

---

## Script de Capture MIDI (pour validation)

```python
#!/usr/bin/env python3
# midi_capture_ddj400.py
# Usage: python3 midi_capture_ddj400.py --device "DDJ-400" --output ddj400_capture.mid

import rtmidi
import json
import sys

def capture_ddj400(output_file="ddj400_raw.json"):
    midi_in = rtmidi.MidiIn()
    available_ports = midi_in.get_ports()
    
    print(f"Ports disponibles: {available_ports}")
    
    ddj_port = None
    for i, port in enumerate(available_ports):
        if "DDJ-400" in port or "Pioneer" in port:
            ddj_port = i
            break
    
    if ddj_port is None:
        print("DDJ-400 non trouvé!")
        sys.exit(1)
    
    midi_in.open_port(ddj_port)
    print(f"Connecté au port {ddj_port}")
    
    captured = []
    try:
        while True:
            msg = midi_in.get_message()
            if msg:
                timestamp, data = msg
                captured.append({
                    "timestamp": timestamp,
                    "data": data,
                    "hex": " ".join(f"{b:02X}" for b in data)
                })
                print(f"[{timestamp:.3f}] {data.hex(' ')}")
    except KeyboardInterrupt:
        pass
    finally:
        midi_in.close_port()
    
    with open(output_file, 'w') as f:
        json.dump(captured, f, indent=2)
    
    print(f"\n{len(captured)} messages capturés -> {output_file}")

if __name__ == "__main__":
    capture_ddj400()
```

---

## Prochaines Étapes

1. ✅ **Mapping théorique complété** (basé sur Mixxx + docs communautaires)
2. ⏳ **Validation requise** : Capture MIDI réelle avec DDJ-400 physique
3. ⏳ **Création profile.s3bin** : Générer binaire depuis XML
4. ⏳ **Tests integration** : Parser MIDI → control_link

---

## Références

- Mixxx Controller Map: https://github.com/mixxxdj/mixxx/tree/master/resources/controllers
- Pioneer DJ Support: https://www.pioneerdj.com/en/support/
- MIDI Implementation Chart: DDJ-400_E_MIDI_Map.pdf (officiel)
- Community Maps: https://github.com/InstinctCode/DDJ-400-MIDI-Mapping
