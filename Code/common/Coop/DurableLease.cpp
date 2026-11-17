#include <Coop/DurableLease.h>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Coop
{
DurableLease::DurableLease(const std::filesystem::path& path) noexcept
{
    try
    {
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return;
#ifdef _WIN32
        auto* handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) m_handle = reinterpret_cast<intptr_t>(handle);
#else
        const auto fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) return;
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) m_handle = fd;
        else ::close(fd);
#endif
    }
    catch (...) {}
}
DurableLease::~DurableLease() noexcept
{
    if (!Acquired()) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#else
    ::flock(static_cast<int>(m_handle), LOCK_UN);
    ::close(static_cast<int>(m_handle));
#endif
}
}
