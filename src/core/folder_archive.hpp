#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace tempshare {

struct FolderArchive {
    std::string name;
    std::shared_ptr<const std::filesystem::path> directory;
};

/** 生成可下载的 ZIP。最后一个持有者释放临时目录。 */
FolderArchive archive_folder(const std::filesystem::path& folder, const std::function<bool()>& cancelled);

}
