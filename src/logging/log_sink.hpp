#pragma once
#include <string>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

class LogSink {
public:
    LogSink();
    ~LogSink();

    bool init(const std::string& log_dir,
              const std::string& instance_name,
              std::string& error);

    void log_event(spdlog::level::level_enum level,
                   const std::string& event_name,
                   const std::map<std::string, std::string>& fields = {});

    void log_state_changed(const std::string& from, const std::string& to);

    void log_error(const std::string& code,
                   const std::string& message,
                   const std::map<std::string, std::string>& extra = {});

    void log_publish_started(const std::string& url,
                             int width, int height, int fps);

    void log_publish_stopped(const std::string& reason);

    void log_reconnect(int attempt, int delay_ms);

    void log_frame_received(uint64_t sequence, uint32_t width, uint32_t height);

    void log_encoder_initialized(const std::string& codec,
                                 int width, int height,
                                 int fps, int bitrate_kbps);

    void flush();

private:
    std::string build_json(const std::string& event_name,
                           const std::map<std::string, std::string>& fields);

    std::shared_ptr<spdlog::logger> logger_;
};
