#include <string_view>

#include <doctest/doctest.h>

#include "jarvis/node/build_info.hpp"

TEST_SUITE("unit") {
  TEST_CASE("build info identifies the build") {
    const jarvis::node::BuildInfo info = jarvis::node::build_info();
    CHECK_FALSE(info.version.empty());
    CHECK_FALSE(info.git_commit.empty());
    const bool known_compiler = info.compiler.starts_with("GNU ") ||
                                info.compiler.starts_with("Clang ") ||
                                info.compiler.starts_with("AppleClang ");
    CHECK(known_compiler);
  }
}
