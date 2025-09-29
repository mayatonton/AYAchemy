#include "llfloater.h"

class LLComboBox;
class LLUICtrl;

class LLFloaterPoseStand : public LLFloater
{
public:
    LLFloaterPoseStand(const LLSD& key);
    static LLFloater* create(const LLSD& key) { return new LLFloaterPoseStand(key); }

    BOOL postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    void onCommitCombo(LLUICtrl* ctrl, const LLSD& param);

    // settings.xml の PoseStandAnimN（N=0..8）を参照して UUID を返す
    LLUUID getSlotAnimUUID(S32 index) const;

    // サーバー再生（他者に見える）
    void stopAllPoseStandMotionsServer();
    void startPoseByIndexServer(S32 index);

private:
    LLComboBox* mCombo = nullptr;
};