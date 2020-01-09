/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2019 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */
#pragma once
#ifndef __SRT_SYNC_H__
#define __SRT_SYNC_H__

//#define USE_STL_CHRONO
//#define ENABLE_CXX17

#include <cstdlib>
#include <pthread.h>
#include "utilities.h"

namespace srt
{
namespace sync
{
using namespace std;

template <class _Clock>
class Duration
{
public:
    Duration()
        : m_duration(0)
    {
    }

    explicit Duration(int64_t d)
        : m_duration(d)
    {
    }

public:
    inline int64_t count() const { return m_duration; }

    static Duration zero() { return Duration(); }

public: // Relational operators
    inline bool operator>=(const Duration& rhs) const { return m_duration >= rhs.m_duration; }
    inline bool operator>(const Duration& rhs) const { return m_duration > rhs.m_duration; }
    inline bool operator==(const Duration& rhs) const { return m_duration == rhs.m_duration; }
    inline bool operator!=(const Duration& rhs) const { return m_duration != rhs.m_duration; }
    inline bool operator<=(const Duration& rhs) const { return m_duration <= rhs.m_duration; }
    inline bool operator<(const Duration& rhs) const { return m_duration < rhs.m_duration; }

public: // Assignment operators
    inline void operator*=(const double mult) { m_duration = static_cast<int64_t>(m_duration * mult); }
    inline void operator+=(const Duration& rhs) { m_duration += rhs.m_duration; }
    inline void operator-=(const Duration& rhs) { m_duration -= rhs.m_duration; }

    inline Duration operator+(const Duration& rhs) const { return Duration(m_duration + rhs.m_duration); }
    inline Duration operator-(const Duration& rhs) const { return Duration(m_duration - rhs.m_duration); }
    inline Duration operator*(const int& rhs) const { return Duration(m_duration * rhs); }

  private:
    // int64_t range is from -9,223,372,036,854,775,808 to 9,223,372,036,854,775,807
    int64_t m_duration;
};

template <class _Clock>
class TimePoint;

class steady_clock
{
public:
    typedef Duration<steady_clock>  duration;
    typedef TimePoint<steady_clock> time_point;

public:
    static time_point now();
    static time_point zero();
};

template <class _Clock>
class TimePoint
{ // represents a point in time
public:
    TimePoint()
        : m_timestamp(0)
    {
    }

    explicit TimePoint(uint64_t tp)
        : m_timestamp(tp)
    {
    }

    TimePoint(const TimePoint<_Clock>& other)
        : m_timestamp(other.m_timestamp)
    {
    }

