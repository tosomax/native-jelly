// The libplayerAPIs contract, reproduced for a host test: the caller and the stock definition
// live in separate shared objects, so the call goes through a PLT slot exactly as it does in the
// television's library.
#pragma once
#include <string>

namespace smp {
namespace util {
bool isPlatformSupportSoftwareAudioDecoder(std::string platform);
}  // namespace util
}  // namespace smp

bool mock_query(const char* platform);
