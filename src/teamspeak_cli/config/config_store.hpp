#pragma once

#include <filesystem>
#include <string>

#include "teamspeak_cli/domain/models.hpp"
#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::config {

class ConfigStore {
  public:
    auto default_path() const -> std::filesystem::path;
    auto load(const std::filesystem::path& path) const -> domain::Result<domain::AppConfig>;
    auto load_or_default(const std::filesystem::path& path) const -> domain::Result<domain::AppConfig>;
    auto save(const std::filesystem::path& path, const domain::AppConfig& config) const -> domain::Result<void>;
    auto init(const std::filesystem::path& path, bool force) const -> domain::Result<domain::AppConfig>;
    auto find_profile(domain::AppConfig& config, const std::string& name) const -> domain::Result<domain::Profile*>;
    auto find_profile(const domain::AppConfig& config, const std::string& name) const
        -> domain::Result<const domain::Profile*>;
    [[nodiscard]] auto default_config() const -> domain::AppConfig;
};

}  // namespace teamspeak_cli::config