    ~TimePoint() {}

public: // Relational operators
    inline bool operator<(const TimePoint<_Clock>& rhs) const { return m_timestamp < rhs.m_timestamp; }
    inline bool operator<=(const TimePoint<_Clock>& rhs) const { return m_timestamp <= rhs.m_timestamp; }
    inline bool operator==(const TimePoint<_Clock>& rhs) const { return m_timestamp == rhs.m_timestamp; }
    inline bool operator!=(const TimePoint<_Clock>& rhs) const { return m_timestamp != rhs.m_timestamp; }
    inline bool operator>=(const TimePoint<_Clock>& rhs) const { return m_timestamp >= rhs.m_timestamp; }
    inline bool operator>(const TimePoint<_Clock>& rhs) const { return m_timestamp > rhs.m_timestamp; }

public: // Arithmetic operators
    inline Duration<_Clock> operator-(const TimePoint<_Clock>& rhs) const
    {
        return Duration<_Clock>(m_timestamp - rhs.m_timestamp);
    }
    inline TimePoint operator+(const Duration<_Clock>& rhs) const { return TimePoint(m_timestamp + rhs.count()); }
    inline TimePoint operator-(const Duration<_Clock>& rhs) const { return TimePoint(m_timestamp - rhs.count()); }

public: // Assignment operators
    inline void operator=(const TimePoint<_Clock>& rhs) { m_timestamp = rhs.m_timestamp; }
    inline void operator+=(const Duration<_Clock>& rhs) { m_timestamp += rhs.count(); }
    inline void operator-=(const Duration<_Clock>& rhs) { m_timestamp -= rhs.count(); }

public: //
#if HAVE_FULL_CXX11
    static inline ATR_CONSTEXPR TimePoint min() { return TimePoint(numeric_limits<uint64_t>::min()); }
    static inline ATR_CONSTEXPR TimePoint max() { return TimePoint(numeric_limits<uint64_t>::max()); }
#else
#ifndef UINT64_MAX
#define UNDEF_UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif
    static inline TimePoint min() { return TimePoint(0); }
    static inline TimePoint max() { return TimePoint(UINT64_MAX); }

#ifdef UNDEF_UINT64_MAX
#undef UINT64_MAX
#endif
#endif

public:
    uint64_t         us_since_epoch() const;
    Duration<_Clock> time_since_epoch() const;

public:
    bool is_zero() const { return m_timestamp == 0; }

private:
    uint64_t m_timestamp;
};

inline TimePoint<srt::sync::steady_clock> steady_clock::zero()
{
    return TimePoint<steady_clock>(0);
}


template <>
uint64_t srt::sync::TimePoint<srt::sync::steady_clock>::us_since_epoch() const;

template <>
srt::sync::Duration<srt::sync::steady_clock> srt::sync::TimePoint<srt::sync::steady_clock>::time_since_epoch() const;

inline Duration<steady_clock> operator*(const int& lhs, const Duration<steady_clock>& rhs)
{
    return rhs * lhs;
}

inline int64_t count_microseconds(const TimePoint<steady_clock> tp)
{
    return static_cast<int64_t>(tp.us_since_epoch());
}

typedef int64_t count_fn_t(const steady_clock::duration& t);

int64_t count_microseconds(const steady_clock::duration& t);
int64_t count_milliseconds(const steady_clock::duration& t);
int64_t count_seconds(const steady_clock::duration& t);

Duration<steady_clock> microseconds_from(int64_t t_us);
Duration<steady_clock> milliseconds_from(int64_t t_ms);
Duration<steady_clock> seconds_from(int64_t t_s);

inline bool is_zero(const TimePoint<steady_clock>& t) { return t.is_zero(); }

///////////////////////////////////////////////////////////////////////////////
//
// Common pthread/chrono section
//
///////////////////////////////////////////////////////////////////////////////

class SyncEvent
{
public:
    /// Atomically releases lock, blocks the current executing thread,
    /// and adds it to the list of threads waiting on* this.
    /// The thread will be unblocked when notify_all() or notify_one() is executed,
    /// or when the relative timeout rel_time expires.
    /// It may also be unblocked spuriously.
    /// When unblocked, regardless of the reason, lock is reacquiredand wait_for() exits.
    ///
    /// @return result of pthread_cond_wait(...) function call
    ///
    static int wait_for(pthread_cond_t* cond, pthread_mutex_t* mutex, const steady_clock::duration& rel_time);

    static int wait_for_monotonic(pthread_cond_t* cond, pthread_mutex_t* mutex, const steady_clock::duration& rel_time);
};

class CGuard
{
#if ENABLE_THREAD_LOGGING
    std::string lockname;
#endif
public:
   /// Constructs CGuard, which locks the given mutex for
   /// the scope where this object exists.
   /// @param lock Mutex to lock
   /// @param if_condition If this is false, CGuard will do completely nothing
   CGuard(pthread_mutex_t& lock, const char* ln = 0, bool if_condition = true);
   ~CGuard();

public:

   // The force-Lock/Unlock mechanism can be used to forcefully
   // change the lock on the CGuard object. This is in order to
   // temporarily change the lock status on the given mutex, but
   // still do the right job in the destructor. For example, if
   // a lock has been forcefully unlocked by forceUnlock, then
   // the CGuard object will not try to unlock it in the destructor,
   // but again, if the forceLock() was done again, the destructor
   // will still unlock the mutex.
   void forceLock()
   {
       if (m_iLocked == 0)
           return;
       Lock();
   }

   // After calling this on a scoped lock wrapper (CGuard),
   // the mutex will be unlocked right now, and no longer
   // in destructor
   void forceUnlock()
   {
       if (m_iLocked == 0)
       {
           m_iLocked = -1;
           Unlock();
       }
   }

   static int enterCS(pthread_mutex_t& lock, const char* ln = 0, bool block = true);
   static int leaveCS(pthread_mutex_t& lock, const char* ln = 0);

   static bool isthread(const pthread_t& thrval);

   static bool join(pthread_t& thr, void*& result);
   static bool join(pthread_t& thr);

   static void createMutex(pthread_mutex_t& lock);
   static void releaseMutex(pthread_mutex_t& lock);

   static void createCond(pthread_cond_t& cond, pthread_condattr_t* opt_attr = NULL);
   static void releaseCond(pthread_cond_t& cond);

#if ENABLE_LOGGING

   // Turned explicitly to string because this is exposed only for logging purposes.
   std::string show_mutex()
   {
       std::ostringstream os;
       os << (&m_Mutex);
       return os.str();
   }
#endif

private:

   void Lock()
   {
       m_iLocked = pthread_mutex_lock(&m_Mutex);
   }

   void Unlock()
   {
        pthread_mutex_unlock(&m_Mutex);
   }

   pthread_mutex_t& m_Mutex;            // Alias name of the mutex to be protected
   int m_iLocked;                       // Locking status

