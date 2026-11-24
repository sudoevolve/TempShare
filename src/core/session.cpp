#include "core/session.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <utility>

namespace tempshare {
namespace {

std::string short_code(const std::string& previous = {}) {
    std::random_device random;
    std::uniform_int_distribution<int> digits(100000, 999999);
    std::string code;
    do {
        code = std::to_string(digits(random));
    } while (code == previous);
    return code;
}

constexpr char hex[] = "0123456789abcdef";

bool equals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

bool utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        const int trailing = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC2 ? 1 : 0;
        if (!trailing || first > 0xF4 || i + trailing > value.size()) return false;
        const auto second = static_cast<unsigned char>(value[i]);
        if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second > 0x9F) ||
            (first == 0xF0 && second < 0x90) || (first == 0xF4 && second > 0x8F)) return false;
        for (int j = 0; j < trailing; ++j) {
            if ((static_cast<unsigned char>(value[i++]) & 0xC0) != 0x80) return false;
        }
    }
    return true;
}

template<typename Offers>
auto find_offer(Offers& offers, const std::string& id) {
    return std::find_if(offers.begin(), offers.end(), [&](const Offer& offer) {
        return offer.id == id;
    });
}

}  // namespace

std::string random_token() {
    std::random_device random;
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::string result;
    for (int i = 0; i < 16; ++i) {
        const auto value = byte(random);
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}

std::string json_string(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

std::string_view state_name(State state) {
    switch (state) {
    case State::pending: return "pending";
    case State::approved: return "approved";
    case State::rejected: return "rejected";
    case State::uploading: return "uploading";
    case State::complete: return "complete";
    }
    return {};
}

bool valid_filename(std::string_view name) {
    if (name.empty() || name.size() > 240 || name.back() == '.' || name.back() == ' ' || !utf8(name)) {
        return false;
    }
    for (const unsigned char c : name) {
        if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            return false;
        }
    }
    auto stem = std::string(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::toupper(c); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) {
        const auto suffix = stem.substr(3);
        if ((suffix.size() == 1 && suffix.front() >= '0' && suffix.front() <= '9') ||
            suffix == "¹" || suffix == "²" || suffix == "³") return false;
    }
    return true;
}

Session::Session(std::filesystem::path directory, std::function<void()> changed, Clock::duration lifetime)
    : changed_(std::move(changed)), lifetime_(lifetime), directory_(std::move(directory)) {}

bool Session::active() const { return Clock::now() < expires_; }

bool Session::has_access(std::string_view token) const {
    return active() && pairing_ && pairing_->state == State::approved && equals(token, pairing_->token);
}

void Session::notify() const {
    if (changed_) changed_();
}

void Session::start() {
    std::lock_guard lock(mutex_);
    invitation_ = random_token();
    code_ = short_code(code_);
    expires_ = Clock::now() + lifetime_;
    retry_after_ = {};
    failed_codes_ = 0;
    pairing_.reset();
    offers_.clear();
    shared_.clear();
    staging_.clear();
    notify();
}

void Session::end() {
    std::lock_guard lock(mutex_);
    expires_ = {};
    pairing_.reset();
    offers_.clear();
    shared_.clear();
    staging_.clear();
    notify();
}

bool Session::is_active() const {
    std::lock_guard lock(mutex_);
    return active();
}

Snapshot Session::snapshot() const {
    std::lock_guard lock(mutex_);
    if (!active()) return {false, {}, {}, {}, {}, {}, received_, directory_};
    return {true, invitation_, code_, pairing_, offers_, shared_, received_, directory_};
}

bool Session::set_directory(std::filesystem::path directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return false;
    std::lock_guard lock(mutex_);
    directory_ = std::move(directory);
    staging_.clear();
    notify();
    return true;
}

bool Session::share(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    const auto size = std::filesystem::file_size(path, error);
    const std::string name = path.filename().u8string();
    if (error || size > max_file_size || !utf8(name)) return false;
    std::lock_guard lock(mutex_);
    if (!active() || shared_.size() >= 100) return false;
    shared_.push_back({random_token(), name, size, path});
    notify();
    return true;
}

void Session::unshare(const std::string& id) {
    std::lock_guard lock(mutex_);
    shared_.erase(std::remove_if(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    }), shared_.end());
    notify();
}

