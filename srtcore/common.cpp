/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

/*****************************************************************************
Copyright (c) 2001 - 2016, The Board of Trustees of the University of Illinois.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the
  above copyright notice, this list of conditions
  and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the University of Illinois
  nor the names of its contributors may be used to
  endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

/*****************************************************************************
written by
   Yunhong Gu, last updated 07/25/2010
modified by
   Haivision Systems Inc.
*****************************************************************************/

#define SRT_IMPORT_TIME 1
#include "platform_sys.h"

#if ENABLE_THREAD_LOGGING
#include <iostream>
#endif
#include <string>
#include <sstream>
#include <cmath>
#include <iostream>
#include <iomanip>
#include "srt.h"
#include "md5.h"
#include "common.h"
#include "netinet_any.h"
#include "logging.h"
#include "threadname.h"

#include <srt_compat.h> // SysStrError

using namespace std;


bool CTimer::m_bUseMicroSecond = false;
uint64_t CTimer::s_ullCPUFrequency = CTimer::readCPUFrequency();

pthread_mutex_t CTimer::m_EventLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t CTimer::m_EventCond = PTHREAD_COND_INITIALIZER;

CTimer::CTimer():
m_ullSchedTime(),
m_TickCond(),
m_TickLock()
{
    pthread_mutex_init(&m_TickLock, NULL);
    pthread_cond_init(&m_TickCond, NULL);
}

CTimer::~CTimer()
{
    pthread_mutex_destroy(&m_TickLock);
    pthread_cond_destroy(&m_TickCond);
}

void CTimer::rdtsc(uint64_t &x)
{
   if (m_bUseMicroSecond)
   {
      x = getTime();
      return;
   }

   #ifdef IA32
      uint32_t lval, hval;
      //asm volatile ("push %eax; push %ebx; push %ecx; push %edx");
      //asm volatile ("xor %eax, %eax; cpuid");
      asm volatile ("rdtsc" : "=a" (lval), "=d" (hval));
      //asm volatile ("pop %edx; pop %ecx; pop %ebx; pop %eax");
      x = hval;
      x = (x << 32) | lval;
   #elif defined(IA64)
      asm ("mov %0=ar.itc" : "=r"(x) :: "memory");
   #elif defined(AMD64)
      uint32_t lval, hval;
      asm ("rdtsc" : "=a" (lval), "=d" (hval));
      x = hval;
      x = (x << 32) | lval;
   #elif defined(_WIN32)
      // This function should not fail, because we checked the QPC
      // when calling to QueryPerformanceFrequency. If it failed,
      // the m_bUseMicroSecond was set to true.
      QueryPerformanceCounter((LARGE_INTEGER *)&x);
   #elif defined(OSX) || (TARGET_OS_IOS == 1) || (TARGET_OS_TV == 1)
      x = mach_absolute_time();
   #else
      // use system call to read time clock for other archs
      x = getTime();
   #endif
}

uint64_t CTimer::readCPUFrequency()
{
   uint64_t frequency = 1;  // 1 tick per microsecond.

#if defined(IA32) || defined(IA64) || defined(AMD64)
    uint64_t t1, t2;

    rdtsc(t1);
    timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000;
    nanosleep(&ts, NULL);
    rdtsc(t2);

    // CPU clocks per microsecond
    frequency = (t2 - t1) / 100000;
#elif defined(_WIN32)
    LARGE_INTEGER counts_per_sec;
    if (QueryPerformanceFrequency(&counts_per_sec))
        frequency = counts_per_sec.QuadPart / 1000000;
#elif defined(OSX) || (TARGET_OS_IOS == 1) || (TARGET_OS_TV == 1)
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    frequency = info.denom * uint64_t(1000) / info.numer;
#endif

   // Fall back to microsecond if the resolution is not high enough.
   if (frequency < 10)
   {
      frequency = 1;
      m_bUseMicroSecond = true;
   }
   return frequency;
}

uint64_t CTimer::getCPUFrequency()
{
   return s_ullCPUFrequency;
}

void CTimer::sleep(uint64_t interval)
{
   uint64_t t;
   rdtsc(t);

   // sleep next "interval" time
   sleepto(t + interval);
}

