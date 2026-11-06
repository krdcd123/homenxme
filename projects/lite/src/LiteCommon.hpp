#pragma once
// Small drawing helpers and the layout of LiteNX.
#include <nxui/core/Font.hpp>
#include <nxui/core/Renderer.hpp>
#include <nxui/core/Texture.hpp>
#include <nxui/core/ThreadPool.hpp>
#include <switch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace lite::ui {

// The layout: the stock Switch's, on 1280x720.
inline constexpr float kSize = 224.f, kGrow = 44.f, kPitch = 254.f, kFirstX = 108.f, kMidY = 322.f;
inline constexpr int kDockCount = 5; // Sphaira, Album, Controllers, Settings, Power
inline const nxui::Rect kDock{390.f, 586.f, 500.f, 84.f};
inline float dockX(int i) { return kDock.x + 54.f + 98.f * static_cast<float>(i); }
inline float dockY() { return kDock.y + kDock.height * 0.5f; }
inline const nxui::Rect kAvatar{48.f, 28.f, 52.f, 52.f};
// All Software: a grid, three rows in view, scrolling by row.
inline constexpr int kGridCols = 7;
inline constexpr float kGridSize = 120.f, kGridPitchX = 164.f, kGridPitchY = 150.f, kGridX = 88.f, kGridY = 150.f;
inline constexpr float kReturnSecs = 0.30f; // the game's picture shrinking into its card
inline constexpr std::uint64_t kAllSoftwareId = 0xFE00AAAAAAAAAAAAULL;

inline const nxui::Color kWhite(1.f, 1.f, 1.f, 1.f);
inline const nxui::Color kMuted(1.f, 1.f, 1.f, 0.82f);
inline const nxui::Color kBlue(0.36f, 0.78f, 1.f, 1.f);
inline const nxui::Color kGreen(0.204f, 0.78f, 0.349f, 1.f);
inline const nxui::Color kRed(1.f, 0.231f, 0.188f, 1.f);

struct Accent {
    const char* name;
    nxui::Color color;
};
inline const Accent kAccents[] = {
    {"White", nxui::Color(1.f, 1.f, 1.f, 1.f)},       {"Blue", nxui::Color(0.36f, 0.68f, 1.f, 1.f)},
    {"Green", nxui::Color(0.30f, 0.86f, 0.47f, 1.f)}, {"Pink", nxui::Color(1.f, 0.46f, 0.72f, 1.f)},
    {"Orange", nxui::Color(1.f, 0.63f, 0.26f, 1.f)},  {"Purple", nxui::Color(0.72f, 0.55f, 1.f, 1.f)},
    {"Gold", nxui::Color(1.f, 0.84f, 0.36f, 1.f)},
};
inline constexpr int kAccentCount = sizeof(kAccents) / sizeof(kAccents[0]);

inline float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
inline float easeOut(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
inline float easeIn(float t) { t = clamp01(t); return t * t * t; }
inline float easeInOut(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;
}
inline float smoothstep(float a, float b, float x) {
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}
inline float easeOutBack(float t) { // a small overshoot, then settles
    t = clamp01(t);
    const float c1 = 1.4f, c3 = c1 + 1.f;
    return 1.f + c3 * std::pow(t - 1.f, 3.f) + c1 * std::pow(t - 1.f, 2.f);
}
inline nxui::Rect lerpRect(const nxui::Rect& a, const nxui::Rect& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.width + (b.width - a.width) * t, a.height + (b.height - a.height) * t};
}
inline nxui::Rect scaled(const nxui::Rect& r, float s) {
    const float w = r.width * s, h = r.height * s;
    return {r.x + (r.width - w) * 0.5f, r.y + (r.height - h) * 0.5f, w, h};
}

// Text sizes, measured once: measuring shapes the text again.
inline nxui::Vec2 measured(nxui::Font& font, const std::string& s) {
    static std::unordered_map<std::string, nxui::Vec2> cache;
    const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 4000) cache.clear();
    return cache[key] = font.measure(s);
}

