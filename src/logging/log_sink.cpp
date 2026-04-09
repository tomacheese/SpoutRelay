#include "logging/log_sink.hpp"
#include "common/time_utils.hpp"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

LogSink::LogSink() = default;
LogSink::~LogSink() {
    if (logger_) logger_->flush();
}

bool LogSink::init(const std::string& log_dir,
                   const std::string& instance_name,
                   std::string& error) {
    try {
        fs::create_directories(log_dir);

        std::string log_path = log_dir + "/" + instance_name + ".jsonl";

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_path, 10 * 1024 * 1024 /* 10 MB */, 5 /* files */);

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        logger_ = std::make_shared<spdlog::logger>(
            instance_name,
            spdlog::sinks_init_list{file_sink, console_sink});

        // Plain message pattern (JSON is built manually)
        logger_->set_pattern("%v");
        logger_->set_level(spdlog::level::debug);

    } catch (const std::exception& e) {
        error = std::string("Failed to init log sink: ") + e.what();
        return false;
    }

    return true;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;     break;
        }
    }
    return out;
}

std::string LogSink::build_json(const std::string& event_name,
                                const std::map<std::string, std::string>& fields) {
    std::ostringstream oss;
    oss << "{\"ts\":\"" << time_utils::iso8601_now() << "\"";
    oss << ",\"event\":\"" << json_escape(event_name) << "\"";
    for (const auto& kv : fields) {
        oss << ",\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

void LogSink::log_event(spdlog::level::level_enum level,
                        const std::string& event_name,
                        const std::map<std::string, std::string>& fields) {
    if (!logger_) return;
    logger_->log(level, build_json(event_name, fields));
}

void LogSink::log_state_changed(const std::string& from, const std::string& to) {
    log_event(spdlog::level::info, "state_changed",
              {{"from", from}, {"to", to}});
}

void LogSink::log_error(const std::string& code,
                        const std::string& message,
                        const std::map<std::string, std::string>& extra) {
    std::map<std::string, std::string> fields = {{"code", code}, {"message", message}};
    for (const auto& kv : extra) fields[kv.first] = kv.second;
    log_event(spdlog::level::err, "error", fields);
}

void LogSink::log_publish_started(const std::string& url,
                                  int width, int height, int fps) {
    log_event(spdlog::level::info, "publish_started",
              {{"url", url},
               {"width",  std::to_string(width)},
               {"height", std::to_string(height)},
               {"fps",    std::to_string(fps)}});
}

void LogSink::log_publish_stopped(const std::string& reason) {
    log_event(spdlog::level::info, "publish_stopped", {{"reason", reason}});
}

void LogSink::log_reconnect(int attempt, int delay_ms) {
    log_event(spdlog::level::warn, "reconnecting",
              {{"attempt",  std::to_string(attempt)},
               {"delay_ms", std::to_string(delay_ms)}});
}

void LogSink::log_frame_received(uint64_t sequence,
                                 uint32_t width, uint32_t height) {
    log_event(spdlog::level::debug, "frame_received",
              {{"seq",    std::to_string(sequence)},
               {"width",  std::to_string(width)},
               {"height", std::to_string(height)}});
}

void LogSink::log_encoder_initialized(const std::string& codec,
                                      int width, int height,
                                      int fps, int bitrate_kbps) {
    log_event(spdlog::level::info, "encoder_initialized",
              {{"codec",       codec},
               {"width",       std::to_string(width)},
               {"height",      std::to_string(height)},
               {"fps",         std::to_string(fps)},
               {"bitrate_kbps",std::to_string(bitrate_kbps)}});
}

void LogSink::flush() {
    if (logger_) logger_->flush();
}
