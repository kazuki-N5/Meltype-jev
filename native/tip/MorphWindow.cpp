// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5
//
// Jev が変えた文節を、変える前の候補から今の候補へ溶けるように入れ替えて見せる (MorphWindow.h)。
// 1. Play: 文節の上を、その場の背景の色で覆う (アプリが今の候補を描いても見えないように)。
// 2. アプリが描き直したら画面を写し取り (このウィンドウは写らない)、今の候補の形・文字の色・背景の色・下線を読み取る。
//    前の候補は、今の候補と字の高さがそろう大きさの Yu Gothic UI で描く。
// 3. 0.28 秒で前の候補をぼかしながら消し、今の候補をぼかした所からくっきりさせる (どちらもグラデーションの色)。
//    幅が変わる分だけ、後ろの文字 (写したもの) を寄せる。続く 0.18 秒で、グラデーションから写した文字そのものへ戻して消える。
// ぼかしは文字の大きさの 0.12 倍を単位に「単位 × (1 / 進み具合 − 1)」(最大で単位の 5 倍)、不透明度は 進み具合^0.4。
// Jev を待つ間: BeginThinking で、Space を押したときの変換中の文字を写し、文字の濃さを読み取っておく。応答を待つ間に呼ばれる
// ThinkFrame で、写した文字のうち光の帯 (文字の幅の 3 割、24〜160 ピクセル) にかかる所をグラデーションの色に変えて重ねる。

#include "MorphWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace meltype {
namespace {

constexpr wchar_t kClassName[] = L"MeltypeMorphWindow";
constexpr wchar_t kFontFamily[] = L"Yu Gothic UI";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 16;

// 時間 (ミリ秒)
constexpr float kMorphMs = 280;          // 前の候補 → 今の候補
constexpr float kSettleMs = 180;         // グラデーション → ふつうの文字の色
constexpr ULONGLONG kPaintWaitMs = 150;  // アプリが今の候補を描き直すのを待つ長さの上限
constexpr ULONGLONG kThinkDelayMs = 120; // Space からこれだけたっても応答が無ければ、Jev を待つ表示を出す
constexpr float kThinkPeriodMs = 700;    // 光が文字の左から右へ流れ切る長さ
constexpr float kThinkRiseMs = 120;      // 出し始めに光が強くなる長さ
constexpr float kThinkFadeMs = 150;      // 聞かなかったとき (Jev を使わなかった) 消していく長さ
// Jev の答えが無かったとき (時間切れ・通信エラーなど): 変換中の文字全体を少し赤くしてから消していく
constexpr float kFailRiseMs = 100;       // 赤くなるまで
constexpr float kFailHoldMs = 250;       // ここまで赤いまま (出し始めから)
constexpr float kFailMs = 450;           // ここで消え終わる
constexpr float kFailTint = 0.75f;       // 文字の色をどれだけ赤に寄せるか
constexpr LONG kThinkPad = 2;            // Jev を待つ表示で、文字の四角形の外に写す分

constexpr float kBlurUnit = 0.12f;  // ぼかしの単位 (文字の大きさに対する割合)
constexpr float kMaxBlur = 5;       // 単位の何倍までぼかすか
constexpr long long kMaxPixels = 4096LL * 1024;  // これより大きいウィンドウは作らない

// 色 (0..1)
struct Color {
    float r, g, b;
};
// 変わる間だけ使うグラデーション (左から右へ)
constexpr Color kGradient[] = {
    {0x42 / 255.0f, 0x85 / 255.0f, 0xf4 / 255.0f},
    {0x9b / 255.0f, 0x72 / 255.0f, 0xcb / 255.0f},
    {0xd9 / 255.0f, 0x65 / 255.0f, 0x70 / 255.0f},
};
constexpr float kPi = 3.14159265f;
constexpr Color kFailColor = {0xe5 / 255.0f, 0x48 / 255.0f, 0x4d / 255.0f};

std::once_flag g_classOnce;
bool g_registered = false;

float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }

float EaseInOutQuad(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) / 2;
}

