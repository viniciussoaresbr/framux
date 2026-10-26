// Lê um vídeo gravado e imprime uma linha por trilha, para o e2e.sh:
//   video <codec> <largura> <altura> <pacotes> <duração em s>
//   audio <codec> <canais> <taxa>
// Sai com 1 se o arquivo não abrir ou não tiver trilha de vídeo.
extern "C" {
#include <libavformat/avformat.h>
}

#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "uso: video_probe <arquivo>\n");
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
  return video ? 0 : 1;
}
