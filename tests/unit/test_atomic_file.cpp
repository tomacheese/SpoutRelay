#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "common/atomic_file.hpp"
#include "test_utils.hpp"

namespace fs = std::filesystem;

static std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

int run_atomic_file_tests() {
    printf("=== Atomic File Tests ===\n");

    {
        // 新規作成できること。tmp ファイルが残らないこと
        std::string path = "test_atomic_new_tmp.json";
        std::remove(path.c_str());
        VERIFY(atomic_file::write(path, "first"));
        VERIFY(read_all(path) == "first");
        VERIFY_MSG(!fs::exists(path + ".tmp"), "tmp must not remain after success");
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write creates a new file\n");
    }

    {
        // 既存ファイルを上書きできること (複数回)
        std::string path = "test_atomic_overwrite_tmp.json";
        std::remove(path.c_str());
        VERIFY(atomic_file::write(path, "first"));
        VERIFY(atomic_file::write(path, "second"));
        VERIFY(atomic_file::write(path, "third"));
        VERIFY(read_all(path) == "third");
        VERIFY(!fs::exists(path + ".tmp"));
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write overwrites an existing file\n");
    }

    {
        // 親ディレクトリが無くても作成されること
        std::string dir  = "test_atomic_dir_tmp";
        std::string path = dir + "/nested/health.json";
        fs::remove_all(dir);
        VERIFY(atomic_file::write(path, "nested"));
        VERIFY(read_all(path) == "nested");
        fs::remove_all(dir);
        printf("[PASS] atomic_file::write creates parent directories\n");
    }

    {
        // tmp への書き込みに失敗した場合、旧ファイルの内容が保持されること
        // ("<path>.tmp" をディレクトリにして open を失敗させる)
        std::string path = "test_atomic_keep_open_tmp.json";
        std::remove(path.c_str());
        fs::remove_all(path + ".tmp");
        VERIFY(atomic_file::write(path, "old"));
        fs::create_directory(path + ".tmp");
        VERIFY_MSG(!atomic_file::write(path, "new"), "write must fail");
        VERIFY_MSG(read_all(path) == "old", "old content must be preserved");
        fs::remove_all(path + ".tmp");
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write keeps old file when tmp cannot be created\n");
    }

    {
        // 置換に失敗した場合、旧内容が保持され tmp も残らないこと
        // (宛先を空でないディレクトリにして置換を失敗させる)
        std::string path = "test_atomic_keep_replace_tmp";
        fs::remove_all(path);
        fs::create_directories(path + "/child");
        VERIFY_MSG(!atomic_file::write(path, "new"), "replace must fail");
        VERIFY_MSG(fs::is_directory(path), "destination must be untouched");
        VERIFY_MSG(!fs::exists(path + ".tmp"), "tmp must be cleaned up on failure");
        fs::remove_all(path);
        printf("[PASS] atomic_file::write cleans up tmp when replace fails\n");
    }

    return 0;
}