void CTimer::sleepto(uint64_t nexttime)
{
   // Use class member such that the method can be interrupted by others
   m_ullSchedTime = nexttime;

   uint64_t t;
   rdtsc(t);

   while (t < m_ullSchedTime)
   {
#if USE_BUSY_WAITING
#ifdef IA32
       __asm__ volatile ("pause; rep; nop; nop; nop; nop; nop;");
#elif IA64
       __asm__ volatile ("nop 0; nop 0; nop 0; nop 0; nop 0;");
#elif AMD64
       __asm__ volatile ("nop; nop; nop; nop; nop;");
#elif defined(_WIN32) && !defined(__MINGW__)
       __nop ();
       __nop ();
       __nop ();
       __nop ();
       __nop ();
#endif
#else
       const uint64_t wait_us = (m_ullSchedTime - t) / CTimer::getCPUFrequency();
       // The while loop ensures that (t < m_ullSchedTime).
       // Division by frequency may lose precision, therefore can be 0.
       if (wait_us == 0)
           break;

       timeval now;
       gettimeofday(&now, 0);
       const uint64_t time_us = now.tv_sec * uint64_t(1000000) + now.tv_usec + wait_us;
       timespec timeout;
       timeout.tv_sec = time_us / 1000000;
       timeout.tv_nsec = (time_us % 1000000) * 1000;

       THREAD_PAUSED();
       pthread_mutex_lock(&m_TickLock);
       pthread_cond_timedwait(&m_TickCond, &m_TickLock, &timeout);
       pthread_mutex_unlock(&m_TickLock);
       THREAD_RESUMED();
#endif

       rdtsc(t);
   }
}

void CTimer::interrupt()
{
   // schedule the sleepto time to the current CCs, so that it will stop
   rdtsc(m_ullSchedTime);
   tick();
}

void CTimer::tick()
{
    pthread_cond_signal(&m_TickCond);
}

uint64_t CTimer::getTime()
{
    // XXX Do further study on that. Currently Cygwin is also using gettimeofday,
    // however Cygwin platform is supported only for testing purposes.

    //For other systems without microsecond level resolution, add to this conditional compile
#if defined(OSX) || (TARGET_OS_IOS == 1) || (TARGET_OS_TV == 1)
    // Otherwise we will have an infinite recursive functions calls
    if (m_bUseMicroSecond == false)
    {
        uint64_t x;
        rdtsc(x);
        return x / s_ullCPUFrequency;
    }
    // Specific fix may be necessary if rdtsc is not available either.
    // Going further on Apple platforms might cause issue, fixed with PR #301.
    // But it is very unlikely for the latest platforms.
#endif
    timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec * uint64_t(1000000) + t.tv_usec;
}

void CTimer::triggerEvent()
{
    pthread_cond_signal(&m_EventCond);
}

CTimer::EWait CTimer::waitForEvent()
{
    timeval now;
    timespec timeout;
    gettimeofday(&now, 0);
    if (now.tv_usec < 990000)
    {
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = (now.tv_usec + 10000) * 1000;
    }
    else
    {
        timeout.tv_sec = now.tv_sec + 1;
        timeout.tv_nsec = (now.tv_usec + 10000 - 1000000) * 1000;
    }
    pthread_mutex_lock(&m_EventLock);
    int reason = pthread_cond_timedwait(&m_EventCond, &m_EventLock, &timeout);
    pthread_mutex_unlock(&m_EventLock);

    return reason == ETIMEDOUT ? WT_TIMEOUT : reason == 0 ? WT_EVENT : WT_ERROR;
}

void CTimer::sleep()
{
   #ifndef _WIN32
      usleep(10);
   #else
      Sleep(1);
   #endif
}

int CTimer::condTimedWaitUS(pthread_cond_t* cond, pthread_mutex_t* mutex, uint64_t delay) {
    timeval now;
    gettimeofday(&now, 0);
    const uint64_t time_us = now.tv_sec * uint64_t(1000000) + now.tv_usec + delay;
    timespec timeout;
    timeout.tv_sec = time_us / 1000000;
    timeout.tv_nsec = (time_us % 1000000) * 1000;

    return pthread_cond_timedwait(cond, mutex, &timeout);
}

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
} g_gmtx;
#endif

