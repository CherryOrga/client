#include "include.h"
#include "util/io.h"
#include "client/client.h"

tcp::client* g_client = nullptr;
std::atomic<bool> should_exit{ false };

enum class AppState {
    connecting,
    connected,
    waiting_input,
    logging_in,
    logged_in,
    error_state,
    shutdown
};

int WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);

    std::atomic<AppState> state{ AppState::connecting };
    tcp::client client;
    g_client = &client;

    client.connect_event.add([]() {
        io::log("connected to server");
        });

    client.receive_event.add([&](tcp::packet_t packet) {
        if (!packet) return;

        switch (packet.id) {
        case tcp::packet_id::session:
            // 2. session initiated
            client.session_id = packet.session_id;
            // 3. send hwid
            if (client.write(tcp::packet_t(
                nlohmann::json{
                    {"hwid", nlohmann::json{{"uid", 0}}.dump()},
                    {"ver", client.ver}
                }.dump(),
                tcp::packet_type::write,
                client.session_id,
                tcp::packet_id::hwid
            )) <= 0) {
                io::log_error("failed to send HWID");
                state = AppState::error_state;
            }
            break;

        case tcp::packet_id::hwid_resp:
            client.hwid_result = nlohmann::json::parse(packet())["status"];
            if (client.hwid_result == tcp::hwid_result::ok) {
                state = AppState::connected;
            }
            break;

        case tcp::packet_id::login_resp: {
            auto j = nlohmann::json::parse(packet());
            client.login_result = j["result"];

            if (client.login_result == tcp::login_result::login_success) {
                io::log("Login successful!");
                state = AppState::logged_in;

                if (j.contains("games")) {
                    io::log("available games:");
                    client.games.clear();

                    for (const auto& [name, data] : j["games"].items()) {
                        client.games.emplace_back(game_data_t{
                            .x64 = data["x64"],
                            .id = data["id"],
                            .version = data["version"],
                            .name = name,
                            .process_name = data["process"]
                            });
                        io::log_indented<4>("", name.c_str());
                    }
                }
            }
            else if (client.login_result == tcp::login_result::server_error) {
                io::log_error("internal server error");
                state = AppState::error_state;
            }
            break;
        }
        case tcp::packet_id::ban:
            client.shutdown();
            break;
        }
        });

    // Start client connection
    client.start("51.210.103.134", 3000);
    if (!client) {
        io::log_error("failed to connect to the server");
        return 1;
    }

    // Start monitoring thread
    std::jthread monitor_thread{ [&client]() {
        tcp::client::monitor(client);
    } };

    // Main application loop
    while (!should_exit) {
        switch (state.load()) {
        case AppState::connected:
            io::log("ready for authentication");
            client.write(tcp::packet_t(
                "user,pass",
                tcp::packet_type::write,
                client.session_id,
                tcp::packet_id::login_req
            ));
            state = AppState::waiting_input;
            break;

        case AppState::logged_in:
            std::cin.get();
            should_exit = true;
            break;

        case AppState::error_state:
            io::log_error("error...");
            std::cin.get();
            should_exit = true;
            break;

        case AppState::shutdown:
            should_exit = true;
            break;

        default:
            break;
        }
    }

    client.shutdown();
    return 0;
}