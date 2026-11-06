#ifdef SWITCHU_MENU // (the HOME build only: the standalone .nro has no daemon to talk to)
// LiteNX: everything that is drawn, the popups, the album, the lock screen and touch.
#include "LiteActivity.hpp"

#include "LiteCommon.hpp"
#include "widgets/BatteryDrawing.hpp"

#include <nxui/Application.hpp>
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/third_party/stb/stb_image.h>
#include <switchu/lock_picture.hpp>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace lite {
using namespace ui;

namespace {
constexpr int kGalleryPage = 12;
const nxui::Rect kGalCard{110.f, 70.f, 1060.f, 580.f};

nxui::Rect galTile(int i) {
    const float w = 224.f, h = 126.f;
    const float x0 = kGalCard.x + (kGalCard.width - (4.f * w + 3.f * 20.f)) * 0.5f;
    return {x0 + static_cast<float>(i % 4) * (w + 20.f), kGalCard.y + 96.f + static_cast<float>(i / 4) * 150.f, w, h};
}
} // namespace

// ---------------------------------------------------------------- helpers

nxui::Color LiteActivity::accent(float a) const {
    return kAccents[std::clamp(m_settings.accent, 0, kAccentCount - 1)].color.withAlpha(a);
}

void LiteActivity::icon(nxui::Renderer& r, const char* name, float cx, float cy, float size, const nxui::Color& tint) {
    const auto it = m_glyphs.find(name);
    if (it == m_glyphs.end() || !it->second || !it->second->valid()) return;
    r.drawTexture(it->second.get(), {cx - size * 0.5f, cy - size * 0.5f, size, size}, tint);
}

// Glass without the live blur: a pre-blurred copy of the background frame (animbg/blur_*.jpg) under the rect.
void LiteActivity::frostFast(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint) {
    if (m_animCur < m_animBlur.size() && m_animBlur[m_animCur] && m_animBlur[m_animCur]->valid() && rect.x >= 0.f && rect.y >= 0.f
        && rect.x + rect.width <= 1280.f && rect.y + rect.height <= 720.f) {
        const nxui::Texture* t = m_animBlur[m_animCur].get();
        const float kx = static_cast<float>(t->width()) / 1280.f, ky = static_cast<float>(t->height()) / 720.f;
        r.drawTextureSubRounded(t, {rect.x * kx, rect.y * ky, rect.width * kx, rect.height * ky}, rect, radius, kWhite);
        r.drawRoundedRect(rect, tint, radius);
        return;
    }
    // No blurred copy: a plainer, darker glass.
    r.drawRoundedRect(rect, nxui::Color(tint.r * 0.8f, tint.g * 0.8f, tint.b * 0.9f, std::min(1.f, tint.a + 0.28f)), radius);
}

// The live blur (one blurred copy of the frame, made when the lock screen opens).
void LiteActivity::frostLive(nxui::Renderer& r, const nxui::Rect& rect, float radius, const nxui::Color& tint) {
    if (m_frosted) r.drawOffscreenRounded(0, rect, radius, kWhite.withAlpha(std::min(1.f, tint.a * 3.f)));
    r.drawRoundedRect(rect, tint, radius);
}

void LiteActivity::drawHints(nxui::Renderer& r, float right, float cy,
                             std::initializer_list<std::pair<const char*, const char*>> hints, float a) {
    float x = right;
    for (auto it = std::rbegin(hints); it != std::rend(hints); ++it) {
        x -= measured(m_fBody, it->second).x * 0.9f;
        textMid(r, m_fBody, it->second, x, cy, kMuted.withAlpha(a), 0.9f);
        const float bw = std::max(28.f, measured(m_fBadge, it->first).x + 16.f);
        const nxui::Rect pill{x - 8.f - bw, cy - 14.f, bw, 28.f};
        r.drawRoundedRect(pill, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 14.f);
        r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), 14.f, 1.f);
        textMid(r, m_fBadge, it->first, pill.x + (bw - measured(m_fBadge, it->first).x) * 0.5f, cy, kWhite.withAlpha(a));
        x = pill.x - 22.f;
    }
}

void LiteActivity::ensureLockFonts() {
    if (m_lockFonts) return;
    m_lockFonts = true;
    const std::string fonts = std::string(cards::kAssets) + "fonts/";
    m_fLock.load(app().gpu(), app().renderer(), fonts + "Inter-600.ttf", 104);
    m_fLockDate.load(app().gpu(), app().renderer(), fonts + "Inter-500.ttf", 28);
}

// ---------------------------------------------------------------- the frame

void LiteActivity::draw(nxui::Renderer& r) {
    // Locked and settled: nothing under the lock screen moves, so HOME isn't drawn at all: the game's last
    // frame (blurred once), the lock picture, or the blurred copy of HOME already made.
    if (m_locked && m_lockIn >= 1.f && m_unlockT < 0.f) {
        const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
        if (m_lockShot && m_lockShot->valid()) {
            r.drawTexture(m_lockShot.get(), screen, kWhite);
            m_frosted = r.gpu().offscreenReady() && !m_gpuSlow;
            if (m_frosted && !m_lockShotBlur) {
                r.captureToOffscreen(false);
                r.applyBlur(4.f, 3);
                m_lockShotBlur = true;
            }
            drawLock(r);
            return;
        }
        if (m_lockTex && m_lockTex->valid()) {
            drawLock(r);
            return;
        }
        if (m_lockBlurOk && r.gpu().offscreenReady() && !m_gpuSlow) {
            m_frosted = true;
            drawLock(r);
            return;
        }
    }
    drawBackground(r);
    drawCards(r);
    // Launching: the bar goes first.
    const float ui = m_leaving ? 1.f - easeOut(m_leaveT / 0.3f) : 1.f;
    drawChrome(r, easeOut(m_intro * 2.f) * ui);
    drawMessage(r);
    drawMenu(r);
    drawStats(r);
    drawGallery(r);
    drawLaunch(r);
    drawReturn(r);
    // The lock screen over HOME: HOME is blurred once (again every few frames while it opens).
    if (m_locked) {
        m_frosted = r.gpu().offscreenReady() && !m_gpuSlow;
        if (m_frosted && (!m_lockBlurOk || (m_lockIn < 1.f && ++m_blurAge >= 6))) {
            r.captureToOffscreen(false);
            r.applyBlur(2.5f, 2);
            m_blurAge = 0;
            m_lockBlurOk = true;
        }
    }
    drawLock(r);
}

// ---------------------------------------------------------------- the background

// sdmc:/config/CardsNX/animbg/*.jpg|png, in name order, played at fps.txt's rate. Streamed: the files are
// kept as JPEGs in RAM, one frame is decoded at a time on a thread and uploaded (two textures in all).
void LiteActivity::loadAnimBackground() {
    m_animTried = true;
    std::error_code ec;
    const std::string dir = "sdmc:/config/CardsNX/animbg";
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = lower(e.path().extension().string());
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
        if (e.path().filename().string().rfind("blur_", 0) == 0) m_animBlurPaths.push_back(e.path().string());
        else files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    std::sort(m_animBlurPaths.begin(), m_animBlurPaths.end());
    { // at most 8 of the small blurred ones (they change slowly): every step-th
        m_animBlurStep = std::max<std::size_t>(1, (m_animBlurPaths.size() + 7) / 8);
        std::vector<std::string> kept;
        for (std::size_t i = 0; i < m_animBlurPaths.size(); i += m_animBlurStep) kept.push_back(m_animBlurPaths[i]);
        m_animBlurPaths = std::move(kept);
    }
    if (files.size() > 48) files.resize(48);
    m_animPaths = std::move(files);
    std::string rate;
    std::getline(std::ifstream(dir + "/fps.txt"), rate);
    const float fps = rate.empty() ? 0.f : static_cast<float>(std::atof(rate.c_str()));
    if (fps >= 1.f && fps <= 60.f) m_animFps = fps;
}

