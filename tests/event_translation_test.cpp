#include "teamspeak_cli/sdk/fake_backend.hpp"
#include "test_support.hpp"

int main() {
    using namespace teamspeak_cli;

    sdk::FakeBackend backend;
    tests::expect(backend.initialize(sdk::InitOptions{}).ok(), "init");
    tests::expect(
        backend.connect(sdk::ConnectRequest{
            .host = "127.0.0.1",
            .port = 9987,
            .nickname = "tester",
            .identity = "identity",
            .server_password = "",
            .channel_password = "",
            .default_channel = "",
            .profile_name = "built-test",
        })
            .ok(),
        "connect"
    );

    auto first = backend.next_event(std::chrono::milliseconds(50));
    tests::expect(first.ok(), "first event fetch");
    tests::expect(first.value().has_value(), "first event present");
    tests::expect_eq(first.value()->type, std::string("connection.connected"), "connect event type");

    auto generated = backend.next_event(std::chrono::milliseconds(1000));
    tests::expect(generated.ok(), "generated event fetch");
    tests::expect(generated.value().has_value(), "generated event present");

    tests::expect(backend.disconnect("done").ok(), "disconnect");
    tests::expect(backend.shutdown().ok(), "shutdown");
    return 0;
}
