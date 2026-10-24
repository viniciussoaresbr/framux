// Conta pixels do contador (verde-limão #C6F432, a cor padrão) no canto
// superior esquerdo de um PNG.
// uso: png_probe <arquivo.png> [lado]   → imprime a contagem
#include <png.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  png_image img{};
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, argv[1])) return 1;
  img.format = PNG_FORMAT_RGB;
  std::vector<unsigned char> buf(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, nullptr, buf.data(), 0, nullptr)) return 1;
  const unsigned side = argc > 2 ? unsigned(std::atoi(argv[2])) : 96;
  int counter = 0;
  for (unsigned y = 0; y < side && y < img.height; ++y)
    for (unsigned x = 0; x < side && x < img.width; ++x) {
      const unsigned char *p = &buf[(y * img.width + x) * 3];
      if (p[0] > 150 && p[1] > 200 && p[2] < 100) ++counter;
    }
  std::printf("%d\n", counter);
  return 0;
}
