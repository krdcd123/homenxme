#ifdef SWITCHU_MENU // (the HOME build only: the standalone .nro has no daemon to talk to)
// LiteNX: the lists, icons, status, starting things and the daemon (the drawing is in LiteDraw.cpp).
#include "LiteActivity.hpp"
#include <switchu/file_log.hpp>

#include "LiteCommon.hpp"
#include "smi_commands.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <nxui/widgets/Widget.hpp>
#include <switchu/control_cache.hpp>
#include <switchu/lock_picture.hpp>
#include <switchu/ns_ext.hpp>
#include <switchu/smi_protocol.hpp>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace lite {
using namespace ui;

namespace {
class Painter final : public nxui::Widget {
public:
    explicit Painter(std::function<void(nxui::Renderer&)> fn) : m_fn(std::move(fn)) { setRect({0.f, 0.f, 1280.f, 720.f}); }

protected:
    void onRender(nxui::Renderer& r) override {
        if (m_fn) m_fn(r);
    }

private:
    std::function<void(nxui::Renderer&)> m_fn;
};

bool parseUid(const std::string& hex, cards::AccountUidLike& out) {
    if (hex.size() != 32) return false;
    out.uid[0] = std::strtoull(hex.substr(0, 16).c_str(), nullptr, 16);
    out.uid[1] = std::strtoull(hex.substr(16).c_str(), nullptr, 16);
    return out.uid[0] || out.uid[1];
}

constexpr const char* kRecentPath = "sdmc:/config/LiteNX/recent.tsv";
} // namespace

LiteActivity::LiteActivity(std::uint64_t suspendedId, bool appRunning, bool atBoot, bool appletReturn, bool hiddenStart, bool standby)
    : m_startSuspended(suspendedId), m_startRunning(appRunning), m_atBoot(atBoot), m_appletReturn(appletReturn),
      m_standby(standby), m_hiddenStart(hiddenStart) {}

// ---------------------------------------------------------------- start and stop

bool LiteActivity::onCreate() {
    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    ren.setBoxWireframeEnabled(false);
    app().setIdleThrottle(false); // it keeps moving: always 60 Hz

    pdmqryInitialize(); // play time
    nifmInitialize(NifmServiceType_User);
    m_launcher.init({
        [this]() { m_audio.play("hide_modal"); },
        [this]() { app().requestExit(); },
        [this]() { // before sleep / restart / power off: everything on the SD card
            m_settings.save();
            return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
        },
    });
    m_launcher.setStartupStatus(m_startSuspended, m_startRunning);

    m_settings.load();
    m_clock24 = m_settings.clock24;

    // Names in other scripts (Japanese, Chinese, Korean) from the console's own fonts.
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (PlSharedFontType type : {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                      PlSharedFontType_ChineseTraditional, PlSharedFontType_KO, PlSharedFontType_NintendoExt}) {
            PlFontData font{};
            if (R_SUCCEEDED(plGetSharedFontByType(&font, type))) nxui::Font::addFallback(font.address, font.size);
        }
    }
    const std::string fonts = std::string(cards::kAssets) + "fonts/";
    if (!m_fTitle.load(gpu, ren, fonts + "Inter-700.ttf", 30) || !m_fLabel.load(gpu, ren, fonts + "Inter-600.ttf", 22)
        || !m_fPill.load(gpu, ren, fonts + "Inter-600.ttf", 19) || !m_fBody.load(gpu, ren, fonts + "Inter-500.ttf", 17)
        || !m_fBadge.load(gpu, ren, fonts + "Inter-700.ttf", 15))
        return false;

    // The few glyphs the header and dock use (small files, read once).
    {
        nxui::GpuDevice::EssentialScope essential;
        for (const char* name : {"wifi", "airplane", "gear", "power", "pad", "play"}) {
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromFile(gpu, ren, std::string(cards::kAssets) + "icons/" + name + ".png", 96)) m_glyphs[name] = std::move(tex);
        }
    }

    // The games: last time's lists show at once; the real ones are read on a thread when they are old.
    const long long nowSec = static_cast<long long>(std::time(nullptr));
    const bool stale = m_atBoot || nowSec - m_settings.listsAt > 30 * 60 || nowSec < m_settings.listsAt;
    if (cards::loadLists(m_games, m_homebrew)) {
        m_homebrew.erase(std::remove_if(m_homebrew.begin(), m_homebrew.end(), [](const Game& g) { return !g.path.empty(); }), m_homebrew.end());
        m_changesChecks = 5;
        if (stale) {
            startListRefresh(true);
        } else { // anything installed or downloaded meanwhile? (a count and the paths: quick)
            m_changesJob = std::async(std::launch::async, []() {
                offScreenCore();
                std::pair<int, std::uint64_t> c{cards::installedTitleCount(), 0};
                cards::threadDone();
                return c;
            });
        }
    } else { // the very first time: read them now
        Lists l = readLists(false, 0);
        m_games = std::move(l.games);
        m_homebrew = std::move(l.homebrew);
        cards::saveLists(m_games, m_homebrew);
        m_settings.listsAt = nowSec;
        m_settings.titles = l.titles;
        m_settings.nroHash = l.nroHash;
    }
    {
        std::ifstream in(kRecentPath);
        std::string line;
        while (std::getline(in, line)) {
            const auto tab = line.rfind('\t');
            if (tab == std::string::npos) continue;
            m_recent[line.substr(0, tab)] = std::strtoull(line.c_str() + tab + 1, nullptr, 10);
        }
    }
    applyNames();
    applyRecent();

    // You: the one chosen last, else the last account that played, else the first.
    cards::AccountUidLike chosen;
    AccountUid last{};
    if (parseUid(m_settings.user, chosen)) { // still on the console?
        AccountUid uids[8] = {};
        s32 count = 0;
        bool exists = false;
        if (R_SUCCEEDED(accountListAllUsers(uids, 8, &count)))
            for (s32 i = 0; i < count; ++i) exists |= uids[i].uid[0] == chosen.uid[0] && uids[i].uid[1] == chosen.uid[1];
        if (!exists) chosen = cards::AccountUidLike{};
    }
    if (!chosen.uid[0] && !chosen.uid[1] && R_SUCCEEDED(accountGetLastOpenedUser(&last)) && accountUidIsValid(&last)) {
        chosen.uid[0] = last.uid[0];
        chosen.uid[1] = last.uid[1];
    }
    if (!chosen.uid[0] && !chosen.uid[1]) {
        AccountUid uids[8] = {};
        s32 count = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, 8, &count)) && count > 0) {
            chosen.uid[0] = uids[0].uid[0];
            chosen.uid[1] = uids[0].uid[1];
        }
    }
    m_user = chosen;

    m_painter = std::make_shared<Painter>([this](nxui::Renderer& r) { draw(r); });
    rootBox().addChild(m_painter);

    if (m_startSuspended || m_startRunning) { m_introFor = 0.35f; m_chimeWanted = !m_appletReturn; } // back from a game: quicker, with a chime
    if (m_settings.animBg && !m_gpuSlow) loadAnimBackground(); // (the pictures are read at once, behind everything else)
    m_intro = m_settings.motion ? 0.f : 1.f;

    rebuildList();
    if (!m_atBoot) { // back from a game: the game you were playing is chosen
        const std::uint64_t last = std::strtoull(m_settings.lastGame.c_str(), nullptr, 16);
        const bool lastWasNro = (last >> 56) == 0xFF;
        m_backTo = m_startSuspended && !lastWasNro ? m_startSuspended : last;
        if (m_backTo) selectById(m_backTo);
        // Its play time now (the rest of the lists stay as they were).
        for (auto* list : {&m_games, &m_homebrew})
            for (Game& g : *list)
                if (m_backTo && g.id == m_backTo && g.path.empty()) {
                    PdmPlayStatistics st{};
                    if (R_SUCCEEDED(pdmqryQueryPlayStatisticsByApplicationId(g.id, false, &st))) {
                        g.lastPlayed = st.last_timestamp_user;
                        g.playSeconds = st.playtime / 1000000000ULL;
                        g.launches = st.total_launches;
                    }
                }
        rebuildList();
        if (m_backTo) selectById(m_backTo);
    }
    m_scroll = scrollTarget();

    m_threads.emplace_back([this]() { iconWorker(); });
    m_threads.emplace_back([this]() { statusWorker(); });
    m_threads.emplace_back([this]() { watchdog(); });
    wantIcons();

    if (m_atBoot && m_settings.lockScreen) lock();
    if (!m_startSuspended && !m_startRunning) { // nothing's open, so no homebrew was left open
        m_settings.hbPath.clear();
        m_settings.hbHost.clear();
    }
    syncHomebrewFlag();
    return true;
}

