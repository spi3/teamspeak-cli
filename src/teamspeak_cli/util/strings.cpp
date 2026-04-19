#include "teamspeak_cli/util/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>

namespace teamspeak_cli::util {

auto trim(std::string_view value) -> std::string {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

auto lower_copy(std::string_view value) -> std::string {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

auto iequals(std::string_view lhs, std::string_view rhs) -> bool {
    return lower_copy(lhs) == lower_copy(rhs);
}

auto split(std::string_view value, char delimiter) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : value) {
        if (ch == delimiter) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    parts.push_back(current);
    return parts;
}

auto parse_u64(std::string_view value) -> std::optional<std::uint64_t> {
    std::uint64_t parsed = 0;
    const auto begin = value.data();
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

auto parse_u16(std::string_view value) -> std::optional<std::uint16_t> {
    const auto parsed = parse_u64(value);
    if (!parsed.has_value() || *parsed > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

auto parse_bool(std::string_view value) -> std::optional<bool> {
    const auto normalized = lower_copy(value);
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        return false;
    }
    return std::nullopt;
}

auto join(const std::vector<std::string>& parts, std::string_view delimiter) -> std::string {
    std::ostringstream out;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            out << delimiter;
        }
        out << parts[index];
    }
    return out.str();
}

}  // namespace teamspeak_cli::util
