#ifndef Capture_h
#define Capture_h

#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

class Capture {
public:
  virtual ~Capture();
  static std::unique_ptr<Capture> create(const std::string&, const std::string&);
  bool addOutput(const std::string& uri);
  bool read(AVPacket*);
  bool writeVideo(int&);
  bool writeAudio(int&);
  const int videoIndex() const { return mVideoSrcId; }
  const int audioIndex() const { return mAudioSrcId; }
private:
  Capture(const std::string&, const std::string&);
  bool init();
  bool decodeAudio(AVPacket*);
  bool decodeVideo(AVPacket*, AVFrame**);
  AVFormatContext*    mInputContext;
  AVFormatContext*    mOutputContext;
  struct SwsContext*  mSwsCtx;
  SwrContext*         mResCtx;
  AVAudioFifo*        mAudioFifo;
  AVCodecContext*     mVideoDecoder;
  AVCodecContext*     mAudioDecoder;
  int                 mVideoSrcId, mAudioSrcId;
};

#endif // Capture_h