std::optional<Pairing> Session::pair(std::string_view invitation) {
    std::lock_guard lock(mutex_);
    if (!active() || pairing_) return {};
    if (!equals(invitation, invitation_)) {
        const auto now = Clock::now();
        if (now < retry_after_) return {};
        if (!equals(invitation, code_)) {
            if (++failed_codes_ >= 10) {
                failed_codes_ = 0;
                retry_after_ = now + std::chrono::minutes(1);
            }
            return {};
        }
    }
    pairing_ = Pairing{random_token(), short_code(), State::pending};
    notify();
    return pairing_;
}

std::optional<Pairing> Session::pairing(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || !equals(token, pairing_->token)) return {};
    return pairing_;
}

bool Session::decide_pairing(const std::string& token, bool accept) {
    std::lock_guard lock(mutex_);
    if (!active() || !pairing_ || pairing_->token != token || pairing_->state != State::pending) return false;
    if (accept) {
        pairing_->state = State::approved;
    } else {
        pairing_.reset();
        invitation_ = random_token();
        code_ = short_code(code_);
    }
    notify();
    return true;
}

bool Session::authorized(std::string_view token) const {
    std::lock_guard lock(mutex_);
    return has_access(token);
}

bool Session::downloading(std::string_view token, const std::string& id) const {
    std::lock_guard lock(mutex_);
    return has_access(token) && std::any_of(shared_.begin(), shared_.end(), [&](const File& file) {
        return file.id == id;
    });
}

std::optional<std::vector<File>> Session::shared(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    return shared_;
}

std::optional<Offer> Session::offer(std::string_view token, std::string name, std::uint64_t size) {
    if (!valid_filename(name) || size > max_file_size) return {};
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    if (offers_.size() >= 100) return {};
    offers_.push_back({random_token(), std::move(name), size});
    notify();
    return offers_.back();
}

std::optional<std::vector<Offer>> Session::offers(std::string_view token) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    return offers_;
}

bool Session::decide_offer(const std::string& id, bool accept) {
    std::lock_guard lock(mutex_);
    if (!active()) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::pending) return false;
    offer->state = accept ? State::approved : State::rejected;
    notify();
    return true;
}

bool Session::cancel_offer(std::string_view token, const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state == State::complete) return false;
    offer->state = State::rejected;
    notify();
    return true;
}

std::optional<Upload> Session::begin_upload(std::string_view token, const std::string& id, std::uint64_t size) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return {};
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::approved || offer->size != size) return {};
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) return {};
    if (staging_.empty()) {
        const auto folder = directory_ / ("TempShare-" + random_token().substr(0, 12));
        if (!std::filesystem::create_directory(folder, error)) return {};
        staging_ = folder;
    }
    const auto name = std::filesystem::u8path(offer->name);
    const auto destination = staging_ / name;
    const auto temporary = staging_ / (".upload-" + random_token() + ".part");
    offer->state = State::uploading;
    notify();
    return Upload{temporary, destination};
}

bool Session::finish_upload(std::string_view token, const std::string& id, const Upload& upload) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return false;
    auto offer = find_offer(offers_, id);
    if (offer == offers_.end() || offer->state != State::uploading) return false;
    std::error_code error;
    auto destination = upload.destination;
    for (unsigned suffix = 2; std::filesystem::exists(destination, error); ++suffix) {
        const auto prefix = std::to_string(suffix) + "-";
        const auto name = upload.destination.filename().u8string();
        destination = upload.destination.parent_path() / std::filesystem::u8path(prefix + name);
    }
    if (error) return false;
    File received{id, destination.filename().u8string(), offer->size, destination};
    received_.reserve(received_.size() + 1);
    std::filesystem::rename(upload.temporary, destination, error);
    if (error) return false;
    offer->state = State::complete;
    received_.push_back(std::move(received));
    notify();
    return true;
}

bool Session::uploading(std::string_view token, const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return false;
    const auto offer = find_offer(offers_, id);
    return offer != offers_.end() && offer->state == State::uploading;
}

void Session::retry_upload(std::string_view token, const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!has_access(token)) return;
    auto offer = find_offer(offers_, id);
    if (offer != offers_.end() && offer->state == State::uploading) {
        offer->state = State::approved;
        notify();
    }
}

}  // namespace tempshare
