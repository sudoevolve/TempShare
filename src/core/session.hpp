#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tempshare {

inline constexpr std::uint64_t max_file_size = 1024ULL * 1024 * 1024;

enum class State { pending, approved, rejected, uploading, complete };

struct Pairing {
    std::string token;
    std::string verification;
    State state = State::pending;
};

struct File {
    std::string id;
    std::string name;
    std::uint64_t size;
    std::filesystem::path path;
};

struct Offer {
    std::string id;
    std::string name;
    std::uint64_t size;
    State state = State::pending;
};

struct Snapshot {
    bool active = false;
    std::string invitation;
    std::string code;
    std::optional<Pairing> pairing;
    std::vector<Offer> offers;
    std::vector<File> shared;
    std::vector<File> received;
    std::filesystem::path directory;
};

struct Upload {
    std::filesystem::path temporary;
    std::filesystem::path destination;
};

/** Owns one expiring invitation and its independently approved browser and upload requests. */
class Session {
public:
    using Clock = std::chrono::steady_clock;
    explicit Session(std::filesystem::path directory, std::function<void()> changed = {},
                     Clock::duration lifetime = std::chrono::minutes(30));

    void start();
    void end();
    bool is_active() const;
    Snapshot snapshot() const;
    bool set_directory(std::filesystem::path directory);
    bool share(const std::filesystem::path& path);
    void unshare(const std::string& id);
    bool decide_pairing(const std::string& token, bool accept);
    bool decide_offer(const std::string& id, bool accept);

    std::optional<Pairing> pair(std::string_view invitation);
    std::optional<Pairing> pairing(std::string_view token) const;
    bool authorized(std::string_view token) const;
    bool downloading(std::string_view token, const std::string& id) const;
    std::optional<std::vector<File>> shared(std::string_view token) const;
    std::optional<Offer> offer(std::string_view token, std::string name, std::uint64_t size);
    std::optional<std::vector<Offer>> offers(std::string_view token) const;
    bool cancel_offer(std::string_view token, const std::string& id);
    std::optional<Upload> begin_upload(std::string_view token, const std::string& id, std::uint64_t size);
    bool uploading(std::string_view token, const std::string& id) const;
    bool finish_upload(std::string_view token, const std::string& id, const Upload& upload);
    void retry_upload(std::string_view token, const std::string& id);

private:
    bool active() const;
    bool has_access(std::string_view token) const;
    void notify() const;

    mutable std::mutex mutex_;
    std::function<void()> changed_;
    Clock::duration lifetime_;
    Clock::time_point expires_{};
    Clock::time_point retry_after_{};
    unsigned failed_codes_ = 0;
    std::string invitation_;
    std::string code_;
    std::optional<Pairing> pairing_;
    std::vector<Offer> offers_;
    std::vector<File> shared_;
    std::vector<File> received_;
    std::filesystem::path directory_;
    std::filesystem::path staging_;
};

std::string random_token();
std::string json_string(std::string_view value);
std::string_view state_name(State state);
bool valid_filename(std::string_view name);

}  // namespace tempshare
