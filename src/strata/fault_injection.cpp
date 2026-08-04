#include "strata/fault_injection.hpp"

#include <signal.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace strata {

void MaybeCrash(const char* point) {
  const char* target = std::getenv("STRATA_CRASH_AT");
  if (target == nullptr || std::strcmp(target, point) != 0) return;

  ::kill(::getpid(), SIGKILL);
  for (;;) {
  }  // unreachable: SIGKILL can't be caught, blocked, or ignored
}

}  // namespace strata
