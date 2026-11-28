// platformfix.cpp — EXPERIMENT (`swdec`, o24n): an app-scoped interposer for
//   smp::util::isPlatformSupportSoftwareAudioDecoder(std::string)
//
// The G5's board is `o24n` (O24N_DVB_EU). LG's compiled whitelist in libplayerAPIs does not list
// it, so this function answers false and the pipeline never emits
// `option.useSoftwareAudioDecoder` — and never converts the ADEC to PCMMC — no matter what the
// Load payload asks for. The DTS Load then still requests two ADECs from the Resource Manager and
// is refused with `601 Resource Allocation Error`.
//
// This object makes the answer true for `o24n` ONLY, inside THIS process only. For every other
// string it delegates to the original via dlsym(RTLD_NEXT); if the delegation is impossible it
// answers from a copy of LG's stock whitelist, so a platform that was already admitted stays
// admitted and nothing is widened by accident.
//
// Why LD_PRELOAD at exec, and not dlopen later: in libplayerAPIs the symbol is
//   - STB_GLOBAL / STV_DEFAULT / STT_FUNC, NOT versioned (.gnu.version = 1)
//   - called from 4 sites, all through the PLT: preemptible
//   - the library is not linked -Bsymbolic (no DF_SYMBOLIC)
// but it IS linked -z now (DT_FLAGS_1 = NOW), so its GOT is resolved when the library loads. A
// dlopen() after startup cannot rebind those slots. The object must therefore be present in the
// process's LD_PRELOAD at exec. The app does that itself, once, in
// `rust-modules/base/src/platformfix.rs` (`maybe_reexec`), only when the `nativejelly-swdec`
// trigger arms the debug install. Nothing is written to /etc/ld.so.preload and nothing is
// installed system-wide.
//
// ABI: std::string passed BY VALUE (the mangled name has no "RK"). Build with
// -D_GLIBCXX_USE_CXX11_ABI=1 (the TV's libstdc++ is cxx11, GLIBCXX_3.4.21+). With the old ABI the
// mangled name differs, this definition is never called, and nothing crashes — a safe failure.
//
// Two safety gates beyond the trigger: the override applies only when the re-exec marker
// `NJ_PLATFORMFIX_ACTIVE` is exactly `1` (so an accidental system-wide preload is inert), matching
// Rust's anti-loop contract, and `NJ_PLATFORMFIX=0` disables it. `NJ_PLATFORMFIX_DEBUG=1` prints one
// line per call to stderr. The marker is forgeable and is not an authentication mechanism.

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace smp {
namespace util {
bool isPlatformSupportSoftwareAudioDecoder(std::string platform);
}  // namespace util
}  // namespace smp

namespace {

constexpr char kMangled[] =
    "_ZN3smp4util37isPlatformSupportSoftwareAudioDecoderENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE";

constexpr char kTarget[] = "o24n";  // board O24N_DVB_EU (exact match, lowercase)

// A copy of the SoC whitelist read from libplayerAPIs (0x72ae1, 584 B). It is used ONLY as the
// fallback when dlsym(RTLD_NEXT) cannot reach the original.
const char* const kStockWhitelist[] = {
    "m16p",  "m16plite", "m16pstb", "m16pbno", "m16pp",  "m16pplite",
    "m16ppez", "m16p3",  "o18",     "o18k",    "o20",    "o208k",
    "o20n",  "e60",      "e60n",    "k3l",     "k3lp",   "k4lp",
    "k5lp",  "k5ap",     "k6lp",    "k6hp",    "k6ap",   "m3",
    "lm18a", "m3r",      "lm19a",   "lm20a",   "lm20u",  "lm21ut",
    "k8hpt", "k8hppt",   "k8ap",    "kf23f",   "fhd",    "k8hp",
    "k24",   "k8lpn",    nullptr};

bool env_is_set_nonzero(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

bool env_is_exact(const char* name, const char* expected) {
  const char* v = std::getenv(name);
  return v != nullptr && std::strcmp(v, expected) == 0;
}

// `NJ_PLATFORMFIX=0` -> everything stock.
bool disabled_by_env() {
  const char* v = std::getenv("NJ_PLATFORMFIX");
  return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") == 0;
}

// Only the process the app re-exec'd with our library may answer `true`. An accidental preload
// into some other process stays stock.
bool override_armed() { return env_is_exact("NJ_PLATFORMFIX_ACTIVE", "1"); }

bool is_target(const std::string& p) {
  return p.size() == sizeof(kTarget) - 1 && std::memcmp(p.data(), kTarget, p.size()) == 0;
}

bool in_stock_whitelist(const std::string& p) {
  for (const char* const* it = kStockWhitelist; *it != nullptr; ++it) {
    if (p == *it) return true;
  }
  return p == "com.webos.app.mediadiscovery" || p == "null";
}

using fn_t = bool (*)(std::string);

fn_t real_fn() {
  static fn_t fn = []() -> fn_t {
    void* p = ::dlsym(RTLD_NEXT, kMangled);
    if (p == nullptr) return nullptr;
    if (p == reinterpret_cast<void*>(
                 &::smp::util::isPlatformSupportSoftwareAudioDecoder)) {
      return nullptr;  // defence: never recurse into ourselves
    }
    return reinterpret_cast<fn_t>(p);
  }();
  return fn;
}

}  // namespace

namespace smp {
namespace util {

__attribute__((visibility("default"))) bool
isPlatformSupportSoftwareAudioDecoder(std::string platform) {
  const bool disabled = ::disabled_by_env();
  const bool armed = !disabled && ::override_armed();
  const bool target = armed && ::is_target(platform);
  if (::env_is_set_nonzero("NJ_PLATFORMFIX_DEBUG")) {
    std::fprintf(stderr, "[platformfix] '%s' -> %s\n", platform.c_str(),
                 target ? "true (o24n override)"
                        : (disabled ? "stock (NJ_PLATFORMFIX=0)"
                                    : (armed ? "delegate" : "stock (not re-exec'd)")));
  }
  if (target) return true;

  fn_t f = ::real_fn();
  if (f != nullptr) return f(platform);

  // Safe fallback: replicate the stock whitelist (+ the no-argument case).
  return ::in_stock_whitelist(platform);
}

}  // namespace util
}  // namespace smp
