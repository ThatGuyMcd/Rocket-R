#include "runtime_log.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <ctime>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {
constexpr std::size_t kMaxLines = 2000;
constexpr std::size_t kMaxBytes = 1024 * 1024;
constexpr std::size_t kMaxLine = 4096;
constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;

struct Capture {
    FILE* stream = nullptr;
    int original = -1;
    int read_fd = -1;
    bool echo = false;
    std::thread reader;
};

struct LogState {
    std::mutex history_mutex;
    std::deque<std::string> lines;
    std::array<std::string, 2> partial;
    std::size_t bytes = 0;
    std::uint64_t revision = 1;
    std::uint64_t discarded = 0;
    std::mutex file_mutex;
    std::ofstream file;
    std::filesystem::path path;
    std::size_t file_bytes = 0;
    std::atomic<bool> file_ok{false};
    std::atomic<bool> active{false};
    std::array<Capture, 2> captures;
};

LogState& State() {
    // Runtime dependencies can log from static destructors. Keep the state
    // alive until process teardown; stop_log restores their original streams.
    static auto* state = new LogState;
    return *state;
}

#if defined(_WIN32)
int Close(int fd) { return _close(fd); }
int Dup(int fd) { return _dup(fd); }
int DupTo(int from, int to) { return _dup2(from, to); }
int Fileno(FILE* stream) { return _fileno(stream); }
int Read(int fd, char* data, unsigned size) { return _read(fd, data, size); }
int Write(int fd, const char* data, unsigned size) { return _write(fd, data, size); }
#else
int Close(int fd) { return close(fd); }
int Dup(int fd) { return dup(fd); }
int DupTo(int from, int to) { return dup2(from, to); }
int Fileno(FILE* stream) { return fileno(stream); }
int Read(int fd, char* data, unsigned size) { return static_cast<int>(read(fd, data, size)); }
int Write(int fd, const char* data, unsigned size) { return static_cast<int>(write(fd, data, size)); }
#endif

void AddLine(LogState& state, std::string& line) {
    state.bytes += line.size();
    state.lines.push_back(std::move(line));
    line.clear();
    while (state.lines.size() > kMaxLines || state.bytes > kMaxBytes) {
        state.bytes -= state.lines.front().size();
        state.lines.pop_front();
        ++state.discarded;
    }
}

void Append(unsigned stream, std::string_view text) {
    auto& state = State();
    {
        std::lock_guard lock(state.history_mutex);
        auto& line = state.partial[stream];
        for (char ch : text) {
            if (ch == '\n') AddLine(state, line);
            else if (ch != '\r') {
                // ImGui consumes C strings; keep embedded NULs visible.
                line += ch == '\0' ? '?' : ch;
                if (line.size() == kMaxLine) AddLine(state, line);
            }
        }
        ++state.revision;
    }
    // Only background pipe readers touch disk. No UI/history lock is held.
    std::lock_guard lock(state.file_mutex);
    if (!state.file_ok.load()) return;
    if (state.file_bytes + text.size() > kMaxFileBytes) {
        state.file.close();
        auto previous = state.path;
        previous += ".previous";
        std::error_code ec;
        std::filesystem::remove(previous, ec);
        ec.clear();
        std::filesystem::rename(state.path, previous, ec);
        state.file.open(state.path, std::ios::binary | std::ios::trunc);
        state.file_bytes = 0;
    }
    state.file.write(text.data(), static_cast<std::streamsize>(text.size()));
    state.file.flush();
    state.file_bytes += text.size();
    state.file_ok.store(state.file.good());
}

