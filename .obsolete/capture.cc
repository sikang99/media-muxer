#include <unistd.h>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/avstring.h>
// #include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
// #include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

class Capture {
public:
  Capture(const std::string&, const std::string&);
  virtual ~Capture();
  bool setOutput(const std::string& fmt, const std::string& uri);
  bool read(AVPacket*);
  bool decode2YUV(AVPacket*, AVFrame**);
  void routine();
private:
  AVFormatContext*    mInputContext;
  AVFormatContext*    mOutputContext;
  struct SwsContext*  mSwsCtx;
  AVCodecContext*     mVideoDecoder;
  AVCodecContext*     mAudioDecoder;
  int                 mVideoId, mAudioId;
};

Capture::Capture(const std::string& driver, const std::string& device)
  : mInputContext (nullptr)
  , mOutputContext (nullptr)
  , mSwsCtx (nullptr)
  , mVideoDecoder (nullptr)
  , mAudioDecoder (nullptr)
  , mVideoId (-1), mAudioId(-1)
{
  mInputContext = avformat_alloc_context();
  if (!mInputContext) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate input context\n");
    return;
  }
  AVInputFormat* ifmt = av_find_input_format(driver.c_str());
  if (!ifmt) {
    av_log(nullptr, AV_LOG_ERROR, "cannot find input driver %s\n", driver.c_str());
    return;
  }
  AVDictionary* options = nullptr;
  av_dict_set(&options, "list_devices", "true", 0);
  avformat_open_input(&mInputContext, nullptr, ifmt, &options);

  if (avformat_open_input(&mInputContext, device.c_str(), ifmt, nullptr) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot open %s\n", device.c_str());
    return;
  }
  if (avformat_find_stream_info(mInputContext, nullptr) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot find stream information\n");
    return;
  }
  av_log(nullptr, AV_LOG_INFO, "[-] %d streams in context\n", mInputContext->nb_streams);
  av_dump_format(mInputContext, 0, device.c_str(), 0);
}

bool Capture::read(AVPacket* pkt)
{
  if (!pkt)
    return false;
  if (av_read_frame(mInputContext, pkt) < 0) {
    av_log(nullptr, AV_LOG_WARNING, "cannot read frame\n");
    return false;
  }
  return true;
}

bool Capture::decode2YUV(AVPacket* pkt, AVFrame** frame)
{
  if (!frame)
    return false;
  AVFrame* frameRGB = av_frame_alloc();
  int finished = 0;
  int ret = avcodec_decode_video2(mVideoDecoder, frameRGB, &finished, pkt);
  av_free_packet(pkt);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot decode frame\n");
    av_frame_free(&frameRGB);
    return false;
  }
  if (finished) {
    *frame = av_frame_alloc();
    (*frame)->format = mVideoDecoder->pix_fmt;
    (*frame)->width = mVideoDecoder->width;
    (*frame)->height = mVideoDecoder->height;
    av_image_alloc((*frame)->data, (*frame)->linesize, mVideoDecoder->width, mVideoDecoder->height, mVideoDecoder->pix_fmt, 32);
    sws_scale(mSwsCtx, const_cast<const uint8_t**>(frameRGB->data), frameRGB->linesize, 0, mVideoDecoder->height, (*frame)->data, (*frame)->linesize);
    return true;
  }
  return false;
}

void Capture::routine()
{
  if (!mInputContext || !mOutputContext)
    return;
  while (true) {





    // av_interleaved_write_frame(mOutputContext, &pkt);
    // usleep(30000);
  }
}

