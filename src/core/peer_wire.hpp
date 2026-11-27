#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tempshare::peer_wire {
// 有界长度前缀字段，不使用换行分隔；文本、路径中的换行不会改变协议结构。
inline constexpr std::size_t limit = 4 * 1024 * 1024;
inline std::string encode(const std::vector<std::string>& fields) {
    std::string result;
    for (const auto& field : fields) {
        const auto size = static_cast<std::uint32_t>(field.size());
        for (int shift = 24; shift >= 0; shift -= 8) result += static_cast<char>(size >> shift);
        result += field;
    }
    return result;
}
inline std::optional<std::vector<std::string>> decode(std::string_view body) {
    if (body.size() > limit) return {};
    std::vector<std::string> result;
    while (!body.empty()) {
        if (body.size() < 4 || result.size() >= 60000) return {};
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(body[i]);
        body.remove_prefix(4);
        if (size > 65536 || size > body.size()) return {};
        result.emplace_back(body.substr(0, size));
        body.remove_prefix(size);
    }
    return result;
}
inline std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    return value;
}
inline bool token(std::string_view text) {
    return text.size() == 32 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
} // namespace tempshare::peer_wire