Color Mix(const Color& a, const Color& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

Color GradientAt(float t) {
    t = Clamp01(t);
    return t < 0.5f ? Mix(kGradient[0], kGradient[1], t * 2) : Mix(kGradient[1], kGradient[2], (t - 0.5f) * 2);
}

Color FromPixel(uint32_t pixel) {
    return {static_cast<float>((pixel >> 16) & 0xFF) / 255, static_cast<float>((pixel >> 8) & 0xFF) / 255, static_cast<float>(pixel & 0xFF) / 255};
}

// 不透明なピクセル (乗算済みアルファの BGRA)
uint32_t ToPixel(const Color& color) {
    auto channel = [](float v) { return static_cast<uint32_t>(Clamp01(v) * 255 + 0.5f); };
    return channel(color.b) | (channel(color.g) << 8) | (channel(color.r) << 16) | 0xFF000000u;
}

float Distance(const Color& a, const Color& b) {
    float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
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

// 文字の形 (0..1 の濃さ)。まわりに、ぼかしがはみ出す分の余白を持つ
struct Mask {
    int width = 0;
    int height = 0;
    std::vector<float> values;
};

// 濃さが threshold を超える行の上と下 (無ければ false)
bool InkRows(const Mask& mask, int& top, int& bottom) {
    top = -1;
    bottom = -1;
    for (int y = 0; y < mask.height; y++) {
        const float* row = mask.values.data() + static_cast<size_t>(y) * mask.width;
        if (std::any_of(row, row + mask.width, [](float v) { return v > 0.3f; })) {
            if (top < 0) top = y;
            bottom = y;
        }
    }
    return top >= 0;
}

// 半径 r の箱ぼかし (横)。外は 0
void BoxBlurH(const float* src, float* dst, int w, int h, int r) {
    float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; y++) {
        const float* s = src + static_cast<size_t>(y) * w;
        float* d = dst + static_cast<size_t>(y) * w;
        float sum = 0;
        for (int i = 0; i <= std::min(r, w - 1); i++) sum += s[i];
        for (int x = 0; x < w; x++) {
            d[x] = sum * inv;
            if (x + r + 1 < w) sum += s[x + r + 1];
            if (x - r >= 0) sum -= s[x - r];
        }
    }
}

// 半径 r の箱ぼかし (縦)。外は 0
void BoxBlurV(const float* src, float* dst, int w, int h, int r) {
    float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int x = 0; x < w; x++) {
        float sum = 0;
        for (int i = 0; i <= std::min(r, h - 1); i++) sum += src[static_cast<size_t>(i) * w + x];
        for (int y = 0; y < h; y++) {
            dst[static_cast<size_t>(y) * w + x] = sum * inv;
            if (y + r + 1 < h) sum += src[static_cast<size_t>(y + r + 1) * w + x];
            if (y - r >= 0) sum -= src[static_cast<size_t>(y - r) * w + x];
        }
    }
}

// 標準偏差 sigma のガウスぼかしを、箱ぼかし 3 回で近づける。sigma が 1 より小さいときは、ぼかさない形と sigma = 1 の形を混ぜる
void GaussianBlur(const Mask& mask, float sigma, std::vector<float>& out, std::vector<float>& work) {
    out = mask.values;
    if (sigma <= 0.01f || mask.width == 0 || mask.height == 0) return;
    float s = std::max(sigma, 1.0f);
    // 箱の幅 (Kutskir の方法)
    constexpr int n = 3;
    float ideal = std::sqrt(12 * s * s / n + 1);
    int lower = static_cast<int>(std::floor(ideal));
    if (lower % 2 == 0) lower--;
    int upper = lower + 2;
    int m = static_cast<int>(std::lround((12 * s * s - n * lower * lower - 4 * n * lower - 3 * n) / (-4.0f * lower - 4)));
    work.resize(out.size());
    for (int i = 0; i < n; i++) {
        int r = ((i < m ? lower : upper) - 1) / 2;
        if (r <= 0) continue;
        BoxBlurH(out.data(), work.data(), mask.width, mask.height, r);
        BoxBlurV(work.data(), out.data(), mask.width, mask.height, r);
    }
    if (sigma < 1) {
        for (size_t i = 0; i < out.size(); i++) out[i] = mask.values[i] + (out[i] - mask.values[i]) * sigma;
    }
}

// Yu Gothic UI を大きさ em (ピクセル) で使う GDI の DC
class TextDc {
public:
    explicit TextDc(float em) {
        dc_ = CreateCompatibleDC(nullptr);
        font_ = CreateFontW(-static_cast<int>(std::lround(em)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH, kFontFamily);
        if (dc_ != nullptr && font_ != nullptr) previous_ = SelectObject(dc_, font_);
    }
    ~TextDc() {
        if (previous_ != nullptr) SelectObject(dc_, previous_);
        if (font_ != nullptr) DeleteObject(font_);
        if (dc_ != nullptr) DeleteDC(dc_);
    }
    TextDc(const TextDc&) = delete;
    TextDc& operator=(const TextDc&) = delete;

    bool Ok() const { return previous_ != nullptr; }
    HDC Dc() const { return dc_; }
    int Width(const std::wstring& text) const {
        SIZE size = {};
        if (!Ok() || text.empty() || !GetTextExtentPoint32W(dc_, text.c_str(), static_cast<int>(text.size()), &size)) return 0;
        return size.cx;
    }

private:
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

// 32bit の DIB (上の行から)
HBITMAP CreateDib(HDC dc, int width, int height, uint32_t** bits) {
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (bitmap != nullptr && raw == nullptr) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
    *bits = bitmap != nullptr ? static_cast<uint32_t*>(raw) : nullptr;
    return bitmap;
}

// text を大きさ em で描いた形 (まわりに pad の余白)。width には文字の送り幅を返す
bool RenderText(const std::wstring& text, float em, int pad, Mask& mask, int& width) {
    TextDc t(em);
    if (!t.Ok()) return false;
    width = t.Width(text);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(t.Dc(), &metrics);
    mask.width = width + pad * 2;
    mask.height = metrics.tmHeight + pad * 2;
    if (width <= 0 || metrics.tmHeight <= 0) return false;
    uint32_t* bits = nullptr;
    HBITMAP bitmap = CreateDib(t.Dc(), mask.width, mask.height, &bits);
    if (bitmap == nullptr) return false;
    HGDIOBJ old = SelectObject(t.Dc(), bitmap);
    std::memset(bits, 0, static_cast<size_t>(mask.width) * mask.height * sizeof(uint32_t));
    SetBkMode(t.Dc(), TRANSPARENT);
    SetTextColor(t.Dc(), RGB(255, 255, 255));
    TextOutW(t.Dc(), pad, pad, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    mask.values.resize(static_cast<size_t>(mask.width) * mask.height);
    for (size_t i = 0; i < mask.values.size(); i++) mask.values[i] = static_cast<float>((bits[i] >> 8) & 0xFF) / 255;
    SelectObject(t.Dc(), old);
    DeleteObject(bitmap);
    return true;
}

// 画面の screen の範囲を写す (UpdateLayeredWindow のウィンドウは写らない: CAPTUREBLT を付けない)
bool CaptureScreen(const RECT& screen, std::vector<uint32_t>& out) {
    int width = screen.right - screen.left, height = screen.bottom - screen.top;
    if (width <= 0 || height <= 0) return false;
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) return false;
    HDC memory = CreateCompatibleDC(screenDc);
    uint32_t* bits = nullptr;
    HBITMAP bitmap = memory != nullptr ? CreateDib(memory, width, height, &bits) : nullptr;
    bool ok = false;
    if (bitmap != nullptr) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, width, height, screenDc, screen.left, screen.top, SRCCOPY) != FALSE;
        GdiFlush();
        if (ok) {
            out.resize(static_cast<size_t>(width) * height);
            for (size_t i = 0; i < out.size(); i++) out[i] = bits[i] & 0xFFFFFF;
        }
        SelectObject(memory, old);
        DeleteObject(bitmap);
    }
    if (memory != nullptr) DeleteDC(memory);
    ReleaseDC(nullptr, screenDc);
    return ok;
}

