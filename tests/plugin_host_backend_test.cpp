#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak_cli/sdk/plugin_host_backend.hpp"
#include "test_support.hpp"

namespace {

struct HostState {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t current_handler_id = 7;
    int register_custom_device_calls = 0;
    int unregister_custom_device_calls = 0;
    int close_capture_device_calls = 0;
    int activate_capture_device_calls = 0;
    int flush_client_self_updates_calls = 0;
    int process_custom_capture_data_calls = 0;
    int start_voice_recording_calls = 0;
    int stop_voice_recording_calls = 0;
    int set_channel_variable_calls = 0;
    int flush_channel_updates_calls = 0;
    int request_server_group_add_client_calls = 0;
    unsigned int register_custom_device_result = ERROR_ok;
    unsigned int open_capture_result = ERROR_ok;
    unsigned int activate_capture_device_result = ERROR_ok;
    unsigned int set_client_self_variable_result = ERROR_ok;
    unsigned int flush_client_self_updates_result = ERROR_ok;
    unsigned int process_custom_capture_data_result = ERROR_ok;
    unsigned int set_channel_variable_result = ERROR_ok;
    unsigned int flush_channel_updates_result = ERROR_ok;
    unsigned int request_server_group_add_client_result = ERROR_ok;
    std::uint64_t last_channel_update_id = 0;
    std::size_t last_channel_update_flag = 0;
    std::string last_channel_update_value;
    std::uint64_t last_server_group_id = 0;
    std::uint64_t last_server_group_client_database_id = 0;
    std::string error_message = "fake TeamSpeak error";
    std::vector<std::pair<std::string, std::string>> open_capture_calls;
    std::vector<std::pair<std::size_t, int>> self_variable_updates;
    std::vector<short> last_capture_samples;
};

auto host_state() -> HostState& {
    static HostState state;
    return state;
}

void reset_host_state() {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.current_handler_id = 7;
    state.register_custom_device_calls = 0;
    state.unregister_custom_device_calls = 0;
    state.close_capture_device_calls = 0;
    state.activate_capture_device_calls = 0;
    state.flush_client_self_updates_calls = 0;
    state.process_custom_capture_data_calls = 0;
    state.start_voice_recording_calls = 0;
    state.stop_voice_recording_calls = 0;
    state.set_channel_variable_calls = 0;
    state.flush_channel_updates_calls = 0;
    state.request_server_group_add_client_calls = 0;
    state.register_custom_device_result = ERROR_ok;
    state.open_capture_result = ERROR_ok;
    state.activate_capture_device_result = ERROR_ok;
    state.set_client_self_variable_result = ERROR_ok;
    state.flush_client_self_updates_result = ERROR_ok;
    state.process_custom_capture_data_result = ERROR_ok;
    state.set_channel_variable_result = ERROR_ok;
    state.flush_channel_updates_result = ERROR_ok;
    state.request_server_group_add_client_result = ERROR_ok;
    state.last_channel_update_id = 0;
    state.last_channel_update_flag = 0;
    state.last_channel_update_value.clear();
    state.last_server_group_id = 0;
    state.last_server_group_client_database_id = 0;
    state.error_message = "fake TeamSpeak error";
    state.open_capture_calls.clear();
    state.self_variable_updates.clear();
    state.last_capture_samples.clear();
}

auto duplicate_string(const char* value) -> char* {
    const std::size_t size = std::strlen(value) + 1;
    auto* copy = static_cast<char*>(std::malloc(size));
    std::memcpy(copy, value, size);
    return copy;
}

unsigned int fake_free_memory(void* pointer) {
    std::free(pointer);
    return ERROR_ok;
}

unsigned int fake_get_error_message(unsigned int, char** error) {
    if (error == nullptr) {
        return ERROR_parameter_invalid;
    }
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    *error = duplicate_string(state.error_message.c_str());
    return ERROR_ok;
}

std::uint64_t fake_current_handler_id() {
    std::lock_guard<std::mutex> lock(host_state().mutex);
    return host_state().current_handler_id;
}

unsigned int fake_get_current_capture_mode(std::uint64_t, char** result) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = duplicate_string("pulse");
    return ERROR_ok;
}

