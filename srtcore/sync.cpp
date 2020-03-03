/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2019 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include <iomanip>
#include <math.h>
#include <stdexcept>
#include "sync.h"
#include "logging.h"
#include "udt.h"
#include "srt_compat.h"

#if defined(_WIN32)
#define TIMING_USE_QPC
#include "win/wintime.h"
#include <sys/timeb.h>
#elif defined(OSX) || (TARGET_OS_IOS == 1) || (TARGET_OS_TV == 1)
#define TIMING_USE_MACH_ABS_TIME
#include <mach/mach_time.h>
//#elif defined(_POSIX_MONOTONIC_CLOCK) && _POSIX_TIMERS > 0
#elif defined(ENABLE_MONOTONIC_CLOCK)
#define TIMING_USE_CLOCK_GETTIME
#endif

namespace srt_logging
{
extern Logger mglog; // For ThreadCheckAffinity
}

namespace srt
{
namespace sync
{

void rdtsc(uint64_t& x)
{
#ifdef IA32
    uint32_t lval, hval;
    // asm volatile ("push %eax; push %ebx; push %ecx; push %edx");
    // asm volatile ("xor %eax, %eax; cpuid");
    asm volatile("rdtsc" : "=a"(lval), "=d"(hval));
    // asm volatile ("pop %edx; pop %ecx; pop %ebx; pop %eax");
    x = hval;
    x = (x << 32) | lval;
#elif defined(IA64)
    asm("mov %0=ar.itc" : "=r"(x)::"memory");
#elif defined(AMD64)
    uint32_t lval, hval;
    asm("rdtsc" : "=a"(lval), "=d"(hval));
    x = hval;
    x = (x << 32) | lval;
#elif defined(TIMING_USE_QPC)
    // This function should not fail, because we checked the QPC
    // when calling to QueryPerformanceFrequency. If it failed,
    // the m_bUseMicroSecond was set to true.
    QueryPerformanceCounter((LARGE_INTEGER*)&x);
#elif defined(TIMING_USE_MACH_ABS_TIME)
    x = mach_absolute_time();
#else
    // use system call to read time clock for other archs
    timeval t;
    gettimeofday(&t, 0);
    x = t.tv_sec * uint64_t(1000000) + t.tv_usec;
#endif
}

int64_t get_cpu_frequency()
{
    int64_t frequency = 1; // 1 tick per microsecond.

#if defined(TIMING_USE_QPC)
    LARGE_INTEGER ccf; // in counts per second
    if (QueryPerformanceFrequency(&ccf))
        frequency = ccf.QuadPart / 1000000; // counts per microsecond

#elif defined(TIMING_USE_MACH_ABS_TIME)

    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    frequency = info.denom * int64_t(1000) / info.numer;

#elif defined(IA32) || defined(IA64) || defined(AMD64)
    uint64_t t1, t2;

    rdtsc(t1);
    timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);
    rdtsc(t2);

    // CPU clocks per microsecond
    frequency = int64_t(t2 - t1) / 100000;
#endif

    return frequency;
}

const int64_t s_cpu_frequency = get_cpu_frequency();

#ifdef ENABLE_THREAD_LOGGING
struct CGuardLogMutex
{
    pthread_mutex_t mx;
    CGuardLogMutex()
    {
        pthread_mutex_init(&mx, NULL);
    }

    ~CGuardLogMutex()
    {
        pthread_mutex_destroy(&mx);
    }