bool LiteActivity::presentInitialFrame(nxui::Renderer& ren) {
    if (!m_startSuspended || m_appletReturn) return false; // (the game's last picture is for HOME pressed in a game, not for the Album)
    std::vector<std::uint8_t> px(1280u * 720u * 4u);
    bool ok = false;
    if (R_FAILED(appletGetLastApplicationCaptureImageEx(px.data(), px.size(), &ok)) || !ok) return false;
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 0xFF; // games leave alpha undefined
    auto tex = std::make_unique<nxui::Texture>();
    if (!tex->loadFromPixels(app().gpu(), ren, px.data(), 1280, 720)) return false;
    m_returnTex = std::move(tex);
    ren.drawTexture(m_returnTex.get(), {0.f, 0.f, 1280.f, 720.f});
    return true;
}

void LiteActivity::onDestroy() {
    switchu::FileLog::logCommit("[lite] closing");
    m_quit = true;
    m_cv.notify_all();
    for (std::thread& t : m_threads)
        if (t.joinable()) t.join();
    if (m_animReadJob.valid()) m_animReadJob.wait();
    if (m_listJob.valid()) m_listJob.wait();
    if (m_deleteJob.valid()) m_deleteJob.wait();
    if (m_changesJob.valid()) m_changesJob.wait();
    if (m_statsJob.valid()) m_statsJob.wait();
    if (m_animJob.valid()) m_animJob.wait();
    if (m_galJob.valid()) m_galJob.wait();
    if (m_galBigJob.valid()) m_galBigJob.wait();
    if (m_lockJob.valid()) m_lockJob.wait();
    switchu::FileLog::logCommit("[lite] threads stopped");
    switchu::menu::smi_cmd::menuClosing();
    nifmExit();
    pdmqryExit();
    m_settings.save();
    cards::flushWrites();
    m_audio.stop();
    switchu::FileLog::logCommit("[lite] closed");
}

// ---------------------------------------------------------------- the lists

LiteActivity::Lists LiteActivity::readLists(bool titlesOnly, std::uint64_t knownNroHash) {
    Lists l;
    l.titlesOnly = titlesOnly;
    std::vector<Game> titles = cards::listGames();
    l.titles = static_cast<int>(titles.size());
    for (Game& g : titles) (g.homebrew ? l.homebrew : l.games).push_back(std::move(g));
    if (titlesOnly) return l;
    // Only the installed titles and the NRO forwarders: the .nro files on the SD card are not listed.
    return l;
}

void LiteActivity::startListRefresh(bool titlesOnly) {
    if (m_listJob.valid()) return;
    const std::uint64_t known = m_settings.nroHash;
    m_listJob = std::async(std::launch::async, [titlesOnly, known]() {
        offScreenCore(); // this thread only (never the screen's)
        Lists l = readLists(titlesOnly, known);
        cards::threadDone();
        return l;
    });
}

void LiteActivity::takeLists(Lists lists) {
    if (!lists.titlesOnly) {
        m_settings.listsAt = static_cast<long long>(std::time(nullptr));
        m_settings.nroHash = lists.nroHash;
    }
    m_settings.titles = lists.titles;
    m_settings.save();
    for (std::size_t i = 0; i < lists.homebrew.size(); ++i) lists.homebrew[i].order = static_cast<int>(i);
    // Play times and last played stay as they were (the stats job reads them, not the lists).
    std::unordered_map<std::uint64_t, Game> had;
    for (const Game& g : m_games) had[g.id] = g;
    for (const Game& g : m_homebrew) had[g.id] = g;
    for (auto* list : {&lists.games, &lists.homebrew})
        for (Game& g : *list)
            if (const auto it = had.find(g.id); it != had.end() && !g.lastPlayed && !g.playSeconds) {
                g.lastPlayed = it->second.lastPlayed;
                g.playSeconds = it->second.playSeconds;
                g.launches = it->second.launches;
            }
    m_games = std::move(lists.games);
    m_homebrew = std::move(lists.homebrew);
    applyNames();
    applyRecent();
    const std::uint64_t keep = m_sel >= 0 && m_sel < static_cast<int>(m_list.size()) ? m_list[(std::size_t)m_sel].id : 0;
    rebuildList();
    if (keep) selectById(keep);
    if (m_backTo) selectById(m_backTo);
    cards::saveLists(m_games, m_homebrew);
    m_statsAsked = false; // the play times are read again
}

void LiteActivity::applyNames() {
    for (auto* list : {&m_games, &m_homebrew})
        for (Game& g : *list) {
            if (!g.original.empty()) {
                g.name = g.original;
                g.original.clear();
            }
            const std::string key = g.path.empty() ? cards::idText(g.id) : g.path;
            const auto it = m_settings.names.find(key);
            if (it != m_settings.names.end() && !it->second.empty() && it->second != g.name) {
                g.original = g.name;
                g.name = it->second;
            }
        }
}

// An .nro app has no play log: when it was last started from here stands in for it.
void LiteActivity::applyRecent() {
    for (Game& g : m_homebrew) {
        if (g.path.empty()) continue;
        const auto it = m_recent.find(g.path);
        if (it != m_recent.end() && it->second > g.lastPlayed) g.lastPlayed = it->second;
    }
}

void LiteActivity::noteRecent(const std::string& path) {
    m_recent[path] = static_cast<std::uint64_t>(std::time(nullptr));
    std::string out;
    for (const auto& [p, t] : m_recent) out += p + "\t" + std::to_string(t) + "\n";
    cards::writeLater(kRecentPath, out);
    applyRecent();
}

// The one row: games and apps together, the most recently played first (the top 10), then an "All Software"
// card that opens the whole list (B comes back); or, in All Software, every one.
void LiteActivity::rebuildList() {
    std::vector<Game> all;
    all.reserve(m_games.size() + m_homebrew.size());
    for (const auto* list : {&m_games, &m_homebrew})
        for (const Game& g : *list)
            if (g.action.empty() && std::none_of(all.begin(), all.end(), [&g](const Game& o) { return o.id == g.id; })) all.push_back(g);
    std::stable_sort(all.begin(), all.end(), [](const Game& a, const Game& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.order < b.order;
    });
    if (!m_grid) {
        if (all.size() > 10) all.resize(10);
        Game more;
        more.id = kAllSoftwareId;
        more.action = "allsoftware";
        more.name = "All Software";
        more.homebrew = true;
        all.push_back(std::move(more));
    }
    bool same = all.size() == m_list.size();
    for (std::size_t i = 0; same && i < all.size(); ++i) same = all[i].id == m_list[i].id;
    if (same) { // (play times and names still refresh)
        for (std::size_t i = 0; i < all.size(); ++i) m_list[i] = all[i];
        return;
    }
    const std::uint64_t keep = !m_list.empty() && m_sel >= 0 && m_sel < static_cast<int>(m_list.size()) ? m_list[(std::size_t)m_sel].id : 0;
    std::unordered_map<std::uint64_t, float> oldRel; // where each card was, for the move to the new order
    if (!m_grid && !m_list.empty())
        for (std::size_t i = 0; i < m_list.size(); ++i) oldRel[m_list[i].id] = static_cast<float>(i) - m_scroll;
    m_list = std::move(all);
    m_sel = 0;
    for (std::size_t i = 0; keep && i < m_list.size(); ++i)
        if (m_list[i].id == keep) m_sel = static_cast<int>(i);
    m_scroll = scrollTarget();
    m_iconWantSel = ~0ull;
    m_slideFrom.clear(); // the cards slide to their new places (not a jump)
    if (!oldRel.empty() && !m_grid && m_settings.motion) {
        for (std::size_t j = 0; j < m_list.size(); ++j) {
            const auto it = oldRel.find(m_list[j].id);
            if (it == oldRel.end()) continue;
            const float from = it->second - (static_cast<float>(j) - m_scroll);
            if (std::abs(from) > 0.01f) m_slideFrom[m_list[j].id] = from;
        }
        m_slideT = m_slideFrom.empty() ? 1.f : 0.f;
    }
}

