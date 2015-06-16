#include "capture.h"
#include "muxer.h"
#include <chrono>
#include <unistd.h>

#include <boost/thread.hpp>

static void list_devices() {
#ifdef __APPLE__
  AVFormatContext* ictx = avformat_alloc_context();
  AVInputFormat* ifmt = av_find_input_format("avfoundation");
  AVDictionary* options = nullptr;
  av_dict_set(&options, "list_devices", "true", 0);
  avformat_open_input(&ictx, nullptr, ifmt, &options);
#endif
}

class MyMuxer {
public:
  static std::unique_ptr<MyMuxer> New(const std::string& uri);
  ~MyMuxer();
  void run();
private:
  MyMuxer() : mMuxer (new Muxer()), mRunning (false) {}
  bool init(const std::string& uri);
  std::unique_ptr<Muxer> mMuxer;
  boost::thread mVideoThread;
  boost::thread mAudioThread;
  bool mRunning;
  void audioLoop();
  void videoLoop();
};

MyMuxer::~MyMuxer()
{
  mRunning = false;
  mVideoThread.join();
  mAudioThread.join();
}

void MyMuxer::run()
{
  mRunning = true;
  mVideoThread = boost::thread(&MyMuxer::videoLoop, this);
  mAudioThread = boost::thread(&MyMuxer::audioLoop, this);
}

void MyMuxer::videoLoop()
{
  while (mRunning) {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    mMuxer->writeVideo();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    int interval = 30000 - std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (interval > 0)
      usleep(interval);
    continue;
  }
}

void MyMuxer::audioLoop()
{
  while (mRunning) {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    mMuxer->writeAudio();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    int interval = 10000 - std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    if (interval > 0)
      usleep(interval);
    continue;
  }
}

bool MyMuxer::init(const std::string& uri)
{
  if (!mMuxer)
    return false;
  if (!mMuxer->addOutput(uri, AV_CODEC_ID_H264, AV_CODEC_ID_AAC)) {
    av_log(nullptr, AV_LOG_ERROR, "cannot set output\n");
    return false;
  }
  return true;
}

std::unique_ptr<MyMuxer> MyMuxer::New(const std::string& uri)
{
  auto m = std::unique_ptr<MyMuxer>(new MyMuxer());
  if (!m->init(uri))
    m.reset();
  return m;
}

int main(int argc, char const *argv[])
{
  if (argc < 2)
    return 1;
  av_register_all();
  avformat_network_init();
  avdevice_register_all();
  list_devices();

  auto muxer = MyMuxer::New(argv[1]);
  if (!muxer) {
    av_log(nullptr, AV_LOG_ERROR, "my muxer initialization failed.\n");
    return -1;
  }
  muxer->run();
  sleep(50);
  return 0;
}