unsigned int fake_get_current_capture_device_name(std::uint64_t, char** result, int* is_default) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = duplicate_string("alsa_input.usb-0");
    if (is_default != nullptr) {
        *is_default = 0;
    }
    return ERROR_ok;
}

unsigned int fake_get_current_playback_mode(std::uint64_t, char** result) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = duplicate_string("pulse");
    return ERROR_ok;
}

unsigned int fake_get_current_playback_device_name(std::uint64_t, char** result, int* is_default) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = duplicate_string("alsa_output.usb-0");
    if (is_default != nullptr) {
        *is_default = 0;
    }
    return ERROR_ok;
}

unsigned int fake_get_client_self_variable_as_int(std::uint64_t, std::size_t flag, int* result) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    if (flag == CLIENT_INPUT_DEACTIVATED) {
        *result = INPUT_DEACTIVATED;
        return ERROR_ok;
    }
    if (flag == CLIENT_INPUT_MUTED) {
        *result = MUTEINPUT_MUTED;
        return ERROR_ok;
    }
    return ERROR_parameter_invalid;
}

unsigned int fake_register_custom_device(
    const char* device_id,
    const char*,
    int capture_frequency,
    int capture_channels,
    int,
    int
) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.register_custom_device_calls;
    teamspeak_cli::tests::expect_eq(
        std::string(device_id == nullptr ? "" : device_id),
        std::string("ts3cli_media_capture"),
        "custom capture device id should match the plugin media bridge device"
    );
    teamspeak_cli::tests::expect_eq(
        capture_frequency,
        teamspeak_cli::bridge::kMediaSampleRate,
        "custom capture device frequency should match the media bridge"
    );
    teamspeak_cli::tests::expect_eq(
        capture_channels,
        teamspeak_cli::bridge::kMediaPlaybackChannels,
        "custom capture device channel count should match the media bridge"
    );
    return state.register_custom_device_result;
}

unsigned int fake_unregister_custom_device(const char*) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.unregister_custom_device_calls;
    return ERROR_ok;
}

unsigned int fake_close_capture_device(std::uint64_t) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.close_capture_device_calls;
    return ERROR_ok;
}

unsigned int fake_open_capture_device(std::uint64_t, const char* mode, const char* device) {
    auto& state = host_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.open_capture_calls.emplace_back(
            mode == nullptr ? std::string{} : std::string(mode),
            device == nullptr ? std::string{} : std::string(device)
        );
    }
    state.cv.notify_all();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.open_capture_result;
}

unsigned int fake_activate_capture_device(std::uint64_t) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.activate_capture_device_calls;
    return state.activate_capture_device_result;
}

unsigned int fake_set_client_self_variable_as_int(std::uint64_t, std::size_t flag, int value) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.self_variable_updates.emplace_back(flag, value);
    return state.set_client_self_variable_result;
}

unsigned int fake_flush_client_self_updates(std::uint64_t, const char*) {
    auto& state = host_state();
    unsigned int result = ERROR_ok;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.flush_client_self_updates_calls;
        result = state.flush_client_self_updates_result;
    }
    state.cv.notify_all();
    return result;
}

unsigned int fake_process_custom_capture_data(const char* device_name, const short* buffer, int samples) {
    auto& state = host_state();
    unsigned int result = ERROR_ok;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.process_custom_capture_data_calls;
        state.last_capture_samples.assign(buffer, buffer + samples);
        teamspeak_cli::tests::expect_eq(
            std::string(device_name == nullptr ? "" : device_name),
            std::string("ts3cli_media_capture"),
            "custom capture audio should be submitted to the registered media bridge device"
        );
        result = state.process_custom_capture_data_result;
    }
    state.cv.notify_all();
    return result;
}

unsigned int fake_start_voice_recording(std::uint64_t) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.start_voice_recording_calls;
    return ERROR_ok;
}

unsigned int fake_stop_voice_recording(std::uint64_t) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.stop_voice_recording_calls;
    return ERROR_ok;
}

unsigned int fake_get_channel_list(std::uint64_t, std::uint64_t** result) {
    if (result == nullptr) {
        return ERROR_parameter_invalid;
    }
    auto* channels = static_cast<std::uint64_t*>(std::malloc(sizeof(std::uint64_t) * 2));
    channels[0] = 2;
    channels[1] = 0;
    *result = channels;
    return ERROR_ok;
}

