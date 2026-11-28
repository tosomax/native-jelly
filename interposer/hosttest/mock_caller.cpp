// Built into its own DSO: the stock definition is not linked in, so the interposed symbol stays
// UND in this object's .dynsym with a JUMP_SLOT relocation, like libplayerAPIs' call sites.
#include "mock.h"

#include <string>

bool mock_query(const char* platform) {
  std::string s(platform);
  return smp::util::isPlatformSupportSoftwareAudioDecoder(s);
}
