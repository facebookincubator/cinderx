// Test runner for ARM64 branch patching tests.

#include <asmjit/core.h>
#include "broken.h"

int main(int argc, const char* argv[]) {
  printf("AsmJit ARM64 Patching Test-Suite v%u.%u.%u\n\n",
    unsigned((ASMJIT_LIBRARY_VERSION >> 16)       ),
    unsigned((ASMJIT_LIBRARY_VERSION >>  8) & 0xFF),
    unsigned((ASMJIT_LIBRARY_VERSION      ) & 0xFF));

  return BrokenAPI::run(argc, argv);
}