// image (横幅 width) の rect の中でいちばん多い色 (背景)
uint32_t MostCommonIn(const std::vector<uint32_t>& image, int width, int height, const RECT& rect) {
    std::unordered_map<uint32_t, int> counts;
    for (LONG y = std::max(0L, rect.top); y < std::min<LONG>(height, rect.bottom); y++) {
        for (LONG x = std::max(0L, rect.left); x < std::min<LONG>(width, rect.right); x++) counts[image[static_cast<size_t>(y) * width + x]]++;
    }
    uint32_t best = 0xFFFFFF;
    int bestCount = 0;
    for (const auto& [color, count] : counts) {
        if (count > bestCount) best = color, bestCount = count;
    }
    return best;
}

}  // namespace

static bool EnsureMorphClass();

struct MorphWindow::Impl {
    // Jev が変えた文節 1 つ (座標はウィンドウの中)
    struct Item {
        RECT rect = {};       // 今の候補を描いた所
        std::wstring from;
        std::wstring to;
        int oldWidth = 0;     // 前の候補の幅
        int newWidth = 0;     // 今の候補の幅
        Mask oldMask;         // 前の候補の形 (Yu Gothic UI で描いたもの)
        Mask newMask;         // 今の候補の形 (画面から読み取ったもの)
        POINT oldOrigin = {};  // 寄せる前の、形の左上の位置
        POINT newOrigin = {};
    };
    enum class Phase { Idle, Thinking, ThinkFading, Waiting, Morphing };

    HWND hwnd = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t* pixels = nullptr;
    int bitmapWidth = 0;
    int bitmapHeight = 0;
    bool excluded = false;  // このウィンドウを画面の写しに入れない設定にした

    RECT bounds = {};          // ウィンドウの画面上の四角形
    std::vector<Item> items;
    std::vector<RECT> covers;  // 背景の色で覆う所
    bool slide = false;        // 後ろの文字を寄せる (文節がすべて同じ行にある)
    LONG lineRight = 0;        // 寄せるとき: 変換中の文字の右端
    float em = 0;              // 文字の大きさ (ピクセル)
    int pad = 0;               // ぼかしがはみ出す分
    Phase phase = Phase::Idle;
    bool settleOnly = false;   // 入れ替えずに、グラデーションからふつうの色に戻すところだけ
    ULONGLONG playStart = 0;
    ULONGLONG morphStart = 0;
    // Jev を待つ間の表示
    ULONGLONG thinkStart = 0;  // BeginThinking した時刻
    ULONGLONG fadeStart = 0;   // 消していき始めた時刻
    bool thinkShown = false;   // 見せた
    bool failed = false;       // Jev の答えが無かった (赤くしてから消していく)
    LONG thinkLeft = 0;        // 変換中の文字の左右 (ウィンドウの中)
    LONG thinkRight = 0;
    std::vector<float> thinkInk;  // ピクセルごとの文字の濃さ (0..1。文字の四角形の外は 0)
    std::vector<uint32_t> before;   // Play したときの画面
    std::vector<uint32_t> capture;  // アプリが今の候補を描いたあとの画面
    uint32_t background = 0;
    Color backgroundColor = {};
    Color textColor = {};
    std::vector<char> underline;  // 行ごとの、変換中の文字の下線か
    std::vector<float> blurred;
    std::vector<float> work;

    int Width() const { return bounds.right - bounds.left; }
    int Height() const { return bounds.bottom - bounds.top; }

