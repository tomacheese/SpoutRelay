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

// tmp を dest へ置換する。dest が既に存在しても削除せず、置換に失敗した場合は
// dest の内容を変更しない。
bool replace_file(const fs::path& tmp, const fs::path& dest) {
#ifdef _WIN32
    // 読み取り側が dest を開いている間は共有違反で失敗しうるため、短時間リトライする。
    constexpr int kMaxAttempts = 5;
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (MoveFileExW(tmp.c_str(), dest.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        if (i + 1 < kMaxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
#else
    // POSIX の rename(2) は既存ファイルをアトミックに置換する。
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    return !ec;
#endif
}

} // namespace

bool write(const std::string& path, const std::string& content) {
    fs::path dest(path);
    fs::path tmp(path + ".tmp");
    std::error_code ec;
    try {
        auto parent = dest.parent_path();
        if (!parent.empty())
            fs::create_directories(parent);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << content;
            f.flush();
            // close() 時にはじめて報告される書き込みエラー (ENOSPC / EIO 等) を
            // 検出するため、スコープ終了時の暗黙 close に任せず明示的に確認する。
            f.close();
            if (f.fail()) {
                fs::remove(tmp, ec);
                return false;
            }
        }

        if (!replace_file(tmp, dest)) {
            fs::remove(tmp, ec);   // 旧ファイルは保持したまま tmp のみ破棄する
            return false;
        }
        return true;
    } catch (...) {
        fs::remove(tmp, ec);
        return false;
    }
}

} // namespace atomic_file