bool LiteActivity::drawAnimBackground(nxui::Renderer& r, const nxui::Rect& screen) {
    if (!m_settings.animBg || m_gpuSlow) return false;
    if (!m_animTried) loadAnimBackground();
    if (m_animPaths.empty()) return false;
    // The files are read into RAM one a frame (JPEGs, ~0.2 MB each); nothing is uploaded until all are in.
    if (m_animJpegs.size() < m_animPaths.size()) {
        std::ifstream in(m_animPaths[m_animJpegs.size()], std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> data;
        if (in) {
            const std::streamoff size = in.tellg();
            if (size > 0 && size < (8 << 20)) {
                data.resize(static_cast<std::size_t>(size));
                in.seekg(0);
                if (!in.read(reinterpret_cast<char*>(data.data()), size)) data.clear();
            }
        }
        if (data.empty()) m_animPaths.erase(m_animPaths.begin() + static_cast<std::ptrdiff_t>(m_animJpegs.size()));
        else m_animJpegs.push_back(std::move(data));
        if (m_animJpegs.size() == 1 && !m_animTex) { // the first picture goes up at once: HOME never shows the plain gradient
            int w = 0, h = 0;
            if (stbi_uc* p = cards::decodePicture(m_animJpegs[0].data(), static_cast<int>(m_animJpegs[0].size()), w, h)) {
                auto tex = std::make_unique<nxui::Texture>();
                nxui::GpuDevice::EssentialScope essential;
                if (tex->loadFromPixels(app().gpu(), app().renderer(), p, w, h)) {
                    m_animTex = std::move(tex);
                    m_animTexIdx = 0;
                }
                stbi_image_free(p);
            }
        }
        if (m_animJpegs.size() < m_animPaths.size() || m_animJpegs.empty()) { // (the rest are read, a file a frame)
            if (!m_animTex || !m_animTex->valid()) return false;
            drawCover(r, m_animTex.get(), screen, 0.f, kWhite);
            return true;
        }
    }
    const std::size_t n = m_animJpegs.size();
    if (n == 0) return false;
    const std::size_t idx = static_cast<std::size_t>(m_time * m_animFps) % n;
    // The small blurred copies (the glass): a few, one a frame.
    if (m_animBlur.size() < m_animBlurPaths.size()) {
        auto tex = std::make_unique<nxui::Texture>();
        if (tex->loadFromFile(app().gpu(), app().renderer(), m_animBlurPaths[m_animBlur.size()], 320)) m_animBlur.push_back(std::move(tex));
        else m_animBlurPaths.clear();
    }
    m_animCur = std::min(idx / m_animBlurStep, m_animBlur.empty() ? std::size_t{0} : m_animBlur.size() - 1);
    // A finished decode goes up as a texture; the one on screen is replaced when the next one is the one wanted.
    if (m_animJob.valid() && m_animJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Pixels px = m_animJob.get();
        if (px.raw) {
            auto tex = std::make_unique<nxui::Texture>();
            nxui::GpuDevice::EssentialScope essential; // (two frames: always wanted)
            if (tex->loadFromPixels(app().gpu(), app().renderer(), px.raw.get(), px.w, px.h)) {
                m_animNextTex = std::move(tex);
                m_animNextIdx = m_animJobIdx;
            }
        }
        m_animJobIdx = -1;
    }
    if (m_animNextTex && (m_animNextIdx == static_cast<int>(idx) || !m_animTex)) {
        m_animTex = std::move(m_animNextTex); // (the old one is freed once the GPU is done with it)
        m_animTexIdx = m_animNextIdx;
        m_animNextIdx = -1;
    }
    // What to decode next: the frame that's wanted now if it isn't up, else the one after it.
    const int want = m_animTexIdx == static_cast<int>(idx) ? static_cast<int>((idx + 1) % n) : static_cast<int>(idx);
    if (!m_animJob.valid() && m_animNextIdx != want && m_animTexIdx != want) {
        m_animJobIdx = want;
        // (the pictures are all in before the first decode and never change: no copy of the JPEG, none of the pixels)
        m_animJob = std::async(std::launch::async, [data = &m_animJpegs[static_cast<std::size_t>(want)]]() {
            offScreenCore();
            Pixels px;
            int w = 0, h = 0;
            if (stbi_uc* p = cards::decodePicture(data->data(), static_cast<int>(data->size()), w, h)) {
                px.raw = std::shared_ptr<std::uint8_t>(p, [](std::uint8_t* q) { stbi_image_free(q); });
                px.w = w;
                px.h = h;
            }
            cards::threadDone();
            return px;
        });
    }
    if (!m_animTex || !m_animTex->valid()) return false;
    drawCover(r, m_animTex.get(), screen, 0.f, kWhite);
    return true;
}

void LiteActivity::drawBackground(nxui::Renderer& r) {
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    if (!drawAnimBackground(r, screen)) {
        r.drawGradientRect(screen, nxui::Color(0.36f, 0.41f, 0.55f, 1.f), nxui::Color(0.22f, 0.25f, 0.34f, 1.f));
    }
    r.drawGradientRect({0.f, 0.f, 1280.f, 140.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.35f), nxui::Color(0.05f, 0.06f, 0.10f, 0.f));
    r.drawGradientRect({0.f, 560.f, 1280.f, 160.f}, nxui::Color(0.05f, 0.06f, 0.10f, 0.f), nxui::Color(0.05f, 0.06f, 0.10f, 0.40f));
}

// ---------------------------------------------------------------- the row

void LiteActivity::drawCards(nxui::Renderer& r) {
    if (m_list.empty()) {
        textMid(r, m_fLabel, "No games installed", 480.f, kMidY, kWhite);
        return;
    }
    const bool focused = m_zone == Zone::Cards;
    const float making = m_leaving ? 1.f - easeIn(m_leaveT / 0.5f) : 1.f; // launching: the others fade
    for (int i = 0; i < static_cast<int>(m_list.size()); ++i) {
        if (m_leaving && i == m_leaveIndex) continue; // drawn on top (drawLaunch)
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t)) continue;
        const float term = (m_intro - 0.1f - 0.07f * riseOrder(i)) / 0.5f;
        const float alpha = (m_grid ? clamp01(gridTerm(i) * 2.2f) : easeOut(term)) * making;
        if (alpha <= 0.01f) continue;
        const float pressed = m_pressIndex == i ? std::sin(clamp01(m_pressT) * 3.14159f) : 0.f;
        if (pressed > 0.f) card = scaled(card, 1.f - 0.05f * pressed);
        drawCard(r, card, m_list[(std::size_t)i], t, alpha, focused);
    }
}

// One entry: a rounded square icon (the game's own), the selection frame round the chosen one.
void LiteActivity::drawCard(nxui::Renderer& r, const nxui::Rect& card, const Game& g, float t, float alpha, bool focusedRow) {
    const float rad = card.width * 0.2f;
    r.drawRoundedRect({card.x - 2.f, card.y + 6.f + 6.f * t, card.width + 4.f, card.height + 4.f},
                      nxui::Color(0.f, 0.f, 0.f, (0.20f + 0.16f * t) * alpha), rad + 2.f);
    const nxui::Color tint = kWhite.withAlpha(alpha);
    if (g.action == "allsoftware") { // frosted tile with a grid of squares
        frostFast(r, card, rad, nxui::Color(0.12f, 0.14f, 0.20f, 0.50f * alpha));
        r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.25f * alpha), rad, 1.5f);
        const float u = card.width * 0.14f, gap = card.width * 0.05f;
        const float x0 = card.x + (card.width - 3.f * u - 2.f * gap) * 0.5f, y0 = card.y + card.height * 0.5f - 1.5f * u - gap - 14.f;
        for (int i = 0; i < 9; ++i)
            r.drawRoundedRect({x0 + static_cast<float>(i % 3) * (u + gap), y0 + static_cast<float>(i / 3) * (u + gap), u, u},
                              kWhite.withAlpha(0.85f * alpha), u * 0.25f);
        const std::string name = "All Software";
        textMid(r, m_fLabel, name, card.x + (card.width - measured(m_fLabel, name).x * 0.8f) * 0.5f, card.y + card.height - 26.f,
                kWhite.withAlpha(alpha), 0.8f);
    } else if (const nxui::Texture* ico = iconOf(g.id)) { // the game's own icon: nothing else is ever drawn here
        r.drawTextureRounded(ico, card, rad, tint);
    } else {
        r.drawRoundedRect(card, nxui::Color(0.28f, 0.31f, 0.40f, 0.85f * alpha), rad);
        const std::string name = fit(m_fLabel, g.name, card.width - 24.f, 0.9f);
        textMid(r, m_fLabel, name, card.x + (card.width - measured(m_fLabel, name).x * 0.9f) * 0.5f, card.y + card.height * 0.5f,
                kMuted.withAlpha(alpha), 0.9f);
    }
    if (!m_grid && g.id == m_running && g.action.empty() && m_running) { // open now: a pill at the bottom of the icon
        const std::string word = "Resume";
        const float w = 20.f + 10.f + measured(m_fBadge, word).x + 16.f;
        const nxui::Rect pill{card.x + (card.width - w) * 0.5f, card.y + card.height - 46.f, w, 32.f};
        frostFast(r, pill, 16.f, nxui::Color(0.05f, 0.07f, 0.09f, 0.55f * alpha));
        r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.2f * alpha), 16.f, 1.5f);
        icon(r, "play", pill.x + 20.f, pill.y + 16.f, 16.f, kGreen.withAlpha(alpha));
        textMid(r, m_fBadge, word, pill.x + 34.f, pill.y + 16.f, kWhite.withAlpha(alpha));
    }
    if (t < 0.5f) // the ones beside the chosen one sit a little dimmer
        r.drawRoundedRect(card, nxui::Color(0.f, 0.f, 0.f, 0.18f * (1.f - t) * alpha), rad);
    if (t > 0.01f) { // the frame: a bright outer line and a white inner one, pulsing a little
        const float ring = focusedRow ? 0.85f + 0.15f * std::sin(m_beat * 4.f) : 0.55f;
        r.drawRoundedRectOutline({card.x - 5.f, card.y - 5.f, card.width + 10.f, card.height + 10.f}, accent(ring * t * alpha), rad + 5.f, 4.5f);
        r.drawRoundedRectOutline(card, kWhite.withAlpha(0.85f * t * alpha), rad, 2.f);
    }
}

// ---------------------------------------------------------------- header, dock, footer