const Game* LiteActivity::current() {
    if (m_list.empty()) return nullptr;
    m_sel = std::clamp(m_sel, 0, static_cast<int>(m_list.size()) - 1);
    return &m_list[(std::size_t)m_sel];
}

const Game* LiteActivity::gameById(std::uint64_t id) const {
    for (const auto* list : {&m_games, &m_homebrew})
        for (const Game& g : *list)
            if (g.id == id) return &g;
    return nullptr;
}

void LiteActivity::select(int index) {
    m_backTo = 0; // you've moved on
    if (m_list.empty()) return;
    index = std::clamp(index, 0, static_cast<int>(m_list.size()) - 1);
    if (index == m_sel) return;
    m_sel = index;
    m_audio.play("navigation");
    wantIcons();
}

void LiteActivity::selectById(std::uint64_t id) {
    for (std::size_t i = 0; i < m_list.size(); ++i)
        if (m_list[i].id == id) {
            m_sel = static_cast<int>(i);
            m_scroll = scrollTarget();
            m_iconWantSel = ~0ull;
            return;
        }
}

float LiteActivity::scrollTarget() const {
    if (m_grid) { // the grid scrolls by rows, the chosen one's row in view
        const int n = static_cast<int>(m_list.size());
        const int rows = (n + kGridCols - 1) / kGridCols;
        return static_cast<float>(std::clamp(m_sel / kGridCols - 1, 0, std::max(0, rows - 3)));
    }
    return static_cast<float>(m_sel);
}

float LiteActivity::riseOrder(int i) const {
    if (m_grid) // (rows come in one after another, left to right)
        return std::max(0.f, static_cast<float>(i / kGridCols) - m_scroll + 0.5f * static_cast<float>(i % kGridCols) + 1.f);
    return std::max(0.f, static_cast<float>(i) - m_scroll + 1.f);
}

void LiteActivity::openAllSoftware(bool on) {
    m_grid = on;
    m_sel = 0;
    m_list.clear();
    m_slideFrom.clear();
    m_slideT = 1.f;
    rebuildList();
    if (!on) // back to the row: the All Software tile is the chosen one
        for (std::size_t i = 0; i < m_list.size(); ++i)
            if (m_list[i].action == "allsoftware") m_sel = static_cast<int>(i);
    m_scroll = scrollTarget();
    if (on) m_gridT = 0.f;                    // the tiles come together
    else m_intro = std::min(m_intro, 0.35f); // the row's cards rise back in
    m_iconWantSel = ~0ull;
    m_audio.play(on ? "tab_transition" : "hide_modal");
    wantIcons();
}

// A grid tile's way into place: they start spread outward and small, and come together from the middle out.
float LiteActivity::gridTerm(int index) const {
    const float col = static_cast<float>(index % kGridCols), row = static_cast<float>(index / kGridCols);
    const float ord = std::max(0.f, row - m_scroll) * 0.5f + std::abs(col - 3.f) * 0.35f;
    return clamp01((m_gridT - 0.045f * ord) / 0.5f);
}

float LiteActivity::slideOf(std::uint64_t id) const {
    if (m_slideT >= 1.f) return 0.f;
    const auto it = m_slideFrom.find(id);
    return it == m_slideFrom.end() ? 0.f : it->second * (1.f - easeOut(m_slideT));
}

bool LiteActivity::cardRect(int index, nxui::Rect& card, float& chosen) {
    if (m_grid) {
        const float col = static_cast<float>(index % kGridCols), row = static_cast<float>(index / kGridCols);
        const float x = kGridX + col * kGridPitchX, y = kGridY + (row - m_scroll) * kGridPitchY;
        if (y < 96.f || y + kGridSize > 586.f) return false;
        const bool isSel = index == m_sel;
        chosen = isSel ? 1.f : 0.f;
        const float grow = isSel ? 16.f : 0.f;
        float cx = x + (kGridSize + grow) * 0.0f, cy = y - (isSel ? 4.f : 0.f);
        float size = kGridSize + grow;
        if (m_gridT < 1.f) { // coming together: from further out and smaller
            const float e = easeOut(gridTerm(index));
            const float mx = kGridX + 3.f * kGridPitchX + kGridSize * 0.5f, my = 330.f;
            const float tx = x + kGridSize * 0.5f, ty = y + kGridSize * 0.5f;
            const float fx = tx + (tx - mx) * 0.5f * (1.f - e), fy = ty + (ty - my) * 0.5f * (1.f - e);
            size *= 0.55f + 0.45f * e;
            cx = fx - size * 0.5f;
            cy = fy - size * 0.5f;
            card = {cx, cy, size, size};
            return true;
        }
        card = {x - grow * 0.5f, cy - grow * 0.5f, size, size};
        return true;
    }
    const float rel = static_cast<float>(index) - m_scroll + (index < static_cast<int>(m_list.size()) ? slideOf(m_list[(std::size_t)index].id) : 0.f);
    float slide = 0.f;
    if (m_leaving && m_leaveIndex >= 0 && index != m_leaveIndex) // launching: the others make way
        slide += (index < m_leaveIndex ? -1.f : 1.f) * 420.f * easeIn(m_leaveT / 0.5f);
    const float x0 = kFirstX + rel * kPitch + slide + (rel > 0.f ? std::min(rel, 1.f) * kGrow * 0.5f : 0.f);
    if (x0 > 1290.f || x0 + kPitch < -10.f) return false;
    chosen = clamp01(1.f - std::abs(rel));
    const float grow = easeOutBack(chosen);
    const float w = kSize + kGrow * grow, h = kSize + kGrow * grow;
    // Opening: the cards rise in one after another.
    const float k = std::max(0.f, rel + 1.f);
    const float rise = 1.f - easeOut((m_intro - 0.1f - 0.07f * k) / 0.5f);
    const float lift = 7.f * chosen;
    card = {x0 + kSize * 0.5f - w * 0.5f, kMidY - h * 0.5f - lift + 60.f * rise, w, h};
    return true;
}

// ---------------------------------------------------------------- icons

const nxui::Texture* LiteActivity::iconOf(std::uint64_t id) const {
    const auto it = m_icons.find(id);
    return it != m_icons.end() && it->second && it->second->valid() ? it->second.get() : nullptr;
}

// A game's own icon (the one in the game, from the console's cache): read once and kept, so it never
// changes to anything else. The ones near the choice are asked for, nearest first.
void LiteActivity::wantIcons() {
    if (m_list.empty()) return;
    const std::uint64_t key = (static_cast<std::uint64_t>(m_sel) << 1) | (m_grid ? 1u : 0u);
    if (key == m_iconWantSel) return;
    m_iconWantSel = key;
    std::vector<std::pair<int, int>> order; // distance, index
    const int n = static_cast<int>(m_list.size());
    for (int i = 0; i < n; ++i) {
        int d;
        if (m_grid) {
            const int dr = std::abs(i / kGridCols - m_sel / kGridCols);
            if (dr > 3) continue;
            d = dr * 10 + std::abs(i % kGridCols - m_sel % kGridCols);
        } else {
            d = std::abs(i - m_sel);
            if (d > 9) continue;
        }
        order.emplace_back(d, i);
    }
    std::sort(order.begin(), order.end());
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (const auto& o : order) {
            const Game& g = m_list[(std::size_t)o.second];
            if (!g.action.empty() || m_icons.count(g.id) || m_iconAsked[g.id]) continue;
            const auto retry = m_iconRetry.find(g.id);
            if (retry != m_iconRetry.end() && m_time < retry->second) continue;
            m_iconAsked[g.id] = true;
            m_iconJobs.push_back(g);
        }
    }
    m_cv.notify_all();
}

