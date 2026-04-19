#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace teamspeak_cli::util {

auto trim(std::string_view value) -> std::string;
auto lower_copy(std::string_view value) -> std::string;
auto iequals(std::string_view lhs, std::string_view rhs) -> bool;
auto split(std::string_view value, char delimiter) -> std::vector<std::string>;
auto parse_u64(std::string_view value) -> std::optional<std::uint64_t>;
auto parse_u16(std::string_view value) -> std::optional<std::uint16_t>;
auto parse_bool(std::string_view value) -> std::optional<bool>;
auto join(const std::vector<std::string>& parts, std::string_view delimiter) -> std::string;

}  // namespace teamspeak_cli::util
