#!/bin/bash
# Roda o comando num Xvfb próprio com renderização por software
# (lavapipe/llvmpipe). Sai com 77 (teste pulado) se faltar algo.
command -v Xvfb >/dev/null || { echo "Xvfb ausente"; exit 77; }
FIFO=$(mktemp -u)
mkfifo "$FIFO"
# -displayfd: o próprio Xvfb escolhe um display livre (testes em paralelo).
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$FIFO" >/dev/null 2>&1 &
XP=$!
trap 'kill $XP 2>/dev/null; rm -f "$FIFO"' EXIT
read -r -t 10 D <"$FIFO" || { echo "Xvfb não iniciou"; exit 77; }
export DISPLAY=:$D
unset WAYLAND_DISPLAY
export LIBGL_ALWAYS_SOFTWARE=1
for icd in /usr/share/vulkan/icd.d/lvp_icd.*.json; do
  [ -e "$icd" ] && export VK_DRIVER_FILES=${VK_DRIVER_FILES:+$VK_DRIVER_FILES:}$icd
done
"$@"
