#pragma once
// LiteNX: a light HOME screen with the stock Switch layout, an animated background and frosted glass.
//
// One row of the most recent games and apps (the game's own icon), then "All Software" as a grid. A pill
// dock (Sphaira, Album, Controllers, Settings, Power), the profile at the top left, the clock, Wi-Fi and
// battery at the top right. It is HOME itself (built into HomeNX's menu): it keeps no Discord, Spotify,
// news, control centre or box-art downloads, and it blurs nothing live while it is idle: the glass is a
// picture of the background, blurred once when the animation was made (config/CardsNX/animbg/blur_*.jpg).
#include "Audio.hpp"
#include "Library.hpp"
#include "Settings.hpp"
#include "launcher/AppletLauncher.hpp"

#include <switchu/lock_password.hpp>

#include <nxui/Activity.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Texture.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lite {

using cards::Game;

class LiteActivity final : public nxui::Activity {
public:
    // What the daemon said at start: a game suspended, one running, the console just booted.
    explicit LiteActivity(std::uint64_t suspendedId = 0, bool appRunning = false, bool atBoot = true,
                          bool appletReturn = false, bool hiddenStart = false, bool standby = false);
    bool onCreate() override;
    bool presentInitialFrame(nxui::Renderer& ren) override;
    void onDestroy() override;
    void onUpdate(float dt) override;
    int frameInterval() const override;
    void pollBetweenFrames() override { pumpDaemon(); } // (the sleep notice can't wait for the next frame)
    void onRender(nxui::Renderer&) override {}
    nxui::Widget* focusRoot() override { return nullptr; } // it reads the buttons itself

    struct Pixels {
        std::vector<std::uint8_t> rgba;
        std::shared_ptr<std::uint8_t> raw; // or the decoder's own buffer, not copied
        int w = 0, h = 0;
    };

private:
    enum class Zone { Top, Cards, Bottom };

    // ---- the lists
    struct Lists {
        std::vector<Game> games, homebrew;
        bool titlesOnly = false; // the installed titles only: the .nro apps stay as they are
        int titles = -1;
        std::uint64_t nroHash = 0;
    };
    static Lists readLists(bool titlesOnly, std::uint64_t knownNroHash);
    void startListRefresh(bool titlesOnly);
    void takeLists(Lists lists);
    void applyNames();
    void applyRecent();
    void noteRecent(const std::string& path);
    void rebuildList();
    float gridTerm(int index) const;      // how far a grid tile is on its way into place (0..1)
    float slideOf(std::uint64_t id) const; // a row card on its way to a new place, in cards
    void buzz();                          // a short, light vibration of the controllers (waking to the lock screen)
    void select(int index);
    void selectById(std::uint64_t id);
    const Game* current();
    const Game* gameById(std::uint64_t id) const;
    float scrollTarget() const;
    float riseOrder(int i) const;
    bool cardRect(int index, nxui::Rect& card, float& chosen);

    // ---- game icons: read and decoded on one thread, uploaded a few a frame
    struct IconDone {
        std::uint64_t id = 0;
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };
    void iconWorker();
    void wantIcons();
    void takeIcons();
    void trimIcons();
    const nxui::Texture* iconOf(std::uint64_t id) const;

    // ---- status (clock, battery, network): read on a thread, once a second
    struct Status {
        std::string clock, date, battery;
        bool charging = false, online = false;
        cards::QuickStatus quick;
    };
    void statusWorker();
    void watchdog();
    void takeStatus();

    // ---- starting things
    void play();
    void launchGame(const Game& g);
    void openNro(const std::string& path);
    void openSphaira();
    void openAllSoftware(bool on);
    void activateDock(int index);
    void leave(int index, float seconds, std::function<void()> then);
    void comeBack();
    void handleHome();
    void askClose();
    void askCloseFirst(std::uint64_t open, const std::string& title, std::uint64_t next);
    void resumeOpen(int index);
    bool closeEverything();
    void pumpDaemon();
    void say(const std::string& message, float seconds = 4.5f);
    std::uint64_t runningId() const;
    std::uint64_t hostTitleId() const;
    AccountUid launchUid() const;
    void syncHomebrewFlag();

    // ---- popups
    struct Item {
        std::string label;
        std::function<void()> run;
        std::function<std::string()> value;
        std::function<void(int)> change;
        bool danger = false;
        bool stay = false;
    };
    void openMenu(std::string title, std::vector<Item> items);
    void openOptions();
    void openPower();
    void openSettings();
    void openStats();
    void updateMenu();
    void drawMenu(nxui::Renderer& r);
    void drawStats(nxui::Renderer& r);
    void drawMessage(nxui::Renderer& r);
    void drawPadNotice(nxui::Renderer& r);
    void enterStandby();

