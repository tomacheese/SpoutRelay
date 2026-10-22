#include "common/atomic_file.hpp"
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <chrono>
#include <thread>
#endif

namespace fs = std::filesystem;

namespace atomic_file {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

#ifdef _WIN32
// 読み取り側が dest を開いている間に起こりうる、一時的なエラーのみ再試行する。
bool is_transient_error(DWORD code) {
    return code == ERROR_SHARING_VIOLATION ||
           code == ERROR_LOCK_VIOLATION ||
           code == ERROR_ACCESS_DENIED;
}
#endif

// tmp を dest へ置換する。dest が既に存在しても削除せず、置換に失敗した場合は
// dest の内容を変更しない。
bool replace_file(const fs::path& tmp, const fs::path& dest, std::string* error) {
#ifdef _WIN32
    constexpr int kMaxAttempts = 5;
    DWORD last_error = 0;
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (MoveFileExW(tmp.c_str(), dest.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        last_error = GetLastError();
        if (!is_transient_error(last_error) || i + 1 == kMaxAttempts)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    set_error(error, "MoveFileExW failed (error " + std::to_string(last_error) + ")");
    return false;
#else
    // POSIX の rename(2) は既存ファイルをアトミックに置換する。
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        set_error(error, "rename failed: " + ec.message());
        return false;
    }
    return true;
#endif
}

} // namespace

bool write(const std::string& path, const std::string& content, std::string* error) {
    if (error) error->clear();
    fs::path dest(path);
    fs::path tmp(path + ".tmp");
    std::error_code ec;
    try {
        auto parent = dest.parent_path();
        if (!parent.empty())
            fs::create_directories(parent);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                // tmp の位置に既存のファイルやディレクトリがありうるため削除しない
                set_error(error, "failed to open " + tmp.string());
                return false;
            }
            f << content;
            f.flush();
            // flush / close で報告される書き込みエラー (ENOSPC / EIO 等) を
            // 検出するため、暗黙の close に任せず明示的に確認する。
            f.close();
            if (f.fail()) {
                fs::remove(tmp, ec);
                set_error(error, "failed to write " + tmp.string());
                return false;
            }
        }

        if (!replace_file(tmp, dest, error)) {
            fs::remove(tmp, ec);   // 旧ファイルは保持したまま tmp のみ破棄する
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        fs::remove(tmp, ec);
        set_error(error, e.what());
        return false;
    } catch (...) {
        fs::remove(tmp, ec);
        set_error(error, "unknown exception");
        return false;
    }
}

} // namespace atomic_file
