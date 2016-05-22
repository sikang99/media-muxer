#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/time.h>
}

static void list_devices()
{
#ifdef __APPLE__
    AVDictionary* options = nullptr;
    av_dict_set(&options, "list_devices", "true", 0);
    AVFormatContext* ictx = nullptr;
    avformat_open_input(&ictx, nullptr, av_find_input_format("avfoundation"), &options);
    avformat_close_input(&ictx);
#endif
}

int main(int argc, char const* argv[])
{
    av_register_all();
    avformat_network_init();
    avdevice_register_all();
    av_log_set_level(AV_LOG_DEBUG);

    list_devices();

    AVFormatContext* ictx = nullptr;
    AVFormatContext* octx = nullptr;
    AVRational timeBase{ 0, AV_TIME_BASE };
    std::string url{ "rtmp://192.168.1.104:1935/live/abc" };
    int ret = 0;
    if ((ret = avformat_open_input(&ictx, "0:0", av_find_input_format("avfoundation"), nullptr)) < 0) {
        av_log(ictx, AV_LOG_ERROR, "avformat_open_input failed\n");
        return ret;
    }
    if ((ret = avformat_find_stream_info(ictx, 0)) < 0) {
        av_log(ictx, AV_LOG_ERROR, "avformat_find_stream_info failed\n");
        goto exit;
    }
    av_dump_format(ictx, 0, "0:0", 0);
    avformat_alloc_output_context2(&octx, nullptr, "flv", url.c_str());
    if (!octx) {
        av_log(octx, AV_LOG_ERROR, "avformat_alloc_output_context2 failed\n");
        goto exit;
    }

    for (int i = 0; i < ictx->nb_streams; ++i) {
        auto codecType = ictx->streams[i]->codec->codec_type;
        switch (codecType) {
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_AUDIO:
            break;
        default:
            continue;
        }
        auto istream = ictx->streams[i];
        auto ostream = avformat_new_stream(octx, istream->codec->codec);
        if (!ostream) {
            av_log(octx, AV_LOG_ERROR, "avformat_new_stream failed for %s\n", codecType == AVMEDIA_TYPE_AUDIO ? "AUDIO" : "VIDEO");
            goto exit;
        }
        if ((ret = avcodec_copy_context(ostream->codec, istream->codec)) < 0) {
            av_log(octx, AV_LOG_ERROR, "avcodec_copy_context failed for %s\n", codecType == AVMEDIA_TYPE_AUDIO ? "AUDIO" : "VIDEO");
            goto exit;
        }
        ostream->codec->codec_tag = 0;
        ostream->codec->time_base = AVRational{ 0, 0 };
        if (octx->oformat->flags & AVFMT_GLOBALHEADER)
            ostream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

        av_log(octx, AV_LOG_INFO, "found %s stream\n", codecType == AVMEDIA_TYPE_AUDIO ? "AUDIO" : "VIDEO");
    }

    if (!(octx->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&octx->pb, url.c_str(), AVIO_FLAG_WRITE)) < 0) {
            av_log(octx, AV_LOG_ERROR, "avio_open failed\n");
            goto exit;
        }
    }

    if ((ret = avformat_write_header(octx, nullptr)) < 0) {
        av_log(octx, AV_LOG_ERROR, "avformat_write_header failed\n");
        goto exit;
    }
    av_dump_format(octx, 0, url.c_str(), 1);

exit:
    avformat_close_input(&ictx);
    if (octx && !(octx->oformat->flags & AVFMT_NOFILE))
        avio_close(octx->pb);
    if (octx)
        avformat_free_context(octx);
    return 0;
}