// Automatically lock in constructor
CGuard::CGuard(pthread_mutex_t& lock, const char* ln SRT_ATR_UNUSED, bool shouldwork):
    m_Mutex(lock),
    m_iLocked(-1)
{
#if ENABLE_THREAD_LOGGING
    std::ostringstream cv;
    cv << &m_Mutex;
    if (ln)
    {
        cv << "(" << ln << ")";
    }
    lockname = cv.str();
    char errbuf[256];
#endif
    if (shouldwork)
    {
        LOGS(cerr, log << "CGuard: { LOCK:" << lockname << " ...");
        Lock();

#if ENABLE_THREAD_ASSERT
        if (m_iLocked != 0)
            abort();
#endif
        LOGS(cerr, log << "... " << lockname << " lock state:" <<
                (m_iLocked == 0 ? "locked successfully" : SysStrError(m_iLocked, errbuf, 256)));
    }
    else
    {
        LOGS(cerr, log << "CGuard: LOCK NOT DONE (not required):" << lockname);
    }
}

// Automatically unlock in destructor
CGuard::~CGuard()
{
    if (m_iLocked == 0)
    {
        LOGS(cerr, log << "CGuard: } UNLOCK:" << lockname);
        Unlock();
    }
    else
    {
        LOGS(cerr, log << "CGuard: UNLOCK NOT DONE (not locked):" << lockname);
    }
}

int CGuard::enterCS(pthread_mutex_t& lock, const char* ln SRT_ATR_UNUSED, bool block)
{
#if ENABLE_THREAD_LOGGING
    std::ostringstream cv;
    cv << &lock;
    if (ln)
    {
        cv << "(" << ln << ")";
    }
    string lockname = cv.str();
#endif
    int retval;
    if (block)
    {
        LOGS(cerr, log << "enterCS(block) {  LOCK: " << lockname << " ...");
        retval = pthread_mutex_lock(&lock);
        LOGS(cerr, log << "... " << lockname << " locked.");
    }
    else
    {
        retval = pthread_mutex_trylock(&lock);
        LOGS(cerr, log << "enterCS(try) {  LOCK: " << lockname << " "
                << (retval == 0 ? " LOCKED." : " FAILED }"));
    }
    return retval;
}

int CGuard::leaveCS(pthread_mutex_t& lock, const char* ln SRT_ATR_UNUSED)
{
#if ENABLE_THREAD_LOGGING
    std::ostringstream cv;
    cv << &lock;
    if (ln)
    {
        cv << "(" << ln << ")";
    }
    string lockname = cv.str();
#endif
    LOGS(cerr, log << "leaveCS: } UNLOCK: " << lockname);
    return pthread_mutex_unlock(&lock);
}

/// This function checks if the given thread id
/// is a thread id, stating that a thread id variable
/// that doesn't hold a running thread, is equal to
/// a null thread (pthread_t()).
bool CGuard::isthread(const pthread_t& thr)
{
    return pthread_equal(thr, pthread_t()) == 0; // NOT equal to a null thread
}

bool CGuard::join(pthread_t& thr)
{
    LOGS(cerr, log << "JOIN: " << thr << " ---> " << pthread_self());
    int ret = pthread_join(thr, NULL);
    thr = pthread_t(); // prevent dangling
    return ret == 0;
}

bool CGuard::join(pthread_t& thr, void*& result)
{
    LOGS(cerr, log << "JOIN: " << thr << " ---> " << pthread_self());
    int ret = pthread_join(thr, &result);
    thr = pthread_t();
    return ret == 0;
}

void CGuard::createMutex(pthread_mutex_t& lock)
{
    pthread_mutexattr_t* pattr = NULL;
#if ENABLE_THREAD_LOGGING
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pattr = &attr;
#endif
    pthread_mutex_init(&lock, pattr);
}

void CGuard::releaseMutex(pthread_mutex_t& lock)
{
    pthread_mutex_destroy(&lock);
}

void CGuard::createCond(pthread_cond_t& cond, pthread_condattr_t* attr)
{
    pthread_cond_init(&cond, attr);
}

void CGuard::releaseCond(pthread_cond_t& cond)
{
    pthread_cond_destroy(&cond);
}

CCondDelegate::CCondDelegate(pthread_cond_t& cond, CGuard& g, const char* ln SRT_ATR_UNUSED)
    : m_cond(&cond), m_mutex(&g.m_Mutex)
#if ENABLE_THREAD_LOGGING
      , nolock(false)
