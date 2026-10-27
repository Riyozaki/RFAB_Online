#include <TiltedOnlinePCH.h>

#include <Forms/TESNPC.h>

#include <mutex>
#include <unordered_map>

namespace
{
std::mutex s_leveledNpcSourceMutex;
std::unordered_map<uint32_t, uint32_t> s_leveledNpcSourceByTempFormId;

void RecordLeveledNpcSource(const TESNPC* apTempNpc, const TESNPC* apSelectedNpc) noexcept
{
    if (!apTempNpc || !apSelectedNpc || apTempNpc->formID == 0 || apSelectedNpc->formID == 0)
        return;

    const TESNPC* pResolved = apSelectedNpc;
    if (pResolved->IsTemporary())
    {
        if (const TESNPC* pTemplate = pResolved->GetTemplateBase())
            pResolved = pTemplate;
    }

    uint32_t sourceFormId = pResolved ? pResolved->formID : 0u;
    std::lock_guard<std::mutex> lock(s_leveledNpcSourceMutex);
    uint32_t depth = 0;
    while ((sourceFormId & 0xFF000000u) == 0xFF000000u && depth++ < 8u)
    {
        const auto it = s_leveledNpcSourceByTempFormId.find(sourceFormId);
        if (it == s_leveledNpcSourceByTempFormId.end() || it->second == sourceFormId)
            break;
        sourceFormId = it->second;
    }

    if (sourceFormId != 0 && (sourceFormId & 0xFF000000u) != 0xFF000000u)
    {
        if (s_leveledNpcSourceByTempFormId.size() > 16384u)
            s_leveledNpcSourceByTempFormId.clear();
        s_leveledNpcSourceByTempFormId[apTempNpc->formID] = sourceFormId;
    }
}
} // namespace

TESNPC* TESNPC::GetLeveledSourceNpc(const TESNPC* apNpc) noexcept
{
    if (!apNpc || apNpc->formID == 0)
        return nullptr;

    if (!apNpc->IsTemporary())
        return const_cast<TESNPC*>(apNpc);

    uint32_t sourceFormId = 0;
    {
        std::lock_guard<std::mutex> lock(s_leveledNpcSourceMutex);
        const auto it = s_leveledNpcSourceByTempFormId.find(apNpc->formID);
        if (it != s_leveledNpcSourceByTempFormId.end())
            sourceFormId = it->second;
    }

    if (sourceFormId == 0 || (sourceFormId & 0xFF000000u) == 0xFF000000u)
        return nullptr;

    TESNPC* pSourceNpc = Cast<TESNPC>(TESForm::GetById(sourceFormId));
    return (pSourceNpc && !pSourceNpc->IsTemporary()) ? pSourceNpc : nullptr;
}

TP_THIS_FUNCTION(TSetLeveledNpc, TESNPC*, TESNPC, TESNPC*);
static TSetLeveledNpc* RealSetLeveledNpc = nullptr;

TESNPC* TP_MAKE_THISCALL(HookSetLeveledNpc, TESNPC, TESNPC* apSelectedNpc)
{
    RecordLeveledNpcSource(apThis, apSelectedNpc);
    TESNPC* pResult = TiltedPhoques::ThisCall(RealSetLeveledNpc, apThis, apSelectedNpc);
    if (pResult && pResult != apThis)
        RecordLeveledNpcSource(pResult, apSelectedNpc);
    return pResult;
}

static TiltedPhoques::Initializer s_npcInitHooks(
    []()
    {
        POINTER_SKYRIMSE(TSetLeveledNpc, s_SetLeveledNpc, 14257);

        RealSetLeveledNpc = s_SetLeveledNpc.Get();

        TP_HOOK(&RealSetLeveledNpc, HookSetLeveledNpc);
    });
