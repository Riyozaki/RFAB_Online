#include <Structs/CachedString.h>
#include <TiltedCore/Serialization.hpp>
#include "StringCache.h"

using TiltedPhoques::Serialization;

CachedString& CachedString::operator=(const TiltedPhoques::String& acRhs) noexcept
{
    String::operator=(acRhs);
    return *this;
}

void CachedString::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const auto cId = StringCache::Get()[*this];

    Serialization::WriteBool(aWriter, cId.has_value());
    if (cId)
    {
        Serialization::WriteVarInt(aWriter, *cId);
    }
    else
    {
        Serialization::WriteString(aWriter, *this);

        StringCache::Get().AddWanted(*this);
    }
}

void CachedString::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    const auto cHasId = Serialization::ReadBool(aReader);
    if (cHasId)
    {
        const auto cId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const auto cValue = StringCache::Get()[cId];
        if (cValue)
        {
            *this = *cValue;
        }
        else
        {
            // The peer referenced a table entry we do not have (late join, dropped/reordered
            // StringCacheUpdate or a hostile packet). Asserting killed debug builds and release
            // builds silently kept the *previous* value of this field, which then travelled on as
            // a valid-looking animation/tint name. Fail closed with a deterministic empty value.
            clear();
            StringCache::Get().NotifyUnknownId(static_cast<uint32_t>(cId));
        }
    }
    else
    {
        *this = Serialization::ReadString(aReader);

        StringCache::Get().AddWanted(*this);
    }
}