#endif
{
#if ENABLE_THREAD_LOGGING
    // This constructor expects that the mutex is locked, and 'g' should designate
    // the CGuard variable that holds the mutex. Test in debug mode whether the
    // mutex is locked
    std::ostringstream cv;
    cv << &cond;
    if (ln)
    {
        cv << "(" << ln << ")";
    }
    cvname = cv.str();
    lockname = g.lockname;

    int lockst = pthread_mutex_trylock(m_mutex);
    if (lockst == 0)
    {
        pthread_mutex_unlock(m_mutex);
        LOGS(std::cerr, log << "CCond: IPE: Mutex " << g.lockname << " in CGuard IS NOT LOCKED.");
        return;
    }
#endif
    // XXX it would be nice to check whether the owner is also current thread
    // but this can't be done portable way.

    // When constructed by this constructor, the user is expected
    // to only call signal_locked() function. You should pass the same guard
    // variable that you have used for construction as its argument.
}

CCondDelegate::CCondDelegate(pthread_cond_t& cond, pthread_mutex_t& mutex, Nolock,
        const char* cn SRT_ATR_UNUSED, const char* ln SRT_ATR_UNUSED)
    : m_cond(&cond), m_mutex(&mutex)
#if ENABLE_THREAD_LOGGING
      , nolock(true)
#endif
{
#if ENABLE_THREAD_LOGGING
    std::ostringstream cv;
    cv << &m_cond;
    if (cn)
    {
        cv << "(" << cn << ")";
    }
    cvname = cv.str();
    std::ostringstream lv;
    lv << &mutex;
    if (ln)
    {
        lv << "(" << ln << ")";
    }
    lockname = lv.str();
#endif
    // We expect that the mutex is NOT locked at this moment by the current thread,
    // but it is perfectly ok, if the mutex is locked by another thread. We'll just wait.

    // When constructed by this constructor, the user is expected
    // to only call lock_signal() function.
}

void CCondDelegate::wait()
{
    LOGS(cerr, log << "Cond: WAIT:" << cvname << " UNLOCK:" << lockname);
    THREAD_PAUSED();
    pthread_cond_wait(m_cond, m_mutex);
    THREAD_RESUMED();
    LOGS(cerr, log << "Cond: CAUGHT:" << cvname << " LOCKED:" << lockname);
}

/// Block the call until either @a timestamp time achieved
/// or the conditional is signaled.
/// @param [in] timestamp Absolute time (since epoch [us]) to wait up to
/// @retval true Resumed due to getting a CV signal
/// @retval false Resumed due to being past @a timestamp
bool CCondDelegate::wait_until(uint64_t timestamp)
{
    timespec locktime;
    locktime.tv_sec = timestamp / 1000000;
    locktime.tv_nsec = (timestamp % 1000000) * 1000;
    LOGS(cerr, log << "Cond: WAIT:" << cvname << " UNLOCK:" << lockname << " - until TS=" << logging::FormatTime(timestamp));
    THREAD_PAUSED();
    bool signaled = pthread_cond_timedwait(m_cond, m_mutex, &locktime) != ETIMEDOUT;
    THREAD_RESUMED();
    LOGS(cerr, log << "Cond: CAUGHT:" << cvname << " LOCKED:" << lockname << " REASON:" << (signaled ? "SIGNAL" : "TIMEOUT"));
    return signaled;
}

/// Block the call until either @a timestamp time achieved
/// or the conditional is signaled.
/// @param [in] delay Maximum time to wait since the moment of the call
/// @retval true Resumed due to getting a CV signal
/// @retval false Resumed due to being past @a timestamp
bool CCondDelegate::wait_for(uint64_t delay)
{
    timeval now;
    gettimeofday(&now, 0); //  CTimer::getTime ???
    uint64_t time_us = now.tv_sec * uint64_t(1000000) + now.tv_usec + delay;
    return wait_until(time_us);
}

void CCondDelegate::lock_signal()
{
    // We expect nolock == true.
#if ENABLE_THREAD_LOGGING
    if (!nolock)
    {
        LOGS(cerr, log << "Cond: IPE: lock_signal done on LOCKED Cond.");
    }
#endif
    LOGS(cerr, log << "Cond: SIGNAL:" << cvname << " { LOCKING: " << lockname << "...");

    // Not using CGuard here because it would be logged
    // and this will result in unnecessary excessive logging.
    pthread_mutex_lock(m_mutex);
    LOGS(cerr, log << "Cond: ... locked: " << lockname << " - SIGNAL!");
    pthread_cond_signal(m_cond);
    pthread_mutex_unlock(m_mutex);

    LOGS(cerr, log << "Cond: } UNLOCK:" << lockname);
}

