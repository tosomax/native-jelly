// A stand-in for libplayerAPIs' stock whitelist. It is a separate TU from the caller, so the
// caller has no body to inline and the call is resolved through the PLT.
#include "mock.h"

#include <cstdio>

namespace smp {
namespace util {

bool isPlatformSupportSoftwareAudioDecoder(std::string platform) {
  std::fprintf(stderr, "[mock-stock] asked for '%s'\n", platform.c_str());
  return platform == "k24" || platform == "lm21ut" || platform == "o18";
}

}  // namespace util
}  // namespace smp
