#!/bin/bash
# Roda um app Vulkan com a camada do Framux e, se instalada, a camada de
# validação Khronos abaixo dela: qualquer "Validation Error" reprova.
# uso: vk_validate.sh <app> <dir do manifesto da camada>
APP=$1
export VK_ADD_IMPLICIT_LAYER_PATH=$2 FRAMUX=1 FRAMUX_DEBUG=1
HERE=$(cd "$(dirname "$0")" && pwd)
if ls /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json >/dev/null 2>&1; then
  export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
OUT=$("$HERE/with_xvfb.sh" "$APP" 2 2>&1)
RC=$?
echo "$OUT" | tail -20
[ $RC -eq 77 ] && exit 77
[ $RC -eq 0 ] || exit 1
echo "$OUT" | grep -q "ativo em" || { echo "a camada não carregou"; exit 1; }
echo "$OUT" | grep -q "Validation Error" && exit 1
exit 0
