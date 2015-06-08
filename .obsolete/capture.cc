#include <unistd.h>
#include <memory>
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
  const int videoIndex() const { return mVideoId; }
  const int audioIndex() const { return mAudioId; }
private:
  AVFormatContext*    mInputContext;
  AVFormatContext*    mOutputContext;
  struct SwsContext*  mSwsCtx;
  SwrContext*         mResCtx;
  AVCodecContext*     mVideoDecoder;
  AVCodecContext*     mAudioDecoder;
  int                 mVideoId, mAudioId;
};

Capture::Capture(const std::string& driver, const std::string& device)
  : mInputContext (nullptr)
  , mOutputContext (nullptr)
  , mSwsCtx (nullptr)
  , mResCtx (nullptr)
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
#ifdef __APPLE__
  AVDictionary* options = nullptr;
  av_dict_set(&options, "list_devices", "true", 0);
  avformat_open_input(&mInputContext, nullptr, ifmt, &options);
#endif
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

bool Capture::decodeAudio(AVPacket* pkt, AVFrame** frame)
{
  if (!frame)
    return false;
  AVFrame* framePCM = av_frame_alloc();
  int finished = 0;
  int ret = avcodec_decode_audio4(mAudioDecoder, framePCM, &finished, pkt);
  av_free_packet(pkt);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot decode audio frame\n");
    return false;
  }
  if (!finished)
    return false;
  if (mResCtx) {
    AVFrame* frameResampled = av_frame_alloc();
    if (!frameResampled) {
        av_log(nullptr, AV_LOG_ERROR, "cannot allocate audio frame\n");
        return false;
    }
    AVCodecContext* c = mOutputContext->streams[mAudioId]->codec;
    frameResampled->nb_samples     = framePCM->nb_samples;
    frameResampled->format         = c->sample_fmt;
    frameResampled->channel_layout = framePCM->channel_layout;
    int buffer_size = av_samples_get_buffer_size(nullptr, framePCM->channels, framePCM->nb_samples, c->sample_fmt, 0);
    uint8_t* samples = reinterpret_cast<uint8_t*>(av_malloc(buffer_size)); //FIXME: release this buffer properly.
    if (!samples) {
      av_log(nullptr, AV_LOG_ERROR, "cannot allocate converted sample buffer\n");
      return false;
    }
    if (avcodec_fill_audio_frame(frameResampled, c->channels, c->sample_fmt, samples, buffer_size, 0) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot setup converted audio frame\n");
      av_freep(samples);
      return false;
    }
    if (swr_convert(mResCtx, &samples, framePCM->nb_samples, const_cast<const uint8_t**>(framePCM->extended_data), framePCM->nb_samples) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot convert input samples\n");
      return false;
    }
    *frame = frameResampled;
  } else
    *frame = framePCM;
  return true;
}

bool Capture::decodeVideo(AVPacket* pkt, AVFrame** frame)
{
  if (!frame)
    return false;
  AVFrame* frameRGB = av_frame_alloc();
  int finished = 0;
  int ret = avcodec_decode_video2(mVideoDecoder, frameRGB, &finished, pkt);
  av_free_packet(pkt);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot decode video frame\n");
    av_frame_free(&frameRGB);
    return false;
  }
  if (finished) {
    *frame = av_frame_alloc();
    (*frame)->format = AV_PIX_FMT_YUV420P;
    (*frame)->width = mVideoDecoder->width;
    (*frame)->height = mVideoDecoder->height;
    av_image_alloc((*frame)->data, (*frame)->linesize, mVideoDecoder->width, mVideoDecoder->height, AV_PIX_FMT_YUV420P, 32);
    sws_scale(mSwsCtx,
      const_cast<const uint8_t**>(frameRGB->data), frameRGB->linesize,
      0, mVideoDecoder->height,
      (*frame)->data, (*frame)->linesize);
    return true;
  }
  return false;
}

