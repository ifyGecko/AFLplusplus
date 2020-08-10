//===- afl_driver.cpp - a glue between AFL and libFuzzer --------*- C++ -* ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

/* This file allows to fuzz libFuzzer-style target functions
 (LLVMFuzzerTestOneInput) with AFL using AFL's persistent (in-process) mode.

Usage:
################################################################################
cat << EOF > test_fuzzer.cc
#include <stddef.h>
#include <stdint.h>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

  if (size > 0 && data[0] == 'H')
    if (size > 1 && data[1] == 'I')
       if (size > 2 && data[2] == '!')
       __builtin_trap();
  return 0;

}

EOF
# Build your target with -fsanitize-coverage=trace-pc-guard using fresh clang.
clang -g -fsanitize-coverage=trace-pc-guard test_fuzzer.cc -c
# Build afl-llvm-rt.o.c from the AFL distribution.
clang -c -w $AFL_HOME/llvm_mode/afl-llvm-rt.o.c
# Build this file, link it with afl-llvm-rt.o.o and the target code.
clang++ afl_driver.cpp test_fuzzer.o afl-llvm-rt.o.o
# Run AFL:
rm -rf IN OUT; mkdir IN OUT; echo z > IN/z;
$AFL_HOME/afl-fuzz -i IN -o OUT ./a.out
################################################################################
AFL_DRIVER_STDERR_DUPLICATE_FILENAME: Setting this *appends* stderr to the file
specified. If the file does not exist, it is created. This is useful for getting
stack traces (when using ASAN for example) or original error messages on hard
to reproduce bugs. Note that any content written to stderr will be written to
this file instead of stderr's usual location.

AFL_DRIVER_CLOSE_FD_MASK: Similar to libFuzzer's -close_fd_mask behavior option.
If 1, close stdout at startup. If 2 close stderr; if 3 close both.

*/
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "config.h"
#include "cmplog.h"

#ifdef _DEBUG
  #include "hash.h"
#endif

#ifndef MAP_FIXED_NOREPLACE
  #define MAP_FIXED_NOREPLACE 0x100000
#endif

#define MAX_DUMMY_SIZE 256000

// Platform detection. Copied from FuzzerInternal.h
#ifdef __linux__
  #define LIBFUZZER_LINUX 1
  #define LIBFUZZER_APPLE 0
  #define LIBFUZZER_NETBSD 0
  #define LIBFUZZER_FREEBSD 0
  #define LIBFUZZER_OPENBSD 0
#elif __APPLE__
  #define LIBFUZZER_LINUX 0
  #define LIBFUZZER_APPLE 1
  #define LIBFUZZER_NETBSD 0
  #define LIBFUZZER_FREEBSD 0
  #define LIBFUZZER_OPENBSD 0
#elif __NetBSD__
  #define LIBFUZZER_LINUX 0
  #define LIBFUZZER_APPLE 0
  #define LIBFUZZER_NETBSD 1
  #define LIBFUZZER_FREEBSD 0
  #define LIBFUZZER_OPENBSD 0
#elif __FreeBSD__
  #define LIBFUZZER_LINUX 0
  #define LIBFUZZER_APPLE 0
  #define LIBFUZZER_NETBSD 0
  #define LIBFUZZER_FREEBSD 1
  #define LIBFUZZER_OPENBSD 0
#elif __OpenBSD__
  #define LIBFUZZER_LINUX 0
  #define LIBFUZZER_APPLE 0
  #define LIBFUZZER_NETBSD 0
  #define LIBFUZZER_FREEBSD 0
  #define LIBFUZZER_OPENBSD 1
#else
  #error "Support for your platform has not been implemented"
#endif

int                   __afl_sharedmem_fuzzing = 0;
extern unsigned char *__afl_area_ptr;
// extern struct cmp_map *__afl_cmp_map;

// libFuzzer interface is thin, so we don't include any libFuzzer headers.
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);
__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv);

// Notify AFL about persistent mode.
static volatile char AFL_PERSISTENT[] = "##SIG_AFL_PERSISTENT##";
int                  __afl_persistent_loop(unsigned int);

// Notify AFL about deferred forkserver.
static volatile char AFL_DEFER_FORKSVR[] = "##SIG_AFL_DEFER_FORKSRV##";
void                 __afl_manual_init();

// Use this optionally defined function to output sanitizer messages even if
// user asks to close stderr.
__attribute__((weak)) void __sanitizer_set_report_fd(void *);

// Keep track of where stderr content is being written to, so that
// dup_and_close_stderr can use the correct one.
static FILE *output_file;

// Experimental feature to use afl_driver without AFL's deferred mode.
// Needs to run before __afl_auto_init.
__attribute__((constructor(0))) static void __decide_deferred_forkserver(void) {

  if (getenv("AFL_DRIVER_DONT_DEFER")) {

    if (unsetenv("__AFL_DEFER_FORKSRV")) {

      perror("Failed to unset __AFL_DEFER_FORKSRV");
      abort();

    }

  }

}

// If the user asks us to duplicate stderr, then do it.
static void maybe_duplicate_stderr() {

  char *stderr_duplicate_filename =
      getenv("AFL_DRIVER_STDERR_DUPLICATE_FILENAME");

  if (!stderr_duplicate_filename) return;

  FILE *stderr_duplicate_stream =
      freopen(stderr_duplicate_filename, "a+", stderr);

  if (!stderr_duplicate_stream) {

    fprintf(
        stderr,
        "Failed to duplicate stderr to AFL_DRIVER_STDERR_DUPLICATE_FILENAME");
    abort();

  }

  output_file = stderr_duplicate_stream;

}

