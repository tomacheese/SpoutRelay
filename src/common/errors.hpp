#pragma once
#include <string>
#include <stdexcept>

enum class ErrorCode {
    OK = 0,
    CONFIG_NOT_FOUND,
    CONFIG_PARSE_ERROR,
    CONFIG_VALIDATION_ERROR,
    SPOUT_INIT_FAILED,
    SPOUT_CONNECT_FAILED,
    SPOUT_RECEIVE_FAILED,
    ENCODER_INIT_FAILED,
    ENCODER_ENCODE_FAILED,
    RTSP_CONNECT_FAILED,
    RTSP_SEND_FAILED,
    RTSP_TIMEOUT,
    LOG_INIT_FAILED,
    FATAL_ERROR
};

inline const char* error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK:                       return "OK";
        case ErrorCode::CONFIG_NOT_FOUND:         return "CONFIG_NOT_FOUND";
        case ErrorCode::CONFIG_PARSE_ERROR:       return "CONFIG_PARSE_ERROR";
        case ErrorCode::CONFIG_VALIDATION_ERROR:  return "CONFIG_VALIDATION_ERROR";
        case ErrorCode::SPOUT_INIT_FAILED:        return "SPOUT_INIT_FAILED";
        case ErrorCode::SPOUT_CONNECT_FAILED:     return "SPOUT_CONNECT_FAILED";
        case ErrorCode::SPOUT_RECEIVE_FAILED:     return "SPOUT_RECEIVE_FAILED";
        case ErrorCode::ENCODER_INIT_FAILED:      return "ENCODER_INIT_FAILED";
        case ErrorCode::ENCODER_ENCODE_FAILED:    return "ENCODER_ENCODE_FAILED";
        case ErrorCode::RTSP_CONNECT_FAILED:      return "RTSP_CONNECT_FAILED";
        case ErrorCode::RTSP_SEND_FAILED:         return "RTSP_SEND_FAILED";
        case ErrorCode::RTSP_TIMEOUT:             return "RTSP_TIMEOUT";
        case ErrorCode::LOG_INIT_FAILED:          return "LOG_INIT_FAILED";
        case ErrorCode::FATAL_ERROR:              return "FATAL_ERROR";
        default:                                  return "UNKNOWN";
    }
}

class AppError : public std::runtime_error {
public:
    AppError(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};
