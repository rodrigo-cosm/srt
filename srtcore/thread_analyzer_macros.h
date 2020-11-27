#ifndef SRT_THREAD_SAFETY_ANALYSIS_MUTEX_H
#define SRT_THREAD_SAFETY_ANALYSIS_MUTEX_H

// Enable thread safety attributes only with clang.
// The attributes can be safely erased when compiling with other compilers.
#if defined(__clang__) && (!defined(SWIG))
#define SRT_CLANG_ATTR(x)   __attribute__((x))
#else
#define SRT_CLANG_ATTR(x)   // no-op
#endif

#define SRTSYNC_CAPABILITY(x) \
  SRT_CLANG_ATTR(capability(x))

#define SRTSYNC_SCOPED_CAPABILITY \
  SRT_CLANG_ATTR(scoped_lockable)

#define SRTSYNC_GUARDED_BY(x) \
  SRT_CLANG_ATTR(guarded_by(x))

#define SRTSYNC_PT_GUARDED_BY(x) \
  SRT_CLANG_ATTR(pt_guarded_by(x))

#define SRTSYNC_ACQUIRED_BEFORE(...) \
  SRT_CLANG_ATTR(acquired_before(__VA_ARGS__))

#define SRTSYNC_ACQUIRED_AFTER(...) \
  SRT_CLANG_ATTR(acquired_after(__VA_ARGS__))

#define SRTSYNC_REQUIRES(...) \
  SRT_CLANG_ATTR(requires_capability(__VA_ARGS__))

#define SRTSYNC_REQUIRES_SHARED(...) \
  SRT_CLANG_ATTR(requires_shared_capability(__VA_ARGS__))

#define SRTSYNC_ACQUIRE(...) \
  SRT_CLANG_ATTR(acquire_capability(__VA_ARGS__))

#define SRTSYNC_ACQUIRE_SHARED(...) \
  SRT_CLANG_ATTR(acquire_shared_capability(__VA_ARGS__))

#define SRTSYNC_RELEASE(...) \
  SRT_CLANG_ATTR(release_capability(__VA_ARGS__))

#define SRTSYNC_RELEASE_SHARED(...) \
  SRT_CLANG_ATTR(release_shared_capability(__VA_ARGS__))

#define SRTSYNC_RELEASE_GENERIC(...) \
  SRT_CLANG_ATTR(release_generic_capability(__VA_ARGS__))

#define SRTSYNC_TRY_ACQUIRE(...) \
  SRT_CLANG_ATTR(try_acquire_capability(__VA_ARGS__))

#define SRTSYNC_TRY_ACQUIRE_SHARED(...) \
  SRT_CLANG_ATTR(try_acquire_shared_capability(__VA_ARGS__))

#define SRTSYNC_EXCLUDES(...) \
  SRT_CLANG_ATTR(locks_excluded(__VA_ARGS__))

#define SRTSYNC_ASSERT_CAPABILITY(x) \
  SRT_CLANG_ATTR(assert_capability(x))

#define SRTSYNC_ASSERT_SHARED_CAPABILITY(x) \
  SRT_CLANG_ATTR(assert_shared_capability(x))

#define SRTSYNC_RETURN_CAPABILITY(x) \
  SRT_CLANG_ATTR(lock_returned(x))

#define SRTSYNC_NO_THREAD_SAFETY_ANALYSIS \
  SRT_CLANG_ATTR(no_thread_safety_analysis)


// informational only
// an example of mutex class to be instrumented
#if 0

// Defines an annotated interface for mutexes.
// These methods can be implemented to use any internal mutex implementation.
class CAPABILITY("mutex") Mutex {
public:
  // Acquire/lock this mutex exclusively.  Only one thread can have exclusive
  // access at any one time.  Write operations to guarded data require an
  // exclusive lock.
  void Lock() ACQUIRE();

  // Acquire/lock this mutex for read operations, which require only a shared
  // lock.  This assumes a multiple-reader, single writer semantics.  Multiple
  // threads may acquire the mutex simultaneously as readers, but a writer
  // must wait for all of them to release the mutex before it can acquire it
  // exclusively.
  void ReaderLock() ACQUIRE_SHARED();

  // Release/unlock an exclusive mutex.
  void Unlock() RELEASE();

  // Release/unlock a shared mutex.
  void ReaderUnlock() RELEASE_SHARED();

  // Generic unlock, can unlock exclusive and shared mutexes.
  void GenericUnlock() RELEASE_GENERIC();

  // Try to acquire the mutex.  Returns true on success, and false on failure.
  bool TryLock() TRY_ACQUIRE(true);

  // Try to acquire the mutex for read operations.
  bool ReaderTryLock() TRY_ACQUIRE_SHARED(true);

  // Assert that this mutex is currently held by the calling thread.
  void AssertHeld() ASSERT_CAPABILITY(this);

  // Assert that is mutex is currently held for read operations.
  void AssertReaderHeld() ASSERT_SHARED_CAPABILITY(this);

  // For negative capabilities.
  const Mutex& operator!() const { return *this; }
};

// Tag types for selecting a constructor.
struct adopt_lock_t {} inline constexpr adopt_lock = {};
struct defer_lock_t {} inline constexpr defer_lock = {};
struct shared_lock_t {} inline constexpr shared_lock = {};

// MutexLocker is an RAII class that acquires a mutex in its constructor, and
// releases it in its destructor.
class SCOPED_CAPABILITY MutexLocker {
private:
  Mutex* mut;
  bool locked;

public:
  // Acquire mu, implicitly acquire *this and associate it with mu.
  MutexLocker(Mutex *mu) ACQUIRE(mu) : mut(mu), locked(true) {
    mu->Lock();
  }

