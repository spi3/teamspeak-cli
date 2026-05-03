#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::bridge::protocol {

inline constexpr std::string_view kMagic = "tscli1";

using Fields = std::vector<std::string>;

struct Response {
    std::string type;
    std::vector<Fields> payload;
};

auto split_fields(std::string_view line) -> Fields;
auto join_fields(const Fields& fields) -> std::string;

auto hex_encode(std::string_view value) -> std::string;
auto hex_decode(std::string_view value) -> domain::Result<std::string>;

auto read_line(int fd, std::string& line) -> bool;
auto write_line(int fd, const Fields& fields) -> bool;

auto encode_error(const domain::Error& error) -> Fields;
auto decode_error(const Fields& fields) -> domain::Error;

auto encode(const domain::PluginInfo& info) -> std::vector<Fields>;
auto decode_plugin_info(const std::vector<Fields>& lines) -> domain::Result<domain::PluginInfo>;

auto encode(const domain::ConnectionState& state) -> std::vector<Fields>;
auto decode_connection_state(const std::vector<Fields>& lines) -> domain::Result<domain::ConnectionState>;

auto encode(const domain::ServerInfo& info) -> std::vector<Fields>;
auto decode_server_info(const std::vector<Fields>& lines) -> domain::Result<domain::ServerInfo>;

auto encode(const domain::Channel& channel) -> Fields;
auto encode_channels(const std::vector<domain::Channel>& channels) -> std::vector<Fields>;
auto decode_channel(const Fields& fields) -> domain::Result<domain::Channel>;
auto decode_channels(const std::vector<Fields>& lines) -> domain::Result<std::vector<domain::Channel>>;

auto encode(const domain::Client& client) -> Fields;
auto encode_clients(const std::vector<domain::Client>& clients) -> std::vector<Fields>;
auto decode_client(const Fields& fields) -> domain::Result<domain::Client>;
auto decode_clients(const std::vector<Fields>& lines) -> domain::Result<std::vector<domain::Client>>;

auto encode(const domain::ServerGroupApplication& application) -> std::vector<Fields>;
auto decode_server_group_application(const std::vector<Fields>& lines)
    -> domain::Result<domain::ServerGroupApplication>;

auto encode_event(const std::optional<domain::Event>& event) -> std::vector<Fields>;
auto decode_event(const std::vector<Fields>& lines) -> domain::Result<std::optional<domain::Event>>;

}  // namespace teamspeak_cli::bridge::protocol
