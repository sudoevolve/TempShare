#pragma once

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace tempshare {

struct SourceVersion {
    std::uint64_t volume;
    std::uint64_t file;
    std::uint64_t size;
    std::uint64_t modified;
    std::uint32_t precision;
    std::uint64_t changed;
    std::uint32_t change_precision;

    bool operator==(const SourceVersion& other) const;
};

/** Opens one regular file and reads and validates the same operating system handle. */
class SharedSource {
public:
    explicit SharedSource(const std::filesystem::path& path);
    ~SharedSource();
    SharedSource(const SharedSource&) = delete;
    SharedSource& operator=(const SharedSource&) = delete;

    std::optional<SourceVersion> version() const;
    bool read(std::uint64_t offset, char* output, std::size_t length);

private:
    std::FILE* file_ = nullptr;
    std::uint64_t position_ = 0;
};

}  // namespace tempshare
