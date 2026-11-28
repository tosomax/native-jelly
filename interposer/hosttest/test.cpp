// Host test (x86_64) for the interposer mechanism, not for the television's ABI. Run with
// `run.sh`, which builds the real interposer from ../platformfix.cpp and sets the same env the
// app's one re-exec sets.
#include <cstdio>
#include <cstdlib>

#include "mock.h"

static int failures = 0;

static void expect(const char* platform, bool want, const char* why) {
  const bool got = mock_query(platform);
  if (got != want) ++failures;
  std::printf("%-4s %-7s got=%-5s want=%-5s  %s\n", got == want ? "PASS" : "FAIL", platform,
              got ? "true" : "false", want ? "true" : "false", why);
}

int main() {
  const char* armed = std::getenv("NJ_PLATFORMFIX_ACTIVE");
  const bool over = armed != nullptr && std::string(armed) == "1";
  const char* kill = std::getenv("NJ_PLATFORMFIX");
  const bool stock = kill != nullptr && std::string(kill) == "0";

  if (over && !stock) {
    expect("o24n", true, "target: o24n override");
    expect("O24N", false, "exact match: uppercase is not o24n");
  } else {
    expect("o24n", false, "no override without the re-exec marker / with the kill switch");
  }
  expect("k24", true, "delegation: already in the stock whitelist");
  expect("lm21ut", true, "delegation: already in the stock whitelist");
  expect("o18", true, "delegation: already in the stock whitelist");
  expect("o24x", false, "non-target stays false");
  expect("k24n", false, "non-target stays false");
  expect("", false, "empty string");
  std::printf(failures ? "\n%d FAILURE(S)\n" : "\nall good\n", failures);
  return failures ? 1 : 0;
}