void LiteActivity::iconWorker() {
    offScreenCore();
    for (;;) {
        Game g;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this]() { return m_quit.load() || !m_iconJobs.empty(); });
            if (m_quit) break;
            g = std::move(m_iconJobs.front());
            m_iconJobs.pop_front();
        }
        IconDone done;
        done.id = g.id;
        const std::vector<std::uint8_t> data = cards::gameIcon(cards::forLookup(g));
        int w = 0, h = 0;
        if (!data.empty())
            if (stbi_uc* px = cards::decodePicture(data.data(), static_cast<int>(data.size()), w, h)) {
                done.rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
                stbi_image_free(px);
                if (w > 256) cards::shrink(done.rgba, w, h, 256);
                done.w = w;
                done.h = h;
            }
        std::lock_guard<std::mutex> lk(m_mu);
        m_iconDone.push_back(std::move(done));
    }
    cards::threadDone();
}

// Uploaded two a frame: the upload is a copy and a queue kick; the reading and decoding already happened.
void LiteActivity::takeIcons() {
    std::vector<IconDone> done;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const std::size_t n = std::min<std::size_t>(2, m_iconDone.size());
        for (std::size_t i = 0; i < n; ++i) done.push_back(std::move(m_iconDone[i]));
        m_iconDone.erase(m_iconDone.begin(), m_iconDone.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (done.empty()) return;
    app().gpu().beginTextureUploadBatch();
    for (IconDone& d : done) {
        m_iconAsked[d.id] = false;
        if (d.rgba.empty()) { // not in the cache yet: asked again in a few seconds
            m_iconRetry[d.id] = m_time + 4.f;
            continue;
        }
        auto tex = std::make_unique<nxui::Texture>();
        nxui::GpuDevice::EssentialScope essential; // (small, and always wanted)
        if (tex->loadFromPixels(app().gpu(), app().renderer(), d.rgba.data(), d.w, d.h)) m_icons[d.id] = std::move(tex);
        else m_iconRetry[d.id] = m_time + 4.f;
    }
    app().gpu().endTextureUploadBatch();
    if (m_icons.size() > 110) trimIcons();
}

// A long All Software list: the icons far from the choice are let go (they are read again if wanted).
void LiteActivity::trimIcons() {
    app().gpu().waitIdle(); // some may still be drawing
    for (auto it = m_icons.begin(); it != m_icons.end();) {
        std::size_t at = m_list.size();
        for (std::size_t i = 0; i < m_list.size(); ++i)
            if (m_list[i].id == it->first) at = i;
        const bool near = at < m_list.size() && (m_grid ? std::abs(static_cast<int>(at) / kGridCols - m_sel / kGridCols) <= 4
                                                         : std::abs(static_cast<int>(at) - m_sel) <= 12);
        const bool lockIcon = it->first == m_lockGameId;
        if (near || lockIcon) ++it;
        else it = m_icons.erase(it);
    }
    m_iconWantSel = ~0ull;
}

// ---------------------------------------------------------------- the clock, battery and network

void LiteActivity::statusWorker() {
    offScreenCore();
    cards::QuickStatus quick;
    std::uint64_t quickAt = 0;
    std::uint64_t lastClockMinute = ~0ull;
    while (!m_quit) {
        if (m_sleepFlag) { // asleep: no clock, battery or network reads (they would wake the processor for nothing)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        const std::uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
        Status s;
        TimeCalendarTime t{};
        TimeCalendarAdditionalInfo info{};
        u64 posix = 0;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &posix)) && R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &t, &info))) {
            char buf[16];
            if (m_clock24) std::snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
            else std::snprintf(buf, sizeof(buf), "%d:%02d %s", (t.hour + 11) % 12 + 1, t.minute, t.hour < 12 ? "AM" : "PM");
            s.clock = buf;
            static const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
            static const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                                                  "August", "September", "October", "November", "December"};
            if (info.wday < 7 && t.month >= 1 && t.month <= 12)
                s.date = std::string(kDays[info.wday]) + " " + std::to_string(t.day) + " " + kMonths[t.month - 1];
            lastClockMinute = posix / 60;
        }
        u32 charge = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) s.battery = std::to_string(charge) + "%";
        PsmChargerType charger = PsmChargerType_Unconnected;
        s.charging = R_SUCCEEDED(psmGetChargerType(&charger)) && charger != PsmChargerType_Unconnected;
        NifmInternetConnectionType type{};
        u32 strength = 0;
        NifmInternetConnectionStatus status{};
        s.online = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) && status == NifmInternetConnectionStatus_Connected;
        if (now >= quickAt) { // flight mode and the Wi-Fi switch: every 15 s is plenty
            quickAt = now + 15000;
            quick = cards::readQuickStatus();
        }
        s.quick = quick;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_statusIn = std::move(s);
            m_statusFresh = true;
        }
        for (int i = 0; i < 10 && !m_quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    (void)lastClockMinute;
    cards::threadDone();
}

void LiteActivity::takeStatus() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_statusFresh) return;
    m_statusFresh = false;
    if (!m_statusIn.clock.empty()) m_clock = m_statusIn.clock;
    if (!m_statusIn.battery.empty()) m_battery = m_statusIn.battery;
    if (!m_statusIn.date.empty()) m_date = m_statusIn.date;
    m_charging = m_statusIn.charging;
    m_online = m_statusIn.online;
    m_quick = m_statusIn.quick;
}

// ---------------------------------------------------------------- the daemon

std::uint64_t LiteActivity::runningId() const {
    // A homebrew app left open (Keep homebrew open) runs as the game it borrowed:
    // it's the app's card that's open, not that game's.
    const std::uint64_t id = m_launcher.suspendedTitleId();
    if (id && !m_settings.hbPath.empty() && cards::idText(id) == m_settings.hbHost) return cards::nroId(m_settings.hbPath);
    return id;
}

// A game whose code the daemon swaps for hbloader while homebrew runs, so homebrew gets application mode
// (installed, startable, not a game card, not the one open). 0: over the Album instead.
std::uint64_t LiteActivity::hostTitleId() const {
    using namespace switchu::ns;
    for (const bool official : {true, false})
        for (const Game& g : m_games)
            if (g.id && g.id != runningId() && g.path.empty() && !g.homebrew
                && (!official || (g.id >> 56) == 0x01)
                && (g.flags == 0 || ((g.flags & AppViewFlag_CanLaunch) && !(g.flags & AppViewFlag_IsGameCard))))
                return g.id;
    return 0;
}

AccountUid LiteActivity::launchUid() const {
    AccountUid uid{};
    uid.uid[0] = m_user.uid[0];
    uid.uid[1] = m_user.uid[1];
    return uid;
}

// The daemon closes homebrew on HOME only while this file is there (Keep homebrew open is off).
void LiteActivity::syncHomebrewFlag() {
    constexpr const char* kFlag = "sdmc:/config/SwitchU/homebrew_close_on_home";
    std::error_code ec;
    if (m_settings.keepHomebrew) std::filesystem::remove(kFlag, ec);
    else std::ofstream(kFlag) << "1";
}

