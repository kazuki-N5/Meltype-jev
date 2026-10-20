// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5
//
// Jev が変えた文節の光。文節の四角形ごとに、角の丸い柔らかな光 (シアン → 紫のグラデーション、薄く、縁はぼかす) を
// ふわっと出して少し上へ浮かせ、左から右へ光の帯を流し、小さな粒をいくつか昇らせて、約 0.7 秒で消す。
// 半透明のピクセルを自分で計算して UpdateLayeredWindow で出す (クリックは下のアプリに素通しする)。

#include "GlowWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace meltype {
namespace {

constexpr wchar_t kClassName[] = L"MeltypeGlowWindow";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 16;

// 時間 (ミリ秒)
constexpr float kDurationMs = 700;
constexpr float kFadeInMs = 120;
constexpr float kFadeOutStartMs = 380;
constexpr float kShimmerStartMs = 80;
constexpr float kShimmerMs = 480;
constexpr float kSparkleLifeMs = 420;

// 大きさ (DIP。96 dpi のときのピクセル)
constexpr float kMarginX = 16;       // 光のぼかしの分
constexpr float kMarginTop = 28;     // 浮く分と、昇る粒の分
constexpr float kMarginBottom = 12;
constexpr float kInflate = 2;        // 文節の四角形より少し大きく光らせる
constexpr float kRadius = 8;
constexpr float kFeather = 10;       // 光の外側のぼかしの幅
constexpr float kEdge = 3;           // 内側の縁を明るくする幅
constexpr float kDrift = 5;          // 浮き上がる高さ
constexpr float kSparkleRise = 18;
constexpr long long kMaxPixels = 4096LL * 1024;  // これより大きいウィンドウは作らない (文節が何行にもまたがるときなど)

// 色 (0..1)
struct Color {
    float r, g, b;
};
constexpr Color kCyan = {0.22f, 0.84f, 1.0f};
constexpr Color kViolet = {0.63f, 0.43f, 1.0f};
constexpr Color kWhite = {1.0f, 1.0f, 1.0f};

std::once_flag g_classOnce;
bool g_registered = false;

float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

float Smoothstep(float edge0, float edge1, float x) {
    float t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3 - 2 * t);
}

float EaseOutCubic(float p) {
    float q = 1 - Clamp01(p);
    return 1 - q * q * q;
}

