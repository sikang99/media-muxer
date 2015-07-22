#include "player.h"

namespace render {

std::unique_ptr<Player> Player::New(int width, int height, const std::string& title) {
  auto player = std::unique_ptr<Player>(new Player(width, height));
  SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
    goto fail;
  }

  player->mWindow = SDL_CreateWindow(title.c_str(),
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    width, height,
                                    SDL_WINDOW_BORDERLESS);
  if (!player->mWindow) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set create window: %s\n", SDL_GetError());
    goto fail;
  }

  player->mRenderer = SDL_CreateRenderer(player->mWindow, -1, 0);
  if (!player->mRenderer) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set create renderer: %s\n", SDL_GetError());
    goto fail;
  }

  player->mTexture = SDL_CreateTexture(player->mRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
  if (!player->mTexture) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set create texture: %s\n", SDL_GetError());
    goto fail;
  }
  SDL_SetWindowSize(player->mWindow, width, height);

  return player;

fail:
  player.reset();
  return player;
}

Player::~Player()
{
  if (this->mTexture) {
    SDL_DestroyTexture(this->mTexture);
    this->mTexture = nullptr;
  }
  if (this->mRenderer) {
    SDL_DestroyRenderer(this->mRenderer);
    this->mRenderer = nullptr;
  }
  if (this->mWindow) {
    SDL_DestroyWindow(this->mWindow);
    this->mWindow = nullptr;
  }
  SDL_Quit();
}

void Player::Delete(Player*& player)
{
  if (!player) return;
  delete player;
  player = nullptr;
}

void Player::draw(const unsigned char* image, int size, int width, int height)
{
  if ((this->mWidth != width) || (this->mHeight != height)) {
    if (this->mTexture)
      SDL_DestroyTexture(this->mTexture);

    this->mTexture = SDL_CreateTexture(this->mRenderer,
                                        SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        width, height);
    SDL_SetWindowSize(this->mWindow, width, height);
    this->mWidth = width;
    this->mHeight = height;
  }

  void* pixels = nullptr;
  int pitch = 0;
  if (SDL_LockTexture(this->mTexture, nullptr, &pixels, &pitch) < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't lock texture: %s\n", SDL_GetError());
    return;
  }

  memcpy(pixels, image, size);
  SDL_UnlockTexture(this->mTexture);

  SDL_RenderClear(this->mRenderer);
  SDL_RenderCopy(this->mRenderer, this->mTexture, nullptr, nullptr);
  SDL_RenderPresent(this->mRenderer);
}

bool Player::keepRunning()
{
  bool result = true;
  if (this->mWindow) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_KEYDOWN:
          if (event.key.keysym.sym == SDLK_ESCAPE) {
            result = false;
          }
        break;
        case SDL_QUIT:
          result = false;
        break;
      }
    }
  }
  return result;
}

} // namespace render