// Battery, Wi-Fi (or flight mode) and, on HOME, the time: the same on HOME and on the lock screen.
void LiteActivity::drawStatus(nxui::Renderer& r, float a, float hy, bool withClock) {
    float x = 1236.f - measured(m_fPill, m_battery).x;
    textMid(r, m_fPill, m_battery, x, hy, kWhite.withAlpha(a));
    const int charge = m_battery.empty() ? -1 : std::atoi(m_battery.c_str());
    x -= 14.f + 36.f;
    switchu::battery_drawing::drawBattery(r, {x, hy - 10.f, 36.f, 20.f}, charge, m_charging, a);
    x -= 16.f + 26.f;
    if (m_quick.airplane) {
        icon(r, "airplane", x + 13.f, hy, 26.f, kWhite.withAlpha(a));
    } else {
        icon(r, "wifi", x + 13.f, hy, 26.f, kWhite.withAlpha((m_online ? 1.f : 0.4f) * a));
        if (!m_online) r.drawLine({x + 2.f, hy + 11.f}, {x + 24.f, hy - 11.f}, kWhite.withAlpha(0.7f * a), 2.5f);
    }
    if (!withClock) return;
    x -= 22.f + measured(m_fPill, m_clock).x;
    textMid(r, m_fPill, m_clock, x, hy, kWhite.withAlpha(a));
}

void LiteActivity::drawChrome(nxui::Renderer& r, float a) {
    if (a <= 0.001f) return;
    // Header: you on the left; the time, Wi-Fi, flight mode and the battery on the right.
    const float face = 52.f;
    const nxui::Rect fr{kAvatar.x, kAvatar.y - 14.f * (1.f - a), face, face};
    if (m_avatar && m_avatar->valid()) r.drawTextureRounded(m_avatar.get(), fr, face * 0.5f, kWhite.withAlpha(a));
    else r.drawCircle({fr.x + face * 0.5f, fr.y + face * 0.5f}, face * 0.5f, nxui::Color(1.f, 1.f, 1.f, 0.3f * a), 32);
    r.drawRoundedRectOutline(fr, kWhite.withAlpha(0.8f * a), face * 0.5f, 2.f);
    if (m_zone == Zone::Top)
        r.drawRoundedRectOutline({fr.x - 5.f, fr.y - 5.f, face + 10.f, face + 10.f}, accent((0.8f + 0.2f * std::sin(m_beat * 5.f)) * a),
                                 face * 0.5f + 5.f, 3.f);
    const float hy = fr.y + face * 0.5f;
    drawStatus(r, a, hy, true);

    // The chosen one's name, in blue above its icon, and its play time in a frosted box under it
    if (const Game* g = current()) {
        nxui::Rect card;
        float t = 0.f;
        if (cardRect(m_sel, card, t) && a >= 0.999f) {
            const std::string name = fit(m_fTitle, g->name, 640.f, 0.62f);
            textMid(r, m_fTitle, name, m_grid ? kGridX : card.x, m_grid ? 112.f : card.y - 34.f, kBlue.withAlpha(a), 0.62f);
            if (m_zone == Zone::Cards) {
                std::string line;
                if (g->action == "allsoftware") line = std::to_string(m_games.size() + m_homebrew.size()) + " titles";
                else line = g->playSeconds ? playTime(g->playSeconds) + " played" : std::string("Not played yet");
                const float lw = measured(m_fBody, line).x;
                const nxui::Rect box = m_grid ? nxui::Rect{kGridX + measured(m_fTitle, name).x * 0.62f + 22.f, 92.f, lw + 44.f, 40.f}
                                              : nxui::Rect{card.x + (card.width - (lw + 44.f)) * 0.5f, card.y + card.height + 18.f, lw + 44.f, 40.f};
                frostFast(r, box, 20.f, nxui::Color(0.07f, 0.08f, 0.11f, 0.45f * a));
                r.drawRoundedRectOutline(box, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), 20.f, 1.5f);
                textMid(r, m_fBody, line, box.x + 22.f, box.y + 20.f, kWhite.withAlpha(a));
            }
        }
    }

    // The dock
    const float rise = 40.f * (1.f - a);
    const nxui::Rect pill{kDock.x, kDock.y + rise, kDock.width, kDock.height};
    frostFast(r, pill, pill.height * 0.5f, nxui::Color(0.07f, 0.08f, 0.11f, 0.45f * a));
    r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.22f * a), pill.height * 0.5f, 1.5f);
    r.drawGradientRect({pill.x + 30.f, pill.y + 3.f, pill.width - 60.f, 24.f}, nxui::Color(1.f, 1.f, 1.f, 0.08f * a), nxui::Color(1.f, 1.f, 1.f, 0.f));
    const bool dock = m_zone == Zone::Bottom;
    for (int i = 0; i < kDockCount; ++i) {
        const float cx = dockX(i), cy = dockY() + rise;
        const bool on = dock && m_dockSel == i;
        const float k = on ? 1.12f : 1.f;
        if (on) {
            const float ring = 0.8f + 0.2f * std::sin(m_beat * 5.f);
            r.drawCircle({cx, cy}, 33.f, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 40);
            r.drawRoundedRectOutline({cx - 36.f, cy - 36.f, 72.f, 72.f}, accent(ring * a), 36.f, 3.f);
        }
        const nxui::Color c = kWhite.withAlpha(a);
        const float w = 2.6f;
        switch (i) {
            case 0: // Sphaira: a grid of apps
                for (int q = 0; q < 4; ++q)
                    r.drawRoundedRectOutline({cx - 15.f * k + static_cast<float>(q % 2) * 17.f * k, cy - 15.f * k + static_cast<float>(q / 2) * 17.f * k,
                                              13.f * k, 13.f * k}, c, 4.f, w);
                break;
            case 1: // Album: a frame with a lens
                r.drawRoundedRectOutline({cx - 18.f * k, cy - 13.f * k, 36.f * k, 26.f * k}, c, 7.f, w);
                r.drawRoundedRectOutline({cx - 7.f * k, cy - 7.f * k, 14.f * k, 14.f * k}, c, 7.f * k, w);
                break;
            case 2: icon(r, "pad", cx, cy, 38.f * k, c); break;
            case 3: icon(r, "gear", cx, cy, 36.f * k, c); break;
            default: icon(r, "power", cx, cy, 34.f * k, c); break;
        }
    }
    // What the chosen dock button opens, in a small frosted label above it
    if (dock && a >= 0.999f) {
        static const char* const kLabel[] = {"Sphaira", "Album", "Controllers", "Settings", "Power"};
        const std::string label = kLabel[std::clamp(m_dockSel, 0, kDockCount - 1)];
        const float lw = measured(m_fBody, label).x;
        const nxui::Rect lb{dockX(m_dockSel) - (lw + 36.f) * 0.5f, pill.y - 50.f, lw + 36.f, 36.f};
        frostFast(r, lb, 18.f, nxui::Color(0.07f, 0.08f, 0.11f, 0.5f));
        r.drawRoundedRectOutline(lb, nxui::Color(1.f, 1.f, 1.f, 0.25f), 18.f, 1.5f);
        textMid(r, m_fBody, label, lb.x + 18.f, lb.y + 18.f, kWhite);
    }
    // The footer
    if (m_grid) drawHints(r, 1236.f, 696.f, {{"A", "OK"}, {"B", "Back"}, {"+", "Options"}}, a);
    else drawHints(r, 1236.f, 696.f, {{"A", "OK"}, {"+", "Options"}, {"-", "Most played"}}, a);
}

// ---------------------------------------------------------------- opening and closing a game

// Like the stock Switch: the icon lifts a little, then opens straight out to the whole screen, quickly,
// and only at the very end does the screen go dark for the game.
void LiteActivity::drawLaunch(nxui::Renderer& r) {
    if (!m_leaving) return;
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    const float t = clamp01(m_leaveT);
    const Game* g = m_leaveIndex >= 0 && m_leaveIndex < static_cast<int>(m_list.size()) ? &m_list[(std::size_t)m_leaveIndex] : nullptr;
    if (g) {
        const float lift = easeOut(t / 0.22f);
        const float open = easeOut(clamp01((t - 0.10f) / 0.80f)); // fast at first, easing into the screen
        nxui::Rect from = scaled(m_leaveFrom, 1.f + 0.07f * lift);
        from.y -= 8.f * lift;
        const nxui::Rect rect = lerpRect(from, screen, open);
        const float radius = m_leaveFrom.width * 0.2f * (1.f - open);
        r.drawRoundedRect({rect.x - 8.f, rect.y + 12.f, rect.width + 16.f, rect.height + 16.f}, nxui::Color(0.f, 0.f, 0.f, 0.35f * (1.f - open)), radius + 8.f);
        if (const nxui::Texture* ico = iconOf(g->id)) r.drawTextureRounded(ico, rect, radius, kWhite);
        else r.drawRoundedRect(rect, nxui::Color(0.28f, 0.31f, 0.40f, 1.f), radius);
        r.drawRect(screen, nxui::Color(0.f, 0.f, 0.f, smoothstep(0.62f, 1.f, t)));
    } else {
        r.drawRect(screen, nxui::Color(0.f, 0.f, 0.f, smoothstep(0.1f, 1.f, t))); // nothing to show (Sphaira, power): a fade
    }
    if (m_left) r.drawRect(screen, nxui::Color(0.f, 0.f, 0.f, 1.f));
}

