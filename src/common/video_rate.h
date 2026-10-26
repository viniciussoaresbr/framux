// Teto de taxa da gravação: bits por pixel por quadro × pixels × fps. Em
// 2560×1080 a 60 fps, H.264 em qualidade alta fica em ~20 Mbit/s
// (~150 MB/min), o que o openh264 do Fedora ainda decodifica em tempo real;
// AV1 entrega o mesmo com ~65% disso. Usado pelo gravador e pela estimativa
// de tamanho da janela.
#pragma once

#include <cstdint>
#include <string>

namespace framux {

// bit/s; 0 = sem teto (sem perda).
inline uint64_t video_max_bitrate(bool av1, const std::string &quality, int width, int height, int fps) {
  double bpp = quality == "low" ? 0.05 : quality == "medium" ? 0.08 : quality == "high" ? 0.12 : 0;
  if (av1) bpp *= 0.65;
  return uint64_t(bpp * double(width) * double(height) * double(fps));
}

}  // namespace framux
