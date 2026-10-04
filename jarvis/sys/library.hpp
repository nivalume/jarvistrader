#pragma once

#include <string>

// Shared libraries loaded at run time (strategy plugins, docs/architecture.md section 7.3):
// dlopen(RTLD_NOW | RTLD_LOCAL) on Linux and macOS, LoadLibraryExW on Windows (the library's
// own directory is searched for its dependencies).

namespace jarvis::sys {

// The library at `path`, or nullptr with the reason in `error`.
[[nodiscard]] void* load_library(const std::string& path, std::string& error);
// The address of `name` in `library`, or nullptr.
[[nodiscard]] void* library_symbol(void* library, const char* name) noexcept;
void unload_library(void* library) noexcept;

} // namespace jarvis::sys

// What a library exports for the loader to find (with the other symbols hidden).
#if defined(_WIN32)
#define JARVIS_EXPORT __declspec(dllexport) // NOLINT(cppcoreguidelines-macro-usage)
#else
#define JARVIS_EXPORT                                                                              \
  __attribute__((visibility("default"))) // NOLINT(cppcoreguidelines-macro-usage)
#endif