    // ---- the album
    struct Tile {
        std::string file, label;
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };
    void openGallery();
    void closeGallery();
    void showGalleryPage(int page);
    void loadGalleryPicture();
    void openRealAlbum();   // Nintendo's own Album, to play a video (HOME starts again after it)
    void setUpRealAlbum(bool on);
    void updateGallery();
    void drawGallery(nxui::Renderer& r);

    // ---- lock screen
    void lock();
    void beginUnlock();
    void startPasswordSetup();
    bool updateLock(float dt);
    void drawLock(nxui::Renderer& r);
    void loadLockPicture();
    void ensureLockFonts();

    // ---- drawing
    void draw(nxui::Renderer& r);
    void drawBackground(nxui::Renderer& r);
    bool drawAnimBackground(nxui::Renderer& r, const nxui::Rect& screen);
    void loadAnimBackground();
    void drawCards(nxui::Renderer& r);
    void drawCard(nxui::Renderer& r, const nxui::Rect& card, const Game& g, float t, float alpha, bool focusedRow);
    void drawChrome(nxui::Renderer& r, float a);
    void drawStatus(nxui::Renderer& r, float a, float hy, bool withClock);
    void drawPads(nxui::Renderer& r, float a, float rise); // the connected controllers, bottom left
    void readPads();
    void drawLaunch(nxui::Renderer& r);
    void drawReturn(nxui::Renderer& r);
    void drawHints(nxui::Renderer& r, float right, float cy, std::initializer_list<std::pair<const char*, const char*>> hints, float a);
    void icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint);
    void frostFast(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint);
    void frostLive(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint);
    nxui::Color accent(float a) const;
    bool handleTouch();

    // ---- state
    cards::Settings m_settings;
    cards::Audio m_audio;
    AppletLauncher m_launcher;
    std::shared_ptr<nxui::Widget> m_painter;
    nxui::Font m_fTitle, m_fLabel, m_fPill, m_fBody, m_fBadge, m_fLock, m_fLockDate;
    bool m_lockFonts = false;

    std::uint64_t m_startSuspended = 0;
    bool m_startRunning = false, m_atBoot = true;
    float m_time = 0.f;
    float m_beat = 0.f;      // m_time in the steps of the frames that are drawn (so the pulses step evenly)
    std::uint64_t m_running = 0; // the game or app that is open (suspended)
    float m_gridT = 1.f;     // All Software: the tiles coming together
    float m_slideT = 1.f;    // the row's cards settling after the order changed
    std::unordered_map<std::uint64_t, float> m_slideFrom;
    HidVibrationDeviceHandle m_vib[5]{};
    int m_vibCount = 0;
    bool m_vibTried = false;
    float m_buzzFor = 0.f;
    enum PadKind { kHandheld, kJoyDual, kJoyLeft, kJoyRight, kPro, kOtherPad };
    struct PadInfo {
        int kind = 0;
        int level[2] = {-1, -1}; // battery 0-4 (-1: none to show); a Joy-Con pair has one each
        bool charging[2] = {false, false};
    };
    std::vector<PadInfo> m_pads; // the controllers connected, in player order
    float m_padsIn = 0.f;
    float m_busyUntil = 0.f; // a press, a touch or a movement: every refresh is drawn until then
    bool m_moving = false;   // something is animating
    int m_startStage = 0;
    bool m_avatarTried = false;

    std::vector<Game> m_games, m_homebrew; // the installed titles, the .nro apps (and NRO forwarders)
    std::vector<Game> m_list;              // what shows: the row, or the grid
    bool m_grid = false;                   // All Software
    int m_sel = 0;
    float m_scroll = 0.f;
    Zone m_zone = Zone::Cards;
    int m_dockSel = 0;
    std::uint64_t m_backTo = 0;
    float m_repeat = 0.f;
    float m_rebuildIn = 1.f;
    std::future<Lists> m_listJob;
    std::future<std::uint32_t> m_deleteJob; // a game being uninstalled (its Result)
    std::uint64_t m_deleteId = 0;
    std::string m_deleteName;
    float m_gamesChangedIn = 0.f;
    float m_changesIn = 10.f;
    std::future<std::pair<int, std::uint64_t>> m_changesJob;
    int m_changesChecks = 0;
    std::map<std::string, std::uint64_t> m_recent; // .nro path -> when it was last started

    // icons
    std::unordered_map<std::uint64_t, std::unique_ptr<nxui::Texture>> m_icons;
    std::unordered_map<std::uint64_t, float> m_iconRetry;
    std::unordered_map<std::uint64_t, bool> m_iconAsked;
    std::unordered_map<std::string, std::unique_ptr<nxui::Texture>> m_glyphs;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Game> m_iconJobs;
    std::vector<IconDone> m_iconDone;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_quit{false};
    std::uint64_t m_iconWantSel = ~0ull;

    // status
    Status m_statusIn;
    bool m_statusFresh = false;
    std::atomic<bool> m_clock24{true};
    std::string m_clock = "--:--", m_battery, m_date;
    bool m_charging = false, m_online = true;
    cards::QuickStatus m_quick;
    cards::AccountUidLike m_user;
    std::unique_ptr<nxui::Texture> m_avatar;
    float m_brightness = 0.5f;

    // animated background
    bool m_animTried = false;
    std::vector<std::string> m_animPaths, m_animBlurPaths;
    std::vector<std::vector<std::uint8_t>> m_animJpegs; // (sized once; a thread fills them in order)
    std::atomic<std::size_t> m_animReady{0};            // how many are read
    std::atomic<bool> m_animDone{false};
    std::future<void> m_animReadJob;
    bool m_appletReturn = false;                         // back from the Album or the controller screen: not from a game
    bool m_resident = false;     // stays open behind a game it starts (standby_home)
    bool m_residentNext = false;
    bool m_standby = false;      // waiting hidden behind a game until the daemon says Home was pressed
    bool m_hiddenStart = false;  // started hidden: asks for the screen itself
    bool m_gotFocus = false;
    float m_nextAsk = 0.f;
    bool m_readySent = false;
    bool m_chimeWanted = false;                          // HOME came back from a game: a small sound
    std::vector<std::unique_ptr<nxui::Texture>> m_animBlur;
    std::size_t m_animBlurStep = 1, m_animCur = 0;
    float m_animFps = 10.f;
    std::unique_ptr<nxui::Texture> m_animTex, m_animNextTex;
    int m_animTexIdx = -1, m_animNextIdx = -1, m_animJobIdx = -1;
    std::future<Pixels> m_animJob;
    bool m_gpuSlow = false;
    float m_gpuWaitAvg = 0.f;

    // intro, return, launching
    float m_intro = 0.f, m_introFor = 1.0f, m_pageIn = 1.f, m_pressT = 1.f;
    int m_pressIndex = -1;
    std::unique_ptr<nxui::Texture> m_returnTex;
    float m_returnT = 0.f;
    bool m_leaving = false, m_left = false;
    float m_leaveT = 0.f, m_leaveFor = 1.f, m_leftAt = 0.f;
    int m_leaveIndex = -1;
    std::uint64_t m_leaveId = 0;
    nxui::Rect m_leaveFrom;
    std::function<void()> m_leaveThen;
    std::uint64_t m_pendingLaunch = 0;
    bool m_asleep = false, m_inSleepLock = false;
    std::atomic<bool> m_sleepFlag{false}; // the console is asleep: the status thread reads nothing

    // popups
    std::string m_menuTitle;
    std::vector<Item> m_menu;
    int m_menuSel = 0;
    float m_menuIn = 1.f;
    bool m_statsOpen = false;
    std::vector<Game> m_stats;
    float m_statsIn = 1.f;
    std::string m_message;
    float m_messageFor = 0.f;
    float m_messageLen = 4.5f;
    bool m_padNotice = false; // Joy-Con taken off with flight mode on
    float m_padNoticeIn = 0.f;
    bool m_statsAsked = false, m_statsEvery = false;
    struct PlayStat {
        std::uint64_t id = 0, last = 0, seconds = 0;
        std::uint32_t launches = 0;
    };
    std::future<std::vector<PlayStat>> m_statsJob;

    // album
    bool m_galOpen = false, m_galView = false;
    std::shared_ptr<std::vector<std::string>> m_galAll;
    std::vector<Tile> m_galTiles;
    std::vector<std::unique_ptr<nxui::Texture>> m_galTex;
    std::unique_ptr<nxui::Texture> m_galBig;
    std::future<std::vector<Tile>> m_galJob;
    std::future<Pixels> m_galBigJob;
    int m_galPage = 0, m_galSel = 0, m_galAt = 0;
    float m_galIn = 1.f;

    // lock screen
    bool m_locked = false;
    float m_lockIn = 0.f, m_unlockT = -1.f, m_lockIdle = 0.f;
    bool m_lockShotBlur = false, m_lockBlurOk = false;
    int m_blurAge = 1000;
    bool m_frosted = false;
    std::unique_ptr<nxui::Texture> m_lockShot, m_lockTex;
    std::string m_lockPicture, m_lockTexFor;
    std::future<Pixels> m_lockJob;
    std::uint64_t m_lockGameId = 0;
    std::string m_lockGameName;
    std::string m_dateText;
    bool m_pwSet = false, m_pwSetup = false;
    switchu::lockpw::Entry m_pw;

    // touch
    bool m_touchDown = false;
    float m_touchX0 = 0.f, m_touchY0 = 0.f;
};

} // namespace lite
