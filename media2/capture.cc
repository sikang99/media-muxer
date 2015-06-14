#include "capture.h"

Capture::Capture(const std::string& driver, const std::string& device)
  : mInputContext (nullptr)
  , mSwsCtx (nullptr)
  , mResCtx (nullptr)
  , mVideoDecoder (nullptr)
  , mAudioDecoder (nullptr)
#ifdef CAPTURE_EXTENDED
  , mOutputContext (nullptr)
  , mAudioFifo (nullptr)
  , mVideoSrcId (-1), mAudioSrcId(-1)
#endif
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
  if (avformat_open_input(&mInputContext, device.c_str(), ifmt, nullptr) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot open %s\n", device.c_str());
    return;
  }
  if (avformat_find_stream_info(mInputContext, nullptr) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot find stream information\n");
    return;
  }
  av_dump_format(mInputContext, Capture::mCtxId, device.c_str(), 0);
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

bool Capture::decodeAudio(AVAudioFifo* fifo)
{
  if (!fifo) {
    av_log(nullptr, AV_LOG_ERROR, "cannot write to nil fifo!\n");
    return false;
  }
  AVPacket src = { 0 };
  av_init_packet(&src);
  if (!read(&src)) {
    av_free_packet(&src);
    return false;
  }
  AVFrame* framePCM = av_frame_alloc();
  int finished = 0;
  int ret = avcodec_decode_audio4(mAudioDecoder, framePCM, &finished, &src);
  av_free_packet(&src);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot decode audio frame\n");
    return false;
  }
  if (!finished)
    return false;
  if (av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + framePCM->nb_samples) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot reallocate fifo\n");
    return false;
  }
  if (framePCM->format == AV_SAMPLE_FMT_S16) {
    if (av_audio_fifo_write(fifo, reinterpret_cast<void**>(framePCM->extended_data), framePCM->nb_samples) < framePCM->nb_samples) {
      av_log(nullptr, AV_LOG_ERROR, "cannot not write data to fifo\n");
      return false;
    }
  } else {
    uint8_t** samples = nullptr;
    bool r = false;
    if (!(samples = reinterpret_cast<uint8_t**>(calloc(framePCM->channels, sizeof(*samples))))) {
      av_log(nullptr, AV_LOG_ERROR, "cannot allocate converted sample buffer ptrs\n");
      return false;
    }
    if (av_samples_alloc(samples, nullptr, framePCM->channels, framePCM->nb_samples, AV_SAMPLE_FMT_S16, 0) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot allocate converted input sample buffer\n");
      goto done;
    }
    if (swr_convert(mResCtx, samples, framePCM->nb_samples, const_cast<const uint8_t**>(framePCM->extended_data), framePCM->nb_samples) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot convert input samples\n");
      goto done;
    }
    if (av_audio_fifo_write(fifo, reinterpret_cast<void**>(samples), framePCM->nb_samples) < framePCM->nb_samples) {
      av_log(nullptr, AV_LOG_ERROR, "cannot not write data to fifo\n");
      goto done;
    }
    r = true;
done:
    av_freep(&samples[0]);
    free(samples);
    return r;
  }
  return true;
}

bool Capture::decodeVideo(AVFrame** frame)
{
  if (!frame)
    return false;
  AVPacket src = { 0 };
  av_init_packet(&src);
  if (!read(&src)) {
    av_free_packet(&src);
    return false;
  }
  AVFrame* frameRGB = av_frame_alloc();
  int finished = 0;
  int ret = avcodec_decode_video2(mVideoDecoder, frameRGB, &finished, &src);
  av_free_packet(&src);
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

bool Capture::init()
{
  if (!mInputContext)
    return false;
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
    switch (decoder->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      mVideoDecoder = decoder;
#ifdef CAPTURE_EXTENDED
      mVideoSrcId = i;
#endif
      mSwsCtx = sws_getContext(decoder->width, decoder->height, decoder->pix_fmt,
                                decoder->width, decoder->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
      break;
    case AVMEDIA_TYPE_AUDIO:
      mAudioDecoder = decoder;
#ifdef CAPTURE_EXTENDED
      mAudioSrcId = i;
#endif
      mResCtx = swr_alloc_set_opts(nullptr,
                                  av_get_default_channel_layout(mAudioDecoder->channels),
                                  AV_SAMPLE_FMT_S16,
                                  mAudioDecoder->sample_rate,
                                  av_get_default_channel_layout(mAudioDecoder->channels),
                                  mAudioDecoder->sample_fmt,
                                  mAudioDecoder->sample_rate,
                                  0, nullptr);
      if (!mResCtx) {
        av_log(nullptr, AV_LOG_ERROR, "cannot allocate resample context\n");
        return false;
      }
      if (swr_init(mResCtx) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "cannot open resample context\n");
        swr_free(&mResCtx);
        mResCtx = nullptr;
        return false;
      }
      break;
    default:
      av_log(nullptr, AV_LOG_WARNING, "unsupported media type: %d\n", decoder->codec_type);
      continue;
    }
  }
  return true;
}

Capture::~Capture()
{
  if (mInputContext)
  avformat_close_input(&mInputContext);
#ifdef CAPTURE_EXTENDED
  if (mAudioFifo)
    av_audio_fifo_free(mAudioFifo);
  if (mOutputContext) {
    av_write_trailer(mOutputContext);
    if (!(mOutputContext->oformat->flags & AVFMT_NOFILE))
      avio_close(mOutputContext->pb);
    avformat_free_context(mOutputContext);
  }
#endif
  if (mSwsCtx)
    sws_freeContext(mSwsCtx);
  if (mResCtx)
    swr_free(&mResCtx);
  if (mVideoDecoder)
    avcodec_close(mVideoDecoder);
  if (mAudioDecoder)
    avcodec_close(mAudioDecoder);
}

std::unique_ptr<Capture> Capture::New(const std::string& driver, const std::string& device) {
  auto capture = std::unique_ptr<Capture>(new Capture(driver, device));
  if (!capture->init())
    capture.reset();
  else
    mCtxId++;
  return capture;
}

int Capture::mCtxId = 0;
