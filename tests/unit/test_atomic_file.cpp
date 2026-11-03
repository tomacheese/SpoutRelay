#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "common/atomic_file.hpp"
#include "test_utils.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

int run_atomic_file_tests() {
    printf("=== Atomic File Tests ===\n");

    {
        // Can create a new file; no tmp file is left behind
        std::string path = "test_atomic_new_tmp.json";
        std::remove(path.c_str());
        std::string error = "stale";
        VERIFY(atomic_file::write(path, "first", &error));
        VERIFY_MSG(error.empty(), "error must be cleared on success");
        VERIFY(read_all(path) == "first");
        VERIFY_MSG(!fs::exists(path + ".tmp"), "tmp must not remain after success");
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write creates a new file\n");
    }

    {
        // Can overwrite an existing file (repeatedly)
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
        // Creates the parent directory if it is missing
        std::string dir  = "test_atomic_dir_tmp";
        std::string path = dir + "/nested/health.json";
        fs::remove_all(dir);
        VERIFY(atomic_file::write(path, "nested"));
        VERIFY(read_all(path) == "nested");
        fs::remove_all(dir);
        printf("[PASS] atomic_file::write creates parent directories\n");
    }

    {
        // If writing to tmp fails, the old file content is preserved
        // (inject by making "<path>.tmp" a directory so open fails)
        std::string path = "test_atomic_keep_open_tmp.json";
        std::remove(path.c_str());
        fs::remove_all(path + ".tmp");
        VERIFY(atomic_file::write(path, "old"));
        fs::create_directory(path + ".tmp");
        std::string error;
        VERIFY_MSG(!atomic_file::write(path, "new", &error), "write must fail");
        VERIFY_MSG(!error.empty(), "error message must be set on failure");
        VERIFY_MSG(read_all(path) == "old", "old content must be preserved");
        fs::remove_all(path + ".tmp");
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write keeps old file when tmp cannot be created\n");
    }

#ifndef _WIN32
    {
        // If writing to tmp fails with ENOSPC (flush/close failure),
        // the old file must not be replaced with partial content
        // (inject by making "<path>.tmp" a symlink to /dev/full)
        if (fs::exists("/dev/full")) {
            std::string path = "test_atomic_enospc_tmp.json";
            std::remove(path.c_str());
            std::remove((path + ".tmp").c_str());
            VERIFY(atomic_file::write(path, "old"));
            fs::create_symlink("/dev/full", path + ".tmp");
            VERIFY_MSG(!atomic_file::write(path, "new"), "write must fail on ENOSPC");
            VERIFY_MSG(read_all(path) == "old", "old content must be preserved");
            VERIFY_MSG(!fs::exists(fs::symlink_status(path + ".tmp")),
                       "tmp must be cleaned up on failure");
            std::remove(path.c_str());
            printf("[PASS] atomic_file::write preserves old file on write error (ENOSPC)\n");
        }
    }
#endif

#ifdef _WIN32
    {
        // While the destination is open without FILE_SHARE_DELETE (replacement hits a sharing violation),
        // the old content is preserved and no tmp is left even after the retry limit.
        // Replacement succeeds once the handle is closed.
        std::string path = "test_atomic_sharing_tmp.json";
        std::remove(path.c_str());
        VERIFY(atomic_file::write(path, "old"));

        HANDLE h = CreateFileW(fs::path(path).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        VERIFY_MSG(h != INVALID_HANDLE_VALUE, "failed to open destination");
        VERIFY_MSG(!atomic_file::write(path, "new"), "replace must fail while locked");
        VERIFY_MSG(!fs::exists(path + ".tmp"), "tmp must be cleaned up on failure");
        CloseHandle(h);
        VERIFY_MSG(read_all(path) == "old", "old content must be preserved");

        VERIFY_MSG(atomic_file::write(path, "new"), "write must succeed after unlock");
        VERIFY(read_all(path) == "new");
        std::remove(path.c_str());
        printf("[PASS] atomic_file::write preserves old file on sharing violation\n");
    }
#endif

    {
        // If replacement fails, the old content is preserved and no tmp is left
        // (inject by making the destination a non-empty directory)
        std::string path = "test_atomic_keep_replace_tmp";
        fs::remove_all(path);
        fs::create_directories(path + "/child");
        std::string error;
        VERIFY_MSG(!atomic_file::write(path, "new", &error), "replace must fail");
        VERIFY_MSG(!error.empty(), "error message must be set on failure");
        VERIFY_MSG(fs::is_directory(path), "destination must be untouched");
        VERIFY_MSG(!fs::exists(path + ".tmp"), "tmp must be cleaned up on failure");
        fs::remove_all(path);
        printf("[PASS] atomic_file::write cleans up tmp when replace fails\n");
    }

    return 0;
}
