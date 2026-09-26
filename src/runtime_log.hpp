#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rocket::diagnostics {

struct LogSnapshot {
    std::uint64_t revision = 0;
    std::uint64_t discarded_lines = 0;
    std::vector<std::string> lines;
};

// Called once before starting the platform/runtime. Captures native stdout and
// stderr, including dependencies, while preserving inherited terminal output.
void start_log(const std::filesystem::path& log_directory);
void stop_log();

// Non-blocking for the UI; retains the caller's snapshot if a reader is busy.
bool refresh_log(LogSnapshot& snapshot);
void clear_log();
std::filesystem::path log_path();
bool log_active();
bool log_file_available();

} // namespace rocket::diagnostics
