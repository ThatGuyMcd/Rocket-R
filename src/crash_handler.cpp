#include "crash_handler.hpp"
#include "runtime_log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <ctime>
#endif

namespace {
std::filesystem::path g_log_directory;

#if defined(_WIN32)
std::string Timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

LONG WINAPI RocketUnhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    const std::string stamp = Timestamp();
    const auto dump_path = g_log_directory / ("crash-" + stamp + ".dmp");
    const auto text_path = g_log_directory / ("crash-" + stamp + ".txt");

    DWORD code = 0;
    void* address = nullptr;
    if (exception != nullptr && exception->ExceptionRecord != nullptr) {
        code = exception->ExceptionRecord->ExceptionCode;
        address = exception->ExceptionRecord->ExceptionAddress;
    }

    {
        std::ofstream report(text_path, std::ios::trunc);
        if (report) {
            report << "Rocket-R unhandled exception\n";
            report << "Exception code: 0x" << std::hex << code << "\n";
            report << "Exception address: " << address << "\n";
            report << "Minidump: " << dump_path.string() << "\n";
        }
    }

    HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = exception;
        info.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(
                              MiniDumpWithThreadInfo |
                              MiniDumpWithIndirectlyReferencedMemory),
                          exception != nullptr ? &info : nullptr,
                          nullptr, nullptr);
        CloseHandle(file);
    }

    std::fprintf(stderr,
                 "[crash] unhandled Windows exception 0x%08lX at %p\n"
                 "[crash] report: %s\n[crash] minidump: %s\n",
                 static_cast<unsigned long>(code), address,
                 text_path.string().c_str(), dump_path.string().c_str());
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
}

void rocket::diagnostics::install(const std::filesystem::path& config_directory) {
    g_log_directory = config_directory / "logs";
    std::error_code ec;
    std::filesystem::create_directories(g_log_directory, ec);
    start_log(g_log_directory);
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(RocketUnhandledExceptionFilter);
#endif
}
