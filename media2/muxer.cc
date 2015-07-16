#include "muxer.h"
#include <sys/time.h>

static inline int64_t currentTimeMillis() {
  timeval time;
  gettimeofday(&time, nullptr);
  return ((time.tv_sec * 1000) + (time.tv_usec / 1000));
}

Muxer::Muxer()
  : mStartTime (currentTimeMillis())
  , mVideoId (-1)
  , mAudioId (-1)
  , mAudioFifo (nullptr)
#ifdef __APPLE__
  , mVideo (Capture::New("avfoundation", "0"))
  , mAudio (Capture::New("avfoundation", ":0"))
#else
  , mVideo (Capture::New("v4l2", "/dev/video0"))
  , mAudio (Capture::New("alsa", "hw:2"))
#endif
{
}

Muxer::~Muxer()
{
  if (mAudioFifo)
    av_audio_fifo_free(mAudioFifo);
  std::map<std::string, AVFormatContext*>::iterator it = mOutputs.begin();
  for (; it != mOutputs.end(); ++it) {
    AVFormatContext* context = (it->second);
    if (context) {
      av_write_trailer(context);
      if (!(context->oformat->flags & AVFMT_NOFILE))
        avio_close(context->pb);
      avformat_free_context(context);
    }
  }
}

void Muxer::writeVideo()
{
  AVFrame* frame = nullptr;
  if (!mVideo->decodeVideo(&frame)) {
    return;
  }
  frame->pts = currentTimeMillis() - mStartTime;
  std::map<std::string, AVFormatContext*>::iterator it = mOutputs.begin();
  for (; it != mOutputs.end(); ++it) {
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    AVFormatContext* context = (it->second);
    int finished = 0;
    int ret = avcodec_encode_video2(context->streams[mVideoId]->codec, &pkt, frame, &finished);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot encode a video frame\n");
      av_free_packet(&pkt);
      continue;
    }
    if (!finished)
      continue;
    pkt.pts = (int64_t)(pkt.pts / (av_q2d(context->streams[mVideoId]->time_base) * 1000));
    pkt.stream_index = mVideoId;
    av_interleaved_write_frame(context, &pkt);
  }
  av_frame_free(&frame);
}

void Muxer::writeAudio()
{
  AVFormatContext* context = (mOutputs.begin()->second);
  while (av_audio_fifo_size(mAudioFifo) < 1024) {
    if (!mAudio->decodeAudio(mAudioFifo))
      return;
  }
  AVFrame* frame = nullptr;
  while (av_audio_fifo_size(mAudioFifo) >= 1024) {
    frame = av_frame_alloc();
    frame->nb_samples = context->streams[mAudioId]->codec->frame_size;
    frame->channel_layout = context->streams[mAudioId]->codec->channel_layout;
    frame->format = context->streams[mAudioId]->codec->sample_fmt;
    frame->sample_rate = context->streams[mAudioId]->codec->sample_rate;
    int frame_size = frame->nb_samples;
    if (av_frame_get_buffer(frame, 0) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot allocate output frame samples\n");
      av_frame_free(&frame);
      return;
    }
    if (av_audio_fifo_read(mAudioFifo, (void**)frame->data, frame_size) < frame_size) {
      av_log(nullptr, AV_LOG_ERROR, "cannot read enough data from fifo\n");
      av_frame_free(&frame);
      return;
    }
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    frame->pts = currentTimeMillis() - mStartTime;
    int finished = 0;
    int ret = avcodec_encode_audio2(context->streams[mAudioId]->codec, &pkt, frame, &finished);
    av_frame_free(&frame);
    if (ret < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot encode a audio frame\n");
      av_free_packet(&pkt);
      return;
    }
    if (!finished)
      return;
    pkt.pts = (int64_t)(pkt.pts / (av_q2d(context->streams[mAudioId]->time_base) * 1000));
    pkt.stream_index = mAudioId;
    if (av_interleaved_write_frame(context, &pkt) != 0)
      return;
  }
  return;
}

