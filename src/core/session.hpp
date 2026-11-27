#pragma once

#include "core/shared_source.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tempshare {

inline constexpr std::uint64_t max_file_size = 1024ULL * 1024 * 1024;
inline constexpr std::uint64_t max_session_bytes = 4ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t minimum_free_bytes = 16ULL * 1024 * 1024;
inline constexpr std::size_t max_text_size = 32 * 1024;
inline constexpr std::size_t max_cast_frame_size = 2 * 1024 * 1024;

enum class State { pending, approved, rejected, uploading, complete };
enum class UploadFailure { unavailable, storage, insufficient_space };
enum class SnapshotPage { all, share, sync, received, devices, settings, cast, nearby, home };

struct Pairing {
    std::string token;
    std::string verification;
    State state = State::pending;
    std::string name;
    std::string trusted_key;
    std::uint64_t remaining_seconds = 0;
    bool online = false;
};

struct File {
    std::string id;
    std::string name;
    std::uint64_t size;
    std::filesystem::path path;
    std::optional<SourceVersion> source{};
    std::shared_ptr<const std::filesystem::path> temporary_directory{};
    bool folder = false;
    std::string received_from{};
    std::uint64_t received_at = 0;
};

struct Offer {
    std::string id;
    std::string name;
    std::uint64_t size;
    State state = State::pending;
    std::uint64_t transferred = 0;
    std::string batch{};
    std::string relative_directory{};
    bool image = false;
};

struct Permissions {
    bool download = true;
    bool upload = true;
    bool text = true;
    bool cast = true;
};

struct TrustedDevice {
    std::string key;
    std::string name;
    Permissions permissions;
};

struct Message {
    std::string text;
    bool from_desktop;
    std::uint64_t sent_at = 0;
    std::string attachment{};
    std::uint64_t sequence = 0;
    bool remote_attachment = false;
    bool local_only = false;
};

struct CastSignal {
    std::string offer;
    std::string answer;
    bool viewing = false;
    unsigned revision = 0;
    bool ready = false;
    bool frames = false;
};

struct CastFrame {
    std::uint64_t sequence = 0;
    std::string jpeg;
};

struct DownloadActivity {
    std::string id, name;
    std::uint64_t size = 0, transferred = 0;
    bool finished = false, success = false;
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
    Permissions permissions;
    std::vector<Message> messages;
    bool casting = false;
    bool trust_enabled = false;
    std::vector<TrustedDevice> trusted;
    bool capture_ready = false;
    bool viewer_answered = false;
    bool cast_compatibility = false;
    std::string messages_cleared;
    std::vector<DownloadActivity> downloads;
    std::uint64_t remaining_seconds = 0;
    std::size_t pending_transfers = 0;
    bool unlimited = false;
    std::uint64_t lifetime_seconds = 0;
};

struct NotificationState {
    std::filesystem::path directory;
    std::string pairing;
    std::string offer;
    std::string received;
    std::uint64_t incoming_messages = 0;
};

struct FolderProgress {
    std::size_t scanned = 0;
    std::size_t skipped = 0;
    std::size_t added = 0;
    std::size_t removed = 0;
};

struct Upload {
    std::shared_ptr<const std::filesystem::path> temporary;
    std::filesystem::path destination;
};

/** Owns one expiring invitation and its independently approved browser and upload requests. */
class Session {
public:
    using Clock = std::chrono::steady_clock;
    explicit Session(std::filesystem::path directory, std::function<void()> changed = {},
                     Clock::duration lifetime = std::chrono::minutes(30), std::filesystem::path trust_directory = {},
                     std::filesystem::path history_file = {});

    void start();
    void end();
    bool extend();
    /** @brief 设置会话时长并从现在起应用；零表示不限时，不会重新激活已结束会话。 */
    bool set_lifetime(Clock::duration lifetime);
    bool is_active() const;
    bool expire();
    bool invitation_active(std::string_view invitation) const;
    Snapshot snapshot(SnapshotPage page = SnapshotPage::all) const;
    NotificationState notification_state() const;
    bool clear_received();
    bool set_directory(std::filesystem::path directory);
    bool share(const std::filesystem::path& path, bool image = false);
    bool share_folder(std::string_view invitation, const std::filesystem::path& path,
                      const std::string& refresh = {}, const std::function<bool()>& cancelled = {},
                      const std::function<void(FolderProgress)>& progress = {});
    bool share_archive(std::string_view invitation, std::string name,
                       std::shared_ptr<const std::filesystem::path> directory);
    void unshare(const std::string& id);
    bool decide_pairing(const std::string& token, bool accept);
    bool disconnect(std::string_view token);
    bool touch(std::string_view token);
    bool cancel_offer(const std::string& id);
    bool decide_offer(const std::string& id, bool accept);
    std::size_t decide_offers(const std::vector<std::string>& ids, bool accept);
    void set_permissions(Permissions permissions);
    Permissions permissions() const;
    bool send_text(std::string text);
    bool send_text(std::string_view token, std::string text);
    std::optional<std::vector<Message>> messages(std::string_view token) const;
    std::optional<File> message_attachment(std::string_view token, std::string_view id) const;
    std::uint64_t text_revision() const;
    void clear_messages();
    bool clear_messages(std::string_view token);
    bool receive_peer_message(std::string_view token, const Message& message);
    bool record_peer_sent(std::string_view token, const std::string& text);
    void merge_peer_clear(std::string_view token, const std::string& mark);
    bool record_download(const std::filesystem::path& path, const std::string& sender);
    void download_progress(const std::string& id, const std::string& name, std::uint64_t size, std::uint64_t sent);
    void download_finished(const std::string& id, bool success);

