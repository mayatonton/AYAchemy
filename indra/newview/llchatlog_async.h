#pragma once
//
// llchatlog_async.h - 非同期チャットログ書き出し（AYAchemy）
// 目的: onChat() 経路の同期I/Oを避け、UIヒッチを無くす
//
// 使い方:
//   - UIスレッドから: ChatLogAsync::instance().enqueue(chat);
//   - 起動/終了はプロセスに追従（スレッドは自動起動/停止）
// 設定（settings.xml）:
//   - ChatLogAsyncEnabled (bool, default 1)
//   - ChatLogFlushIntervalMs (U32, default 200)
//   - ChatLogFlushBatch (U32, default 64)
//
#include "llsingleton.h"
#include "llchat.h"
#include "llsd.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <atomic>

class ChatLogAsync : public LLSingleton<ChatLogAsync>
{
    LLSINGLETON(ChatLogAsync);
    ~ChatLogAsync();

public:
    // UIスレッドから呼ぶ：非常に軽い
    void enqueue(const LLChat& chat, const LLSD& args = LLSD());

    // 明示停止（通常はデストラクタで自動停止）
    void shutdown();

private:
    // ワーカースレッド本体
    void workerLoop();

    // バッチ吐き出し
    void flushBatch(std::deque<std::pair<LLChat, LLSD>>& batch);

private:
    std::mutex                      mMutex;
    std::condition_variable         mCv;
    std::deque<std::pair<LLChat, LLSD>> mQ;
    std::thread                     mThread;
    std::atomic<bool>               mRunning{false};

    // キャッシュ: 設定のスナップショット
    bool mEnabled = true;
    U32  mFlushIntervalMs = 200;
    U32  mFlushBatch = 64;
};