void ReadStream(unsigned index) {
    auto& capture = State().captures[index];
    char buffer[4096];
    for (;;) {
        const int count = Read(capture.read_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        Append(index, {buffer, static_cast<std::size_t>(count)});
#if defined(__ANDROID__)
        // Retain native diagnostics in logcat, including unterminated messages.
        __android_log_print(ANDROID_LOG_INFO, "Rocket-R", "%.*s", count, buffer);
#else
        if (capture.echo) {
            int offset = 0;
            while (offset < count) {
                const int written = Write(capture.original, buffer + offset, count - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { capture.echo = false; break; }
                offset += written;
            }
        }
#endif
    }
    Close(capture.read_fd);
    capture.read_fd = -1;
}

bool StartStream(unsigned index, FILE* stream) {
    auto& capture = State().captures[index];
    capture.stream = stream;
    int fd = Fileno(stream);
    capture.echo = fd >= 0;
#if defined(_WIN32)
    if (fd < 0) {
        // GUI processes launched from Explorer have no CRT standard streams.
        // Give freopen a real descriptor before dup2; never allocate a console.
        FILE* reopened = nullptr;
        if (freopen_s(&reopened, "NUL", "w", stream) != 0) return false;
        fd = Fileno(stream);
    }
#endif
    if (fd < 0) return false;
    std::fflush(stream);
    capture.original = Dup(fd);
    if (capture.original < 0) return false;
    int pipes[2];
#if defined(_WIN32)
    const int result = _pipe(pipes, 65536, _O_BINARY | _O_NOINHERIT);
#else
    const int result = pipe(pipes);
    if (result == 0) {
        fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
        fcntl(capture.original, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (result != 0) { Close(capture.original); capture.original = -1; return false; }
    if (DupTo(pipes[1], fd) < 0) {
        Close(pipes[0]); Close(pipes[1]); Close(capture.original);
        capture.original = -1;
        return false;
    }
    Close(pipes[1]);
    capture.read_fd = pipes[0];
    std::setvbuf(stream, nullptr, _IONBF, 0);
#if defined(_WIN32)
    SetStdHandle(index == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,
                 reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
#else
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    try {
        capture.reader = std::thread(ReadStream, index);
    } catch (...) {
        DupTo(capture.original, fd);
        Close(capture.original); Close(capture.read_fd);
        capture.original = capture.read_fd = -1;
        return false;
    }
    return true;
}
} // namespace

void rocket::diagnostics::start_log(const std::filesystem::path& directory) {
    auto& state = State();
    if (state.active.load()) return;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // A separate file for each process avoids corrupting another open launcher.
    const auto now = std::time(nullptr);
#if defined(_WIN32)
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    state.path = directory / ("runtime-" + std::to_string(now) + "-" + std::to_string(pid) + ".log");
    state.file.open(state.path, std::ios::binary | std::ios::trunc);
    state.file_bytes = 0;
    state.file_ok.store(state.file.good());
    const bool out = StartStream(0, stdout);
    const bool err = StartStream(1, stderr);
    state.active.store(out || err);
    std::atexit(stop_log);
    if (!out || !err) Append(1, "[diagnostics] Could not capture both standard output streams.\n");
}

void rocket::diagnostics::stop_log() {
    auto& state = State();
    if (!state.active.exchange(false)) return;
    // Restore both write ends before joining, so EOF drains even partial lines.
    for (unsigned i = 0; i < state.captures.size(); ++i) {
        auto& capture = state.captures[i];
        if (!capture.reader.joinable()) continue;
        std::fflush(capture.stream);
        DupTo(capture.original, Fileno(capture.stream));
#if defined(_WIN32)
        SetStdHandle(i == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,
                     reinterpret_cast<HANDLE>(_get_osfhandle(Fileno(capture.stream))));
#endif
    }
    for (auto& capture : state.captures) {
        if (!capture.reader.joinable()) continue;
        capture.reader.join();
        Close(capture.original);
        capture.original = -1;
    }
    std::lock_guard lock(state.file_mutex);
    state.file.close();
}

bool rocket::diagnostics::refresh_log(LogSnapshot& snapshot) {
    auto& state = State();
    std::unique_lock lock(state.history_mutex, std::try_to_lock);
    if (!lock || snapshot.revision == state.revision) return false;
    snapshot.lines.assign(state.lines.begin(), state.lines.end());
    for (const auto& line : state.partial) {
        if (!line.empty()) snapshot.lines.push_back(line);
    }
    snapshot.discarded_lines = state.discarded;
    snapshot.revision = state.revision;
    return true;
}

void rocket::diagnostics::clear_log() {
    auto& state = State();
    std::lock_guard lock(state.history_mutex);
    state.lines.clear();
    for (auto& line : state.partial) line.clear();
    state.bytes = 0;
    state.discarded = 0;
    ++state.revision;
}

std::filesystem::path rocket::diagnostics::log_path() { return State().path; }
bool rocket::diagnostics::log_active() { return State().active.load(); }
bool rocket::diagnostics::log_file_available() { return State().file_ok.load(); }
