// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5

using System.Diagnostics;
using System.Drawing.Drawing2D;

namespace Meltype.Composition;

/// <summary>
/// Jev が候補を変えた文節にかける、ふわっと浮かんで消える光。変換ボックスの中に重ねて描くだけで、別のウィンドウは作らない。
/// 水色から紫の淡い光が 120ms で出て、少し上に浮きながら、左から右へ光の帯が流れ、小さな粒がいくつか昇って、700ms で消える。
/// </summary>
internal sealed class JevGlow : IDisposable
{
    private const double DurationMs = 700;
    private const double FadeInMs = 120;
    private static readonly Color Cyan = Color.FromArgb(80, 220, 255);
    private static readonly Color Violet = Color.FromArgb(170, 120, 255);

    private readonly Control _owner;
    private readonly System.Windows.Forms.Timer _timer = new() { Interval = 16 };
    private long _start;

    public JevGlow(Control owner)
    {
        _owner = owner;
        _timer.Tick += (_, _) => Tick();
    }

    /// <summary>光らせている最中か。</summary>
    public bool Running => _timer.Enabled;

    /// <summary>最初から光らせる。</summary>
    public void Start()
    {
        _start = Stopwatch.GetTimestamp();
        _timer.Start();
        _owner.Invalidate();
    }

    /// <summary>すぐに消す (描いていた光も消すため描き直す)。</summary>
    public void Stop()
    {
        if (!_timer.Enabled) return;
        _timer.Stop();
        _owner.Invalidate();
    }

    private double ElapsedMs => Stopwatch.GetElapsedTime(_start).TotalMilliseconds;

    private void Tick()
    {
        if (ElapsedMs >= DurationMs) Stop();
        else _owner.Invalidate();
    }

    /// <summary>
    /// 文節の四角 (文字を描いた位置) ごとに光を描く。unit は 96 DPI・倍率 1 のときの 1 ピクセルに当たる大きさ。
    /// </summary>
    public void Paint(Graphics g, IReadOnlyList<RectangleF> clauses, float unit)
    {
        if (!Running || clauses.Count == 0) return;
        var ms = ElapsedMs;
        var t = Math.Clamp(ms / DurationMs, 0, 1);
        // 出るときは速く、消えるときはゆっくり
        var fadeIn = EaseOut(Math.Clamp(ms / FadeInMs, 0, 1), 2);
        var fadeOut = t < 0.4 ? 1 : 1 - SmoothStep((t - 0.4) / 0.6);
        var strength = fadeIn * fadeOut;
        if (strength <= 0.003) return;
        // 少しずつ上に浮く
        var drift = (float)(-EaseOut(t, 3) * 4 * unit);

        var state = g.Save();
        g.SmoothingMode = SmoothingMode.AntiAlias;
        for (var i = 0; i < clauses.Count; i++)
        {
            var body = clauses[i];
            body.Offset(0, drift);
            body.Inflate(2 * unit, unit);
            if (body.Width <= 1 || body.Height <= 1) continue;
            PaintHalo(g, body, strength, unit);
            PaintShimmer(g, body, t, strength, unit);
            PaintSparkles(g, body, i, t, strength, unit);
        }
        g.Restore(state);
    }

    /// <summary>ぼかした角丸の光。少しずつ大きくした角丸を薄く重ねて、縁をぼかす。</summary>
    private static void PaintHalo(Graphics g, RectangleF body, double strength, float unit)
    {
        for (var k = 3; k >= 0; k--)
        {
            var rect = body;
            rect.Inflate(k * 1.5F * unit, k * 1.5F * unit);
            var alpha = (int)(22 * strength);
            if (alpha <= 0) continue;
            using var path = RoundedRect(rect, Math.Min(rect.Height / 2, 6 * unit + k * 1.5F * unit));
            using var brush = new LinearGradientBrush(rect, Color.FromArgb(alpha, Cyan), Color.FromArgb(alpha, Violet), LinearGradientMode.Horizontal)
            {
                WrapMode = WrapMode.TileFlipX,
            };
            g.FillPath(brush, path);
        }
        // 細い縁で、近未来っぽく輪郭を少しだけ見せる
        var rimAlpha = (int)(70 * strength);
        if (rimAlpha <= 0) return;
        using var rimPath = RoundedRect(body, Math.Min(body.Height / 2, 6 * unit));
        using var rimBrush = new LinearGradientBrush(body, Color.FromArgb(rimAlpha, Cyan), Color.FromArgb(rimAlpha, Violet), LinearGradientMode.Horizontal)
        {
            WrapMode = WrapMode.TileFlipX,
        };
        using var rim = new Pen(rimBrush, Math.Max(1F, unit));
        g.DrawPath(rim, rimPath);
    }

