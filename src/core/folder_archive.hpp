#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tempshare {

struct File;

struct FolderArchive {
    std::string name;
    std::shared_ptr<const std::filesystem::path> directory;
};

/** 生成可下载的 ZIP。最后一个持有者释放临时目录。 */
FolderArchive archive_folder(const std::filesystem::path& folder, const std::function<bool()>& cancelled);
FolderArchive archive_files(std::string name, const std::filesystem::path& root,
                            const std::vector<File>& files, const std::function<bool()>& cancelled);

}
