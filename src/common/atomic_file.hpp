#pragma once
#include <string>

namespace atomic_file {

// Atomically writes content to path.
// An existing path is replaced without being deleted, so the old content survives a failure.
// If error is non-null, it receives a message describing the cause on failure.
bool write(const std::string& path, const std::string& content,
           std::string* error = nullptr);

} // namespace atomic_file
