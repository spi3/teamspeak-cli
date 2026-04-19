#pragma once

#include <string>

#include "teamspeak_cli/domain/result.hpp"

namespace teamspeak_cli::cli::completion {

auto generate(const std::string& shell) -> domain::Result<std::string>;

}  // namespace teamspeak_cli::cli::completion
