// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5

using System.Buffers;
using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Meltype.Config;
using Meltype.Diagnostics;

namespace Meltype.Composition;

/// <summary>Jev に聞く 1 つの文節。Options は (候補, その候補を入れ、ほかの文節は読みのままにした文)。先頭が今の 1 番目。</summary>
public sealed record JevQuestion(int Index, string Reading, IReadOnlyList<(string Text, string Sentence)> Options);

/// <summary>
/// Jev (TypeSafe の決定モデル) に、文節ごとにどの候補が正しいかを 1 回で聞く。問い合わせ先は OpenRouter (/api/alpha/decisions) か
/// TypeSafe 公式 (/v1/systemone)。どちらも送る JSON・答えの形は同じで、URL とモデル名だけが違う。
/// UI スレッドから呼ぶ。待つのは渡した時間まで。間に合わなかった答えは控えに入れ、同じ文をもう一度変換したときに使う。
/// </summary>
public sealed class JevRanker : IDisposable
{
    // 控えの数 (ZenzaiConverter.MaxCache と同じ)
    private const int MaxCache = 256;
    // 2 回続けて時間切れになったら休む (ZenzaiConverter.BackoffMs と同じ)
    private const int TimeoutsBeforeBackoff = 2;
    private const int TimeoutBackoffMs = 5000;
    // 429 / 529 (混雑) のとき休む (GitHubIssueTitles.RetryAfter と同じ)
    private const int BusyBackoffMs = 60_000;
    // 通信エラー・読めない応答がこれだけ続いたら止める (ZenzaiConverter と同じ)
    private const int MaxFailures = 3;
    // 接続を温め直す間隔。接続の使い回しの期限 (PooledConnectionIdleTimeout) より短く
    private const int WarmUpIntervalMs = 60_000;

    private static readonly JsonWriterOptions WriterOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    private readonly string _apiKey;
    private readonly JevProvider _provider;
    private readonly Uri _endpoint;
    private readonly string _model;
    private readonly HttpClient _http;
    private readonly Func<long> _now;
    private readonly object _gate = new();
    // 送る JSON → 文節の番号ごとの答え
    private readonly Dictionary<string, IReadOnlyDictionary<int, string>> _cache = new(StringComparer.Ordinal);
    // 送る JSON → 送っている途中の問い合わせ (同じものを 2 度送らない)
    private readonly Dictionary<string, Task<IReadOnlyDictionary<int, string>?>> _pending = new(StringComparer.Ordinal);
    private int _timeouts;
    private int _failures;
    private long _pausedUntil = long.MinValue;
    private long? _lastSent;
    private volatile bool _disabled;
    private volatile bool _disposed;

    public JevRanker(JevProvider provider, string apiKey) : this(apiKey, new SocketsHttpHandler
    {
        PooledConnectionIdleTimeout = TimeSpan.FromMinutes(2),
        ConnectTimeout = TimeSpan.FromSeconds(3),
    }, provider: provider)
    {
    }

