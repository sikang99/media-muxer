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
  static std::unique_ptr<Capture> New(const std::string&, const std::string&);
  bool decodeAudio(AVAudioFifo*);
  bool decodeVideo(AVFrame**);
  const AVCodecContext* videoCodec() const { return mVideoDecoder; }
  const AVCodecContext* audioCodec() const { return mAudioDecoder; }

#ifdef CAPTURE_EXTENDED
  bool writeVideo(int&);
  bool writeAudio(int&);
  bool addOutput(const std::string& uri);
#endif
private:
  Capture(const std::string&, const std::string&);
  bool init();
  bool read(AVPacket*);
  static int          mCtxId;
  AVFormatContext*    mInputContext;
  struct SwsContext*  mSwsCtx;
  SwrContext*         mResCtx;
  AVCodecContext*     mVideoDecoder;
  AVCodecContext*     mAudioDecoder;
#ifdef CAPTURE_EXTENDED
  AVFormatContext*    mOutputContext;
  AVAudioFifo*        mAudioFifo;
  int                 mVideoSrcId, mAudioSrcId;
#endif
};

#endif // Capture_h