Color Mix(const Color& a, const Color& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// 決まった並びの 0..1 の乱数 (毎回同じ所に粒を出す)
float Random(uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352dU;
    seed ^= seed >> 15;
    seed *= 0x846ca68bU;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

// 中心 (cx, cy)・半分の幅と高さ・角の半径の、角の丸い四角形までの距離 (内側は負)
float RoundRectDistance(float x, float y, float cx, float cy, float halfWidth, float halfHeight, float radius) {
    float qx = std::fabs(x - cx) - (halfWidth - radius);
    float qy = std::fabs(y - cy) - (halfHeight - radius);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// 乗算済みアルファの BGRA のピクセルに、色 color を不透明度 alpha で重ねる
void Blend(uint32_t& pixel, const Color& color, float alpha) {
    if (alpha <= 0) return;
    alpha = std::min(alpha, 1.0f);
    float keep = 1 - alpha;
    float b = static_cast<float>(pixel & 0xFF) * keep + color.b * alpha * 255;
    float g = static_cast<float>((pixel >> 8) & 0xFF) * keep + color.g * alpha * 255;
    float r = static_cast<float>((pixel >> 16) & 0xFF) * keep + color.r * alpha * 255;
    float a = static_cast<float>((pixel >> 24) & 0xFF) * keep + alpha * 255;
    auto channel = [](float v) { return static_cast<uint32_t>(std::min(v + 0.5f, 255.0f)); };
    pixel = channel(b) | (channel(g) << 8) | (channel(r) << 16) | (channel(a) << 24);
}

}  // namespace

struct GlowWindow::Impl {
    HWND hwnd = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t* pixels = nullptr;
    int bitmapWidth = 0;
    int bitmapHeight = 0;
    RECT bounds = {};           // ウィンドウの画面上の四角形
    std::vector<RECT> clauses;  // 光らせる文節の四角形 (ウィンドウの中の座標)
    float scale = 1;
    ULONGLONG start = 0;
    bool playing = false;

    int Width() const { return bounds.right - bounds.left; }
    int Height() const { return bounds.bottom - bounds.top; }
    LONG Px(float dip) const { return static_cast<LONG>(std::ceil(dip * scale)); }

    // 文節をまとめて囲み、光・浮く分・粒の分の余白を足した四角形
    RECT Bounds(const RECT& united) const {
        return {united.left - Px(kMarginX), united.top - Px(kMarginTop), united.right + Px(kMarginX), united.bottom + Px(kMarginBottom)};
    }

    bool EnsureBitmap() {
        int width = Width(), height = Height();
        if (bitmap != nullptr && width == bitmapWidth && height == bitmapHeight) return true;
        ReleaseBitmap();
        if (memory == nullptr) memory = CreateCompatibleDC(nullptr);
        if (memory == nullptr) return false;
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 上の行から
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            bitmap = nullptr;
            return false;
        }
        previous = SelectObject(memory, bitmap);
        pixels = static_cast<uint32_t*>(bits);
        bitmapWidth = width;
        bitmapHeight = height;
        return true;
    }

    void ReleaseBitmap() {
        if (bitmap == nullptr) return;
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        bitmap = nullptr;
        previous = nullptr;
        pixels = nullptr;
        bitmapWidth = bitmapHeight = 0;
    }

    void Stop() {
        playing = false;
        if (hwnd != nullptr) {
            KillTimer(hwnd, kFrameTimer);
            ShowWindow(hwnd, SW_HIDE);
        }
        clauses.clear();
        // 出していない間は絵を持たない
        ReleaseBitmap();
    }

    // 1 コマ描いて出す (タイマーから)
    void Frame() {
        if (!playing) return;
        float t = static_cast<float>(GetTickCount64() - start);
        if (t >= kDurationMs || !EnsureBitmap()) {
            Stop();
            return;
        }
        Render(t);
        POINT position = {bounds.left, bounds.top};
        SIZE size = {Width(), Height()};
        POINT source = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(hwnd, nullptr, &position, &size, memory, &source, 0, &blend, ULW_ALPHA)) {
            TipLog(L"文節を光らせるウィンドウを描けませんでした: %lu", GetLastError());
            Stop();
            return;
        }
        if (!IsWindowVisible(hwnd)) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void Render(float t) {
        std::memset(pixels, 0, static_cast<size_t>(bitmapWidth) * static_cast<size_t>(bitmapHeight) * sizeof(uint32_t));
        // ふわっと出て (ease-out)、後半でゆっくり消える
        float envelope = EaseOutCubic(t / kFadeInMs) * (1 - Smoothstep(kFadeOutStartMs, kDurationMs, t));
        float lift = kDrift * scale * EaseOutCubic(t / kDurationMs);
        float shimmer = Smoothstep(0, 1, (t - kShimmerStartMs) / kShimmerMs);
        for (size_t i = 0; i < clauses.size(); i++) DrawClause(clauses[i], static_cast<uint32_t>(i), t, envelope, lift, shimmer);
    }

    void DrawClause(const RECT& clause, uint32_t index, float t, float envelope, float lift, float shimmer) {
        float inflate = kInflate * scale, feather = kFeather * scale, edge = kEdge * scale;
        float left = static_cast<float>(clause.left) - inflate, right = static_cast<float>(clause.right) + inflate;
        float top = static_cast<float>(clause.top) - inflate - lift, bottom = static_cast<float>(clause.bottom) + inflate - lift;
        float width = std::max(right - left, 1.0f), height = std::max(bottom - top, 1.0f);
        float cx = (left + right) / 2, cy = (top + bottom) / 2;
        float radius = std::min(kRadius * scale, std::min(width, height) / 2);
        // 光の帯: 文節の左の外から右の外へ、少し斜めに流れる
        float band = std::max(width * 0.16f, 10 * scale);
        float bandX = left - band * 2 + shimmer * (width + band * 4);

        if (envelope > 0) {
            int x0 = std::max(0, static_cast<int>(std::floor(left - feather)));
            int x1 = std::min(bitmapWidth, static_cast<int>(std::ceil(right + feather)));
            int y0 = std::max(0, static_cast<int>(std::floor(top - feather)));
            int y1 = std::min(bitmapHeight, static_cast<int>(std::ceil(bottom + feather)));
            for (int y = y0; y < y1; y++) {
                uint32_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(bitmapWidth);
                float py = static_cast<float>(y) + 0.5f;
                for (int x = x0; x < x1; x++) {
                    float px = static_cast<float>(x) + 0.5f;
                    float d = RoundRectDistance(px, py, cx, cy, width / 2, height / 2, radius);
                    if (d >= feather) continue;
                    // 内側は薄く塗って縁を少し明るく、外側はなめらかに消える (縁でつながる: 0.24)
                    float glow;
                    if (d <= 0) {
                        glow = 0.14f + 0.10f * std::exp(-(d * d) / (edge * edge));
                    } else {
                        float k = 1 - Smoothstep(0, feather, d);
                        glow = 0.24f * k * k;
                    }
                    Blend(row[x], Mix(kCyan, kViolet, Clamp01((px - left) / width)), glow * envelope);
                    float slant = px - bandX + (py - cy) * 0.4f;
                    float inside = 1 - Smoothstep(-scale, scale, d);
                    float shine = 0.30f * std::exp(-(slant * slant) * 4 / (band * band)) * inside;
                    Blend(row[x], kWhite, shine * envelope);
                }
            }
        }

        // 小さな粒: 文節の中から上へ昇って消える (文節ごとに 3〜5 個)
        int count = 3 + static_cast<int>(Random(index * 977 + 13) * 2.999f);
        float core = 1.3f * scale, halo = 3.5f * scale, reach = halo * 2.5f;
        for (int k = 0; k < count; k++) {
            uint32_t seed = index * 64 + static_cast<uint32_t>(k) * 4 + 7;
            float p = (t - (60 + 200 * Random(seed))) / kSparkleLifeMs;
            if (p <= 0 || p >= 1) continue;
            float alpha = std::sin(p * 3.14159265f) * 0.85f;
            float sx = left + width * (0.1f + 0.8f * Random(seed + 1));
            float sy = top + height * (0.45f + 0.35f * Random(seed + 2)) - kSparkleRise * scale * EaseOutCubic(p);
            int x0 = std::max(0, static_cast<int>(std::floor(sx - reach)));
            int x1 = std::min(bitmapWidth, static_cast<int>(std::ceil(sx + reach)));
            int y0 = std::max(0, static_cast<int>(std::floor(sy - reach)));
            int y1 = std::min(bitmapHeight, static_cast<int>(std::ceil(sy + reach)));
            for (int y = y0; y < y1; y++) {
                uint32_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(bitmapWidth);
                float dy = static_cast<float>(y) + 0.5f - sy;
                for (int x = x0; x < x1; x++) {
                    float dx = static_cast<float>(x) + 0.5f - sx;
                    float dd = dx * dx + dy * dy;
                    float c = std::exp(-dd / (core * core));
                    float h = std::exp(-dd / (halo * halo));
                    Blend(row[x], Mix(kCyan, kWhite, 0.5f + 0.5f * c), alpha * std::min(1.0f, c * 0.9f + h * 0.35f));
                }
            }
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
            case WM_NCHITTEST:
                return HTTRANSPARENT;  // マウスは下のアプリへ
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;  // 入力欄からフォーカスを奪わない
            case WM_TIMER:
                if (wParam == kFrameTimer) self->Frame();
                return 0;
            case WM_DPICHANGED:
                return 0;  // 大きさは Play で決める
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

static bool EnsureGlowClass() {
    std::call_once(g_classOnce, [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = GlowWindow::Impl::WndProc;
        wc.hInstance = g_module;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        g_registered = true;
    });
    return g_registered;
}

GlowWindow::GlowWindow() : impl_(new Impl()) {}

GlowWindow::~GlowWindow() {
    impl_->Stop();
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->memory) DeleteDC(impl_->memory);
    delete impl_;
}

bool GlowWindow::Playing() const { return impl_->playing; }

void GlowWindow::Hide() { impl_->Stop(); }

void GlowWindow::Play(const std::vector<RECT>& clauses) {
    Impl& d = *impl_;
    d.Stop();
    RECT united = {};
    for (const RECT& rect : clauses) {
        if (!IsRectEmpty(&rect)) UnionRect(&united, &united, &rect);
    }
    if (IsRectEmpty(&united) || !EnsureGlowClass()) return;
    if (d.hwnd == nullptr) {
        d.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 0, 0, 1,
                                 1, nullptr, nullptr, g_module, &d);
        if (d.hwnd == nullptr) {
            TipLog(L"文節を光らせるウィンドウを作れませんでした: %lu", GetLastError());
            return;
        }
    }
    d.scale = GetDpiForWindow(d.hwnd) / 96.0f;
    if (d.scale <= 0) d.scale = 1;
    // DPI は置いたモニターで決まる。置いてから読み直し、変わったらその DPI で余白を決め直す (まだ出さない)
    for (int i = 0; i < 2; i++) {
        d.bounds = d.Bounds(united);
        SetWindowPos(d.hwnd, nullptr, d.bounds.left, d.bounds.top, d.Width(), d.Height(), SWP_NOACTIVATE | SWP_NOZORDER);
        float moved = GetDpiForWindow(d.hwnd) / 96.0f;
        if (moved <= 0 || moved == d.scale) break;
        d.scale = moved;
    }
    if (static_cast<long long>(d.Width()) * d.Height() > kMaxPixels) {
        TipLog(L"光らせる範囲が大きすぎるので光らせません (%dx%d)", d.Width(), d.Height());
        return;
    }
    for (const RECT& rect : clauses) {
        if (IsRectEmpty(&rect)) continue;
        RECT local = rect;
        OffsetRect(&local, -d.bounds.left, -d.bounds.top);
        d.clauses.push_back(local);
    }
    // 描くのはタイマーの中 (キーの処理を待たせない)
    d.start = GetTickCount64();
    d.playing = true;
    SetTimer(d.hwnd, kFrameTimer, kFrameMs, nullptr);
}

void GlowWindow::UnregisterClasses() {
    if (!g_registered) return;
    UnregisterClassW(kClassName, g_module);
    g_registered = false;
}

}  // namespace meltype
