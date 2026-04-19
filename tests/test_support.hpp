#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <chrono>

namespace teamspeak_cli::tests {

inline void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

template <typename T, typename U>
void expect_eq(const T& actual, const U& expected, const std::string& message) {
    if (!(actual == expected)) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

inline void expect_contains(
    std::string_view haystack, std::string_view needle, const std::string& message
) {
    if (haystack.find(needle) == std::string_view::npos) {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}

inline auto make_temp_path(std::string_view prefix) -> std::filesystem::path {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "-" + std::to_string(nonce));
}

}  // namespace teamspeak_cli::tests