// Back from a game: its last picture shrinks into its icon, quickly, while the others rise in.
void LiteActivity::drawReturn(nxui::Renderer& r) {
    if (!m_returnTex) return;
    const float t = std::min(1.f, m_returnT / kReturnSecs), e = easeOut(t);
    nxui::Rect card{640.f - kSize * 0.5f, kMidY - kSize * 0.5f, kSize, kSize};
    float chosen = 0.f;
    if (!m_list.empty()) cardRect(m_sel, card, chosen);
    const nxui::Rect at{card.x * e, card.y * e, 1280.f + (card.width - 1280.f) * e, 720.f + (card.height - 720.f) * e};
    drawCover(r, m_returnTex.get(), at, card.width * 0.2f * e, kWhite.withAlpha(1.f - easeIn(t)));
}

// ---------------------------------------------------------------- popups

void LiteActivity::drawMessage(nxui::Renderer& r) {
    if (m_messageFor <= 0.f || m_message.empty()) return;
    const float a = std::min(1.f, m_messageFor * 3.f) * easeOut((4.5f - m_messageFor) * 5.f);
    const std::string text = fit(m_fBody, m_message, 900.f);
    const float w = measured(m_fBody, text).x + 44.f;
    const nxui::Rect pill{640.f - w * 0.5f, 520.f + 10.f * (1.f - a), w, 42.f};
    frostFast(r, pill, 21.f, nxui::Color(0.06f, 0.07f, 0.10f, 0.75f * a));
    r.drawRoundedRectOutline(pill, nxui::Color(1.f, 1.f, 1.f, 0.15f * a), 21.f, 1.5f);
    textMid(r, m_fBody, text, pill.x + 22.f, pill.y + 21.f, kWhite.withAlpha(a));
}

void LiteActivity::openMenu(std::string title, std::vector<Item> items) {
    m_menuTitle = std::move(title);
    m_menu = std::move(items);
    m_menuSel = 0;
    m_menuIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
}

void LiteActivity::openOptions() {
    const Game* gp = current();
    if (!gp || gp->action == "allsoftware") return;
    const Game g = *gp;
    const bool running = g.id == runningId();
    std::vector<Item> items;
    items.push_back({running ? "Resume" : g.path.empty() ? "Start" : "Open", [this]() { play(); }, nullptr, nullptr, false, false});
    if (running)
        items.push_back({"Close software", [this]() {
                             m_launcher.terminateApplication();
                             say("Closing it...");
                         }, nullptr, nullptr, true, false});
    items.push_back({"Cancel", [] {}, nullptr, nullptr, false, false});
    openMenu(g.name, std::move(items));
}

void LiteActivity::openPower() {
    std::vector<Item> items;
    items.push_back({"Sleep", [this]() { m_launcher.enterSleep(); }, nullptr, nullptr, false, false});
    items.push_back({"Restart", [this]() { leave(-1, 0.4f, [this]() { m_launcher.reboot(); }); }, nullptr, nullptr, false, false});
    items.push_back({"Shut down", [this]() { leave(-1, 0.4f, [this]() { m_launcher.shutdown(); }); }, nullptr, nullptr, false, false});
    items.push_back({"Reboot to Hekate", [this]() {
                         // Atmosphere sends hbl's reboots to its payload (not HOME's): CardsNX (the .nro) does it, asked by a file.
                         std::error_code ec;
                         const char* cardsNro = "sdmc:/switch/CardsNX/CardsNX.nro";
                         if (std::filesystem::exists(cardsNro, ec) && AppletLauncher::canRebootToPayload() && !AppletLauncher::hekatePayload().empty()) {
                             std::ofstream("sdmc:/config/SwitchU/cardsnx_reboot_hekate") << "1";
                             leave(-1, 0.4f, [this, cardsNro]() {
                                 if (!m_launcher.launchNro(cardsNro, hostTitleId(), launchUid())) {
                                     std::remove("sdmc:/config/SwitchU/cardsnx_reboot_hekate");
                                     comeBack();
                                     say("Hekate didn't start.");
                                 }
                             });
                             return;
                         }
                         if (!AppletLauncher::canRebootToPayload()) { // newer models: the power chip's clock, then a restart
                             leave(-1, 0.45f, [this]() {
                                 if (!m_launcher.rebootToHekate()) {
                                     comeBack();
                                     say("The power chip didn't take the request for Hekate's menu.");
                                 }
                             });
                             return;
                         }
                         if (!m_launcher.rebootToHekate())
                             say(AppletLauncher::hekatePayload().empty() ? "Hekate isn't on the SD card (bootloader/update.bin)."
                                                                         : "Atmosphere didn't take the Hekate payload.");
                     }, nullptr, nullptr, false, false});
    openMenu("Power", std::move(items));
}

void LiteActivity::openSettings() {
    m_brightness = m_quick.brightness >= 0.f ? m_quick.brightness : m_brightness;
    auto onOff = [](bool v) { return std::string(v ? "On" : "Off"); };
    std::vector<Item> items;
    items.push_back({"Brightness", nullptr,
                     [this]() { return std::to_string(static_cast<int>(m_brightness * 100.f + 0.5f)) + "%"; },
                     [this](int d) {
                         m_brightness = std::clamp(m_brightness + 0.1f * static_cast<float>(d), 0.05f, 1.f);
                         cards::setBrightness(m_brightness);
                     }, false, true});
    items.push_back({"Wi-Fi", nullptr, [this, onOff]() { return onOff(m_quick.wifi && !m_quick.airplane); },
                     [this](int) {
                         const bool on = !(m_quick.wifi && !m_quick.airplane);
                         if (cards::setWifi(on)) { m_quick.wifi = on; if (on) m_quick.airplane = false; }
                     }, false, true});
    items.push_back({"Airplane mode", nullptr, [this, onOff]() { return onOff(m_quick.airplane); },
                     [this](int) {
                         if (cards::setAirplaneMode(!m_quick.airplane)) m_quick.airplane = !m_quick.airplane;
                     }, false, true});
    items.push_back({"Lock screen on wake", nullptr, [this, onOff]() { return onOff(m_settings.lockScreen); },
                     [this](int) { m_settings.lockScreen = !m_settings.lockScreen; m_settings.save(); }, false, true});
    items.push_back({"Lock screen password", [this]() { startPasswordSetup(); }, nullptr, nullptr, false, false});
    items.push_back({"Animated background", nullptr, [this, onOff]() { return onOff(m_settings.animBg); },
                     [this](int) { m_settings.animBg = !m_settings.animBg; m_settings.save(); }, false, true});
    items.push_back({"Sounds", nullptr, [this, onOff]() { return onOff(m_settings.sounds); },
                     [this](int) {
                         m_settings.sounds = !m_settings.sounds;
                         m_audio.setSounds(m_settings.sounds, m_settings.soundVolume);
                         m_settings.save();
                     }, false, true});
    items.push_back({"24-hour clock", nullptr, [this, onOff]() { return onOff(m_settings.clock24); },
                     [this](int) { m_settings.clock24 = !m_settings.clock24; m_clock24 = m_settings.clock24; m_settings.save(); }, false, true});
    items.push_back({"Keep homebrew open", nullptr, [this, onOff]() { return onOff(m_settings.keepHomebrew); },
                     [this](int) { m_settings.keepHomebrew = !m_settings.keepHomebrew; syncHomebrewFlag(); m_settings.save(); }, false, true});
    openMenu("Settings", std::move(items));
}

void LiteActivity::updateMenu() {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = static_cast<int>(m_menu.size());
    if ((in.isDown(B::DUp) || in.isDown(B::LStickU)) && m_menuSel > 0) { --m_menuSel; m_audio.play("navigation"); }
    if ((in.isDown(B::DDown) || in.isDown(B::LStickD)) && m_menuSel < n - 1) { ++m_menuSel; m_audio.play("navigation"); }
    Item& item = m_menu[(std::size_t)std::clamp(m_menuSel, 0, n - 1)];
    const int d = (in.isDown(B::DRight) || in.isDown(B::LStickR)) ? 1 : (in.isDown(B::DLeft) || in.isDown(B::LStickL)) ? -1 : 0;
    if (d && item.change) {
        item.change(d);
        m_audio.play("navigation");
    }
    if (in.isDown(B::B) || in.isDown(B::Plus)) {
        m_menu.clear();
        m_audio.play("hide_modal");
        return;
    }
    if (in.isDown(B::A)) {
        if (item.change && !item.run) { // a switch: A flips it too
            item.change(1);
            m_audio.play("toggle_on");
            return;
        }
        auto run = item.run;
        if (!item.stay) m_menu.clear();
        m_audio.play("activation");
        if (run) run();
    }
}

namespace {
void popupBox(std::size_t rows, nxui::Rect& box, float& rowH) {
    rows = std::min<std::size_t>(rows, 9);
    rowH = std::min(56.f, (650.f - 84.f - 44.f) / static_cast<float>(std::max<std::size_t>(1, rows)));
    const float h = 84.f + 44.f + rowH * static_cast<float>(rows);
    box = {(1280.f - 600.f) * 0.5f, (720.f - h) * 0.5f, 600.f, h};
}
} // namespace

