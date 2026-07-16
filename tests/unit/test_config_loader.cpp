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
    // placeholder セクション省略時は既存挙動を維持する既定値となること
    VERIFY(cfg.placeholder.enabled == false);
    VERIFY(cfg.placeholder.width == 1280);
    VERIFY(cfg.placeholder.height == 720);
    VERIFY(cfg.placeholder.message == "NO SIGNAL");
    VERIFY(cfg.placeholder.background_hex == "#000000");
    VERIFY(cfg.placeholder.text_hex == "#FFFFFF");
    VERIFY(cfg.placeholder.show_sender_name == true);
    // spout.stalled_recovery_max_attempts 省略時は既定値 (10) となること
    VERIFY(cfg.spout.stalled_recovery_max_attempts == 10);
    printf("[PASS] test_load_minimal_valid\n");
}

void test_load_stalled_recovery_max_attempts() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender", "stalled_recovery_max_attempts": 5 },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "spout.stalled_recovery_max_attempts should load");
    VERIFY(cfg.spout.stalled_recovery_max_attempts == 5);
    printf("[PASS] test_load_stalled_recovery_max_attempts\n");
}

void test_stalled_recovery_max_attempts_zero_disables_watchdog() {
    // 0 以下はウォッチドッグ無効化を意味する仕様上の正当な値であり、
    // バリデーションエラーにはしない
    const std::string json = R"({
      "spout": { "sender_name": "TestSender", "stalled_recovery_max_attempts": 0 },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "spout.stalled_recovery_max_attempts=0 should be valid (watchdog disabled)");
    VERIFY(cfg.spout.stalled_recovery_max_attempts == 0);
    printf("[PASS] test_stalled_recovery_max_attempts_zero_disables_watchdog\n");
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
extern int run_placeholder_renderer_tests();
extern int run_log_sink_tests();
extern int run_frame_pump_tests();
extern int run_supervisor_logic_tests();

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

void test_load_placeholder_section() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 },
      "placeholder": {
        "enabled": true,
        "width": 1920,
        "height": 1080,
        "message": "WAITING",
        "background_hex": "#112233",
        "text_hex": "#aabbcc",
        "show_sender_name": false
      }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(ok, "placeholder section should load");
    VERIFY(cfg.placeholder.enabled == true);
    VERIFY(cfg.placeholder.width == 1920);
    VERIFY(cfg.placeholder.height == 1080);
    VERIFY(cfg.placeholder.message == "WAITING");
    VERIFY(cfg.placeholder.background_hex == "#112233");
    VERIFY(cfg.placeholder.text_hex == "#aabbcc");
    VERIFY(cfg.placeholder.show_sender_name == false);
    printf("[PASS] test_load_placeholder_section\n");
}

void test_invalid_placeholder_hex_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 },
      "placeholder": { "background_hex": "not-a-color" }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Invalid placeholder.background_hex should fail validation");
    printf("[PASS] test_invalid_placeholder_hex_fails\n");
}

void test_invalid_placeholder_size_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 },
      "placeholder": { "width": 0, "height": 720 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Non-positive placeholder.width should fail validation");
    printf("[PASS] test_invalid_placeholder_size_fails\n");
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

// -----------------------------------------------------------------------
// issue #20 の調査で判明した未テストの検証ルールを網羅する
// -----------------------------------------------------------------------

void test_fps_over_limit_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": 241, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "encoder.fps > 240 should fail validation");
    printf("[PASS] test_fps_over_limit_fails\n");
}

void test_non_positive_bitrate_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": 30, "bitrate_kbps": 0 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "encoder.bitrate_kbps <= 0 should fail validation");
    printf("[PASS] test_non_positive_bitrate_fails\n");
}

void test_non_positive_poll_interval_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender", "poll_interval_ms": 0 },
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "spout.poll_interval_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_poll_interval_fails\n");
}

void test_non_positive_frame_timeout_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender", "frame_timeout_ms": 0 },
      "rtsp": { "url": "rtsp://host/path" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "spout.frame_timeout_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_frame_timeout_fails\n");
}

void test_non_positive_connect_timeout_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "connect_timeout_ms": 0 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.connect_timeout_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_connect_timeout_fails\n");
}

void test_non_positive_send_timeout_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "send_timeout_ms": 0 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.send_timeout_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_send_timeout_fails\n");
}

void test_negative_max_reconnect_attempts_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "max_reconnect_attempts": -1 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.max_reconnect_attempts < 0 should fail validation");
    printf("[PASS] test_negative_max_reconnect_attempts_fails\n");
}

void test_non_positive_reconnect_delay_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "reconnect_delay_ms": 0 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.reconnect_delay_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_reconnect_delay_fails\n");
}

void test_non_positive_reconnect_max_delay_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "reconnect_max_delay_ms": 0 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.reconnect_max_delay_ms <= 0 should fail validation");
    printf("[PASS] test_non_positive_reconnect_max_delay_fails\n");
}

void test_reconnect_max_delay_less_than_delay_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "reconnect_delay_ms": 5000, "reconnect_max_delay_ms": 1000 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.reconnect_max_delay_ms < rtsp.reconnect_delay_ms should fail validation");
    printf("[PASS] test_reconnect_max_delay_less_than_delay_fails\n");
}

void test_reconnect_backoff_multiplier_below_one_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://host/path", "reconnect_backoff_multiplier": 0.5 },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "rtsp.reconnect_backoff_multiplier < 1.0 should fail validation");
    printf("[PASS] test_reconnect_backoff_multiplier_below_one_fails\n");
}

void test_invalid_placeholder_text_hex_fails() {
    const std::string json = R"({
      "spout": { "sender_name": "TestSender" },
      "rtsp": { "url": "rtsp://192.168.0.100:8554/live" },
      "encoder": { "fps": 30, "bitrate_kbps": 4000 },
      "placeholder": { "text_hex": "not-a-color" }
    })";
    auto path = write_temp_config(json);
    AppConfig cfg;
    std::string err;
    bool ok = ConfigLoader::load(path, cfg, err);
    cleanup(path);
    VERIFY_MSG(!ok, "Invalid placeholder.text_hex should fail validation");
    printf("[PASS] test_invalid_placeholder_text_hex_fails\n");
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
    test_load_placeholder_section();
    test_invalid_placeholder_hex_fails();
    test_invalid_placeholder_size_fails();
    test_fps_over_limit_fails();
    test_non_positive_bitrate_fails();
    test_non_positive_poll_interval_fails();
    test_non_positive_frame_timeout_fails();
    test_non_positive_connect_timeout_fails();
    test_non_positive_send_timeout_fails();
    test_negative_max_reconnect_attempts_fails();
    test_non_positive_reconnect_delay_fails();
    test_non_positive_reconnect_max_delay_fails();
    test_reconnect_max_delay_less_than_delay_fails();
    test_reconnect_backoff_multiplier_below_one_fails();
    test_invalid_placeholder_text_hex_fails();
    test_load_stalled_recovery_max_attempts();
    test_stalled_recovery_max_attempts_zero_disables_watchdog();

    run_state_machine_tests();
    run_metrics_tests();
    run_backoff_tests();
    run_placeholder_renderer_tests();
    run_log_sink_tests();
    run_frame_pump_tests();
    run_supervisor_logic_tests();

    printf("\nAll tests passed.\n");
    return 0;
}
