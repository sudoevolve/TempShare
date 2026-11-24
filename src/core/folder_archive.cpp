#include "core/folder_archive.hpp"
#include "core/session.hpp"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <fstream>
#include <stdexcept>

namespace tempshare {
namespace {

struct Archive {
    mz_zip_archive zip{};
    std::fstream output;
    const std::function<bool()>& cancelled;

    Archive(const std::filesystem::path& path, const std::function<bool()>& check)
        : output(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc),
          cancelled(check) {
        zip.m_pIO_opaque = this;
        zip.m_pWrite = [](void* opaque, mz_uint64 offset, const void* data, size_t bytes) -> size_t {
            auto& archive = *static_cast<Archive*>(opaque);
            if (offset > max_file_size || bytes > max_file_size - offset || archive.cancelled()) return 0;
            archive.output.seekp(static_cast<std::streamoff>(offset));
            archive.output.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            return archive.output ? bytes : 0;
        };
        if (!output || !mz_zip_writer_init(&zip, 0)) throw std::runtime_error("无法创建 ZIP 文件");
    }

    ~Archive() { mz_zip_writer_end(&zip); }
};

struct Entry {
    std::ifstream input;
    const std::function<bool()>& cancelled;
    std::uint64_t copied = 0;
};

size_t read_entry(void* opaque, mz_uint64 offset, void* buffer, size_t bytes) {
    auto& entry = *static_cast<Entry*>(opaque);
    if (entry.cancelled()) return 0;
    entry.input.seekg(static_cast<std::streamoff>(offset));
    entry.input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    const auto copied = static_cast<size_t>(entry.input.gcount());
    entry.copied = offset + copied;
    return copied;
}

std::shared_ptr<const std::filesystem::path> temporary_directory() {
    const auto path = std::filesystem::temp_directory_path() / ("TempShare-zip-" + random_token());
    auto directory = std::unique_ptr<std::filesystem::path>(new std::filesystem::path(path));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("无法创建 ZIP 临时目录");
    return {directory.release(), [](const std::filesystem::path* value) {
        std::error_code error;
        std::filesystem::remove_all(*value, error);
        delete value;
    }};
}

}  // namespace

FolderArchive archive_folder(const std::filesystem::path& folder, const std::function<bool()>& cancelled) {
    namespace fs = std::filesystem;
    const auto root = fs::canonical(folder);
    const std::string name = root.filename().u8string();
    if (!fs::is_directory(root) || !valid_filename(name) || !valid_filename(name + ".zip")) {
        throw std::runtime_error("文件夹名称不可用");
    }
    auto directory = temporary_directory();
    Archive archive(*directory / "folder.zip", cancelled);
    if (!mz_zip_writer_add_mem(&archive.zip, (name + '/').c_str(), nullptr, 0, 0)) {
        throw std::runtime_error("无法添加 ZIP 根目录");
    }
    std::uint64_t total = 0;
    unsigned entries = 0;
    for (auto it = fs::recursive_directory_iterator(root), end = fs::recursive_directory_iterator(); it != end; ++it) {
        if (cancelled()) throw std::runtime_error("打包已取消");
        const auto relative = it->path().lexically_relative(root);
        const auto state = it->symlink_status();
        if (fs::is_symlink(state) || fs::canonical(it->path()).lexically_relative(root) != relative) {
            it.disable_recursion_pending();
            continue;
        }
        if (++entries > 10000) throw std::runtime_error("文件夹包含超过 10000 个条目");
        for (const auto& component : relative) {
            if (!valid_filename(component.u8string())) throw std::runtime_error("文件夹含有无法跨平台解压的名称");
        }
        auto entry_name = name + '/' + relative.generic_u8string();
        if (fs::is_directory(state)) {
            entry_name += '/';
            if (!mz_zip_writer_add_mem(&archive.zip, entry_name.c_str(), nullptr, 0, 0)) {
                throw std::runtime_error("无法写入 ZIP 目录");
            }
        } else if (fs::is_regular_file(state)) {
            const auto size = it->file_size();
            if (size > max_file_size - total) throw std::runtime_error("文件夹超过 1 GiB");
            total += size;
            Entry entry{std::ifstream(it->path(), std::ios::binary), cancelled};
            if (!entry.input || !mz_zip_writer_add_read_buf_callback(
                &archive.zip, entry_name.c_str(), read_entry, &entry, size,
                nullptr, nullptr, 0, MZ_BEST_SPEED, nullptr, 0, nullptr, 0) || entry.copied != size) {
                throw std::runtime_error("无法打包文件夹");
            }
        }
    }
    if (cancelled() || !mz_zip_writer_finalize_archive(&archive.zip)) throw std::runtime_error("无法完成 ZIP 文件");
    archive.output.close();
    if (!archive.output) throw std::runtime_error("无法保存 ZIP 文件");
    return {name + ".zip", std::move(directory)};
}

}  // namespace tempshare
