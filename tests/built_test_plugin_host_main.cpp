#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include "teamspeak_cli/bridge/socket_server.hpp"
#include "teamspeak_cli/sdk/mock_backend.hpp"

namespace {

std::atomic_bool& stop_requested() {
    static std::atomic_bool stop{false};
    return stop;
}

void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested().store(true);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    teamspeak_cli::bridge::SocketBridgeServer server(std::make_unique<teamspeak_cli::sdk::MockBackend>());

    teamspeak_cli::sdk::InitOptions options;
    if (argc > 1) {
        options.socket_path = argv[1];
    }

    const auto started = server.start(options);
    if (!started) {
        return static_cast<int>(started.error().exit_code);
    }

    while (!stop_requested().load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto stopped = server.stop();
    if (!stopped) {
        return static_cast<int>(stopped.error().exit_code);
    }

    return 0;
}
