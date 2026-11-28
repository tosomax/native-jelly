#include "mock.h"

#include <string>

bool mock_query(const char* platform) {
  std::string s(platform);
  return smp::util::isPlatformSupportSoftwareAudioDecoder(s);
}
