// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5

using System.Net;
using System.Text;
using System.Text.Json;
using Meltype.Composition;

namespace Meltype.Tests;

/// <summary>Jev (OpenRouter の /api/alpha/decisions・TypeSafe 公式の /v1/systemone) への問い合わせ。本物の代わりに、送られた中身を記録して決めた答えを返す。</summary>
internal static class JevRankerTests
{
    internal sealed record SentRequest(HttpMethod Method, string? Authorization, string? ContentType, string? Body, Uri? Uri = null);

    /// <summary>送られたリクエストを記録し、Reply (送られた本文 → 状態コード・本文) で、DelayMs だけ遅らせて答える。</summary>
    internal sealed class FakeJev : HttpMessageHandler
    {
        private readonly List<SentRequest> _requests = [];
        private int _finished;

        public Func<string, (HttpStatusCode Status, string Body)> Reply { get; set; } = _ => (HttpStatusCode.OK, Answers());
        public int DelayMs { get; set; }

        public IReadOnlyList<SentRequest> Requests
        {
            get { lock (_requests) return [.. _requests]; }
        }

        /// <summary>答え終わった数。</summary>
        public int Finished => Volatile.Read(ref _finished);

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            var body = request.Content is null ? null : await request.Content.ReadAsStringAsync(cancellationToken);
            lock (_requests)
                _requests.Add(new SentRequest(request.Method, request.Headers.Authorization?.ToString(), request.Content?.Headers.ContentType?.ToString(), body, request.RequestUri));
            if (DelayMs > 0) await Task.Delay(DelayMs, cancellationToken);
            var (status, text) = Reply(body ?? "");
            Interlocked.Increment(ref _finished);
            return new HttpResponseMessage(status) { Content = new StringContent(text, Encoding.UTF8, "application/json") };
        }

