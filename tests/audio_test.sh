#!/bin/bash
# Roda o teste de captura de áudio num PipeWire + WirePlumber privados
# (XDG_RUNTIME_DIR e config próprios, monitores de hardware desligados):
# nada chega aos alto-falantes nem ao grafo da sessão do usuário.
# uso: audio_test.sh <audio_capture_test>
for b in pipewire wireplumber; do command -v $b >/dev/null || { echo "$b ausente"; exit 77; }; done
T=$(mktemp -d)
trap 'kill $WP $PP 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT
export XDG_RUNTIME_DIR=$T/rt PIPEWIRE_RUNTIME_DIR=$T/rt XDG_CONFIG_HOME=$T/cfg XDG_STATE_HOME=$T/state
unset PIPEWIRE_REMOTE DBUS_SESSION_BUS_ADDRESS
mkdir -m700 -p "$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d"
cat > "$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d/90-framux-test.conf" <<EOT
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez.midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
    support.dbus = disabled
    support.reserve-device = disabled
    support.portal-permissionstore = disabled
    support.logind = disabled
  }
}
EOT
pipewire >"$T/pw.log" 2>&1 & PP=$!
for _ in $(seq 50); do [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] && break; sleep 0.1; done
[ -S "$XDG_RUNTIME_DIR/pipewire-0" ] || { echo "pipewire não subiu"; cat "$T/pw.log"; exit 77; }
wireplumber >"$T/wp.log" 2>&1 & WP=$!
sleep 1
kill -0 $WP 2>/dev/null || { echo "wireplumber não subiu"; cat "$T/wp.log"; exit 77; }
"$1" || { echo "--- wireplumber"; tail -20 "$T/wp.log"; exit 1; }
