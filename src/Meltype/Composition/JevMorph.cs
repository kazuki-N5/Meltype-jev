// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5

using System.Diagnostics;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace Meltype.Composition;

/// <summary>
/// Jev が候補を変えた文節を、変える前の候補から今の候補へ溶けるように入れ替えて見せる (Meltype IME の MorphWindow と同じ動き)。
/// 前の候補はぼやけながら、今の候補はぼやけた所からくっきりしながら同時に入れ替わり (0.28 秒)、その間だけ青 → 紫 → ピンクの
/// グラデーションで描いて、0.18 秒でふつうの文字の色に戻す。文節の幅は前の候補の幅から今の候補の幅へ動かし、後ろの文節を寄せる。
/// ぼかしは文字の大きさの 0.12 倍を単位に「単位 × (1 / 進み具合 − 1)」(最大で単位の 5 倍)、不透明度は 進み具合^0.4。
/// </summary>
internal sealed class JevMorph : IDisposable
{
    private const double MorphMs = 280;
    private const double SettleMs = 180;
    private const float BlurUnit = 0.12F;
    private const float MaxBlur = 5;
    private const TextFormatFlags Flags = TextFormatFlags.NoPrefix | TextFormatFlags.NoPadding;
    private static readonly Color[] Gradient = [Color.FromArgb(0x42, 0x85, 0xf4), Color.FromArgb(0x9b, 0x72, 0xcb), Color.FromArgb(0xd9, 0x65, 0x70)];

    private readonly Control _owner;
    private readonly System.Windows.Forms.Timer _timer = new() { Interval = 16 };
    private long _start;
    private Dictionary<int, string> _from = [];
    // 文字の形 (文字列とフォントごと)。入れ替えている間だけ持つ
    private readonly Dictionary<(string Text, Font Font), Mask> _masks = [];
    private float[] _blurred = [];
    private float[] _work = [];

    public JevMorph(Control owner)
    {
        _owner = owner;
        _timer.Tick += (_, _) => Tick();
    }

    /// <summary>入れ替えて見せている最中か。</summary>
    public bool Running => _timer.Enabled;

    /// <summary>最初から入れ替えて見せる。</summary>
    public void Start(IReadOnlyList<JevChange> changes)
    {
        _from = changes.ToDictionary(c => c.Clause, c => c.From);
        _masks.Clear();
        _start = Stopwatch.GetTimestamp();
        _timer.Start();
        _owner.Invalidate();
    }

    /// <summary>すぐにやめる (ふつうの表示に描き直す)。</summary>
    public void Stop()
    {
        if (!_timer.Enabled) return;
        _timer.Stop();
        _masks.Clear();
        _owner.Invalidate();
    }

    private double ElapsedMs => Stopwatch.GetElapsedTime(_start).TotalMilliseconds;

    private void Tick()
    {
        if (ElapsedMs >= MorphMs + SettleMs) Stop();
        else _owner.Invalidate();
    }

    private float Progress => (float)Math.Clamp(ElapsedMs / MorphMs, 0, 1);

    /// <summary>グラデーションからふつうの文字の色へ戻している最中か (このときは、ふつうの文字を先に描いておく)。</summary>
    public bool Settling => Running && ElapsedMs >= MorphMs;

    /// <summary>文節 clause を入れ替えている最中なら、変える前の候補。そうでなければ null。</summary>
    public string? From(int clause) => Running && _from.TryGetValue(clause, out var from) ? from : null;

