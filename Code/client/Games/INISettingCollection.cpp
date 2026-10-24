#include <TiltedOnlinePCH.h>

#include <Games/TES.h>
#include <cstring>

GameSettingCollection* GameSettingCollection::Get() noexcept
{
    POINTER_SKYRIMSE(GameSettingCollection*, s_singleton, 514622);
    return *s_singleton.Get();
}

Setting* GameSettingCollection::GetSetting(const char* acpName) noexcept
{
    if (!acpName || !buckets || bucketCount == 0 || bucketCount > 4096)
        return nullptr;

    for (uint32_t i = 0; i < bucketCount; ++i)
    {
        for (MapItem* pItem = buckets[i]; pItem != nullptr; pItem = pItem->next)
        {
            if (pItem->key && _stricmp(pItem->key, acpName) == 0)
                return pItem->value;
        }
    }

    return nullptr;
}

INISettingCollection* INISettingCollection::Get() noexcept
{
    POINTER_SKYRIMSE(INISettingCollection*, settingCollection, 524557);

    return *settingCollection.Get();
}

Setting* INISettingCollection::GetSetting(const char* acpName) noexcept
{
    Entry* pCurrent = &head;

    while (pCurrent)
    {
        if (_stricmp(acpName, pCurrent->setting->name) == 0)
            return pCurrent->setting;

        pCurrent = pCurrent->next;
    }

    return nullptr;
}