bool Capture::setOutput(const std::string& fmt, const std::string& uri)
{
  mOutputContext = avformat_alloc_context();
  if (!mOutputContext) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate output context\n");
    return false;
  }
  mOutputContext->oformat = av_guess_format(fmt.c_str(), uri.c_str(), nullptr);
  if (!mOutputContext->oformat) {
    av_log(nullptr, AV_LOG_ERROR, "output format not supported\n");
    return false;
  }
  av_strlcpy(mOutputContext->filename, uri.c_str(), sizeof(mOutputContext->filename));
  if (!(mOutputContext->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&mOutputContext->pb, mOutputContext->filename, AVIO_FLAG_WRITE) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "open output file failed\n");
      return false;
    }
  }
  for (size_t i = 0; i < mInputContext->nb_streams; ++i) {
    if (mAudioDecoder && mVideoDecoder)
      break;
    AVCodec* codec = avcodec_find_decoder(mInputContext->streams[i]->codec->codec_id);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find decode codec\n");
      return false;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(decoder, mInputContext->streams[i]->codec) != 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot copy codec context\n");
      return false;
    }
    if (avcodec_open2(decoder, codec, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open decode codec\n");
      return false;
    }
    AVStream* stream = nullptr;
    AVCodecContext* c = nullptr;
    switch (decoder->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      mVideoDecoder = decoder;
      mVideoId = i;
      mSwsCtx = sws_getContext(decoder->width, decoder->height, decoder->pix_fmt,
                                decoder->width, decoder->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
      codec = avcodec_find_encoder(AV_CODEC_ID_H264); // encoder
      if (!codec) {
        av_log(nullptr, AV_LOG_ERROR, "cannot find video encode codec\n");
        return false;
      }
      stream = avformat_new_stream(mOutputContext, codec);
      if (!stream) {
        av_log(nullptr, AV_LOG_ERROR, "cannot create new video stream\n");
        return false;
      }
      c = stream->codec;
      c->codec_id = AV_CODEC_ID_H264;
      c->codec_type = AVMEDIA_TYPE_VIDEO;
      c->bit_rate = 400000; //TODO: calculate
      c->width    = decoder->width;
      c->height   = decoder->height;
      stream->time_base = (AVRational){ 1, 30 };
      c->time_base      = stream->time_base;
      c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
      c->pix_fmt       = AV_PIX_FMT_YUV420P;
      if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
      if (avcodec_open2(c, nullptr, nullptr) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "cannot open video encode codec\n");
        return false;
      }
      break;
    case AVMEDIA_TYPE_AUDIO:
      mAudioDecoder = decoder;
      mAudioId = i;
      codec = avcodec_find_encoder(AV_CODEC_ID_PCM_MULAW); // encoder
      if (!codec) {
        av_log(nullptr, AV_LOG_ERROR, "cannot find audio encode codec\n");
        return false;
      }
      stream = avformat_new_stream(mOutputContext, codec);
      if (!stream) {
        av_log(nullptr, AV_LOG_ERROR, "cannot create new audio stream\n");
        return false;
      }
      c = stream->codec;
      c->bit_rate = 48000;
      c->sample_fmt = AV_SAMPLE_FMT_S16;
      c->channels       = decoder->channels;
      c->channel_layout = decoder->channel_layout;
      c->sample_rate    = decoder->sample_rate;
      stream->time_base = (AVRational){ 1, c->sample_rate };
      if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
      if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
        c->frame_size = 10000;
      if (avcodec_open2(c, nullptr, nullptr) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "cannot open audio encode codec\n");
        return false;
      }
      break;
    default:
      av_log(nullptr, AV_LOG_WARNING, "unsupported media type: %d\n", decoder->codec_type);
      continue;
    }
  }
  avformat_write_header(mOutputContext, nullptr);
  av_dump_format(mOutputContext, 0, mOutputContext->filename, 1);
  return true;
}

Capture::~Capture()
{
  if (mInputContext)
    avformat_close_input(&mInputContext);
  if (mOutputContext) {
    av_write_trailer(mOutputContext);
    if (!(mOutputContext->oformat->flags & AVFMT_NOFILE))
      avio_close(mOutputContext->pb);
    avformat_free_context(mOutputContext);
  }
  if (mSwsCtx)
    sws_freeContext(mSwsCtx);
  if (mVideoDecoder)
    avcodec_close(mVideoDecoder);
  if (mAudioDecoder)
    avcodec_close(mAudioDecoder);
}


int main(int argc, char const *argv[])
{
  av_register_all();
  avformat_network_init();
  avdevice_register_all();

  std::auto_ptr<Capture> capture = std::auto_ptr<Capture>(new Capture("avfoundation", "0:0"));
  if (!capture->setOutput("", "a.mkv"))
    av_log(nullptr, AV_LOG_ERROR, "cannot set output\n");
  // capture->routine();
  return 0;
}
