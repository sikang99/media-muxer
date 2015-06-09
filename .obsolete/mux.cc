#include "mux.h"

Muxer::Muxer()
{
#ifdef __APPLE__
  mAudio.reset(new Capture("avfoundation", ":0"));
  mVideo.reset(new Capture("avfoundation", "0"));
#else
  mAudio.reset(new Capture("alsa", "hw:1"));
  mVideo.reset(new Capture("v4l2", "/dev/video0"));
#endif
}

Muxer::~Muxer()
{
}