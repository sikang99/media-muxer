// c++ muxer.cc -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavresample -lavcodec -lavformat -lavutil -lvpx -lopus -lz -lbz2 -lfdk-aac -lboost_system -lboost_thread-mt -o muxer

// c++ muxer.cc -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavformat -lavcodec -lavutil -lavresample -lz -lbz2 -lboost_system -lboost_thread -pthread -L../../build/lib -lfdk-aac -o muxer

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
}

#include <boost/thread.hpp>
#include <string>


/* check that a given sample format is supported by the encoder */
static int check_sample_fmt(AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* just pick the highest supported samplerate */
// static int select_sample_rate(AVCodec *codec)
// {
//     const int *p;
//     int best_samplerate = 0;

//     if (!codec->supported_samplerates)
//         return 44100;

//     p = codec->supported_samplerates;
//     while (*p) {
//         best_samplerate = FFMAX(*p, best_samplerate);
//         p++;
//     }
//     return best_samplerate;
// }

/* select layout with the highest channel count */
// static int select_channel_layout(AVCodec *codec)
// {
//     const uint64_t *p;
//     uint64_t best_ch_layout = 0;
//     int best_nb_channels   = 0;

//     if (!codec->channel_layouts)
//         return AV_CH_LAYOUT_STEREO;

//     p = codec->channel_layouts;
//     while (*p) {
//         int nb_channels = av_get_channel_layout_nb_channels(*p);

//         if (nb_channels > best_nb_channels) {
//             best_ch_layout    = *p;
//             best_nb_channels = nb_channels;
//         }
//         p++;
//     }
//     return best_ch_layout;
// }

static void fill_yuv_image(AVFrame *pict, int frame_index, int width, int height)
{
    int x, y, i, ret;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(pict);
    if (ret < 0)
        exit(1);

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static AVFrame* alloc_video_frame(AVCodecContext* c)
{
    AVFrame *picture;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = c->pix_fmt;
    picture->width  = c->width;
    picture->height = c->height;

    /* allocate the buffers for the frame data */
    if (av_frame_get_buffer(picture, 32) < 0) {
        av_frame_free(&picture);
        return NULL;
    }

    return picture;
}

static AVFrame* alloc_audio_frame(AVCodecContext* c, uint8_t** buffer)
{
    int buffer_size;

    /* frame containing input raw audio */
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "allocate audio frame failed\n");
        return NULL;
    }

    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    frame->channel_layout = c->channel_layout;

    /* the codec gives us the frame size, in samples,
     * we calculate the size of the samples buffer in bytes */
    buffer_size = av_samples_get_buffer_size(NULL, c->channels, c->frame_size, c->sample_fmt, 0);
    uint8_t* samples = reinterpret_cast<uint8_t*>(av_malloc(buffer_size));
    if (!samples) {
        av_log(NULL, AV_LOG_ERROR, "allocate %d bytes for samples buffer failed\n", buffer_size);
        return NULL;
    }
    /* setup the data pointers in the AVFrame */
    if (avcodec_fill_audio_frame(frame, c->channels, c->sample_fmt, samples, buffer_size, 0) < 0) {
        av_log(NULL, AV_LOG_ERROR, "setup audio frame failed\n");
        return NULL;
    }
    *buffer = samples;
    return frame;
}

class Muxer {
public:
    Muxer(const char*, const std::string&);
    virtual ~Muxer();
    void start();
    void stop();
private:
    AVFormatContext* mContext;
    AVStream* mAudioStream;
    AVStream* mVideoStream;
    bool mMuxing;
    bool mHasVideo;
    bool mHasAudio;

    bool addAudioStream(enum AVCodecID);
    bool addVideoStream(enum AVCodecID);
    int writeAudioFrame(AVFrame*, int, uint8_t*);
    int writeVideoFrame(AVFrame*, int);
    void loop();
    boost::thread mThread;
};

Muxer::Muxer(const char* fmt, const std::string& uri)
    : mContext (NULL)
    , mAudioStream (NULL)
    , mVideoStream (NULL)
    , mMuxing (false)
    , mHasVideo (false)
    , mHasAudio (false)
{
    mContext = avformat_alloc_context();
    if (!mContext) {
        av_log(NULL, AV_LOG_ERROR, "allocate output format context failed\n");
        return;
    }

    mContext->oformat = av_guess_format(fmt, uri.c_str(), NULL);
    if (!mContext->oformat) {
        av_log(NULL, AV_LOG_ERROR, "output format not supported\n");
        return;
    }

    av_strlcpy(mContext->filename, uri.c_str(), sizeof(mContext->filename));
}

Muxer::~Muxer()
{
    if (mMuxing)
        stop();
}

void Muxer::stop()
{
    mMuxing = false;
    mThread.join();

    av_write_trailer(mContext);
    avcodec_close(mAudioStream->codec);
    if (!(mContext->oformat->flags & AVFMT_NOFILE))
        avio_close(mContext->pb);
    avformat_free_context(mContext);
}

void Muxer::start() {
    if (addVideoStream(AV_CODEC_ID_H264))
        mHasVideo = true;
    if (addAudioStream(AV_CODEC_ID_AAC))
        mHasAudio = true;
    if (mHasAudio || mHasVideo) {
        if (!(mContext->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&mContext->pb, mContext->filename, AVIO_FLAG_WRITE) < 0) {
                av_log(NULL, AV_LOG_ERROR, "open output file failed\n");
                return;
            }
        }
        avformat_write_header(mContext, NULL);
        av_dump_format(mContext, 0, mContext->filename, 1);
        mMuxing = true;
        mThread = boost::thread(&Muxer::loop, this);
        av_log(NULL, AV_LOG_INFO, "muxer started - audio: %s, video: %s\n", mHasAudio?"true":"false", mHasVideo?"true":"false");
    } else {
        av_log(NULL, AV_LOG_ERROR, "no stream to publish\n");
    }
}