void LiteActivity::drawMenu(nxui::Renderer& r) {
    if (m_menu.empty()) return;
    const float a = easeOut(m_menuIn);
    nxui::Rect box;
    float rowH = 56.f;
    popupBox(m_menu.size(), box, rowH);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.45f * a));
    const nxui::Rect card = scaled(box, 0.95f + 0.05f * easeOutBack(m_menuIn));
    frostFast(r, card, 30.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.82f * a));
    r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 30.f, 1.5f);
    const float dy = card.y - box.y;
    textMid(r, m_fTitle, fit(m_fTitle, m_menuTitle, 520.f, 0.75f), box.x + 34.f, box.y + 44.f + dy, kWhite.withAlpha(a), 0.75f);
    const std::size_t shown = std::min<std::size_t>(m_menu.size(), 9);
    const std::size_t top = m_menu.size() <= 9 ? 0 : static_cast<std::size_t>(std::clamp(m_menuSel - 4, 0, static_cast<int>(m_menu.size()) - 9));
    for (std::size_t k = 0; k < shown; ++k) {
        const std::size_t i = top + k;
        const Item& it = m_menu[i];
        const nxui::Rect row{box.x + 18.f, box.y + 84.f + rowH * static_cast<float>(k) + dy, box.width - 36.f, rowH - 6.f};
        const bool chosen = static_cast<int>(i) == m_menuSel;
        if (chosen) {
            r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.14f * a), 18.f);
            r.drawRoundedRectOutline(row, accent((0.8f + 0.2f * std::sin(m_beat * 5.f)) * a), 18.f, 2.5f);
        }
        textMid(r, m_fLabel, it.label, row.x + 20.f, row.y + row.height * 0.5f, (it.danger ? kRed : kWhite).withAlpha(a), 0.9f);
        if (it.value) {
            const std::string v = it.value();
            textMid(r, m_fBody, v, row.x + row.width - 20.f - measured(m_fBody, v).x, row.y + row.height * 0.5f, kMuted.withAlpha(a));
        }
    }
    const std::string hint = "A Choose    B Back";
    textMid(r, m_fBody, hint, box.x + box.width - 34.f - measured(m_fBody, hint).x, box.y + box.height - 24.f + dy, kMuted.withAlpha(0.8f * a), 1.f);
}

// The profile: your most played games, most time first.
void LiteActivity::openStats() {
    m_stats.clear();
    for (const auto* list : {&m_games, &m_homebrew})
        for (const Game& g : *list)
            if (g.action.empty() && g.playSeconds > 0) m_stats.push_back(g);
    std::sort(m_stats.begin(), m_stats.end(), [](const Game& a, const Game& b) { return a.playSeconds > b.playSeconds; });
    if (m_stats.size() > 7) m_stats.resize(7);
    m_statsOpen = true;
    m_statsIn = m_settings.motion ? 0.f : 1.f;
    m_audio.play("show_modal");
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (const Game& g : m_stats)
            if (!m_icons.count(g.id) && !m_iconAsked[g.id]) {
                m_iconAsked[g.id] = true;
                m_iconJobs.push_back(g);
            }
    }
    m_cv.notify_all();
}

void LiteActivity::drawStats(nxui::Renderer& r) {
    if (!m_statsOpen) return;
    const float a = easeOut(m_statsIn);
    const nxui::Rect box{300.f, 90.f, 680.f, 540.f};
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.45f * a));
    const nxui::Rect card = scaled(box, 0.95f + 0.05f * easeOutBack(m_statsIn));
    frostFast(r, card, 32.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.82f * a));
    r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 32.f, 1.5f);
    const float dy = card.y - box.y;
    textMid(r, m_fTitle, "Most played", box.x + 36.f, box.y + 46.f + dy, kWhite.withAlpha(a), 0.8f);
    if (m_stats.empty()) textMid(r, m_fLabel, "Nothing played yet", box.x + 36.f, box.y + 140.f + dy, kMuted.withAlpha(a));
    for (std::size_t i = 0; i < m_stats.size(); ++i) {
        const Game& g = m_stats[i];
        const float ta = a * easeOut((m_statsIn - 0.04f * static_cast<float>(i)) / 0.7f);
        const nxui::Rect row{box.x + 24.f, box.y + 86.f + 58.f * static_cast<float>(i) + dy, box.width - 48.f, 54.f};
        r.drawRoundedRect(row, nxui::Color(1.f, 1.f, 1.f, 0.07f * ta), 16.f);
        const nxui::Rect pic{row.x + 6.f, row.y + 3.f, 48.f, 48.f};
        if (const nxui::Texture* ico = iconOf(g.id)) r.drawTextureRounded(ico, pic, 10.f, kWhite.withAlpha(ta));
        else r.drawRoundedRect(pic, nxui::Color(0.28f, 0.31f, 0.40f, 0.85f * ta), 10.f);
        const std::string t = playTime(g.playSeconds);
        const float tw = measured(m_fLabel, t).x * 0.9f;
        textMid(r, m_fLabel, fit(m_fLabel, g.name, row.width - 80.f - tw - 30.f, 0.9f), row.x + 66.f, row.y + 27.f, kWhite.withAlpha(ta), 0.9f);
        textMid(r, m_fLabel, t, row.x + row.width - 18.f - tw, row.y + 27.f, kBlue.withAlpha(ta), 0.9f);
    }
    const std::string hint = "A Close";
    textMid(r, m_fBody, hint, box.x + box.width - 34.f - measured(m_fBody, hint).x, box.y + box.height - 24.f + dy, kMuted.withAlpha(0.8f * a));
}

// ---------------------------------------------------------------- the album