    void lock() { pthread_mutex_lock(&mx); }
    void unlock() { pthread_mutex_unlock(&mx); }
};
static CGuardLogMutex g_gmtx;
#endif


#if ENABLE_THREAD_LOGGING

// This is the thread-logging version of CGuard.

// Constructors and destructors are defined here in the same order in which
// they will be executed.

// Automatically lock in constructor
CGuardLogEntry::CGuardLogEntry(CMutexWrapper& lock)
{
    LOGS(std::cerr, log << "CGuard: { LOCK:" << lock.lockname << " ...");
}

// Finished, next the CGuardImp ctor (also as an alias to std::unique_lock),
// THEN this:

CGuard::CGuard(CMutexWrapper& lock)
    : CGuardLogEntry(lock) // Display the log BEFORE actual locking
    , CGuardImp(lock.in_sysobj) // DO actual locking...
    , CGuardLogExit(lock)
{
    if (!owns_lock())
    {
#if ENABLE_THREAD_ASSERT
        abort(); // Actually not possible and C++ API doesn't predict it.
#endif
    }
    else
    {
        was_locked = true; // a field in CGuardLogExit
    }
    // ... and display the post-locking message
    LOGS(std::cerr, log << "... " << lock.lockname << " locked."
            << " lock state:" <<
            (owns_lock() ? "locked successfully" : "ERROR"));

    mutexp = &lock; // Field in CGuardLogExit
}

void CGuard::unlock()
{
    if (was_locked) // owns_lock() is already false, even if it WAS locked.
    {
        LOGS(std::cerr, log << "CGuard: } UNLOCK:" << mutexp->lockname);
    }
    else
    {
        LOGS(std::cerr, log << "CGuard: UNLOCK NOT DONE (not locked):" << mutexp->lockname);
    }

    // Still, redirect to original unlock
    CGuardImp::unlock();
    was_locked = false;
}

// So the POST-lock log has been displayed, just after the lock happened.

// Now --- DESTRUCTION ---

// First goes CGuardLogExit

CGuardLogExit::~CGuardLogExit()
{
    if (was_locked) // owns_lock() is already false, even if it WAS locked.
    {
        LOGS(std::cerr, log << "CGuard: } UNLOCK:" << mutexp->lockname);
    }
    else
    {
        LOGS(std::cerr, log << "CGuard: UNLOCK NOT DONE (not locked):" << mutexp->lockname);
    }
}

// NEXT: CGuardImp dtor (unlocks or not), then CGuardLogEntry (does nothing)

void enterCS(Mutex& lock)
{
    LOGS(std::cerr, log << "enterCS(block) {  LOCK: " << lock.lockname << " ...");
    lock.in_sysobj.lock();
    LOGS(std::cerr, log << "... " << lock.lockname << " locked.");
}

bool tryEnterCS(Mutex& lock)
{
    bool islocked = lock.in_sysobj.try_lock();
    LOGS(std::cerr, log << "enterCS(try) {  LOCK: " << lock.lockname << " "
            << (islocked ? " LOCKED." : " FAILED }"));
    return islocked;
}

void leaveCS(Mutex& lock)
{
    LOGS(std::cerr, log << "leaveCS: } UNLOCK: " << lock.lockname);
    lock.in_sysobj.unlock();
}
#endif

/// This function checks if the given thread id
/// is a thread id, stating that a thread id variable
/// that doesn't hold a running thread, is equal to
/// a null thread (pthread_t()).
bool isthread(const pthread_t& thr)
{
    return pthread_equal(thr, pthread_t()) == 0; // NOT equal to a null thread
}

bool jointhread(pthread_t& thr)
{
    LOGS(std::cerr, log << "JOIN: " << thr << " ---> " << pthread_self());
    int ret = pthread_join(thr, NULL);
    thr = pthread_t(); // prevent dangling
    return ret == 0;
}

bool jointhread(pthread_t& thr, void*& result)
{
    LOGS(std::cerr, log << "JOIN: " << thr << " ---> " << pthread_self());
    int ret = pthread_join(thr, &result);
    thr = pthread_t();
    return ret == 0;
}

#if ENABLE_THREAD_LOGGING
// Non-thread-logging imp is a stub provided only in the header file.
void setupMutex(Mutex& lock, const char* name)
{
    std::ostringstream cv;
    cv << &lock.in_sysobj;
    if (name)
    {
        cv << "(" << name << ")";
    }
    lock.lockname = cv.str();
}

void CConditionWrapper::setupName(const char* name, void* adr)
{
    std::ostringstream cv;
    cv << adr;
    if (name)
    {
        cv << "(" << name << ")";
    }
    cvname = cv.str();
}

void CSyncCheckLocked(Mutex& m, bool exp_locked)
{
    bool waslocked = true;
    if (m.try_lock())
    {
        m.unlock();
        waslocked = false;
    }

    if (waslocked != exp_locked)
    {
        LOGS(std::cerr, log << "CCond: IPE: Mutex " << m.lockname << " in CGuard IS "
                << (exp_locked ? "NOT" : "UNEXPECTEDLY")
                << " LOCKED.");
    }
}

#endif


// The signal_relaxed and broadcast_relaxed functions are to be used in case
// when you don't care whether the associated mutex is locked or not (you
// accept the case that a mutex isn't locked and the signal gets effectively
// missed), or you somehow know that the mutex is locked, but you don't have
// access to the associated CGuard object. This function, although it does
// the same thing as signal_locked() and broadcast_locked(), is here for
// the user to declare explicitly that the signal/broadcast is done without
// being prematurely certain that the associated mutex is locked.
//
// It is then expected that whenever these functions are used, an extra
// comment is provided to explain, why the use of the relaxed signaling is
// correctly used.

} // namespace sync
} // namespace srt


