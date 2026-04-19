#include <filesystem>
#include <fstream>

#include "teamspeak_cli/config/config_store.hpp"
#include "test_support.hpp"

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "tscli-config-store-test";
    fs::remove_all(root);
    const fs::path path = root / "config.ini";

    teamspeak_cli::config::ConfigStore store;

    auto initialized = store.init(path, true);
    teamspeak_cli::tests::expect(initialized.ok(), "config init should succeed");
    teamspeak_cli::tests::expect(fs::exists(path), "config file should exist after init");

    auto loaded = store.load(path);
    teamspeak_cli::tests::expect(loaded.ok(), "config load should succeed");
    teamspeak_cli::tests::expect_eq(loaded.value().active_profile, std::string("plugin-local"), "default active profile");
    teamspeak_cli::tests::expect_eq(loaded.value().profiles.size(), std::size_t(2), "default profile count");

    auto found = store.find_profile(loaded.value(), "plugin-local");
    teamspeak_cli::tests::expect(found.ok(), "plugin-local profile should exist");
    teamspeak_cli::tests::expect_eq(found.value()->backend, std::string("plugin"), "plugin backend name");
    teamspeak_cli::tests::expect_eq(
        found.value()->control_socket_path,
        std::string(""),
        "default plugin-local socket path should be empty so runtime defaults stay dynamic"
    );

    {
        std::ofstream legacy_config(path, std::ios::trunc);
        legacy_config << "version=1\n";
        legacy_config << "active_profile=built-test\n\n";
        legacy_config << "[profile.built-test]\n";
        legacy_config << "backend=mock\n";
        legacy_config << "host=127.0.0.1\n";
        legacy_config << "port=9987\n";
        legacy_config << "nickname=terminal\n";
        legacy_config << "identity=built-test-identity\n";
        legacy_config << "server_password=\n";
        legacy_config << "channel_password=\n";
        legacy_config << "default_channel=Lobby\n";
        legacy_config << "control_socket_path=\n";
    }

    auto migrated = store.load(path);
    teamspeak_cli::tests::expect(migrated.ok(), "legacy config should load");
    teamspeak_cli::tests::expect_eq(
        migrated.value().active_profile, std::string("mock-local"), "legacy active profile should migrate"
    );
    auto legacy_alias = store.find_profile(migrated.value(), "built-test");
    teamspeak_cli::tests::expect(legacy_alias.ok(), "legacy built-test alias should resolve");
    teamspeak_cli::tests::expect_eq(
        legacy_alias.value()->name, std::string("mock-local"), "legacy alias should resolve to mock-local"
    );
    teamspeak_cli::tests::expect_eq(
        legacy_alias.value()->identity,
        std::string("mock-local-identity"),
        "legacy mock identity should migrate"
    );

    fs::remove_all(root);
    return 0;
}