void LiteActivity::pumpDaemon() {
    nxui::setPhase("lite: the daemon's messages");
    AppletStorage st;
    while (R_SUCCEEDED(appletPopInteractiveInData(&st))) {
        switchu::smi::DaemonNotification n{};
        s64 size = 0;
        appletStorageGetSize(&st, &size);
        if (size >= static_cast<s64>(sizeof(n))) appletStorageRead(&st, 0, &n, sizeof(n));
        appletStorageClose(&st);
        if (n.magic != switchu::smi::kNotifyMagic) continue;
        using M = switchu::smi::MenuMessage;
        switch (n.msg) {
            case M::HomeRequest:
                if (!m_asleep) handleHome();
                break;
            case M::ApplicationExited: {
                m_launcher.setAppRunning(false);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(0);
                m_settings.hbPath.clear(); // (no homebrew is left open)
                m_settings.hbHost.clear();
                const std::uint64_t next = m_pendingLaunch;
                m_pendingLaunch = 0;
                if (next)
                    for (const auto* list : {&m_games, &m_homebrew})
                        for (const Game& g : *list)
                            if (g.id == next) {
                                const Game copy = g;
                                if (copy.path.empty()) launchGame(copy);
                                else openNro(copy.path);
                                return;
                            }
                break;
            }
            case M::ApplicationSuspended:
                m_launcher.setAppRunning(true);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(n.app_id);
                break;
            case M::AppRecordsChanged:
            case M::GameCardMountFailure:
                m_gamesChangedIn = 3.f; // read again once they stop (an update sends many)
                break;
            case M::AppViewFlagsUpdate:
                for (auto* list : {&m_games, &m_homebrew})
                    for (Game& g : *list)
                        if (g.id == n.app_id) g.flags = n.payload;
                break;
            case M::BatteryStatusChanged:
                m_battery = std::to_string(switchu::smi::batteryPayloadPercentage(n.payload)) + "%";
                m_charging = switchu::smi::batteryPayloadCharging(n.payload);
                break;
            case M::StandbyWake: // HOME was waiting behind the game: take the screen now
                if (!m_standby) break;
                switchu::FileLog::logCommit("[lite] standby: woken, asking for the screen");
                m_standby = false;
                app().setSleeping(false);
                m_hiddenStart = true;
                m_gotFocus = false;
                m_readySent = false;
                m_nextAsk = m_time;
                m_intro = 0.f;
                m_chimeWanted = true;
                break;
            case M::SleepSequence: // stop drawing (a loop that keeps drawing never lets it sleep)
                if (m_standby) break; // (hidden: the game has the screen)
                switchu::FileLog::log("[lite] sleep sequence (lock screen %d, game open %d)", m_settings.lockScreen ? 1 : 0, runningId() ? 1 : 0);
                m_asleep = true;
                m_sleepFlag = true;
                m_audio.pause();
                if (m_settings.lockScreen) { // drawn into the last frames, so waking shows it at once
                    m_inSleepLock = true;
                    lock();
                    m_inSleepLock = false;
                    m_lockIn = 1.f;
                }
                app().setSleeping(true);
                switchu::FileLog::logCommit("[lite] sleep frames drawn; waiting");
                break;
            case M::WakeUp:
                if (m_standby) break;
                switchu::FileLog::log("[lite] wake up (locked %d)", m_locked ? 1 : 0);
                m_asleep = false;
                m_sleepFlag = false;
                app().setSleeping(false);
                if (m_settings.lockScreen) buzz(); // a light buzz as it comes on to the lock screen
                if (m_settings.lockScreen && m_locked) break; // slept while locked: the lock screen is quiet
                if (m_settings.lockScreen && !m_locked) {
                    m_inSleepLock = true;
                    lock();
                    m_inSleepLock = false;
                    break;
                }
                m_audio.resume();
                break;
            case M::OperationFailed: {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "That didn't work (0x%X).", n.payload);
                if (m_leaving) comeBack();
                say(buf);
                break;
            }
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------- starting things

void LiteActivity::say(const std::string& message, float seconds) {
    m_message = message;
    m_messageLen = seconds;
    m_messageFor = seconds;
}

void LiteActivity::leave(int index, float seconds, std::function<void()> then) {
    if (m_leaving) return;
    m_leaving = true;
    m_left = false;
    m_leaveT = m_settings.motion ? 0.f : 0.98f;
    m_leaveFor = std::max(0.05f, seconds);
    m_leaveIndex = -1;
    m_leaveId = 0;
    float t = 0.f;
    if (index >= 0 && index < static_cast<int>(m_list.size()) && cardRect(index, m_leaveFrom, t)) {
        m_leaveIndex = index;
        m_leaveId = m_list[(std::size_t)index].id;
    }
    m_leaveThen = std::move(then);
    m_menu.clear();
    m_statsOpen = false;
    m_audio.play(m_leaveIndex >= 0 ? "launch_game" : "activation");
}

void LiteActivity::comeBack() {
    m_leaving = false;
    m_left = false;
    m_leaveT = 0.f;
    m_leaveThen = nullptr;
    m_intro = std::min(m_intro, 0.6f); // the cards rise back in
}

void LiteActivity::play() {
    const Game* g = current();
    if (!g || m_leaving) return;
    if (g->action == "allsoftware") {
        openAllSoftware(true);
        return;
    }
    if (!g->path.empty()) {
        openNro(g->path);
        return;
    }
    launchGame(*g);
}

void LiteActivity::launchGame(const Game& g) {
    using namespace switchu::ns;
    const std::uint64_t id = g.id;
    m_settings.lastGame = cards::idText(id); // back from it, it's chosen
    const bool running = id != 0 && id == runningId();
    if (!running && g.flags && !(g.flags & AppViewFlag_CanLaunch)) {
        if ((g.flags & AppViewFlag_IsGameCard) && !(g.flags & AppViewFlag_IsGameCardInserted)) say("Put the game card in to play " + g.name + ".");
        else if (g.flags & AppViewFlag_NeedsUpdate) say(g.name + " needs an update first.");
        else if (g.flags & AppViewFlag_NeedsVerify) say(g.name + "'s data needs checking (System Settings > Data).");
        else if (g.flags & AppViewFlag_IsDownloading) say(g.name + " is still downloading.");
        else say(g.name + " can't start right now.");
        return;
    }
    int index = -1; // which card it is (the zoom starts from it)
    for (std::size_t i = 0; i < m_list.size(); ++i)
        if (m_list[i].id == id) index = static_cast<int>(i);
    if (running) { // back to it
        resumeOpen(index);
        return;
    }
    if (const std::uint64_t open = runningId()) { // another one is open: it closes first
        askCloseFirst(open, g.name, id);
        return;
    }
    m_settings.hbPath.clear(); // (a game, not homebrew, is what's open next)
    m_settings.hbHost.clear();
    leave(index, 0.42f, [this, id]() {
        if (!m_launcher.requestLaunch(id, launchUid())) {
            comeBack();
            say("The game didn't start. Try again.");
            return;
        }
        m_launcher.closeForLaunch();
    });
}

// Back to the game or app HOME left open.
void LiteActivity::resumeOpen(int index) {
    leave(index, 0.38f, [this]() { m_launcher.resumeApplication(); });
}

// Another game or app is open: it closes first (starting one ends the other), then `next` starts.
void LiteActivity::askCloseFirst(std::uint64_t open, const std::string& title, std::uint64_t next) {
    std::string name = "the game that's open";
    if (const Game* o = gameById(open)) name = o->name;
    openMenu("Close " + name + "?", {
        {"Close it and start " + title, [this, next, name]() {
             m_pendingLaunch = next;
             m_launcher.terminateApplication();
             say("Closing " + name + "...");
         }, nullptr, nullptr, true, false},
        {"Cancel", [] {}, nullptr, nullptr, false, false},
    });
}

void LiteActivity::openNro(const std::string& path) {
    int index = -1;
    for (std::size_t i = 0; i < m_list.size(); ++i)
        if (m_list[i].path == path) index = static_cast<int>(i);
    if (index >= 0) m_settings.lastGame = cards::idText(m_list[(std::size_t)index].id);
    if (const std::uint64_t open = runningId()) {
        if (open == cards::nroId(path)) { // left open (Keep homebrew open): back to it
            resumeOpen(index);
            return;
        }
        // Another is open: it closes first (starting this one would end it unasked).
        askCloseFirst(open, index >= 0 ? m_list[(std::size_t)index].name : std::filesystem::path(path).stem().string(), cards::nroId(path));
        return;
    }
    const std::uint64_t host = hostTitleId();
    m_settings.hbPath = path; // what's open in that game now (runningId)
    m_settings.hbHost = cards::idText(host);
    noteRecent(path);
    leave(index, 0.38f, [this, path, host]() {
        if (!m_launcher.launchNro(path, host, launchUid())) {
            comeBack();
            const std::string why = AppletLauncher::loaderError();
            say(why.empty() ? std::string("That app didn't start.") : why);
        }
    });
}

// Sphaira as an application (in a borrowed game, with all the memory), not as an applet.
void LiteActivity::openSphaira() {
    std::string path;
    for (const Game& g : m_homebrew)
        if (!g.path.empty() && (lower(g.name).find("sphaira") != std::string::npos || lower(g.path).find("sphaira") != std::string::npos)) {
            path = g.path;
            break;
        }
    std::error_code ec;
    if (path.empty() && std::filesystem::exists("sdmc:/switch/sphaira/sphaira.nro", ec)) path = "sdmc:/switch/sphaira/sphaira.nro";
    if (path.empty()) {
        say("Sphaira isn't on the SD card (switch/sphaira/sphaira.nro).");
        return;
    }
    if (const std::uint64_t open = runningId()) {
        if (open == cards::nroId(path)) {
            resumeOpen(-1);
            return;
        }
        askCloseFirst(open, "Sphaira", cards::nroId(path));
        return;
    }
    const std::uint64_t host = hostTitleId();
    if (!host) {
        say("Sphaira runs as an application, in an installed game it borrows: install one first.");
        return;
    }
    m_settings.hbPath = path;
    m_settings.hbHost = cards::idText(host);
    noteRecent(path);
    leave(-1, 0.38f, [this, path, host]() {
        if (!m_launcher.launchNro(path, host, launchUid(), /*applicationOnly=*/true)) {
            comeBack();
            const std::string why = AppletLauncher::loaderError();
            say(why.empty() ? std::string("Sphaira didn't start.") : why);
        }
    });
}

void LiteActivity::activateDock(int index) {
    switch (index) {
        case 0: openSphaira(); break;
        case 1: openGallery(); break;
        case 2:
            leave(-1, 0.3f, [this]() { m_launcher.launchControllerPairing(); }); // the daemon opens it and starts HOME again
            break;
        case 3: openSettings(); break;
        default: openPower(); break;
    }
}

// HOME pressed while HOME shows: things close; else back to the open game; else the first card.
void LiteActivity::handleHome() {
    switchu::FileLog::log("[lite] home pressed");
    if (m_locked) { // HOME opens the lock screen too (not past a password)
        if (m_pwSetup) { // stops making one
            m_pwSetup = false;
            beginUnlock();
            return;
        }
        if (m_pwSet) return;
        if (m_unlockT < 0.f) beginUnlock();
        return;
    }
    if (m_leaving) return;
    if (closeEverything()) {
        m_audio.play("hide_modal");
        return;
    }
    if (const std::uint64_t open = runningId()) {
        if (const Game* g = gameById(open)) {
            const Game copy = *g;
            if (copy.path.empty()) launchGame(copy);
            else openNro(copy.path);
        }
        return;
    }
    if (m_grid) openAllSoftware(false);
    m_zone = Zone::Cards;
    select(0);
}

bool LiteActivity::closeEverything() {
    const bool any = !m_menu.empty() || m_statsOpen || m_galOpen;
    m_menu.clear();
    m_statsOpen = false;
    if (m_galOpen) closeGallery();
    return any;
}

// X: close the open game or app (a homebrew app runs as the game it borrowed), after asking.
void LiteActivity::askClose() {
    const std::uint64_t open = runningId();
    if (!open) {
        say("Nothing is open.");
        return;
    }
    std::string name = "the open software";
    if (const Game* g = gameById(open)) name = g->name;
    openMenu("Close " + name + "?", {
        {"Close it", [this, name]() {
             m_launcher.terminateApplication();
             say("Closing " + name + "...");
         }, nullptr, nullptr, true, false},
        {"Cancel", [] {}, nullptr, nullptr, false, false},
    });
}

// ---------------------------------------------------------------- every frame

void LiteActivity::onUpdate(float dt) {
    dt = std::min(dt, frameInterval() > 1 ? 0.12f : 1.f / 30.f); // a slow frame doesn't jump everything ahead
    pumpDaemon();
    if (m_asleep) return;
    m_running = runningId(); // (read once a frame: the cards that say Resume)
    if ((m_padsIn -= dt) <= 0.f) {
        m_padsIn = 0.5f;
        readPads();
    }
    if (m_buzzFor > 0.f && (m_buzzFor -= dt) <= 0.f) { // the buzz ends
        HidVibrationValue off{};
        off.freq_low = 160.f;
        off.freq_high = 320.f;
        for (int i = 0; i < m_vibCount; ++i) hidSendVibrationValue(m_vib[i], &off);
    }
    m_time += dt;
    app().input().mergeRightStick();
    {
        // How often frames are drawn: every refresh while anything moves or a button is used; else only as often as
        // the background changes (the GPU and the CPU rest in between: less heat, less battery).
        const int every = frameInterval();
        const float hz = 60.f / static_cast<float>(every > 1 ? every : 3);
        m_beat = std::floor(m_time * hz) / hz;
        if (app().input().hasUserActivity()) m_busyUntil = m_time + 0.7f;
        m_moving = m_intro < 1.f || m_gridT < 1.f || m_slideT < 1.f || m_menuIn < 1.f || m_statsIn < 1.f || m_galIn < 1.f || m_pressT < 1.f || m_pageIn < 1.f
                   || std::abs(m_scroll - scrollTarget()) > 0.001f || m_leaving || m_returnTex || m_messageFor > 0.f || m_padNotice || m_padNoticeIn > 0.f
                   || (m_locked && (m_lockIn < 1.f || m_unlockT >= 0.f || m_pw.wrong > 0.f));
    }

    if (m_standby) { // waiting hidden behind a game: say so, then stay quiet until the daemon wakes us
        if (!m_readySent && m_time > 0.15f) {
            m_readySent = true;
            switchu::menu::smi_cmd::menuReady();
            switchu::FileLog::logCommit("[lite] standby: waiting behind the game");
            app().setSleeping(true);
        }
        return;
    }
    if (m_hiddenStart && !m_gotFocus) { // started hidden: ask for the screen until it is ours
        if (appletGetFocusState() == AppletFocusState_InFocus) {
            m_gotFocus = true;
            switchu::FileLog::log("[lite] hidden start: the screen is ours (%.2f s)", m_time);
        } else if (m_time >= m_nextAsk) {
            const Result rc = appletRequestToGetForeground();
            switchu::FileLog::log("[lite] hidden start: asked for the screen rc=0x%X", rc);
            m_nextAsk = m_time + 0.25f;
        }
    }
    if (!m_readySent && m_time > 0.15f && (!m_hiddenStart || m_gotFocus)) { // (the daemon restarts HOME if it never hears this)
        m_readySent = true;
        switchu::menu::smi_cmd::menuReady();
    }

    // What the first frame doesn't need starts later, so HOME coming back from a game doesn't hitch on it.
    if (m_startStage < 3) {
        if (m_startStage < 2 && (m_time > 1.5f || (m_chimeWanted && m_time > 0.08f))) {
            m_startStage = 2;
            m_audio.start();
            m_audio.setSounds(m_settings.sounds, m_settings.soundVolume);
            m_audio.setMusic(false, 0); // the stock HOME has no music
            if (m_chimeWanted) { m_audio.play("activation"); m_chimeWanted = false; }
        } else if (m_startStage == 2 && m_time > 1.6f) {
            m_startStage = 3;
        }
    }
    // The profile picture: a small decode, loaded early so it is not missing when HOME comes back.
    if (!m_avatarTried && m_time > 0.35f) {
        m_avatarTried = true;
        {
            AccountUid uid{};
            uid.uid[0] = m_user.uid[0];
            uid.uid[1] = m_user.uid[1];
            AccountProfile profile{};
            if (accountUidIsValid(&uid) && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
                u32 size = 0;
                if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0) {
                    std::vector<std::uint8_t> jpeg(size);
                    u32 got = 0;
                    if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), size, &got)) && got > 0) {
                        m_avatar = std::make_unique<nxui::Texture>();
                        if (!m_avatar->loadFromMemory(app().gpu(), app().renderer(), jpeg.data(), got, 96)) m_avatar.reset();
                    }
                }
                accountProfileClose(&profile);
            }
        }
    }
    if (!m_lockFonts && m_time > 4.f && !m_leaving && !m_locked) ensureLockFonts(); // (a hitch at idle, not when locking)
    m_audio.update();
    takeStatus();
    takeIcons();

    // The game's last picture: shown while HOME starts (the loading lag stays behind a still frame of
    // the game), then it shrinks into its card while the cards rise in, as one movement.
    if (m_returnTex && m_time > 0.25f) {
        m_returnT += dt;
        if (m_returnT > kReturnSecs) {
            app().gpu().waitIdle(); // it may still be drawing
            m_returnTex.reset();
        }
    }
    // A GPU that can't keep 60 frames a second (waiting over 22 ms a frame on it, for a second or so): the
    // live glass (the lock screen) goes plain and the background stays still for this session.
    {
        const auto waits = app().gpu().takeWaitStats();
        m_gpuWaitAvg += (std::min(waits.frameMs, 120.f) - m_gpuWaitAvg) * 0.05f;
        if (!m_gpuSlow && m_gpuWaitAvg > 22.f && m_time > 3.f && !m_leaving) m_gpuSlow = true;
    }
    const bool motion = m_settings.motion;
    auto step = [&](float& v, float seconds) { v = motion ? std::min(1.f, v + dt / seconds) : 1.f; };
    if (!m_returnTex || m_returnT > 0.f) step(m_intro, m_introFor);
    step(m_gridT, 0.7f);
    step(m_slideT, 0.45f);
    step(m_menuIn, 0.2f);
    step(m_statsIn, 0.2f);
    step(m_galIn, 0.26f);
    step(m_pressT, 0.22f);
    const float target = scrollTarget();
    m_scroll = motion ? m_scroll + (target - m_scroll) * (1.f - std::exp(-dt * 13.f)) : target;
    if (std::abs(m_scroll - target) < 0.001f) m_scroll = target;
    if (m_messageFor > 0.f) m_messageFor -= dt;

    // A game was uninstalled (or wasn't).
    if (m_deleteJob.valid() && m_deleteJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::uint32_t rc = m_deleteJob.get();
        if (R_SUCCEEDED(rc)) {
            switchu::control_cache::remove(m_deleteId);
            const std::uint64_t id = m_deleteId;
            auto gone = [id](const Game& x) { return x.id == id; };
            m_games.erase(std::remove_if(m_games.begin(), m_games.end(), gone), m_games.end());
            m_homebrew.erase(std::remove_if(m_homebrew.begin(), m_homebrew.end(), gone), m_homebrew.end());
            rebuildList();
            m_sel = std::clamp(m_sel, 0, std::max(0, static_cast<int>(m_list.size()) - 1));
            m_scroll = scrollTarget();
            m_iconWantSel = ~0ull;
            cards::saveLists(m_games, m_homebrew);
            m_audio.play("toggle_off");
            say(m_deleteName + " was uninstalled.");
            wantIcons();
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%X", rc);
            say("Couldn't uninstall " + m_deleteName + " (" + buf + ").");
        }
    }

    // The real lists arrived (not while something is launching).
    if (m_listJob.valid() && !m_leaving && m_listJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Lists l = m_listJob.get();
        takeLists(std::move(l));
    }
    if (m_gamesChangedIn > 0.f && (m_gamesChangedIn -= dt) <= 0.f) {
        if (m_listJob.valid()) m_gamesChangedIn = 1.f; // after the one that's reading
        else startListRefresh(true);
    }
    // Back from a game: anything downloaded or installed meanwhile? (the .nro files' paths, and a count: quick)
    if (m_changesJob.valid() && m_changesJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto c = m_changesJob.get();
        if (c.first != m_settings.titles || (c.second && c.second != m_settings.nroHash)) startListRefresh(c.second == m_settings.nroHash);
    }
    // Play times: every game's at the console's start, back from a game that one's (pdm takes one slow call per game).
    if (!m_statsAsked && m_time > 6.f && !m_leaving) {
        m_statsAsked = true;
        const std::uint64_t lastGame = std::strtoull(m_settings.lastGame.c_str(), nullptr, 16);
        bool known = false;
        for (const Game& g : m_games) known |= g.lastPlayed != 0;
        const bool every = m_atBoot || !known;
        std::vector<std::uint64_t> ids;
        for (const auto* list : {&m_games, &m_homebrew})
            for (const Game& g : *list)
                if (g.path.empty() && (every || g.id == lastGame)) ids.push_back(g.id);
        m_statsJob = std::async(std::launch::async, [ids]() {
            offScreenCore();
            std::vector<PlayStat> out;
            const auto last = cards::lastPlayTimes(ids);
            for (const std::uint64_t id : ids) {
                PdmPlayStatistics st{};
                if (R_FAILED(pdmqryQueryPlayStatisticsByApplicationId(id, false, &st))) continue;
                PlayStat s;
                s.id = id;
                const auto it = last.find(id);
                s.last = it != last.end() ? it->second : st.last_timestamp_user;
                s.seconds = st.playtime / 1000000000ULL;
                s.launches = st.total_launches;
                out.push_back(s);
            }
            cards::threadDone();
            return out;
        });
    }
    if (m_statsJob.valid() && m_statsJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const std::vector<PlayStat> stats = m_statsJob.get();
        std::unordered_map<std::uint64_t, const PlayStat*> byId;
        for (const PlayStat& s : stats) byId[s.id] = &s;
        const std::uint64_t chosen = current() ? current()->id : 0;
        for (auto* list : {&m_games, &m_homebrew})
            for (Game& g : *list)
                if (g.path.empty()) {
                    const auto it = byId.find(g.id);
                    if (it == byId.end()) continue;
                    g.lastPlayed = it->second->last;
                    g.playSeconds = it->second->seconds;
                    g.launches = it->second->launches;
                }
        rebuildList();
        if (chosen) selectById(chosen);
    }
    // The row is refreshed now and then (a game just played moves to the front).
    if ((m_rebuildIn -= dt) <= 0.f) {
        m_rebuildIn = 5.f;
        if (!m_leaving) rebuildList();
    }
    wantIcons();

    // ---- leaving: the icon opens up, then the daemon starts the thing
    if (m_leaving) {
        m_leaveT = std::min(1.f, m_leaveT + dt / m_leaveFor);
        if (m_leaveT >= 1.f && !m_left) {
            m_left = true;
            m_leftAt = m_time;
            m_settings.save();
            auto then = std::move(m_leaveThen);
            m_leaveThen = nullptr;
            if (then) then();
        } else if (m_left && m_time - m_leftAt > 12.f) {
            // Nothing took over (a restart that didn't happen): HOME again, not a black screen.
            comeBack();
            say("That didn't go through. Try again.");
        }
        return;
    }
    if (updateLock(dt)) return;

    auto& in = app().input();
    using B = nxui::Button;
    const bool aDown = in.isDown(B::A), bDown = in.isDown(B::B);
    const bool upDown = in.isDown(B::DUp) || in.isDown(B::LStickU) || in.isDown(B::RStickU);
    const bool downDown = in.isDown(B::DDown) || in.isDown(B::LStickD) || in.isDown(B::RStickD);
    const bool leftDown = in.isDown(B::DLeft) || in.isDown(B::LStickL) || in.isDown(B::RStickL);
    const bool rightDown = in.isDown(B::DRight) || in.isDown(B::LStickR) || in.isDown(B::RStickR);

    if (m_padNotice) return; // the Joy-Con notice holds the screen until they are back on
    if (handleTouch()) return;
    if (m_galOpen) {
        updateGallery();
        return;
    }
    if (!m_menu.empty()) {
        updateMenu();
        return;
    }
    if (m_statsOpen) {
        if (aDown || bDown || in.isDown(B::Minus)) {
            m_statsOpen = false;
            m_audio.play("hide_modal");
        }
        return;
    }

    // Left / right: repeating while held.
    const bool left = in.isHeld(B::DLeft) || in.isHeld(B::LStickL) || in.isHeld(B::RStickL);
    const bool right = in.isHeld(B::DRight) || in.isHeld(B::LStickR) || in.isHeld(B::RStickR);
    int move = 0;
    if (leftDown) {
        move = -1;
        m_repeat = 0.32f;
    } else if (rightDown) {
        move = 1;
        m_repeat = 0.32f;
    } else if (left || right) {
        if ((m_repeat -= dt) <= 0.f) {
            move = left ? -1 : 1;
            m_repeat = 0.075f;
        }
    }
    if (move) {
        if (m_zone == Zone::Cards) {
            select(m_sel + move);
        } else if (m_zone == Zone::Bottom) {
            const int next = std::clamp(m_dockSel + move, 0, kDockCount - 1);
            if (next != m_dockSel) m_audio.play("navigation");
            m_dockSel = next;
        }
    }
    bool gridHandled = false;
    if (m_grid && m_zone == Zone::Cards && (upDown || downDown)) {
        const int n = static_cast<int>(m_list.size()), s = m_sel;
        gridHandled = true;
        if (upDown) {
            if (s >= kGridCols) select(s - kGridCols);
            else { m_zone = Zone::Top; m_audio.play("navigation"); }
        } else {
            if (s + kGridCols < n) select(s + kGridCols);
            else if (n > 0 && s / kGridCols < (n - 1) / kGridCols) select(n - 1);
            else { m_zone = Zone::Bottom; m_audio.play("navigation"); }
        }
    }
    if (upDown && !gridHandled && m_zone != Zone::Top) {
        m_zone = m_zone == Zone::Bottom ? Zone::Cards : Zone::Top;
        m_audio.play("navigation");
    }
    if (downDown && !gridHandled && m_zone != Zone::Bottom) {
        m_zone = m_zone == Zone::Top ? Zone::Cards : Zone::Bottom;
        m_audio.play("navigation");
    }
    if (aDown) {
        if (m_zone == Zone::Cards) {
            m_pressIndex = m_sel;
            m_pressT = 0.f;
            play();
        } else if (m_zone == Zone::Top) {
            openStats();
        } else {
            activateDock(m_dockSel);
        }
    }
    if (bDown && m_grid) openAllSoftware(false); // (B from All Software goes back to the row, wherever the choice is)
    else if (bDown && m_zone != Zone::Cards) m_zone = Zone::Cards;
    if (in.isDown(B::Plus)) openOptions();
    if (in.isDown(B::X)) askClose();
    if (in.isDown(B::Minus)) openStats();
}