// Most of these I/O functions were inspired by/copied from libFuzzer's code.
static void discard_output(int fd) {

  FILE *temp = fopen("/dev/null", "w");
  if (!temp) abort();
  dup2(fileno(temp), fd);
  fclose(temp);

}

static void close_stdout() {

  discard_output(STDOUT_FILENO);

}

// Prevent the targeted code from writing to "stderr" but allow sanitizers and
// this driver to do so.
static void dup_and_close_stderr() {

  int output_fileno = fileno(output_file);
  int output_fd = dup(output_fileno);
  if (output_fd <= 0) abort();
  FILE *new_output_file = fdopen(output_fd, "w");
  if (!new_output_file) abort();
  if (!__sanitizer_set_report_fd) return;
  __sanitizer_set_report_fd((void *)output_fd);
  discard_output(output_fileno);

}

// Close stdout and/or stderr if user asks for it.
static void maybe_close_fd_mask() {

  char *fd_mask_str = getenv("AFL_DRIVER_CLOSE_FD_MASK");
  if (!fd_mask_str) return;
  int fd_mask = atoi(fd_mask_str);
  if (fd_mask & 2) dup_and_close_stderr();
  if (fd_mask & 1) close_stdout();

}

// Define LLVMFuzzerMutate to avoid link failures for targets that use it
// with libFuzzer's LLVMFuzzerCustomMutator.
size_t LLVMFuzzerMutate(uint8_t *Data, size_t Size, size_t MaxSize) {

  // assert(false && "LLVMFuzzerMutate should not be called from afl_driver");
  return 0;

}

// Execute any files provided as parameters.
static int ExecuteFilesOnyByOne(int argc, char **argv) {

  unsigned char *buf = malloc(MAX_FILE);
  for (int i = 1; i < argc; i++) {

    int fd = open(argv[i], O_RDONLY);
    if (fd == -1) continue;
    ssize_t length = read(fd, buf, MAX_FILE);
    if (length > 0) {

      printf("Reading %zu bytes from %s\n", length, argv[i]);
      LLVMFuzzerTestOneInput(buf, length);
      printf("Execution successful.\n");

    }

  }

  free(buf);
  return 0;

}

__attribute__((constructor(1))) void __afl_protect(void) {

  setenv("__AFL_DEFER_FORKSRV", "1", 1);
  __afl_area_ptr = (unsigned char *)mmap(
      (void *)0x10000, MAX_DUMMY_SIZE, PROT_READ | PROT_WRITE,
      MAP_FIXED_NOREPLACE | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if ((uint64_t)__afl_area_ptr == -1)
    __afl_area_ptr = (unsigned char *)mmap((void *)0x10000, MAX_DUMMY_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if ((uint64_t)__afl_area_ptr == -1)
    __afl_area_ptr =
        (unsigned char *)mmap(NULL, MAX_DUMMY_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // __afl_cmp_map = (struct cmp_map *)__afl_area_ptr;

}

int main(int argc, char **argv) {

  fprintf(stderr, "dummy map is at %p\n", __afl_area_ptr);

  printf(
      "======================= INFO =========================\n"
      "This binary is built for afl++.\n"
      "To run the target function on individual input(s) execute this:\n"
      "  %s INPUT_FILE1 [INPUT_FILE2 ... ]\n"
      "To fuzz with afl-fuzz execute this:\n"
      "  afl-fuzz [afl-flags] -- %s [-N]\n"
      "afl-fuzz will run N iterations before re-spawning the process (default: "
      "1000)\n"
      "======================================================\n",
      argv[0], argv[0]);

  output_file = stderr;
  maybe_duplicate_stderr();
  maybe_close_fd_mask();
  if (LLVMFuzzerInitialize) {

    fprintf(stderr, "Running LLVMFuzzerInitialize ...\n");
    LLVMFuzzerInitialize(&argc, &argv);
    fprintf(stderr, "continue...\n");

  }

  // Do any other expensive one-time initialization here.

  uint8_t dummy_input[64] = {0};
  uint8_t buf[1024000];
  memcpy(dummy_input, (void *)AFL_PERSISTENT, sizeof(AFL_PERSISTENT));
  memcpy(dummy_input + 32, (void *)AFL_DEFER_FORKSVR,
         sizeof(AFL_DEFER_FORKSVR));
  int N = INT_MAX;
  if (argc == 2 && argv[1][0] == '-')
    N = atoi(argv[1] + 1);
  else if (argc == 2 && (N = atoi(argv[1])) > 0)
    printf("WARNING: using the deprecated call style `%s %d`\n", argv[0], N);
  else if (argc > 1) {

    __afl_sharedmem_fuzzing = 0;
    munmap(__afl_area_ptr, MAX_DUMMY_SIZE);  // we need to free 0x10000
    __afl_area_ptr = NULL;
    __afl_manual_init();
    return ExecuteFilesOnyByOne(argc, argv);

  }

  assert(N > 0);

  if (!getenv("AFL_DISABLE_LLVM_INSTRUMENTATION")) {
    munmap(__afl_area_ptr, MAX_DUMMY_SIZE);
    __afl_area_ptr = NULL;
    __afl_manual_init();
  }
  fprintf(stderr, "dummy map is now at %p\n", __afl_area_ptr);

  // Call LLVMFuzzerTestOneInput here so that coverage caused by initialization
  // on the first execution of LLVMFuzzerTestOneInput is ignored.
  LLVMFuzzerTestOneInput(dummy_input, 1);

  int num_runs = 0;
  while (__afl_persistent_loop(N)) {

    ssize_t r = read(0, buf, sizeof(buf));

    if (r > 0) {

      LLVMFuzzerTestOneInput(buf, r);

    }

  }

  printf("%s: successfully executed input(s)\n", argv[0]);

}

