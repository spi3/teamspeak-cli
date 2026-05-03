#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "teamspeak_cli/config/config_store.hpp"
#include "test_support.hpp"

namespace {

namespace config = teamspeak_cli::config;
namespace domain = teamspeak_cli::domain;
namespace fs = std::filesystem;
namespace tests = teamspeak_cli::tests;

class ScopedCurrentPath {
  public:
    explicit ScopedCurrentPath(const fs::path& next) : previous_(fs::current_path()) {
        fs::create_directories(next);
        fs::current_path(next);
    }

    ~ScopedCurrentPath() {
        fs::current_path(previous_);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    auto operator=(const ScopedCurrentPath&) -> ScopedCurrentPath& = delete;

  private:
    fs::path previous_;
};

void write_text(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
    tests::expect(static_cast<bool>(output), "fixture write should succeed: " + path.string());
}

void expect_load_error(
    const config::ConfigStore& store,
    const fs::path& path,
    const std::string& contents,
    const std::string& code,
    const std::string& message_part
) {
    write_text(path, contents);

    const auto loaded = store.load(path);
    tests::expect(!loaded.ok(), "config load should fail: " + code);
    tests::expect_eq(loaded.error().code, code, "config load error code");
    tests::expect_contains(loaded.error().message, message_part, "config load error message");
}

auto single_profile_config(
    std::string profile_name,
    std::string backend,
    std::string host,
    std::string port,
    std::string active_profile = "one"
) -> std::string {
    return "version=1\n"
           "active_profile=" +
           active_profile +
           "\n\n"
           "[profile." +
           profile_name +
           "]\n"
           "backend=" +
           backend +
           "\n"
           "host=" +
           host +
           "\n"
           "port=" +
           port +
           "\n";
}

}  // namespace

int main() {
    const fs::path root = tests::make_temp_path("tscli-config-store-test");
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path path = root / "config.ini";

    config::ConfigStore store;

    auto initialized = store.init(path, true);
    tests::expect(initialized.ok(), "config init should succeed");
    tests::expect(fs::exists(path), "config file should exist after init");

    auto loaded = store.load(path);
    tests::expect(loaded.ok(), "config load should succeed");
    tests::expect_eq(loaded.value().active_profile, std::string("plugin-local"), "default active profile");
    tests::expect_eq(loaded.value().profiles.size(), std::size_t(2), "default profile count");

    auto found = store.find_profile(loaded.value(), "plugin-local");
    tests::expect(found.ok(), "plugin-local profile should exist");
    tests::expect_eq(found.value()->backend, std::string("plugin"), "plugin backend name");
    tests::expect_eq(
        found.value()->control_socket_path,
        std::string(""),
        "default plugin-local socket path should be empty so runtime defaults stay dynamic"
    );

    domain::Profile copied_profile = *found.value();
    copied_profile.name = "work";
    copied_profile.host = "voice.example.com";
    auto created = store.create_profile(loaded.value(), std::move(copied_profile));
    tests::expect(created.ok(), "profile creation should succeed");
    tests::expect_eq(loaded.value().profiles.size(), std::size_t(3), "created profile count");
    tests::expect_eq(created.value()->backend, std::string("plugin"), "created profile backend");
    tests::expect_eq(
        created.value()->host, std::string("voice.example.com"), "created profile host"
    );

    domain::Profile duplicate_profile;
    duplicate_profile.name = "work";
    auto duplicate = store.create_profile(loaded.value(), std::move(duplicate_profile));
    tests::expect(!duplicate.ok(), "duplicate profile creation should fail");
    tests::expect_eq(
        duplicate.error().code, std::string("profile_exists"), "duplicate profile error code"
    );

    {
        const ScopedCurrentPath scoped_cwd(root / "parentless");
        auto saved = store.save(fs::path("config.ini"), store.default_config());
        tests::expect(saved.ok(), "parentless config save should succeed");
        tests::expect(fs::exists("config.ini"), "parentless config file should exist");
    }

#if !defined(_WIN32)
    const fs::path mode_path = root / "mode.ini";
    auto mode_saved = store.save(mode_path, store.default_config());
    tests::expect(mode_saved.ok(), "mode config save should succeed");
    struct stat status {};
    tests::expect(::stat(mode_path.c_str(), &status) == 0, "mode stat should succeed");
    const auto actual_mode = static_cast<unsigned int>(status.st_mode & static_cast<mode_t>(0777));
    tests::expect_eq(actual_mode, 0600U, "config file mode should be owner read/write only");
#endif

    expect_load_error(
        store,
        root / "invalid-active.ini",
        single_profile_config("one", "mock", "127.0.0.1", "9987", "missing"),
        "invalid_active_profile",
        "line"
    );

    expect_load_error(
        store,
        root / "duplicate-profile.ini",
        "version=1\n"
        "active_profile=one\n\n"
        "[profile.one]\n"
        "backend=mock\n"
        "host=127.0.0.1\n"
        "port=9987\n\n"
        "[profile.one]\n"
        "backend=plugin\n"
        "host=127.0.0.1\n"
        "port=9987\n",
        "duplicate_profile",
        "line"
    );

    expect_load_error(
        store,
        root / "invalid-backend.ini",
        single_profile_config("one", "remote", "127.0.0.1", "9987"),
        "invalid_backend",
        "line"
    );

    expect_load_error(
        store,
        root / "empty-host.ini",
        single_profile_config("one", "mock", "", "9987"),
        "invalid_host",
        "line"
    );

    expect_load_error(
        store,
        root / "port-zero.ini",
        single_profile_config("one", "mock", "127.0.0.1", "0"),
        "invalid_port",
        "line"
    );

    expect_load_error(
        store,
        root / "unsupported-version.ini",
        "version=2\n"
        "active_profile=one\n\n"
        "[profile.one]\n"
        "backend=mock\n"
        "host=127.0.0.1\n"
        "port=9987\n",
        "unsupported_version",
        "line"
    );

    domain::Profile round_trip_profile;
    round_trip_profile.name = "work";
    round_trip_profile.backend = "fake";
    round_trip_profile.host = "voice.example.com";
    round_trip_profile.port = 9988;
    round_trip_profile.nickname = "tester";
    round_trip_profile.identity = "identity-data";
    round_trip_profile.server_password = "server-secret";
    round_trip_profile.channel_password = "channel-secret";
    round_trip_profile.default_channel = "Operations";
    round_trip_profile.control_socket_path = "/tmp/ts.sock";

    domain::AppConfig round_trip_config;
    round_trip_config.version = 1;
    round_trip_config.active_profile = "work";
    round_trip_config.profiles = {round_trip_profile};

    const fs::path round_trip_path = root / "round-trip.ini";
    auto round_trip_saved = store.save(round_trip_path, round_trip_config);
    tests::expect(round_trip_saved.ok(), "round trip save should succeed");

    auto round_trip_loaded = store.load(round_trip_path);
    tests::expect(round_trip_loaded.ok(), "round trip load should succeed");
    tests::expect_eq(round_trip_loaded.value().active_profile, std::string("work"), "round trip active profile");
    tests::expect_eq(round_trip_loaded.value().profiles.size(), std::size_t(1), "round trip profile count");
    const auto& restored_profile = round_trip_loaded.value().profiles.front();
    tests::expect_eq(restored_profile.backend, std::string("mock"), "fake backend should load as mock");
    tests::expect_eq(restored_profile.host, std::string("voice.example.com"), "round trip host");
    tests::expect_eq(restored_profile.port, static_cast<std::uint16_t>(9988), "round trip port");
    tests::expect_eq(restored_profile.server_password, std::string("server-secret"), "round trip server password");
    tests::expect_eq(
        restored_profile.channel_password, std::string("channel-secret"), "round trip channel password"
    );
    tests::expect_eq(
        restored_profile.control_socket_path, std::string("/tmp/ts.sock"), "round trip socket path"
    );

    fs::remove_all(root);
    return 0;
}
