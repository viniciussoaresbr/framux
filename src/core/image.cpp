#include "core/image.h"

#include <jpeglib.h>
#include <png.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include "framux_build_config.h"

namespace framux {

uint32_t vk_format_bpp(uint32_t f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
  }
}

const char *vk_format_name(uint32_t f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM_PACK32";
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB_PACK32";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "desconhecido";
  }
}

float half_to_float(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t man = h & 0x3ff;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {  // subnormal
      exp = 127 - 15 + 1;
      while (!(man & 0x400)) {
        man <<= 1;
        --exp;
      }
      man &= 0x3ff;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000 | (man << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

namespace {

uint16_t expand10(uint32_t v) { return uint16_t((v << 6) | (v >> 4)); }

// Linear (scRGB) → sRGB codificado em 16 bits, com recorte em [0, 1].
uint16_t linear_to_srgb16(float v) {
  if (!(v > 0.0f)) return 0;  // inclui NaN
  if (v >= 1.0f) return 65535;
  const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
  return uint16_t(std::lround(s * 65535.0f));
}

}  // namespace

bool convert_image(const RawImage &in, RgbImage &out, std::string *err) {
  const uint32_t bpp = vk_format_bpp(in.vk_format);
  if (!bpp) {
    if (err) *err = "formato de imagem não suportado (VkFormat " + std::to_string(in.vk_format) + ")";
    return false;
  }
  if (in.width == 0 || in.height == 0 || in.width > 16384 || in.height > 16384 || in.stride < in.width * bpp ||
      size_t(in.stride) * (in.height - 1) + size_t(in.width) * bpp > in.size) {
    if (err) *err = "dimensões da imagem inconsistentes";
    return false;
  }
  out.width = in.width;
  out.height = in.height;
  const uint32_t f = in.vk_format;
  const bool wide = f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                    f == VK_FORMAT_R16G16B16A16_UNORM || f == VK_FORMAT_R16G16B16A16_SFLOAT;
  out.bits = wide ? 16 : 8;
  const size_t n = size_t(in.width) * in.height * 3;
  if (wide) {
    out.px16.resize(n);
    out.px8.clear();
  } else {
    out.px8.resize(n);
    out.px16.clear();
  }

  for (uint32_t y = 0; y < in.height; ++y) {
    const uint32_t sy = in.flip_y ? in.height - 1 - y : y;
    const uint8_t *row = in.data + size_t(sy) * in.stride;
    const size_t o = size_t(y) * in.width * 3;
    for (uint32_t x = 0; x < in.width; ++x) {
      const uint8_t *p = row + size_t(x) * bpp;
      switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
          out.px8[o + x * 3 + 0] = p[2];
          out.px8[o + x * 3 + 1] = p[1];
          out.px8[o + x * 3 + 2] = p[0];
          break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:  // little-endian: bytes R, G, B, A
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
          out.px8[o + x * 3 + 0] = p[0];
          out.px8[o + x * 3 + 1] = p[1];
          out.px8[o + x * 3 + 2] = p[2];
          break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
          uint32_t v;
          std::memcpy(&v, p, 4);
          const uint32_t lo = v & 0x3ff, mid = (v >> 10) & 0x3ff, hi = (v >> 20) & 0x3ff;
          const bool rgb_order = f == VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // R nos bits baixos
          out.px16[o + x * 3 + 0] = expand10(rgb_order ? lo : hi);
          out.px16[o + x * 3 + 1] = expand10(mid);
          out.px16[o + x * 3 + 2] = expand10(rgb_order ? hi : lo);
          break;
        }
        case VK_FORMAT_R16G16B16A16_UNORM: {
          uint16_t c[3];
          std::memcpy(c, p, 6);
          for (int k = 0; k < 3; ++k) out.px16[o + x * 3 + k] = c[k];
          break;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
          uint16_t c[3];
          std::memcpy(c, p, 6);
          for (int k = 0; k < 3; ++k) out.px16[o + x * 3 + k] = linear_to_srgb16(half_to_float(c[k]));
          break;
        }
      }
    }
  }
  return true;
}

RgbImage to_8bit(const RgbImage &in) {
  if (in.bits == 8) return in;
  RgbImage out;
  out.width = in.width;
  out.height = in.height;
  out.bits = 8;
  out.px8.resize(in.px16.size());
  for (size_t i = 0; i < in.px16.size(); ++i) out.px8[i] = uint8_t((uint32_t(in.px16[i]) * 255 + 32767) / 65535);
  return out;
}

namespace {

void png_write_cb(png_structp png, png_bytep data, png_size_t len) {
  auto *v = static_cast<std::vector<uint8_t> *>(png_get_io_ptr(png));
  v->insert(v->end(), data, data + len);
}
void png_flush_cb(png_structp) {}

}  // namespace

bool encode_png(const RgbImage &img, std::vector<uint8_t> &out, std::string *err) {
  out.clear();
  // Tudo que muda de estado fica pronto antes do setjmp.
  std::vector<uint16_t> be;  // PNG de 16 bits é big-endian
  std::vector<png_bytep> rows(img.height);
  if (img.bits == 16) {
    be.resize(img.px16.size());
    for (size_t i = 0; i < be.size(); ++i) be[i] = uint16_t((img.px16[i] >> 8) | (img.px16[i] << 8));
    for (uint32_t y = 0; y < img.height; ++y)
      rows[y] = reinterpret_cast<png_bytep>(be.data() + size_t(y) * img.width * 3);
  } else {
    for (uint32_t y = 0; y < img.height; ++y)
      rows[y] = const_cast<png_bytep>(img.px8.data() + size_t(y) * img.width * 3);
  }
  std::string sw = std::string("framux ") + FRAMUX_VERSION;
  char key[] = "Software";
  png_text text[1];
  text[0].compression = PNG_TEXT_COMPRESSION_NONE;
  text[0].key = key;
  text[0].text = sw.data();
  text[0].text_length = sw.size();

  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    png_destroy_write_struct(&png, &info);
    if (err) *err = "libpng: sem memória";
    return false;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    if (err) *err = "libpng: falha ao codificar";
    return false;
  }
  png_set_write_fn(png, &out, png_write_cb, png_flush_cb);
  png_set_IHDR(png, info, img.width, img.height, img.bits, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  // Nível 3: bem mais rápido que o padrão, com arquivos pouco maiores.
  png_set_compression_level(png, 3);
  png_set_text(png, info, text, 1);
  png_write_info(png, info);
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return true;
}

namespace {
struct JpegErr {
  jpeg_error_mgr mgr;
  jmp_buf jmp;
  char msg[JMSG_LENGTH_MAX];
};
void jpeg_error_exit(j_common_ptr cinfo) {
  auto *e = reinterpret_cast<JpegErr *>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, e->msg);
  longjmp(e->jmp, 1);
}
}  // namespace