template <>
uint64_t srt::sync::TimePoint<srt::sync::steady_clock>::us_since_epoch() const
{
    return m_timestamp / s_cpu_frequency;
}

template <>
srt::sync::Duration<srt::sync::steady_clock> srt::sync::TimePoint<srt::sync::steady_clock>::time_since_epoch() const
{
    return srt::sync::Duration<srt::sync::steady_clock>(m_timestamp);
}

srt::sync::TimePoint<srt::sync::steady_clock> srt::sync::steady_clock::now()
{
    uint64_t x = 0;
    rdtsc(x);
    return TimePoint<steady_clock>(x);
}

int64_t srt::sync::count_microseconds(const steady_clock::duration& t)
{
    return t.count() / s_cpu_frequency;
}

int64_t srt::sync::count_milliseconds(const steady_clock::duration& t)
{
    return t.count() / s_cpu_frequency / 1000;
}

int64_t srt::sync::count_seconds(const steady_clock::duration& t)
{
    return t.count() / s_cpu_frequency / 1000000;
}

srt::sync::steady_clock::duration srt::sync::microseconds_from(int64_t t_us)
{
    return steady_clock::duration(t_us * s_cpu_frequency);
}

srt::sync::steady_clock::duration srt::sync::milliseconds_from(int64_t t_ms)
{
    return steady_clock::duration((1000 * t_ms) * s_cpu_frequency);
}

srt::sync::steady_clock::duration srt::sync::seconds_from(int64_t t_s)
{
    return steady_clock::duration((1000000 * t_s) * s_cpu_frequency);
}

std::string srt::sync::FormatTime(const steady_clock::time_point& timestamp)
{
    if (is_zero(timestamp))
    {
        // Use special string for 0
        return "00:00:00.000000";
    }

    const uint64_t total_us  = timestamp.us_since_epoch();
    const uint64_t us        = total_us % 1000000;
    const uint64_t total_sec = total_us / 1000000;

    const uint64_t days  = total_sec / (60 * 60 * 24);
    const uint64_t hours = total_sec / (60 * 60) - days * 24;

    const uint64_t minutes = total_sec / 60 - (days * 24 * 60) - hours * 60;
    const uint64_t seconds = total_sec - (days * 24 * 60 * 60) - hours * 60 * 60 - minutes * 60;

    ostringstream out;
    if (days)
        out << days << "D ";
    out << setfill('0') << setw(2) << hours << ":" 
        << setfill('0') << setw(2) << minutes << ":" 
        << setfill('0') << setw(2) << seconds << "." 
        << setfill('0') << setw(6) << us << " [STD]";
    return out.str();
}

