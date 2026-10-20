// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 lnkiai
//
// Jev が変えた文節を、最初の Space のときに一度だけふわっと光らせる (約 0.7 秒で消える)。
// アプリのプロセスの中に出す、クリックを素通しし、フォーカスを奪わないウィンドウ。

#pragma once

#include <vector>

#include "Globals.h"

namespace meltype {

class GlowWindow {
public:
    GlowWindow();
    ~GlowWindow();
    GlowWindow(const GlowWindow&) = delete;
    GlowWindow& operator=(const GlowWindow&) = delete;

    // clauses は光らせる文節の画面上の四角形。最初から再生し直す (描くのはタイマーの中なので、すぐ戻る)
    void Play(const std::vector<RECT>& clauses);
    void Hide();
    bool Playing() const;

    // DllMain (DLL_PROCESS_DETACH) から呼ぶ
    static void UnregisterClasses();

    struct Impl;

private:
    Impl* impl_;
};

}  // namespace meltype
