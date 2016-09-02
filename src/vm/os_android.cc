// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"
#if defined(TARGET_OS_ANDROID)

#include "vm/os.h"

#include <android/log.h>  // NOLINT
#include <errno.h>  // NOLINT
#include <time.h>  // NOLINT
#include <sys/time.h>  // NOLINT
#include <sys/types.h>  // NOLINT
#include <unistd.h>  // NOLINT

#include "vm/assert.h"

namespace psoup {

void OS::InitOnce() {
}


int64_t OS::CurrentMonotonicMicros() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    UNREACHABLE();
    return 0;
  }
  // Convert to nanoseconds.
  int64_t result = ts.tv_sec;
  result *= kNanosecondsPerSecond;
  result += ts.tv_nsec;

  return result / kNanosecondsPerMicrosecond;
}


int64_t OS::CurrentMonotonicMillis() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    UNREACHABLE();
    return 0;
  }
  // Convert to nanoseconds.
  int64_t result = ts.tv_sec;
  result *= kNanosecondsPerSecond;
  result += ts.tv_nsec;

  return result / kNanosecondsPerMillisecond;
}


int OS::NumberOfAvailableProcessors() {
  return sysconf(_SC_NPROCESSORS_ONLN);
}


void OS::DebugBreak() {
  __builtin_trap();
}


static void VFPrint(FILE* stream, const char* format, va_list args) {
  vfprintf(stream, format, args);
  fflush(stream);
}


void OS::Print(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VFPrint(stdout, format, args);
  // Forward to the Android log for remote access.
  __android_log_vprint(ANDROID_LOG_INFO, "PrimordialSoup", format, args);
  va_end(args);
}


void OS::PrintErr(const char* format, ...) {
  va_list args;
  va_start(args, format);
  VFPrint(stderr, format, args);
  // Forward to the Android log for remote access.
  __android_log_vprint(ANDROID_LOG_ERROR, "PrimordialSoup", format, args);
  va_end(args);
}


void OS::Abort() {
  abort();
}


void OS::Exit(int code) {
  exit(code);
}

}  // namespace psoup

#endif  // defined(TARGET_OS_ANDROID)