std::string srt::sync::FormatTimeSys(const steady_clock::time_point& timestamp)
{
    const time_t                   now_s         = ::time(NULL); // get current time in seconds
    const steady_clock::time_point now_timestamp = steady_clock::now();
    const int64_t                  delta_us      = count_microseconds(timestamp - now_timestamp);
    const int64_t                  delta_s =
        floor((static_cast<int64_t>(now_timestamp.us_since_epoch() % 1000000) + delta_us) / 1000000.0);
    const time_t tt = now_s + delta_s;
    struct tm    tm = SysLocalTime(tt); // in seconds
    char         tmp_buf[512];
    strftime(tmp_buf, 512, "%X.", &tm);

    ostringstream out;
    out << tmp_buf << setfill('0') << setw(6) << (timestamp.us_since_epoch() % 1000000) << " [SYS]";
    return out.str();
}

srt::sync::MutexImp::MutexImp()
{
    pthread_mutexattr_t* pattr = NULL;
#if ENABLE_THREAD_LOGGING
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pattr = &attr;
#endif
    pthread_mutex_init(&m_mutex, pattr);
}

srt::sync::MutexImp::~MutexImp()
{
    pthread_mutex_destroy(&m_mutex);
}

void srt::sync::MutexImp::lock()
{
    pthread_mutex_lock(&m_mutex);
}

void srt::sync::MutexImp::unlock()
{
    pthread_mutex_unlock(&m_mutex);
}

bool srt::sync::MutexImp::try_lock()
{
    return (pthread_mutex_trylock(&m_mutex) == 0);
}

srt::sync::ScopedLock::ScopedLock(MutexImp& m)
    : m_mutex(m)
{
    m_mutex.lock();
}

srt::sync::ScopedLock::~ScopedLock()
{
    m_mutex.unlock();
}

//
//
//

srt::sync::UniqueLock::UniqueLock(MutexImp& m)
    : m_iLocked(-1), m_Mutex(m)
{
    m_Mutex.lock();
    m_iLocked = 0;
}

srt::sync::UniqueLock::~UniqueLock()
{
    unlock();
}

void srt::sync::UniqueLock::unlock()
{
    if (m_iLocked == 0)
    {
        m_Mutex.unlock();
        m_iLocked = -1;
    }
}

srt::sync::MutexImp* srt::sync::UniqueLock::mutex()
{
    return &m_Mutex;
}

////////////////////////////////////////////////////////////////////////////////
//
// Condition section (POSIX version)
//
////////////////////////////////////////////////////////////////////////////////
#ifndef USE_STDCXX_CHRONO

namespace srt
{
namespace sync
{

ConditionImp::ConditionImp() {}

ConditionImp::~ConditionImp() {}

void ConditionImp::init()
{
    pthread_condattr_t* attr = NULL;
#if ENABLE_MONOTONIC_CLOCK
    pthread_condattr_t  CondAttribs;
    pthread_condattr_init(&CondAttribs);
    pthread_condattr_setclock(&CondAttribs, CLOCK_MONOTONIC);
    attr = &CondAttribs;
#endif
    const int res = pthread_cond_init(&m_cv, attr);
    if (res != 0)
        throw std::runtime_error("pthread_cond_init monotonic failed");
}


void ConditionImp::destroy()
{
    pthread_cond_destroy(&m_cv);
}

void ConditionImp::wait(UniqueLock& lock)
{
    pthread_cond_wait(&m_cv, &lock.mutex()->ref());
}

bool ConditionImp::wait_for(UniqueLock& lock, const steady_clock::duration& rel_time)
{
    // This will be only monotonic according to the value of ENABLE_MONOTONIC_CLOCK.
    return (SyncEvent::wait_for(&m_cv, &lock.mutex()->ref(), rel_time) != ETIMEDOUT);
}


bool ConditionImp::wait_until(UniqueLock& lock, const steady_clock::time_point& timeout_time)
{
    // This will work regardless as to which clock is in use. The time
    // should be specified as steady_clock::time_point, so there's no
    // question of the timer base.
    const steady_clock::time_point now = steady_clock::now();
    if (now >= timeout_time)
        return false; // timeout

    // wait_for() is used because it will be converted to pthread-frienly timeout_time inside.
    return wait_for(lock, timeout_time - now);
}


void ConditionImp::notify_one()
{
    pthread_cond_signal(&m_cv);
}

void ConditionImp::notify_all()
{
    pthread_cond_broadcast(&m_cv);
}

}; // namespace sync
}; // namespace srt