bool Muxer::addOutput(const std::string& uri, enum AVCodecID videoCodec, enum AVCodecID audioCodec)
{
  if (!mVideo && !mAudio) {
    av_log(nullptr, AV_LOG_ERROR, "no video or audio stream available\n");
    return false;
  }
  AVFormatContext* outputContext = avformat_alloc_context();
  if (!outputContext) {
    av_log(nullptr, AV_LOG_ERROR, "cannot allocate output context\n");
    return false;
  }
  const char* fmt = nullptr;
  AVStream* stream = nullptr;
  AVCodecContext* c = nullptr;
  AVCodec* codec = nullptr;
  if (uri.compare(0, 7, "rtsp://") == 0)
    fmt = "rtsp";
  outputContext->oformat = av_guess_format(fmt, uri.c_str(), nullptr);
  if (!outputContext->oformat) {
    av_log(nullptr, AV_LOG_ERROR, "output format not supported\n");
    goto fail;
  }
  av_strlcpy(outputContext->filename, uri.c_str(), sizeof(outputContext->filename));
  if (!(outputContext->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&outputContext->pb, outputContext->filename, AVIO_FLAG_WRITE) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "open output file failed\n");
      goto fail;
    }
  }
  if (mVideo && videoCodec != AV_CODEC_ID_NONE) { // add video stream
    codec = avcodec_find_encoder(videoCodec);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find video encode codec\n");
      goto fail;
    }
    stream = avformat_new_stream(outputContext, codec);
    if (!stream) {
      av_log(nullptr, AV_LOG_ERROR, "cannot create new video stream\n");
      goto fail;
    }
    c = stream->codec;
    c->codec_id   = videoCodec;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    c->width      = mVideo->videoCodec()->width;
    c->height     = mVideo->videoCodec()->height;
    c->gop_size   = 12; /* emit one intra frame every twelve frames at most */
    c->pix_fmt    = AV_PIX_FMT_YUV420P;
    c->time_base  = AVRational{1, 30};
    if (outputContext->oformat->flags & AVFMT_GLOBALHEADER)
      c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, nullptr, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open video encode codec\n");
      goto fail;
    }
    mVideoId = stream->index;
  }
  if (mAudio && audioCodec != AV_CODEC_ID_NONE) { // add audio stream
    codec = avcodec_find_encoder(audioCodec);
    if (!codec) {
      av_log(nullptr, AV_LOG_ERROR, "cannot find audio encode codec\n");
      goto fail;
    }
    stream = avformat_new_stream(outputContext, codec);
    if (!stream) {
      av_log(nullptr, AV_LOG_ERROR, "cannot create new audio stream\n");
      goto fail;
    }
    c = stream->codec;
    c->sample_fmt     = AV_SAMPLE_FMT_S16;
    c->channels       = mAudio->audioCodec()->channels;
    c->channel_layout = av_get_default_channel_layout(mAudio->audioCodec()->channels);
    c->sample_rate    = mAudio->audioCodec()->sample_rate;
    // c->time_base      = AVRational{1, c->sample_rate};
    if (outputContext->oformat->flags & AVFMT_GLOBALHEADER)
      c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
      c->frame_size = 10000;
    if (avcodec_open2(c, nullptr, nullptr) < 0) {
      av_log(nullptr, AV_LOG_ERROR, "cannot open audio encode codec\n");
      goto fail;
    }
    if (!mAudioFifo) {
      if (!(mAudioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, c->channels, 1))) {
        av_log(nullptr, AV_LOG_ERROR, "cannot allocate audio fifo\n");
        goto fail;
      }
    }
    mAudioId = stream->index;
  }
  if (avformat_write_header(outputContext, nullptr) < 0)
    goto fail;
  av_dump_format(outputContext, 0, outputContext->filename, 1);
  mOutputs[uri] = outputContext; // FIXME: thread-safe.
  return true;
fail:
  if (!(outputContext->oformat->flags & AVFMT_NOFILE))
    avio_close(outputContext->pb);
  avformat_free_context(outputContext);
  outputContext = nullptr;
  return false;
}
