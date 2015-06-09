#include "capture.h"
#include <memory>

class Muxer {
public:
  Muxer();
  virtual ~Muxer();
private:
  std::auto_ptr<Capture> mVideo;
  std::auto_ptr<Capture> mAudio;
};