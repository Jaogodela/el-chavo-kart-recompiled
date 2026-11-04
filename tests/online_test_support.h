#pragma once

#include <cstdio>
#include <string>

namespace online_test {

inline int g_failures = 0;
inline int g_checks = 0;

inline void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

inline int Finish() {
  std::printf("%s (%d checks, %d failed)\n", g_failures ? "FAILED" : "passed", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

}  // namespace online_test