    /// <summary>今見せる文節の幅 (前の候補の幅から今の候補の幅へ)。</summary>
    public int Width(int fromWidth, int toWidth)
    {
        var t = Progress;
        var e = t < 0.5F ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) / 2;
        return (int)Math.Round(fromWidth + (toWidth - fromWidth) * e);
    }

    /// <summary>
    /// 文節を (x, y) に描く。背景は塗ってある前提。Settling のときは、ふつうの文字を描いた上にグラデーションの文字を薄く重ねる。
    /// </summary>
    public void Paint(Graphics g, string from, string to, Font font, Point location, int fromWidth, int toWidth, Color text)
    {
        var em = font.SizeInPoints * g.DpiY / 72;
        var unit = em * BlurUnit;
        var pad = (int)Math.Ceiling(unit * MaxBlur * 3) + 2;
        var newMask = MaskOf(to, font, pad);
        var elapsed = ElapsedMs;
        if (elapsed >= MorphMs)
        {
            var settle = (float)Math.Clamp((elapsed - MorphMs) / SettleMs, 0, 1);
            DrawMask(g, newMask, 0, location, toWidth, text, 0, 1 - settle);
            return;
        }
        var f = Progress;
        float BlurAt(float x) => x <= 0 ? unit * MaxBlur : Math.Min(unit * (1 / x - 1), unit * MaxBlur);
        // 前の候補: 文字の色から少しずつグラデーションに変えながら、ぼかして消す
        if (f < 1) DrawMask(g, MaskOf(from, font, pad), BlurAt(1 - f), location, fromWidth, text, Math.Max(1 - f * 3, 0), MathF.Pow(1 - f, 0.4F));
        // 今の候補: ぼかした所からくっきりさせる
        if (f > 0) DrawMask(g, newMask, f < 1 ? BlurAt(f) : 0, location, toWidth, text, 0, MathF.Pow(f, 0.4F));
    }

    /// <summary>文字の形 (0..1 の濃さ)。まわりに、ぼかしがはみ出す分の余白 Pad を持つ。</summary>
    private sealed record Mask(int Width, int Height, int Pad, float[] Values);

    private Mask MaskOf(string text, Font font, int pad)
    {
        if (_masks.TryGetValue((text, font), out var cached) && cached.Pad == pad) return cached;
        var size = TextRenderer.MeasureText(text, font, Size.Empty, Flags);
        var width = Math.Max(1, size.Width) + pad * 2;
        var height = Math.Max(1, size.Height) + pad * 2;
        using var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bitmap))
        {
            g.Clear(Color.Black);
            TextRenderer.DrawText(g, text, font, new Point(pad, pad), Color.White, Color.Black, Flags);
        }
        var pixels = new int[width * height];
        var data = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
        bitmap.UnlockBits(data);
        // ClearType で色がにじむので、いちばん明るいチャンネルを濃さにする
        var values = new float[pixels.Length];
        for (var i = 0; i < pixels.Length; i++)
        {
            var p = pixels[i];
            values[i] = Math.Max((p >> 16) & 0xFF, Math.Max((p >> 8) & 0xFF, p & 0xFF)) / 255F;
        }
        var mask = new Mask(width, height, pad, values);
        _masks[(text, font)] = mask;
        return mask;
    }

    /// <summary>
    /// 形 mask を sigma でぼかし、文節の左端 location から幅 span のグラデーション (textMix の分だけ文字の色を混ぜる) で、不透明度 opacity で描く。
    /// </summary>
    private void DrawMask(Graphics g, Mask mask, float sigma, Point location, int span, Color text, float textMix, float opacity)
    {
        if (opacity <= 0.002F) return;
        GaussianBlur(mask, sigma);
        var pixels = new int[mask.Width * mask.Height];
        for (var y = 0; y < mask.Height; y++)
        {
            for (var x = 0; x < mask.Width; x++)
            {
                var a = _blurred[y * mask.Width + x] * opacity;
                if (a < 0.002F) continue;
                var color = GradientAt((x - mask.Pad) / (float)Math.Max(span, 1));
                if (textMix > 0) color = Mix(color, text, textMix);
                pixels[y * mask.Width + x] = ((int)Math.Min(255, a * 255 + 0.5F) << 24) | (color.R << 16) | (color.G << 8) | color.B;
            }
        }
        using var bitmap = new Bitmap(mask.Width, mask.Height, PixelFormat.Format32bppArgb);
        var data = bitmap.LockBits(new Rectangle(0, 0, mask.Width, mask.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(pixels, 0, data.Scan0, pixels.Length);
        bitmap.UnlockBits(data);
        g.DrawImageUnscaled(bitmap, location.X - mask.Pad, location.Y - mask.Pad);
    }

    private static Color GradientAt(float t)
    {
        t = Math.Clamp(t, 0, 1);
        return t < 0.5F ? Mix(Gradient[0], Gradient[1], t * 2) : Mix(Gradient[1], Gradient[2], (t - 0.5F) * 2);
    }

    private static Color Mix(Color a, Color b, float t) => Color.FromArgb(
        (int)(a.R + (b.R - a.R) * t + 0.5F), (int)(a.G + (b.G - a.G) * t + 0.5F), (int)(a.B + (b.B - a.B) * t + 0.5F));

    /// <summary>標準偏差 sigma のガウスぼかしを、箱ぼかし 3 回で近づけて _blurred に入れる。sigma が 1 より小さいときは、ぼかさない形と sigma = 1 の形を混ぜる。</summary>
    private void GaussianBlur(Mask mask, float sigma)
    {
        var length = mask.Values.Length;
        if (_blurred.Length < length) _blurred = new float[length];
        if (_work.Length < length) _work = new float[length];
        Array.Copy(mask.Values, _blurred, length);
        if (sigma <= 0.01F) return;
        var s = Math.Max(sigma, 1F);
        // 箱の幅 (Kutskir の方法)
        const int n = 3;
        var ideal = MathF.Sqrt(12 * s * s / n + 1);
        var lower = (int)MathF.Floor(ideal);
        if (lower % 2 == 0) lower--;
        var upper = lower + 2;
        var m = (int)MathF.Round((12 * s * s - n * lower * lower - 4 * n * lower - 3 * n) / (-4F * lower - 4));
        for (var i = 0; i < n; i++)
        {
            var r = ((i < m ? lower : upper) - 1) / 2;
            if (r <= 0) continue;
            BoxBlurH(_blurred, _work, mask.Width, mask.Height, r);
            BoxBlurV(_work, _blurred, mask.Width, mask.Height, r);
        }
        if (sigma < 1)
        {
            for (var i = 0; i < length; i++) _blurred[i] = mask.Values[i] + (_blurred[i] - mask.Values[i]) * sigma;
        }
    }

    private static void BoxBlurH(float[] src, float[] dst, int w, int h, int r)
    {
        var inv = 1F / (2 * r + 1);
        for (var y = 0; y < h; y++)
        {
            var row = y * w;
            var sum = 0F;
            for (var i = 0; i <= Math.Min(r, w - 1); i++) sum += src[row + i];
            for (var x = 0; x < w; x++)
            {
                dst[row + x] = sum * inv;
                if (x + r + 1 < w) sum += src[row + x + r + 1];
                if (x - r >= 0) sum -= src[row + x - r];
            }
        }
    }

    private static void BoxBlurV(float[] src, float[] dst, int w, int h, int r)
    {
        var inv = 1F / (2 * r + 1);
        for (var x = 0; x < w; x++)
        {
            var sum = 0F;
            for (var i = 0; i <= Math.Min(r, h - 1); i++) sum += src[i * w + x];
            for (var y = 0; y < h; y++)
            {
                dst[y * w + x] = sum * inv;
                if (y + r + 1 < h) sum += src[(y + r + 1) * w + x];
                if (y - r >= 0) sum -= src[(y - r) * w + x];
            }
        }
    }

    public void Dispose() => _timer.Dispose();
}
