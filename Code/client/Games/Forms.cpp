#include <TiltedOnlinePCH.h>

#include <PlayerCharacter.h>
#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>

#include <Games/IFormFactory.h>

#include <SaveLoad.h>
#include <Games/Overrides.h>

TESForm* TESForm::GetById(const uint32_t aId)
{
    using TGetFormById = TESForm*(uint32_t);
    POINTER_SKYRIMSE(TGetFormById, getFormById, 14461);

    return getFormById.Get()(aId);
}

void TESNPC::Serialize(String* apSaveBuffer) const noexcept
{
    if (!apSaveBuffer)
        return;

    ScopedSaveLoadOverride saveLoadOverride;

    constexpr uint32_t kNpcSerializeScratchCapacity = 1u << 18; // 256 KB reusable TLS scratch (prevents 32 KB stack overflow & engine heap-free on realloc)
    static thread_local std::vector<char> s_npcSerializeScratch;
    if (s_npcSerializeScratch.size() < kNpcSerializeScratchCapacity)
        s_npcSerializeScratch.resize(kNpcSerializeScratchCapacity, '\0');

    BGSSaveFormBuffer saveBuffer;
    saveBuffer.buffer = s_npcSerializeScratch.data();
    saveBuffer.capacity = kNpcSerializeScratchCapacity;
    saveBuffer.changeFlags = GetChangeFlags();

    Save(&saveBuffer);

    const uint32_t safeLength = std::min(saveBuffer.position, saveBuffer.capacity);
    apSaveBuffer->assign(saveBuffer.buffer, safeLength);

    saveBuffer.buffer = nullptr;
}

void TESNPC::Deserialize(const String& acBuffer, uint32_t aChangeFlags) noexcept
{
    ScopedSaveLoadOverride saveLoadOverride;

    BGSLoadFormBuffer loadBuffer(aChangeFlags);
    loadBuffer.SetSize(acBuffer.size() & 0xFFFFFFFF);
    loadBuffer.buffer = acBuffer.data();
    loadBuffer.formId = formID;
    loadBuffer.form = this;

    Load(&loadBuffer);

    loadBuffer.buffer = nullptr;
    originalRace = nullptr;

    if (const uint32_t cRemoved = spellList.RemoveNullEntries())
        spdlog::warn("Removed {} unresolved spell list entries from NPC {:X}", cRemoved, formID);
}

void TESNPC::Initialize() noexcept
{
    auto pPlayerBaseForm = PlayerCharacter::Get() ? Cast<TESNPC>(PlayerCharacter::Get()->baseForm) : nullptr;
    if (!pPlayerBaseForm)
    {
        flags |= 0x200000;
        return;
    }

    // These values are all defaulted, if the other actor did not modify them they won't be loaded, therefore we need to force them before load
    attackDataForm.attackDataMap = pPlayerBaseForm->attackDataForm.attackDataMap;
    npcClass = pPlayerBaseForm->npcClass;
    combatStyle = pPlayerBaseForm->combatStyle;
    raceForm.race = pPlayerBaseForm->raceForm.race;
    defaultOutfit = pPlayerBaseForm->defaultOutfit;
    sleepOutfit = pPlayerBaseForm->sleepOutfit;
    spellList.Initialize();
    // End defaults

    flags |= 0x200000;
}

void TESForm::Save_Reversed(const uint32_t aChangeFlags, Buffer::Writer& aWriter)
{
    if (aChangeFlags & 1)
    {
        aWriter.WriteBytes(reinterpret_cast<uint8_t*>(&flags), 4);
        aWriter.WriteBytes(reinterpret_cast<uint8_t*>(&unk10), 2);
    }
}

void TESForm::SetSkipSaveFlag(bool aSet) noexcept
{
    if (aSet)
    {
        unk10 = 0xFFFF;
    }
    /*const uint32_t flag = 1 << 14;

    if (aSet)
        flags |= flag;
    else
        flags &= ~flag;*/
}

uint32_t TESForm::GetChangeFlags() const noexcept
{
    struct Unk
    {
        uint8_t unk0[0x330];
        void* unk330;
    };

    TP_THIS_FUNCTION(InternalGetChangeFlags, bool, void, uint32_t formId, ChangeFlags& changeFlags);

    POINTER_SKYRIMSE(InternalGetChangeFlags, internalGetChangeFlags, 34582);

    POINTER_SKYRIMSE(Unk*, s_singleton, 516851);

    const auto pUnk = *(s_singleton.Get());

    ChangeFlags changeFlags;
    const auto cResult = TiltedPhoques::ThisCall(internalGetChangeFlags, pUnk->unk330, formID, changeFlags);
    if (!cResult)
        return 0;

    return changeFlags.flags;
}

TESNPC* TESNPC::Create(const String& acBuffer, const uint32_t aChangeFlags) noexcept
{
    auto pNpc = IFormFactory::Create<TESNPC>();

    pNpc->Initialize();
    pNpc->Deserialize(acBuffer, aChangeFlags);

    // This forces facegen for some reason
    pNpc->originalRace = nullptr;

    return pNpc;
}

BGSHeadPart* TESNPC::GetHeadPart(uint32_t aType)
{
    if (headparts)
    {
        for (auto i = 0; i < headpartsCount; ++i)
        {
            if (headparts[i] && headparts[i]->type == aType)
                return headparts[i];
        }
    }

    return nullptr;
}