void CCondDelegate::signal_locked(CGuard& lk SRT_ATR_UNUSED)
{
    // We expect nolock == false.
#if ENABLE_THREAD_LOGGING
    if (nolock)
    {
        LOGS(cerr, log << "Cond: IPE: signal done on no-lock-checked Cond.");
    }

    if (&lk.m_Mutex != m_mutex)
    {
        LOGS(cerr, log << "Cond: IPE: signal declares CGuard.mutex=" << lk.lockname << " but Cond.mutex=" << lockname);
    }
    LOGS(cerr, log << "Cond: SIGNAL:" << cvname << " (with locked:" << lockname << ")");
#endif

    pthread_cond_signal(m_cond);
}

void CCondDelegate::signal_relaxed()
{
    LOGS(cerr, log << "Cond: SIGNAL:" << cvname << " (NOT locking " << lockname << ")");
    pthread_cond_signal(m_cond);
}

//
CUDTException::CUDTException(CodeMajor major, CodeMinor minor, int err):
m_iMajor(major),
m_iMinor(minor)
{
   if (err == -1)
       m_iErrno = NET_ERROR;
   else
      m_iErrno = err;
}

CUDTException::CUDTException(const CUDTException& e):
m_iMajor(e.m_iMajor),
m_iMinor(e.m_iMinor),
m_iErrno(e.m_iErrno),
m_strMsg()
{
}

CUDTException::~CUDTException()
{
}

const char* CUDTException::getErrorMessage()
{
   // translate "Major:Minor" code into text message.

   switch (m_iMajor)
   {
      case MJ_SUCCESS:
        m_strMsg = "Success";
        break;

      case MJ_SETUP:
        m_strMsg = "Connection setup failure";

        switch (m_iMinor)
        {
        case MN_TIMEOUT:
           m_strMsg += ": connection time out";
           break;

        case MN_REJECTED:
           m_strMsg += ": connection rejected";
           break;

        case MN_NORES:
           m_strMsg += ": unable to create/configure SRT socket";
           break;

        case MN_SECURITY:
           m_strMsg += ": abort for security reasons";
           break;

        default:
           break;
        }

        break;

      case MJ_CONNECTION:
        switch (m_iMinor)
        {
        case MN_CONNLOST:
           m_strMsg = "Connection was broken";
           break;

        case MN_NOCONN:
           m_strMsg = "Connection does not exist";
           break;

        default:
           break;
        }

        break;

      case MJ_SYSTEMRES:
        m_strMsg = "System resource failure";

        switch (m_iMinor)
        {
        case MN_THREAD:
           m_strMsg += ": unable to create new threads";
           break;

        case MN_MEMORY:
           m_strMsg += ": unable to allocate buffers";
           break;

        default:
           break;
        }

        break;

      case MJ_FILESYSTEM:
        m_strMsg = "File system failure";

        switch (m_iMinor)
        {
        case MN_SEEKGFAIL:
           m_strMsg += ": cannot seek read position";
           break;

        case MN_READFAIL:
           m_strMsg += ": failure in read";
           break;

        case MN_SEEKPFAIL:
           m_strMsg += ": cannot seek write position";
           break;

        case MN_WRITEFAIL:
           m_strMsg += ": failure in write";
           break;

        default:
           break;
        }

        break;

      case MJ_NOTSUP:
        m_strMsg = "Operation not supported";
 
        switch (m_iMinor)
        {
        case MN_ISBOUND:
           m_strMsg += ": Cannot do this operation on a BOUND socket";
           break;

        case MN_ISCONNECTED:
           m_strMsg += ": Cannot do this operation on a CONNECTED socket";
           break;

        case MN_INVAL:
           m_strMsg += ": Bad parameters";
           break;

        case MN_SIDINVAL:
           m_strMsg += ": Invalid socket ID";
           break;

        case MN_ISUNBOUND:
           m_strMsg += ": Cannot do this operation on an UNBOUND socket";
           break;

        case MN_NOLISTEN:
           m_strMsg += ": Socket is not in listening state";
           break;

        case MN_ISRENDEZVOUS:
           m_strMsg += ": Listen/accept is not supported in rendezous connection setup";
           break;

        case MN_ISRENDUNBOUND:
           m_strMsg += ": Cannot call connect on UNBOUND socket in rendezvous connection setup";
           break;

        case MN_INVALMSGAPI:
           m_strMsg += ": Incorrect use of Message API (sendmsg/recvmsg).";
           break;

        case MN_INVALBUFFERAPI:
           m_strMsg += ": Incorrect use of Buffer API (send/recv) or File API (sendfile/recvfile).";
           break;

        case MN_BUSY:
           m_strMsg += ": Another socket is already listening on the same port";
           break;

        case MN_XSIZE:
           m_strMsg += ": Message is too large to send (it must be less than the SRT send buffer size)";
           break;

        case MN_EIDINVAL:
           m_strMsg += ": Invalid epoll ID";
           break;

        default:
           break;
        }

        break;

     case MJ_AGAIN:
        m_strMsg = "Non-blocking call failure";

        switch (m_iMinor)
        {
        case MN_WRAVAIL:
           m_strMsg += ": no buffer available for sending";
           break;

        case MN_RDAVAIL:
           m_strMsg += ": no data available for reading";
           break;

        case MN_XMTIMEOUT:
           m_strMsg += ": transmission timed out";
           break;

#ifdef SRT_ENABLE_ECN
        case MN_CONGESTION:
           m_strMsg += ": early congestion notification";
           break;
#endif /* SRT_ENABLE_ECN */
        default:
           break;
        }

        break;

     case MJ_PEERERROR:
        m_strMsg = "The peer side has signalled an error";

        break;

      default:
        m_strMsg = "Unknown error";
   }

   // Adding "errno" information
   if ((MJ_SUCCESS != m_iMajor) && (0 < m_iErrno))
   {
      m_strMsg += ": " + SysStrError(m_iErrno);
   }

   return m_strMsg.c_str();
}

