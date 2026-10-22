#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <Games/IFormFactory.h>
#include <Games/Memory.h>
#include <Services/CoordinatedSaveService.h>
#include <Utils.h>
#include <World.h>

#include <cstring>
#include <string>

namespace
{
TP_THIS_FUNCTION(TSaveImpl, bool, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName);
TP_THIS_FUNCTION(TLoadImpl, bool, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods);

TSaveImpl* RealSaveImpl = nullptr;
TLoadImpl* RealLoadImpl = nullptr;

bool ExecuteConsoleSaveOrLoadCommand(const std::string& acCommand) noexcept
{
    if (acCommand.empty())
        return false;

    static TESForm* s_pConsoleScript = nullptr;
    if (!s_pConsoleScript)
    {
        auto* pFactory = IFormFactory::GetForType(FormType::Script);
        if (!pFactory)
            return false;
        s_pConsoleScript = pFactory->Create();
    }
    if (!s_pConsoleScript)
        return false;

    // Offset 0x38 inside RE::Script (sizeof == 0x80) is `char* text`
    char** ppText = reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(s_pConsoleScript) + 0x38);
    if (*ppText)
    {
        Memory::Free(*ppText);
        *ppText = nullptr;
    }

    char* pBuf = static_cast<char*>(Memory::Allocate(acCommand.size() + 1));
    if (!pBuf)
        return false;

    std::memcpy(pBuf, acCommand.data(), acCommand.size() + 1);
    *ppText = pBuf;

    struct DummyScriptCompiler
    {
        uint8_t pad{0};
    } compiler{};

    TP_THIS_FUNCTION(TCompileAndRun, void, TESForm, DummyScriptCompiler*, uint32_t, void*);
    POINTER_SKYRIMSE(TCompileAndRun, s_compileAndRun, 21416);
    TiltedPhoques::ThisCall(s_compileAndRun, s_pConsoleScript, &compiler, 1U /*kSystemWindowCompiler*/, nullptr);
    return true;
}

bool TP_MAKE_THISCALL(HookSaveImpl, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineSaveAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);
}

bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineLoadAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealLoadImpl, apThis, acpFileName, aDeviceId, aOutputStats, aCheckForMods);
}

TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TSaveImpl, s_saveImpl, 34818);
        POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 34819);

        RealSaveImpl = s_saveImpl.Get();
        RealLoadImpl = s_loadImpl.Get();

        TP_HOOK(&RealSaveImpl, HookSaveImpl);
        TP_HOOK(&RealLoadImpl, HookLoadImpl);
    });
} // namespace

BGSSaveLoadManager* BGSSaveLoadManager::Get() noexcept
{
    POINTER_SKYRIMSE(BGSSaveLoadManager*, s_singleton, 516860);
    return *s_singleton.Get();
}

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}

bool BGSSaveLoadManager::SaveNamed(const char* acpFileName) noexcept
{
    if (!acpFileName || acpFileName[0] == '\0')
        return false;

    // Enqueue via Skyrim's native console command runner (`save "<slot>"`) instead of calling
    // Save_Impl directly inside Main::Update, avoiding renderer screenshot capture deadlocks.
    std::string cmd = "save \"";
    cmd += acpFileName;
    cmd += "\"";
    return ExecuteConsoleSaveOrLoadCommand(cmd);
}

bool BGSSaveLoadManager::LoadNamed(const char* acpFileName, bool aCheckForMods) noexcept
{
    TP_UNUSED(aCheckForMods);
    if (!acpFileName || acpFileName[0] == '\0')
        return false;

    std::string cmd = "load \"";
    cmd += acpFileName;
    cmd += "\"";
    return ExecuteConsoleSaveOrLoadCommand(cmd);
}