    /// <summary>左から右へ流れる光の帯。角丸の中だけに描く。</summary>
    private static void PaintShimmer(Graphics g, RectangleF body, double t, double strength, float unit)
    {
        var progress = Math.Clamp((t - 0.12) / 0.6, 0, 1);
        if (progress <= 0 || progress >= 1) return;
        var alpha = (int)(110 * strength);
        if (alpha <= 0) return;
        var bandWidth = Math.Max(10 * unit, body.Width * 0.35F);
        var center = body.Left - bandWidth / 2 + (float)SmoothStep(progress) * (body.Width + bandWidth);
        var band = new RectangleF(center - bandWidth / 2, body.Top, bandWidth, body.Height);
        using var path = RoundedRect(body, Math.Min(body.Height / 2, 6 * unit));
        using var brush = new LinearGradientBrush(band, Color.Transparent, Color.Transparent, LinearGradientMode.Horizontal)
        {
            WrapMode = WrapMode.TileFlipX,
            InterpolationColors = new ColorBlend
            {
                Colors = [Color.FromArgb(0, 255, 255, 255), Color.FromArgb(alpha, 235, 250, 255), Color.FromArgb(0, 255, 255, 255)],
                Positions = [0F, 0.5F, 1F],
            },
        };
        var state = g.Save();
        g.SetClip(path, CombineMode.Intersect);
        g.FillRectangle(brush, band);
        g.Restore(state);
    }

    /// <summary>文節の下から昇って消える小さな粒 (4 個)。位置は文節の番号で決め、毎回同じに出す。</summary>
    private static void PaintSparkles(Graphics g, RectangleF body, int clause, double t, double strength, float unit)
    {
        ReadOnlySpan<float> columns = [0.18F, 0.44F, 0.68F, 0.88F];
        for (var k = 0; k < columns.Length; k++)
        {
            var delay = 0.08 + k * 0.09 + (clause % 2) * 0.04;
            var p = (t - delay) / 0.45;
            if (p <= 0 || p >= 1) continue;
            var alpha = Math.Sin(Math.PI * p) * Math.Min(1, strength * 1.5);
            if (alpha <= 0.01) continue;
            var column = columns[(k + clause) % columns.Length];
            var x = body.Left + body.Width * column;
            var y = body.Bottom - (float)p * (body.Height * 0.9F + 6 * unit);
            var size = (1.2F + 0.25F * (k % 3)) * unit * (float)(1 - 0.4 * p);

            using (var halo = new SolidBrush(Color.FromArgb((int)(60 * alpha), k % 2 == 0 ? Cyan : Violet)))
            {
                g.FillEllipse(halo, x - size * 3, y - size * 3, size * 6, size * 6);
            }
            using (var core = new SolidBrush(Color.FromArgb((int)(220 * alpha), 235, 250, 255)))
            {
                g.FillEllipse(core, x - size, y - size, size * 2, size * 2);
            }
            using var cross = new Pen(Color.FromArgb((int)(120 * alpha), 235, 250, 255), Math.Max(0.6F, 0.6F * unit));
            g.DrawLine(cross, x - size * 3, y, x + size * 3, y);
            g.DrawLine(cross, x, y - size * 3, x, y + size * 3);
        }
    }

    private static GraphicsPath RoundedRect(RectangleF rect, float radius)
    {
        var path = new GraphicsPath();
        var d = Math.Max(0.5F, Math.Min(radius * 2, Math.Min(rect.Width, rect.Height)));
        path.AddArc(rect.Left, rect.Top, d, d, 180, 90);
        path.AddArc(rect.Right - d, rect.Top, d, d, 270, 90);
        path.AddArc(rect.Right - d, rect.Bottom - d, d, d, 0, 90);
        path.AddArc(rect.Left, rect.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    private static double EaseOut(double x, int power) => 1 - Math.Pow(1 - x, power);

    private static double SmoothStep(double x)
    {
        x = Math.Clamp(x, 0, 1);
        return x * x * (3 - 2 * x);
    }

    public void Dispose() => _timer.Dispose();
}
