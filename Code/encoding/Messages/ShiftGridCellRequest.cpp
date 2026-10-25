#include <Messages/ShiftGridCellRequest.h>
#include <algorithm>

void ShiftGridCellRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    WorldSpaceId.Serialize(aWriter);
    PlayerCell.Serialize(aWriter);
    CenterCoords.Serialize(aWriter);

    const size_t count = std::min<size_t>(Cells.size(), 255);
    aWriter.WriteBits(count & 0xFF, 8);

    for (size_t i = 0; i < count; ++i)
    {
        Cells[i].Serialize(aWriter);
    }
}

void ShiftGridCellRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    WorldSpaceId.Deserialize(aReader);
    PlayerCell.Deserialize(aReader);
    CenterCoords.Deserialize(aReader);

    uint64_t count = 0;
    aReader.ReadBits(count, 8);

    Cells.clear();
    Cells.reserve(count & 0xFF);

    for (size_t i = 0; i < (count & 0xFF) && !aReader.Eof(); ++i)
    {
        GameId cell{};
        cell.Deserialize(aReader);
        Cells.push_back(cell);
    }
}