        protected override HttpResponseMessage Send(HttpRequestMessage request, CancellationToken cancellationToken) =>
            SendAsync(request, cancellationToken).GetAwaiter().GetResult();
    }

    /// <summary>Jev の答えの形 ({"answers": {"c0": {"type": "choice", "choice": …}}})。</summary>
    internal static string Answers(params (string Id, string Choice)[] answers)
    {
        using var stream = new MemoryStream();
        using (var json = new Utf8JsonWriter(stream))
        {
            json.WriteStartObject();
            json.WriteStartObject("answers");
            foreach (var (id, choice) in answers)
            {
                json.WriteStartObject(id);
                json.WriteString("type", "choice");
                json.WriteString("choice", choice);
                json.WriteNumber("confidence", 0.9);
                json.WriteEndObject();
            }
            json.WriteEndObject();
            json.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }

    private static JevQuestion Question(int index, string reading, string before, string after, params string[] options) =>
        new(index, reading, options.Select(o => (o, before + o + after)).ToList());

    /// <summary>「私は | 共サッカーを | します」の 3 問。選択肢の文は、聞く文節だけを候補にして、ほかの文節は読みのまま (CompositionController.ApplyJev と同じ)。</summary>
    private static List<JevQuestion> Soccer() =>
    [
        Question(0, "わたしは", "", "きょうさっかーをします", "私は", "わたしは", "渡しは"),
        Question(1, "きょうさっかーを", "わたしは", "します", "共サッカーを", "きょうさっかーを", "京サッカーを", "今日サッカーを"),
        Question(2, "します", "わたしはきょうさっかーを", "", "します", "死ます"),
    ];

    private const string SoccerReading = "わたしはきょうさっかーをします";
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(2);

    [Test]
    public static void Body_HasModelStateAndOneQuestionPerClause()
    {
        var jev = new FakeJev();
        using var ranker = new JevRanker("key", jev);
        ranker.Choose(SoccerReading, Soccer(), Wait);
        var request = jev.Requests.Single();
        Assert.Equal(HttpMethod.Post, request.Method);
        Assert.Equal("Bearer key", request.Authorization, "キーを Bearer で送る");
        Assert.Equal("application/json", request.ContentType);
        using var body = JsonDocument.Parse(request.Body!);
        var root = body.RootElement;
        Assert.Equal("https://openrouter.ai/api/alpha/decisions", request.Uri?.ToString(), "既定は OpenRouter");
        Assert.Equal("typesafe/jev-1.14", root.GetProperty("model").GetString());
        var state = root.GetProperty("state").GetString()!;
        Assert.True(state.EndsWith(": " + SoccerReading), "文は読み: " + state);
        var questions = root.GetProperty("questions");
        Assert.Equal("c0,c1,c2", string.Join(",", questions.EnumerateObject().Select(q => q.Name)), "文節ごとに c<番号>");
        var c1 = questions.GetProperty("c1");
        Assert.Equal("choice", c1.GetProperty("type").GetString());
        var instructions = c1.GetProperty("instructions").GetString()!;
        Assert.True(instructions.Contains("「きょうさっかーを」"), "聞く文節の読みを示す: " + instructions);
        var criteria = c1.GetProperty("criteria").EnumerateObject().ToDictionary(p => p.Name, p => p.Value.GetString());
        Assert.Equal(4, criteria.Count, "選択肢ごとに 1 つ");
        Assert.Equal("わたしは今日サッカーをします", criteria["今日サッカーを"], "候補 → その候補を入れた文");
        Assert.Equal("わたしは共サッカーをします", criteria["共サッカーを"]);
    }

    [Test]
    public static void TypeSafe_SendsTheSameQuestionsToTheOfficialApi()
    {
        var jev = new FakeJev { Reply = _ => (HttpStatusCode.OK, Answers(("c1", "今日サッカーを"))) };
        using var ranker = new JevRanker("ts-key", jev, provider: Config.JevProvider.TypeSafe);
        Assert.Equal("今日サッカーを", ranker.Choose(SoccerReading, Soccer(), Wait)?[1], "答えの形は OpenRouter と同じ");
        var request = jev.Requests.Single();
        Assert.Equal("https://api.typesafe.ai/v1/systemone", request.Uri?.ToString());
        Assert.Equal("Bearer ts-key", request.Authorization);
        using var body = JsonDocument.Parse(request.Body!);
        Assert.Equal("jev-1.14.0", body.RootElement.GetProperty("model").GetString(), "TypeSafe 公式のモデル名 (OpenRouter の typesafe/jev-1.14 ではない)");
        var state = body.RootElement.GetProperty("state").GetString()!;
        Assert.True(state.EndsWith(": " + SoccerReading), "文は読み: " + state);
    }

    [Test]
    public static void Choose_ReturnsTheChoicePerClause()
    {
        var jev = new FakeJev { Reply = _ => (HttpStatusCode.OK, Answers(("c0", "私は"), ("c1", "今日サッカーを"), ("c2", "します"))) };
        using var ranker = new JevRanker("key", jev);
        var picks = ranker.Choose(SoccerReading, Soccer(), Wait);
        Assert.True(picks is not null, "答えがある");
        Assert.Equal("今日サッカーを", picks![1]);
        Assert.Equal("私は", picks[0]);
        Assert.Equal(3, picks.Count);
    }

    [Test]
    public static void Choose_IgnoresAChoiceThatIsNotAnOption()
    {
        var jev = new FakeJev { Reply = _ => (HttpStatusCode.OK, Answers(("c0", "私は"), ("c1", "昨日サッカーを"))) };
        using var ranker = new JevRanker("key", jev);
        var picks = ranker.Choose(SoccerReading, Soccer(), Wait)!;
        Assert.True(!picks.ContainsKey(1), "選択肢に無い答えの文節は返さない");
        Assert.Equal("私は", picks[0]);
    }

    [Test]
    public static void Choose_SameSentence_IsNotSentTwice()
    {
        var jev = new FakeJev { Reply = _ => (HttpStatusCode.OK, Answers(("c1", "今日サッカーを"))) };
        using var ranker = new JevRanker("key", jev);
        ranker.Choose(SoccerReading, Soccer(), Wait);
        var again = ranker.Choose(SoccerReading, Soccer(), Wait);
        Assert.Equal(1, jev.Requests.Count, "2 回目は控えから");
        Assert.Equal("今日サッカーを", again?[1]);
    }

    [Test]
    public static void Choose_TimedOut_ReturnsNullAndKeepsTheLateAnswer()
    {
        var jev = new FakeJev { DelayMs = 300, Reply = _ => (HttpStatusCode.OK, Answers(("c1", "今日サッカーを"))) };
        using var ranker = new JevRanker("key", jev);
        Assert.True(ranker.Choose(SoccerReading, Soccer(), TimeSpan.FromMilliseconds(20)) is null, "間に合わなければ null");
        Assert.True(SpinWait.SpinUntil(() => jev.Finished == 1, 3000), "答えが届く");
        var late = ranker.Choose(SoccerReading, Soccer(), Wait);
        Assert.Equal("今日サッカーを", late?[1], "後から届いた答えを使う");
        Assert.Equal(1, jev.Requests.Count, "送り直さない");
    }

    [Test]
    public static void Choose_AfterTimeouts_AsksAgainRightAway()
    {
        var jev = new FakeJev { DelayMs = 500 };
        using var ranker = new JevRanker("key", jev);
        var quick = TimeSpan.FromMilliseconds(10);
        ranker.Choose("一", [Question(0, "いち", "", "", "一", "いち")], quick);
        ranker.Choose("二", [Question(0, "に", "", "", "二", "に")], quick);
        ranker.Choose("三", [Question(0, "さん", "", "", "三", "さん")], quick);
        Assert.Equal(3, jev.Requests.Count, "時間切れが続いても休まずに送る");
        Assert.True(ranker.IsAvailable, "時間切れでは止めない");
    }

    [Test]
    public static void Choose_AfterAFailedReply_SendsAgainOnTheNextPress()
    {
        var failures = new (HttpStatusCode Status, string Body)[]
        {
            (HttpStatusCode.Unauthorized, "{\"error\":{}}"), (HttpStatusCode.Forbidden, "{\"error\":{}}"),
            (HttpStatusCode.PaymentRequired, "{\"error\":{}}"), (HttpStatusCode.TooManyRequests, "{}"), ((HttpStatusCode)529, "{}"),
            (HttpStatusCode.InternalServerError, ""), (HttpStatusCode.OK, "<html>"), (HttpStatusCode.OK, "{\"usage\":{}}"),
        };
        foreach (var (status, text) in failures)
        {
            var jev = new FakeJev { Reply = _ => (status, text) };
            using var ranker = new JevRanker("key", jev);
            for (var i = 0; i < 4; i++) Assert.True(ranker.Choose(SoccerReading, Soccer(), Wait) is null, $"{(int)status} {text}: 答えなし");
            Assert.Equal(4, jev.Requests.Count, $"{(int)status} {text}: 同じ文でも押すたびに送り直す");
            Assert.True(ranker.IsAvailable, $"{(int)status} {text}: 止めない");
            jev.Reply = _ => (HttpStatusCode.OK, Answers(("c1", "今日サッカーを")));
            Assert.Equal("今日サッカーを", ranker.Choose(SoccerReading, Soccer(), Wait)?[1], $"{(int)status} {text}: 直ったらすぐ答えを使う");
        }
    }

    [Test]
    public static void WarmUp_SendsOnlyAHeadRequestOncePerMinute()
    {
        long clock = 0;
        var jev = new FakeJev();
        using var ranker = new JevRanker("key", jev, () => clock);
        ranker.WarmUp();
        Assert.True(SpinWait.SpinUntil(() => jev.Finished == 1, 3000), "接続を作りに行く");
        var request = jev.Requests.Single();
        Assert.Equal(HttpMethod.Head, request.Method);
        Assert.True(request.Authorization is null, "キーを送らない");
        Assert.True(request.Body is null, "本文を送らない");
        clock += 59_999;
        ranker.WarmUp();
        Thread.Sleep(50);
        Assert.Equal(1, jev.Requests.Count, "60 秒以内なら 2 度送らない");
        clock += 2;
        ranker.WarmUp();
        Assert.True(SpinWait.SpinUntil(() => jev.Finished == 2, 3000), "60 秒たったら温め直す");
    }
}
