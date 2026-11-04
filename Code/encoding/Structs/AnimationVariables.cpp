#include <Structs/AnimationVariables.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <iostream>

bool AnimationVariables::operator==(const AnimationVariables& acRhs) const noexcept
{
    return Booleans == acRhs.Booleans && Integers == acRhs.Integers && Floats == acRhs.Floats;
}

bool AnimationVariables::operator!=(const AnimationVariables& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

// std::vector<bool> implementation is unspecified, but often packed reasonably.
// The spec does not guarantee contiguous memory, though, so somewhat laborious 
// translation needed. Should be better than winding down several layers to 
// TiltedPhoques::Serialization::WriteBool, though.
//
void AnimationVariables::VectorBool_to_String(const Vector<bool>& bools, TiltedPhoques::String& chars) const
{
    chars.assign((bools.size() + 7) >> 3, 0);

    auto citer = chars.begin();
    auto biter = bools.begin();
    for (uint32_t mask = 1; biter < bools.end(); mask = 1, citer++)
        for (; mask < 0x100 && biter < bools.end(); mask <<= 1)
            *citer |= *biter++ ? mask : 0;
}

// Unpacks a bitstream into bools. The two sizes are independent: the counts come from
// one part of the packet and the packed bytes from another, so a sender can declare more
// values than it paid bytes for. The loop used to walk the output and read the input in
// lockstep without ever checking the input end - a remote read past the string, which
// AddressSanitizer reports as a stack-buffer-overflow on a short (SSO) string.
// Bits the packet did not carry stay false.
//
void AnimationVariables::String_to_VectorBool(const TiltedPhoques::String& chars, Vector<bool>& bools)
{
    bools.assign(bools.size(), false);

    const size_t count = bools.size();
    size_t index = 0;
    for (const char packedChar : chars)
    {
        if (index >= count)
            break;

        const auto packed = static_cast<uint8_t>(packedChar);
        for (uint32_t mask = 1; mask < 0x100 && index < count; mask <<= 1)
            bools[index++] = (packed & mask) != 0;
    }
}


void AnimationVariables::Load(std::istream& aInput)
{
    // Booleans are bitpacked and a bit different, not guaranteed contiguous.
    TiltedPhoques::String chars((Booleans.size() + 7) >> 3, 0);

    aInput.read(reinterpret_cast<char*>(chars.data()), chars.size());
    String_to_VectorBool(chars, Booleans);
    aInput.read(reinterpret_cast<char*>(Integers.data()), Integers.size() * sizeof(uint32_t));
    aInput.read(reinterpret_cast<char*>(Floats.data()), Floats.size() * sizeof(float));
}

void AnimationVariables::Save(std::ostream& aOutput) const
{
    // Booleans bitpacked and not guaranteed contiguous.
    TiltedPhoques::String chars;
    VectorBool_to_String(Booleans, chars);
 
    aOutput.write(reinterpret_cast<const char*>(chars.data()), chars.size());
    aOutput.write(reinterpret_cast<const char*>(Integers.data()), Integers.size() * sizeof(uint32_t));
    aOutput.write(reinterpret_cast<const char*>(Floats.data()), Floats.size() * sizeof(float));
}

// Wire format description.
//
// Sends 3 VarInts, the count of Booleans, Integers and Floats, in that order. Then sends a bitstream of the
// sum of those counts. For the Booleans, these represent the bit values for the Booleans. For the Integers and
// Floats, it represents a truth table for whether the value has changed. If values HAVE changed, they follow on 
// the stream.
//
//
void AnimationVariables::GenerateDiff(const AnimationVariables& aPrevious, TiltedPhoques::Buffer::Writer& aWriter) const
{
    const size_t sizeChangedVector = Booleans.size() + Integers.size() + Floats.size();
    auto changedVector = Booleans;
    changedVector.reserve(sizeChangedVector);

    for (size_t i = 0; i < Integers.size(); i++)
        changedVector.push_back(aPrevious.Integers.size() != Integers.size() || aPrevious.Integers[i] != Integers[i]);
    for (size_t i = 0; i < Floats.size(); i++)
        changedVector.push_back(aPrevious.Floats.size() != Floats.size() || aPrevious.Floats[i] != Floats[i]);

    // Now serialize: VarInts Booleans.size(), Integers.size(), Floats.size(),
    // then the change table bits, then changed Integers, then changed Floats.
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Booleans.size());
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Integers.size());
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Floats.size());

    TiltedPhoques::String chars;
    VectorBool_to_String(changedVector, chars);
    TiltedPhoques::Serialization::WriteString(aWriter, chars);

    auto biter = changedVector.begin() + Booleans.size();
    for (size_t i = 0; i < Integers.size(); i++)
        if (*biter++)
            TiltedPhoques::Serialization::WriteVarInt(aWriter, Integers[i] & 0xFFFFFFFF);
    for (size_t i = 0; i < Floats.size(); i++)
        if (*biter++)
            TiltedPhoques::Serialization::WriteFloat(aWriter, Floats[i]);
}

// Reads 3 VarInts that represent the size of the Booleans, Integers and Floats.
// That's followed by a bitstream in a string of the Booleans values combined
// with a Changed? truth table for Integers and Floats.
// The Changed? table is scanned and for each true bit, the corresponsing Integer
// or Float is deserialized.
// 
void AnimationVariables::ApplyDiff(TiltedPhoques::Buffer::Reader& aReader)
{
    const size_t booleansSize = CoopPacketSafety::ClampCollectionCount(
        TiltedPhoques::Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxAnimVars);
    const size_t integersSize = CoopPacketSafety::ClampCollectionCount(
        TiltedPhoques::Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxAnimVars);
    const size_t floatsSize = CoopPacketSafety::ClampCollectionCount(
        TiltedPhoques::Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxAnimVars);

    if (Integers.size() != integersSize)
        Integers.assign(integersSize, 0);
    if (Floats.size() != floatsSize)
        Floats.assign(floatsSize, 0.f);
    
    TiltedPhoques::Vector<bool> changedVector(booleansSize + integersSize + floatsSize);
    auto chars = TiltedPhoques::Serialization::ReadString(aReader);
    String_to_VectorBool(chars, changedVector);

    Booleans.assign(changedVector.begin(), changedVector.begin() + booleansSize);

    auto biter = changedVector.begin() + booleansSize;
    for (size_t i = 0; i < integersSize; i++)
        if (*biter++ && !aReader.Eof())
            Integers[i] = TiltedPhoques::Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    for (size_t i = 0; i < floatsSize; i++)
        if (*biter++ && !aReader.Eof())
            Floats[i] = TiltedPhoques::Serialization::ReadFloat(aReader);
}
