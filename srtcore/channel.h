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
Copyright (c) 2001 - 2011, The Board of Trustees of the University of Illinois.
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
   Yunhong Gu, last updated 01/27/2011
modified by
   Haivision Systems Inc.
*****************************************************************************/

#ifndef __UDT_CHANNEL_H__
#define __UDT_CHANNEL_H__


#include "udt.h"
#include "packet.h"
#include "netinet_any.h"
#include <utility>

   // Constants used for signal reading that passes
   // extra information
enum PipeSignal
{
    PSG_CLOSE = 0,
    PSG_NEWUNIT = 1,
    PSG_NEWCONN = 2,
    PSG_NONE = -1
};


class EventRunner
{
    // Note: Socket is only "borrowed" from Channel, exists
    // here in order to have access to it. It should not be
    // created nor deleted.
    int m_fdSocket;

#ifdef _WIN32

    // This state is kept as a field here because the Windows
    // event system does not allow for any data passing for events.
    // So the idea is to set this value to required value and then
    // set the event state. The value of the state can be then
    // retrieved from here.
    mutable volatile PipeSignal m_state;
    PipeSignal m_permstate;
    int m_sockstate;
    enum {WE_TRIGGER = 0, WE_SOCKET = 1};
    WSAEVENT m_Event[2];
#else // Standard POSIX version
   enum {PIPE_IN = 0, PIPE_OUT = 1};
   int m_fdTrigger[2];                      // descriptor for read-end of the pipe used to break reading

   // This is to get and keep output from ::select
   // so that it can be returned when asked for state.
   fd_set in_set, err_set;
#endif

public:

   // Set everything to initial nothing
   EventRunner():
#ifdef _WIN32
   m_fdSocket(INVALID_SOCKET),
   m_state(PSG_NONE),
   m_permstate(PSG_NONE),
   m_sockstate(0)
   {
	   m_Event[0] = m_Event[1] = INVALID_HANDLE_VALUE;
   }

#else
   m_fdSocket(-1)
   {
       m_fdTrigger[0] = m_fdTrigger[1] = -1;
   }

#endif

   // To be bound to Channel's constructor
   void init(int sock);

   int signalReading(PipeSignal val) const;

   void poll(int64_t timeout);

   // The implementation should call clearSignalReading
   // and socketReady. The first tells it as to whether
   // the event was triggered (and what kind of), or not.
   // This will be returned once and the event will be 
   // cleared thereafter.
   // The latter if the socket is ready for extraction.
   PipeSignal clearSignalReading();
   int socketReady() const;

   ~EventRunner();

};

class CChannel
{
public:

    // Maximum time to hangup in polling: 0.5s
    static const uint64_t MAX_POLL_TIME_US = 500000;

   // XXX There's currently no way to access the socket ID set for
   // whatever the channel is currently working for. Required to find
   // some way to do this, possibly by having a "reverse pointer".
   // Currently just "unimplemented".
   std::string CONID() const { return ""; }

   //CChannel();
   CChannel(int version);
   ~CChannel();

      /// Open a UDP channel.
      /// @param [in] addr The local address that UDP will use.

   void open(const sockaddr* addr = NULL);

      /// Open a UDP channel based on an existing UDP socket.
      /// @param [in] udpsock UDP socket descriptor.

   void attach(UDPSOCKET udpsock);

      /// Disconnect and close the UDP entity.

   void close() const;

      /// Get the UDP sending buffer size.
      /// @return Current UDP sending buffer size.

   int getSndBufSize();

      /// Get the UDP receiving buffer size.
      /// @return Current UDP receiving buffer size.

   int getRcvBufSize();

      /// Set the UDP sending buffer size.
      /// @param [in] size expected UDP sending buffer size.

   void setSndBufSize(int size);

      /// Set the UDP receiving buffer size.
      /// @param [in] size expected UDP receiving buffer size.

   void setRcvBufSize(int size);

      /// Query the socket address that the channel is using.
      /// @param [out] addr pointer to store the returned socket address.

   void getSockAddr(sockaddr* addr) const;

      /// Query the peer side socket address that the channel is connect to.
      /// @param [out] addr pointer to store the returned socket address.

   void getPeerAddr(sockaddr* addr) const;

      /// Send a packet to the given address.
      /// @param [in] addr pointer to the destination address.
      /// @param [in] packet reference to a CPacket entity.
      /// @return Actual size of data sent.

   int sendto(const sockaddr* addr, CPacket& packet) const;

      /// Receive a packet from the channel and record the source address.
      /// @param [in] addr pointer to the source address.
      /// @param [in] packet reference to a CPacket entity.
      /// @return Actual size of data received.

   EReadStatus recvfrom(sockaddr* addr, CPacket& packet, uint64_t uptime_us) const;

#ifdef SRT_ENABLE_IPOPTS
      /// Set the IP TTL.
      /// @param [in] ttl IP Time To Live.
      /// @return none.

   void setIpTTL(int ttl);

      /// Set the IP Type of Service.
      /// @param [in] tos IP Type of Service.

   void setIpToS(int tos);

      /// Get the IP TTL.
      /// @param [in] ttl IP Time To Live.
      /// @return TTL.

   int getIpTTL() const;

      /// Get the IP Type of Service.
      /// @return ToS.

   int getIpToS() const;
#endif

   int ioctlQuery(int type) const;
   int sockoptQuery(int level, int option) const;

   const sockaddr* bindAddress() { return &m_BindAddr; }
   const sockaddr_any& bindAddressAny() { return m_BindAddr; }

   mutable EventRunner m_EventRunner;

private:
   void setUDPSockOpt();

   EReadStatus sys_recvmsg(ref_t<CPacket> r_pkt, ref_t<int> r_result, ref_t<int> r_msg_flags, sockaddr* addr) const;

private:
   int m_iIPversion;                    // IP version
   int m_iSockAddrSize;                 // socket address structure size (pre-defined to avoid run-time test)

   UDPSOCKET m_iSocket;                 // socket descriptor
#ifdef SRT_ENABLE_IPOPTS
   int m_iIpTTL;
   int m_iIpToS;
#endif
   int m_iSndBufSize;                   // UDP sending buffer size
   int m_iRcvBufSize;                   // UDP receiving buffer size
   sockaddr_any m_BindAddr;
};


#endif
