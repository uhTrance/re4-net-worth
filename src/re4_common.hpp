// Shared helpers for the RE4 REFramework plugins.
//
// Both plugins need the same handful of things: logging into REFramework's log,
// pulling UTF-8 strings out of managed System.String objects, escaping JSON,
// and writing files without a reader ever seeing a half-written one. Keeping
// them here rather than copy-pasting means a fix (or a corrected memory offset)
// lands in both places at once.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include <windows.h>

#include "reframework/API.hpp"

namespace re4 {

// ---------------------------------------------------------------------------
// Verified memory layouts
//
// The REFramework C++ API exposes no accessors for these, so they are read
// directly. None of it is guessed: each offset below was confirmed against the
// live game by cross-checking raw reads against what the reflection API and the
// Lua binding independently reported for the same objects. Anything relying on
// them should still validate at runtime, because a game update could move them
// and reading at a wrong offset is a hard crash rather than a catchable error.
// ---------------------------------------------------------------------------

// Managed arrays (both reference-type and value-type element arrays).
constexpr uintptr_t ARRAY_COUNT_OFFSET = 0x1C;
constexpr uintptr_t ARRAY_DATA_OFFSET = 0x20;

// System.String: character count, then UTF-16 data.
constexpr uintptr_t STRING_LENGTH_OFFSET = 0x10;
constexpr uintptr_t STRING_CHARS_OFFSET = 0x14;

// System.Collections.Generic.Dictionary<,>.Entry, as laid out inside the
// dictionary's private _entries array. Element stride is the entry's
// valuetype_size (24 for the dictionaries used here); 'value' sits at this
// offset within each entry's data.
constexpr uintptr_t DICT_ENTRY_STRIDE = 24;
constexpr uintptr_t DICT_ENTRY_VALUE_OFFSET = 0x10;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
inline const REFrameworkPluginFunctions* g_functions = nullptr;
inline const char* g_log_prefix = "re4_plugin";

inline void set_functions(const REFrameworkPluginFunctions* fns, const char* prefix) {
    g_functions = fns;
    g_log_prefix = prefix;
}

inline void log_info(const char* fmt, ...) {
    if (g_functions == nullptr || g_functions->log_info == nullptr) return;
    char msg[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    char out[600];
    std::snprintf(out, sizeof(out), "[%s] %s", g_log_prefix, msg);
    g_functions->log_info("%s", out);
}

inline void log_error(const char* fmt, ...) {
    if (g_functions == nullptr || g_functions->log_error == nullptr) return;
    char msg[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    char out[600];
    std::snprintf(out, sizeof(out), "[%s] %s", g_log_prefix, msg);
    g_functions->log_error("%s", out);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// Managed System.String -> UTF-8. Item and challenge text legitimately contains
// non-ASCII ("Méndez's False Eye", "attaché case"), so this is a real
// conversion rather than a narrowing cast.
inline std::string managed_string_to_utf8(reframework::API::ManagedObject* str_obj) {
    if (str_obj == nullptr) return {};

    const auto base = (uintptr_t)str_obj;
    const auto len = *(uint32_t*)(base + STRING_LENGTH_OFFSET);
    if (len == 0 || len > 8192) return {}; // implausible: refuse rather than read wild memory

    const auto* chars = (const wchar_t*)(base + STRING_CHARS_OFFSET);
    const auto needed = WideCharToMultiByte(CP_UTF8, 0, chars, (int)len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, chars, (int)len, out.data(), needed, nullptr, nullptr);
    return out;
}

inline void json_escape_into(std::string& out, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back((char)c); // UTF-8 passes through untouched
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// Writes via a temp file then moves it into place, so a reader (the data
// server, or an overlay's fetch) can never observe a partially written file.
inline bool write_file_atomic(const std::string& path, const std::string& content) {
    const auto tmp = path + ".tmp";

    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);

    if (!ok || written != content.size()) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

inline bool append_file(const std::string& path, const std::string& line) {
    HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
    return true;
}

inline std::string read_file(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
        CloseHandle(h);
        return {};
    }

    std::string out((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    out.resize(read);
    return out;
}

// reframework/data/, resolved from the game executable's own location so this
// works regardless of where the game is installed.
inline std::string resolve_data_dir() {
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string dir(exe_path);
    const auto slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
    return dir + "\\reframework\\data\\";
}

} // namespace re4
