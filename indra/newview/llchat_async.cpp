//
// llchat_async.cpp - Second Life viewer (AYAchemy) 非同期チャット仲介 実装
//

#include "llviewerprecompiledheaders.h"   // newview系では最初にこれ
#include "llchat_async.h"

#include "llviewercontrol.h"              // gSavedSettings
#include "llchat.h"
#include "lltrans.h"
#include "llmath.h"                       // llclamp
#include "llsd.h"
#include "llnotificationmanager.h"        // LLNotificationsUI::LLNotificationManager
#include "llchatlog_async.h"           // ★ 追加：非同期ログ

//=== LLSingleton: 通常のコンストラクタ/デストラクタ定義 ===//
ChatAsync::ChatAsync() {}
ChatAsync::~ChatAsync() {}

void ChatAsync::enqueue(ChatItem&& item)
{
    // 無効化時はフォールバック（同期経路と同等の onChat を直接呼ぶ）
    if (!mEnabled)
    {
        LLChat chat;
        chat.mText       = item.text;
        chat.mFromName   = item.from;
        chat.mSourceType = (EChatSourceType)item.source_type;
        chat.mChatType   = (EChatType)item.chat_type;
        chat.mFromID     = item.from_id;
        chat.mOwnerID    = item.owner_id;
        if (item.time > 0.f)
        {
            chat.mTime = item.time; // F32
        }
        else
        {
            chat.mTime = (F32)LLFrameTimer::getElapsedSeconds(); // 明示的にF32へ
        }

        LLSD args;
        args["is_history"] = false;
        LLNotificationsUI::LLNotificationManager::instance().onChat(chat, args);
        return;
    }

    if (item.arrival_mono == 0)
        item.arrival_mono = LLTimer::getTotalTime();

    {
        std::lock_guard<std::mutex> lk(mMutex);
        mQ.emplace_back(std::move(item));
    }
}

