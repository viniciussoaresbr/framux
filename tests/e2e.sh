#!/bin/bash
# Testes ponta a ponta (§C.12 testes 1, 2, 5, 10) com daemon, camada/shim e
# CLI reais, num ambiente isolado (XDG_RUNTIME_DIR, config e D-Bus próprios).
# Requer: Xvfb (via with_xvfb.sh), dbus-run-session, vkcube e glxgears.
# uso: e2e.sh <builddir> <cenário: vulkan|gl|kill|reconnect|pause|hotkeys|auto|reload>
set -u
B=$1
SCENARIO=$2
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -z "${FRAMUX_E2E_INNER:-}" ]; then
  command -v dbus-run-session >/dev/null || { echo "dbus-run-session ausente"; exit 77; }
  export FRAMUX_E2E_INNER=1
  exec dbus-run-session -- "$HERE/with_xvfb.sh" "$0" "$@"
fi

case $SCENARIO in
  gl) APP=(glxgears) ;;
  *) APP=(vkcube --wsi xcb --c 1000000) ;;
esac
command -v "${APP[0]}" >/dev/null || { echo "${APP[0]} ausente"; exit 77; }

T=$(mktemp -d)
# Mata também qualquer framuxd que tenha subido neste XDG_RUNTIME_DIR.
cleanup() {
  kill ${AP:-} ${DP:-} 2>/dev/null
  for p in $(pgrep -x framuxd); do
    grep -qz "^XDG_RUNTIME_DIR=$T/rt$" /proc/$p/environ 2>/dev/null && kill $p
  done
  rm -rf "$T"
}
trap cleanup EXIT
export XDG_RUNTIME_DIR=$T/rt XDG_CONFIG_HOME=$T/cfg
mkdir -m700 -p "$XDG_RUNTIME_DIR"
mkdir -p "$XDG_CONFIG_HOME/framux"
cat > "$XDG_CONFIG_HOME/framux/config.toml" <<EOT
[general]
notifications = false
[benchmark]
folder = "$T/bench"
fraps_compat = true # chave removida: deve ser aceita sem efeito
[screenshot]
folder = "$T/shots"
EOT
export PATH=$B/src/cli:$B/src/daemon:$PATH FRAMUX_DAEMON=$B/src/daemon/framuxd
export FRAMUX_GL_PRELOAD=$B/src/gl/libframux_gl.so VK_ADD_IMPLICIT_LAYER_PATH=$B/src/layer

