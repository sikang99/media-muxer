#include "capture.h"
#include "muxer.h"
#include <chrono>
#include <unistd.h>

static void list_devices() {
#ifdef __APPLE__
  AVFormatContext* ictx = avformat_alloc_context();
  AVInputFormat* ifmt = av_find_input_format("avfoundation");
  AVDictionary* options = nullptr;
  av_dict_set(&options, "list_devices", "true", 0);
  avformat_open_input(&ictx, nullptr, ifmt, &options);
#endif
}

int main(int argc, char const *argv[])
{
  if (argc < 2)
    return 1;
  av_register_all();
  avformat_network_init();
  avdevice_register_all();
  list_devices();

  auto muxer = std::unique_ptr<Muxer>(new Muxer());
#ifdef CAP_AUDIO
  enum AVCodecID audioCodec = AV_CODEC_ID_AAC;
  enum AVCodecID videoCodec = AV_CODEC_ID_NONE;
#else
  enum AVCodecID audioCodec = AV_CODEC_ID_NONE;
  enum AVCodecID videoCodec = AV_CODEC_ID_H264;
#endif
  if (!muxer->addOutput(argv[1], videoCodec, audioCodec)) {
    av_log(nullptr, AV_LOG_ERROR, "cannot set output\n");
    return -1;
  } else {
    av_log(nullptr, AV_LOG_INFO, "[-] start capturing & muxing...\n");
    // av_log_set_level(AV_LOG_ERROR);
    while (true) {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
#ifdef CAP_AUDIO
      muxer->writeAudio();
#else
      muxer->writeVideo();
#endif
      std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
#ifdef CAP_AUDIO
      int interval = 10000 - std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#else
      int interval = 30000 - std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#endif
      if (interval > 0)
        usleep(interval);
      continue;
    }
  }
  return 0;
}
