#include "jarvis/sys/library.hpp"

#include <filesystem>

#include "jarvis/sys/error.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace jarvis::sys {

#if defined(_WIN32)

void* load_library(const std::string& path, std::string& error) {
  // A path with a directory loads that file, and the DLLs next to it come first in the search
  // for its own imports (the flag needs an absolute path). A bare name is searched for the way
  // the system searches for any DLL, as dlopen does on POSIX.
  std::filesystem::path native{path};
  DWORD flags = 0;
  if (native.has_parent_path()) {
    native = std::filesystem::absolute(native);
    flags = LOAD_WITH_ALTERED_SEARCH_PATH;
  }
  HMODULE library = ::LoadLibraryExW(native.c_str(), nullptr, flags);
  if (library == nullptr) {
    error = last_system_error_text();
    return nullptr;
  }
  return library;
}

void* library_symbol(void* library, const char* name) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a function's address as void*
  return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library), name));
}

void unload_library(void* library) noexcept {
  if (library != nullptr) {
    ::FreeLibrary(static_cast<HMODULE>(library));
  }
}

#else

void* load_library(const std::string& path, std::string& error) {
  // RTLD_LOCAL: the library's copies of the jarvis code stay its own.
  void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    const char* why = ::dlerror(); // NOLINT(concurrency-mt-unsafe): plugins load at startup
    error = why != nullptr ? why : "unknown error";
  }
  return library;
}

void* library_symbol(void* library, const char* name) noexcept { return ::dlsym(library, name); }

void unload_library(void* library) noexcept {
  if (library != nullptr) {
    ::dlclose(library);
  }
}

#endif

} // namespace jarvis::sys