fail() { echo "FALHOU: $*"; echo "--- daemon"; cat "$T/daemon.log"; echo "--- app"; tail -5 "$T/app.log"; exit 1; }
start_daemon() { framuxd 2>>"$T/daemon.log" & DP=$!; for _ in $(seq 40); do framux status >/dev/null 2>&1 && return; sleep 0.1; done; fail "daemon não subiu"; }
start_app() { framux run "${APP[@]}" >/dev/null 2>"$T/app.log" & AP=$!; }
wait_session() {
  for _ in $(seq 60); do framux status 2>/dev/null | grep -q "${APP[0]}" && return; sleep 0.1; done
  fail "sessão de ${APP[0]} não apareceu"
}
check_bench() {
  local summary
  for _ in $(seq 50); do summary=$(ls "$T"/bench/*summary.json 2>/dev/null) && break; sleep 0.1; done
  [ -n "$summary" ] || fail "summary.json não foi gravado"
  local frames
  frames=$(sed -n 's/.*"frames":\([0-9]*\).*/\1/p' "$summary")
  [ "${frames:-0}" -gt 10 ] || fail "benchmark com poucos quadros ($frames)"
  local lines
  lines=$(wc -l < "$(ls "$T"/bench/*\ frametimes.csv)")
  [ "$lines" -eq $((frames + 1)) ] || fail "frametimes.csv com $lines linhas para $frames quadros"
  ls "$T"/bench/*\ fps.csv >/dev/null || fail "fps.csv ausente"
  grep -q "Frames: $frames " "$T"/bench/benchmarks.log || fail "benchmarks.log sem a linha do benchmark"
  [ ! -e "$T"/bench/fraps ] || fail "pasta fraps/ não deveria existir"
  echo "benchmark ok: $frames quadros"
}

case $SCENARIO in
  vulkan|gl)
    start_daemon
    start_app
    wait_session
    framux bench start --duration 2 >/dev/null || fail "bench start"
    shot_overlay=$(framux shot --overlay) || fail "shot --overlay"
    shot_plain=$(framux shot) || fail "shot"
    framux overlay next | grep -q top-right || fail "overlay next"
    framux overlay toggle | grep -q hidden || fail "overlay toggle (ocultar)"
    framux overlay toggle | grep -q top-left || fail "overlay toggle volta ao canto da config"
    sleep 2.5
    check_bench
    y1=$("$B/tests/png_probe" "$shot_overlay")
    y2=$("$B/tests/png_probe" "$shot_plain")
    echo "pixels do overlay: com=$y1 sem=$y2"
    [ "$y1" -gt 20 ] || fail "overlay ausente no screenshot com --overlay"
    [ "$y2" -lt 5 ] || fail "overlay presente no screenshot sem --overlay"
    ;;
  kill)
    # Teste 5: matar o jogo no meio do benchmark deixa os arquivos completos.
    start_daemon
    start_app
    wait_session
    framux bench start >/dev/null || fail "bench start"
    sleep 2
    kill -9 $AP
    wait $AP 2>/dev/null
    check_bench
    ;;
  reconnect)
    # Teste 10: sem daemon o jogo roda; quando o daemon sobe, a camada reconecta.
    FRAMUX_AUTOSTART=0 start_app
    sleep 1.5
    kill -0 $AP || fail "o jogo morreu sem o daemon"
    start_daemon
    wait_session
    kill $DP; wait $DP 2>/dev/null
    sleep 1
    kill -0 $AP || fail "o jogo morreu quando o daemon caiu"
    start_daemon
    wait_session
    echo "reconexão ok"
    ;;
  hotkeys)
    # Teste 11 (fallback in-game): F11/F12/F10 lidas pela camada via XTest.
    start_daemon
    start_app
    wait_session
    sleep 0.5
    # F12 só mostra/oculta, sempre no canto da config; Shift+F12 troca o canto.
    "$B/tests/xkey" F12 || fail "xkey"
    sleep 0.3
    framux status | grep -q "overlay hidden" || fail "F12 não ocultou o overlay"
    "$B/tests/xkey" F12
    sleep 0.3
    framux status | grep -q "overlay top-left" || fail "F12 não mostrou o overlay no canto da config"
    "$B/tests/xkey" Shift+F12
    sleep 0.3
    framux status | grep -q "overlay top-right" || fail "Shift+F12 não mudou o canto do overlay"
    "$B/tests/xkey" F12
    sleep 0.3
    "$B/tests/xkey" F12
    sleep 0.3
    framux status | grep -q "overlay top-left" || fail "F12 não voltou ao canto da config"
    "$B/tests/xkey" F11
    sleep 0.3
    framux status | grep -q "benchmark" || fail "F11 não iniciou o benchmark"
    "$B/tests/xkey" F10
    sleep 1
    ls "$T"/shots/*.png >/dev/null 2>&1 || fail "F10 não gerou screenshot"
    "$B/tests/xkey" F11
    check_bench
    echo "hotkeys ok"
    ;;
  reload)
    # A GUI só grava o config.toml: o canto novo tem que chegar ao jogo aberto.
    start_daemon
    start_app
    wait_session
    sleep 0.5
    before=$("$B/tests/png_probe" "$(framux shot --overlay)") || fail "shot antes"
    printf '[overlay]\ncorner = "bottom-right"\n' >> "$XDG_CONFIG_HOME/framux/config.toml"
    for _ in $(seq 30); do framux status | grep -q "overlay bottom-right" && break; sleep 0.1; done
    sleep 0.3
    after=$("$B/tests/png_probe" "$(framux shot --overlay)") || fail "shot depois"
    echo "pixels no canto superior esquerdo: antes=$before depois=$after"
    [ "$before" -gt 20 ] || fail "overlay ausente antes da mudança"
    [ "$after" -lt 5 ] || fail "o canto novo não chegou ao jogo"
    ;;
  auto)
    # Modo automático da GUI (FRAMUX=1 FRAMUX_AUTO=1 na sessão): sem `framux
    # run`, um app qualquer fica sem sessão; um jogo da Steam ganha uma.
    start_daemon
    FRAMUX=1 FRAMUX_AUTO=1 "${APP[@]}" >/dev/null 2>"$T/app.log" & AP=$!
    sleep 2
    kill -0 $AP || fail "o app morreu com a camada transparente"
    framux status | grep -q "${APP[0]}" && fail "app comum virou sessão no modo automático"
    kill $AP; wait $AP 2>/dev/null
    FRAMUX=1 FRAMUX_AUTO=1 SteamAppId=480 "${APP[@]}" >/dev/null 2>"$T/app.log" & AP=$!
    wait_session
    echo "modo automático ok"
    ;;
  pause)
    # Comandos com mais de 2 s são descartados pela camada (§C.3.3): com o
    # jogo parado, o screenshot pedido não pode sair "atrasado" na volta.
    start_daemon
    start_app
    wait_session
    kill -STOP $AP
    framux shot >/dev/null 2>&1 & SP=$!
    sleep 3
    kill -CONT $AP
    wait $SP && fail "screenshot com o jogo parado deveria falhar"
    sleep 1
    ls "$T"/shots/*.png >/dev/null 2>&1 && fail "screenshot atrasado foi gravado"
    framux shot >/dev/null || fail "screenshot depois da pausa"
    echo "pausa ok"
    ;;
  *) echo "cenário desconhecido: $SCENARIO"; exit 2 ;;
esac
echo ok
