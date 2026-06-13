#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>
#include "logging/log_sink.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

/// @brief ファイル全体を文字列として読み込む
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

/// @brief JSON Lines のうち最初の非空行を 1 件の JSON として解析する
static json parse_first_line(const std::string& content) {
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) return json::parse(line);
    }
    throw std::runtime_error("no non-empty line found");
}

/// @brief JSON Lines の最後の非空行を 1 件の JSON として解析する
static json parse_last_line(const std::string& content) {
    std::istringstream iss(content);
    std::string line, last;
    while (std::getline(iss, line)) {
        if (!line.empty()) last = line;
    }
    if (last.empty()) throw std::runtime_error("no non-empty line found");
    return json::parse(last);
}

int run_log_sink_tests() {
    printf("=== Log Sink Tests ===\n");

    const std::string log_dir = "test_log_sink_tmp";
    fs::remove_all(log_dir);

    {
        // init() がログディレクトリとファイルを作成する
        LogSink sink;
        std::string err;
        bool ok = sink.init(log_dir, "test-instance", err);
        VERIFY_MSG(ok, "LogSink::init should succeed");
        VERIFY(fs::exists(log_dir));

        sink.log_state_changed("IDLE", "PROBING");
        sink.flush();

        std::string content = read_file(log_dir + "/test-instance.jsonl");
        VERIFY_MSG(!content.empty(), "log file should not be empty after log_state_changed");

        json j = parse_first_line(content);
        VERIFY(j["event"] == "state_changed");
        VERIFY(j["from"]  == "IDLE");
        VERIFY(j["to"]    == "PROBING");
        VERIFY(j.contains("ts"));
        printf("[PASS] init() creates log dir/file, log_state_changed writes valid JSON\n");
    }

    {
        // json_escape: 改行・タブ・バックスラッシュ・ダブルクオートを含む値が
        // 正しくエスケープされ、パース後に元の文字列へ復元できること
        fs::remove_all(log_dir);
        LogSink sink;
        std::string err;
        VERIFY(sink.init(log_dir, "test-instance", err));

        const std::string tricky = "line1\nline2\ttab\\backslash\"quote";
        sink.log_error("TEST_CODE", tricky);
        sink.flush();

        std::string content = read_file(log_dir + "/test-instance.jsonl");
        json j = parse_first_line(content);
        VERIFY(j["event"]   == "error");
        VERIFY(j["code"]    == "TEST_CODE");
        VERIFY(j["message"] == tricky);
        printf("[PASS] json_escape round-trips special characters (\\n, \\t, \\\\, \\\")\n");
    }

    {
        // log_error: extra フィールドが code/message とともにマージされること
        fs::remove_all(log_dir);
        LogSink sink;
        std::string err;
        VERIFY(sink.init(log_dir, "test-instance", err));

        sink.log_error("RTSP_SEND_FAILED", "send failed", {{"attempt", "3"}});
        sink.flush();

        std::string content = read_file(log_dir + "/test-instance.jsonl");
        json j = parse_first_line(content);
        VERIFY(j["code"]    == "RTSP_SEND_FAILED");
        VERIFY(j["message"] == "send failed");
        VERIFY(j["attempt"] == "3");
        printf("[PASS] log_error merges extra fields\n");
    }

    {
        // log_publish_started / log_reconnect / log_frame_received /
        // log_encoder_initialized が想定どおりのフィールドを出力すること
        fs::remove_all(log_dir);
        LogSink sink;
        std::string err;
        VERIFY(sink.init(log_dir, "test-instance", err));

        sink.log_publish_started("rtsp://127.0.0.1:8554/spout-e2e", 1280, 720, 30);
        sink.log_reconnect(2, 1500);
        sink.log_frame_received(42, 1280, 720);
        sink.log_encoder_initialized("h264_nvenc", 1280, 720, 30, 4000);
        sink.flush();

        std::string content = read_file(log_dir + "/test-instance.jsonl");
        std::istringstream iss(content);
        std::vector<json> events;
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) events.push_back(json::parse(line));
        }
        VERIFY_MSG(events.size() == 4, "expected 4 log lines");

        VERIFY(events[0]["event"] == "publish_started");
        VERIFY(events[0]["url"]   == "rtsp://127.0.0.1:8554/spout-e2e");
        VERIFY(events[0]["width"] == "1280");
        VERIFY(events[0]["fps"]   == "30");

        VERIFY(events[1]["event"]    == "reconnecting");
        VERIFY(events[1]["attempt"]  == "2");
        VERIFY(events[1]["delay_ms"] == "1500");

        VERIFY(events[2]["event"] == "frame_received");
        VERIFY(events[2]["seq"]   == "42");

        VERIFY(events[3]["event"]        == "encoder_initialized");
        VERIFY(events[3]["codec"]        == "h264_nvenc");
        VERIFY(events[3]["bitrate_kbps"] == "4000");
        printf("[PASS] structured log helpers emit expected fields\n");
    }

    {
        // log_publish_stopped が reason フィールドを出力すること
        fs::remove_all(log_dir);
        LogSink sink;
        std::string err;
        VERIFY(sink.init(log_dir, "test-instance", err));

        sink.log_publish_stopped("shutdown_requested");
        sink.flush();

        std::string content = read_file(log_dir + "/test-instance.jsonl");
        json j = parse_last_line(content);
        VERIFY(j["event"]  == "publish_stopped");
        VERIFY(j["reason"] == "shutdown_requested");
        printf("[PASS] log_publish_stopped emits reason field\n");
    }

    {
        // ログディレクトリと同名の通常ファイルが既に存在する場合、
        // ディレクトリ作成に失敗し init() がエラーを返すこと
        const std::string blocked = "test_log_sink_blocked_file";
        fs::remove_all(blocked);
        {
            std::ofstream f(blocked);
            f << "not a directory";
        }

        LogSink sink;
        std::string err;
        bool ok = sink.init(blocked, "test-instance", err);
        VERIFY_MSG(!ok, "init() should fail when log_dir path is occupied by a regular file");
        VERIFY_MSG(!err.empty(), "init() should report an error message on failure");

        fs::remove_all(blocked);
        printf("[PASS] init() fails gracefully when log directory cannot be created\n");
    }

    fs::remove_all(log_dir);
    return 0;
}
