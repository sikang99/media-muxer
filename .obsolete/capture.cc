#include "capture.h"

#include <unistd.h>
#include <memory>

Capture::Capture(const std::string& driver, const std::string& device)
  : mInputContext (nullptr)
  , mOutputContext (nullptr)
  , mSwsCtx (nullptr)
  , mResCtx (nullptr)
  , mAudioFifo (nullptr)
  , mVideoDecoder (nullptr)
  , mAudioDecoder (nullptr)
  , mVideoSrcId (-1), mAudioSrcId(-1)
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
  init();
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

bool Capture::decodeAudio(AVPacket* pkt)
{
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

  int buffer_size = av_samples_get_buffer_size(nullptr, framePCM->channels, framePCM->nb_samples, mOutputContext->streams[mAudioSrcId]->codec->sample_fmt, 0);
  uint8_t* samples = reinterpret_cast<uint8_t*>(av_malloc(buffer_size)); //FIXME: release this buffer properly.
  if (!samples) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate converted sample buffer\n");
    return false;
  }
  if (swr_convert(mResCtx, &samples, framePCM->nb_samples, const_cast<const uint8_t**>(framePCM->extended_data), framePCM->nb_samples) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot convert input samples\n");
    return false;
  }
  if (av_audio_fifo_realloc(mAudioFifo, av_audio_fifo_size(mAudioFifo) + framePCM->nb_samples) < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot reallocate fifo\n");
    return false;
  }
  if (av_audio_fifo_write(mAudioFifo, (void**)(&samples), framePCM->nb_samples) < framePCM->nb_samples) {
    av_log(nullptr, AV_LOG_ERROR, "cannot not write data to fifo\n");
    return false;
  }
  av_freep(&samples);
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

bool Capture::writeAudio(int& pts)
{
  while (av_audio_fifo_size(mAudioFifo) < mOutputContext->streams[mAudioSrcId]->codec->frame_size) {
    AVPacket src = { 0 };
    av_init_packet(&src);
    if (!read(&src))
      return false;
    if (!decodeAudio(&src))
      return false;
    av_free_packet(&src);
  }
  AVFrame* frame;
  while (av_audio_fifo_size(mAudioFifo) >= mOutputContext->streams[mAudioSrcId]->codec->frame_size) {
    frame = av_frame_alloc();
    frame->nb_samples = mOutputContext->streams[mAudioSrcId]->codec->frame_size;
    frame->channel_layout = mOutputContext->streams[mAudioSrcId]->codec->channel_layout;
    frame->format = mOutputContext->streams[mAudioSrcId]->codec->sample_fmt;
    frame->sample_rate = mOutputContext->streams[mAudioSrcId]->codec->sample_rate;
    int frame_size = frame->nb_samples;
    if (av_frame_get_buffer(frame, 0) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot allocate output frame samples\n");
      av_frame_free(&frame);
      return false;
    }
    if (av_audio_fifo_read(mAudioFifo, (void**)frame->data, frame_size) < frame_size) {
      av_log(nullptr, AV_LOG_ERROR, "cannot read enough data from fifo\n");
      av_frame_free(&frame);
      return false;
    }
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    frame->pts = pts;
    int finished = 0;
    int ret = avcodec_encode_audio2(mOutputContext->streams[mAudioSrcId]->codec, &pkt, frame, &finished);
    av_frame_free(&frame);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot encode a audio frame\n");
      av_free_packet(&pkt);
      return false;
    }
    if (!finished)
      return false;
    av_packet_rescale_ts(&pkt, mOutputContext->streams[mAudioSrcId]->codec->time_base, mOutputContext->streams[mAudioSrcId]->time_base);
    // pkt.dts = pkt.pts;
    pkt.stream_index = mAudioSrcId;
    pts += frame_size;
    if (av_interleaved_write_frame(mOutputContext, &pkt) != 0)
      return false;
  }
  return true;
}

