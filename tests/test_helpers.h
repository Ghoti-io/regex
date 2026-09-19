/**
 * @file
 *
 * Shared helpers for the Ghoti.io Regex unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_TEST_HELPERS_H
#define GHOTI_IO_GRX_TEST_HELPERS_H

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ghoti.io/regex/regex.h>

namespace grxtest {

/**
 * Directory holding the checked-in fixtures. The Makefile bakes in
 * GRX_TEST_DATA so the binaries can run from the build tree; the environment
 * variable wins when it is set, and there is a relative fallback for a manual
 * build.
 */
inline std::string data_dir() {
  const char * env = std::getenv("GRX_TEST_DATA");
  if (env) {
    return std::string(env);
  }
#ifdef GRX_TEST_DATA
  return std::string(GRX_TEST_DATA);
#else
  return std::string("tests/data");
#endif
}

/** Path to a checked-in fixture. */
inline std::string data(const std::string & relative) {
  return data_dir() + "/" + relative;
}

/**
 * Run a dump function against a temporary file and return what it wrote.
 *
 * The dumps take a FILE * because that is what a C library can portably
 * write to; a test wants a string. tmpfile() rather than open_memstream()
 * because the former is C and the latter is POSIX, and nothing here is worth
 * a platform branch.
 */
template <typename Fn> inline std::string capture_dump(Fn && fn) {
  FILE * file = std::tmpfile();
  if (!file) {
    return std::string();
  }

  fn(file);
  std::fflush(file);

  long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    return std::string();
  }
  std::rewind(file);

  std::vector<char> buffer(static_cast<size_t>(size) + 1, '\0');
  size_t read = std::fread(buffer.data(), 1, static_cast<size_t>(size), file);
  std::fclose(file);

  return std::string(buffer.data(), read);
}

/**
 * An allocator that counts what it hands out, so a test can state that the
 * library allocated nothing the caller was not told about and freed
 * everything it did.
 *
 * It is cutil's vtable, so the same object works for every library in the
 * suite.
 */
class CountingAllocator {
public:
  CountingAllocator() {
    vtable_.ctx = this;
    vtable_.malloc_fn = [](void * ctx, size_t size) -> void * {
      auto * self = static_cast<CountingAllocator *>(ctx);
      void * p = std::malloc(size ? size : 1);
      if (p) {
        self->live_++;
        self->total_++;
      }
      return p;
    };
    vtable_.calloc_fn = [](void * ctx, size_t nitems, size_t size) -> void * {
      auto * self = static_cast<CountingAllocator *>(ctx);
      // Overflow is an allocation failure, not a truncated block; the suite's
      // allocator contract requires it (CONVENTIONS.md section 5).
      if (nitems && size && nitems > (size_t)-1 / size) {
        return nullptr;
      }
      void * p = std::calloc(nitems ? nitems : 1, size ? size : 1);
      if (p) {
        self->live_++;
        self->total_++;
      }
      return p;
    };
    vtable_.realloc_fn = [](void * ctx, void * ptr, size_t size) -> void * {
      auto * self = static_cast<CountingAllocator *>(ctx);
      void * p = std::realloc(ptr, size ? size : 1);
      if (p && !ptr) {
        self->live_++;
        self->total_++;
      }
      return p;
    };
    vtable_.free_fn = [](void * ctx, void * ptr) {
      auto * self = static_cast<CountingAllocator *>(ctx);
      if (ptr) {
        self->live_--;
      }
      std::free(ptr);
    };
  }

  const GRX_Allocator * get() const { return &vtable_; }

  /** Allocations made and not yet freed. */
  long live() const { return live_; }

  /** Allocations made in total. */
  long total() const { return total_; }

private:
  GRX_Allocator vtable_ {};
  long live_ = 0;
  long total_ = 0;
};

} // namespace grxtest

#endif // GHOTI_IO_GRX_TEST_HELPERS_H
