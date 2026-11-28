// The libplayerAPIs contract, reproduced for a host test: the caller and the stock definition
// live in separate translation units, so the call goes through the PLT exactly as it does in the
// television's library.
#pragma once
#include <string>

namespace smp {
namespace util {
bool isPlatformSupportSoftwareAudioDecoder(std::string platform);
}  // namespace util
}  // namespace smp

bool mock_query(const char* platform);
