// Lê um vídeo gravado e imprime uma linha por trilha, para o e2e.sh:
//   video <codec> <largura> <altura> <pacotes> <duração em s>
//   audio <codec> <canais> <taxa>
// Com --pixel X Y, também o pixel (X, Y) do primeiro quadro em YUV 4:2:0:
//   pixel <Y> <U> <V>
// Sai com 1 se o arquivo não abrir ou não tiver trilha de vídeo.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void print_pixel(const char *path, int x, int y) {
  AVFormatContext *f = nullptr;
  if (avformat_open_input(&f, path, nullptr, nullptr) < 0) return;
  avformat_find_stream_info(f, nullptr);
  const int si = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const AVCodec *codec = si >= 0 ? avcodec_find_decoder(f->streams[si]->codecpar->codec_id) : nullptr;
  AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : nullptr;
  if (c && avcodec_parameters_to_context(c, f->streams[si]->codecpar) >= 0 && avcodec_open2(c, codec, nullptr) >= 0) {
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    bool eof = false;
    for (;;) {
      const int r = avcodec_receive_frame(c, fr);
      if (r == 0) {
        const bool nv12 = fr->format == AV_PIX_FMT_NV12;
        if (nv12 || fr->format == AV_PIX_FMT_YUV420P) {
          const uint8_t *uv = fr->data[1] + (y / 2) * fr->linesize[1];
          std::printf("pixel %d %d %d\n", fr->data[0][y * fr->linesize[0] + x], nv12 ? uv[(x / 2) * 2] : uv[x / 2],
                      nv12 ? uv[(x / 2) * 2 + 1] : fr->data[2][(y / 2) * fr->linesize[2] + x / 2]);
        }
        break;
      }
      if (r != AVERROR(EAGAIN) || eof) break;
      if (av_read_frame(f, pkt) < 0) {
        eof = true;
        avcodec_send_packet(c, nullptr);
      } else {
        if (pkt->stream_index == si) avcodec_send_packet(c, pkt);
        av_packet_unref(pkt);
      }
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
  }
  avcodec_free_context(&c);
  avformat_close_input(&f);
}

}  // namespace

int main(int argc, char **argv) {
  const bool pixel = argc == 5 && !std::strcmp(argv[2], "--pixel");
  if (argc != 2 && !pixel) {
    std::fprintf(stderr, "uso: video_probe <arquivo> [--pixel X Y]\n");
    return 2;
  }
  AVFormatContext *f = nullptr;
  if (avformat_open_input(&f, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(f, nullptr) < 0) {
    std::fprintf(stderr, "não abriu %s\n", argv[1]);
    return 1;
  }
  std::vector<long> packets(f->nb_streams, 0);
  AVPacket *pkt = av_packet_alloc();
  while (av_read_frame(f, pkt) >= 0) {
    ++packets[pkt->stream_index];
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  bool video = false;
  for (unsigned i = 0; i < f->nb_streams; ++i) {
    const AVCodecParameters *p = f->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
      video = true;
      std::printf("video %s %d %d %ld %.2f\n", avcodec_get_name(p->codec_id), p->width, p->height, packets[i],
                  f->duration > 0 ? double(f->duration) / AV_TIME_BASE : 0.0);
    } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
      std::printf("audio %s %d %d\n", avcodec_get_name(p->codec_id), p->ch_layout.nb_channels, p->sample_rate);
    }
  }
  avformat_close_input(&f);
  if (video && pixel) print_pixel(argv[1], std::atoi(argv[3]), std::atoi(argv[4]));
  return video ? 0 : 1;
}
