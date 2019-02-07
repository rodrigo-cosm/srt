/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

#ifndef INC__COMMON_TRANSMITMEDIA_HPP
#define INC__COMMON_TRANSMITMEDIA_HPP

#include <string>
#include <map>
#include <stdexcept>
#include <deque>

#include "testmediabase.hpp"
#include <udt.h> // Needs access to CUDTException

using namespace std;

const logging::LogFA SRT_LOGFA_APP = 10;
extern logging::Logger applog;

// Trial version of an exception. Try to implement later an official
// interruption mechanism in SRT using this.

struct TransmissionError: public std::runtime_error
{
    TransmissionError(const std::string& arg):
        std::runtime_error(arg)
    {
    }
};

class SrtCommon
{
    void SpinWaitAsync();

protected:

    struct Connection
    {
        string host;
        int port = 0;
        SRTSOCKET socket = SRT_INVALID_SOCK;
        SRT_SOCKSTATUS status = SRTS_NONEXIST;
        sockaddr_storage peeraddr;
        int result = -1;
        int errorcode = SRT_SUCCESS;

        // Add for caller
        Connection(string h, int p): host(h), port(p) {}

        // Add for listener
        Connection(SRTSOCKET s): socket(s), status(SRTS_CONNECTED), result(0)
        {}
    };

    int srt_epoll = -1;
    int srt_listener_epoll = -1;
    SRT_EPOLL_OPT m_direction = SRT_EPOLL_OPT_NONE; //< Defines which of SND or RCV option variant should be used, also to set SRT_SENDER for output
    bool m_blocking_mode = true; //< enforces using SRTO_SNDSYN or SRTO_RCVSYN, depending on @a m_direction
    int m_timeout = 0; //< enforces using SRTO_SNDTIMEO or SRTO_RCVTIMEO, depending on @a m_direction
    bool m_tsbpdmode = true;
    int m_outgoing_port = 0;
    string m_mode;
    string m_adapter;
    map<string, string> m_options; // All other options, as provided in the URI
    vector<Connection> m_links;
    string m_group_type;
    TransportPacket* m_group_wrapper = nullptr;
    bool m_wrapper_passthru = false;

    struct ReadPos
    {
        uint16_t sequence;
        bytevector packet;
    };
    map<SRTSOCKET, ReadPos> m_group_positions;
    int32_t m_group_rcvseq = -1;
    SRTSOCKET m_group_active; // The link from which the last packet was delivered

    SRTSOCKET m_listener = SRT_INVALID_SOCK;
    bool m_listener_group = false;
    bool IsUsable() { SRT_SOCKSTATUS st = srt_getsockstate(Socket()); return st > SRTS_INIT && st < SRTS_BROKEN; }
    bool IsBroken() { return srt_getsockstate(Socket()) > SRTS_CONNECTED; }

public:
    void InitParameters(string host, string path, map<string,string> par);
    void PrepareListener(string host, int port, int backlog);
    void StealFrom(SrtCommon& src);
    Connection& AcceptNewClient();

    SRTSOCKET Socket() { return m_links.empty() ? SRT_INVALID_SOCK : m_links[0].socket; }
    SRTSOCKET Listener() { return m_listener; }

    virtual void Close();

protected:

    void Error(UDT::ERRORINFO& udtError, string src);
    void Error(int errorcode, string src)
    {
        auto major = CodeMajor(errorcode/1000);
        auto minor = CodeMinor(errorcode%1000);
        UDT::ERRORINFO i(major, minor, 0);
        return Error(i, src);
    }
    void Error(string msg);
    void Init(string host, int port, string path, map<string,string> par, SRT_EPOLL_OPT dir);
    void AddPoller(SRTSOCKET socket, int modes);
    virtual int ConfigurePost(SRTSOCKET sock);
    virtual int ConfigurePre(SRTSOCKET sock);
    int ConfigurePre(SRTSOCKET sock, bool blocking);

    void OpenClient(string host, int port);
    void OpenGroupClient();
    SRTSOCKET PrepareClient(bool blocking);
    void SetupAdapter(SRTSOCKET sock, const std::string& host, int port);
    void ConnectClient(SRTSOCKET sock, string host, int port);
    void SetupRendezvous(SRTSOCKET sock, string adapter, int port);

    void UpdateGroupConnections();
    void RefreshGroupStatus();

    void OpenServer(string host, int port)
    {
        // For group connections, use backlog = 10 because it will have
        // to handle multiple connections.
        PrepareListener(host, port, m_group_type == "" ? 1 : 10);
        AcceptNewClient();
    }

    SRTSOCKET OpenRendezvous(string adapter, string host, int port)
    {
        SRTSOCKET sock = PrepareClient(m_blocking_mode);
        SetupRendezvous(sock, adapter, port);
        ConnectClient(sock, host, port);
        m_links.push_back(sock);
        return sock;
    }

    virtual ~SrtCommon();
};


class SrtSource: public virtual Source, public virtual SrtCommon
{
    std::string hostport_copy;
public:

