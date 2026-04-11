#include <cstdio>
#include <fstream>
#include <filesystem>
#include "config/config_loader.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;

static std::string write_temp_config(const std::string& content) {
    std::string path = "test_config_tmp.json";
    std::ofstream f(path);
    f << content;
    return path;
}

static void cleanup(const std::string& path) {
    fs::remove(path);
}

void test_load_minimal_valid() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Minimal valid config should load");
    VERIFY(cfg.rtsp.url == "rtsp://192.168.0.100:8554/live");
    VERIFY(cfg.encoder.fps == 30);
    VERIFY(cfg.encoder.bitrate_kbps == 4000);
    printf("[PASS] test_load_minimal_valid\n");
}

void test_load_all_sections() {
    const std::string json = R"({
      "app": {
        "instance_name": "test-01",
        "log_dir": "./testlogs",
        "metrics_path": "./state/m.json",
        "health_path": "./state/h.json"
      },
      "spout": {
        "sender_name": "MySender",
        "poll_interval_ms": 33,
        "frame_timeout_ms": 500
      },
      "encoder": {
        "codec": "libx264",
        "fallback_codec": "",
        "bitrate_kbps": 2000,
        "fps": 60,
        "gop_size": 60,
        "max_b_frames": 0,
        "preset": "ultrafast",
        "tune": "zerolatency"
      },
      "rtsp": {
        "url": "rtsp://localhost:8554/test",
        "connect_timeout_ms": 3000,
        "max_reconnect_attempts": 3
      },
      "runtime": {
        "shutdown_grace_ms": 2000,
        "emit_metrics_interval_ms": 500
      }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "Full config should load");
    VERIFY(cfg.app.instance_name == "test-01");
    VERIFY(cfg.spout.sender_name == "MySender");
    VERIFY(cfg.spout.poll_interval_ms == 33);
    VERIFY(cfg.encoder.codec == "libx264");
    VERIFY(cfg.encoder.fps == 60);
    VERIFY(cfg.rtsp.max_reconnect_attempts == 3);
    VERIFY(cfg.runtime.shutdown_grace_ms == 2000);
    printf("[PASS] test_load_all_sections\n");
}

void test_missing_file() {
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load("nonexistent_file_xyz.json", cfg, err);
    VERIFY_MSG(!ok, "Missing file should fail");
    VERIFY(!err.empty());
    printf("[PASS] test_missing_file\n");
}

void test_invalid_json() {
    auto path = write_temp_config("{invalid json}");
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Invalid JSON should fail");
    printf("[PASS] test_invalid_json\n");
}

void test_missing_rtsp_url_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Missing rtsp.url should fail validation");
    printf("[PASS] test_missing_rtsp_url_fails\n");
}

void test_invalid_fps_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": -1, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Negative fps should fail");
    printf("[PASS] test_invalid_fps_fails\n");
}

// Declared in other test files
extern int run_state_machine_tests();
extern int run_metrics_tests();
extern int run_backoff_tests();

void test_invalid_rtsp_url_prefix_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "http://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Non-rtsp:// URL should fail validation");
    printf("[PASS] test_invalid_rtsp_url_prefix_fails\n");
}

void test_empty_sender_name_fails() {
    const std::string json = R"({
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Empty sender_name should fail validation");
    printf("[PASS] test_empty_sender_name_fails\n");
}

int main() {
    printf("=== Config Loader Tests ===\n");
    test_load_minimal_valid();
    test_load_all_sections();
    test_missing_file();
    test_invalid_json();
    test_missing_rtsp_url_fails();
    test_invalid_fps_fails();
    test_invalid_rtsp_url_prefix_fails();
    test_empty_sender_name_fails();

    run_state_machine_tests();
    run_metrics_tests();
    run_backoff_tests();

    printf("\nAll tests passed.\n");
    return 0;
}
