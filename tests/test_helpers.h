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

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>
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

/**
 * Join two path components with the host's separator.
 *
 * cutil's, rather than `a + "/" + b`. A forward slash does reach the right
 * file on Windows, so this is not a bug being fixed - but these paths are
 * printed in failure messages, and a path that is half `\\` and half `/` is a
 * path a reader stops to look at instead of reading past.
 */
inline std::string path_join(const std::string & base,
    const std::string & relative) {
  size_t length = 0;
  if (gcu_path_join(GCU_PATH_NATIVE, base.c_str(), relative.c_str(), nullptr,
          0, &length) != GCU_PATH_OK) {
    return relative;
  }
  std::string out(length, '\0');
  if (gcu_path_join(GCU_PATH_NATIVE, base.c_str(), relative.c_str(), &out[0],
          length + 1, nullptr) != GCU_PATH_OK) {
    return relative;
  }
  return out;
}

/** Path to a checked-in fixture. */
inline std::string data(const std::string & relative) {
  return path_join(data_dir(), relative);
}

/**
 * Path to a file in the repository, for the tests that check a document
 * against the code it describes.
 *
 * Baked in at compile time the same way the fixture directory is, because a
 * test binary runs from the build tree and has no other way to find the
 * source it came from.
 */
inline std::string repo(const std::string & relative) {
#ifdef GRX_REPO_ROOT
  return path_join(std::string(GRX_REPO_ROOT), relative);
#else
  return relative;
#endif
}

/**
 * The whole of a text file, or an empty string when it cannot be read.
 *
 * cutil's reader rather than a loop here, which is what the other two copies
 * in this repository now call too. The loop this replaced stopped when
 * `fread` returned 0 and never asked `ferror()`, so a fixture that failed to
 * read halfway through came back as a *shorter fixture* - and a shorter
 * fixture is a test that checks less and still passes. A vector file read
 * short is the same failure the conformance runner's own self-test exists to
 * rule out.
 */
inline std::string read_file(const std::string & path) {
  void * data = nullptr;
  size_t length = 0;
  if (gcu_file_read(path.c_str(), GCU_FILE_UNLIMITED, nullptr, &data, &length)
      != GCU_FILE_OK) {
    return std::string();
  }
  std::string out(static_cast<const char *>(data), length);
  gcu_file_free(nullptr, data);
  return out;
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

/**
 * An allocator that fails the nth allocation and succeeds at every other.
 *
 * What it is for: every `GRX_ERR_OOM` branch in the library is a path that a
 * test written by hand will not reach, because reaching it means making one
 * particular `malloc` fail. Sweeping `n` from 1 upwards over a compile and a
 * match reaches all of them, one per run, and each run gets to assert the two
 * things that matter - that the failure came back as a result code rather
 * than a crash, and that nothing was leaked on the way out.
 *
 * It is the same cutil vtable as CountingAllocator, so the same object works
 * for any library in the suite.
 */
class FailingAllocator {
public:
  /** @param fail_at Which allocation to fail, counting from 1. 0 fails none. */
  explicit FailingAllocator(long fail_at) : fail_at_(fail_at) {
    vtable_.ctx = this;
    vtable_.malloc_fn = [](void * ctx, size_t size) -> void * {
      auto * self = static_cast<FailingAllocator *>(ctx);
      if (!self->allow()) {
        return nullptr;
      }
      void * p = std::malloc(size ? size : 1);
      if (p) {
        self->live_++;
      }
      return p;
    };
    vtable_.calloc_fn = [](void * ctx, size_t nitems, size_t size) -> void * {
      auto * self = static_cast<FailingAllocator *>(ctx);
      if (!self->allow()) {
        return nullptr;
      }
      if (nitems && size && nitems > (size_t)-1 / size) {
        return nullptr;
      }
      void * p = std::calloc(nitems ? nitems : 1, size ? size : 1);
      if (p) {
        self->live_++;
      }
      return p;
    };
    vtable_.realloc_fn = [](void * ctx, void * ptr, size_t size) -> void * {
      auto * self = static_cast<FailingAllocator *>(ctx);
      if (!self->allow()) {
        // A failed realloc must leave the original block alone, or the
        // caller's cleanup would free memory that is still live.
        return nullptr;
      }
      void * p = std::realloc(ptr, size ? size : 1);
      if (p && !ptr) {
        self->live_++;
      }
      return p;
    };
    vtable_.free_fn = [](void * ctx, void * ptr) {
      auto * self = static_cast<FailingAllocator *>(ctx);
      if (ptr) {
        self->live_--;
      }
      std::free(ptr);
    };
  }

  const GRX_Allocator * get() const { return &vtable_; }

  /** Allocations requested so far, whether they succeeded or not. */
  long requested() const { return requested_; }

  /** Allocations made and not yet freed. */
  long live() const { return live_; }

  /** Whether the injected failure actually happened. */
  bool failed() const { return fail_at_ && requested_ >= fail_at_; }

private:
  bool allow() {
    requested_++;
    return !fail_at_ || requested_ != fail_at_;
  }

  GRX_Allocator vtable_ {};
  long fail_at_ = 0;
  long requested_ = 0;
  long live_ = 0;
};

} // namespace grxtest

#endif // GHOTI_IO_GRX_TEST_HELPERS_H
