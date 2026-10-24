// Conversão de formatos de swapchain e codificação de screenshots (§B.6,
// §C.7.5). Roda só no daemon, nunca no processo do jogo.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/config.h"

namespace framux {

struct RawImage {
  const uint8_t *data = nullptr;
  size_t size = 0;
  uint32_t width = 0, height = 0;
  uint32_t stride = 0;     // bytes por linha
  uint32_t vk_format = 0;  // VkFormat
  bool flip_y = false;     // linhas de baixo para cima
};

// RGB intercalado, 8 ou 16 bits por canal (16 para 10 bits e HDR).
struct RgbImage {
  uint32_t width = 0, height = 0;
  int bits = 8;
  std::vector<uint8_t> px8;    // bits == 8
  std::vector<uint16_t> px16;  // bits == 16
};

// Bytes por pixel do formato, 0 se não suportado.
uint32_t vk_format_bpp(uint32_t vk_format);
const char *vk_format_name(uint32_t vk_format);

bool convert_image(const RawImage &in, RgbImage &out, std::string *err);
// Reduz para 8 bits (JPEG/BMP/TGA, ou PNG de 8 bits).
RgbImage to_8bit(const RgbImage &in);

// Codifica para a memória.
bool encode_png(const RgbImage &img, std::vector<uint8_t> &out, std::string *err);
bool encode_jpeg(const RgbImage &img, int quality, std::vector<uint8_t> &out, std::string *err);
void encode_bmp(const RgbImage &img, std::vector<uint8_t> &out);
void encode_tga(const RgbImage &img, std::vector<uint8_t> &out);

bool encode_image(const RgbImage &img, ImageFormat fmt, int jpeg_quality, std::vector<uint8_t> &out, std::string *err);

// Conversão de meia precisão para float (exposta para testes).
float half_to_float(uint16_t h);

}  // namespace framux