    bool EnsureBitmap() {
        int width = Width(), height = Height();
        if (bitmap != nullptr && width == bitmapWidth && height == bitmapHeight) return true;
        ReleaseBitmap();
        if (memory == nullptr) memory = CreateCompatibleDC(nullptr);
        if (memory == nullptr) return false;
        bitmap = CreateDib(memory, width, height, &pixels);
        if (bitmap == nullptr) return false;
        previous = SelectObject(memory, bitmap);
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
        Reset();
        if (hwnd != nullptr) ShowWindow(hwnd, SW_HIDE);
    }

    // 止めて、持っているものを捨てる。ウィンドウは出したまま (Jev を待つ表示から、続けて入れ替えに移るとき)
    void Reset() {
        phase = Phase::Idle;
        settleOnly = false;
        thinkShown = false;
        failed = false;
        if (hwnd != nullptr) KillTimer(hwnd, kFrameTimer);
        items.clear();
        covers.clear();
        before.clear();
        capture.clear();
        underline.clear();
        blurred.clear();
        work.clear();
        thinkInk.clear();
        // 出していない間は絵を持たない (出したウィンドウの絵は、UpdateLayeredWindow で渡したときに写し取られている)
        ReleaseBitmap();
    }

    bool EnsureWindow() {
        if (hwnd != nullptr) return true;
        if (!EnsureMorphClass()) return false;
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, g_module, this);
        if (hwnd == nullptr) {
            TipLog(L"Jev の入れ替えのウィンドウを作れませんでした: %lu", GetLastError());
            return false;
        }
        // 画面を写すとき、このウィンドウは入れない (下のアプリの文字を写すため)
        excluded = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
        if (!excluded) TipLog(L"Jev の入れ替えのウィンドウを、画面の写しから外せませんでした: %lu", GetLastError());
        return true;
    }

    // Play の中身。失敗したら false (呼ぶ側が止める)
    bool Start(const std::vector<MorphClause>& clauses, LONG tailRight, bool settle);

    uint32_t& Pixel(int x, int y) { return pixels[static_cast<size_t>(y) * bitmapWidth + x]; }
    uint32_t Captured(int x, int y) const { return capture[static_cast<size_t>(y) * Width() + x]; }

    // 文節の中でいちばん多い色 (背景)
    uint32_t MostCommon(const std::vector<uint32_t>& image) const {
        std::unordered_map<uint32_t, int> counts;
        for (const Item& item : items) {
            for (LONG y = std::max(0L, item.rect.top); y < std::min<LONG>(Height(), item.rect.bottom); y++) {
                for (LONG x = std::max(0L, item.rect.left); x < std::min<LONG>(Width(), item.rect.right); x++) counts[image[static_cast<size_t>(y) * Width() + x]]++;
            }
        }
        uint32_t best = 0xFFFFFF;
        int bestCount = 0;
        for (const auto& [color, count] : counts) {
            if (count > bestCount) best = color, bestCount = count;
        }
        return best;
    }

    // Jev を待つ表示: 写した画面 (capture) の text (ウィンドウの中) から、背景の色と文字の濃さを読み取る
    bool PrepareThinking(const RECT& text) {
        background = MostCommonIn(capture, Width(), Height(), text);
        backgroundColor = FromPixel(background);
        float farthest = 0;
        for (LONG y = text.top; y < text.bottom; y++) {
            for (LONG x = text.left; x < text.right; x++) farthest = std::max(farthest, Distance(FromPixel(Captured(x, y)), backgroundColor));
        }
        if (farthest < 0.15f) {
            TipLog(L"Jev を待つ表示: 変換中の文字が画面から読み取れませんでした");
            return false;
        }
        thinkInk.assign(capture.size(), 0);
        for (LONG y = text.top; y < text.bottom; y++) {
            for (LONG x = text.left; x < text.right; x++) {
                thinkInk[static_cast<size_t>(y) * Width() + x] = Clamp01(Distance(FromPixel(Captured(x, y)), backgroundColor) / farthest);
            }
        }
        thinkLeft = text.left;
        thinkRight = text.right;
        return true;
    }

    // Jev を待つ表示の 1 コマ: 写した画面に、光の帯にかかる文字だけグラデーションの色を重ねる
    void RenderThinking(ULONGLONG now) {
        float s = static_cast<float>(now - thinkStart) - static_cast<float>(kThinkDelayMs);
        if (s < 0) s = 0;
        float span = static_cast<float>(std::max(thinkRight - thinkLeft, 1L));
        float half = std::clamp(span * 0.15f, 12.0f, 80.0f);  // 帯の幅の半分
        float center = static_cast<float>(thinkLeft) - half + std::fmod(s, kThinkPeriodMs) / kThinkPeriodMs * (span + half * 2);
        float strength = Clamp01(s / kThinkRiseMs);
        for (int y = 0; y < bitmapHeight; y++) {
            for (int x = 0; x < bitmapWidth; x++) {
                uint32_t captured = Captured(x, y);
                float ink = thinkInk[static_cast<size_t>(y) * bitmapWidth + x];
                float d = (static_cast<float>(x) - center) / half;
                if (ink <= 0.01f || d <= -1 || d >= 1) {
                    Pixel(x, y) = captured | 0xFF000000u;
                    continue;
                }
                float k = 0.5f * (1 + std::cos(kPi * d)) * strength;
                Color glow = Mix(backgroundColor, GradientAt((d + 1) / 2), ink);
                Pixel(x, y) = ToPixel(Mix(FromPixel(captured), glow, k));
            }
        }
    }

    // Jev の答えが無かった: 光を止めた表示の文字を、strength (0..1) の分だけ赤に寄せる
    void RenderFailed(float strength) {
        RenderThinking(fadeStart);
        float k = strength * kFailTint;
        for (int y = 0; y < bitmapHeight; y++) {
            for (int x = 0; x < bitmapWidth; x++) {
                float ink = thinkInk[static_cast<size_t>(y) * bitmapWidth + x];
                if (ink <= 0.01f) continue;
                Pixel(x, y) = ToPixel(Mix(FromPixel(Pixel(x, y)), Mix(backgroundColor, kFailColor, ink), k));
            }
        }
    }

    // アプリが文節を描き直したか (Play したときの画面と比べる。右端はキャレットかもしれないので見ない)
    bool Repainted() const {
        for (const Item& item : items) {
            int changed = 0, area = 0;
            for (LONG y = std::max(0L, item.rect.top); y < std::min<LONG>(Height(), item.rect.bottom); y++) {
                for (LONG x = std::max(0L, item.rect.left); x < std::min<LONG>(Width(), item.rect.right - 3); x++) {
                    size_t i = static_cast<size_t>(y) * Width() + x;
                    area++;
                    if (before[i] != capture[i]) changed++;
                }
            }
            if (changed >= std::max(4, area / 200)) return true;
        }
        return false;
    }

    // 写した画面から、背景・文字の色・下線・今の候補の形を読み取り、前の候補を描く
    bool Prepare() {
        background = MostCommon(capture);
        backgroundColor = FromPixel(background);
        float farthest = 0;
        for (const Item& item : items) {
            for (LONG y = item.rect.top; y < item.rect.bottom; y++) {
                for (LONG x = item.rect.left; x < item.rect.right; x++) {
                    Color color = FromPixel(Captured(x, y));
                    float d = Distance(color, backgroundColor);
                    if (d > farthest) farthest = d, textColor = color;
                }
            }
        }
        if (farthest < 0.15f) {
            TipLog(L"Jev の入れ替え: 文節の文字が画面から読み取れませんでした");
            return false;
        }
        auto ink = [&](uint32_t pixel) { return Clamp01(Distance(FromPixel(pixel), backgroundColor) / farthest); };

        // 変換中の文字の下線は動かさない: 行の下半分で、横に 6 割以上つながっている行
        underline.assign(static_cast<size_t>(Height()), 0);
        for (const RECT& cover : covers) {
            LONG right = slide ? lineRight : cover.right;
            LONG span = right - cover.left;
            if (span <= 0) continue;
            for (LONG y = cover.top + (cover.bottom - cover.top) / 2; y < cover.bottom; y++) {
                int count = 0;
                for (LONG x = cover.left; x < right; x++) count += ink(Captured(x, y)) > 0.5f ? 1 : 0;
                if (count * 10 >= span * 6) underline[y] = 1;
            }
        }

        // 今の候補の形 (画面から読み取ったもの)
        std::vector<int> inkTops, inkBottoms;
        for (size_t k = 0; k < items.size();) {
            Item& item = items[k];
            int width = item.rect.right - item.rect.left, height = item.rect.bottom - item.rect.top;
            item.newMask.width = width + pad * 2;
            item.newMask.height = height + pad * 2;
            item.newMask.values.assign(static_cast<size_t>(item.newMask.width) * item.newMask.height, 0);
            for (int y = 0; y < height; y++) {
                if (underline[item.rect.top + y]) continue;
                for (int x = 0; x < width; x++) {
                    item.newMask.values[static_cast<size_t>(y + pad) * item.newMask.width + x + pad] = ink(Captured(item.rect.left + x, item.rect.top + y));
                }
            }
            item.newOrigin = {item.rect.left - pad, item.rect.top - pad};
            int top = 0, bottom = 0;
            if (!InkRows(item.newMask, top, bottom)) {
                TipLog(L"Jev の入れ替え: 「%s」の形が画面から読み取れませんでした", item.to.c_str());
                // 色を戻すだけなら、読み取れない文節は覆わずに除いて続ける (文節ごとの四角形を正しく返さないアプリがある)
                if (!settleOnly) return false;
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(k));
                covers.erase(covers.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            inkTops.push_back(top);
            inkBottoms.push_back(bottom);
            k++;
        }
        if (items.empty()) return false;
        // 文字の大きさを、字の高さで決め直す (幅で決めると、アプリのフォントとかなの幅が違うときに大きさがずれる)
        float emSum = 0;
        int emCount = 0;
        for (size_t k = 0; k < items.size(); k++) {
            Mask probe;
            int probeWidth = 0, top = 0, bottom = 0;
            if (RenderText(items[k].to, 100, 0, probe, probeWidth) && InkRows(probe, top, bottom)) {
                emSum += 100.0f * static_cast<float>(inkBottoms[k] - inkTops[k] + 1) / static_cast<float>(bottom - top + 1);
                emCount++;
            }
        }
        if (emCount > 0) em = std::clamp(emSum / static_cast<float>(emCount), 6.0f, 200.0f);
        // 前の候補は同じ大きさのフォントで描き、字の上下の真ん中を今の候補にそろえる
        for (size_t k = 0; k < items.size(); k++) {
            Item& item = items[k];
            int oldTop = 0, oldBottom = 0, oldWidth = 0;
            if (!RenderText(item.from, em, pad, item.oldMask, oldWidth) || !InkRows(item.oldMask, oldTop, oldBottom)) return false;
            item.oldWidth = oldWidth;
            item.oldOrigin = {item.rect.left - pad, item.newOrigin.y + (inkTops[k] + inkBottoms[k]) / 2 - (oldTop + oldBottom) / 2};
        }
        return true;
    }

    // 進み具合 x (0..1) のときのぼかし (ピクセル)
    float BlurAt(float x) const {
        float unit = em * kBlurUnit;
        if (x <= 0) return unit * kMaxBlur;
        return std::min(unit * (1 / x - 1), unit * kMaxBlur);
    }

    // 形 mask を、左上 (x0, y0) に、横幅 span のグラデーション (textMix の分だけ文字の色を混ぜる) で、不透明度 opacity で重ねる
    void DrawMask(const Mask& mask, float sigma, int x0, int y0, int textLeft, int span, float textMix, float opacity) {
        if (opacity <= 0.002f) return;
        GaussianBlur(mask, sigma, blurred, work);
        for (int my = 0; my < mask.height; my++) {
            int y = y0 + my;
            if (y < 0 || y >= bitmapHeight) continue;
            for (int mx = 0; mx < mask.width; mx++) {
                float v = blurred[static_cast<size_t>(my) * mask.width + mx] * opacity;
                int x = x0 + mx;
                if (v < 0.002f || x < 0 || x >= bitmapWidth) continue;
                Color color = GradientAt(static_cast<float>(x - textLeft) / static_cast<float>(std::max(span, 1)));
                if (textMix > 0) color = Mix(color, textColor, textMix);
                Blend(Pixel(x, y), color, v);
            }
        }
    }

    void Render(float t) {
        std::memset(pixels, 0, static_cast<size_t>(bitmapWidth) * bitmapHeight * sizeof(uint32_t));
        float f = Clamp01(t / kMorphMs);
        float settle = Clamp01((t - kMorphMs) / kSettleMs);
        float e = EaseInOutQuad(f);
        uint32_t opaqueBackground = background | 0xFF000000u;

        // 背景の色で覆い、下線は写したまま置く
        for (const RECT& cover : covers) {
            for (LONG y = std::max(0L, cover.top); y < std::min<LONG>(bitmapHeight, cover.bottom); y++) {
                LONG right = std::min<LONG>(bitmapWidth, cover.right);
                LONG lineEnd = slide ? std::min(right, lineRight) : right;
                for (LONG x = std::max(0L, cover.left); x < right; x++) {
                    Pixel(x, y) = underline[y] && x < lineEnd ? (Captured(x, y) | 0xFF000000u) : opaqueBackground;
                }
            }
        }

        // 文節ごとの、幅が変わった分のずれ (左の文節から足していく)
        std::vector<int> shifts(items.size() + 1, 0);
        float sum = 0;
        for (size_t k = 0; k < items.size(); k++) {
            const Item& item = items[k];
            if (slide) sum += static_cast<float>(item.oldWidth) + static_cast<float>(item.newWidth - item.oldWidth) * e - static_cast<float>(item.newWidth);
            shifts[k + 1] = static_cast<int>(std::lround(sum));
        }

        // 後ろの文字 (写したもの) を、ずれた位置に置く
        if (slide) {
            const RECT& cover = covers.front();
            for (size_t k = 0; k < items.size(); k++) {
                LONG from = items[k].rect.right;
                LONG to = k + 1 < items.size() ? items[k + 1].rect.left : lineRight;
                int shift = shifts[k + 1];
                for (LONG y = std::max(0L, cover.top); y < std::min<LONG>(bitmapHeight, cover.bottom); y++) {
                    if (underline[y]) continue;
                    for (LONG x = from; x < to; x++) {
                        LONG dx = x + shift;
                        if (dx >= 0 && dx < bitmapWidth) Pixel(dx, y) = Captured(x, y) | 0xFF000000u;
                    }
                }
            }
        }

        for (size_t k = 0; k < items.size(); k++) {
            Item& item = items[k];
            int shift = shifts[k];
            if (settle <= 0) {
                // 前の候補: 文字の色から少しずつグラデーションに変えながら、ぼかして消す
                if (f < 1) {
                    DrawMask(item.oldMask, BlurAt(1 - f), item.oldOrigin.x + shift, item.oldOrigin.y, item.rect.left + shift, item.oldWidth,
                             std::max(1 - f * 3, 0.0f), std::pow(1 - f, 0.4f));
                }
                // 今の候補: ぼかした所からくっきりさせる
                if (f > 0) {
                    DrawMask(item.newMask, f < 1 ? BlurAt(f) : 0, item.newOrigin.x + shift, item.newOrigin.y, item.rect.left + shift, item.newWidth, 0,
                             std::pow(f, 0.4f));
                }
                continue;
            }
            // グラデーションの文字から、写した文字そのものへ戻す (寄せ終わっているので、ずれは無い)
            for (LONG y = item.rect.top; y < item.rect.bottom; y++) {
                if (underline[y]) continue;
                for (LONG x = item.rect.left; x < item.rect.right; x++) {
                    float m = item.newMask.values[static_cast<size_t>(y - item.rect.top + pad) * item.newMask.width + (x - item.rect.left + pad)];
                    Color gradient = Mix(backgroundColor, GradientAt(static_cast<float>(x - item.rect.left) / static_cast<float>(std::max(item.newWidth, 1))), m);
                    Pixel(x, y) = ToPixel(Mix(gradient, FromPixel(Captured(x, y)), settle));
                }
            }
        }
    }

    // 背景の色で覆うだけ (アプリが描き直すのを待つ間)
    void RenderCover() {
        std::memset(pixels, 0, static_cast<size_t>(bitmapWidth) * bitmapHeight * sizeof(uint32_t));
        for (const RECT& cover : covers) {
            for (LONG y = std::max(0L, cover.top); y < std::min<LONG>(bitmapHeight, cover.bottom); y++) {
                for (LONG x = std::max(0L, cover.left); x < std::min<LONG>(bitmapWidth, cover.right); x++) Pixel(x, y) = background | 0xFF000000u;
            }
        }
    }

    bool Present(BYTE alpha = 255) {
        POINT position = {bounds.left, bounds.top};
        SIZE size = {Width(), Height()};
        POINT source = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(hwnd, nullptr, &position, &size, memory, &source, 0, &blend, ULW_ALPHA)) {
            TipLog(L"Jev の入れ替えのウィンドウを描けませんでした: %lu", GetLastError());
            return false;
        }
        if (!IsWindowVisible(hwnd)) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    // 1 コマ進める (タイマーから)
    void Frame() {
        ULONGLONG now = GetTickCount64();
        if (phase == Phase::ThinkFading) {
            float t = static_cast<float>(now - fadeStart);
            if (t >= (failed ? kFailMs : kThinkFadeMs)) {
                Stop();
                return;
            }
            float f = failed ? Clamp01((t - kFailHoldMs) / (kFailMs - kFailHoldMs)) : t / kThinkFadeMs;
            if (failed) {
                RenderFailed(Clamp01(t / kFailRiseMs));
            } else {
                RenderThinking(now);
            }
            if (!Present(static_cast<BYTE>(std::lround(255 * (1 - f))))) Stop();
            return;
        }
        if (phase == Phase::Waiting) {
            if (!CaptureScreen(bounds, capture)) {
                TipLog(L"Jev の入れ替え: 画面を写せませんでした");
                Stop();
                return;
            }
            // アプリがまだ描き直していなければ、覆ったまま待つ (描き直しが見えないアプリでも、少し待ったら始める)
            if (!Repainted() && now - playStart < kPaintWaitMs) return;
            if (!Prepare()) {
                Stop();
                return;
            }
            phase = Phase::Morphing;
            // 色を戻すだけなら、入れ替え終わったところから
            morphStart = settleOnly ? now - static_cast<ULONGLONG>(kMorphMs) : now;
        }
        if (phase != Phase::Morphing) return;
        float t = static_cast<float>(now - morphStart);
        if (t >= kMorphMs + kSettleMs) {
            Stop();
            return;
        }
        Render(t);
        if (!Present()) Stop();
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

static bool EnsureMorphClass() {
    std::call_once(g_classOnce, [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = MorphWindow::Impl::WndProc;
        wc.hInstance = g_module;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        g_registered = true;
    });
    return g_registered;
}

MorphWindow::MorphWindow() : impl_(new Impl()) {}

MorphWindow::~MorphWindow() {
    impl_->Stop();
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->memory) DeleteDC(impl_->memory);
    delete impl_;
}

bool MorphWindow::Playing() const { return impl_->phase != Impl::Phase::Idle; }

void MorphWindow::Hide() { impl_->Stop(); }

void MorphWindow::Play(const std::vector<MorphClause>& clauses, LONG tailRight, bool settleOnly) {
    Impl& d = *impl_;
    // Jev を待つ表示を見せていたら、隠さずにそのまま移る (覆う・色を戻すまでの間に、待つ前の文字が透けて見えないように)
    if (d.phase == Impl::Phase::Thinking && d.thinkShown) {
        d.Reset();
    } else {
        d.Stop();
    }
    if (!d.Start(clauses, tailRight, settleOnly)) d.Stop();
}

bool MorphWindow::Impl::Start(const std::vector<MorphClause>& clauses, LONG tailRight, bool settle) {
    if (clauses.empty() || !EnsureWindow()) return false;

    // 文字の大きさ: 今の候補を Yu Gothic UI で描いた幅が、文節の四角形の幅に合う大きさ
    float emSum = 0;
    int emCount = 0;
    {
        TextDc probe(100);
        for (const MorphClause& clause : clauses) {
            int width = probe.Width(clause.to);
            LONG rectWidth = clause.rect.right - clause.rect.left;
            if (width > 0 && rectWidth > 0) emSum += 100.0f * static_cast<float>(rectWidth) / static_cast<float>(width), emCount++;
        }
    }
    if (emCount == 0) return false;
    em = std::clamp(emSum / static_cast<float>(emCount), 6.0f, 200.0f);
    pad = static_cast<int>(std::ceil(em * kBlurUnit * kMaxBlur * 3)) + 2;

    RECT united = {};
    int grow = 0;  // 前の候補のほうが広い分 (右へはみ出す)
    {
        TextDc text(em);
        for (const MorphClause& clause : clauses) {
            // 色を戻すだけのときは、前の候補は今の候補と同じ
            const std::wstring& from = settle ? clause.to : clause.from;
            if (IsRectEmpty(&clause.rect) || from.empty() || clause.to.empty()) continue;
            Item item;
            item.rect = clause.rect;
            item.from = from;
            item.to = clause.to;
            item.newWidth = clause.rect.right - clause.rect.left;
            item.oldWidth = settle ? item.newWidth : text.Width(from);
            grow += std::max(0, item.oldWidth - item.newWidth);
            UnionRect(&united, &united, &clause.rect);
            items.push_back(std::move(item));
        }
    }
    if (items.empty()) return false;
    settleOnly = settle;
    slide = !settle && tailRight >= 0;
    if (slide) std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rect.left < b.rect.left; });
    LONG right = slide ? std::max(tailRight, united.right) : united.right;
    RECT screen = {united.left - pad, united.top - pad, right + grow + pad, united.bottom + pad};
    if (static_cast<long long>(screen.right - screen.left) * (screen.bottom - screen.top) > kMaxPixels) {
        TipLog(L"Jev の入れ替え: 範囲が大きすぎるので見せません (%ldx%ld)", screen.right - screen.left, screen.bottom - screen.top);
        return false;
    }

    bounds = screen;
    for (Item& item : items) OffsetRect(&item.rect, -screen.left, -screen.top);
    lineRight = right - screen.left;
    if (slide) {
        covers = {{items.front().rect.left, united.top - screen.top, lineRight + grow, united.bottom - screen.top}};
    } else {
        for (const Item& item : items) covers.push_back(item.rect);
    }

    // 今の画面を写して覚え (描き直したかを見るため)、背景の色で文節を覆う。
    // このウィンドウを写しから外せないときは覆わない (覆うと、写したときにアプリの文字ではなく覆った色が写る)。
    // 色を戻すだけのときも覆わない (同じ文字なので隠さなくてよい。Jev を待つ表示を見せていれば、色を戻し始めるまでそのまま)
    if (!CaptureScreen(bounds, before) || !EnsureBitmap()) return false;
    background = MostCommon(before);
    if (excluded && !settle) {
        RenderCover();
        if (!Present()) return false;
    }
    playStart = GetTickCount64();
    phase = Phase::Waiting;
    SetTimer(hwnd, kFrameTimer, kFrameMs, nullptr);
    return true;
}