unsigned int fake_get_channel_variable_as_string(
    std::uint64_t,
    std::uint64_t channel_id,
    std::size_t flag,
    char** result
) {
    if (channel_id != 2 || flag != CHANNEL_NAME || result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = duplicate_string("Engineering");
    return ERROR_ok;
}

unsigned int fake_get_channel_variable_as_int(
    std::uint64_t,
    std::uint64_t channel_id,
    std::size_t flag,
    int* result
) {
    if (channel_id != 2 || flag != CHANNEL_FLAG_DEFAULT || result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = 0;
    return ERROR_ok;
}

unsigned int fake_get_channel_client_list(std::uint64_t, std::uint64_t channel_id, anyID** result) {
    if (channel_id != 2 || result == nullptr) {
        return ERROR_parameter_invalid;
    }
    auto* clients = static_cast<anyID*>(std::malloc(sizeof(anyID)));
    clients[0] = 0;
    *result = clients;
    return ERROR_ok;
}

unsigned int fake_get_parent_channel(std::uint64_t, std::uint64_t channel_id, std::uint64_t* result) {
    if (channel_id != 2 || result == nullptr) {
        return ERROR_parameter_invalid;
    }
    *result = 0;
    return ERROR_ok;
}

unsigned int fake_set_channel_variable_as_string(
    std::uint64_t,
    std::uint64_t channel_id,
    std::size_t flag,
    const char* value
) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.set_channel_variable_calls;
    state.last_channel_update_id = channel_id;
    state.last_channel_update_flag = flag;
    state.last_channel_update_value = value == nullptr ? "" : value;
    return state.set_channel_variable_result;
}

unsigned int fake_flush_channel_updates(std::uint64_t, std::uint64_t channel_id, const char*) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.flush_channel_updates_calls;
    state.last_channel_update_id = channel_id;
    return state.flush_channel_updates_result;
}

unsigned int fake_get_server_group_name_by_id(
    std::uint64_t,
    unsigned int group_id,
    char* result,
    std::size_t max_len
) {
    if (group_id != 7 || result == nullptr || max_len == 0) {
        return ERROR_parameter_invalid;
    }
    std::strncpy(result, "Operator", max_len);
    result[max_len - 1] = '\0';
    return ERROR_ok;
}

unsigned int fake_request_server_group_add_client(
    std::uint64_t,
    std::uint64_t server_group_id,
    std::uint64_t client_database_id,
    const char*
) {
    auto& state = host_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.request_server_group_add_client_calls;
    state.last_server_group_id = server_group_id;
    state.last_server_group_client_database_id = client_database_id;
    return state.request_server_group_add_client_result;
}

class TestMediaBridge final : public teamspeak_cli::bridge::MediaBridge {
  public:
    void arm(std::vector<short> samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_samples_ = std::move(samples);
        active_ = true;
        filled_once_ = false;
    }

    [[nodiscard]] auto socket_path() const -> std::string override {
        return {};
    }

    [[nodiscard]] auto status() const -> teamspeak_cli::bridge::MediaStatus override {
        return {};
    }

    void publish_speaker_start(const teamspeak_cli::bridge::MediaSpeaker&) override {}
    void publish_speaker_stop(const teamspeak_cli::bridge::MediaSpeaker&) override {}
    void publish_audio_chunk(const teamspeak_cli::bridge::MediaSpeaker&, int, int, const short*, int) override {}

    [[nodiscard]] auto fill_playback_samples(int, int, short* samples, int sample_count) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return false;
        }
        if (!filled_once_) {
            filled_once_ = true;
            for (int index = 0; index < sample_count; ++index) {
                const auto sample_index = static_cast<std::size_t>(index);
                samples[index] = pending_samples_.size() > sample_index ? pending_samples_[sample_index] : 0;
            }
            return true;
        }
        active_ = false;
        return false;
    }

  private:
    mutable std::mutex mutex_;
    bool active_ = false;
    bool filled_once_ = false;
    std::vector<short> pending_samples_;
};

}  // namespace

