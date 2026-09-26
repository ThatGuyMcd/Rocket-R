#include "runtime_log.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

using namespace rocket::diagnostics;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Text(const LogSnapshot& snapshot) {
    std::string result;
    for (const auto& line : snapshot.lines) { result += line; result += '\n'; }
    return result;
}
std::string FileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
LogSnapshot WaitFor(const std::string& text) {
    LogSnapshot snapshot;
    for (int i = 0; i < 500; ++i) {
        refresh_log(snapshot);
        if (Text(snapshot).find(text) != std::string::npos) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("timed out waiting for captured output: " + text);
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("rocket-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    try {
#if defined(_WIN32)
        Require(GetConsoleWindow() == nullptr, "GUI test must not create a console");
#endif
        start_log(directory / "logs");
        Require(log_active() && log_file_available(), "capture/file startup failed");
        std::printf("stdout: 100%% ready\r\n");
        std::fprintf(stderr, "stderr: ready\n");
        std::cout << "iostream: ready\n" << std::flush;
        std::fprintf(stderr, "unfinished");
        WaitFor("unfinished");
        std::fprintf(stderr, " message\n");
#if defined(_WIN32)
        DWORD written = 0;
        const char native[] = "native handle: ready\n";
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), native, sizeof(native) - 1, &written, nullptr);
        Require(written == sizeof(native) - 1, "native handle write failed");
        WaitFor("native handle: ready");
#endif
        auto snapshot = WaitFor("unfinished message");
        WaitFor("stdout: 100% ready");
        WaitFor("iostream: ready");
        stop_log();
        const auto first_file = log_path();
        const auto saved = FileText(first_file);
        Require(saved.find("stdout: 100% ready") != std::string::npos, "missing stdout in file");
        Require(saved.find("stderr: ready") != std::string::npos, "missing stderr in file");
        // The two streams may interleave in the raw file, as in a terminal.
        Require(saved.find("unfinished") != std::string::npos && saved.find(" message") != std::string::npos,
                "partial line bytes were lost");

        // Tee into inherited streams while stressing both readers. Keep test
        // flood output out of CTest's own capture pipe.
        const auto tee_out = directory / "stdout.txt";
        const auto tee_err = directory / "stderr.txt";
#if defined(_WIN32)
        FILE* ignored = nullptr;
        _wfreopen_s(&ignored, tee_out.c_str(), L"wb", stdout);
        _wfreopen_s(&ignored, tee_err.c_str(), L"wb", stderr);
#else
        std::freopen(tee_out.c_str(), "wb", stdout);
        std::freopen(tee_err.c_str(), "wb", stderr);
#endif
        clear_log();
        start_log(directory / "flood");
        std::thread out([] {
            for (int i = 0; i < 4000; ++i) std::printf("out-%d\n", i);
            std::printf("out-complete\n");
        });
        std::thread err([] {
            for (int i = 0; i < 4000; ++i) std::fprintf(stderr, "err-%d\n", i);
            std::fprintf(stderr, "err-complete\n");
        });
        out.join(); err.join();
        WaitFor("err-complete");
        std::printf("out-complete\n");
        WaitFor("out-complete");
        snapshot = {};
        refresh_log(snapshot);
        Require(snapshot.lines.size() <= 2002 && snapshot.discarded_lines >= 6000,
                "history line limit failed");
        clear_log();
        snapshot = {};
        refresh_log(snapshot);
        Require(snapshot.lines.empty(), "clear must empty the live history");

        const std::string large(10 * 1024 * 1024, 'x');
        std::fwrite(large.data(), 1, large.size(), stdout);
        std::printf("\nrotation-complete\n");
        snapshot = WaitFor("rotation-complete");
        Require(Text(snapshot).size() <= 1024 * 1024 + 8192, "history byte limit failed");
        for (const auto& line : snapshot.lines) Require(line.size() <= 4096, "long line limit failed");
        std::fprintf(stderr, "shutdown-tail");
        const auto path = log_path();
        stop_log();
#if defined(_WIN32)
        // Release the CRT's open file-sharing handles before reading the tees.
        freopen_s(&ignored, "NUL", "w", stdout);
        freopen_s(&ignored, "NUL", "w", stderr);
#else
        std::freopen("/dev/null", "w", stdout);
        std::freopen("/dev/null", "w", stderr);
#endif
        Require(FileText(path).find("shutdown-tail") != std::string::npos, "shutdown did not drain pipe");
        Require(std::filesystem::file_size(path) <= 8 * 1024 * 1024, "current log too large");
        Require(std::filesystem::file_size(path.string() + ".previous") <= 8 * 1024 * 1024,
                "rotated log too large");
        Require(FileText(tee_out).find("out-complete") != std::string::npos, "stdout tee failed");
        Require(FileText(tee_err).find("err-complete") != std::string::npos, "stderr tee failed");

        // An unwritable log destination must not disable live diagnostics.
        std::ofstream(directory / "not-a-directory") << "file";
        start_log(directory / "not-a-directory" / "logs");
        Require(log_active() && !log_file_available(), "disk failure disabled capture");
        std::fprintf(stderr, "memory-only-survives\n");
        WaitFor("memory-only-survives");
        stop_log();
        std::ofstream(directory / "PASS.txt") << "GUI/capture/partial/tee/concurrency/bounds/rotation/clear/shutdown/disk-failure passed\n";
        return 0;
    } catch (const std::exception& error) {
        stop_log();
        std::ofstream(directory / "FAIL.txt") << error.what() << '\n';
        std::fprintf(stderr, "runtime_log_tests: %s\n", error.what());
        return 1;
    }
}
