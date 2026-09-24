#!/bin/bash
# HIL v3 - Pajoniiir JC1060. Durable (lives in repo, not /tmp).
# Full serial capture ALWAYS on ttyUSB0 (fallback ttyUSB1), OTA host IP set
# automatically, build verified end-to-end (no silent failures).
#
#   hil.sh daemon       : (re)start permanent serial capture daemon (auto port)
#   hil.sh tail N PAT   : last N lines of serial log, optional grep (case-insens)
#   hil.sh status       : daemon + heartbeat + version + USB/UAC events
#   hil.sh ota          : set board OTA host to THIS machine's IP (after reboot)
#   hil.sh serve        : ensure the OTA http server (port 8080) is up
#   hil.sh build        : docker build + bin version check + chown fix
set -uo pipefail

LOG=/tmp/serial_all.log
BOARD_IP=192.168.100.131
BOARD_CONSOLE_PORT=2333
FWSRC=/home/kay/Pajoniiir/firmware/main-deck-jc1060
FWBIN="$FWSRC/build/pajoniiir_deck.bin"
IDF_IMG=espressif/idf:v6.0.2

host_ip() {
  ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | grep -q '^192\.168\.100\.' \
    && ip -4 -o addr show scope global | awk '$4 ~ /^192\.168\.100\./ {print $4}' | cut -d/ -f1 | head -1 \
    || ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1
}

serial_port() { ls /dev/ttyUSB* 2>/dev/null | head -1; }

daemon() {
  local p; p=$(serial_port)
  [ -z "$p" ] && { echo "no ttyUSB0/1 present"; exit 1; }
  docker rm -f hild 2>/dev/null
  cp -f /home/kay/Pajoniiir/tools/hil_capture.py /tmp/hil_capture.py
  # -p1 restart loop keeps capture alive across replug; full log, never truncated
  docker run -d --rm --name hild --user root --group-add dialout \
    --device="$p" -v /tmp:/hosttmp "$IDF_IMG" \
    bash -c "while true; do python3 /hosttmp/hil_capture.py '$p' || true; sleep 1; done" >/dev/null \
    && echo "daemon started on $p -> $LOG (full log, always on)"
}

case "${1:-}" in
  daemon) daemon ;;
  tail)
    N="${2:-40}"; PAT="${3:-}"
    if [ -n "$PAT" ]; then
      tail -"$N" "$LOG" 2>/dev/null | tr -d '\000' | grep -iE "$PAT" | tail -30
    else
      tail -"$N" "$LOG" 2>/dev/null | tr -d '\000'
    fi ;;
  status)
    docker ps --format '{{.Names}} {{.Status}}' | grep -q '^hild' && echo "daemon: alive" || echo "daemon: DEAD (run: hil.sh daemon)"
    echo "port: $(serial_port)  log: $(stat -c %s "$LOG" 2>/dev/null || echo 0) bytes"
    echo "-- heartbeat/version:"
    bash "$0" tail 60 'alive|version' | tail -3
    echo "-- USB / UAC / OTA events:"
    bash "$0" tail 4000 'New device|mounted|CONNECTED|reject|MIDI|UAC|usb_audio|stream|OTA' | tail -8 ;;
  ota)
    IP=$(host_ip)
    python3 - "$BOARD_IP" "$BOARD_CONSOLE_PORT" "$IP" <<'EOF'
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=6)
s.settimeout(3)
try: print("board:", s.recv(300).decode(errors="replace").strip())
except Exception: pass
s.close()
print(f"OTA host confirmed -> {sys.argv[3]}")
EOF
    echo "serve with: hil.sh serve (http://$IP:8080/pajoniiir_deck.bin)" ;;
  serve)
    IP=$(host_ip)
    docker ps --format '{{.Names}}' | grep -q '^ota_serve' || {
      docker rm -f ota_serve 2>/dev/null
      docker run -d --rm --name ota_serve -p 8080:8080 -w /srv \
        -v "$FWSRC/build:/srv" python:3-alpine \
        python3 -m http.server 8080 --bind 0.0.0.0 >/dev/null
      echo "ota_serve (re)started"; }
    curl -fsI "http://$IP:8080/pajoniiir_deck.bin" | head -1 \
      && echo "OK: http://$IP:8080/pajoniiir_deck.bin" \
      || echo "WARN: bin not reachable yet (build first?)" ;;
  build)
    cd "$FWSRC" || exit 1
    VER=$(grep -oP 'PROJECT_VER "\K[0-9]+' CMakeLists.txt | head -1)
    echo "target: v$VER"
    # root-owned build dir from a previous root docker run breaks clean rebuild
    [ -d build ] && docker run --rm -v "$FWSRC":/w alpine chown -R "$(id -u):$(id -g)" /w/build 2>/dev/null
    OUT=$(docker run --rm --user root -e HOME=/tmp -v /home/kay/Pajoniiir:/host \
      -w /host/firmware/main-deck-jc1060 "$IDF_IMG" bash -lc \
      'source /opt/esp/idf/export.sh >/dev/null 2>&1 && idf.py build 2>&1; echo IDF_EXIT=$?' | tee /tmp/hil_build.log)
    echo "$OUT" | grep -E 'IDF_EXIT=[0-9]+' | tail -1
    if echo "$OUT" | grep -E '^IDF_EXIT=[^0]' >/dev/null; then
      echo "BUILD FAILED (full log: /tmp/hil_build.log):"
      echo "$OUT" | grep -iE 'error|failed|fatal' | grep -vi 'IDF_EXIT' | sort -u | head -12; exit 1
    fi
    echo "$OUT" | grep 'binary size' | tail -1
    # root docker run re-owns build/ -> hand it back so host rm/edit works
    docker run --rm -v "$FWSRC":/w alpine chown -R "$(id -u):$(id -g)" /w/build
    python3 - "$FWBIN" "$VER" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
assert sys.argv[2].encode() in d[:0x200], "version string not in bin header"
print(f"bin v{sys.argv[2]} OK ({len(d)} bytes)")
EOF
    echo "next: hil.sh serve && hil.sh ota" ;;
  *) echo "usage: hil.sh {daemon|tail N PAT|status|ota|serve|build}" ;;
esac
