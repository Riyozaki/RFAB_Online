#include <Coop/DurableFile.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Coop
{
namespace
{
constexpr std::array<uint8_t, 8> kMagic{'R', 'F', 'A', 'B', 'J', 'N', 'L', 1};
uint32_t Checksum(std::span<const uint8_t> aBytes) noexcept
{
    uint32_t crc = ~0u;
    for (const auto byte : aBytes)
    {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
void Put32(uint8_t* aOut, uint32_t aValue) noexcept
{
    for (unsigned i = 0; i < 4; ++i) aOut[i] = static_cast<uint8_t>(aValue >> (8 * i));
}
uint32_t Get32(const uint8_t* aIn) noexcept
{
    uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(aIn[i]) << (8 * i);
    return value;
}
bool Fail(const DurableFile::Fault& aFault, DurableFile::Step aStep) { return aFault && aFault(aStep); }
} // namespace

bool DurableFile::Write(std::span<const uint8_t> aBytes, const Fault& aFault) const noexcept
{
    try
    {
        if (aBytes.size() > kMaxBytes || Fail(aFault, Step::BeforeWrite)) return false;
        std::error_code ec;
        const auto parent = m_path.has_parent_path() ? m_path.parent_path() : std::filesystem::path(".");
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
        auto temporary = m_path;
        temporary += ".pending"; // same filesystem; a leftover is never promoted by Read()
        std::array<uint8_t, 16> header{};
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        Put32(header.data() + 8, static_cast<uint32_t>(aBytes.size()));
        Put32(header.data() + 12, Checksum(aBytes));
#ifdef _WIN32
        const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        auto write = [&](std::span<const uint8_t> bytes) {
            DWORD written{};
            return WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
        };
        bool ok = write(header) && write(aBytes) && !Fail(aFault, Step::AfterWrite) && FlushFileBuffers(handle);
        if (Fail(aFault, Step::AfterFlush)) ok = false;
        if (!CloseHandle(handle)) ok = false;
        if (!ok) return false;
        if (!MoveFileExW(temporary.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
#else
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        auto write = [&](std::span<const uint8_t> bytes) {
            while (!bytes.empty())
            {
                const auto count = ::write(fd, bytes.data(), bytes.size());
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return false;
                bytes = bytes.subspan(static_cast<size_t>(count));
            }
            return true;
        };
        bool ok = write(header) && write(aBytes) && !Fail(aFault, Step::AfterWrite) && ::fsync(fd) == 0;
        if (Fail(aFault, Step::AfterFlush)) ok = false;
        if (::close(fd) != 0) ok = false;
        if (!ok) return false;
        std::filesystem::rename(temporary, m_path, ec); // POSIX atomic replacement; never delete the old file first
        if (ec) return false;
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) return false;
        ok = ::fsync(directory) == 0;
        if (::close(directory) != 0) ok = false;
        if (!ok) return false;
#endif
        return !Fail(aFault, Step::AfterReplace);
    }
    catch (...) { return false; }
}

DurableFile::ReadResult DurableFile::Read(std::vector<uint8_t>& aBytes) const noexcept
{
    aBytes.clear();
    try
    {
        std::error_code ec;
        const bool exists = std::filesystem::exists(m_path, ec);
        if (ec) return ReadResult::Failed;
        if (!exists) return ReadResult::Missing;
        const auto size = std::filesystem::file_size(m_path, ec);
        if (ec || size < 16 || size > kMaxBytes + 16ull) return ReadResult::Failed;
        std::ifstream input(m_path, std::ios::binary);
        std::array<uint8_t, 16> header{};
        if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
            !std::equal(kMagic.begin(), kMagic.end(), header.begin()) || Get32(header.data() + 8) != size - 16)
            return ReadResult::Failed;
        std::vector<uint8_t> bytes(size - 16);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            Checksum(bytes) != Get32(header.data() + 12)) return ReadResult::Failed;
        aBytes = std::move(bytes);
        return ReadResult::Loaded;
    }
    catch (...) { return ReadResult::Failed; }
}
} // namespace Coop
