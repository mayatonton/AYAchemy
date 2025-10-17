// llfloaterposestand.cpp
#include "llviewerprecompiledheaders.h"
#include "llfloaterposestand.h"

#include <boost/bind.hpp>
#include "llagent.h"             // gAgent (sendAnimationRequest)
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llviewercontrol.h"     // gSavedSettings

namespace {
    static const char* const LAST_IDX_SETTING = "PoseStandLastIndex";

    inline void ensure_last_index_setting()
    {
        if (!gSavedSettings.controlExists(LAST_IDX_SETTING))
        {
            gSavedSettings.declareS32(
                LAST_IDX_SETTING,
                0,
                "Last used pose index for PoseStand",
                LLControlVariable::PERSIST_NONDFT  // ← TRUE ではなく列挙値
            );
        }
    }
}

LLFloaterPoseStand::LLFloaterPoseStand(const LLSD& key)
: LLFloater(key)
{
}

BOOL LLFloaterPoseStand::postBuild()
{
    mCombo = getChild<LLComboBox>("pose_combo");
    if (mCombo)
    {
        mCombo->setCommitCallback(boost::bind(&LLFloaterPoseStand::onCommitCombo, this, _1, _2));
    }
    return TRUE;
}

void LLFloaterPoseStand::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    if (!mCombo) mCombo = getChild<LLComboBox>("pose_combo");
    if (!mCombo) return;

    ensure_last_index_setting();

    // 保存された前回インデックスを取得し、コンボに反映
    S32 saved = gSavedSettings.getS32(LAST_IDX_SETTING);
    if (mCombo->getItemCount() > 0)
    {
        saved = llclamp(saved, 0, mCombo->getItemCount() - 1);
        mCombo->selectNthItem(saved);
    }

    // ウィンドウ表示と同時に、選択されているポーズをサーバー再生（前回の選択に合わせる）
    stopAllPoseStandMotionsServer();
    startPoseByIndexServer(mCombo->getCurrentIndex());
}

void LLFloaterPoseStand::onClose(bool app_quitting)
{
    stopAllPoseStandMotionsServer();
    LLFloater::onClose(app_quitting);
}

void LLFloaterPoseStand::onCommitCombo(LLUICtrl* ctrl, const LLSD& param)
{
    if (!mCombo) return;

    stopAllPoseStandMotionsServer();
    startPoseByIndexServer(mCombo->getCurrentIndex());

    // 変更された選択インデックスを保存
    ensure_last_index_setting();
    gSavedSettings.setS32(LAST_IDX_SETTING, mCombo->getCurrentIndex());
}

LLUUID LLFloaterPoseStand::getSlotAnimUUID(S32 index) const
{
    const S32 i = llclamp(index, 0, 8);
    const std::string key = llformat("PoseStandAnim%d", i);
    LLUUID id;
    id.set(gSavedSettings.getString(key));
    return id;
}

void LLFloaterPoseStand::stopAllPoseStandMotionsServer()
{
    for (S32 i = 0; i <= 8; ++i)
    {
        LLUUID id = getSlotAnimUUID(i);
        if (id.notNull())
        {
            gAgent.sendAnimationRequest(id, ANIM_REQUEST_STOP);
        }
    }
}

void LLFloaterPoseStand::startPoseByIndexServer(S32 index)
{
    LLUUID id = getSlotAnimUUID(index);
    if (id.notNull())
    {
        gAgent.sendAnimationRequest(id, ANIM_REQUEST_START);
    }
}