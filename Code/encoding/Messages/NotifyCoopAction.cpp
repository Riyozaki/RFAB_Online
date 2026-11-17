#include <Messages/NotifyCoopAction.h>
void NotifyCoopAction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept { Data.Serialize(aWriter); }
void NotifyCoopAction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept { Data.Deserialize(aReader); }
