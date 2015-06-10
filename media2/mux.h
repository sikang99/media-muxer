#include "capture.h"
#include <memory>

class Muxer {
public:
  Muxer();
  virtual ~Muxer();
private:
  std::unique_ptr<Capture> mVideo;
  std::unique_ptr<Capture> mAudio;
};