#pragma once
//
// llchat_async.h - Second Life viewer (AYAchemy) 非同期チャット仲介
//
// 目的:
//   - 受信(ネットワーク)→UIを分離し、短いジッタバッファで順序安定・スパイク耐性を向上。
//   - まずは最小実装: mutex + deque ベース。後から lock-free へ差し替え可能。
//   - Auto-Jitter: 直近到着間隔の分布から hold を動的調整（少数サンプル時は max、十分なら p95）。
//
// 使い方:
//   - 受信側: ChatAsync::instance().enqueue(ChatItem&&)
//   - 毎フレーム: ChatAsync::instance().drainReady()
//   - 設定: ChatAsyncEnabled / ChatAsyncJitterMs / ChatAsyncAutoJitter / ChatAsyncJitterMinMs / ChatAsyncJitterMaxMs
//

#include "llsingleton.h"
#include "lltimer.h"
#include "llstring.h"
#include "lluuid.h"
#include "llpreprocessor.h"
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>

// 必要最小のチャット項目
struct ChatItem
{
    std::string text;
    std::string from;
    S32         channel = 0;          // 情報用途のみ（AYAchemyのLLChatにはmChannelなし）
    S32         chat_type = 0;        // whisper/say/shout 等 (EChatType)
    S32         source_type = 0;      // EChatSourceType
    LLUUID      from_id;              // 発言者（アバター/オブジェクト）
    LLUUID      owner_id;             // オブジェクト発言時のオーナー
    F32         time = 0.f;           // chat.mTime（UIの“履歴扱い”防止に重要）
    U64         arrival_mono = 0;     // monotonic usec (LLTimer::getTotalTime)
    U32         sim_time = 0;         // シム時刻(秒) 取得できれば設定、無ければ 0
    bool        is_severely_delayed = false; // しきい値超の遅延(>3s)でバッジ表示などに使用
    bool        no_hold = false;      // ★ ジッタ待ちなし（自分発言など）
};

// 受信→UI の非同期仲介クラス
class ChatAsync : public LLSingleton<ChatAsync>
{
    LLSINGLETON(ChatAsync);
    ~ChatAsync();

public:
    // 受信スレッド/ネットワーク処理側から呼ぶ：極力軽量に
    void enqueue(ChatItem&& item);

    // メインスレッド（毎フレーム）から呼ぶ：ホールド満了分だけ UI へ流す
    void drainReady();

    // ランタイム設定（必要なら明示的に変えられる）
    void setEnabled(bool v) { mEnabled = v; }
    void setJitterMs(U32 ms) { mJitterMs = ms; }

private:
    // まとめて UI へ出力する内部ヘルパ
    void uiAppendBatch(std::vector<ChatItem>& ready);

    // ソートキー: sim_time(取得できた場合) > 到着時刻
    static U64 sortKey(const ChatItem& c)
    {
        if (c.sim_time != 0)
        {
            return static_cast<U64>(c.sim_time) * 1000000ULL;
        }
        return c.arrival_mono;
    }

    // 統計：少数サンプルなら max、多ければ p95
    static U32 pick_statistic(const std::deque<U32>& d)
    {
        if (d.empty()) return 0;
        if (d.size() < 8) // 母数不足は最大値の方が安定
        {
            return *std::max_element(d.begin(), d.end());
        }
        std::vector<U32> v(d.begin(), d.end());
        std::sort(v.begin(), v.end());
        size_t idx = (size_t)((v.size() * 95) / 100);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
    }

private:
    // 受信直後の一次キュー（pushは軽く）
    std::mutex            mMutex;
    std::deque<ChatItem>  mQ;

    // ジッタ保持中のペンディング
    std::vector<ChatItem> mPending;

    // 手動設定
    U32  mJitterMs = 300;   // 0〜2000ms 推奨: 150〜300
    bool mEnabled  = true;  // false で従来の同期経路へフォールバック

    // ★ Auto-Jitter 用
    bool mAuto = true;
    U32  mAutoMinMs = 80;
    U32  mAutoMaxMs = 600;
    std::deque<U32> mInterArrivalMs;  // 直近の到着間隔(ms)（最大16）
    U64  mLastArrivalMono = 0;        // 前回 arrival のモノトニック(usec)
    U64  mLastSampleMono = 0;         // 直近にサンプルを得た時刻(usec)
};