    std::string open_cast();
    bool stop_cast(std::string_view token = {});
    std::optional<CastSignal> cast_host(std::string_view token);
    bool cast_ready(std::string_view token);
    std::optional<bool> capture_ready(std::string_view token) const;
    std::optional<CastSignal> cast_viewer(std::string_view token, bool viewing, std::string_view viewer = {},
                                          bool frames = false);
    bool cast_offer(std::string_view token, std::string sdp);
    bool cast_answer(std::string_view token, unsigned revision, std::string sdp, std::string_view viewer = {});
    bool publish_cast_frame(std::string_view token, unsigned revision, std::string jpeg);
    std::optional<CastFrame> cast_frame(std::string_view token, std::string_view viewer, std::uint64_t after,
                                       Clock::duration wait = {}) const;

    std::optional<Pairing> pair(std::string_view invitation, std::string name = "浏览器");
    std::optional<Pairing> reconnect(std::string_view trusted);
    bool remember_device();
    bool revoke_device(const std::string& key);
    std::optional<Pairing> pairing(std::string_view token) const;
    std::string_view pairing_error(std::string_view token) const;
    bool authorized(std::string_view token) const;
    bool upload_allowed(std::string_view token) const;
    bool downloading(std::string_view token, const std::string& id, const std::string& folder = {}) const;
    std::optional<std::vector<File>> shared(std::string_view token) const;
    std::optional<std::vector<File>> folder_contents(std::string_view token, const std::string& root,
                                                     const std::string& directory) const;
    std::optional<File> folder_file(std::string_view token, const std::string& root, const std::string& id) const;
    std::optional<std::vector<File>> folder_selection(std::string_view token, const std::string& root,
        const std::string& directory, const std::vector<std::string>& selected) const;
    std::optional<Offer> offer(std::string_view token, std::string name, std::uint64_t size, std::string batch = {},
                               std::string relative_directory = {}, bool image = false);
    std::optional<std::vector<Offer>> offers(std::string_view token) const;
    std::uint64_t files_revision() const;
    std::uint64_t offers_revision() const;
    bool cancel_offer(std::string_view token, const std::string& id);
    std::optional<Upload> begin_upload(std::string_view token, const std::string& id,
                                       std::uint64_t size, std::optional<std::uint64_t> offset = {},
                                       UploadFailure* failure = nullptr);
    bool uploading(std::string_view token, const std::string& id) const;
    bool finish_upload(std::string_view token, const std::string& id, const Upload& upload,
                       std::optional<std::uint64_t> transferred = {});
    void retry_upload(std::string_view token, const std::string& id);

private:
    bool active() const;
    void reset_content();
    bool has_access(std::string_view token) const;
    bool add_message(std::string text, bool from_desktop);
    bool cast_available() const;
    bool cast_host_access(std::string_view token) const;
    void reset_cast_signal();
    bool save_received() const;

    mutable std::mutex mutex_;
    std::function<void()> changed_;
    Clock::duration lifetime_;
    Clock::time_point expires_{};
    Clock::time_point retry_after_{};
    Clock::time_point pairing_deadline_{};
    Clock::time_point peer_seen_{};
    mutable Clock::time_point progress_at_{};
    unsigned failed_codes_ = 0;
    std::string invitation_;
    std::string code_;
    std::optional<Pairing> pairing_;
    std::string previous_pairing_;
    std::string_view previous_pairing_error_ = "session_ended";
    std::vector<Offer> offers_;
    std::uint64_t offers_revision_ = 0;
    std::uint64_t committed_bytes_ = 0;
    std::unordered_map<std::string, Upload> partials_;
    std::vector<File> shared_;
    struct FolderNode {
        File entry;
        std::vector<std::string> children;
    };
    using FolderTree = std::unordered_map<std::string, FolderNode>;
    std::unordered_map<std::string, FolderTree> folders_;
    std::uint64_t files_revision_ = 0;
    std::vector<File> received_;
    std::filesystem::path history_file_;
    std::filesystem::path directory_;
    std::filesystem::path staging_;
    std::filesystem::path trust_directory_;
    std::vector<TrustedDevice> trusted_;
    Permissions permissions_;
    std::vector<Message> messages_;
    std::uint64_t text_revision_ = 0;
    std::uint64_t incoming_messages_ = 0;
    std::uint64_t message_sequence_ = 0;
    std::string messages_cleared_;
    std::vector<DownloadActivity> downloads_;
    std::string cast_token_;
    CastSignal cast_;
    CastFrame cast_frame_;
    mutable std::condition_variable cast_frame_changed_;
    Clock::time_point cast_frame_at_{};
    unsigned cast_revision_ = 0;
    Clock::time_point cast_host_seen_{};
    Clock::time_point cast_viewer_seen_{};
    std::string cast_viewer_id_;
};

std::string random_token();
std::string json_string(std::string_view value);
std::string_view state_name(State state);
bool valid_filename(std::string_view name);
void clean_interrupted_uploads(const std::filesystem::path& directory);

}  // namespace tempshare
