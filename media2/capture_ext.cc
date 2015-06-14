#include "capture.h"

#ifdef CAPTURE_EXTENDED
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
    if (!mAudioFifo) {
      if (!(mAudioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, c->channels, 1))) {
        av_log(nullptr, AV_LOG_ERROR, "cannot allocate audio fifo\n");
        goto fail;
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

bool Capture::writeVideo(int& pts)
{
  AVFrame* frame = nullptr;
  if (!decodeVideo(&frame)) {
    return false;
  }
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

bool Capture::writeAudio(int& pts)
{
  while (av_audio_fifo_size(mAudioFifo) < mOutputContext->streams[mAudioSrcId]->codec->frame_size) {
    if (!decodeAudio(mAudioFifo))
      return false;
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
#endif