#define UDT_XCODE(mj, mn) (int(mj)*1000)+int(mn)

int CUDTException::getErrorCode() const
{
    return UDT_XCODE(m_iMajor, m_iMinor);
}

int CUDTException::getErrno() const
{
   return m_iErrno;
}


void CUDTException::clear()
{
   m_iMajor = MJ_SUCCESS;
   m_iMinor = MN_NONE;
   m_iErrno = 0;
}

#undef UDT_XCODE

//
bool CIPAddress::ipcmp(const sockaddr* addr1, const sockaddr* addr2, int ver)
{
   if (AF_INET == ver)
   {
      sockaddr_in* a1 = (sockaddr_in*)addr1;
      sockaddr_in* a2 = (sockaddr_in*)addr2;

      if ((a1->sin_port == a2->sin_port) && (a1->sin_addr.s_addr == a2->sin_addr.s_addr))
         return true;
   }
   else
   {
      sockaddr_in6* a1 = (sockaddr_in6*)addr1;
      sockaddr_in6* a2 = (sockaddr_in6*)addr2;

      if (a1->sin6_port == a2->sin6_port)
      {
         for (int i = 0; i < 16; ++ i)
            if (*((char*)&(a1->sin6_addr) + i) != *((char*)&(a2->sin6_addr) + i))
               return false;

         return true;
      }
   }

   return false;
}

void CIPAddress::ntop(const sockaddr_any& addr, uint32_t ip[4])
{
    if (addr.family() == AF_INET)
    {
        ip[0] = addr.sin.sin_addr.s_addr;
    }
    else
    {
      const sockaddr_in6* a = &addr.sin6;
      ip[3] = (a->sin6_addr.s6_addr[15] << 24) + (a->sin6_addr.s6_addr[14] << 16) + (a->sin6_addr.s6_addr[13] << 8) + a->sin6_addr.s6_addr[12];
      ip[2] = (a->sin6_addr.s6_addr[11] << 24) + (a->sin6_addr.s6_addr[10] << 16) + (a->sin6_addr.s6_addr[9] << 8) + a->sin6_addr.s6_addr[8];
      ip[1] = (a->sin6_addr.s6_addr[7] << 24) + (a->sin6_addr.s6_addr[6] << 16) + (a->sin6_addr.s6_addr[5] << 8) + a->sin6_addr.s6_addr[4];
      ip[0] = (a->sin6_addr.s6_addr[3] << 24) + (a->sin6_addr.s6_addr[2] << 16) + (a->sin6_addr.s6_addr[1] << 8) + a->sin6_addr.s6_addr[0];
    }
}

