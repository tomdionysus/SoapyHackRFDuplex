// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A mutex and its scoped locks, carrying Clang's thread-safety
// annotations so that -Wthread-safety proves every member marked
// GUARDED_BY is touched only under its lock, and every function marked
// REQUIRES is called with it held. libstdc++'s std::mutex carries no
// annotations, hence the wrapper; to other compilers the macros are
// empty and the types are plain std::mutex underneath.
//
// A helper that needs one of the pair's two sides locked takes the side
// itself (REQUIRES(s.mutex)), since the analysis matches what a caller
// holds by expression, and cannot see that side(direction) is the side a
// caller locked. A lambda that runs under a lock says so the same way,
// as the analysis treats it as a function of its own.

#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__clang__)
#define HACKRFDUPLEX_THREAD_ANNOTATION(x) __attribute__((x))
#else
#define HACKRFDUPLEX_THREAD_ANNOTATION(x)
#endif

#define CAPABILITY(x) HACKRFDUPLEX_THREAD_ANNOTATION(capability(x))
#define SCOPED_CAPABILITY HACKRFDUPLEX_THREAD_ANNOTATION(scoped_lockable)
#define GUARDED_BY(x) HACKRFDUPLEX_THREAD_ANNOTATION(guarded_by(x))
#define REQUIRES(...) HACKRFDUPLEX_THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define ACQUIRE(...) HACKRFDUPLEX_THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define RELEASE(...) HACKRFDUPLEX_THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define EXCLUDES(...) HACKRFDUPLEX_THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define ACQUIRED_AFTER(...) HACKRFDUPLEX_THREAD_ANNOTATION(acquired_after(__VA_ARGS__))

namespace hackrfduplex {

// Taken only through Lock and LockBoth.
class CAPABILITY("mutex") Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

private:
    friend class Lock;
    friend class LockBoth;
    std::mutex mutex_;
};

// Holds one mutex for its scope. wait() and wait_until() wait on a
// condition variable, which releases the mutex while it waits, as
// std::condition_variable does with its own lock.
class SCOPED_CAPABILITY Lock {
public:
    explicit Lock(Mutex& mutex) ACQUIRE(mutex) : lock_(mutex.mutex_) {}
    ~Lock() RELEASE() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    template <typename Rep, typename Period, typename Predicate>
    bool wait_for(std::condition_variable& changed,
                  const std::chrono::duration<Rep, Period>& timeout, Predicate done) {
        return changed.wait_for(lock_, timeout, done);
    }
    template <typename Clock, typename Duration, typename Predicate>
    bool wait_until(std::condition_variable& changed,
                    const std::chrono::time_point<Clock, Duration>& deadline, Predicate done) {
        return changed.wait_until(lock_, deadline, done);
    }

private:
    std::unique_lock<std::mutex> lock_;
};

// Holds two mutexes for its scope, taken together without deadlock
// whatever order others take them in.
class SCOPED_CAPABILITY LockBoth {
public:
    LockBoth(Mutex& first, Mutex& second) ACQUIRE(first, second)
        : lock_(first.mutex_, second.mutex_) {}
    ~LockBoth() RELEASE() = default;
    LockBoth(const LockBoth&) = delete;
    LockBoth& operator=(const LockBoth&) = delete;

private:
    std::scoped_lock<std::mutex, std::mutex> lock_;
};

}  // namespace hackrfduplex
