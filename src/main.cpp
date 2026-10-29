// Volcano OS: the controller-first "Big Picture" software for the Zonto Volcano console.
#include <cstdio>
#include <cstring>
#include <string>

#include "app.hpp"

#ifdef _WIN32
#include <SDL_main.h>
#endif

using namespace vos;

static void usage() {
  std::puts(
      "Zonto Volcano - Volcano OS " VOS_VERSION " (Zonto Games launcher)\n\n"
      "  --windowed            start in a window instead of fullscreen\n"
      "  --fullscreen          force fullscreen\n"
      "  --size WxH            window size when windowed (default 1600x900)\n"
      "  --demo                offline fixture library (no network, fake installs)\n"
      "  --screen NAME         with --demo: signin | system\n"
      "  --press a,b,x,right   with --demo: scripted inputs (up down left right a b x y lb rb start)\n"
      "  --fake-game PATH      with --demo: really run this executable for native launches (overlay testing)\n"
      "  --shot FILE.png       render a few frames, save a screenshot, exit\n"
      "  --shot-frame N        frame number to capture (default 150)\n\n"
      "Environment: ZONTO_API (server), ZONTO_PLATFORM / ZONTO_ARCH (build target),\n"
      "             VOS_DATA_DIR (data directory)");
}

static bool parseAction(const std::string& s, Action& a) {
  static const struct { const char* n; Action a; } k[] = {
      {"up", Action::Up}, {"down", Action::Down}, {"left", Action::Left}, {"right", Action::Right},
      {"a", Action::Accept}, {"b", Action::Back}, {"x", Action::X}, {"y", Action::Y},
      {"lb", Action::LB}, {"rb", Action::RB}, {"start", Action::Start}, {"home", Action::Home}};
  for (auto& e : k)
    if (s == e.n) {
      a = e.a;
      return true;
    }
  return false;
}

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--help" || a == "-h") { usage(); return 0; }
    else if (a == "--windowed") o.windowed = true;
    else if (a == "--fullscreen") o.fullscreen = true;
    else if (a == "--demo") o.demo = true;
    else if (a == "--screen") o.screen = next();
    else if (a == "--fake-game") o.fakeGame = next();
    else if (a == "--shot") o.shotPath = next();
    else if (a == "--shot-frame") o.shotFrame = std::atoi(next().c_str());
    else if (a == "--size") {
      std::string v = next();
      std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height);
    } else if (a == "--press") {
      std::string v = next();
      size_t p = 0;
      while (p <= v.size()) {
        size_t c = v.find(',', p);
        std::string tok = v.substr(p, c == std::string::npos ? std::string::npos : c - p);
        Action act;
        if (parseAction(tok, act)) o.script.push_back(act);
        if (c == std::string::npos) break;
        p = c + 1;
      }
    } else {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      usage();
      return 2;
    }
  }
  App app(std::move(o));
  return app.run();
}