void ChatAsync::drainReady()
{
    // 設定を毎フレーム反映（ライブ調整）
    if (gSavedSettings.controlExists("ChatAsyncEnabled"))
        mEnabled = gSavedSettings.getBOOL("ChatAsyncEnabled");
    if (gSavedSettings.controlExists("ChatAsyncJitterMs"))
        mJitterMs = llclamp((U32)gSavedSettings.getU32("ChatAsyncJitterMs"), (U32)0, (U32)2000);
    if (gSavedSettings.controlExists("ChatAsyncAutoJitter"))
        mAuto = gSavedSettings.getBOOL("ChatAsyncAutoJitter");
    if (gSavedSettings.controlExists("ChatAsyncJitterMinMs"))
        mAutoMinMs = llclamp((U32)gSavedSettings.getU32("ChatAsyncJitterMinMs"), (U32)0, (U32)2000);
    if (gSavedSettings.controlExists("ChatAsyncJitterMaxMs"))
        mAutoMaxMs = llclamp((U32)gSavedSettings.getU32("ChatAsyncJitterMaxMs"), (U32)0, (U32)4000);
    if (mAutoMinMs > mAutoMaxMs) std::swap(mAutoMinMs, mAutoMaxMs);

    if (!mEnabled) return;

    // (1) 一次キュー → pending
    std::vector<ChatItem> batch;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        while (!mQ.empty())
        {
            batch.emplace_back(std::move(mQ.front()));
            mQ.pop_front();
        }
    }
    if (!batch.empty())
    {
        // ★ 到着間隔(ms)を更新（バーストに追従）
        // 直近サンプル窓は 16 件まで（静かな環境でも反応しやすい）
        for (const auto& c : batch)
        {
            if (mLastArrivalMono != 0)
            {
                U64 delta_us = (c.arrival_mono > mLastArrivalMono) ? (c.arrival_mono - mLastArrivalMono) : 0;
                U32 d_ms = (U32)llclamp((S32)(delta_us / 1000ULL), 0, 2000); // 0..2000ms にクリップ
                mInterArrivalMs.push_back(d_ms);
                if (mInterArrivalMs.size() > 16) mInterArrivalMs.pop_front();
            }
            mLastArrivalMono = c.arrival_mono;
        }
        mLastSampleMono = LLTimer::getTotalTime();

        mPending.reserve(mPending.size() + batch.size());
        for (auto& c : batch) mPending.emplace_back(std::move(c));
    }
    if (mPending.empty()) return;

    // (2) ジッタ満了 or 極端遅延 or no_hold（即時出し）
    const U64 now = LLTimer::getTotalTime();

    // ★ Auto-Jitter: pick_statistic * 1.2 を目標にし、Min..Max へクランプ → EMAで平滑
    U32 hold_ms = mJitterMs;
    if (mAuto && !mInterArrivalMs.empty())
    {
        U32 stat = pick_statistic(mInterArrivalMs);        // p95 or max
        U32 target = (U32)((stat * 12) / 10);              // 1.2x
        target = llclamp(target, mAutoMinMs, mAutoMaxMs);
        // 簡易EMA（prev:target = 3:1）…過度な揺れを抑える
        hold_ms = (hold_ms == 0) ? target : (U32)((hold_ms * 3 + target) / 4);
    }

    // ★ Quiet リセット（静寂が続いたら最小値へ寄せる）
    const U32 QUIET_MS = 10000; // 10s
    if (mAuto && mLastSampleMono != 0)
    {
        U64 quiet_us = (now > mLastSampleMono) ? (now - mLastSampleMono) : 0;
        if (quiet_us / 1000ULL > QUIET_MS)
        {
            U32 target = mAutoMinMs;
            hold_ms = (U32)((hold_ms * 1 + target * 3) / 4); // 3:1 で target 寄り
        }
    }

    std::vector<ChatItem> ready;
    ready.reserve(mPending.size());

    auto it = mPending.begin();
    while (it != mPending.end())
    {
        const U32 age_ms = (U32)((now - it->arrival_mono) / 1000ULL);
        const bool severe = (age_ms > 3000); // >3s は即時出し
        if (it->no_hold || age_ms >= hold_ms || severe)
        {
            it->is_severely_delayed = severe;
            ready.emplace_back(std::move(*it));
            it = mPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (ready.empty()) return;

    // (3) 安定ソート（同一キーは FIFO）
    std::stable_sort(ready.begin(), ready.end(),
        [](const ChatItem& a, const ChatItem& b)
        {
            return sortKey(a) < sortKey(b);
        });

    // (4) UIへまとめて排出（NotificationManager 経由）
    uiAppendBatch(ready);
}

void ChatAsync::uiAppendBatch(std::vector<ChatItem>& ready)
{
    for (auto& c : ready)
    {
        LLChat chat;
        chat.mText       = c.text;
        chat.mFromName   = c.from;
        chat.mSourceType = (EChatSourceType)c.source_type;
        chat.mChatType   = (EChatType)c.chat_type;
        chat.mFromID     = c.from_id;
        chat.mOwnerID    = c.owner_id;

        if (c.time > 0.f)
        {
            chat.mTime = c.time;
        }
        else
        {
            chat.mTime = (F32)LLFrameTimer::getElapsedSeconds();
        }

        if (c.is_severely_delayed)
        {
            // strings.xml に "ChatDelayed" が無い場合でも安全にフォールバック
            std::string delayed = "遅延";
            std::string tr = LLTrans::getString("ChatDelayed");
            if (!tr.empty() && tr != "ChatDelayed")
                delayed = tr;
            chat.mText += "  (" + delayed + ")";
        }

        LLSD args;
        args["is_history"] = false; // 履歴扱いを避ける
        LLNotificationsUI::LLNotificationManager::instance().onChat(chat, args);

        // ★ 非同期でログへ（UIをブロックしない）
        if (gSavedSettings.getBOOL("ChatLogAsyncEnabled"))
        {
            ChatLogAsync::instance().enqueue(chat, args);
        }
    }
}
