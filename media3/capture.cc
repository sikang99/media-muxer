#include "capture.h"

std::unique_ptr<Capture> Capture::New(const std::string& driver, const std::string& device, const std::string& dest)
{
    auto capture = std::unique_ptr<Capture>(new Capture(driver, device));
    if (!capture->init(dest))
        capture.reset();
    return capture;
}

Capture::Capture(const std::string& driver, const std::string& device)
    : mInputContext{ nullptr }
    , mOutputContext{ nullptr }
    , mSwsCtx{ nullptr }
    , mResCtx{ nullptr }
    , mVideoDecoder{ nullptr }
    , mAudioDecoder{ nullptr }
    , mAudioFifo{ nullptr }
    , mReady{ false }

{
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
    av_dump_format(mInputContext, 0, device.c_str(), 0);
}

static const char* getShortName(const std::string& uri)
{
    if (uri.compare(0, 7, "rtsp://") == 0)
        return "rtsp";
    else if (uri.compare(0, 7, "rtmp://") == 0)
        return "flv";
    return nullptr;
}

bool Capture::decodeAudio(const AVPacket* pkt, AVAudioFifo* fifo)
{
    if (!pkt || !fifo) {
        av_log(nullptr, AV_LOG_ERROR, "cannot write to nil fifo!\n");
        return false;
    }

    AVFrame* framePCM = av_frame_alloc();
    int finished = 0;
    if (avcodec_decode_audio4(mAudioDecoder, framePCM, &finished, pkt) < 0) {
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

bool Capture::decodeVideo(const AVPacket* pkt, AVFrame** frame)
{
    if (!pkt || !frame)
        return false;

    AVFrame* frameRGB = av_frame_alloc();
    int finished = 0;
    if (avcodec_decode_video2(mVideoDecoder, frameRGB, &finished, pkt) < 0) {
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

bool Capture::init(const std::string& dest)
{
    if (!mInputContext)
        return false;
    avformat_alloc_output_context2(&mOutputContext, nullptr, getShortName(dest), dest.c_str());
    if (!mOutputContext) {
        av_log(mOutputContext, AV_LOG_ERROR, "avformat_alloc_output_context2 failed\n");
        return false;
    }
    for (size_t i = 0; i < mInputContext->nb_streams; ++i) {
        if (mAudioDecoder && mVideoDecoder)
            break;
        auto istream = mInputContext->streams[i];
        auto codec = avcodec_find_decoder(istream->codec->codec_id);
        if (!codec) {
            av_log(mInputContext, AV_LOG_ERROR, "cannot find decode codec\n");
            return false;
        }
        auto decoder = avcodec_alloc_context3(codec);
        if (avcodec_copy_context(decoder, istream->codec) != 0) {
            av_log(mOutputContext, AV_LOG_ERROR, "cannot copy codec context\n");
            return false;
        }
        if (avcodec_open2(decoder, codec, nullptr) < 0) {
            av_log(mOutputContext, AV_LOG_ERROR, "cannot open decode codec\n");
            return false;
        }
        switch (decoder->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            mVideoDecoder = decoder;
            mSwsCtx = sws_getContext(
                decoder->width,
                decoder->height,
                decoder->pix_fmt,
                decoder->width,
                decoder->height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!codec) {
                av_log(mOutputContext, AV_LOG_ERROR, "cannot find video encode codec\n");
                return false;
            }
            {
                auto ostream = avformat_new_stream(mOutputContext, codec);
                if (!ostream) {
                    av_log(mOutputContext, AV_LOG_ERROR, "cannot create new video stream\n");
                    return false;
                }
                auto c = ostream->codec;
                c->codec_id = AV_CODEC_ID_H264;
                c->codec_type = AVMEDIA_TYPE_VIDEO;
                c->width = mVideoDecoder->width;
                c->height = mVideoDecoder->height;
                c->gop_size = 12; /* emit one intra frame every twelve frames at most */
                c->pix_fmt = AV_PIX_FMT_YUV420P;
                if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
                    c->flags |= CODEC_FLAG_GLOBAL_HEADER;
                if (avcodec_open2(c, nullptr, nullptr) < 0) {
                    av_log(mOutputContext, AV_LOG_ERROR, "cannot open video encode codec\n");
                    return false;
                }
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            mAudioDecoder = decoder;
            mResCtx = swr_alloc_set_opts(
                nullptr,
                av_get_default_channel_layout(mAudioDecoder->channels),
                AV_SAMPLE_FMT_S16,
                mAudioDecoder->sample_rate,
                av_get_default_channel_layout(mAudioDecoder->channels),
                mAudioDecoder->sample_fmt,
                44100, /*mAudioDecoder->sample_rate*/
                0,
                nullptr);
            if (!mResCtx) {
                av_log(mResCtx, AV_LOG_ERROR, "cannot allocate resample context\n");
                return false;
            }
            if (swr_init(mResCtx) < 0) {
                av_log(mResCtx, AV_LOG_ERROR, "cannot open resample context\n");
                swr_free(&mResCtx);
                mResCtx = nullptr;
                return false;
            }
            codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (!codec) {
                av_log(mOutputContext, AV_LOG_ERROR, "cannot find audio encode codec\n");
                return false;
            }
            {
                auto ostream = avformat_new_stream(mOutputContext, codec);
                if (!ostream) {
                    av_log(mOutputContext, AV_LOG_ERROR, "cannot create new audio stream\n");
                    return false;
                }
                auto c = ostream->codec;
                c->sample_fmt = AV_SAMPLE_FMT_S16;
                c->channels = mAudioDecoder->channels;
                c->channel_layout = av_get_default_channel_layout(mAudioDecoder->channels);
                c->sample_rate = mAudioDecoder->sample_rate;
                if (mOutputContext->oformat->flags & AVFMT_GLOBALHEADER)
                    c->flags |= CODEC_FLAG_GLOBAL_HEADER;
                if (c->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)
                    c->frame_size = 10000;
                if (avcodec_open2(c, nullptr, nullptr) < 0) {
                    av_log(mOutputContext, AV_LOG_ERROR, "cannot open audio encode codec\n");
                    return false;
                }
                if (!(mAudioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, c->channels, 1))) {
                    av_log(mOutputContext, AV_LOG_ERROR, "cannot allocate audio fifo\n");
                    return false;
                }
            }
            break;
        default:
            av_log(mInputContext, AV_LOG_WARNING, "unsupported media type: %d\n", decoder->codec_type);
            continue;
        }
    }
    if (!(mOutputContext->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&mOutputContext->pb, mOutputContext->filename, AVIO_FLAG_WRITE) < 0) {
            av_log(mOutputContext, AV_LOG_ERROR, "open output uri failed\n");
            return false;
        }
    }
    if (avformat_write_header(mOutputContext, nullptr) < 0) {
        av_log(mOutputContext, AV_LOG_ERROR, "avformat_write_header failed\n");
        return false;
    }
    av_dump_format(mOutputContext, 0, dest.c_str(), 1);
    mReady = true;
    return true;
}

Capture::~Capture()
{
    if (mInputContext)
        avformat_close_input(&mInputContext);
    if (mAudioFifo)
        av_audio_fifo_free(mAudioFifo);
    if (mOutputContext) {
        if (mReady)
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
