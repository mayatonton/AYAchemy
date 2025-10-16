//
// llchatlog_async.cpp - 非同期チャットログ書き出し実装
//
#include "llviewerprecompiledheaders.h"

#include "llchatlog_async.h"

#include "lllogchat.h"          // LLLogChat::saveHistory
#include "llviewercontrol.h"    // gSavedSettings
#include "llapr.h"              // 依存最小化。LLLogChatが内部でAPR使用

//=== LLSingleton boilerplate ===//
ChatLogAsync::ChatLogAsync()
{
    // 設定初期値を掴んで起動
    if (gSavedSettings.controlExists("ChatLogAsyncEnabled"))
        mEnabled = gSavedSettings.getBOOL("ChatLogAsyncEnabled");
    if (gSavedSettings.controlExists("ChatLogFlushIntervalMs"))
        mFlushIntervalMs = llclamp((U32)gSavedSettings.getU32("ChatLogFlushIntervalMs"), (U32)20, (U32)2000);
    if (gSavedSettings.controlExists("ChatLogFlushBatch"))
        mFlushBatch = llclamp((U32)gSavedSettings.getU32("ChatLogFlushBatch"), (U32)1, (U32)1024);

    mRunning.store(true);
    mThread = std::thread([this]{ workerLoop(); });
}

ChatLogAsync::~ChatLogAsync()
{
    shutdown();
}

void ChatLogAsync::shutdown()
{
    bool expected = true;
    if (mRunning.compare_exchange_strong(expected, false))
    {
        {
            std::lock_guard<std::mutex> lk(mMutex);
        }
        mCv.notify_all();
        if (mThread.joinable())
            mThread.join();
    }
}

void ChatLogAsync::enqueue(const LLChat& chat, const LLSD& args)
{
    // 設定をライブ反映（軽い）
    if (gSavedSettings.controlExists("ChatLogAsyncEnabled"))
        mEnabled = gSavedSettings.getBOOL("ChatLogAsyncEnabled");
    if (!mEnabled) return;

    if (gSavedSettings.controlExists("ChatLogFlushIntervalMs"))
        mFlushIntervalMs = llclamp((U32)gSavedSettings.getU32("ChatLogFlushIntervalMs"), (U32)20, (U32)2000);
    if (gSavedSettings.controlExists("ChatLogFlushBatch"))
        mFlushBatch = llclamp((U32)gSavedSettings.getU32("ChatLogFlushBatch"), (U32)1, (U32)1024);

    {
        std::lock_guard<std::mutex> lk(mMutex);
        mQ.emplace_back(chat, args);
    }
    mCv.notify_one();
}

void ChatLogAsync::workerLoop()
{
    // 待ち→一定件数/間隔でバッチ吐き出し
    while (mRunning.load())
    {
        std::deque<std::pair<LLChat, LLSD>> batch;

        {
            std::unique_lock<std::mutex> lk(mMutex);
            if (mQ.empty())
            {
                // 新規到着 or 終了指示まで待機
                mCv.wait_for(lk, std::chrono::milliseconds(mFlushIntervalMs));
            }

            // バッチ取り出し（最大 mFlushBatch）
            U32 n = 0;
            while (!mQ.empty() && n < mFlushBatch)
            {
                batch.emplace_back(std::move(mQ.front()));
                mQ.pop_front();
                ++n;
            }
        }

        if (!batch.empty())
        {
            flushBatch(batch);
        }
    }

    // 終了時：残りを出し切る
    std::deque<std::pair<LLChat, LLSD>> remain;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        remain.swap(mQ);
    }
    if (!remain.empty())
    {
        flushBatch(remain);
    }
}

void ChatLogAsync::flushBatch(std::deque<std::pair<LLChat, LLSD>>& batch)
{
    // ここはワーカースレッド。ファイルI/O可。
    // args["log_name"] があればそのファイル(IM等)、無ければ "chat"（Nearby）へ。
    while (!batch.empty())
    {
        auto entry = std::move(batch.front());
        batch.pop_front();

        const LLChat& chat = entry.first;
        const LLSD&   args = entry.second;

        if (args.has("log_name"))
        {
            // IMなど：セッション固有ファイルに行形式で保存
            const std::string file    = args["log_name"].asString();
            const std::string from    = chat.mFromName;
            const LLUUID&     from_id = chat.mFromID;
            const std::string line    = chat.mText;
            LLLogChat::saveHistory(file, from, from_id, line);
        }
        else
        {
            // Nearby：既定 "chat" へ（4引数版を使用）
            const std::string from    = chat.mFromName;
            const LLUUID&     from_id = chat.mFromID;
            const std::string line    = chat.mText;
            LLLogChat::saveHistory("chat", from, from_id, line);
        }
    }
}