bool Muxer::addVideoStream(enum AVCodecID codecId)
{
    AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "video codec not found\n");
        return false;
    }
    mContext->oformat->video_codec = codecId;
    mVideoStream = avformat_new_stream(mContext, codec);
    if (!mVideoStream) {
        av_log(NULL, AV_LOG_ERROR, "create new video stream failed\n");
        return false;
    }

    AVCodecContext* c = mVideoStream->codec;
    c->codec_id = codecId;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    /* Put sample parameters. */
    c->bit_rate = 400000;
    /* Resolution must be a multiple of two. */
    c->width    = 640;
    c->height   = 480;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    mVideoStream->time_base = (AVRational){ 1, 30 };
    c->time_base             = mVideoStream->time_base;

    c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
    c->pix_fmt       = AV_PIX_FMT_YUV420P;
    /* Some formats want stream headers to be separate. */
    if (mContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(c, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "open video codec failed\n");
        return false;
    }
    return true;
}

bool Muxer::addAudioStream(enum AVCodecID codecId)
{
    AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "audio codec not found\n");
        return false;
    }

    if (!(mAudioStream = avformat_new_stream(mContext, codec))) {
        av_log(NULL, AV_LOG_ERROR, "create new audio stream failed\n");
        return false;
    }

    AVCodecContext* c = mAudioStream->codec;

    /* put sample parameters */
    c->bit_rate = 48000;

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        av_log(NULL, AV_LOG_ERROR, "encoder does not support %s\n", av_get_sample_fmt_name(c->sample_fmt));
        return false;
    }

    /* select other audio parameters supported by the encoder */
    c->channels       = 1;
    c->channel_layout = av_get_default_channel_layout(c->channels);
    c->sample_rate    = 44100;
    mAudioStream->time_base = (AVRational){ 1, c->sample_rate };

    if (mContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(c, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "open audio codec failed\n");
        return false;
    }
    return true;
}

int Muxer::writeVideoFrame(AVFrame* frame, int pts)
{
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    fill_yuv_image(frame, pts, mVideoStream->codec->width, mVideoStream->codec->height);
    frame->pts = pts;
    int got_packet = 0;
    if (avcodec_encode_video2(mVideoStream->codec, &pkt, frame, &got_packet) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error encoding a video frame\n");
        av_free_packet(&pkt);
        return 1;
    }
    if (got_packet) {
        av_packet_rescale_ts(&pkt, mVideoStream->codec->time_base, mVideoStream->time_base);
        pkt.stream_index = mVideoStream->index;
        return av_interleaved_write_frame(mContext, &pkt);
    }
    return 0;
}

int Muxer::writeAudioFrame(AVFrame* frame, int pts, uint8_t* buffer)
{
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    int got_output = 0;
    float t = 0, tincr = 2 * M_PI * 440.0 / mAudioStream->codec->sample_rate;

    for (int j = 0; j < mAudioStream->codec->frame_size; j++) {
        buffer[2*j] = (int)(sin(t) * 10000);

        for (int k = 1; k < mAudioStream->codec->channels; k++)
            buffer[2*j + k] = buffer[2*j];
        t += tincr;
    }
    frame->pts = pts;
    /* encode the samples */
    if (avcodec_encode_audio2(mAudioStream->codec, &pkt, frame, &got_output) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error encoding a audio frame\n");
        av_free_packet(&pkt);
        return 1;
    }
    if (got_output) {
        av_packet_rescale_ts(&pkt, mAudioStream->codec->time_base, mAudioStream->time_base);
        pkt.stream_index = mAudioStream->index;
        return av_interleaved_write_frame(mContext, &pkt);
    }
    return 0;
}

void Muxer::loop()
{
    int apts = 0;
    int vpts = 0;
    AVFrame* vFrame = NULL;
    AVFrame* aFrame = NULL;
    uint8_t* samples = NULL;
    if (mHasVideo) {
        vFrame = alloc_video_frame(mVideoStream->codec);
        if (!vFrame) {
            av_log(NULL, AV_LOG_ERROR, "allocate video frame failed\n");
            return;
        }
    }
    if (mHasAudio) {
        aFrame = alloc_audio_frame(mAudioStream->codec, &samples);
        if (!aFrame) {
            av_log(NULL, AV_LOG_ERROR, "allocate audio frame failed\n");
            return;
        }
    }

    while (mMuxing) {
        if (mHasAudio && mHasVideo) {
            if (av_compare_ts(vpts, mVideoStream->codec->time_base, apts, mAudioStream->codec->time_base) <= 0) {
                writeVideoFrame(vFrame, vpts);
                vpts++;
            } else {
                writeAudioFrame(aFrame, apts, samples);
                apts += aFrame->nb_samples;
            }
        } else if (mHasVideo) {
            writeVideoFrame(vFrame, vpts);
            vpts++;
        } else if (mHasAudio) {
            writeAudioFrame(aFrame, apts, samples);
            apts += aFrame->nb_samples;
        }
    }

    if (samples)
        av_freep(&samples);
    if (aFrame)
        av_frame_free(&aFrame);
    if (vFrame)
        av_frame_free(&vFrame);
}

int main(int argc, char **argv)
{
    av_register_all();
    avformat_network_init();
    // av_log_set_level(AV_LOG_DEBUG);

    auto m = new Muxer("rtsp", "rtsp://localhost:1935/live/bundle.sdp");
    // Muxer* m = new Muxer(NULL, "abc.mkv");
    m->start();
    sleep(20);
    delete m;
    return 0;
}