  // Assume mu is held, implicitly acquire *this and associate it with mu.
  MutexLocker(Mutex *mu, adopt_lock_t) REQUIRES(mu) : mut(mu), locked(true) {}

  // Acquire mu in shared mode, implicitly acquire *this and associate it with mu.
  MutexLocker(Mutex *mu, shared_lock_t) ACQUIRE_SHARED(mu) : mut(mu), locked(true) {
    mu->ReaderLock();
  }

  // Assume mu is held in shared mode, implicitly acquire *this and associate it with mu.
  MutexLocker(Mutex *mu, adopt_lock_t, shared_lock_t) REQUIRES_SHARED(mu)
    : mut(mu), locked(true) {}

  // Assume mu is not held, implicitly acquire *this and associate it with mu.
  MutexLocker(Mutex *mu, defer_lock_t) EXCLUDES(mu) : mut(mu), locked(false) {}

  // Release *this and all associated mutexes, if they are still held.
  // There is no warning if the scope was already unlocked before.
  ~MutexLocker() RELEASE() {
    if (locked)
      mut->GenericUnlock();
  }

  // Acquire all associated mutexes exclusively.
  void Lock() ACQUIRE() {
    mut->Lock();
    locked = true;
  }

  // Try to acquire all associated mutexes exclusively.
  bool TryLock() TRY_ACQUIRE(true) {
    return locked = mut->TryLock();
  }

  // Acquire all associated mutexes in shared mode.
  void ReaderLock() ACQUIRE_SHARED() {
    mut->ReaderLock();
    locked = true;
  }

  // Try to acquire all associated mutexes in shared mode.
  bool ReaderTryLock() TRY_ACQUIRE_SHARED(true) {
    return locked = mut->ReaderTryLock();
  }

  // Release all associated mutexes. Warn on double unlock.
  void Unlock() RELEASE() {
    mut->Unlock();
    locked = false;
  }

  // Release all associated mutexes. Warn on double unlock.
  void ReaderUnlock() RELEASE() {
    mut->ReaderUnlock();
    locked = false;
  }
};
#endif // example class

#ifdef USE_LOCK_STYLE_THREAD_SAFETY_ATTRIBUTES
// The original version of thread safety analysis the following attribute
// definitions.  These use a lock-based terminology.  They are still in use
// by existing thread safety code, and will continue to be supported.

// Deprecated.
#define SRTSYNC_PT_GUARDED_VAR \
  SRT_CLANG_ATTR(pt_guarded_var)

// Deprecated.
#define SRTSYNC_GUARDED_VAR \
  SRT_CLANG_ATTR(guarded_var)

// Replaced by REQUIRES
#define SRTSYNC_EXCLUSIVE_LOCKS_REQUIRED(...) \
  SRT_CLANG_ATTR(exclusive_locks_required(__VA_ARGS__))

// Replaced by REQUIRES_SHARED
#define SRTSYNC_SHARED_LOCKS_REQUIRED(...) \
  SRT_CLANG_ATTR(shared_locks_required(__VA_ARGS__))

// Replaced by CAPABILITY
#define SRTSYNC_LOCKABLE \
  SRT_CLANG_ATTR(lockable)

// Replaced by SCOPED_CAPABILITY
#define SRTSYNC_SCOPED_LOCKABLE \
  SRT_CLANG_ATTR(scoped_lockable)

// Replaced by ACQUIRE
#define SRTSYNC_EXCLUSIVE_LOCK_FUNCTION(...) \
  SRT_CLANG_ATTR(exclusive_lock_function(__VA_ARGS__))

// Replaced by ACQUIRE_SHARED
#define SRTSYNC_SHARED_LOCK_FUNCTION(...) \
  SRT_CLANG_ATTR(shared_lock_function(__VA_ARGS__))

// Replaced by RELEASE and RELEASE_SHARED
#define SRTSYNC_UNLOCK_FUNCTION(...) \
  SRT_CLANG_ATTR(unlock_function(__VA_ARGS__))

// Replaced by TRY_ACQUIRE
#define SRTSYNC_EXCLUSIVE_TRYLOCK_FUNCTION(...) \
  SRT_CLANG_ATTR(exclusive_trylock_function(__VA_ARGS__))

// Replaced by TRY_ACQUIRE_SHARED
#define SRTSYNC_SHARED_TRYLOCK_FUNCTION(...) \
  SRT_CLANG_ATTR(shared_trylock_function(__VA_ARGS__))

// Replaced by ASSERT_CAPABILITY
#define SRTSYNC_ASSERT_EXCLUSIVE_LOCK(...) \
  SRT_CLANG_ATTR(assert_exclusive_lock(__VA_ARGS__))

// Replaced by ASSERT_SHARED_CAPABILITY
#define SRTSYNC_ASSERT_SHARED_LOCK(...) \
  SRT_CLANG_ATTR(assert_shared_lock(__VA_ARGS__))

// Replaced by EXCLUDE_CAPABILITY.
#define SRTSYNC_LOCKS_EXCLUDED(...) \
  SRT_CLANG_ATTR(locks_excluded(__VA_ARGS__))

// Replaced by RETURN_CAPABILITY
#define SRTSYNC_LOCK_RETURNED(x) \
  SRT_CLANG_ATTR(lock_returned(x))

#endif  // USE_LOCK_STYLE_THREAD_SAFETY_ATTRIBUTES

#endif 