bool Capture::writeAudio(AVFrame* frame, int& pts)
{
  AVPacket pkt = { 0 };
  av_init_packet(&pkt);
  frame->pts = pts;
  int finished = 0;
  int frame_size = frame->nb_samples;
  int ret = avcodec_encode_audio2(mOutputContext->streams[mAudioId]->codec, &pkt, frame, &finished);
  av_frame_free(&frame);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot encode a audio frame\n");
    av_free_packet(&pkt);
    return false;
  }
  if (!finished)
    return false;
  av_packet_rescale_ts(&pkt, mOutputContext->streams[mAudioId]->codec->time_base, mOutputContext->streams[mAudioId]->time_base);
  // pkt.dts = pkt.pts;
  pkt.stream_index = mAudioId;
  pts += frame_size;
  return av_interleaved_write_frame(mOutputContext, &pkt) == 0;
}

bool Capture::writeVideo(AVFrame* frame, int& pts)
{
  AVPacket pkt = { 0 };
  av_init_packet(&pkt);
  frame->pts = pts;
  int finished = 0;
  int ret = avcodec_encode_video2(mOutputContext->streams[mVideoId]->codec, &pkt, frame, &finished);
  av_frame_free(&frame);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot encode a video frame\n");
    av_free_packet(&pkt);
    return false;
  }
  if (!finished)
    return false;
  av_packet_rescale_ts(&pkt, mOutputContext->streams[mVideoId]->codec->time_base, mOutputContext->streams[mVideoId]->time_base);
  // pkt.dts = pkt.pts;
  pkt.stream_index = mVideoId;
  pts++;
  return av_interleaved_write_frame(mOutputContext, &pkt) == 0;
}

bool Capture::addOutput(const std::string& uri)
{
  mOutputContext = avformat_alloc_context();
  if (!mOutputContext) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate output context\n");
    return false;
  }
  const char* fmt = nullptr;
  if (uri.compare(0, 7, "rtsp://") == 0)
    fmt = "rtsp";
  mOutputContext->oformat = av_guess_format(fmt, uri.c_str(), nullptr);
  if (!mOutputContext->oformat) {
    av_log(nullptr, AV_LOG_ERROR, "output format not supported\n");
    goto fail;
  }
  av_strlcpy(mOutputContext->filename, uri.c_str(), sizeof(mOutputContext->filename));
  if (!(mOutputContext->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&mOutputContext->pb, mOutputContext->filename, AVIO_FLAG_WRITE) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "open output file failed\n");
      goto fail;
    }
  }
  for (size_t i = 0; i < mInputContext->nb_streams; ++i) {
    if (mAudioDecoder && mVideoDecoder)
      break;
    AVCodec* codec = avcodec_find_decoder(mInputContext->streams[i]->codec->codec_id);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find decode codec\n");
      goto fail;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(decoder, mInputContext->streams[i]->codec) != 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot copy codec context\n");
      goto fail;
    }
    if (avcodec_open2(decoder, codec, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open decode codec\n");
      goto fail;
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
        goto fail;
      }
      stream = avformat_new_stream(mOutputContext, codec);
      if (!stream) {
        av_log(nullptr, AV_LOG_ERROR, "cannot create new video stream\n");
        goto fail;
      }
      c = stream->codec;
      c->codec_id = AV_CODEC_ID_H264;
      c->codec_type = AVMEDIA_TYPE_VIDEO;
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
        goto fail;
      }
      break;
    case AVMEDIA_TYPE_AUDIO:
      mAudioDecoder = decoder;
      mAudioId = i;
      codec = avcodec_find_encoder(AV_CODEC_ID_AAC); // encoder
      if (!codec) {
        av_log(nullptr, AV_LOG_ERROR, "cannot find audio encode codec\n");
        goto fail;
      }
      stream = avformat_new_stream(mOutputContext, codec);
      if (!stream) {
        av_log(nullptr, AV_LOG_ERROR, "cannot create new audio stream\n");
        goto fail;
      }
      av_log(nullptr, AV_LOG_INFO, "audio sample_fmt: %s\n", av_get_sample_fmt_name(mAudioDecoder->sample_fmt));
      c = stream->codec;
      c->sample_fmt = AV_SAMPLE_FMT_S16;
      c->channels       = decoder->channels;
      c->channel_layout = av_get_default_channel_layout(decoder->channels);
      c->sample_rate    = decoder->sample_rate;
      stream->time_base = (AVRational){ 1, c->sample_rate };
      if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
      if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
        c->frame_size = 10000;
      if (avcodec_open2(c, nullptr, nullptr) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "cannot open audio encode codec\n");
        goto fail;
      }
      if (mAudioDecoder->sample_fmt != AV_SAMPLE_FMT_S16 && !mResCtx) {
        mResCtx = swr_alloc_set_opts(nullptr,
                                      av_get_default_channel_layout(c->channels),
                                      c->sample_fmt,
                                      c->sample_rate,
                                      av_get_default_channel_layout(mAudioDecoder->channels),
                                      mAudioDecoder->sample_fmt,
                                      mAudioDecoder->sample_rate,
                                      0, nullptr);
        if (!mResCtx) {
          av_log(nullptr, AV_LOG_ERROR, "cannot allocate resample context\n");
          goto fail;
        }
        if (swr_init(mResCtx) < 0) {
          av_log(nullptr, AV_LOG_ERROR, "cannot open resample context\n");
          swr_free(&mResCtx);
          mResCtx = nullptr;
          goto fail;
        }
      }
      break;
    default:
      av_log(nullptr, AV_LOG_WARNING, "unsupported media type: %d\n", decoder->codec_type);
      continue;
    }
  }
  if (avformat_write_header(mOutputContext, nullptr) < 0)
    goto fail;
  av_dump_format(mOutputContext, 0, mOutputContext->filename, 1);
  return true;