// As much of `text` as fits in maxW (cut between letters, "..." added), remembered.
inline std::string fit(nxui::Font& font, const std::string& text, float maxW, float scale = 1.f) {
    maxW = std::floor(maxW / 16.f) * 16.f;
    static std::unordered_map<std::string, std::string> cache;
    const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&font)) + '|' + std::to_string(static_cast<int>(maxW))
                          + '|' + std::to_string(static_cast<int>(scale * 100.f)) + '|' + text;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 2000) cache.clear();
    if (measured(font, text).x * scale <= maxW) return cache[key] = text;
    std::vector<std::size_t> cuts; // where letters start
    for (std::size_t i = 1; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);
    std::size_t lo = 0, hi = cuts.size(); // the longest start that fits
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (measured(font, text.substr(0, cuts[mid - 1]) + "...").x * scale <= maxW) lo = mid;
        else hi = mid - 1;
    }
    return cache[key] = (lo == 0 ? std::string() : text.substr(0, cuts[lo - 1])) + "...";
}

// Text drawn with its middle at y.
inline void textMid(nxui::Renderer& r, nxui::Font& font, const std::string& s, float x, float y, const nxui::Color& c, float scale = 1.f) {
    r.drawText(s, {x, y - measured(font, s.empty() ? std::string("A") : s).y * scale * 0.5f}, &font, c, scale);
}

// A picture drawn to fill `dst`, cropped to its shape.
inline void drawCover(nxui::Renderer& r, const nxui::Texture* tex, const nxui::Rect& dst, float radius, const nxui::Color& tint) {
    const float tw = static_cast<float>(tex->width()), th = static_cast<float>(tex->height());
    const float scale = std::max(dst.width / tw, dst.height / th);
    const float sw = dst.width / scale, sh = dst.height / scale;
    const nxui::Rect src{(tw - sw) * 0.5f, (th - sh) * 0.5f, sw, sh};
    if (radius > 0.f) r.drawTextureSubRounded(tex, src, dst, radius, tint);
    else r.drawTextureSub(tex, src, dst, tint);
}

// A background job's thread: off the screen's core and below it.
inline void offScreenCore() {
    nxui::moveToWorkerCores();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string playTime(std::uint64_t seconds) {
    const std::uint64_t m = seconds / 60, h = m / 60;
    if (m == 0) return seconds ? "<1m" : "0m";
    if (h == 0) return std::to_string(m) + "m";
    if (h >= 100 || m % 60 == 0) return std::to_string(h) + "h";
    return std::to_string(h) + "h " + std::to_string(m % 60) + "m";
}

inline std::string lastPlayed(std::uint64_t at) {
    if (at == 0) return "Never played";
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t s = now > at ? now - at : 0;
    auto ago = [](std::uint64_t n, const char* unit) { return "Played " + std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };
    if (s < 60) return "Played just now";
    if (s < 3600) return ago(s / 60, "minute");
    if (s < 86400) return ago(s / 3600, "hour");
    if (s < 60 * 86400) return ago(s / 86400, "day");
    return "Played a while ago";
}

inline bool isVideoFile(const std::string& path) {
    const auto dot = path.rfind('.');
    return dot != std::string::npos && lower(path.substr(dot)) == ".mp4";
}

// A video has no picture HOME can read, so its tile is drawn: a dark gradient and a play button.
inline std::vector<std::uint8_t> videoTile(int w, int h) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    const float cx = static_cast<float>(w) * 0.5f, cy = static_cast<float>(h) * 0.5f, R = static_cast<float>(h) * 0.19f;
    for (int y = 0; y < h; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(std::max(1, h - 1));
        for (int x = 0; x < w; ++x) {
            float r = 22.f + 30.f * t, g = 28.f + 34.f * t, b = 46.f + 52.f * t;
            const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
            if (dx * dx + dy * dy <= R * R) { r += 55.f; g += 55.f; b += 60.f; }
            const float tx = dx + R * 0.30f, ty = dy;
            if (tx >= -R * 0.45f && tx <= R * 0.55f && std::abs(ty) <= (R * 0.55f - tx) * 0.95f * 0.85f) { r = g = b = 245.f; }
            std::uint8_t* p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u];
            p[0] = static_cast<std::uint8_t>(std::min(255.f, r));
            p[1] = static_cast<std::uint8_t>(std::min(255.f, g));
            p[2] = static_cast<std::uint8_t>(std::min(255.f, b));
            p[3] = 255;
        }
    }
    return px;
}

} // namespace lite::ui