int main() {
    using namespace teamspeak_cli;

    reset_host_state();

    sdk::PluginHostBackend backend;
    TS3Functions functions{};
    functions.freeMemory = fake_free_memory;
    functions.getErrorMessage = fake_get_error_message;
    functions.getCurrentServerConnectionHandlerID = fake_current_handler_id;
    functions.getCurrentCaptureMode = fake_get_current_capture_mode;
    functions.getCurrentCaptureDeviceName = fake_get_current_capture_device_name;
    functions.getCurrentPlayBackMode = fake_get_current_playback_mode;
    functions.getCurrentPlaybackDeviceName = fake_get_current_playback_device_name;
    functions.getClientSelfVariableAsInt = fake_get_client_self_variable_as_int;
    functions.registerCustomDevice = fake_register_custom_device;
    functions.unregisterCustomDevice = fake_unregister_custom_device;
    functions.closeCaptureDevice = fake_close_capture_device;
    functions.openCaptureDevice = fake_open_capture_device;
    functions.activateCaptureDevice = fake_activate_capture_device;
    functions.setClientSelfVariableAsInt = fake_set_client_self_variable_as_int;
    functions.flushClientSelfUpdates = fake_flush_client_self_updates;
    functions.processCustomCaptureData = fake_process_custom_capture_data;
    functions.startVoiceRecording = fake_start_voice_recording;
    functions.stopVoiceRecording = fake_stop_voice_recording;
    functions.getChannelList = fake_get_channel_list;
    functions.getChannelVariableAsString = fake_get_channel_variable_as_string;
    functions.getChannelVariableAsInt = fake_get_channel_variable_as_int;
    functions.getChannelClientList = fake_get_channel_client_list;
    functions.getParentChannelOfChannel = fake_get_parent_channel;
    functions.setChannelVariableAsString = fake_set_channel_variable_as_string;
    functions.flushChannelUpdates = fake_flush_channel_updates;
    functions.getServerGroupNameByID = fake_get_server_group_name_by_id;
    functions.requestServerGroupAddClient = fake_request_server_group_add_client;

    backend.set_functions(functions);
    auto initialized = backend.initialize(sdk::InitOptions{});
    tests::expect(initialized.ok(), "plugin host backend should initialize with fake TeamSpeak functions");

    auto initial_info = backend.plugin_info();
    tests::expect(initial_info.ok(), "plugin info should expose media diagnostics");
    tests::expect_eq(
        initial_info.value().media_diagnostics.capture.mode,
        std::string("pulse"),
        "media diagnostics should report the effective TeamSpeak capture mode"
    );
    tests::expect_eq(
        initial_info.value().media_diagnostics.capture.device,
        std::string("alsa_input.usb-0"),
        "media diagnostics should report the effective TeamSpeak capture device"
    );
    tests::expect_eq(
        initial_info.value().media_diagnostics.playback.device,
        std::string("alsa_output.usb-0"),
        "media diagnostics should report the effective TeamSpeak playback device"
    );
    tests::expect(
        initial_info.value().media_diagnostics.custom_capture_path_available,
        "media diagnostics should report when the custom capture transmit path is available"
    );
    tests::expect(
        initial_info.value().media_diagnostics.transmit_path_ready,
        "media diagnostics should report the transmit path as ready when a handler is available"
    );
    tests::expect(
        !initial_info.value().media_diagnostics.captured_voice_edit_attached,
        "media diagnostics should make clear that playback is not attached through captured-voice editing"
    );

    auto renamed_channel = backend.rename_channel(domain::Selector{"2"}, "Platform");
    tests::expect(renamed_channel.ok(), "plugin backend should rename a channel through TeamSpeak channel updates");
    tests::expect_eq(
        renamed_channel.value().name,
        std::string("Platform"),
        "plugin backend should report the requested channel name after rename"
    );

    domain::ServerGroupApplicationRequest group_database_request{};
    group_database_request.group = "7";
    group_database_request.client_database_id = domain::ClientDatabaseId{1002};
    auto applied_group = backend.apply_server_group(group_database_request);
    tests::expect(
        applied_group.ok(),
        "plugin backend should apply a server group using a direct client database id"
    );
    tests::expect_eq(
        applied_group.value().server_group.name,
        std::string("Operator"),
        "plugin backend should report the server group name when available"
    );
    tests::expect_eq(
        applied_group.value().client_database_id.value,
        std::uint64_t(1002),
        "plugin backend should preserve the target client database id"
    );

    auto media_bridge = std::make_shared<TestMediaBridge>();
    media_bridge->arm({111, 222, 333, 444});
    backend.set_media_bridge(media_bridge);

    int edited = 0;
    std::vector<short> callback_samples(16, 0);
    backend.on_captured_voice_data(7, callback_samples.data(), static_cast<int>(callback_samples.size()), 1, &edited);
    tests::expect_eq(edited, 0, "custom capture playback should no longer use the captured-voice edit callback");

    auto activated = backend.activate_media_playback();
    tests::expect(activated.ok(), "custom capture playback activation should succeed");

    {
        auto& state = host_state();
        std::unique_lock<std::mutex> lock(state.mutex);
        const bool ready = state.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return state.process_custom_capture_data_calls > 0 &&
                   state.open_capture_calls.size() >= std::size_t(2) &&
                   state.flush_client_self_updates_calls >= 2;
        });
        tests::expect(ready, "custom capture playback should feed audio and restore the capture device");
    }

    auto deactivated = backend.deactivate_media_playback();
    tests::expect(deactivated.ok(), "deactivating drained custom capture playback should still succeed");

    auto shutdown = backend.shutdown();
    tests::expect(shutdown.ok(), "plugin host backend shutdown should succeed after custom capture playback");

    {
        auto& state = host_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        tests::expect_eq(state.register_custom_device_calls, 1, "custom capture device should be registered once");
        tests::expect_eq(state.unregister_custom_device_calls, 1, "custom capture device should be unregistered on shutdown");
        tests::expect_eq(
            state.open_capture_calls.front().first,
            std::string("custom"),
            "activation should switch TeamSpeak to the custom capture mode"
        );
        tests::expect_eq(
            state.open_capture_calls.front().second,
            std::string("ts3cli_media_capture"),
            "activation should open the plugin custom capture device"
        );
        tests::expect_eq(
            state.open_capture_calls.back().first,
            std::string("pulse"),
            "deactivation should restore the previous TeamSpeak capture mode"
        );
        tests::expect_eq(
            state.open_capture_calls.back().second,
            std::string("alsa_input.usb-0"),
            "deactivation should restore the previous TeamSpeak capture device"
        );
        tests::expect_eq(
            state.activate_capture_device_calls,
            2,
            "custom capture playback should activate both the injected and restored capture devices"
        );
        tests::expect_eq(state.start_voice_recording_calls, 0, "custom capture playback should not start TeamSpeak recording");
        tests::expect_eq(state.stop_voice_recording_calls, 0, "custom capture playback should not stop TeamSpeak recording");
        tests::expect_eq(
            state.set_channel_variable_calls,
            1,
            "channel rename should set one TeamSpeak channel variable"
        );
        tests::expect_eq(
            state.flush_channel_updates_calls,
            1,
            "channel rename should flush one TeamSpeak channel update"
        );
        tests::expect_eq(
            state.last_channel_update_id,
            std::uint64_t(2),
            "channel rename should target the selected channel id"
        );
        tests::expect_eq(
            state.last_channel_update_flag,
            static_cast<std::size_t>(CHANNEL_NAME),
            "channel rename should update the channel name flag"
        );
        tests::expect_eq(
            state.last_channel_update_value,
            std::string("Platform"),
            "channel rename should pass the requested new name"
        );
        tests::expect_eq(
            state.request_server_group_add_client_calls,
            1,
            "server group application should request one server group membership update"
        );
        tests::expect_eq(
            state.last_server_group_id,
            std::uint64_t(7),
            "server group application should target the selected group id"
        );
        tests::expect_eq(
            state.last_server_group_client_database_id,
            std::uint64_t(1002),
            "server group application should target the requested client database id"
        );
        tests::expect(!state.last_capture_samples.empty(), "custom capture playback should submit PCM samples to TeamSpeak");
        tests::expect_eq(state.last_capture_samples[0], static_cast<short>(111), "submitted PCM should match the media bridge");
        tests::expect_eq(state.last_capture_samples[1], static_cast<short>(222), "submitted PCM should preserve ordering");
        tests::expect_eq(
            state.self_variable_updates.front(),
            std::make_pair<std::size_t, int>(CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE),
            "activation should force TeamSpeak input active"
        );
        tests::expect_eq(
            state.self_variable_updates[1],
            std::make_pair<std::size_t, int>(CLIENT_INPUT_MUTED, MUTEINPUT_NONE),
            "activation should unmute TeamSpeak input"
        );
        tests::expect_eq(
            state.self_variable_updates[state.self_variable_updates.size() - 2],
            std::make_pair<std::size_t, int>(CLIENT_INPUT_DEACTIVATED, INPUT_DEACTIVATED),
            "deactivation should restore the previous input deactivation state"
        );
        tests::expect_eq(
            state.self_variable_updates.back(),
            std::make_pair<std::size_t, int>(CLIENT_INPUT_MUTED, MUTEINPUT_MUTED),
            "deactivation should restore the previous input mute state"
        );
    }

    reset_host_state();
    {
        auto& error_state = host_state();
        std::lock_guard<std::mutex> lock(error_state.mutex);
        error_state.error_message = "ok";
        error_state.activate_capture_device_result = ERROR_undefined;
    }

    sdk::PluginHostBackend translation_backend;
    translation_backend.set_functions(functions);
    auto translation_initialized = translation_backend.initialize(sdk::InitOptions{});
    tests::expect(translation_initialized.ok(), "translation test backend should initialize");

    auto failed_activation = translation_backend.activate_media_playback();
    tests::expect(!failed_activation.ok(), "generic TeamSpeak success text should still surface a meaningful playback error");
    tests::expect_eq(
        failed_activation.error().code,
        std::string("ts3_error"),
        "failed playback activation should preserve the TeamSpeak error code category"
    );
    tests::expect_contains(
        failed_activation.error().message,
        "failed to activate TeamSpeak custom capture device",
        "playback activation failure should fall back to the contextual operation message when TeamSpeak reports ok"
    );
    tests::expect(
        failed_activation.error().message != "ok",
        "playback activation failure should not surface a bare ok error message"
    );

    auto translation_shutdown = translation_backend.shutdown();
    tests::expect(translation_shutdown.ok(), "translation test backend should shut down cleanly");

    reset_host_state();
    {
        auto& no_update_state = host_state();
        std::lock_guard<std::mutex> lock(no_update_state.mutex);
        no_update_state.activate_capture_device_result = ERROR_ok_no_update;
        no_update_state.set_client_self_variable_result = ERROR_ok_no_update;
        no_update_state.flush_client_self_updates_result = ERROR_ok_no_update;
    }

    sdk::PluginHostBackend no_update_backend;
    no_update_backend.set_functions(functions);
    auto no_update_initialized = no_update_backend.initialize(sdk::InitOptions{});
    tests::expect(no_update_initialized.ok(), "no-update playback backend should initialize");

    auto no_update_media_bridge = std::make_shared<TestMediaBridge>();
    no_update_media_bridge->arm({555, 666, 777, 888});
    no_update_backend.set_media_bridge(no_update_media_bridge);

    auto no_update_activation = no_update_backend.activate_media_playback();
    tests::expect(
        no_update_activation.ok(),
        "TeamSpeak success-without-change responses should not block playback activation"
    );

    {
        auto& no_update_state = host_state();
        std::unique_lock<std::mutex> lock(no_update_state.mutex);
        const bool ready = no_update_state.cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return no_update_state.process_custom_capture_data_calls > 0 &&
                   no_update_state.open_capture_calls.size() >= std::size_t(2) &&
                   no_update_state.flush_client_self_updates_calls >= 2;
        });
        tests::expect(ready, "no-update playback activation should still drive custom capture audio");
    }

    auto no_update_deactivated = no_update_backend.deactivate_media_playback();
    tests::expect(no_update_deactivated.ok(), "no-update playback backend should deactivate cleanly");

    auto no_update_shutdown = no_update_backend.shutdown();
    tests::expect(no_update_shutdown.ok(), "no-update playback backend should shut down cleanly");

    {
        auto& no_update_state = host_state();
        std::lock_guard<std::mutex> lock(no_update_state.mutex);
        tests::expect_eq(
            no_update_state.activate_capture_device_calls,
            2,
            "no-update playback activation should still activate both the injected and restored capture devices"
        );
        tests::expect(
            !no_update_state.last_capture_samples.empty(),
            "no-update playback activation should still submit audio to TeamSpeak"
        );
    }

    return 0;
}
