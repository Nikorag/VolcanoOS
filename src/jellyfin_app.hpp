// The Jellyfin app: browse a Jellyfin server natively (drawn with SDL, controller first) and play
// through mpv. See apps.hpp for the framework and docs/APPS.md for how the pieces fit.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "apps.hpp"
#include "jellyfin.hpp"
#include "mpv_ipc.hpp"

namespace vos {

class JellyfinApp : public AppModule {
 public:
  explicit JellyfinApp(AppContext& c);

  Info info() const override;
  void open() override;
  bool handle(Action a) override;
  void draw(double now, float dt, PadStyle style) override;
  void tick(double now) override;
  void loadDemo() override;
  void demoScreen(const std::string& variant) override;
  std::string status() const override;

  bool sessionAction(Action a) override;
  void sessionTick(double now) override;
  void sessionEnded(int exitCode) override;
  std::string sessionStatus() const override;

 private:
  enum class Mode { Setup, Home, Grid, Detail };
  enum class Step { Servers, Probing, QuickConnect, Password, Signing };
  struct Row {
    std::string title;
    std::vector<JfItem> items;
    bool landscape = false;
    bool loaded = false;
    int col = 0;
    float scroll = 0;
  };
  struct Frame {  // what Back returns to
    Mode mode;
    JfItem item;
    std::string gridTitle, gridParent, gridSearch;
    std::vector<JfItem> grid;
    int gridTotal = 0, gSel = 0;
    int zone = 0, season = 0, ep = 0;
    std::vector<JfItem> seasons, episodes;
  };

  // ---- persistence / session ------------------------------------------------------------------------
  void loadState();
  void saveState();
  void signOut();
  bool failed(int status, const std::string& error);  // 401 -> back to setup; otherwise toast. true if handled.

  // ---- setup ------------------------------------------------------------------------------------------
  void beginSetup();
  void discoverServers();
  void chooseServer(const std::string& address);
  void startQuickConnect();
  void pollQuickConnect();
  void signInPassword();
  void finishSignIn(const JfSession& s);
  bool handleSetup(Action a);
  void drawSetup(double now, PadStyle st);
  std::vector<std::string> setupChoices() const;

  // ---- home -------------------------------------------------------------------------------------------------
  void loadHome(bool force);
  void rowResult(int idx, std::vector<JfItem> items);
  bool handleHome(Action a);
  void drawHome(double now, float dt, PadStyle st);
  const JfItem* focusedItem() const;

  // ---- grid / detail --------------------------------------------------------------------------------------
  void pushFrame();
  void popFrame();
  void openItem(const JfItem& it);
  void openGrid(const std::string& title, const std::string& parentId, const std::string& search = "");
  void loadGridPage();
  void openDetail(const JfItem& it);
  void loadSeasons();
  void loadEpisodes();
  bool handleGrid(Action a);
  bool handleDetail(Action a);
  void drawGrid(double now, float dt, PadStyle st);
  void drawDetail(double now, float dt, PadStyle st);
  void search();

  // ---- playback ------------------------------------------------------------------------------------------------
  void play(const JfItem& it, bool resume);
  static std::string findMpv();

  // ---- drawing helpers ----------------------------------------------------------------------------------------------
  void drawTile(const JfItem& it, float x, float y, float w, float h, bool landscape, bool focused, double now);
  void drawBackdrop(const JfItem* it, double now);
  void drawTop(double now, const std::string& crumb);
  std::string imgFor(const JfItem& it, bool landscape, int h) const;
  std::string backdropFor(const JfItem& it) const;
  static std::string metaLine(const JfItem& it);
  static std::string subLine(const JfItem& it);
  static std::string resumeLabel(const JfItem& it);

  // ---- state -----------------------------------------------------------------------------------------------------------
  Jellyfin jf_;
  Mode mode_ = Mode::Setup;
  bool demo_ = false;
  int gen_ = 0;  // bumped on sign-out: late results from the old session are dropped
  std::string deviceId_;
  double openedAt_ = 0;

  // setup
  Step step_ = Step::Servers;
  std::vector<JfServer> found_;
  bool discovering_ = false;
  int setupSel_ = 0;
  std::string server_, serverName_, setupError_, user_, pass_;
  JfQuickConnect qc_;
  double qcPolledAt_ = 0, qcStarted_ = 0;
  bool qcBusy_ = false;

  // home
  std::vector<Row> rows_;
  int row_ = 0;       // -1 = header actions
  int headSel_ = 0;
  float rowScroll_ = 0;
  double homeLoadedAt_ = 0;
  bool homeLoading_ = false;
  int pending_ = 0;

  // grid
  std::string gridTitle_, gridParent_, gridSearch_;
  std::vector<JfItem> grid_;
  int gridTotal_ = 0, gSel_ = 0;
  bool gridLoading_ = false;
  float gridScroll_ = 0;

  // detail
  JfItem det_;
  std::vector<JfItem> seasons_, episodes_, demoSeasons_, demoEpisodes_;
  int zone_ = 0, season_ = 0, ep_ = 0, btn_ = 0;
  bool detLoading_ = false;
  float epScroll_ = 0;

  std::vector<Frame> stack_;

  // playback
  JfItem playing_;
  std::string playSession_, sockPath_;
  MpvIpc ipc_;
  double playStarted_ = 0, lastReport_ = 0, lastConnectTry_ = 0;
  bool reportedStart_ = false;
  int64_t lastTicks_ = 0;
  bool wasPlaying_ = false;
};

}  // namespace vos