// XXX This has void return and the first argument is passed by reference.
// Consider simply returning sockaddr_any by value.
void CIPAddress::pton(ref_t<sockaddr_any> addr, const uint32_t ip[4], int ver)
{
   if (AF_INET == ver)
   {
      sockaddr_in* a = &addr.get().sin;
      a->sin_addr.s_addr = ip[0];
   }
   else
   {
      sockaddr_in6* a = &addr.get().sin6;
      for (int i = 0; i < 4; ++ i)
      {
         a->sin6_addr.s6_addr[i * 4] = ip[i] & 0xFF;
         a->sin6_addr.s6_addr[i * 4 + 1] = (unsigned char)((ip[i] & 0xFF00) >> 8);
         a->sin6_addr.s6_addr[i * 4 + 2] = (unsigned char)((ip[i] & 0xFF0000) >> 16);
         a->sin6_addr.s6_addr[i * 4 + 3] = (unsigned char)((ip[i] & 0xFF000000) >> 24);
      }
   }
}

using namespace std;


static string ShowIP4(const sockaddr_in* sin)
{
    ostringstream os;
    union
    {
        in_addr sinaddr;
        unsigned char ip[4];
    };
    sinaddr = sin->sin_addr;

    os << int(ip[0]);
    os << ".";
    os << int(ip[1]);
    os << ".";
    os << int(ip[2]);
    os << ".";
    os << int(ip[3]);
    return os.str();
}

static string ShowIP6(const sockaddr_in6* sin)
{
    ostringstream os;
    os.setf(ios::uppercase);

    bool sep = false;
    for (size_t i = 0; i < 16; ++i)
    {
        int v = sin->sin6_addr.s6_addr[i];
        if ( v )
        {
            if ( sep )
                os << ":";

            os << hex << v;
            sep = true;
        }
    }

    return os.str();
}

string CIPAddress::show(const sockaddr* adr)
{
    if ( adr->sa_family == AF_INET )
        return ShowIP4((const sockaddr_in*)adr);
    else if ( adr->sa_family == AF_INET6 )
        return ShowIP6((const sockaddr_in6*)adr);
    else
        return "(unsupported sockaddr type)";
}

//
void CMD5::compute(const char* input, unsigned char result[16])
{
   md5_state_t state;

   md5_init(&state);
   md5_append(&state, (const md5_byte_t *)input, strlen(input));
   md5_finish(&state, result);
}

std::string MessageTypeStr(UDTMessageType mt, uint32_t extt)
{
    using std::string;

    static const char* const udt_types [] = {
        "handshake",
        "keepalive",
        "ack",
        "lossreport",
        "cgwarning", //4
        "shutdown",
        "ackack",
        "dropreq",
        "peererror", //8
    };

    static const char* const srt_types [] = {
        "EXT:none",
        "EXT:hsreq",
        "EXT:hsrsp",
        "EXT:kmreq",
        "EXT:kmrsp",
        "EXT:sid",
        "EXT:congctl"
    };


    if ( mt == UMSG_EXT )
    {
        if ( extt >= Size(srt_types) )
            return "EXT:unknown";

        return srt_types[extt];
    }

    if ( size_t(mt) > Size(udt_types) )
        return "unknown";

    return udt_types[mt];
}

std::string ConnectStatusStr(EConnectStatus cst)
{
    return
          cst == CONN_CONTINUE ? "INDUCED/CONCLUDING"
        : cst == CONN_RUNNING ? "RUNNING"
        : cst == CONN_ACCEPT ? "ACCEPTED"
        : cst == CONN_RENDEZVOUS ? "RENDEZVOUS (HSv5)"
        : cst == CONN_AGAIN ? "AGAIN"
        : cst == CONN_CONFUSED ? "MISSING HANDSHAKE"
        : "REJECTED";
}

std::string TransmissionEventStr(ETransmissionEvent ev)
{
    static const char* const vals [] =
    {
        "init",
        "ack",
        "ackack",
        "lossreport",
        "checktimer",
        "send",
        "receive",
        "custom"
    };

    size_t vals_size = Size(vals);

    if (size_t(ev) >= vals_size)
        return "UNKNOWN";
    return vals[ev];
}

