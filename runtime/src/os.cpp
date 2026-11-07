// The parts of ps2/os.hpp that need the system's own headers, kept out of the header so that every file that includes it does
// not get windows.h too.
#include "ps2/os.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <cstdio>

namespace ps2::os {

namespace {

#ifdef _WIN32
std::string system_message(DWORD code) {
    wchar_t *text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    std::string message = text ? utf8(text) : "error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) message.pop_back();
    return message;
}
#endif

}

bool move_replacing(const std::string &from, const std::string &to, std::string *why) {
#ifdef _WIN32
    // MOVEFILE_WRITE_THROUGH: the call returns once the move is on the disk, not just in the system's cache
    if (MoveFileExW(path(from).c_str(), path(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    if (why) *why = system_message(GetLastError());
    return false;
#else
    std::error_code error;
    std::filesystem::rename(path(from), path(to), error);
    if (error) {
        if (why) *why = error.message();
        return false;
    }
#ifndef __EMSCRIPTEN__
    // the new name is part of the folder, so the folder is synced too (a file's own fsync does not cover its name)
    std::filesystem::path folder = path(to).parent_path();
    if (folder.empty()) folder = ".";
    const int fd = ::open(folder.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        ::close(fd);
    }
#endif
    return true;
#endif
}

bool FileLock::take(const std::string &file, bool &busy, std::string &why) {
    release();
    busy = false;
#ifdef _WIN32
    const HANDLE h = CreateFileW(path(file).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        busy = code == ERROR_SHARING_VIOLATION;
        why = system_message(code);
        return false;
    }
    handle_ = reinterpret_cast<std::intptr_t>(h);
    return true;
#elif defined(__EMSCRIPTEN__)
    (void)file;
    (void)why;
    handle_ = 0;  // one page, one run: nothing to lock against
    return true;
#else
    const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        why = std::strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        busy = errno == EWOULDBLOCK;
        why = std::strerror(errno);
        ::close(fd);
        return false;
    }
    const std::string text = std::to_string(getpid()) + "\n";
    if (ftruncate(fd, 0) == 0 && ::write(fd, text.data(), text.size()) < 0) {}
    handle_ = fd;
    return true;
#endif
}

void FileLock::release() {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#elif !defined(__EMSCRIPTEN__)
    ::close(static_cast<int>(handle_));
#endif
    handle_ = -1;
}

}