// The refreshes between two frames: 1 (every refresh) while anything moves; otherwise the background's own pace
// (a 10 frames a second animation is drawn 20 times a second: each of its pictures twice).
int LiteActivity::frameInterval() const {
    if (m_asleep || m_moving || m_time < m_busyUntil || m_returnTex || m_startStage < 3) return 1;
    if (m_settings.animBg && !m_gpuSlow && !m_animPaths.empty()) {
        if (m_animFps <= 10.f) return 3;
        if (m_animFps <= 15.f) return 2;
        if (m_animFps <= 20.f) return 3;
        return m_animFps <= 30.f ? 2 : 1;
    }
    return 4; // nothing but the slow pulse of the chosen one's ring: 15 a second
}

// A short, light buzz in the controllers: the console's own when it wakes up to the lock screen.
void LiteActivity::buzz() {
    if (!m_vibTried) {
        m_vibTried = true;
        HidVibrationDeviceHandle h[2];
        if (R_SUCCEEDED(hidInitializeVibrationDevices(h, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)))
            for (int i = 0; i < 2; ++i) m_vib[m_vibCount++] = h[i];
        if (R_SUCCEEDED(hidInitializeVibrationDevices(h, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual)))
            for (int i = 0; i < 2; ++i) m_vib[m_vibCount++] = h[i];
        if (m_vibCount < 5 && R_SUCCEEDED(hidInitializeVibrationDevices(h, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey)))
            m_vib[m_vibCount++] = h[0];
    }
    HidVibrationValue v{};
    v.amp_low = 0.22f;
    v.freq_low = 160.f;
    v.amp_high = 0.22f;
    v.freq_high = 320.f;
    for (int i = 0; i < m_vibCount; ++i) hidSendVibrationValue(m_vib[i], &v);
    m_buzzFor = m_vibCount ? 0.14f : 0.f;
}

