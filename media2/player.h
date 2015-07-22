#include <memory>
#include <SDL2/SDL.h>
#include <string>

namespace render {

class Player {
private:
  SDL_Window*   mWindow;
  SDL_Renderer* mRenderer;
  SDL_Texture*  mTexture;
  int           mWidth;
  int           mHeight;
protected:
  Player(int width = 0, int height = 0) : mWindow(nullptr), mRenderer(nullptr), mTexture(nullptr), mWidth(width), mHeight(height) { }
public:
  static std::unique_ptr<Player> New(int, int, const std::string& title = "WebRTC Client");
  static void Delete(Player*&);
  virtual ~Player();
  void draw(const unsigned char* image, int size, int width, int height);
private:
  bool keepRunning();
};

} // namespace rtc