bool encode_jpeg(const RgbImage &src, int quality, std::vector<uint8_t> &out, std::string *err) {
  const RgbImage img = to_8bit(src);
  jpeg_compress_struct cinfo{};
  JpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.mgr);
  jerr.mgr.error_exit = jpeg_error_exit;
  unsigned char *mem = nullptr;
  unsigned long mem_size = 0;
  if (setjmp(jerr.jmp)) {
    jpeg_destroy_compress(&cinfo);
    std::free(mem);
    if (err) *err = std::string("libjpeg: ") + jerr.msg;
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &mem, &mem_size);
  cinfo.image_width = img.width;
  cinfo.image_height = img.height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  const std::string sw = std::string("framux ") + FRAMUX_VERSION;
  jpeg_write_marker(&cinfo, JPEG_COM, reinterpret_cast<const JOCTET *>(sw.data()), unsigned(sw.size()));
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(img.px8.data() + size_t(cinfo.next_scanline) * img.width * 3);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  out.assign(mem, mem + mem_size);
  jpeg_destroy_compress(&cinfo);
  std::free(mem);
  return true;
}

namespace {
void put16(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t> &v, uint32_t x) {
  put16(v, x & 0xffff);
  put16(v, x >> 16);
}
}  // namespace

void encode_bmp(const RgbImage &src, std::vector<uint8_t> &out) {
  const RgbImage img = to_8bit(src);
  const uint32_t row = (img.width * 3 + 3) & ~3u;
  const uint32_t data = row * img.height;
  out.clear();
  out.reserve(54 + data);
  out.push_back('B');
  out.push_back('M');
  put32(out, 54 + data);
  put32(out, 0);
  put32(out, 54);
  put32(out, 40);  // BITMAPINFOHEADER
  put32(out, img.width);
  put32(out, img.height);  // positivo: linhas de baixo para cima
  put16(out, 1);
  put16(out, 24);
  put32(out, 0);
  put32(out, data);
  put32(out, 2835);
  put32(out, 2835);
  put32(out, 0);
  put32(out, 0);
  for (uint32_t y = img.height; y-- > 0;) {
    const uint8_t *p = img.px8.data() + size_t(y) * img.width * 3;
    for (uint32_t x = 0; x < img.width; ++x) {
      out.push_back(p[x * 3 + 2]);
      out.push_back(p[x * 3 + 1]);
      out.push_back(p[x * 3 + 0]);
    }
    for (uint32_t pad = img.width * 3; pad < row; ++pad) out.push_back(0);
  }
}

void encode_tga(const RgbImage &src, std::vector<uint8_t> &out) {
  const RgbImage img = to_8bit(src);
  out.assign(18, 0);
  out[2] = 2;  // true-color sem compressão
  out[12] = uint8_t(img.width);
  out[13] = uint8_t(img.width >> 8);
  out[14] = uint8_t(img.height);
  out[15] = uint8_t(img.height >> 8);
  out[16] = 24;
  out[17] = 0x20;  // origem no topo
  out.reserve(18 + img.px8.size());
  for (size_t i = 0; i < img.px8.size(); i += 3) {
    out.push_back(img.px8[i + 2]);
    out.push_back(img.px8[i + 1]);
    out.push_back(img.px8[i + 0]);
  }
}

bool encode_image(const RgbImage &img, ImageFormat fmt, int jpeg_quality, std::vector<uint8_t> &out, std::string *err) {
  switch (fmt) {
    case ImageFormat::Jpeg: return encode_jpeg(img, jpeg_quality, out, err);
    case ImageFormat::Bmp: encode_bmp(img, out); return true;
    case ImageFormat::Tga: encode_tga(img, out); return true;
    default: return encode_png(img, out, err);
  }
}

}  // namespace framux
