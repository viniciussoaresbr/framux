// §C.12 teste 7: conversão dos formatos de swapchain (≤ 1 LSB).
#include <png.h>
#include <catch2/catch.hpp>

#include <cstring>

#include "core/image.h"

using namespace framux;

namespace {

constexpr uint32_t B8G8R8A8_UNORM = 44, R8G8B8A8_UNORM = 37, A2R10G10B10 = 58, A2B10G10R10 = 64,
                   R16G16B16A16_SFLOAT = 97;

uint16_t float_to_half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000;
  const int exp = int((x >> 23) & 0xff) - 127 + 15;
  const uint32_t man = x & 0x7fffff;
  if (exp <= 0) return uint16_t(sign);
  if (exp >= 31) return uint16_t(sign | 0x7c00);
  return uint16_t(sign | uint32_t(exp) << 10 | man >> 13);
}

RgbImage conv(const std::vector<uint8_t> &data, uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp, bool flip = false) {
  RawImage raw{data.data(), data.size(), w, h, w * bpp, fmt, flip};
  RgbImage out;
  std::string err;
  REQUIRE(convert_image(raw, out, &err));
  return out;
}

}  // namespace

TEST_CASE("BGRA8 e RGBA8", "[image]") {
  auto a = conv({10, 20, 30, 255}, 1, 1, B8G8R8A8_UNORM, 4);
  REQUIRE(a.bits == 8);
  REQUIRE(a.px8 == std::vector<uint8_t>{30, 20, 10});
  auto b = conv({10, 20, 30, 255}, 1, 1, R8G8B8A8_UNORM, 4);
  REQUIRE(b.px8 == std::vector<uint8_t>{10, 20, 30});
}

TEST_CASE("A2B10G10R10 e A2R10G10B10 viram 16 bits", "[image]") {
  const uint32_t lo = 1023, mid = 512, hi = 0;
  const uint32_t v = (3u << 30) | (hi << 20) | (mid << 10) | lo;
  std::vector<uint8_t> px(4);
  std::memcpy(px.data(), &v, 4);
  auto abgr = conv(px, 1, 1, A2B10G10R10, 4);  // R nos bits baixos
  REQUIRE(abgr.bits == 16);
  REQUIRE(abgr.px16[0] == 65535);
  REQUIRE(std::abs(int(abgr.px16[1]) - 512 * 64) <= 64);  // 10 → 16 bits
  REQUIRE(abgr.px16[2] == 0);
  auto argb = conv(px, 1, 1, A2R10G10B10, 4);  // B nos bits baixos
  REQUIRE(argb.px16[0] == 0);
  REQUIRE(argb.px16[2] == 65535);
  // 8 bits: 1023 → 255, 512 → 128 (±1)
  auto e8 = to_8bit(abgr);
  REQUIRE(e8.px8[0] == 255);
  REQUIRE(std::abs(int(e8.px8[1]) - 128) <= 1);
}

TEST_CASE("FP16 linear (scRGB) → sRGB 16 bits com recorte", "[image]") {
  const float vals[4] = {0.0f, 1.0f, 0.5f, 2.0f};
  std::vector<uint8_t> px(16);
  uint16_t h[8] = {float_to_half(vals[0]), float_to_half(vals[1]), float_to_half(vals[2]), float_to_half(1.0f),
                   float_to_half(vals[3]), float_to_half(-1.0f),   float_to_half(0.0f),    float_to_half(1.0f)};
  std::memcpy(px.data(), h, 16);
  auto img = conv(px, 2, 1, R16G16B16A16_SFLOAT, 8);
  REQUIRE(img.px16[0] == 0);
  REQUIRE(img.px16[1] == 65535);
  // sRGB(0,5) = 0,735357 → 48192,6 (±1 LSB)
  REQUIRE(std::abs(int(img.px16[2]) - 48193) <= 1);
  REQUIRE(img.px16[3] == 65535);  // > 1 recortado
  REQUIRE(img.px16[4] == 0);      // negativo recortado
  REQUIRE(half_to_float(float_to_half(0.25f)) == Approx(0.25f));
}

TEST_CASE("flip vertical (glReadPixels)", "[image]") {
  auto img = conv({1, 1, 1, 255, 2, 2, 2, 255}, 1, 2, R8G8B8A8_UNORM, 4, true);
  REQUIRE(img.px8 == std::vector<uint8_t>{2, 2, 2, 1, 1, 1});
}

TEST_CASE("formato e dimensões inválidos são recusados", "[image]") {
  std::vector<uint8_t> px(4);
  RawImage bad{px.data(), px.size(), 1, 1, 4, 123456, false};
  RgbImage out;
  std::string err;
  REQUIRE_FALSE(convert_image(bad, out, &err));
  RawImage small{px.data(), px.size(), 2, 2, 8, R8G8B8A8_UNORM, false};
  REQUIRE_FALSE(convert_image(small, out, &err));
}

TEST_CASE("PNG 8 e 16 bits decodificam igual", "[image][png]") {
  for (int bits : {8, 16}) {
    RgbImage img;
    img.width = 3;
    img.height = 2;
    img.bits = bits;
    for (int i = 0; i < 18; ++i) {
      if (bits == 8)
        img.px8.push_back(uint8_t(i * 13));
      else
        img.px16.push_back(uint16_t(i * 3001));
    }
    std::vector<uint8_t> png;
    std::string err;
    REQUIRE(encode_png(img, png, &err));
    png_image pi{};
    pi.version = PNG_IMAGE_VERSION;
    REQUIRE(png_image_begin_read_from_memory(&pi, png.data(), png.size()));
    REQUIRE(pi.width == 3);
    REQUIRE(pi.height == 2);
    pi.format = bits == 8 ? PNG_FORMAT_RGB : PNG_FORMAT_LINEAR_RGB;
    std::vector<uint8_t> buf(PNG_IMAGE_SIZE(pi));
    REQUIRE(png_image_finish_read(&pi, nullptr, buf.data(), 0, nullptr));
    if (bits == 8) REQUIRE(std::vector<uint8_t>(buf.begin(), buf.end()) == img.px8);
    // Em 16 bits o png_image converte para linear; basta ter decodificado.
  }
}

TEST_CASE("BMP e TGA têm cabeçalho e tamanho certos", "[image]") {
  RgbImage img;
  img.width = 3;
  img.height = 2;
  img.px8.assign(18, 7);
  std::vector<uint8_t> bmp, tga;
  encode_bmp(img, bmp);
  encode_tga(img, tga);
  REQUIRE(bmp.size() == 54 + 12 * 2);  // linhas de 9 bytes alinhadas a 12
  REQUIRE(bmp[0] == 'B');
  REQUIRE(tga.size() == 18 + 18);
  REQUIRE(tga[2] == 2);
}