namespace {
LiteActivity::Pixels readPicture(const std::string& path, int maxW) {
    LiteActivity::Pixels px;
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    if (stbi_uc* p = data.empty() ? nullptr : cards::decodePicture(data.data(), static_cast<int>(data.size()), w, h)) {
        px.rgba.assign(p, p + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        stbi_image_free(p);
        cards::scaleDown(px.rgba, w, h, maxW);
        px.w = w;
        px.h = h;
    }
    return px;
}
} // namespace

void LiteActivity::showGalleryPage(int page) {
    const int n = m_galAll ? static_cast<int>(m_galAll->size()) : 0;
    page = std::clamp(page, 0, std::max(0, (n - 1) / kGalleryPage));
    if (page == m_galPage || m_galJob.valid()) return;
    m_galPage = page;
    m_galSel = 0;
    auto all = m_galAll;
    m_galJob = std::async(std::launch::async, [all, page]() {
        offScreenCore();
        std::vector<Tile> out;
        for (std::size_t i = static_cast<std::size_t>(page) * kGalleryPage; i < all->size() && i < static_cast<std::size_t>(page + 1) * kGalleryPage; ++i) {
            const std::string& file = (*all)[i];
            const bool video = isVideoFile(file);
            Tile t;
            t.file = file;
            if (video) {
                t.rgba = videoTile(320, 180);
                t.w = 320;
                t.h = 180;
            } else {
                Pixels px = readPicture(file, 320);
                t.rgba = std::move(px.rgba);
                t.w = px.w;
                t.h = px.h;
            }
            if (t.rgba.empty()) continue;
            const std::string n2 = std::filesystem::path(file).filename().string(); // YYYYMMDDhhmmss...
            if (n2.size() >= 12) t.label = n2.substr(6, 2) + "/" + n2.substr(4, 2) + " " + n2.substr(8, 2) + ":" + n2.substr(10, 2);
            if (video) t.label += t.label.empty() ? "Video" : "  Video";
            out.push_back(std::move(t));
        }
        cards::threadDone();
        return out;
    });
    m_audio.play("navigation");
}

void LiteActivity::openGallery() {
    if (m_galJob.valid()) m_galJob.wait();
    m_galOpen = true;
    m_galView = false;
    m_galBig.reset();
    m_galTiles.clear();
    m_galTex.clear();
    m_galSel = 0;
    m_galPage = -1;
    m_galIn = m_settings.motion ? 0.f : 1.f;
    m_galAll = std::make_shared<std::vector<std::string>>();
    auto all = m_galAll;
    m_galJob = std::async(std::launch::async, [all]() {
        offScreenCore();
        *all = switchu::lockpicture::newestScreenshots(100000, true); // all of them, videos too
        cards::threadDone();
        return std::vector<Tile>{};
    });
    m_audio.play("show_modal");
}

void LiteActivity::closeGallery() {
    if (m_galJob.valid()) m_galJob.wait();
    if (m_galBigJob.valid()) m_galBigJob.wait();
    app().gpu().waitIdle();
    m_galOpen = false;
    m_galView = false;
    m_galTex.clear();
    m_galTiles.clear();
    m_galBig.reset();
    m_galAll.reset();
}

void LiteActivity::loadGalleryPicture() {
    if (!m_galAll || m_galAt < 0 || m_galAt >= static_cast<int>(m_galAll->size())) return;
    if (m_galBigJob.valid()) m_galBigJob.wait();
    const std::string path = (*m_galAll)[(std::size_t)m_galAt];
    m_galBigJob = std::async(std::launch::async, [path]() {
        offScreenCore();
        Pixels px;
        if (isVideoFile(path)) { // (no player here: the big play button stands for it)
            px.rgba = videoTile(1280, 720);
            px.w = 1280;
            px.h = 720;
        } else {
            px = readPicture(path, 1280);
        }
        cards::threadDone();
        return px;
    });
}

void LiteActivity::updateGallery() {
    auto& in = app().input();
    using B = nxui::Button;
    const int n = m_galAll ? static_cast<int>(m_galAll->size()) : 0;
    if (m_galView) { // one picture, full screen: left / right
        int at = m_galAt;
        if (in.isDown(B::DLeft) || in.isDown(B::LStickL)) at = std::max(0, at - 1);
        if (in.isDown(B::DRight) || in.isDown(B::LStickR)) at = std::min(n - 1, at + 1);
        if (at != m_galAt) {
            m_galAt = at;
            loadGalleryPicture();
            m_audio.play("navigation");
        }
        if (in.isDown(B::Y) || (in.isDown(B::A) && m_galAll && m_galAt < n && isVideoFile((*m_galAll)[(std::size_t)m_galAt]))) {
            openRealAlbum();
            return;
        }
        if (in.isDown(B::B) || in.isDown(B::A)) {
            m_galView = false;
            m_audio.play("hide_modal");
        }
        return;
    }
    const int count = static_cast<int>(m_galTiles.size());
    if (in.isDown(B::DLeft) || in.isDown(B::LStickL)) m_galSel = std::max(0, m_galSel - 1);
    if (in.isDown(B::DRight) || in.isDown(B::LStickR)) m_galSel = std::min(std::max(0, count - 1), m_galSel + 1);
    if (in.isDown(B::DUp) || in.isDown(B::LStickU)) {
        if (m_galSel >= 4) m_galSel -= 4;
        else showGalleryPage(m_galPage - 1);
    }
    if (in.isDown(B::DDown) || in.isDown(B::LStickD)) {
        if (m_galSel + 4 < count) m_galSel += 4;
        else showGalleryPage(m_galPage + 1);
    }
    if (in.isDown(B::L) || in.isDown(B::ZL)) showGalleryPage(m_galPage - 1);
    if (in.isDown(B::R) || in.isDown(B::ZR)) showGalleryPage(m_galPage + 1);
    if (in.isDown(B::Y)) {
        openRealAlbum();
        return;
    }
    if (in.isDown(B::B)) {
        closeGallery();
        m_audio.play("hide_modal");
        return;
    }
    if (in.isDown(B::A) && count > 0) {
        const std::string file = m_galTiles[(std::size_t)std::min(m_galSel, count - 1)].file;
        m_galAt = 0;
        for (std::size_t i = 0; m_galAll && i < m_galAll->size(); ++i)
            if ((*m_galAll)[i] == file) m_galAt = static_cast<int>(i);
        if (isVideoFile(file)) { // HOME has no video decoder: the real Album plays it
            openRealAlbum();
            return;
        }
        m_galView = true;
        m_galBig.reset();
        loadGalleryPicture();
        m_audio.play("activation");
    }
}

void LiteActivity::drawGallery(nxui::Renderer& r) {
    if (!m_galOpen) return;
    // The list of files arrived: the first page is read; a page arrived: its pictures go up.
    if (m_galJob.valid() && m_galJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::vector<Tile> tiles = m_galJob.get();
        if (m_galPage < 0) { // that was the list of files
            m_galTiles.clear();
            showGalleryPage(0); // (m_galPage is -1, so page 0 is read)
        } else {
            m_galTex.clear();
            for (Tile& t : tiles) {
                auto tex = std::make_unique<nxui::Texture>();
                if (t.rgba.empty() || !tex->loadFromPixels(app().gpu(), app().renderer(), t.rgba.data(), t.w, t.h)) tex.reset();
                m_galTex.push_back(std::move(tex));
                t.rgba.clear();
                t.rgba.shrink_to_fit();
            }
            m_galTiles = std::move(tiles);
            m_galSel = std::min(m_galSel, std::max(0, static_cast<int>(m_galTiles.size()) - 1));
        }
    }
    if (m_galBigJob.valid() && m_galBigJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Pixels px = m_galBigJob.get();
        auto tex = std::make_unique<nxui::Texture>();
        if (!px.rgba.empty() && tex->loadFromPixels(app().gpu(), app().renderer(), px.rgba.data(), px.w, px.h)) m_galBig = std::move(tex);
    }
    if (m_galView) {
        r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 1.f));
        if (m_galBig && m_galBig->valid()) {
            const float tw = static_cast<float>(m_galBig->width()), th = static_cast<float>(m_galBig->height());
            const float s = std::min(1280.f / tw, 720.f / th);
            r.drawTexture(m_galBig.get(), {(1280.f - tw * s) * 0.5f, (720.f - th * s) * 0.5f, tw * s, th * s});
        }
        r.drawGradientRect({0.f, 620.f, 1280.f, 100.f}, nxui::Color(0.f, 0.f, 0.f, 0.f), nxui::Color(0.f, 0.f, 0.f, 0.7f));
        const int n = m_galAll ? static_cast<int>(m_galAll->size()) : 0;
        textMid(r, m_fBody, std::to_string(m_galAt + 1) + " of " + std::to_string(n), 40.f, 684.f, kMuted);
        const std::string hint = "Left / Right more    A / Y Play    B Back";
        textMid(r, m_fBody, hint, 1240.f - measured(m_fBody, hint).x, 684.f, kWhite.withAlpha(0.9f));
        return;
    }
    const float a = easeOut(m_galIn);
    r.drawRect({0.f, 0.f, 1280.f, 720.f}, nxui::Color(0.f, 0.f, 0.f, 0.5f * a));
    const nxui::Rect card = scaled(kGalCard, 0.95f + 0.05f * easeOutBack(m_galIn));
    frostFast(r, card, 32.f, nxui::Color(0.08f, 0.09f, 0.13f, 0.86f * a));
    r.drawRoundedRectOutline(card, nxui::Color(1.f, 1.f, 1.f, 0.16f * a), 32.f, 1.5f);
    const float dy = card.y - kGalCard.y;
    textMid(r, m_fTitle, "Album", kGalCard.x + 40.f, kGalCard.y + 46.f + dy, kWhite.withAlpha(a), 0.8f);
    const int shots = m_galAll ? static_cast<int>(m_galAll->size()) : 0;
    const std::string info = std::to_string(shots) + " captures, page " + std::to_string(std::max(0, m_galPage) + 1) + " of "
                           + std::to_string(std::max(1, (shots + kGalleryPage - 1) / kGalleryPage));
    textMid(r, m_fBody, info, kGalCard.x + 160.f, kGalCard.y + 48.f + dy, kMuted.withAlpha(a));
    const std::string hint = "A View / Play    Y Real Album    L R More    B Back";
    textMid(r, m_fBody, hint, kGalCard.x + kGalCard.width - 40.f - measured(m_fBody, hint).x, kGalCard.y + 48.f + dy, kMuted.withAlpha(0.8f * a));
    if (m_galJob.valid()) {
        const std::string looking = "Opening your captures...";
        textMid(r, m_fLabel, looking, 640.f - measured(m_fLabel, looking).x * 0.5f, kGalCard.y + 300.f + dy, kMuted.withAlpha(a));
        return;
    }
    if (m_galTiles.empty()) {
        const std::string none = "No captures yet: press the Capture button.";
        textMid(r, m_fBody, none, 640.f - measured(m_fBody, none).x * 0.5f, kGalCard.y + 300.f + dy, kMuted.withAlpha(a));
        return;
    }
    for (int i = 0; i < static_cast<int>(m_galTiles.size()); ++i) {
        const float ta = a * easeOut((m_galIn - 0.02f * static_cast<float>(i)) / 0.7f);
        nxui::Rect tile = galTile(i);
        tile.y += dy;
        const bool chosen = i == m_galSel;
        if (chosen) tile = scaled(tile, 1.05f);
        if (static_cast<std::size_t>(i) < m_galTex.size() && m_galTex[(std::size_t)i] && m_galTex[(std::size_t)i]->valid())
            drawCover(r, m_galTex[(std::size_t)i].get(), tile, 14.f, kWhite.withAlpha(ta));
        else r.drawRoundedRect(tile, nxui::Color(1.f, 1.f, 1.f, 0.08f * ta), 14.f);
        r.drawGradientRect({tile.x, tile.y + tile.height - 34.f, tile.width, 34.f}, nxui::Color(0.f, 0.f, 0.f, 0.f), nxui::Color(0.f, 0.f, 0.f, 0.55f * ta));
        textMid(r, m_fBody, m_galTiles[(std::size_t)i].label, tile.x + 10.f, tile.y + tile.height - 13.f, kWhite.withAlpha(ta), 0.8f);
        r.drawRoundedRectOutline(tile, chosen ? accent((0.8f + 0.2f * std::sin(m_beat * 5.f)) * ta) : nxui::Color(1.f, 1.f, 1.f, 0.12f * ta), 14.f, chosen ? 3.f : 1.f);
    }
}


