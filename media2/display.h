#ifndef Display_h
#define Display_h

#include <boost/thread.hpp>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <SDL/SDL.h>
}
#define REFRESH_EVENT  (SDL_USEREVENT + 1)

class Display {
public:
  Display(const std::string&, int, int);
  virtual ~Display();
  void render(AVFrame*);
  const bool getStatus() const {return mRendering;}
  void start();
  void stop();
private:
  SDL_Surface* mScreen;
  SDL_Overlay* mBmp;
  SDL_Event mEvent;
  AVFrame* mFrame;
  bool mRendering;
  int mWidth, mHeight;
  boost::thread mThread;
  void loop();
};

#endif //Display_h