// A thread that only watches: if the screen's thread stays in one step for seconds, the step is written to the log
// (and flushed at once), so a freeze can be traced to where it stopped.
void LiteActivity::watchdog() {
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    std::uint32_t serial = nxui::g_phaseSerial.load();
    std::uint64_t since = armGetSystemTick(), woke = since;
    bool told = false;
    int lines = 0;
    while (!m_quit) {
        svcSleepThread(250'000'000ULL);
        const std::uint64_t now = armGetSystemTick();
        const float away = static_cast<float>(armTicksToNs(now - woke)) / 1.0e6f - 250.f;
        woke = now;
        const std::uint32_t s = nxui::g_phaseSerial.load();
        if (away > 1500.f || m_sleepFlag) { // (all of HOME was stopped, or the console sleeps: nothing to blame)
            serial = s;
            since = now;
            told = false;
            continue;
        }
        if (s != serial) {
            if (told) switchu::FileLog::logCommit("[lite] the wait ended after %.1f s", static_cast<float>(armTicksToNs(now - since)) / 1.0e9f);
            serial = s;
            since = now;
            told = false;
        } else if (!told && lines < 30 && static_cast<float>(armTicksToNs(now - since)) / 1.0e9f > 1.5f) {
            told = true;
            ++lines;
            switchu::FileLog::logCommit("[lite] the screen's thread has been in \"%s\" for over 1.5 s", nxui::g_phase.load());
        }
    }
}