// ---------------------------------------------------------------- videos: Nintendo's own Album
//
// HOME has no video decoder, so a video plays in the real Album, which the daemon opens (and starts HOME again
// when it is closed). Atmosphere starts the homebrew loader in place of the Album, though, unless
// override_config.ini says otherwise: the same test as the daemon's.
namespace {
constexpr const char* kOverrideIni = "sdmc:/atmosphere/config/override_config.ini";
constexpr const char* kOverrideBackup = "sdmc:/atmosphere/config/override_config.ini.litenx-backup";
constexpr const char* kUnusedProgram = "0100000000001fff"; // a program nobody starts: the loader has nothing to put in its place

std::string squash(const std::string& line) {
    std::string s;
    for (char c : line)
        if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isAlbumProgram(const std::string& v) { return v == "010000000000100d" || v == "0x010000000000100d"; }

bool albumOpensHomebrew() {
    std::ifstream f(kOverrideIni);
    if (!f) return true; // Atmosphere's default
    std::map<std::string, std::string> programs;
    bool inHbl = false;
    std::string line;
    while (std::getline(f, line)) {
        const std::string s = squash(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            inHbl = s == "[hbl_config]";
            continue;
        }
        const auto eq = s.find('=');
        if (!inHbl || eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        for (const char* base : {"program_id", "title_id"})
            if (k.rfind(base, 0) == 0) programs[k.substr(std::strlen(base))] = v;
    }
    if (programs.empty()) return true;
    for (const auto& kv : programs)
        if (isAlbumProgram(kv.second)) return true; // (with or without a key: the Album stays out of reach of a button press)
    return false;
}
} // namespace

void LiteActivity::setUpRealAlbum(bool on) {
    std::error_code ec;
    if (!on) { // the file as it was
        if (!std::filesystem::exists(kOverrideBackup, ec)) {
            say("There's nothing to put back.");
            return;
        }
        std::filesystem::copy_file(kOverrideBackup, kOverrideIni, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) std::filesystem::remove(kOverrideBackup, ec);
        say(ec ? "Couldn't put override_config.ini back." : "override_config.ini is back as it was: restart the console.");
        return;
    }
    std::vector<std::string> lines;
    const bool existed = std::filesystem::exists(kOverrideIni, ec);
    if (existed) {
        std::ifstream in(kOverrideIni);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        if (!std::filesystem::exists(kOverrideBackup, ec)) // (the first file is the one kept)
            std::filesystem::copy_file(kOverrideIni, kOverrideBackup, ec);
    }
    bool inHbl = false, changed = false, sawHbl = false;
    std::size_t hblEnd = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string s = squash(lines[i]);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            if (inHbl) hblEnd = i;
            inHbl = s == "[hbl_config]";
            sawHbl = sawHbl || inHbl;
            continue;
        }
        const auto eq = s.find('=');
        if (!inHbl || eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq);
        if ((k.rfind("program_id", 0) == 0 || k.rfind("title_id", 0) == 0) && isAlbumProgram(s.substr(eq + 1))) {
            lines[i] = lines[i].substr(0, lines[i].find('=') + 1) + kUnusedProgram;
            changed = true;
        }
    }
    if (!changed) { // nothing names the Album yet: the default entry is replaced by one for another program
        if (!sawHbl) {
            lines.push_back("[hbl_config]");
            hblEnd = lines.size();
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(std::min(hblEnd, lines.size())),
                     std::string("program_id_0=") + kUnusedProgram);
    }
    std::ofstream out(kOverrideIni, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
    out.close();
    say(out ? "Done: restart the console once, then videos play in the Album."
            : "Couldn't write override_config.ini on the SD card.");
}

void LiteActivity::openRealAlbum() {
    if (m_galOpen) closeGallery(); // (the album's picture goes away first; its popup or the launch follows)
    if (albumOpensHomebrew()) {
        std::vector<Item> items;
        items.push_back({"Let the Album play videos", [this]() { setUpRealAlbum(true); }, nullptr, nullptr, false, false});
        std::error_code ec;
        if (std::filesystem::exists(kOverrideBackup, ec))
            items.push_back({"Put override_config.ini back", [this]() { setUpRealAlbum(false); }, nullptr, nullptr, false, false});
        items.push_back({"Not now", []() {}, nullptr, nullptr, false, false});
        openMenu("Atmosphere opens Homebrew in place of the Album", std::move(items));
        return;
    }
    leave(-1, 0.3f, [this]() { m_launcher.launchAlbum(); }); // the daemon opens it and starts HOME again
}

// ---------------------------------------------------------------- the lock screen

void LiteActivity::loadLockPicture() {
    const std::string path = m_lockPicture;
    if (path == m_lockTexFor) return; // asked for already
    m_lockTexFor = path;
    if (path.empty()) {
        if (m_lockTex) app().gpu().waitIdle(); // it may still be drawing
        m_lockTex.reset();
        return;
    }
    if (m_lockJob.valid()) m_lockJob.wait();
    m_lockJob = std::async(std::launch::async, [path]() {
        offScreenCore();
        Pixels px = readPicture(path, 1280);
        cards::threadDone();
        return px;
    });
}

void LiteActivity::lock() {
    ensureLockFonts(); // (normally loaded already, while HOME was idle)
    closeEverything();
    if (!m_locked) switchu::lockpicture::next(armGetSystemTick());
    m_lockGameId = 0;
    m_lockGameName.clear();
    const std::uint64_t running = runningId();
    if (running && m_settings.lockGameBanner) {
        const std::uint64_t last = std::strtoull(m_settings.lastGame.c_str(), nullptr, 16);
        for (const auto* list : {&m_games, &m_homebrew})
            for (const Game& g : *list)
                if (g.id == running || (!g.path.empty() && g.id == last && (running >> 56) == 0xFF)) {
                    m_lockGameId = g.id;
                    m_lockGameName = g.name;
                }
    }
    if (running && !m_lockShot && !m_inSleepLock) { // the game's last frame stays behind the frosted glass
        std::vector<std::uint8_t> px(1280u * 720u * 4u);
        bool ok = false;
        if (R_SUCCEEDED(appletGetLastApplicationCaptureImageEx(px.data(), px.size(), &ok)) && ok) {
            for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 0xFF; // games leave alpha undefined
            auto tex = std::make_unique<nxui::Texture>();
            if (tex->loadFromPixels(app().gpu(), app().renderer(), px.data(), 1280, 720)) m_lockShot = std::move(tex);
        }
    }
    m_lockShotBlur = false;
    m_lockBlurOk = false;
    m_blurAge = 1000; // a fresh blurred copy of HOME at once
    m_locked = true;
    m_lockIn = m_settings.motion ? 0.f : 1.f;
    m_unlockT = -1.f;
    m_lockIdle = 0.f;
    m_lockPicture = switchu::lockpicture::get();
    loadLockPicture();
    m_pwSet = switchu::lockpw::isSet();
    m_pwSetup = false;
    m_pw.startUnlock();
}

void LiteActivity::beginUnlock() { m_unlockT = 0.f; }

void LiteActivity::startPasswordSetup() {
    lock();
    m_pwSetup = true;
    m_pw.startMaking();
}

bool LiteActivity::updateLock(float dt) {
    if (!m_locked) return false;
    if (m_lockJob.valid() && m_lockJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Pixels px = m_lockJob.get();
        auto tex = std::make_unique<nxui::Texture>();
        if (!px.rgba.empty() && tex->loadFromPixels(app().gpu(), app().renderer(), px.rgba.data(), px.w, px.h)) m_lockTex = std::move(tex);
    }
    m_lockIn = std::min(1.f, m_lockIn + dt / 0.6f);
    if (m_unlockT >= 0.f) {
        m_unlockT += m_settings.motion ? dt / 0.35f : 1.f;
        if (m_unlockT >= 1.f) {
            m_locked = false;
            m_unlockT = -1.f;
            if (m_lockShot) {
                app().gpu().waitIdle(); // it may still be drawing
                m_lockShot.reset();
            }
        }
        return true;
    }
    auto& in = app().input();
    using B = nxui::Button;
    // Locked and ignored: standby after 15 s. Waking comes back to this same lock screen.
    bool activity = false;
    for (B b : {B::A, B::B, B::X, B::Y, B::L, B::R, B::ZL, B::ZR, B::Plus, B::Minus, B::DUp, B::DDown, B::DLeft, B::DRight,
                B::LStickU, B::LStickD, B::LStickL, B::LStickR, B::RStickU, B::RStickD, B::RStickL, B::RStickR})
        activity |= in.isDown(b);
    activity |= in.touchUp() || in.touchDown();
    if (activity) m_lockIdle = 0.f; else m_lockIdle += dt;
    if (m_lockIdle > 15.f && m_lockIn >= 1.f) {
        m_lockIdle = -1e9f; // once; waking lands here again
        m_launcher.enterSleep();
        return true;
    }
    m_pw.update(dt);
    if (m_pwSet || m_pwSetup) { // the password: D-pad presses (either stick too)
        using E = switchu::lockpw::Entry;
        int key = -1;
        if (in.isDown(B::DUp) || in.isDown(B::LStickU)) key = 0;
        else if (in.isDown(B::DDown) || in.isDown(B::LStickD)) key = 1;
        else if (in.isDown(B::DLeft) || in.isDown(B::LStickL)) key = 2;
        else if (in.isDown(B::DRight) || in.isDown(B::LStickR)) key = 3;
        E::Result res = E::None;
        if (key >= 0 && m_lockIn > 0.25f) {
            res = m_pw.press(switchu::lockpw::directionOf(key));
            m_audio.play("navigation");
        } else if (in.isDown(B::A) && m_pwSetup) {
            res = m_pw.confirm();
            if (res == E::None) m_audio.play("activation");
        } else if (in.isDown(B::B)) {
            if (m_pwSetup && m_pw.presses.empty()) { // with nothing pressed: stop making one
                m_pwSetup = false;
                beginUnlock();
                m_audio.play("hide_modal");
                return true;
            }
            m_pw.presses.clear();
            m_audio.play("hide_modal");
        }
        if (res == E::Unlocked) {
            beginUnlock();
            m_audio.play("activation");
        } else if (res == E::Made) {
            m_pwSetup = false;
            m_pwSet = true;
            beginUnlock();
            if (!m_settings.lockScreen) { // a password needs the lock screen
                m_settings.lockScreen = true;
                m_settings.save();
            }
            m_audio.play("activation");
            say("Password set: the lock screen asks for it.");
        } else if (res == E::Wrong || res == E::Mismatch || res == E::TooShort) {
            m_audio.play("toggle_off");
        }
        return true;
    }
    bool pressed = in.touchUp();
    for (B b : {B::A, B::B, B::X, B::Y, B::L, B::R, B::ZL, B::ZR, B::Plus, B::Minus, B::DUp, B::DDown, B::DLeft, B::DRight})
        pressed |= in.isDown(b);
    if (pressed && m_lockIn > 0.25f) {
        beginUnlock();
        m_audio.play("activation");
    }
    return true;
}

void LiteActivity::drawLock(nxui::Renderer& r) {
    if (!m_locked) return;
    const float in = easeOut(m_lockIn);
    const float out = m_unlockT < 0.f ? 0.f : easeInOut(std::min(1.f, m_unlockT));
    const float a = in * (1.f - out), lift = -110.f * out;
    const nxui::Rect screen{0.f, 0.f, 1280.f, 720.f};
    if (m_lockShot && m_lockShot->valid()) r.drawTexture(m_lockShot.get(), screen, kWhite.withAlpha(a)); // the game behind
    if (m_lockTex && m_lockTex->valid() && !m_lockShot) { // its own picture, sharp; a little shade keeps the clock readable
        drawCover(r, m_lockTex.get(), screen, 0.f, kWhite.withAlpha(a));
        r.drawGradientRect(screen, nxui::Color(0.f, 0.f, 0.f, 0.18f * a), nxui::Color(0.f, 0.f, 0.f, 0.45f * a));
    } else {
        frostLive(r, screen, 0.f, nxui::Color(0.03f, 0.04f, 0.07f, 0.5f * a));
        r.drawGradientRect(screen, nxui::Color(0.f, 0.f, 0.f, 0.10f * a), nxui::Color(0.f, 0.f, 0.f, 0.55f * a));
    }
    const float rise = 18.f * (1.f - in);
    textMid(r, m_fLock, m_clock, 640.f - measured(m_fLock, m_clock).x * 0.5f, 268.f + rise + lift, kWhite.withAlpha(a));
    textMid(r, m_fLockDate, m_date, 640.f - measured(m_fLockDate, m_date).x * 0.5f, 350.f + rise + lift, kMuted.withAlpha(a));
    if (m_lockGameId && m_settings.lockGameBanner) {
        // A small round icon of the game under the date, with a ring and its name.
        const float d = 64.f, cy = 452.f + lift;
        const nxui::Rect ring{640.f - d * 0.5f, cy - d * 0.5f, d, d};
        frostLive(r, {ring.x - 6.f, ring.y - 6.f, d + 12.f, d + 12.f}, d * 0.5f + 6.f, nxui::Color(0.07f, 0.08f, 0.11f, 0.5f * a));
        if (const nxui::Texture* ico = iconOf(m_lockGameId)) r.drawTextureRounded(ico, ring, d * 0.5f, kWhite.withAlpha(a));
        else r.drawCircle({640.f, cy}, d * 0.5f, nxui::Color(1.f, 1.f, 1.f, 0.25f * a), 32);
        r.drawRoundedRectOutline({ring.x - 6.f, ring.y - 6.f, d + 12.f, d + 12.f}, nxui::Color(1.f, 1.f, 1.f, 0.35f * a), d * 0.5f + 6.f, 1.5f);
        if (!(m_pwSet || m_pwSetup)) {
            const std::string nm = fit(m_fBody, m_lockGameName, 360.f, 0.85f);
            textMid(r, m_fBody, nm, 640.f - measured(m_fBody, nm).x * 0.425f, cy + 56.f, kMuted.withAlpha(a), 0.85f);
        }
    }
    drawStatus(r, a, 54.f + lift, false); // (the same icons as HOME's, in the same place)
    if (m_pwSet || m_pwSetup) { // the password's dots (arrows while making it)
        const std::string prompt = m_pw.prompt();
        textMid(r, m_fLabel, prompt, 640.f - measured(m_fLabel, prompt).x * 0.5f, 440.f + lift, (m_pw.wrong > 0.f ? kRed : kWhite).withAlpha(a));
        switchu::lockpw::drawDots(r, m_pw, 500.f + lift, a, m_time);
        const std::string hint = m_pwSetup ? "A Done    B Clear (again to stop)" : "B Clear";
        textMid(r, m_fBody, hint, 640.f - measured(m_fBody, hint).x * 0.5f, 560.f + lift, kMuted.withAlpha(a));
        return;
    }
    // The way in, breathing.
    const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(m_beat * 2.4f));
    const std::string hint = "Press any button";
    r.drawRoundedRect({640.f - 56.f, 612.f + lift, 112.f, 5.f}, nxui::Color(1.f, 1.f, 1.f, 0.55f * a * pulse), 2.5f);
    textMid(r, m_fLabel, hint, 640.f - measured(m_fLabel, hint).x * 0.5f, 648.f + lift, kWhite.withAlpha(0.85f * a * pulse));
}

