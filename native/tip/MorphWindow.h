// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 kazuki-N5
//
// Jev が変えた文節を、最初の Space のときに一度だけ、変える前の候補から今の候補へ溶けるように入れ替えて見せる。
// 前の文字はぼやけながら、今の文字はぼやけた所からくっきりしながら同時に入れ替わり (0.28 秒)、その間だけ青 → 紫 → ピンクの
// グラデーションで描いて、0.18 秒でふつうの文字の色に戻す。文節の幅が変わるときは、後ろの文字を同じ行の中ですっと寄せる。
// 文字はアプリが描くので、変換したあとの画面を写し取って、その上に重ねたウィンドウで動かす (クリックは素通しし、フォーカスを奪わない)。
//
// Jev を待つ間 (最初の Space の応答が 0.12 秒たっても来ないとき) は、Space を押したときに見えていた変換中の文字の上を、
// 同じグラデーションの光が左から右へ流れる (0.7 秒で 1 回)。応答が来たら、Jev が候補を変えたなら入れ替え、
// Jev も同じ候補を選んだならグラデーションからふつうの色に戻すところだけを見せる。Jev の答えが無ければ (時間切れ・通信エラーなど)
// 文字全体を少し赤くしてから消していき、Jev に聞かなかったときは 0.15 秒で消していく。
// 応答を待つ間はアプリの画面のスレッドが止まっているので (タイマーが来ない)、待つ側が ThinkFrame を呼んで動かす。

#pragma once

#include <string>
#include <vector>

#include "Globals.h"

namespace meltype {

struct MorphClause {
    RECT rect;          // 文節の画面上の四角形 (今の候補を描いた位置)
    std::wstring from;  // Jev が変える前の候補
    std::wstring to;    // 今の候補
};

class MorphWindow {
public:
    MorphWindow();
    ~MorphWindow();
    MorphWindow(const MorphWindow&) = delete;
    MorphWindow& operator=(const MorphWindow&) = delete;

    // clauses は左から順に。tailRight は最後の文節に続く変換中の文字の右端 (すべて同じ行のとき)。-1 なら後ろの文字は寄せない。
    // 最初から再生し直す (画面を写すのも描くのもタイマーの中なので、すぐ戻る)。
    // settleOnly: 前の候補から入れ替えずに、グラデーションからふつうの色に戻すところだけを見せる (from は使わない)
    void Play(const std::vector<MorphClause>& clauses, LONG tailRight, bool settleOnly = false);
    void Hide();
    bool Playing() const;

    // Jev を待つ間の表示を用意する。text は変換中の文字の画面上の四角形 (1 行)。今の画面を写すだけで、まだ見せない
    void BeginThinking(const RECT& text);
    // 応答を待つ間に何度も呼ぶ。BeginThinking から 0.12 秒たったら、光を流して見せる
    void ThinkFrame();
    // Jev を待つ間の表示を用意している (応答が来て終わらせるまで) / 見せている
    bool Thinking() const;
    bool ThinkingShown() const;
    // Jev を待つ表示を終わらせる。failed (Jev の答えが無かった) なら、文字全体を少し赤くしてから消していく (まだ見せていなくても)。
    // そうでなければ、見せていたら光を流したまま消していき、見せていなければすぐやめる
    void EndThinking(bool failed = false);

    // DllMain (DLL_PROCESS_DETACH) から呼ぶ
    static void UnregisterClasses();

    struct Impl;

private:
    Impl* impl_;
};

}  // namespace meltype