bool Capture::writeVideo(AVFrame* frame, int& pts)
{
  AVPacket pkt = { 0 };
  av_init_packet(&pkt);
  frame->pts = pts;
  int finished = 0;
  int ret = avcodec_encode_video2(mOutputContext->streams[mVideoSrcId]->codec, &pkt, frame, &finished);
  av_frame_free(&frame);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "cannot encode a video frame\n");
    av_free_packet(&pkt);
    return false;
  }
  if (!finished)
    return false;
  av_packet_rescale_ts(&pkt, mOutputContext->streams[mVideoSrcId]->codec->time_base, mOutputContext->streams[mVideoSrcId]->time_base);
  // pkt.dts = pkt.pts;
  pkt.stream_index = mVideoSrcId;
  pts++;
  return av_interleaved_write_frame(mOutputContext, &pkt) == 0;
}

void Capture::init()
{
  for (size_t i = 0; i < mInputContext->nb_streams; ++i) {
    if (mAudioDecoder && mVideoDecoder)
      break;
    AVCodec* codec = avcodec_find_decoder(mInputContext->streams[i]->codec->codec_id);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find decode codec\n");
      return;
    }
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(decoder, mInputContext->streams[i]->codec) != 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot copy codec context\n");
      return;
    }
    if (avcodec_open2(decoder, codec, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open decode codec\n");
      return;
    }
    switch (decoder->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      mVideoDecoder = decoder;
      mVideoSrcId = i;
      mSwsCtx = sws_getContext(decoder->width, decoder->height, decoder->pix_fmt,
                                decoder->width, decoder->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
      break;
    case AVMEDIA_TYPE_AUDIO:
      mAudioDecoder = decoder;
      mAudioSrcId = i;
      break;
    default:
      av_log(nullptr, AV_LOG_WARNING, "unsupported media type: %d\n", decoder->codec_type);
      continue;
    }
  }
}

bool Capture::addOutput(const std::string& uri)
{
  mOutputContext = avformat_alloc_context();
  if (!mOutputContext) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate output context\n");
    return false;
  }
  const char* fmt = nullptr;
  AVStream* stream = nullptr;
  AVCodecContext* c = nullptr;
  AVCodec* codec = nullptr;
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
  if (mVideoDecoder) { // add video stream
    codec = avcodec_find_encoder(AV_CODEC_ID_H264);
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
    c->width    = mVideoDecoder->width;
    c->height   = mVideoDecoder->height;
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
  }
  if (mAudioDecoder) { // add audio stream
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find audio encode codec\n");
      goto fail;
    }
    stream = avformat_new_stream(mOutputContext, codec);
    if (!stream) {
      av_log(nullptr, AV_LOG_ERROR, "cannot create new audio stream\n");
      goto fail;
    }
    c = stream->codec;
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    c->channels       = mAudioDecoder->channels;
    c->channel_layout = av_get_default_channel_layout(mAudioDecoder->channels);
    c->sample_rate    = mAudioDecoder->sample_rate;
    stream->time_base = (AVRational){ 1, c->sample_rate };
    if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
      c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
      c->frame_size = 10000;
    if (avcodec_open2(c, nullptr, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open audio encode codec\n");
      goto fail;
    }
    if (/*mAudioDecoder->sample_fmt != AV_SAMPLE_FMT_S16 && */!mResCtx) {
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
      if (!mAudioFifo) {
        if (!(mAudioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, c->channels, 1))) {
          av_log(nullptr, AV_LOG_ERROR, "cannot allocate audio fifo\n");
          goto fail;
        }
      }
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
#ifdef CAP_AUDIO
      capture->writeAudio(pts);
      usleep(5000);
      continue;
#else
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
        } else
          av_log(nullptr, AV_LOG_WARNING, "unrecognised packet index: %d\n", pkt.stream_index);
      }
      av_free_packet(&pkt);
#endif
    }
  }
  return 0;
}
#endif // MAIN_PROGRAM
