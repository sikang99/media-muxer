#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

class Capture {
public:
  Capture(const std::string&, const std::string&);
  virtual ~Capture();
  bool addOutput(const std::string& uri);
  bool read(AVPacket*);
  bool decodeVideo(AVPacket*, AVFrame**);
  bool decodeAudio(AVPacket*, AVFrame**);
  bool writeVideo(AVFrame*, int&);
  bool writeAudio(AVFrame*, int&);
  const int videoIndex() const { return mVideoSrcId; }
  const int audioIndex() const { return mAudioSrcId; }
private:
  void init();
  AVFormatContext*    mInputContext;
  AVFormatContext*    mOutputContext;
  struct SwsContext*  mSwsCtx;
  SwrContext*         mResCtx;
  AVCodecContext*     mVideoDecoder;
  AVCodecContext*     mAudioDecoder;
  int                 mVideoSrcId, mAudioSrcId;
};