#endif // ndef USE_STDCXX_CHRONO

// Condition for thread logging imp.

#if ENABLE_THREAD_LOGGING
namespace srt
{
namespace sync
{

void CConditionWrapper::wait(CGuard& lock)
{
    LOGS(std::cerr, log << "Cond: WAIT:" << cvname << " UNLOCK:" << lock.mutex()->lockname);
    THREAD_PAUSED();
    in_sysobj.wait(lock);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << cvname << " LOCKED:" << lock.mutexp->lockname);
}

bool CConditionWrapper::wait_for(CGuard& lock, const steady_clock::duration& delay)
{
    LOGS(std::cerr, log << "Cond: WAIT:" << cvname << " UNLOCK:" << lock.mutex()->lockname
            << " - for " << FormatDuration(delay) << " ...");
    THREAD_PAUSED();
    bool signaled = in_sysobj.wait_for(lock, delay);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << cvname << " LOCKED:" << lock.mutex()->lockname
            << " REASON: " << (signaled ? "SIGNAL" : "TIMEOUT"));
    return signaled;
}


bool CConditionWrapper::wait_until(CGuard& lock, const steady_clock::time_point& exptime)
{
    steady_clock::time_point now = steady_clock::now();
    if (now >= exptime)
    {
        LOGS(std::cerr, log << "Cond: WAIT: NOT waiting on " << cvname << " - already past time: " << FormatTime(exptime));
        return false; // timeout
    }

    LOGS(std::cerr, log << "Cond: WAIT: will wait on: " << cvname << " - until " << FormatTime(exptime));
    THREAD_PAUSED();
    bool signaled = in_sysobj.wait_until(lock, exptime);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << cvname << " LOCKED:" << lock.mutex()->lockname
            << " REASON: " << (signaled ? "SIGNAL" : "TIMEOUT"));
    return signaled;
}



}
}
#endif

// CSync implementation. This is common for with and without thread logging.
namespace srt
{
namespace sync
{

void CSync::wait()
{
    LOGS(std::cerr, log << "Cond: WAIT:" << m_cond->cvname << " UNLOCK:" << m_locker->mutexp->lockname);
    THREAD_PAUSED();
    RawAddr(*m_cond)->wait(*m_locker);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << m_cond->cvname << " LOCKED:" << m_locker->mutexp->lockname);
}

bool CSync::wait_for(const steady_clock::duration& delay)
{
    LOGS(std::cerr, log << "Cond: WAIT:" << m_cond->cvname << " UNLOCK:" << m_locker->mutexp->lockname
            << " - for " << FormatDuration(delay) << " ...");
    THREAD_PAUSED();
    bool signaled = RawAddr(*m_cond)->wait_for(*m_locker, delay);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << m_cond->cvname << " LOCKED:" << m_locker->mutexp->lockname
            << " REASON: " << (signaled ? "SIGNAL" : "TIMEOUT"));
    return signaled;
}

bool CSync::wait_until(const steady_clock::time_point& exptime)
{
    // This will work regardless as to which clock is in use. The time
    // should be specified as steady_clock::time_point, so there's no
    // question of the timer base.
#if ENABLE_THREAD_LOGGING
    steady_clock::time_point now = steady_clock::now();
    if (now >= exptime)
    {
        LOGS(std::cerr, log << "Cond: WAIT: NOT waiting on " << m_cond->cvname << " - already past time: " << FormatTime(exptime));
        return false; // timeout
    }
#endif

    LOGS(std::cerr, log << "Cond: WAIT: will wait on: " << m_cond->cvname << " - until " << FormatTime(exptime));
    THREAD_PAUSED();
    bool signaled = RawAddr(*m_cond)->wait_until(*m_locker, exptime);
    THREAD_RESUMED();
    LOGS(std::cerr, log << "Cond: CAUGHT:" << m_cond->cvname << " LOCKED:" << m_locker->mutexp->lockname
            << " REASON: " << (signaled ? "SIGNAL" : "TIMEOUT"));
    return signaled;
}


