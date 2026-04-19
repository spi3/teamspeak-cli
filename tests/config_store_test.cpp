#include <filesystem>

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

    fs::remove_all(root);
    return 0;
}
