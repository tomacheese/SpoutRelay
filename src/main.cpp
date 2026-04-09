#include <windows.h>
#include <csignal>
#include <iostream>
#include <string>
#include <atomic>
#include "app/supervisor.hpp"
#include "config/config_loader.hpp"

static std::atomic<bool> g_stop_requested{false};

static Supervisor* g_supervisor = nullptr;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        g_stop_requested.store(true);
        if (g_supervisor) g_supervisor->request_stop();
        return TRUE;
    }
    return FALSE;
}

static void usage(const char* exe) {
    std::cerr << "Usage: " << exe << " --config <path>\n";
}

int main(int argc, char* argv[]) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        }
    }

    if (config_path.empty()) {
        // Default config location
        config_path = "config/config.json";
    }

    AppConfig config;
    std::string load_error;
    if (!ConfigLoader::load(config_path, config, load_error)) {
        std::cerr << "[ERROR] " << load_error << "\n";
        return 1;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    Supervisor supervisor;
    g_supervisor = &supervisor;

    std::string init_error;
    if (!supervisor.init(config, init_error)) {
        std::cerr << "[ERROR] Supervisor init failed: " << init_error << "\n";
        return 1;
    }

    supervisor.run();

    g_supervisor = nullptr;
    return 0;
}
