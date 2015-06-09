#include "display.h"

void Display::render(AVFrame* frame) {
  mFrame = frame;
  SDL_Event evt;
  evt.type = REFRESH_EVENT;
  SDL_PushEvent(&evt);
}

void Display::start()
{
  mRendering = true;
  mThread = boost::thread(&Display::loop, this);
}

void Display::stop()
{
  mRendering = false;
  mThread.join();
}

Display::Display(const std::string& title, int width, int height)
  : mRendering (false)
  , mWidth (width)
  , mHeight (height)
{
  mScreen = SDL_SetVideoMode(mWidth, mHeight, 0, 0);
  mBmp = SDL_CreateYUVOverlay(mWidth, mHeight, SDL_IYUV_OVERLAY, mScreen);
  SDL_WM_SetCaption(title.c_str(), nullptr);
}

Display::~Display()
{
  stop();
}

void Display::loop()
{
  SDL_Rect rect;
  rect.x = 0;
  rect.y = 0;
  rect.w = mWidth;
  rect.h = mHeight;
  while (mRendering) {
    SDL_WaitEvent(&mEvent);
    if (mEvent.type == REFRESH_EVENT) {
      SDL_LockYUVOverlay(mBmp);
      mBmp->pixels[0] = mFrame->data[0];
      mBmp->pixels[1] = mFrame->data[1];
      mBmp->pixels[2] = mFrame->data[2];
      mBmp->pitches[0] = mFrame->linesize[0];
      mBmp->pitches[1] = mFrame->linesize[1];
      mBmp->pitches[2] = mFrame->linesize[2];
      SDL_UnlockYUVOverlay(mBmp);
      SDL_DisplayYUVOverlay(mBmp, &rect);
    } else if (mEvent.type == SDL_QUIT) {
      mRendering = false;
      break;
    }
  }
}
