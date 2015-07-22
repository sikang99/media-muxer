#ifndef Muxer_h
#define Muxer_h

#include "capture.h"
#include "player.h"
#include <map>

class Muxer {
public:
  Muxer();
  virtual ~Muxer();
  bool addOutput(const std::string& uri,
                  enum AVCodecID videoCodec = AV_CODEC_ID_H264,
                  enum AVCodecID audioCodec = AV_CODEC_ID_AAC);
  void writeVideo();
  void writeAudio();
private:
  int64_t mStartTime;
  int mVideoId, mAudioId;
  AVAudioFifo* mAudioFifo;
  std::unique_ptr<Capture> mVideo;
  std::unique_ptr<Capture> mAudio;
  std::map<std::string, AVFormatContext*> mOutputs;
  std::unique_ptr<render::Player> mPlayer;
};

#endif // Muxer_h
