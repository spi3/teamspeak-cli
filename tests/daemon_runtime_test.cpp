#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "teamspeak_cli/daemon/runtime.hpp"
#include "test_support.hpp"

namespace fs = std::filesystem;

namespace {

class EnvGuard {
  public:
    EnvGuard(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* current = std::getenv(name_.c_str())) {
            previous_ = current;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~EnvGuard() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

}  // namespace

int main() {
    using namespace teamspeak_cli;

    const fs::path root = tests::make_temp_path("ts-daemon-runtime-test");
    fs::create_directories(root);
    EnvGuard daemon_state_env("TS_DAEMON_STATE_DIR", root.string());

    auto resolved_state_dir = daemon::resolve_state_dir();
    tests::expect(resolved_state_dir.ok(), "daemon state dir should resolve from TS_DAEMON_STATE_DIR");
    tests::expect_eq(resolved_state_dir.value(), root, "daemon state dir should match env override");

    const auto paths = daemon::state_paths_for(root);
    const fs::path hook_log = root / "hook.log";

    auto saved_hooks = daemon::save_hooks(
        paths,
        {daemon::Hook{
            .id = "1",
            .event_type = "message.received",
            .message_kind = "channel",
            .command =
                "printf '%s|%s\\n' \"$TS_EVENT_TYPE\" \"${TS_MESSAGE_KIND:-}\" >> '" + hook_log.string() + "'",
        }}
    );
    tests::expect(saved_hooks.ok(), "saving hooks should succeed");

    auto loaded_hooks = daemon::load_hooks(paths);
    tests::expect(loaded_hooks.ok(), "loading hooks should succeed");
    tests::expect_eq(loaded_hooks.value().size(), std::size_t(1), "one hook should be present");
    tests::expect_eq(
        loaded_hooks.value().front().message_kind, std::string("channel"), "hook kind should round-trip"
    );

    const domain::Profile profile{
        .name = "mock-local",
        .backend = "mock",
        .host = "127.0.0.1",
        .port = 9987,
        .nickname = "terminal",
        .identity = "mock-local-identity",
        .server_password = "",
        .channel_password = "",
        .default_channel = "Lobby",
        .control_socket_path = "",
    };

    std::atomic<bool> stop_requested = false;
    std::thread worker([&] {
        auto run = daemon::run_event_daemon(
            profile,
            sdk::InitOptions{},
            paths,
            std::chrono::milliseconds(100),
            [&] { return stop_requested.load(); }
        );
        tests::expect(run.ok(), "daemon runtime should exit cleanly");
    });

    auto wait_for_message = [&]() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            auto inbox = daemon::read_inbox(paths, 10);
            if (inbox.ok() && !inbox.value().empty()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };

    tests::expect(wait_for_message(), "daemon runtime should journal at least one message");

    const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fs::exists(hook_log) && std::chrono::steady_clock::now() < hook_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    stop_requested = true;
    worker.join();

    auto inbox = daemon::read_inbox(paths, 10);
    tests::expect(inbox.ok(), "reading daemon inbox should succeed");
    tests::expect(!inbox.value().empty(), "daemon inbox should contain at least one message");
    tests::expect_eq(inbox.value().front().type, std::string("message.received"), "inbox should capture message events");
    tests::expect_eq(
        inbox.value().front().fields.at("message_kind"),
        std::string("channel"),
        "mock message should be normalized as a channel message"
    );
    tests::expect_contains(
        inbox.value().front().fields.at("from_name"),
        "alice",
        "mock message should normalize the sender name"
    );

    std::ifstream hook_input(hook_log);
    std::string hook_output;
    std::getline(hook_input, hook_output);
    tests::expect_contains(
        hook_output, "message.received|channel", "hook command should receive event metadata"
    );

    auto status = daemon::read_status(paths);
    tests::expect(status.ok(), "reading daemon status should succeed");
    tests::expect(!status.value().running, "daemon status should be stopped after shutdown");
    tests::expect_eq(status.value().hook_count, std::size_t(1), "daemon status should report hook count");
    tests::expect(!status.value().started_at.empty(), "daemon status should record start time");

    return 0;
}