void CSync::lock_signal(Condition& cond, Mutex& m)
{
    CSyncCheckLocked(m, false);
    LOGS(std::cerr, log << "Cond: SIGNAL:" << cond.cvname << " { LOCKING: " << m.lockname << "...");
    // Not using CGuard here because it would be logged
    // and this will result in unnecessary excessive logging.
    RawAddr(m)->lock();
    LOGS(std::cerr, log << "lock_signal: LOCKED " << m.lockname << " > SIGNAL " << cond.cvname << " <");
    RawAddr(cond)->notify_one();
    RawAddr(m)->unlock();
    LOGS(std::cerr, log << "Cond: } UNLOCK:" << m.lockname);
}

void CSync::lock_broadcast(Condition& cond, Mutex& m)
{
    CSyncCheckLocked(m, false);
    LOGS(std::cerr, log << "Cond: BROADCAST:" << cond.cvname << " { LOCKING: " << m.lockname << "...");
    RawAddr(m)->lock();
    LOGS(std::cerr, log << "lock_signal: LOCKED " << m.lockname << " > BROADCAST " << cond.cvname << " <");
    RawAddr(cond)->notify_all();
    RawAddr(m)->unlock();
    LOGS(std::cerr, log << "Cond: } UNLOCK:" << m.lockname);
}

void CSync::signal_locked(CGuard& lk ATR_UNUSED)
{
#if ENABLE_THREAD_LOGGING
    if (!lk.owns_lock())
    {
        LOGS(std::cerr, log << "signal_locked: IPE: MUTEX " << lk.mutexp->lockname << " NOT LOCKED!");
    }
    else
    {
        LOGS(std::cerr, log << "Cond: SIGNAL:" << m_cond->cvname << " (with locked:" << lk.mutexp->lockname << ")");
    }
#endif
    RawAddr(*m_cond)->notify_one();
}

void CSync::signal_relaxed(Condition& cond)
{
    LOGS(std::cerr, log << "SIGNAL(relaxed): " << cond.cvname);
    RawAddr(cond)->notify_one();
}

void CSync::broadcast_relaxed(Condition& cond)
{
    LOGS(std::cerr, log << "BROADCAST(relaxed): " << cond.cvname);
    RawAddr(cond)->notify_all();
}


}
}

////////////////////////////////////////////////////////////////////////////////
//
// SyncEvent section
//
////////////////////////////////////////////////////////////////////////////////

static timespec us_to_timespec(const uint64_t time_us)
{
    timespec timeout;
    timeout.tv_sec         = time_us / 1000000;
    timeout.tv_nsec        = (time_us % 1000000) * 1000;
    return timeout;
}

#if ENABLE_MONOTONIC_CLOCK
int srt::sync::SyncEvent::wait_for(pthread_cond_t* cond, pthread_mutex_t* mutex, const Duration<steady_clock>& rel_time)
{
    timespec timeout;
    clock_gettime(CLOCK_MONOTONIC, &timeout);
    const uint64_t now_us = timeout.tv_sec * uint64_t(1000000) + (timeout.tv_nsec / 1000);
    timeout = us_to_timespec(now_us + count_microseconds(rel_time));

    return pthread_cond_timedwait(cond, mutex, &timeout);
}
#else
int srt::sync::SyncEvent::wait_for(pthread_cond_t* cond, pthread_mutex_t* mutex, const Duration<steady_clock>& rel_time)
{
    timespec timeout;
    timeval now;
    gettimeofday(&now, 0);
    const uint64_t now_us = now.tv_sec * uint64_t(1000000) + now.tv_usec;
    timeout = us_to_timespec(now_us + count_microseconds(rel_time));

    return pthread_cond_timedwait(cond, mutex, &timeout);
}
#endif

#if ENABLE_THREAD_LOGGING
void srt::sync::ThreadCheckAffinity(const char* function, pthread_t thr)
{
    using namespace srt_logging;

    if (thr == pthread_self())
        return;

    LOGC(mglog.Fatal, log << "IPE: '" << function << "' should not be executed in this thread!");
    throw std::runtime_error("INTERNAL ERROR: incorrect function affinity");
}
#endif
