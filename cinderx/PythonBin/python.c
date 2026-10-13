// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <Python.h>

// The sanitizer builds that this caters for are Linux-only, and a weak
// undefined symbol has no Mach-O equivalent that ld64 will resolve at load
// time, so everything below is compiled out elsewhere.
#ifdef __linux__

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// In Meta's setup, ASAN_OPTIONS set in an environment variable are ignored.
// However, we want to respect the log_path we set via cinder_test_runner.py
// for use by sub-processes of individual tests.
__attribute__((weak)) void __sanitizer_set_report_fd(void*);

void open_asan_logfile(void) {
  if (__sanitizer_set_report_fd == NULL) {
    return;
  }

  char* asan_options = getenv("ASAN_OPTIONS");
  if (asan_options == NULL) {
    return;
  }

  char* tokenptr = NULL;
  char* token = strtok_r(asan_options, ",", &tokenptr);
  const char* log_path_str = "log_path=";
  const int log_path_len = strlen(log_path_str);
  while (token != NULL) {
    if (strncmp(token, log_path_str, log_path_len) == 0) {
      char* filename = token + log_path_len;
      int fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0666);
      // open() can reuse a closed stdin, stdout, or stderr descriptor, making
      // Python treat the log as that stream at startup. Move the log above
      // stderr and close the original so Python still sees the stream as
      // closed.
      if (fd >= 0 && fd <= STDERR_FILENO) {
        int report_fd = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
        close(fd);
        fd = report_fd;
      }
      if (fd >= 0) {
        __sanitizer_set_report_fd((void*)(uintptr_t)fd);
      }
      break;
    }
    token = strtok_r(NULL, ",", &tokenptr);
  }
}

#else

void open_asan_logfile(void) {}

#endif

int main(int argc, char** argv) {
  open_asan_logfile();
  return Py_BytesMain(argc, argv);
}
