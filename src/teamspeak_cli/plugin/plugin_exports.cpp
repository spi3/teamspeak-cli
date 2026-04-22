#include <cstdlib>
#include <cstring>
#include <memory>

#include "plugin_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"

#include "teamspeak_cli/bridge/socket_server.hpp"
#include "teamspeak_cli/build/version.hpp"
#include "teamspeak_cli/sdk/plugin_host_backend.hpp"

namespace {

constexpr int kPluginApiVersion = 26;

std::unique_ptr<teamspeak_cli::bridge::SocketBridgeServer>& bridge_server_instance() {
    static std::unique_ptr<teamspeak_cli::bridge::SocketBridgeServer> server;
    return server;
}

teamspeak_cli::sdk::PluginHostBackend*& backend_handle() {
    static teamspeak_cli::sdk::PluginHostBackend* backend = nullptr;
    return backend;
}

TS3Functions& pending_functions() {
    static TS3Functions funcs{};
    return funcs;
}

bool& pending_functions_set() {
    static bool ready = false;
    return ready;
}

char*& plugin_id_storage() {
    static char* plugin_id = nullptr;
    return plugin_id;
}

}  // namespace

extern "C" {

const char* ts3plugin_name() {
    return "ts3cli";
}

const char* ts3plugin_version() {
    return TSCLI_VERSION;
}

int ts3plugin_apiVersion() {
    return kPluginApiVersion;
}

const char* ts3plugin_author() {
    return "OpenAI";
}

const char* ts3plugin_description() {
    return "Expose TeamSpeak client interaction through a local CLI control socket.";
}

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    pending_functions() = funcs;
    pending_functions_set() = true;
    if (backend_handle() != nullptr) {
        backend_handle()->set_functions(funcs);
    }
}

int ts3plugin_init() {
    if (bridge_server_instance()) {
        return 0;
    }
    auto backend = std::make_unique<teamspeak_cli::sdk::PluginHostBackend>();
    backend_handle() = backend.get();
    if (pending_functions_set()) {
        backend_handle()->set_functions(pending_functions());
    }
    bridge_server_instance() =
        std::make_unique<teamspeak_cli::bridge::SocketBridgeServer>(std::move(backend));
    auto started = bridge_server_instance()->start(teamspeak_cli::sdk::InitOptions{});
    if (!started) {
        backend_handle() = nullptr;
        bridge_server_instance().reset();
        return 1;
    }
    return 0;
}

void ts3plugin_shutdown() {
    if (bridge_server_instance()) {
        const auto ignored = bridge_server_instance()->stop();
        (void)ignored;
        bridge_server_instance().reset();
        backend_handle() = nullptr;
    }
    if (plugin_id_storage() != nullptr) {
        std::free(plugin_id_storage());
        plugin_id_storage() = nullptr;
    }
}

void ts3plugin_registerPluginID(const char* id) {
    if (id == nullptr) {
        return;
    }
    if (plugin_id_storage() != nullptr) {
        std::free(plugin_id_storage());
    }
    const std::size_t size = std::strlen(id) + 1;
    plugin_id_storage() = static_cast<char*>(std::malloc(size));
    std::memcpy(plugin_id_storage(), id, size);
    if (backend_handle() != nullptr) {
        backend_handle()->set_plugin_id(id);
    }
}

void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    if (backend_handle() != nullptr) {
        backend_handle()->on_current_server_connection_changed(serverConnectionHandlerID);
    }
}

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    if (backend_handle() != nullptr) {
        backend_handle()->on_connect_status_change(serverConnectionHandlerID, newStatus, errorNumber);
    }
}

int ts3plugin_onTextMessageEvent(
    uint64 serverConnectionHandlerID,
    anyID targetMode,
    anyID toID,
    anyID fromID,
    const char* fromName,
    const char* fromUniqueIdentifier,
    const char* message,
    int ffIgnored
) {
    (void)ffIgnored;
    if (backend_handle() != nullptr) {
        backend_handle()->on_text_message(
            serverConnectionHandlerID,
            static_cast<unsigned int>(targetMode),
            static_cast<std::uint16_t>(toID),
            static_cast<std::uint16_t>(fromID),
            fromName,
            fromUniqueIdentifier,
            message
        );
    }
    return 0;
}

void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
    (void)isReceivedWhisper;
    if (backend_handle() != nullptr) {
        backend_handle()->on_talk_status_change(
            serverConnectionHandlerID, status, static_cast<std::uint16_t>(clientID)
        );
    }
}

void ts3plugin_onEditPlaybackVoiceDataEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    short* samples,
    int sampleCount,
    int channels
) {
    if (backend_handle() != nullptr) {
        backend_handle()->on_playback_voice_data(
            serverConnectionHandlerID,
            static_cast<std::uint16_t>(clientID),
            samples,
            sampleCount,
            channels
        );
    }
}

void ts3plugin_onEditCapturedVoiceDataEvent(
    uint64 serverConnectionHandlerID,
    short* samples,
    int sampleCount,
    int channels,
    int* edited
) {
    if (backend_handle() != nullptr) {
        backend_handle()->on_captured_voice_data(
            serverConnectionHandlerID,
            samples,
            sampleCount,
            channels,
            edited
        );
    }
}

void ts3plugin_onClientMoveEvent(
    uint64 serverConnectionHandlerID,
    anyID clientID,
    uint64 oldChannelID,
    uint64 newChannelID,
    int visibility,
    const char* moveMessage
) {
    (void)visibility;
    if (backend_handle() != nullptr) {
        backend_handle()->on_client_move(
            serverConnectionHandlerID,
            static_cast<std::uint16_t>(clientID),
            oldChannelID,
            newChannelID,
            moveMessage
        );
    }
}

int ts3plugin_onServerErrorEvent(
    uint64 serverConnectionHandlerID,
    const char* errorMessage,
    unsigned int error,
    const char* returnCode,
    const char* extraMessage
) {
    if (backend_handle() != nullptr) {
        backend_handle()->on_server_error(
            serverConnectionHandlerID, errorMessage, error, returnCode, extraMessage
        );
    }
    return 0;
}

int ts3plugin_offersConfigure() {
    return PLUGIN_OFFERS_NO_CONFIGURE;
}

int ts3plugin_requestAutoload() {
    return 1;
}

}  // extern "C"
