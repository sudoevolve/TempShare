#include "core/shared_source.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tempshare {

bool SourceVersion::operator==(const SourceVersion& other) const {
    return volume == other.volume && file == other.file && size == other.size &&
        modified == other.modified && precision == other.precision &&
        changed == other.changed && change_precision == other.change_precision;
}

SharedSource::SharedSource(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        CloseHandle(handle);
        return;
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return;
    }
    file_ = _fdopen(descriptor, "rb");
    if (!file_) _close(descriptor);
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0) return;
    file_ = fdopen(descriptor, "rb");
    if (!file_) close(descriptor);
#endif
}

SharedSource::~SharedSource() {
    if (file_) std::fclose(file_);
}

std::optional<SourceVersion> SharedSource::version() const {
    if (!file_) return {};
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO times{};
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_)));
    if (!GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof(times)) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    return SourceVersion{info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        static_cast<std::uint64_t>(times.LastWriteTime.QuadPart), 0,
        static_cast<std::uint64_t>(times.ChangeTime.QuadPart), 0};
#else
    struct stat info{};
    if (fstat(fileno(file_), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) return {};
#ifdef __APPLE__
    const auto time = info.st_mtimespec;
    const auto change = info.st_ctimespec;
#else
    const auto time = info.st_mtim;
    const auto change = info.st_ctim;
#endif
    return SourceVersion{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
        static_cast<std::uint64_t>(info.st_size), static_cast<std::uint64_t>(time.tv_sec),
        static_cast<std::uint32_t>(time.tv_nsec), static_cast<std::uint64_t>(change.tv_sec),
        static_cast<std::uint32_t>(change.tv_nsec)};
#endif
}

bool SharedSource::read(std::uint64_t offset, char* output, std::size_t length) {
    if (!file_) return false;
    if (position_ != offset) {
#ifdef _WIN32
        if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) return false;
#else
        if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
        position_ = offset;
    }
    const auto size = std::fread(output, 1, length, file_);
    position_ += size;
    return size == length;
}

}  // namespace tempshare