// Which controllers are connected (read from the shared memory: no service call), twice a second.
void LiteActivity::readPads() {
    std::vector<PadInfo> now;
    static const HidNpadIdType kIds[] = {HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
                                         HidNpadIdType_No4,      HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7,
                                         HidNpadIdType_No8};
    for (const HidNpadIdType id : kIds) {
        const u32 s = hidGetNpadStyleSet(id);
        if (!s) continue;
        PadInfo p;
        if (s & HidNpadStyleTag_NpadHandheld) p.kind = kHandheld; // (the console's own battery is at the top right)
        else if (s & HidNpadStyleTag_NpadJoyDual) p.kind = kJoyDual;
        else if (s & HidNpadStyleTag_NpadFullKey) p.kind = kPro;
        else if (s & HidNpadStyleTag_NpadJoyLeft) p.kind = kJoyLeft;
        else if (s & HidNpadStyleTag_NpadJoyRight) p.kind = kJoyRight;
        else p.kind = kOtherPad;
        HidPowerInfo one{}, left{}, right{};
        if (p.kind == kJoyDual) {
            hidGetNpadPowerInfoSplit(id, &left, &right);
            p.level[0] = static_cast<int>(std::min<u32>(left.battery_level, 4));
            p.level[1] = static_cast<int>(std::min<u32>(right.battery_level, 4));
            p.charging[0] = left.is_charging;
            p.charging[1] = right.is_charging;
        } else if (p.kind != kHandheld) {
            hidGetNpadPowerInfoSingle(id, &one);
            p.level[0] = static_cast<int>(std::min<u32>(one.battery_level, 4));
            p.charging[0] = one.is_charging;
        }
        now.push_back(p);
    }
    if (now.size() > 4) now.resize(4);
    // Joy-Con taken off the console with flight mode on: they can't connect, so say so (as the console does).
    bool wasHandheld = false, isHandheld = false;
    for (const PadInfo& p : m_pads) wasHandheld |= p.kind == kHandheld;
    for (const PadInfo& p : now) isHandheld |= p.kind == kHandheld;
    const bool detached = wasHandheld && !isHandheld;
    const bool anyPad = !now.empty();
    m_pads = std::move(now);
    if (detached && m_quick.airplane) m_padNotice = true; // stays until they are back on the console
    else if (m_padNotice && (isHandheld || (!m_quick.airplane && anyPad))) m_padNotice = false;
}

} // namespace lite

#endif // SWITCHU_MENU
