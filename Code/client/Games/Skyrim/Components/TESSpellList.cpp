#include <TiltedOnlinePCH.h>

#include <Components/TESSpellList.h>
#include <Games/Memory.h>

void TESSpellList::Initialize()
{
    if (lists != nullptr)
        return;

    lists = Memory::New<Lists>();
}

namespace
{
template <class T> uint32_t CompactSpellArray(T** apEntries, uint32_t& aCount) noexcept
{
    if (!apEntries)
        return 0;

    const uint32_t oldCount = aCount;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < oldCount; ++i)
    {
        if (apEntries[i])
            apEntries[kept++] = apEntries[i];
    }
    for (uint32_t i = kept; i < oldCount; ++i)
        apEntries[i] = nullptr;

    aCount = kept;
    return oldCount - kept;
}
} // namespace

uint32_t TESSpellList::RemoveNullEntries() noexcept
{
    if (!lists)
        return 0;

    return CompactSpellArray(lists->spells, lists->spellCount) +
           CompactSpellArray(lists->unk4, lists->unk4Count) +
           CompactSpellArray(lists->shouts, lists->shoultCount);
}
