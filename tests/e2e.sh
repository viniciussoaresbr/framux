#!/bin/bash
# Testes ponta a ponta (§C.12 testes 1, 2, 5, 8, 10) com daemon, camada/shim e
# CLI reais, num ambiente isolado (XDG_RUNTIME_DIR, config e D-Bus próprios).
# Requer: Xvfb (via with_xvfb.sh), dbus-run-session, vkcube e glxgears.
# uso: e2e.sh <builddir> <cenário: vulkan|gl|kill|reconnect|pause|hotkeys|auto|reload|video|video-half|video-color|video-gl|video-kill>
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
  gl|video-gl) APP=(glxgears) ;;
  video-color) PATH=$B/tests:$PATH APP=(vk_present_test) ;;  # limpa a tela de azul puro
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
[video]
folder = "$T/videos"
fps = 30
[audio]
game = false # sem PipeWire no ambiente de teste
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
# Espera o arquivo final da gravação e confere a trilha de vídeo:
# check_video <extensão> <codec esperado ou ""> <largura> <altura> <quadros mínimos>
check_video() {
  local file info
  for _ in $(seq 100); do
    file=$(ls "$T"/videos/*."$1" 2>/dev/null) && grep -q "gravação salva" "$T/daemon.log" && break
    sleep 0.1
  done
  [ -n "$file" ] || fail "vídeo .$1 não foi gravado"
  [ "$(ls "$T"/videos | wc -l)" -eq 1 ] || fail "sobrou outro arquivo na pasta: $(ls "$T"/videos)"
  info=$("$B/tests/video_probe" "$file" | grep '^video') || fail "vídeo ilegível: $file"
  echo "vídeo: $info"
  read -r _ codec w h n _ <<<"$info"
  [ -z "$2" ] || [ "$codec" = "$2" ] || fail "codec $codec, esperado $2"
  [ "$w" -eq "$3" ] && [ "$h" -eq "$4" ] || fail "tamanho ${w}x$h, esperado $3x$4"
  [ "$n" -ge "$5" ] || fail "só $n quadros"
  # Medição ao parar: FPS antes/durante, custo da cópia no jogo, CPU do daemon.
  local cost
  for _ in $(seq 50); do cost=$(grep -m1 "medição da gravação" "$T/daemon.log") && break; sleep 0.1; done
  echo "${cost#*] }"
  echo "$cost" | grep -q "gravando; cópia na thread do jogo [0-9.]* ms por quadro" || fail "medição sem o custo da camada"
  echo "$cost" | grep -q "framuxd [0-9.]* núcleos" || fail "medição sem a CPU do daemon"
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
    # A marca de "Parar o Framux" deixa a camada quieta até o daemon voltar.
    mkdir -m700 -p "$XDG_RUNTIME_DIR/framux" && touch "$XDG_RUNTIME_DIR/framux/stopped"
    FRAMUX_AUTOSTART=0 start_app
    sleep 1.5
    kill -0 $AP || fail "o jogo morreu sem o daemon"
    start_daemon
    [ ! -e "$XDG_RUNTIME_DIR/framux/stopped" ] || fail "o framuxd não apagou a marca de parado"
    wait_session
    shot=$(framux shot --overlay) || fail "shot --overlay"
    [ "$("$B/tests/png_probe" "$shot")" -gt 20 ] || fail "overlay não voltou depois de o daemon subir"
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
    # F9 inicia a gravação e, apertada de novo, para.
    "$B/tests/xkey" F9
    sleep 0.3
    framux status | grep -q "gravando" || fail "F9 não iniciou a gravação"
    sleep 1.5
    "$B/tests/xkey" F9
    check_video mkv "" 500 500 30
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
  video | video-half)
    # Teste 8 (reduzido): 2 s a 30 fps com o encoder automático, MP4 no fim.
    # A cor é convertida (e, em video-half, reduzida) na GPU pela camada, sob
    # a camada de validação Khronos quando instalada.
    sed -i 's/^\[video\]$/[video]\ncontainer = "mp4"/' "$XDG_CONFIG_HOME/framux/config.toml"
    [ "$SCENARIO" = video-half ] && sed -i 's/^\[video\]$/[video]\nscale = "half"/' "$XDG_CONFIG_HOME/framux/config.toml"
    ls /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json >/dev/null 2>&1 &&
      export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    start_daemon
    start_app
    wait_session
    framux rec start >/dev/null || fail "rec start"
    framux status | grep -q "gravando" || fail "status não mostra a gravação"
    framux rec start >/dev/null 2>&1 && fail "rec start repetido deveria falhar"
    sleep 2
    framux rec stop >/dev/null || fail "rec stop"
    # vkcube usa a janela padrão de 500×500.
    if [ "$SCENARIO" = video-half ]; then check_video mp4 "" 250 250 50; else check_video mp4 "" 500 500 50; fi
    grep -q "cor convertida na GPU" "$T/daemon.log" || fail "a cor não foi convertida na GPU"
    ! grep -m3 "Validation Error" "$T/app.log" || fail "erros da camada de validação"
    ;;
  video-color)
    # Cores da conversão na GPU: azul puro (0, 0, 255) em BT.709 de faixa
    # limitada é Y 32, U 240, V 118 (± compressão).
    ls /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json >/dev/null 2>&1 &&
      export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    start_daemon
    start_app
    wait_session
    framux rec start >/dev/null || fail "rec start"
    sleep 2
    framux rec stop >/dev/null || fail "rec stop"
    check_video mkv "" 640 480 50
    grep -q "cor convertida na GPU" "$T/daemon.log" || fail "a cor não foi convertida na GPU"
    ! grep -m3 "Validation Error" "$T/app.log" || fail "erros da camada de validação"
    read -r _ y u v <<<"$("$B/tests/video_probe" "$(ls "$T"/videos/*.mkv)" --pixel 320 240 | grep '^pixel')"
    echo "pixel central: Y $y U $u V $v"
    near() { [ "${1:-0}" -ge $(($2 - 4)) ] && [ "${1:-0}" -le $(($2 + 4)) ]; }
    near "$y" 32 && near "$u" 240 && near "$v" 118 || fail "cor errada: Y $y U $u V $v (esperado 32/240/118)"
    ;;
  video-gl)
    # OpenGL (PBO, linhas invertidas) em Ut Video sem perda e metade do tamanho.
    sed -i 's/^\[video\]$/[video]\nquality = "lossless"\nscale = "half"/' "$XDG_CONFIG_HOME/framux/config.toml"
    start_daemon
    start_app
    wait_session
    framux rec toggle >/dev/null || fail "rec toggle (iniciar)"
    sleep 2
    framux rec toggle >/dev/null || fail "rec toggle (parar)"
    # glxgears abre em 300×300.
    check_video mkv utvideo 150 150 50
    ;;
  video-kill)
    # Jogo morto no meio da gravação: o daemon fecha um arquivo válido.
    start_daemon
    start_app
    wait_session
    framux rec start >/dev/null || fail "rec start"
    sleep 2
    kill -9 $AP
    wait $AP 2>/dev/null
    check_video mkv "" 500 500 40
    ;;
  *) echo "cenário desconhecido: $SCENARIO"; exit 2 ;;
esac
echo ok
