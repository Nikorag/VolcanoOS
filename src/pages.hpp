// Native Zonto pages (Achievements, Social, Profile) drawn with SDL and fed
// entirely by the platform's JSON APIs — no web content anywhere.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "api.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "util.hpp"

namespace vos {

enum class PageTab { Achievements = 0, Social = 1, Profile = 2 };

class Pages {
 public:
  struct Hooks {
    std::function<void(const std::string&, Color)> toast;
    std::function<void(const std::string& title, const std::string& initial, size_t maxLen,
                       std::function<void(const std::string&)> done)> text;
    std::function<void(const std::string& title, const std::string& msg, const std::string& okLabel,
                       std::function<void()> onOk)> confirm;
    std::function<void()> unauthorized;
    std::function<void()> tick;
  };

  Pages(Gfx& g, Api& api, Pool& pool, MainQueue& main, Hooks hooks)
      : g_(g), api_(api), pool_(pool), main_(main), h_(std::move(hooks)) {}

  static constexpr int kTabs = 3;
  static const char* label(int i);

  void open(PageTab t);
  PageTab tab() const { return tab_; }
  void reset();       // sign-out: forget everything
  void loadDemo();    // fixtures (offline demo / screenshots)
  // Returns true when the page wants to close (Back).
  bool handle(Action a);
  void draw(double now, float dt, PadStyle style);

 private:
  // ---- data -----------------------------------------------------------------
  struct Ach { std::string name, desc, unlockedAt; double rarity = 0; };
  struct AchGroup { std::string name; int unlocked = 0, total = 0; std::vector<Ach> items; };
  struct Friend { int64_t id = 0; std::string gamertag, status; bool online = false; };
  struct Req { int64_t id = 0; std::string gamertag; };
  struct Act { std::string gamertag, detail, createdAt; };
  struct Msg { bool mine = false; std::string body; };
  struct Device { int64_t id = 0; std::string name, platform, lastSeen; std::vector<std::string> installs; };
  struct SocialRow { enum Kind { Header, Add, Request, Friend } kind; int idx; };
  struct ProfileRow { enum Kind { Header, Bio, Setting, Device } kind; int idx; };

  void fetch(const std::string& path, std::function<void(const std::string&)> ok, bool quiet = false);
  void loadAchievements();
  void loadSocial();
  void loadProfile();
  void loadChat(int64_t friendId);
  void rebuildSocialRows();
  void rebuildProfileRows();

  bool handleAch(Action a);
  bool handleSocial(Action a);
  bool handleProfile(Action a);
  void drawAch(double now, PadStyle st);
  void drawSocial(double now, PadStyle st);
  void drawProfile(double now, PadStyle st);
  void drawEmpty(const std::string& title, const std::string& sub, bool loading, double now);
  void hints(PadStyle st, const std::vector<std::pair<Action, std::string>>& items);

  Gfx& g_;
  Api& api_;
  Pool& pool_;
  MainQueue& main_;
  Hooks h_;
  PageTab tab_ = PageTab::Achievements;
  bool demo_ = false;

  // Achievements
  bool achLoaded_ = false, achLoading_ = false;
  int achUnlocked_ = 0, achTotal_ = 0, achPerfect_ = 0;
  std::vector<AchGroup> groups_;
  int gSel_ = 0, aSel_ = 0;
  bool inItems_ = false;

  // Social
  bool socLoaded_ = false, socLoading_ = false;
  std::vector<Friend> friends_;
  std::vector<Req> reqs_;
  std::vector<Act> acts_;
  std::vector<SocialRow> srows_;
  int sSel_ = 1;
  int64_t chatFor_ = 0;
  std::vector<Msg> chat_;
  double chatFetched_ = 0;

  // Profile
  bool proLoaded_ = false, proLoading_ = false;
  std::string gamertag_, bio_, memberSince_;
  int level_ = 0, xp_ = 0, titles_ = 0, achCount_ = 0, friendCount_ = 0;
  double hours_ = 0;
  std::vector<std::pair<std::string, int>> playtime_;
  std::vector<Device> devices_;
  bool settings_[5] = {true, true, true, true, true};
  std::vector<ProfileRow> prows_;
  int pSel_ = 1;

  double lastLoad_[3] = {0, 0, 0};
};

}  // namespace vos