fail:
  if (!(mOutputContext->oformat->flags & AVFMT_NOFILE))
    avio_close(mOutputContext->pb);
  avformat_free_context(mOutputContext);
  mOutputContext = nullptr;
  return false;
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
  if (mResCtx)
    swr_free(&mResCtx);
  if (mVideoDecoder)
    avcodec_close(mVideoDecoder);
  if (mAudioDecoder)
    avcodec_close(mAudioDecoder);
}

#ifdef MAIN_PROGRAM
int main(int argc, char const *argv[])
{
  if (argc < 2)
    return 1;
  av_register_all();
  avformat_network_init();
  avdevice_register_all();

#ifdef __APPLE__
  #ifdef CAP_AUDIO
  std::shared_ptr<Capture> capture = std::shared_ptr<Capture>(new Capture("avfoundation", ":0"));
  #else
  std::shared_ptr<Capture> capture = std::shared_ptr<Capture>(new Capture("avfoundation", "0"));
  #endif
#else
  #ifdef CAP_AUDIO
  std::shared_ptr<Capture> capture = std::shared_ptr<Capture>(new Capture("alsa", "hw:1"));
  #else
  std::shared_ptr<Capture> capture = std::shared_ptr<Capture>(new Capture("v4l2", "/dev/video0"));
  #endif
#endif
  std::string uri = argv[1];
  if (!capture->addOutput(uri))
    av_log(nullptr, AV_LOG_ERROR, "cannot set output\n");
  else {
    av_log(nullptr, AV_LOG_INFO, "start capturing & muxing...\n");
    av_log_set_level(AV_LOG_ERROR);
    int pts = 0;
    while (true) {
      AVPacket pkt = { 0 };
      av_init_packet(&pkt);
      AVFrame* frame = nullptr;
      if (capture->read(&pkt)) {
        if (pkt.stream_index == capture->videoIndex()) {
          if (capture->decodeVideo(&pkt, &frame)) {
            capture->writeVideo(frame, pts);
            usleep(30000);
            continue;
          }
        } else if (pkt.stream_index == capture->audioIndex()) {
          if (capture->decodeAudio(&pkt, &frame)) {
            capture->writeAudio(frame, pts);
            usleep(1000);
            continue;
          }
        } else
          av_log(nullptr, AV_LOG_WARNING, "unrecognised packet index: %d\n", pkt.stream_index);
      }
      av_free_packet(&pkt);
    }
  }
  return 0;
}
#endif // MAIN_PROGRAM
