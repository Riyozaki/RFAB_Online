#include <Messages/RequestCoopAction.h>
void RequestCoopAction::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept { Data.Serialize(aWriter); }
void RequestCoopAction::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept { Data.Deserialize(aReader); }
