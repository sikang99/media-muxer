#ifndef Common_h
#define Common_h

class AudioSink {
public:
  AudioSink() {}
  virtual ~AudioSink() {}
  void sink(uint8_t* data, int sample_per_channel, int sample_rate, int channels) = 0;
};

#endif // Common_h
