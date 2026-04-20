#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <sys/types.h>
#include <string>
#include <vector>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"
#include "teamspeak_cli/sdk/backend_factory.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::daemon {

struct StatePaths {
    std::filesystem::path state_dir;
    std::filesystem::path pid_file;
    std::filesystem::path log_file;
    std::filesystem::path status_file;
    std::filesystem::path inbox_file;
    std::filesystem::path hooks_file;
};

struct Hook {
    std::string id;
    std::string event_type;
    std::string message_kind;
    std::string command;
};

struct Status {
    bool running = false;
    pid_t pid = 0;
    std::string profile;
    std::string backend;
    std::string started_at;
    std::string last_event_at;
    std::string last_event_type;
    std::string last_error;
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds::zero();
    std::size_t inbox_count = 0;
    std::size_t hook_count = 0;
};

auto resolve_state_dir() -> domain::Result<std::filesystem::path>;
auto state_paths_for(const std::filesystem::path& state_dir) -> StatePaths;

auto load_hooks(const StatePaths& paths) -> domain::Result<std::vector<Hook>>;
auto save_hooks(const StatePaths& paths, const std::vector<Hook>& hooks) -> domain::Result<void>;

auto read_inbox(const StatePaths& paths, std::size_t limit) -> domain::Result<std::vector<domain::Event>>;
auto append_inbox_event(const StatePaths& paths, const domain::Event& event) -> domain::Result<void>;

auto read_status(const StatePaths& paths) -> domain::Result<Status>;
auto write_status(const StatePaths& paths, const Status& status) -> domain::Result<void>;

auto run_event_daemon(
    const domain::Profile& profile,
    const sdk::InitOptions& init_options,
    const StatePaths& paths,
    std::chrono::milliseconds poll_interval,
    const std::function<bool()>& stop_requested,
    const sdk::BackendFactory& factory = sdk::BackendFactory{}
) -> domain::Result<void>;

}  // namespace teamspeak_cli::daemon