void MorphWindow::BeginThinking(const RECT& text) {
    Impl& d = *impl_;
    d.Stop();
    // 画面の写しから外せないウィンドウでは出さない (出したまま入れ替えに移ると、入れ替えで写す画面に、この表示が写る)
    if (IsRectEmpty(&text) || !d.EnsureWindow() || !d.excluded) return;
    RECT screen = {text.left - kThinkPad, text.top - kThinkPad, text.right + kThinkPad, text.bottom + kThinkPad};
    if (static_cast<long long>(screen.right - screen.left) * (screen.bottom - screen.top) > kMaxPixels) return;
    d.bounds = screen;
    RECT inner = text;
    OffsetRect(&inner, -screen.left, -screen.top);
    if (!CaptureScreen(d.bounds, d.capture) || !d.EnsureBitmap() || !d.PrepareThinking(inner)) {
        d.Stop();
        return;
    }
    d.thinkStart = GetTickCount64();
    d.phase = Impl::Phase::Thinking;
}

void MorphWindow::ThinkFrame() {
    Impl& d = *impl_;
    if (d.phase != Impl::Phase::Thinking) return;
    ULONGLONG now = GetTickCount64();
    if (now - d.thinkStart < kThinkDelayMs) return;
    d.RenderThinking(now);
    if (!d.Present()) {
        d.Stop();
        return;
    }
    d.thinkShown = true;
}

bool MorphWindow::Thinking() const { return impl_->phase == Impl::Phase::Thinking; }

bool MorphWindow::ThinkingShown() const { return impl_->phase == Impl::Phase::Thinking && impl_->thinkShown; }

void MorphWindow::EndThinking(bool failed) {
    Impl& d = *impl_;
    if (d.phase != Impl::Phase::Thinking) return;
    // 答えが無かったことは、Jev を待つ表示をまだ見せていなくても (すぐ失敗した) 赤くして知らせる
    if (!d.thinkShown && !failed) {
        d.Stop();
        return;
    }
    d.failed = failed;
    d.phase = Impl::Phase::ThinkFading;
    d.fadeStart = GetTickCount64();
    SetTimer(d.hwnd, kFrameTimer, kFrameMs, nullptr);
}

void MorphWindow::UnregisterClasses() {
    if (!g_registered) return;
    UnregisterClassW(kClassName, g_module);
    g_registered = false;
}

}  // namespace meltype
