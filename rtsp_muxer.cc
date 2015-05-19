// c++ aac_encoder.cc -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavresample -lavcodec -lavformat -lavutil -lvpx -lopus -lz -lbz2 -lfdk-aac -lboost_system -lboost_thread-mt -o aac-transcoder

// c++ aac_encoder.cc -L../libav-11.3/libavcodec -L../libav-11.3/libavformat -L../libav-11.3/libavutil -L../libav-11.3/libavresample -lavformat -lavcodec -lavutil -lavresample -lz -lbz2 -lboost_system -lboost_thread -pthread -L../../build/lib -lfdk-aac -o aac-transcoder

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
static int select_sample_rate(AVCodec *codec)
{
    const int *p;
    int best_samplerate = 0;

    if (!codec->supported_samplerates)
        return 44100;

    p = codec->supported_samplerates;
    while (*p) {
        best_samplerate = FFMAX(*p, best_samplerate);
        p++;
    }
    return best_samplerate;
}

/* select layout with the highest channel count */
static int select_channel_layout(AVCodec *codec)
{
    const uint64_t *p;
    uint64_t best_ch_layout = 0;
    int best_nb_channels   = 0;

    if (!codec->channel_layouts)
        return AV_CH_LAYOUT_STEREO;

    p = codec->channel_layouts;
    while (*p) {
        int nb_channels = av_get_channel_layout_nb_channels(*p);

        if (nb_channels > best_nb_channels) {
            best_ch_layout    = *p;
            best_nb_channels = nb_channels;
        }
        p++;
    }
    return best_ch_layout;
}

class Muxer {
public:
    Muxer(const std::string&);
    virtual ~Muxer();
    void start();
    void stop();
private:
    AVFormatContext* mContext;
    AVStream* mAudioStream;
    bool mMuxing;
    boost::thread mThread;

    bool addAudioStream(enum AVCodecID);
    void loop();
};

Muxer::Muxer(const std::string& uri)
{
    mContext = avformat_alloc_context();
    if (!mContext) {
        fprintf(stderr, "Could not allocate output format context\n");
        return;
    }

    mContext->oformat = av_guess_format("rtsp", uri.c_str(), NULL);
    if (!mContext->oformat) {
        fprintf(stderr, "Could not find output file format\n");
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
    // avio_close(mContext->pb);
    avformat_free_context(mContext);
}

void Muxer::start() {
    if (addAudioStream(AV_CODEC_ID_AAC)) {
        // if (avio_open(&mContext->pb, mContext->filename, AVIO_FLAG_WRITE) < 0) {
        //     fprintf(stderr, "Could not open output file\n");
        //     return;
        // }
        avformat_write_header(mContext, NULL);
        av_dump_format(mContext, 0, mContext->filename, 1);
        mMuxing = true;
        mThread = boost::thread(&Muxer::loop, this);
    }
}

bool Muxer::addAudioStream(enum AVCodecID codecId)
{
    AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        return false;
    }

    if (!(mAudioStream = avformat_new_stream(mContext, codec))) {
        fprintf(stderr, "Could not create new stream\n");
        return false;
    }

    AVCodecContext* c = mAudioStream->codec;

    /* put sample parameters */
    c->bit_rate = 64000;

    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        fprintf(stderr, "encoder does not support %s",
                av_get_sample_fmt_name(c->sample_fmt));
        return false;
    }

    /* select other audio parameters supported by the encoder */
    c->sample_rate    = select_sample_rate(codec);
    c->channel_layout = select_channel_layout(codec);
    c->channels       = av_get_channel_layout_nb_channels(c->channel_layout);

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        return false;
    }
    // if (mContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
    return true;
}

void Muxer::loop()
{
    AVPacket pkt;
    int got_output;
    int buffer_size;
    float t, tincr;

    /* frame containing input raw audio */
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "could not allocate audio frame\n");
        return;
    }

    AVCodecContext* c = mAudioStream->codec;
    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    frame->channel_layout = c->channel_layout;

    /* the codec gives us the frame size, in samples,
     * we calculate the size of the samples buffer in bytes */
    buffer_size = av_samples_get_buffer_size(NULL, c->channels, c->frame_size, c->sample_fmt, 0);
    uint8_t* samples = reinterpret_cast<uint8_t*>(av_malloc(buffer_size));
    if (!samples) {
        fprintf(stderr, "could not allocate %d bytes for samples buffer\n", buffer_size);
        return;
    }
    /* setup the data pointers in the AVFrame */
    if (avcodec_fill_audio_frame(frame, c->channels, c->sample_fmt, samples, buffer_size, 0) < 0) {
        fprintf(stderr, "could not setup audio frame\n");
        return;
    }

    /* encode a single tone sound */
    t = 0;
    tincr = 2 * M_PI * 440.0 / c->sample_rate;
    int pts = 0;
    while (mMuxing) {
        av_init_packet(&pkt);
        pkt.data = NULL; // packet data will be allocated by the encoder
        pkt.size = 0;

        for (int j = 0; j < c->frame_size; j++) {
            samples[2*j] = (int)(sin(t) * 10000);

            for (int k = 1; k < c->channels; k++)
                samples[2*j + k] = samples[2*j];
            t += tincr;
        }
        /* encode the samples */
        if (avcodec_encode_audio2(c, &pkt, frame, &got_output) < 0) {
            fprintf(stderr, "error encoding audio frame\n");
            return;
        }
        if (got_output) {
            pts += 400;
            pkt.pts = pts;
            av_write_frame(mContext, &pkt);
            av_free_packet(&pkt);
        }
    }

    av_freep(&samples);
    av_frame_free(&frame);
}

int main(int argc, char **argv)
{
    av_register_all();
    avformat_network_init();
    av_log_set_level(AV_LOG_DEBUG);

    Muxer* m = new Muxer("rtsp://localhost:1935/live/aac.sdp");
    m->start();
    sleep(100);
    delete m;
    return 0;
}