   CGuard& operator=(const CGuard&);

   friend class CSync;
};

class InvertedGuard
{
    pthread_mutex_t* m_pMutex;
#if ENABLE_THREAD_LOGGING
    std::string lockid;
#endif
public:

    InvertedGuard(pthread_mutex_t* smutex, const char* ln = NULL): m_pMutex(smutex)
    {
        if ( !smutex )
            return;
#if ENABLE_THREAD_LOGGING
        if (ln)
            lockid = ln;
#endif
        CGuard::leaveCS(*smutex, ln);
    }

    ~InvertedGuard()
    {
        if ( !m_pMutex )
            return;

#if ENABLE_THREAD_LOGGING
        CGuard::enterCS(*m_pMutex, lockid.empty() ? (const char*)0 : lockid.c_str());
#else
        CGuard::enterCS(*m_pMutex);
#endif
    }
};

// This class is used for condition variable combined with mutex by different ways.
// This should provide a cleaner API around locking with debug-logging inside.
class CSync
{
    pthread_cond_t* m_cond;
    pthread_mutex_t* m_mutex;
#if ENABLE_THREAD_LOGGING
    bool nolock;
    std::string cvname;
    std::string lockname;
#endif

public:

    enum Nolock { NOLOCK };

    // Locked version: must be declared only after the declaration of CGuard,
    // which has locked the mutex. On this delegate you should call only
    // signal_locked() and pass the CGuard variable that should remain locked.
    // Also wait() and wait_until() can be used only with this socket.
    CSync(pthread_cond_t& cond, CGuard& g, const char* ln = 0);

    // This is only for one-shot signaling. This doesn't need a CGuard
    // variable, only the mutex itself. Only lock_signal() can be used.
    CSync(pthread_cond_t& cond, pthread_mutex_t& mutex, Nolock, const char* cn = 0, const char* ln = 0);

    // Wait indefinitely, until getting a signal on CV.
    void wait();

    // Wait only up to given time (microseconds since epoch, the same unit as
    // for CTimer::getTime()).
    // Return: true, if interrupted by a signal. False if exit on timeout.
    bool wait_until(const srt::sync::steady_clock::time_point& timestamp);

    // Wait only for a given time delay (in microseconds). This function
    // extracts first current time using gettimeofday().
    bool wait_for(const srt::sync::steady_clock::duration& delay);

    // You can signal using two methods:
    // - lock_signal: expect the mutex NOT locked, lock it, signal, then unlock.
    // - signal: expect the mutex locked, so only issue a signal, but you must pass the CGuard that keeps the lock.
    void lock_signal();
    void signal_locked(CGuard& lk);
    void signal_relaxed();
};


/// Print steady clock timepoint in a human readable way.
/// days HH:MM::SS.us [STD]
/// Example: 1D 02:12:56.123456
///
/// @param [in] steady clock timepoint
/// @returns a string with a formatted time representation
std::string FormatTime(const steady_clock::time_point& time);

/// Print steady clock timepoint relative to the current system time
/// Date HH:MM::SS.us [SYS]
/// @param [in] steady clock timepoint
/// @returns a string with a formatted time representation
std::string FormatTimeSys(const steady_clock::time_point& time);

extern const char* const duration_unit_names [];

enum eUnit {DUNIT_S, DUNIT_MS, DUNIT_US};

template <eUnit u>
struct DurationUnitName;

template<>
struct DurationUnitName<DUNIT_US>
{
    static const char* name() { return "us"; }
    static double count(const steady_clock::duration& dur) { return count_microseconds(dur); }
};

template<>
struct DurationUnitName<DUNIT_MS>
{
    static const char* name() { return "ms"; }
    static double count(const steady_clock::duration& dur) { return count_microseconds(dur)/1000.0; }
};

template<>
struct DurationUnitName<DUNIT_S>
{
    static const char* name() { return "s"; }
    static double count(const steady_clock::duration& dur) { return count_microseconds(dur)/1000000.0; }
};

template<eUnit UNIT>
inline std::string FormatDuration(const steady_clock::duration& dur)
{
    return Sprint(DurationUnitName<UNIT>::count(dur)) + DurationUnitName<UNIT>::name();
}

inline std::string FormatDuration(const steady_clock::duration& dur)
{
    return FormatDuration<DUNIT_US>(dur);
}

// Debug purposes
#if ENABLE_THREAD_LOGGING
void ThreadCheckAffinity(const char* function, pthread_t thr);
#define THREAD_CHECK_AFFINITY(thr) srt::sync::ThreadCheckAffinity(__FUNCTION__, thr)
#else
#define THREAD_CHECK_AFFINITY(thr)
#endif

} // namespace sync
} // namespace srt

#endif // __SRT_SYNC_H__