    /// <summary>テスト用 (偽の通信と時計)。</summary>
    internal JevRanker(string apiKey, HttpMessageHandler handler, Func<long>? now = null, JevProvider provider = JevProvider.OpenRouter)
    {
        _apiKey = apiKey;
        _provider = provider;
        (_endpoint, _model) = Service(provider);
        _http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(10) };
        _now = now ?? (() => Environment.TickCount64);
    }

    /// <summary>問い合わせ先の URL と、そこでの Jev 1.14 のモデル名。</summary>
    internal static (Uri Endpoint, string Model) Service(JevProvider provider) => provider switch
    {
        JevProvider.TypeSafe => (new Uri("https://api.typesafe.ai/v1/systemone"), "jev-1.14.0"),
        _ => (new Uri("https://openrouter.ai/api/alpha/decisions"), "typesafe/jev-1.14"),
    };

    /// <summary>使えるか (キーが違う・クレジットが無い・失敗が続いたら false)。</summary>
    public bool IsAvailable => !_disabled && !_disposed;

    /// <summary>文節の番号 → Jev が一番上に選んだ候補。reading は変換中の文の読み。答えが無い・間に合わない・使えないときは null。</summary>
    public IReadOnlyDictionary<int, string>? Choose(string reading, IReadOnlyList<JevQuestion> questions, TimeSpan timeout)
    {
        if (!IsAvailable || questions.Count == 0) return null;
        var started = Environment.TickCount64;
        var body = Body(_model, reading, questions);
        var key = Encoding.UTF8.GetString(body);
        Task<IReadOnlyDictionary<int, string>?>? task;
        lock (_gate)
        {
            if (_now() < _pausedUntil)
            {
                Report("休み中");
                return null;
            }
            if (_cache.TryGetValue(key, out var cached))
            {
                Report("控え");
                return cached;
            }
            if (!_pending.TryGetValue(key, out task))
            {
                _lastSent = _now();
                task = Send(key, body, questions);
                _pending[key] = task;
            }
        }
        if (!task.Wait(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout))
        {
            // 届いた答えは Send の中で控えに入れる (同じ文をもう一度変換したときに使う)
            lock (_gate)
            {
                if (++_timeouts >= TimeoutsBeforeBackoff)
                {
                    _timeouts = 0;
                    _pausedUntil = _now() + TimeoutBackoffMs;
                }
            }
            Report("時間切れ");
            return null;
        }
        var picks = task.Result;
        if (picks is null)
        {
            Report("失敗");
            return null;
        }
        lock (_gate) _timeouts = 0;
        Report($"答えあり ({picks.Count} 文節)");
        return picks;

        void Report(string result) =>
            Log.Info($"Jev: {questions.Count} 問、{Environment.TickCount64 - started}ms、{result} {Log.Text(reading)}");
    }

    /// <summary>接続だけを作っておく (打った文字は送らない)。前の通信から 60 秒以上たっていなければ何もしない。</summary>
    public void WarmUp()
    {
        if (!IsAvailable) return;
        lock (_gate)
        {
            var now = _now();
            if (now < _pausedUntil || _lastSent is { } last && now - last < WarmUpIntervalMs) return;
            _lastSent = now;
        }
        _ = Task.Run(async () =>
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Head, _endpoint);
                using var response = await _http.SendAsync(request).ConfigureAwait(false);
            }
            catch (Exception ex) when (IsExpected(ex))
            {
            }
        });
    }

    public void Dispose()
    {
        _disposed = true;
        _http.Dispose();
    }

    /// <summary>
    /// 送る JSON。state は変換中の文の読み (今の変換は見せない)。state・instructions・選択肢の文の作り方は、
    /// 送り方を比べた評価で一番安定したものと同じ。
    /// 評価では名前を並べ替えて送った (JSON のキーを辞書順) ので、同じ並びにする (選択肢の並びが変わると答えが変わりうる)。
    /// </summary>
    internal static byte[] Body(string model, string reading, IReadOnlyList<JevQuestion> questions)
    {
        var buffer = new ArrayBufferWriter<byte>();
        using (var json = new Utf8JsonWriter(buffer, WriterOptions))
        {
            json.WriteStartObject();
            json.WriteString("model", model);
            json.WriteStartObject("questions");
            foreach (var question in questions.OrderBy(q => Id(q.Index), StringComparer.Ordinal))
            {
                json.WriteStartObject(Id(question.Index));
                json.WriteStartObject("criteria");
                foreach (var (text, whole) in question.Options.DistinctBy(o => o.Text).OrderBy(o => o.Text, StringComparer.Ordinal))
                    json.WriteString(text, whole);
                json.WriteEndObject();
                json.WriteString("instructions", $"変換中の文の「{question.Reading}」の部分を、自然で正しい日本語として書いた文はどれか");
                json.WriteString("type", "choice");
                json.WriteEndObject();
            }
            json.WriteEndObject();
            json.WriteString("state", "日本語入力で変換中の文: " + reading);
            json.WriteEndObject();
        }
        return buffer.WrittenSpan.ToArray();
    }

    private static string Id(int index) => "c" + index;

    /// <summary>送って答えを読む。結果 (状態コード・答え) はここで控え・回数に入れるので、時間切れで待つのをやめた後に届いても使える。</summary>
    private Task<IReadOnlyDictionary<int, string>?> Send(string key, byte[] body, IReadOnlyList<JevQuestion> questions) => Task.Run(async () =>
    {
        HttpStatusCode? status = null;
        IReadOnlyDictionary<int, string>? picks = null;
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, _endpoint) { Content = new ByteArrayContent(body) };
            request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json");
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _apiKey);
            using var response = await _http.SendAsync(request).ConfigureAwait(false);
            status = response.StatusCode;
            if (status == HttpStatusCode.OK) picks = Parse(await response.Content.ReadAsStringAsync().ConfigureAwait(false), questions);
        }
        catch (Exception ex) when (IsExpected(ex))
        {
        }
        Record(key, status, picks);
        return picks;
    });

    /// <summary>answers の c&lt;番号&gt; ごとの choice。選択肢に無い答えの文節は入れない。answers が無い・読めなければ null。</summary>
    internal static IReadOnlyDictionary<int, string>? Parse(string text, IReadOnlyList<JevQuestion> questions)
    {
        try
        {
            using var json = JsonDocument.Parse(text);
            if (json.RootElement.ValueKind != JsonValueKind.Object ||
                !json.RootElement.TryGetProperty("answers", out var answers) || answers.ValueKind != JsonValueKind.Object) return null;
            var picks = new Dictionary<int, string>();
            foreach (var question in questions)
            {
                if (answers.TryGetProperty(Id(question.Index), out var answer) && answer.ValueKind == JsonValueKind.Object &&
                    answer.TryGetProperty("choice", out var choice) && choice.ValueKind == JsonValueKind.String &&
                    choice.GetString() is { } picked && question.Options.Any(o => o.Text == picked))
                    picks[question.Index] = picked;
            }
            return picks;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private void Record(string key, HttpStatusCode? status, IReadOnlyDictionary<int, string>? picks)
    {
        lock (_gate)
        {
            _pending.Remove(key);
            switch (status)
            {
                case HttpStatusCode.OK when picks is not null:
                    if (_cache.Count >= MaxCache) _cache.Clear();
                    _cache[key] = picks;
                    _failures = 0;
                    break;
                case HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden:
                    Disable($"Jev: {ProviderName} の API キーが使えません ({(int)status})。Jev を使わずに変換します");
                    break;
                case HttpStatusCode.PaymentRequired:
                    Disable($"Jev: {ProviderName} のクレジットがありません (402)。Jev を使わずに変換します");
                    break;
                case HttpStatusCode.TooManyRequests or (HttpStatusCode)529:
                    _pausedUntil = _now() + BusyBackoffMs;
                    break;
                default:
                    if (++_failures >= MaxFailures) Disable($"Jev: {MaxFailures} 回続けて答えが得られませんでした。Jev を使わずに変換します");
                    break;
            }
        }
    }

    private string ProviderName => _provider == JevProvider.TypeSafe ? "TypeSafe" : "OpenRouter";

    private void Disable(string message)
    {
        if (_disabled || _disposed) return;
        _disabled = true;
        Log.Warn(message);
    }

    // ZenzaiConverter.Complete と同じもの。作り直しで捨てた後に届いたもの (ObjectDisposedException) も外に出さない
    private static bool IsExpected(Exception ex) =>
        ex is HttpRequestException or TaskCanceledException or OperationCanceledException or JsonException or IOException or ObjectDisposedException;
}
