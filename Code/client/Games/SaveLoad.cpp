#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <Games/Overrides.h>
#include <common/CoopWorldStabilizer.h>

#include <Forms/TESForm.h>

#include <World.h>

#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/Buffer.hpp>
#include <algorithm>

using TiltedPhoques::Serialization;
using TiltedPhoques::ViewBuffer;

BGSSaveFormBuffer::BGSSaveFormBuffer()
{
    TP_THIS_FUNCTION(CtorT, BGSSaveFormBuffer*, BGSSaveFormBuffer);

    POINTER_SKYRIMSE(CtorT, ctor, 35145);

    TiltedPhoques::ThisCall(ctor, this);

    position = 0;
}

void BGSSaveFormBuffer::WriteId(uint32_t aId) noexcept
{
    if (!CoopWorldStabilizer::CanWriteFormIdVarIntsToSaveBuffer(buffer, position, capacity))
        return;

    uint32_t modId = 0;
    uint32_t baseId = 0;

    if (World::IsCreated())
        World::Get().GetModSystem().GetServerModId(aId, modId, baseId);

    auto pWriteLocation = reinterpret_cast<uint8_t*>(buffer + position);

    ViewBuffer view(pWriteLocation, capacity - position);
    Buffer::Writer writer(&view);

    Serialization::WriteVarInt(writer, modId);
    Serialization::WriteVarInt(writer, baseId);

    position = static_cast<uint32_t>(std::min<size_t>(capacity, static_cast<size_t>(position) + writer.Size()));
}

BGSLoadFormBuffer::BGSLoadFormBuffer(const uint32_t aChangeFlags)
{
    TP_THIS_FUNCTION(CtorT, BGSLoadFormBuffer*, BGSLoadFormBuffer);

    POINTER_SKYRIMSE(CtorT, ctor, 35100);

    TiltedPhoques::ThisCall(ctor, this);

    changeFlags = aChangeFlags;
    loadFlag = 0x40;
    position = 0;
    maybeMoreFlags = 0;

    unk1C = -1;
}

TP_THIS_FUNCTION(TBGSLoadFormBuffer_ReadFormId, bool, BGSLoadFormBuffer, uint32_t&);
TP_THIS_FUNCTION(TBGSSaveFormBuffer_WriteFormId, void, BGSSaveFormBuffer, TESForm*);
TP_THIS_FUNCTION(TBGSSaveFormBuffer_WriteId, void, BGSSaveFormBuffer, uint64_t);

static TBGSSaveFormBuffer_WriteFormId* RealBGSSaveFormBuffer_WriteFormId = nullptr;
static TBGSLoadFormBuffer_ReadFormId* RealBGSLoadFormBuffer_ReadFormId = nullptr;
static TBGSSaveFormBuffer_WriteId* RealBGSSaveFormBuffer_WriteId = nullptr;

void TP_MAKE_THISCALL(BGSSaveFormBuffer_WriteFormId, BGSSaveFormBuffer, TESForm* apForm)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        TiltedPhoques::ThisCall(RealBGSSaveFormBuffer_WriteFormId, apThis, apForm);
        return;
    }

    apThis->WriteId(apForm ? apForm->formID : 0);
}

void TP_MAKE_THISCALL(BGSSaveFormBuffer_WriteId, BGSSaveFormBuffer, uint64_t aId)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        TiltedPhoques::ThisCall(RealBGSSaveFormBuffer_WriteId, apThis, aId);
        return;
    }

    apThis->WriteId(aId & 0xFFFFFFFF);
}

bool TP_MAKE_THISCALL(BGSLoadFormBuffer_LoadFormId, BGSLoadFormBuffer, uint32_t& aFormId)
{
    if (!ScopedSaveLoadOverride::IsOverriden())
    {
        return TiltedPhoques::ThisCall(RealBGSLoadFormBuffer_ReadFormId, apThis, aFormId);
    }

    if (!apThis || !apThis->buffer || apThis->position >= apThis->capacity)
    {
        aFormId = 0;
        return false;
    }

    uint8_t* pReadLocation = (uint8_t*)(apThis->buffer + apThis->position);

    ViewBuffer buffer(pReadLocation, apThis->capacity - apThis->position);
    ViewBuffer::Reader reader(&buffer);

    const uint32_t modId = Serialization::ReadVarInt(reader) & 0xFFFFFFFF;
    const uint32_t baseId = Serialization::ReadVarInt(reader) & 0xFFFFFFFF;

    aFormId = 0;

    if ((modId != 0 || baseId != 0) && World::IsCreated())
    {
        aFormId = World::Get().GetModSystem().GetGameId(modId, baseId);
        if (aFormId == 0)
            spdlog::warn("Failed to resolve form {:X}:{:X} while loading form {:X}", modId, baseId, apThis->formId);
    }

    apThis->position = static_cast<uint32_t>(std::min<size_t>(apThis->capacity, static_cast<size_t>(apThis->position) + reader.Size()));

    return true;
}

static TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TBGSLoadFormBuffer_ReadFormId, s_readFormId, 35107);
        POINTER_SKYRIMSE(TBGSSaveFormBuffer_WriteFormId, s_writeFormId, 35158);
        POINTER_SKYRIMSE(TBGSSaveFormBuffer_WriteId, s_writeId, 35157);

        RealBGSLoadFormBuffer_ReadFormId = s_readFormId.Get();
        RealBGSSaveFormBuffer_WriteFormId = s_writeFormId.Get();
        RealBGSSaveFormBuffer_WriteId = s_writeId.Get();

        TP_HOOK(&RealBGSLoadFormBuffer_ReadFormId, BGSLoadFormBuffer_LoadFormId);
        TP_HOOK(&RealBGSSaveFormBuffer_WriteFormId, BGSSaveFormBuffer_WriteFormId);
        TP_HOOK(&RealBGSSaveFormBuffer_WriteId, BGSSaveFormBuffer_WriteId);
    });