    SrtSource(std::string host, int port, std::string path, const std::map<std::string,std::string>& par);
    SrtSource()
    {
        // Do nothing - create just to prepare for use
    }

    bytevector Read(size_t chunk) override;
    bytevector GroupRead(size_t chunk);
    bool GroupCheckPacketAhead(bytevector& output);


    /*
       In this form this isn't needed.
       Unblock if any extra settings have to be made.
    virtual int ConfigurePre(UDTSOCKET sock) override
    {
        int result = SrtCommon::ConfigurePre(sock);
        if ( result == -1 )
            return result;
        return 0;
    }
    */

    bool IsOpen() override { return IsUsable(); }
    bool End() override { return IsBroken(); }
    void Close() override { return SrtCommon::Close(); }
};

class SrtTarget: public virtual Target, public virtual SrtCommon
{
public:

    SrtTarget(std::string host, int port, std::string path, const std::map<std::string,std::string>& par);
    SrtTarget() {}

    int ConfigurePre(int sock) override;
    int ConfigurePre(SRTSOCKET sock, bool blocking);
    void Write(const bytevector& data) override;
    void GroupWrite(const bytevector& data);
    bool IsOpen() override { return IsUsable(); }
    bool Broken() override { return IsBroken(); }
    void Close() override { return SrtCommon::Close(); }

    size_t Still() override
    {
        size_t bytes;
        int st = srt_getsndbuffer(Socket(), nullptr, &bytes);
        if (st == -1)
            return 0;
        return bytes;
    }

};

class SrtRelay: public Relay, public SrtSource, public SrtTarget
{
public:
    SrtRelay(std::string host, int port, std::string path, const std::map<std::string,std::string>& par);
    SrtRelay() {}

    int ConfigurePre(SRTSOCKET sock) override
    {
        // This overrides the change introduced in SrtTarget,
        // which sets the SRTO_SENDER flag. For a bidirectional transmission
        // this flag should not be set, as the connection should be checked
        // for being 1.3.0 clients only.
        return SrtCommon::ConfigurePre(sock);
    }

    // Override separately overridden methods by SrtSource and SrtTarget
    bool IsOpen() override { return IsUsable(); }
    void Close() override { return SrtCommon::Close(); }
};


// This class is used when we don't know yet whether the given URI
// designates an effective listener or caller. So we create it, initialize,
// then we know what mode we'll be using.
//
// When caller, then we will do connect() using this object, then clone out
// a new object - of a direction specific class - which will steal the socket
// from this one and then roll the data. After this, this object is ready
// to connect again, and will create its own socket for that occasion, and
// the whole procedure repeats.
//
// When listener, then this object will be doing accept() and with every
// successful acceptation it will clone out a new object - of a direction
// specific class - which will steal just the connection socket from this
// object. This object will still live on and accept new connections and
// so on.
class SrtModel: public SrtCommon
{
public:
    bool is_caller = false;
    bool is_rend = false;
    string m_host;
    int m_port = 0;


    SrtModel(string host, int port, map<string,string> par);
    void Establish(ref_t<std::string> name);

    void Close()
    {
        for (auto& l: m_links)
        {
            srt_close(l.socket);
        }
        m_links.clear();
    }
};

class RTPTransportPacket: public TransportPacket
{
    // 'payload' is derived, add RTP headers here:
    int32_t seq = -1;
    int32_t src = -1;

    uint64_t timebase = 0;
    uint32_t pkt_ts = 0;

    int header_size = 0;

    static const uint16_t indata =
        (2 << 6) /* version, other flags 0 */ << 8
          | 96 /* RTP_PT_PRIVATE */;


    static const size_t HDR_SIZE = 
        2 + // initial fields (version, type, csrc len, marks)
        2 + // sequence
        4 + // timestamp
        4;  // srcid

public:

    RTPTransportPacket(const std::string& )
    {
        // Parse options later, currently define the size normally
        // as without any contributing sources provided.

        // This is the basic size of an RTP packet, this can be
        // followed by a defined number of contributing sources,
        // each one getting 4 bytes. Bits 4-7 of the RTP header
        // contains the number of contributing sources. Currently
        // we state this number is 0.
        header_size = 12;
    }

    virtual int plsize() override
    {
        return header_size + SRT_LIVE_DEF_PLSIZE;
    }

    virtual void load(const bytevector&, size_t size = ~size_t()) override;
    virtual void save(bytevector& out) override;

    virtual int32_t& seqno() override { return seq; }
    virtual int32_t& srcid() override { return src; }
};


// Set tools
namespace set_op {

    // VALUE % SET : VALUE is contained in SET
template <class Value, class Container>
inline bool operator%(const Value& v, const Container& c)
{
    return c.count(v) != 0;
}

    // VALUE / SET : VALUE is not contained in SET
template <class Value, class Container>
inline bool operator/(const Value& v, const Container& c)
{
    return c.count(v) == 0;
}

}

#endif
