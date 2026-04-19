#include "teamspeak_cli/sdk/backend_factory.hpp"

#include "teamspeak_cli/sdk/mock_backend.hpp"
#include "teamspeak_cli/sdk/socket_backend.hpp"
#include "teamspeak_cli/util/strings.hpp"

namespace teamspeak_cli::sdk {

auto BackendFactory::create(const std::string& backend_name) const
    -> domain::Result<std::unique_ptr<Backend>> {
    if (util::iequals(backend_name, "mock") || util::iequals(backend_name, "fake")) {
        return domain::ok<std::unique_ptr<Backend>>(std::make_unique<MockBackend>());
    }
    if (util::iequals(backend_name, "plugin")) {
        return domain::ok<std::unique_ptr<Backend>>(std::make_unique<SocketBackend>());
    }

    return domain::fail<std::unique_ptr<Backend>>(domain::make_error(
        "sdk",
        "unknown_backend",
        "unknown backend: " + backend_name,
        domain::ExitCode::usage
    ));
}

}  // namespace teamspeak_cli::sdk