// ---------------------------------------------------------------- touch

// A tap: the profile, a dock button, an icon, a popup's row, a captured picture.
bool LiteActivity::handleTouch() {
    auto& in = app().input();
    if (!in.touchUp()) return in.isTouching();
    const float x = in.touchX(), y = in.touchY();
    if (m_galOpen) {
        if (m_galView) { m_galView = false; return true; }
        for (int i = 0; i < static_cast<int>(m_galTiles.size()); ++i)
            if (galTile(i).contains(x, y)) {
                m_galSel = i;
                const std::string file = m_galTiles[(std::size_t)i].file;
                for (std::size_t k = 0; m_galAll && k < m_galAll->size(); ++k)
                    if ((*m_galAll)[k] == file) m_galAt = static_cast<int>(k);
                m_galView = true;
                m_galBig.reset();
                loadGalleryPicture();
                return true;
            }
        if (!kGalCard.contains(x, y)) closeGallery();
        return true;
    }
    if (!m_menu.empty()) {
        nxui::Rect box;
        float rowH = 56.f;
        popupBox(m_menu.size(), box, rowH);
        const std::size_t top = m_menu.size() <= 9 ? 0 : static_cast<std::size_t>(std::clamp(m_menuSel - 4, 0, static_cast<int>(m_menu.size()) - 9));
        for (std::size_t k = 0; k < std::min<std::size_t>(m_menu.size(), 9); ++k) {
            const nxui::Rect row{box.x + 18.f, box.y + 84.f + rowH * static_cast<float>(k), box.width - 36.f, rowH - 6.f};
            if (!row.contains(x, y)) continue;
            m_menuSel = static_cast<int>(top + k);
            Item& item = m_menu[top + k];
            if (item.change && !item.run) {
                item.change(1);
                return true;
            }
            auto run = item.run;
            if (!item.stay) m_menu.clear();
            if (run) run();
            return true;
        }
        if (!box.contains(x, y)) m_menu.clear();
        return true;
    }
    if (m_statsOpen) {
        m_statsOpen = false;
        return true;
    }
    if (m_locked || m_leaving) return true;
    if (kAvatar.contains(x, y)) {
        openStats();
        return true;
    }
    if (y > kDock.y - 6.f && y < kDock.y + kDock.height + 6.f)
        for (int i = 0; i < kDockCount; ++i)
            if (std::abs(x - dockX(i)) < 44.f) {
                m_zone = Zone::Bottom;
                m_dockSel = i;
                activateDock(i);
                return true;
            }
    for (int i = 0; i < static_cast<int>(m_list.size()); ++i) {
        nxui::Rect card;
        float t = 0.f;
        if (!cardRect(i, card, t) || !card.contains(x, y)) continue;
        m_zone = Zone::Cards;
        if (i == m_sel) {
            m_pressIndex = i;
            m_pressT = 0.f;
            play();
        } else {
            select(i);
        }
        return true;
    }
    return false;
}

} // namespace lite

#endif // SWITCHU_MENU
