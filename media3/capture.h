#ifndef Capture_h
#define Capture_h

#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

class Capture {
public:
    virtual ~Capture();
    static std::unique_ptr<Capture> New(const std::string&, const std::string&, const std::string&);
    // bool writeVideo(int&);
    // bool writeAudio(int&);

private:
    Capture() = delete;
    explicit Capture(const std::string&, const std::string&);
    bool decodeAudio(const AVPacket*, AVAudioFifo*);
    bool decodeVideo(const AVPacket*, AVFrame**);
    bool init(const std::string&);

    AVFormatContext* mInputContext;
    AVFormatContext* mOutputContext;
    struct SwsContext* mSwsCtx;
    SwrContext* mResCtx;
    AVCodecContext* mVideoDecoder;
    AVCodecContext* mAudioDecoder;
    AVAudioFifo* mAudioFifo;
    bool mReady;
};

#endif // Capture_h
