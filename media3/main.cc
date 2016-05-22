#include "capture.h"
extern "C" {
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
    if (argc < 2)
        return -1;

    av_register_all();
    avdevice_register_all();
    avformat_network_init();
    av_log_set_level(AV_LOG_DEBUG);
    list_devices();

    auto cap = Capture::New("avfoundation", "0:0", argv[1]);
    return 0;
}
