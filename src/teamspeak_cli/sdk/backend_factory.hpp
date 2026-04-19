#pragma once

#include <memory>
#include <string>

#include "teamspeak_cli/domain/result.hpp"
#include "teamspeak_cli/sdk/backend.hpp"

namespace teamspeak_cli::sdk {

class BackendFactory {
  public:
    [[nodiscard]] auto create(const std::string& backend_name) const
        -> domain::Result<std::unique_ptr<Backend>>;
};

}  // namespace teamspeak_cli::sdk