extern const char* const srt_rejectreason_msg [] = {
    "Unknown or erroneous",
    "Error in system calls",
    "Peer rejected connection",
    "Resource allocation failure",
    "Rogue peer or incorrect parameters",
    "Listener's backlog exceeded",
    "Internal Program Error",
    "Socket is being closed",
    "Peer version too old",
    "Rendezvous-mode cookie collision",
    "Incorrect passphrase",
    "Password required or unexpected",
    "MessageAPI/StreamAPI collision",
    "Congestion controller type collision",
    "Packet Filter type collision"
};

const char* srt_rejectreason_str(SRT_REJECT_REASON rid)
{
    int id = rid;
    static const size_t ra_size = Size(srt_rejectreason_msg);
    if (size_t(id) >= ra_size)
        return srt_rejectreason_msg[0];
    return srt_rejectreason_msg[id];
}

// Some logging imps
#if ENABLE_LOGGING

namespace srt_logging
{

std::string FormatTime(uint64_t time)
{
    using namespace std;

    time_t sec = time/1000000;
    time_t usec = time%1000000;

    time_t tt = sec;
    struct tm tm = SysLocalTime(tt);

    char tmp_buf[512];
    strftime(tmp_buf, 512, "%X.", &tm);

    ostringstream out;
    out << tmp_buf << setfill('0') << setw(6) << usec;
    return out.str();
}

LogDispatcher::Proxy::Proxy(LogDispatcher& guy) : that(guy), that_enabled(that.CheckEnabled())
{
    if (that_enabled)
    {
        i_file = "";
        i_line = 0;
        flags = that.src_config->flags;
        // Create logger prefix
        that.CreateLogLinePrefix(os);
    }
}

LogDispatcher::Proxy LogDispatcher::operator()()
{
    return Proxy(*this);
}

void LogDispatcher::CreateLogLinePrefix(std::ostringstream& serr)
{
    using namespace std;

    char tmp_buf[512];
    if ( !isset(SRT_LOGF_DISABLE_TIME) )
    {
        // Not necessary if sending through the queue.
        timeval tv;
        gettimeofday(&tv, 0);
        struct tm tm = SysLocalTime((time_t) tv.tv_sec);

        strftime(tmp_buf, 512, "%X.", &tm);
        serr << tmp_buf << setw(6) << setfill('0') << tv.tv_usec;
    }

    string out_prefix;
    if ( !isset(SRT_LOGF_DISABLE_SEVERITY) )
    {
        out_prefix = prefix;
    }

    // Note: ThreadName::get needs a buffer of size min. ThreadName::BUFSIZE
    if ( !isset(SRT_LOGF_DISABLE_THREADNAME) && ThreadName::get(tmp_buf) )
    {
        serr << "/" << tmp_buf << out_prefix << ": ";
    }
    else
    {
        serr << out_prefix << ": ";
    }
}

std::string LogDispatcher::Proxy::ExtractName(std::string pretty_function)
{
    if ( pretty_function == "" )
        return "";
    size_t pos = pretty_function.find('(');
    if ( pos == std::string::npos )
        return pretty_function; // return unchanged.

    pretty_function = pretty_function.substr(0, pos);

    // There are also template instantiations where the instantiating
    // parameters are encrypted inside. Therefore, search for the first
    // open < and if found, search for symmetric >.

    int depth = 1;
    pos = pretty_function.find('<');
    if ( pos != std::string::npos )
    {
        size_t end = pos+1;
        for(;;)
        {
            ++pos;
            if ( pos == pretty_function.size() )
            {
                --pos;
                break;
            }
            if ( pretty_function[pos] == '<' )
            {
                ++depth;
                continue;
            }

            if ( pretty_function[pos] == '>' )
            {
                --depth;
                if ( depth <= 0 )
                    break;
                continue;
            }
        }

        std::string afterpart = pretty_function.substr(pos+1);
        pretty_function = pretty_function.substr(0, end) + ">" + afterpart;
    }

    // Now see how many :: can be found in the name.
    // If this occurs more than once, take the last two.
    pos = pretty_function.rfind("::");

    if ( pos == std::string::npos || pos < 2 )
        return pretty_function; // return whatever this is. No scope name.

    // Find the next occurrence of :: - if found, copy up to it. If not,
    // return whatever is found.
    pos -= 2;
    pos = pretty_function.rfind("::", pos);
    if ( pos == std::string::npos )
        return pretty_function; // nothing to cut

    return pretty_function.substr(pos+2);
}

} // (end namespace srt_logging)

#endif
