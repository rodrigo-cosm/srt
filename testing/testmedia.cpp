/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

// Medium concretizations

// Just for formality. This file should be used 
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <iterator>
#include <map>
#include <srt.h>
#if !defined(_WIN32)
#include <sys/ioctl.h>
#endif

#include "netinet_any.h"
#include "common.h"
#include "api.h"
#include "udt.h"
#include "logging.h"
#include "utilities.h"

#include "apputil.hpp"
#include "socketoptions.hpp"
#include "uriparser.hpp"
#include "testmedia.hpp"
#include "srt_compat.h"
#include "verbose.hpp"

using namespace std;

std::ostream* transmit_cverb = nullptr;
volatile bool transmit_throw_on_interrupt = false;
int transmit_bw_report = 0;
unsigned transmit_stats_report = 0;
size_t transmit_chunk_size = SRT_LIVE_DEF_PLSIZE;

logging::Logger applog(SRT_LOGFA_APP, srt_logger_config, "srt-test");

static string DisplayEpollResults(const std::set<SRTSOCKET>& sockset, std::string prefix)
{
    typedef set<SRTSOCKET> fset_t;
    ostringstream os;
    os << prefix << " ";
    for (fset_t::const_iterator i = sockset.begin(); i != sockset.end(); ++i)
    {
        os << "@" << *i << " ";
    }

    return os.str();
}

string DirectionName(SRT_EPOLL_OPT direction)
{
    string dir_name;
    if (direction & ~SRT_EPOLL_ERR)
    {
        if (direction & SRT_EPOLL_IN)
        {
            dir_name = "source";
        }

        if (direction & SRT_EPOLL_OUT)
        {
            if (!dir_name.empty())
                dir_name = "relay";
            else
                dir_name = "target";
        }

        if (direction & SRT_EPOLL_ERR)
        {
            dir_name += "+error";
        }
    }
    else
    {
        // stupid name for a case of IPE
        dir_name = "stone";
    }

    return dir_name;
}

template<class FileBase> inline
bytevector FileRead(FileBase& ifile, size_t chunk, const string& filename)
{
    bytevector data(chunk);
    ifile.read(data.data(), chunk);
    size_t nread = ifile.gcount();
    if ( nread < data.size() )
        data.resize(nread);

    if ( data.empty() )
        throw Source::ReadEOF(filename);
    return data;
}


class FileSource: public virtual Source
{
    ifstream ifile;
    string filename_copy;
public:

    FileSource(const string& path): ifile(path, ios::in | ios::binary), filename_copy(path)
    {
        if ( !ifile )
            throw std::runtime_error(path + ": Can't open file for reading");
    }

    bytevector Read(size_t chunk) override { return FileRead(ifile, chunk, filename_copy); }

    bool IsOpen() override { return bool(ifile); }
    bool End() override { return ifile.eof(); }
    //~FileSource() { ifile.close(); }
};

class FileTarget: public virtual Target
{
    ofstream ofile;
public:

    FileTarget(const string& path): ofile(path, ios::out | ios::trunc | ios::binary) {}

    void Write(const bytevector& data) override
    {
        ofile.write(data.data(), data.size());
    }

    bool IsOpen() override { return !!ofile; }
    bool Broken() override { return !ofile.good(); }
    //~FileTarget() { ofile.close(); }
    void Close() override { ofile.close(); }
};

// Can't base this class on FileSource and FileTarget classes because they use two
// separate fields, which makes it unable to reliably define IsOpen(). This would
// require to use 'fstream' type field in some kind of FileCommon first. Not worth
// a shot.
class FileRelay: public Relay
{
    fstream iofile;
    string filename_copy;
public:

    FileRelay(const string& path):
        iofile(path, ios::in | ios::out | ios::binary), filename_copy(path)
    {
        if ( !iofile )
            throw std::runtime_error(path + ": Can't open file for reading");
    }
    bytevector Read(size_t chunk) override { return FileRead(iofile, chunk, filename_copy); }

    void Write(const bytevector& data) override
    {
        iofile.write(data.data(), data.size());
    }

    bool IsOpen() override { return !!iofile; }
    bool End() override { return iofile.eof(); }
    bool Broken() override { return !iofile.good(); }
    void Close() override { iofile.close(); }
};

template <class Iface> struct File;
template <> struct File<Source> { typedef FileSource type; };
template <> struct File<Target> { typedef FileTarget type; };
template <> struct File<Relay> { typedef FileRelay type; };

template <class Iface>
Iface* CreateFile(const string& name) { return new typename File<Iface>::type (name); }


template <class PerfMonType>
void PrintSrtStats(int sid, const PerfMonType& mon)
{
    Verb() << "======= SRT STATS: sid=" << sid;
    Verb() << "PACKETS SENT: " << mon.pktSent << " RECEIVED: " << mon.pktRecv;
    Verb() << "LOST PKT SENT: " << mon.pktSndLoss << " RECEIVED: " << mon.pktRcvLoss;
    Verb() << "REXMIT SENT: " << mon.pktRetrans << " RECEIVED: " << mon.pktRcvRetrans;
    Verb() << "RATE SENDING: " << mon.mbpsSendRate << " RECEIVING: " << mon.mbpsRecvRate;
    Verb() << "BELATED RECEIVED: " << mon.pktRcvBelated << " AVG TIME: " << mon.pktRcvAvgBelatedTime;
    Verb() << "REORDER DISTANCE: " << mon.pktReorderDistance;
    Verb() << "WINDOW: FLOW: " << mon.pktFlowWindow << " CONGESTION: " << mon.pktCongestionWindow << " FLIGHT: " << mon.pktFlightSize;
    Verb() << "RTT: " << mon.msRTT << "ms  BANDWIDTH: " << mon.mbpsBandwidth << "Mb/s\n";
    Verb() << "BUFFERLEFT: SND: " << mon.byteAvailSndBuf << " RCV: " << mon.byteAvailRcvBuf;
}


void SrtCommon::InitParameters(string host, string path, map<string,string> par)
{
    // Application-specific options: mode, blocking, timeout, adapter
    if ( Verbose::on )
    {
        Verb() << "Parameters:\n";
        for (map<string,string>::iterator i = par.begin(); i != par.end(); ++i)
        {
            Verb() << "\t" << i->first << " = '" << i->second << "'\n";
        }
    }

    if (path != "")
    {
        // Special case handling of an unusual specification.

        if (path.substr(0, 2) != "//")
        {
            Error("Path specification not supported for SRT (use // in front for special cases)");
        }

        path = path.substr(2);

        if (path == "group")
        {
            // Group specified, check type.
            m_group_type = par["type"];
            if (m_group_type == "")
            {
                Error("With //group, the group 'type' must be specified.");
            }

            if (m_group_type != "redundancy")
            {
                Error("With //group, only type=redundancy is currently supported");
            }

            string group_wrapper = par["wrapper"];
            if (group_wrapper != "")
            {
                vector<string> wrpoptions;
                Split(group_wrapper, '/', back_inserter(wrpoptions));
                group_wrapper = wrpoptions[0];

                // Might be that the wrapper name is empty,
                // but the passthrough option is given. This means
                // that the payload has to be used "as is" (we state
                // that the input medium has already provided a
                // protocol that the other side will understand).
                string options;
                if (wrpoptions.size() > 1)
                {
                    options = wrpoptions[1];
                    if (options[0] == 'p') // passthru
                        m_wrapper_passthru = true;
                }

                // XXX Simple dispatcher, expand later
                if (group_wrapper == "rtp")
                    m_group_wrapper = new RTPTransportPacket(options);
                else if (group_wrapper != "")
                    Error("Unknown wrapper type");

            }

            vector<string> nodes;
            Split(par["nodes"], ',', back_inserter(nodes));

            if (nodes.empty())
            {
                Error("With //group, 'nodes' must specify comma-separated host:port specs.");
            }

            // Check if correctly specified
            for (string& hostport: nodes)
            {
                if (hostport == "")
                    continue;
                UriParser check(hostport, UriParser::EXPECT_HOST);
                if (check.host() == "" || check.port() == "")
                {
                    Error("With //group, 'nodes' must specify comma-separated host:port specs.");
                }

                if (check.portno() <= 1024)
                {
                    Error("With //group, every node in 'nodes' must have port >1024");
                }

                m_links.push_back(Connection(check.host(), check.portno()));
            }

            par.erase("type");
            par.erase("nodes");
            par.erase("wrapper");
        }
    }

    m_mode = "default";
    if ( par.count("mode") )
        m_mode = par.at("mode");

    if ( m_mode == "default" )
    {
        // Use the following convention:
        // 1. If host is empty, then listener.
        // 2. If host is specified, then caller.
        // 3. If listener is enforced, host -> adapter
        // 4. If caller is enforced, host = localhost.
        if ( host == "" && m_links.empty() )
            m_mode = "listener";
        else
            m_mode = "caller";
    }

    if ( m_mode == "client" )
        m_mode = "caller";
    else if ( m_mode == "server" )
        m_mode = "listener";

    if (m_mode == "listener" && !m_links.empty())
    {
        Error("Multiple nodes (redundant links) only supported in CALLER (client) mode.");
    }

    par.erase("mode");

    if ( par.count("blocking") )
    {
        m_blocking_mode = !false_names.count(par.at("blocking"));
        par.erase("blocking");
    }

    if ( par.count("timeout") )
    {
        m_timeout = stoi(par.at("timeout"), 0, 0);
        par.erase("timeout");
    }

    if ( par.count("adapter") )
    {
        m_adapter = par.at("adapter");
        par.erase("adapter");
    }
    else if (m_mode == "listener")
    {
        // For listener mode, adapter is taken from host,
        // if 'adapter' parameter is not given
        m_adapter = host;
    }

    if ( par.count("tsbpd") && false_names.count(par.at("tsbpd")) )
    {
        m_tsbpdmode = false;
    }

    if (par.count("port"))
    {
        m_outgoing_port = stoi(par.at("port"), 0, 0);
        par.erase("port");
    }

    // That's kinda clumsy, but it must rely on the defaults.
    // Default mode is live, so check if the file mode was enforced
    if (par.count("transtype") == 0 || par["transtype"] != "file")
    {
        if (par.count("payloadsize"))
        {
            size_t s = stoi(par["payloadsize"]);
            if (s != transmit_chunk_size)
            {
                cerr << "WARNING: Option -chunk specifies different size than 'payloadsize' parameter in SRT URI\n";
            }
            transmit_chunk_size = s;
        }

        // If the Live chunk size was nondefault, enforce the size.
        if (transmit_chunk_size != SRT_LIVE_DEF_PLSIZE)
        {
            if (transmit_chunk_size > SRT_LIVE_MAX_PLSIZE)
                throw std::runtime_error("Chunk size in live mode exceeds 1456 bytes; this is not supported");

            par["payloadsize"] = Sprint(transmit_chunk_size);
        }
    }

    if (m_mode == "listener")
    {
        // Group specifications

        if (par.count("wrapper"))
        {
            string group_wrapper = par["wrapper"];
            vector<string> wrpoptions;
            Split(group_wrapper, '/', back_inserter(wrpoptions));
            group_wrapper = wrpoptions[0];

            string options;
            if (wrpoptions.size() > 1)
            {
                options = wrpoptions[1];
                if (options[0] == 'p') // passthru
                    m_wrapper_passthru = true;
            }

            // XXX Simple dispatcher, expand later
            if (group_wrapper == "rtp")
                m_group_wrapper = new RTPTransportPacket(options);
            else if (group_wrapper != "")
                throw std::runtime_error("Unknown wrapper type");

            // Might be that the wrapper name is empty,
            // but the passthrough option is given. This means
            // that the payload has to be used "as is" (we state
            // that the input medium has already provided a
            // protocol that the other side will understand).

            Verb() << "Group listener: wrapper: " << group_wrapper << " options: " << options;
            m_listener_group = true;

            // With wrapper, you define a group, type must be also defined.

            m_group_type = par["type"];
            if (m_group_type == "")
            {
                m_group_type = "redundancy";
            }

            if (m_group_type != "redundancy")
            {
                Error("With group listener, only type=redundancy is currently supported");
            }
        }
    }

    if (m_group_wrapper)
    {
        // Extract the payload size from the wrapper default payload size.
        int def_plsize = m_group_wrapper->plsize();

        Verb() << "WRAPPER's PAYLOADSIZE=" << def_plsize;

        if (par.count("payloadsize"))
        {
            // Check if this defined payload size isn't less than the
            // minimum required by the wrapper.
            int s = stoi(par["payloadsize"]);
            if (s < def_plsize)
            {
                Error("The 'payloadsize' parameter in SRT URI provides less size than minimum required by the 'wrapper'");
            }
        }
        else if (def_plsize > int(transmit_chunk_size))
        {
            transmit_chunk_size = def_plsize;
            par["payloadsize"] = Sprint(def_plsize);
            Verb() << "Specified payload size=" << def_plsize << " as required by the wrapper";
        }
    }

    // Assign the others here.
    m_options = par;
    m_options["mode"] = m_mode;
}

void SrtCommon::PrepareListener(string host, int port, int backlog)
{
    m_listener = srt_socket(AF_INET, SOCK_DGRAM, 0);
    if ( m_listener == SRT_ERROR )
        Error(UDT::getlasterror(), "srt_socket");

    int stat = ConfigurePre(m_listener);
    if ( stat == SRT_ERROR )
        Error(UDT::getlasterror(), "ConfigurePre");

    if ( !m_blocking_mode || m_listener_group)
    {
        AddPoller(m_listener, SRT_EPOLL_IN);
    }

    sockaddr_in sa = CreateAddrInet(host, port);
    sockaddr* psa = (sockaddr*)&sa;
    Verb() << "Binding a server on " << host << ":" << port << " ...";
    stat = srt_bind(m_listener, psa, sizeof sa);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_listener);
        Error(UDT::getlasterror(), "srt_bind");
    }

    Verb() << " listen... " << VerbNoEOL;
    stat = srt_listen(m_listener, backlog);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_listener);
        Error(UDT::getlasterror(), "srt_listen");
    }

    Verb() << " accept... " << VerbNoEOL;
    ::transmit_throw_on_interrupt = true;

    if ( !m_blocking_mode )
    {
        Verb() << "[ASYNC] (conn=" << srt_epoll << ")";

        int len = 2;
        SRTSOCKET ready[2];
        if ( srt_epoll_wait(srt_epoll, 0, 0, ready, &len, -1, 0, 0, 0, 0) == -1 )
            Error(UDT::getlasterror(), "srt_epoll_wait(srt_conn_epoll)");

        Verb() << "[EPOLL: " << len << " sockets] " << VerbNoEOL;
    }
}

void SrtCommon::StealFrom(SrtCommon& src)
{
    // This is used when SrtCommon class designates a listener
    // object that is doing Accept in appropriate direction class.
    // The new object should get the accepted socket.
    m_direction = src.m_direction;
    m_blocking_mode = src.m_blocking_mode;
    m_timeout = src.m_timeout;
    m_tsbpdmode = src.m_tsbpdmode;
    m_options = src.m_options;
    m_listener = SRT_INVALID_SOCK; // no listener
    m_links = src.m_links;
    src.m_links.clear();
}

SrtCommon::Connection& SrtCommon::AcceptNewClient()
{
    sockaddr_in scl;
    int sclen = sizeof scl;

    Verb() << " accept..." << VerbNoEOL;

    int sock = srt_accept(m_listener, (sockaddr*)&scl, &sclen);
    if ( sock == SRT_INVALID_SOCK )
    {
        srt_close(m_listener);
        m_listener = SRT_INVALID_SOCK;
        Error(UDT::getlasterror(), "srt_accept");
    }

    ::transmit_throw_on_interrupt = false;

    // ConfigurePre is done on bindsock, so any possible Pre flags
    // are DERIVED by sock. ConfigurePost is done exclusively on sock.
    int stat = ConfigurePost(sock);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_listener);
        m_listener = SRT_INVALID_SOCK;
        Error(UDT::getlasterror(), "ConfigurePost");
    }

    m_links.push_back(Connection(sock));
    Connection& c = m_links.back();
    memcpy(&c.peeraddr, &scl, sclen);
    Verb() << " connected: " << SockaddrToString((sockaddr*)&c.peeraddr);

    return c;
}

void SrtCommon::Init(string host, int port, string path, map<string,string> par, SRT_EPOLL_OPT dir)
{
    m_direction = dir;
    InitParameters(host, path, par);

    Verb() << "Opening SRT " << DirectionName(dir) << " " << m_mode
        << "(" << (m_blocking_mode ? "" : "non-") << "blocking)"
        << " on " << host << ":" << port;

    try
    {
        if ( m_mode == "caller" )
        {
            if (m_links.empty())
            {
                OpenClient(host, port);
            }
            else
            {
                OpenGroupClient(); // Source data are in the fields already.
            }
        }
        else if ( m_mode == "listener" )
            OpenServer(m_adapter, port);
        else if ( m_mode == "rendezvous" )
            OpenRendezvous(m_adapter, host, port);
        else
        {
            throw std::invalid_argument("Invalid 'mode'. Use 'client' or 'server'");
        }
    }
    catch (...)
    {
        // This is an in-constructor-called function, so
        // when the exception is thrown, the destructor won't
        // close the sockets. This intercepts the exception
        // to close them.
        Verb() << "Open FAILED - closing SRT sockets";
        if (m_listener != SRT_INVALID_SOCK)
            srt_close(m_listener);
        m_listener = SRT_INVALID_SOCK;

        for (auto s: m_links)
            srt_close(s.socket);
        m_links.clear();
        throw;
    }

    int pbkeylen = 0;
    SRT_KM_STATE kmstate, snd_kmstate, rcv_kmstate;
    int len = sizeof (int);
    vector<Connection>::iterator l = find_if(m_links.begin(), m_links.end(),
             [] (Connection& c) { return c.socket != SRT_INVALID_SOCK; });

    if (l == m_links.end())
    {
        Error("No active links");
    }

    // Display some selected options on the socket.
    if (Verbose::on)
    {
        srt_getsockflag(l->socket, SRTO_PBKEYLEN, &pbkeylen, &len);
        srt_getsockflag(l->socket, SRTO_KMSTATE, &kmstate, &len);
        srt_getsockflag(l->socket, SRTO_SNDKMSTATE, &snd_kmstate, &len);
        srt_getsockflag(l->socket, SRTO_RCVKMSTATE, &rcv_kmstate, &len);

        // Bring this declaration temporarily, this is only for testing
        std::string KmStateStr(SRT_KM_STATE state);

        Verb() << "ENCRYPTION status: " << KmStateStr(kmstate)
            << " (SND:" << KmStateStr(snd_kmstate) << " RCV:" << KmStateStr(rcv_kmstate)
            << ") PBKEYLEN=" << pbkeylen;

        int64_t bandwidth = 0;
        int latency = 0;
        bool blocking_snd = false, blocking_rcv = false;
        int dropdelay = 0;
        int size_int = sizeof (int), size_int64 = sizeof (int64_t), size_bool = sizeof (bool);

        srt_getsockflag(l->socket, SRTO_MAXBW, &bandwidth, &size_int64);
        srt_getsockflag(l->socket, SRTO_RCVLATENCY, &latency, &size_int);
        srt_getsockflag(l->socket, SRTO_RCVSYN, &blocking_rcv, &size_bool);
        srt_getsockflag(l->socket, SRTO_SNDSYN, &blocking_snd, &size_bool);
        srt_getsockflag(l->socket, SRTO_SNDDROPDELAY, &dropdelay, &size_int);

        Verb() << "OPTIONS: maxbw=" << bandwidth << " rcvlatency=" << latency << boolalpha
            << " blocking{rcv=" << blocking_rcv << " snd=" << blocking_snd
            << "} snddropdelay=" << dropdelay;
    }

    if ( !m_blocking_mode )
    {
        // Don't add new epoll if already created as a part
        // of group management: if (srt_epoll == -1)...
        for (auto s: m_links)
            AddPoller(s.socket, dir);
    }
    else if (m_group_wrapper && (m_listener_group || dir != SRT_EPOLL_OUT))
    {
        // Change connected sockets' epoll subscription
        // to the one of direction. This happens in the
        // following situations:
        // - caller & reader, so the connecting socket was subscribed to OUT, change to IN
        // - listener, so the first accepted socket was not yet added to epoll, add it now.
        // Note that the status of a fresh accepted socket is always CONNECTED.
        for (auto& c: m_links)
        {
            c.status = srt_getsockstate(c.socket);
            if (c.status == SRTS_CONNECTED)
                AddPoller(c.socket, dir);
        }
    }
}

void SrtCommon::AddPoller(SRTSOCKET socket, int modes)
{
    if (srt_epoll == -1)
    {
        srt_epoll = srt_epoll_create();
        if ( srt_epoll == -1 )
            throw std::runtime_error("Can't create epoll in nonblocking mode");
    }
    Verb() << "EPOLL: creating eid=" << srt_epoll << " and adding @" << socket
        << " in " << DirectionName(SRT_EPOLL_OPT(modes)) << " mode";
    srt_epoll_add_usock(srt_epoll, socket, &modes);
}

int SrtCommon::ConfigurePost(SRTSOCKET sock)
{
    bool yes = m_blocking_mode;
    int result = 0;
    if ( m_direction & SRT_EPOLL_OUT )
    {
        Verb() << "Setting SND blocking mode: " << boolalpha << yes << " timeout=" << m_timeout;
        result = srt_setsockopt(sock, 0, SRTO_SNDSYN, &yes, sizeof yes);
        if ( result == -1 )
            return result;

        if ( m_timeout )
            result = srt_setsockopt(sock, 0, SRTO_SNDTIMEO, &m_timeout, sizeof m_timeout);
        if ( result == -1 )
            return result;
    }

    if ( m_direction & SRT_EPOLL_IN )
    {
        Verb() << "Setting RCV blocking mode: " << boolalpha << yes << " timeout=" << m_timeout;
        result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &yes, sizeof yes);
        if ( result == -1 )
            return result;

        if ( m_timeout )
            result = srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &m_timeout, sizeof m_timeout);
        if ( result == -1 )
            return result;
    }

    SrtConfigurePost(sock, m_options);

    for (auto o: srt_options)
    {
        if ( o.binding == SocketOption::POST && m_options.count(o.name) )
        {
            string value = m_options.at(o.name);
            bool ok = o.apply<SocketOption::SRT>(sock, value);
            if (Verbose::on)
            {
                string dir_name = DirectionName(m_direction);

                if ( !ok )
                    Verb() << "WARNING: failed to set '" << o.name << "' (post, " << dir_name << ") to " << value;
                else
                    Verb() << "NOTE: SRT/post::" << o.name << "=" << value;
            }
        }
    }

    return 0;
}

int SrtCommon::ConfigurePre(SRTSOCKET sock)
{
    return ConfigurePre(sock, m_blocking_mode);
}

int SrtCommon::ConfigurePre(SRTSOCKET sock, bool blocking)
{
    int result = 0;

    int no = 0;
    if ( !m_tsbpdmode )
    {
        result = srt_setsockopt(sock, 0, SRTO_TSBPDMODE, &no, sizeof no);
        if ( result == -1 )
            return result;
    }

    // Let's pretend async mode is set this way.
    // This is for asynchronous connect.
    // Note: Beside the reading operation (srt_recv*), the
    // SRTO_RCVSYN defines also whether srt_connect and srt_accept
    // operations are blocking.
    //
    // Note a slight discrepancy: Even though nonblocking srt_connect
    // is setup by SRTO_RCVSYN, the epoll event reporting that srt_connect
    // operation has succeeded in background is SRT_EPOLL_OUT.
    int maybe = blocking;
    result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &maybe, sizeof maybe);
    if ( result == -1 )
        return result;

    // host is only checked for emptiness and depending on that the connection mode is selected.
    // Here we are not exactly interested with that information.
    vector<string> failures;

    // NOTE: here host = "", so the 'connmode' will be returned as LISTENER always,
    // but it doesn't matter here. We don't use 'connmode' for anything else than
    // checking for failures.
    SocketOption::Mode conmode = SrtConfigurePre(sock, "",  m_options, &failures);

    if ( conmode == SocketOption::FAILURE )
    {
        if (Verbose::on )
        {
            Verb() << "WARNING: failed to set options: ";
            copy(failures.begin(), failures.end(), ostream_iterator<string>(cout, ", "));
            Verb();
        }

        return SRT_ERROR;
    }

    return 0;
}

void SrtCommon::SetupAdapter(SRTSOCKET sock, const string& host, int port)
{
    sockaddr_in localsa = CreateAddrInet(host, port);
    sockaddr* psa = (sockaddr*)&localsa;
    int stat = srt_bind(sock, psa, sizeof localsa);
    if ( stat == SRT_ERROR )
        Error(UDT::getlasterror(), "srt_bind");
}

void SrtCommon::OpenClient(string host, int port)
{
    SRTSOCKET sock = PrepareClient(m_blocking_mode);

    if ( m_outgoing_port )
    {
        SetupAdapter(sock, "", m_outgoing_port);
    }

    ConnectClient(sock, host, port);

    // Set it as the only socket
    m_links.push_back(sock);
}

SRTSOCKET SrtCommon::PrepareClient(bool blocking)
{
    struct CloseIfFailed
    {
        int s = -1;
        ~CloseIfFailed() { srt_close(s); }
    } cif;

    cif.s = srt_socket(AF_INET, SOCK_DGRAM, 0);
    if ( cif.s == SRT_ERROR )
        Error(UDT::getlasterror(), "srt_socket");

    int stat = ConfigurePre(cif.s, blocking);
    if ( stat == SRT_ERROR )
        Error(UDT::getlasterror(), "ConfigurePre");

    if ( !blocking)
    {
        AddPoller(cif.s, SRT_EPOLL_OUT);
    }

    int sock = cif.s;
    cif.s = -1;
    return sock;
}

void SrtCommon::OpenGroupClient()
{
    // Resolve group type.
    if (m_group_type == "redundancy" || m_group_type == "bonding")
    {
    }
    else
    {
        Error("With //group, type='" + m_group_type + "' undefined");
    }

    // Don't check this. Should this fail, the above would already.

    // ConnectClient can't be used here, the code must
    // be more-less repeated. In this case the situation
    // that not all connections can be established is tolerated,
    // the only case of error is when none of the connections
    // can be established.

    bool any_node = false;

    Verb() << "REDUNDANT connections with " << m_links.size() << " nodes:";

    int namelen = sizeof (sockaddr_in);

    Verb() << "Connecting to nodes:";
    int i = 1;
    for (Connection& c: m_links)
    {
        sockaddr_in sa = CreateAddrInet(c.host, c.port);
        sockaddr* psa = (sockaddr*)&sa;
        Verb() << "\t[" << i << "] " << c.host << ":" << c.port << " ... " << VerbNoEOL;
        ++i;

        // Set nonblocking mode for connection always. This is because
        // the connection might be made to potentially multiple endpoints.
        c.socket = PrepareClient(false);

        // Add this socket to poller as well, as we use non-blocking mode here.
        AddPoller(c.socket, SRT_EPOLL_OUT);

        c.result = srt_connect(c.socket, psa, namelen);
        c.errorcode = srt_getlasterror(0);
    }

    // Get current connection timeout from the first socket (all should have same).
    int conntimeo = 0;
    int conntimeo_size = sizeof conntimeo;
    srt_getsockflag(m_links[0].socket, SRTO_CONNTIMEO, &conntimeo, &conntimeo_size);

    // Ok, so all connections have started in background.
    // Wait for at least one connection to be established.

    SrtPollState sready;
    int nready = UDT::epoll_swait(srt_epoll, sready, conntimeo);

    if (nready <= 0)
    {
        Error(m_links[0].errorcode, "srt_connect(group, none connected)");
    }

    Verb() << "READY " << nready << " connections: "
        << DisplayEpollResults(sready.wr(), "[CONN]")
        << DisplayEpollResults(sready.ex(), "[ERROR]");

    // Configure only those that have connected. Others will have to wait
    // for the opportunity.

    using namespace set_op;

    for (Connection& c: m_links)
    {
        if (c.socket % sready.ex())
        {
            Verb() << "TARGET '" << c.host << ":" << c.port <<  "' connection failed.";
            // Mark error on the link
            c.result = -1;
            c.errorcode = SRT_ECONNFAIL;
            c.status = SRTS_BROKEN;
            continue;
        }

        if (c.socket / sready.wr())
            continue; // Socket not ready

        // Configuration change applied on a group should
        // spread the setting on all sockets.
        int stat = ConfigurePost(c.socket);
        if (stat == -1)
        {
            // This kind of error must reject the whole operation.
            // Usually you'll get this error on the first socket,
            // and doing this on the others would result in the same.
            Error(UDT::getlasterror(), "ConfigurePost");
        }
        Verb() << "TARGET '" << c.host << ":" << c.port <<  "' connection successful.";
        any_node = true;
    }

    if (!any_node)
    {
        Error("GROUP: all redundant connections failed");
    }
}

/*
   This may be used sometimes for testing, but it's nonportable.
   void SrtCommon::SpinWaitAsync()
   {
   static string udt_status_names [] = {
   "INIT" , "OPENED", "LISTENING", "CONNECTING", "CONNECTED", "BROKEN", "CLOSING", "CLOSED", "NONEXIST"
   };

   for (;;)
   {
   SRT_SOCKSTATUS state = srt_getsockstate(m_links);
   if ( int(state) < SRTS_CONNECTED )
   {
   if ( Verbose::on )
   Verb() << state;
   usleep(250000);
   continue;
   }
   else if ( int(state) > SRTS_CONNECTED )
   {
   Error(UDT::getlasterror(), "UDT::connect status=" + udt_status_names[state]);
   }

   return;
   }
   }
 */

void SrtCommon::ConnectClient(SRTSOCKET sock, string host, int port)
{
    sockaddr_in sa = CreateAddrInet(host, port);
    sockaddr* psa = (sockaddr*)&sa;
    Verb() << "Connecting to " << host << ":" << port << " ... " << VerbNoEOL;
    int stat = srt_connect(sock, psa, sizeof sa);
    if ( stat == SRT_ERROR )
    {
        srt_close(sock);
        Error(UDT::getlasterror(), "UDT::connect");
    }

    // Wait for REAL connected state if nonblocking mode
    if ( !m_blocking_mode )
    {
        AddPoller(sock, SRT_EPOLL_OUT);
        Verb() << "[ASYNC] " << VerbNoEOL;

        // SPIN-WAITING version. Don't use it unless you know what you're doing.
        // SpinWaitAsync();

        // Socket readiness for connection is checked by polling on WRITE allowed sockets.
        int len = 2;
        SRTSOCKET ready[2];
        if ( srt_epoll_wait(srt_epoll, 0, 0, ready, &len, -1, 0, 0, 0, 0) != -1 )
        {
            Verb() << "[EPOLL: " << len << " sockets] " << VerbNoEOL;
        }
        else
        {
            Error(UDT::getlasterror(), "srt_epoll_wait(srt_conn_epoll)");
        }

        if (m_direction != SRT_EPOLL_OUT)
            AddPoller(sock, m_direction);
    }

    Verb() << " connected.";
    stat = ConfigurePost(sock);
    if ( stat == SRT_ERROR )
        Error(UDT::getlasterror(), "ConfigurePost");
}

void SrtCommon::Error(UDT::ERRORINFO& udtError, string src)
{
    int udtResult = udtError.getErrorCode();
    string message = udtError.getErrorMessage();
    if ( Verbose::on )
        Verb() << "FAILURE\n" << src << ": [" << udtResult << "] " << message;
    else
        cerr << "\nERROR #" << udtResult << ": " << message << endl;

    udtError.clear();
    throw TransmissionError("error: " + src + ": " + message);
}

void SrtCommon::Error(string msg)
{
    cerr << "\nERROR (app): " << msg << endl;
    throw std::runtime_error(msg);
}


void SrtCommon::SetupRendezvous(SRTSOCKET sock, string adapter, int port)
{
    bool yes = true;
    srt_setsockopt(sock, 0, SRTO_RENDEZVOUS, &yes, sizeof yes);

    sockaddr_in localsa = CreateAddrInet(adapter, port);
    sockaddr* plsa = (sockaddr*)&localsa;
    Verb() << "Binding a server on " << adapter << ":" << port << " ...";
    int stat = srt_bind(sock, plsa, sizeof localsa);
    if ( stat == SRT_ERROR )
    {
        srt_close(sock);
        Error(UDT::getlasterror(), "srt_bind");
    }
}

void SrtCommon::Close()
{
    bool any = false;
    bool yes = true;
    for (auto& l: m_links)
    {
        Verb() << "SrtCommon: DESTROYING CONNECTION, closing socket (rt%" << l.socket << ")...";
        srt_setsockflag(l.socket, SRTO_SNDSYN, &yes, sizeof yes);
        srt_close(l.socket);
        any = true;
    }

    if ( m_listener != SRT_INVALID_SOCK )
    {
        Verb() << "SrtCommon: DESTROYING SERVER, closing socket (ls%" << m_listener << ")...";
        // Set sndsynchro to the socket to synch-close it.
        srt_setsockflag(m_listener, SRTO_SNDSYN, &yes, sizeof yes);
        srt_close(m_listener);
        any = true;
    }

    if (any)
        Verb() << "SrtCommon: ... done.";
}

SrtCommon::~SrtCommon()
{
    Close();
}

void SrtCommon::UpdateGroupConnections()
{
    if (m_listener_group)
    {
        // Check if a new conenction is pending for accept.
        // If so, accept a new connection.
        // Just check once, without waiting.

        SrtPollState sready;
        UDT::epoll_swait(srt_epoll, sready, 0);
        if (sready.rd().count(m_listener))
        {
            Verb() << "NEW CONNECTION COMING IN";
            AcceptNewClient();
        }

        return;
    }

    int i = 1; // Verb only
    for (auto& n: m_links)
    {
        // Check which nodes are no longer active and activate them.
        if (n.socket != SRT_INVALID_SOCK)
            continue;

        sockaddr_in sa = CreateAddrInet(n.host, n.port);
        sockaddr* psa = (sockaddr*)&sa;
        Verb() << "[" << i << "] RECONNECTING to node " << n.host << ":" << n.port << " ... " << VerbNoEOL;
        ++i;
        int insock = srt_socket(AF_INET, SOCK_DGRAM, 0);
        if (insock == SRT_INVALID_SOCK || srt_connect(insock, psa, sizeof sa) == SRT_INVALID_SOCK)
        {
            srt_close(insock);
            Verb() << "FAILED: ";
            continue;
        }

        // Have socket, store it into the group socket array.
        n.socket = insock;
    }
}

SrtSource::SrtSource(string host, int port, std::string path, const map<string,string>& par)
{
    Init(host, port, path, par, SRT_EPOLL_IN);
    ostringstream os;
    os << host << ":" << port;
    hostport_copy = os.str();
}

bytevector SrtSource::Read(size_t chunk)
{
    static size_t counter = 1;

    bool have_group = m_group_type != "";

    bytevector data(chunk);
    // EXPERIMENTAL

    if (have_group || m_listener_group)
    {
        data = GroupRead(chunk);

        UpdateGroupConnections();

        return data;
    }

    bool ready = true;
    int stat;
    do
    {
        ::transmit_throw_on_interrupt = true;
        stat = srt_recvmsg(m_links[0].socket, data.data(), chunk);
        ::transmit_throw_on_interrupt = false;
        if ( stat == SRT_ERROR )
        {
            if ( !m_blocking_mode )
            {
                // EAGAIN for SRT READING
                if ( srt_getlasterror(NULL) == SRT_EASYNCRCV )
                {
                    Verb() << "AGAIN: - waiting for data by epoll...";
                    // Poll on this descriptor until reading is available, indefinitely.
                    int len = 2;
                    SRTSOCKET sready[2];
                    if ( srt_epoll_wait(srt_epoll, sready, &len, 0, 0, -1, 0, 0, 0, 0) != -1 )
                    {
                        if ( Verbose::on )
                        {
                            Verb() << "... epoll reported ready " << len << " sockets";
                        }
                        continue;
                    }
                    // If was -1, then passthru.
                }
            }
            Error(UDT::getlasterror(), "recvmsg");
        }

        if ( stat == 0 )
        {
            throw ReadEOF(hostport_copy);
        }
    }
    while (!ready);

    chunk = size_t(stat);
    if ( chunk < data.size() )
        data.resize(chunk);

    CBytePerfMon perf;
    srt_bstats(m_links[0].socket, &perf, true);
    if ( transmit_bw_report && int(counter % transmit_bw_report) == transmit_bw_report - 1 )
    {
        Verb() << "+++/+++SRT BANDWIDTH: " << perf.mbpsBandwidth;
    }

    if ( transmit_stats_report && counter % transmit_stats_report == transmit_stats_report - 1)
    {
        PrintSrtStats(m_links[0].socket, perf);
    }

    ++counter;

    return data;
}

// NOTE: 'output' is expected to be EMPTY here.
bool SrtSource::GroupCheckPacketAhead(bytevector& output)
{
    bool status = false;
    vector<SRTSOCKET> past_ahead;

    // This map no longer maps only ahead links.
    // Here are all links, and whether ahead, it's defined by the sequence.
    for (auto i = m_group_positions.begin(); i != m_group_positions.end(); ++i)
    {
        // i->first: socket ID
        // i->second: ReadPos { sequence, packet }
        // We are not interested with the socket ID because we
        // aren't going to read from it - we have the packet already.
        ReadPos& a = i->second;

        int seqdiff = CSeqNo::seqcmp(a.sequence, m_group_wrapper->seqno());
        if ( seqdiff == 1)
        {
            // The very next packet. Return it.
            m_group_wrapper->seqno() = a.sequence;
            Verb() << " (SRT group: ahead delivery %" << a.sequence << " from @" << i->first << ")";
            swap(output, a.packet);
            status = true;
        }
        else if (seqdiff < 1 && !a.packet.empty())
        {
            Verb() << " (@" << i->first << " dropping collected ahead %" << a.sequence << ")";
            a.packet.clear();
        }
        // In case when it's >1, keep it in ahead
    }

    return status;
}

static inline int SeqDiff(uint16_t left, uint16_t right)
{
    uint32_t extl = left, extr = right;

    if ( left < right )
    {
        int32_t diff = right - left;
        if ( diff >= 0x8000 )
        {
            // It means that left is less than right because it was overflown
            // For example: left = 0x0005, right = 0xFFF0; diff = 0xFFEB > 0x8000
            extl |= 0x00010000;  // left was really 0x00010005, just narrowed.
            // Now the difference is 0x0015, not 0xFFFF0015
        }
    }
    else
    {
        int32_t diff = left - right;
        if ( diff >= 0x8000 )
        {
            extr |= 0x00010000;
        }
    }

    return extl - extr;
}


bytevector SrtSource::GroupRead(size_t chunk)
{
    // For group reading you need a wrapper.
    if (!m_group_wrapper)
        Error("GroupRead: No wrapper, for groups a wrapper protocol must be defined");

    Verb() << "GroupRead: chunk=" << chunk;

    // Read the current group status. m_links is here the group id.
    bytevector output;

    // Later iteration over it might be less efficient than
    // by vector, but we'll also often try to check a single id
    // if it was ever seen broken, so that it's skipped.
    set<SRTSOCKET> broken; // Same as sets in epoll

RETRY_READING:

    // Update status on all sockets in the group
    bool connected = false;
    for (auto& d: m_links)
    {
        d.status = srt_getsockstate(d.socket);
        if (d.status == SRTS_CONNECTED)
        {
            connected = true;
            break;
        }
    }
    if (!connected)
    {
        Error("All sockets in the group disconnected");
    }

    if (Verbose::on)
    {
        for (auto& d: m_links)
        {
            if (d.status != SRTS_CONNECTED)
                // id, status, result, peeraddr
                Verb() << "@" << d.socket << " <" << SockStatusStr(d.status) << "> (=" << d.result << ") PEER:"
                    << SockaddrToString((sockaddr*)&d.peeraddr);
        }
    }

    // Check first the ahead packets if you have any to deliver.
    if (m_group_wrapper->seqno() != -1 && !m_group_positions.empty())
    {
        bytevector ahead_packet;

        // This function also updates the group sequence pointer.
        if (GroupCheckPacketAhead(ahead_packet))
            return move(ahead_packet);
    }

    // LINK QUALIFICATION NAMES:
    //
    // HORSE: Correct link, which delivers the very next sequence.
    // Not necessarily this link is currently active.
    //
    // KANGAROO: Got some packets dropped and the sequence number
    // of the packet jumps over the very next sequence and delivers
    // an ahead packet.
    //
    // ELEPHANT: Is not ready to read, while others are, or reading
    // up to the current latest delivery sequence number does not
    // reach this sequence and the link becomes non-readable earlier.

    // The above condition has ruled out one kangaroo and turned it
    // into a horse.

    // Below there's a loop that will try to extract packets. Kangaroos
    // will be among the polled ones because skipping them risks that
    // the elephants will take over the reading. Links already known as
    // elephants will be also polled in an attempt to revitalize the
    // connection that experienced just a short living choking.
    //
    // After polling we attempt to read from every link that reported
    // read-readiness and read at most up to the sequence equal to the
    // current delivery sequence.

    // Links that deliver a packet below that sequence will be retried
    // until they deliver no more packets or deliver the packet of
    // expected sequence. Links that don't have a record in m_group_positions
    // and report readiness will be always read, at least to know what
    // sequence they currently stand on.
    //
    // Links that are already known as kangaroos will be polled, but
    // no reading attempt will be done. If after the reading series
    // it will turn out that we have no more horses, the slowest kangaroo
    // will be "advanced to a horse" (the ahead link with a sequence
    // closest to the current delivery sequence will get its sequence
    // set as current delivered and its recorded ahead packet returned
    // as the read packet).

    // If we find at least one horse, the packet read from that link
    // will be delivered. All other link will be just ensured update
    // up to this sequence number, or at worst all available packets
    // will be read. In this case all kangaroos remain kangaroos,
    // until the current delivery sequence m_group_wrapper->seqno() will be lifted
    // to the sequence recorded for these links in m_group_positions,
    // during the next time ahead check, after which they will become
    // horses.

    Verb() << "E(" << srt_epoll << ") " << VerbNoEOL;

    for (size_t i = 0; i < m_links.size(); ++i)
    {
        Connection& d = m_links[i];
        if (d.status == SRTS_CONNECTING)
        {
            Verb() << "@" << d.socket << " [" << i << "] <pending> " << VerbNoEOL;
            continue; // don't read over a failed or pending socket
        }

        if (d.status >= SRTS_BROKEN)
        {
            broken.insert(d.socket);
        }

        if (broken.count(d.socket))
        {
            Verb() << "@" << d.socket << " [" << i << "] <broken> " << VerbNoEOL;
            continue;
        }

        if (d.status != SRTS_CONNECTED)
        {
            Verb() << "@" << d.socket  << " [" << i << "] <idle:" << SockStatusStr(d.status) << "> " << VerbNoEOL;
            // Sockets in this state are ignored. We are waiting until it
            // achieves CONNECTING state, then it's added to write.
            continue;
        }

        // Don't skip packets that are ahead because if we have a situation
        // that all links are either "elephants" (do not report read readiness)
        // and "kangaroos" (have already delivered an ahead packet) then
        // omiting kangaroos will result in only elephants to be polled for
        // reading. Elephants, due to the strict timing requirements and
        // ensurance that TSBPD on every link will result in exactly the same
        // delivery time for a packet of given sequence, having an elephant
        // and kangaroo in one cage means that the elephant is simply a broken
        // or half-broken link (the data are not delivered, but it will get
        // repaired soon, enough for SRT to maintain the connection, but it
        // will still drop packets that didn't arrive in time), in both cases
        // it may potentially block the reading for an indefinite time, while
        // simultaneously a kangaroo might be a link that got some packets
        // dropped, but then it's still capable to deliver packets on time.

        // Note also that about the fact that some links turn out to be
        // elephants we'll learn only after we try to poll and read them.

        // Note that d.socket might be a socket that was previously being polled
        // on write, when it's attempting to connect, but now it's connected.
        // This will update the socket with the new event set.

        int modes = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        srt_epoll_add_usock(srt_epoll, d.socket, &modes);
        Verb() << "@" << d.socket << "[READ] " << VerbNoEOL;
    }

    Verb() << "";

    // Here we need to make an additional check.
    // There might be a possibility that all sockets that
    // were added to the reader group, are ahead. At least
    // surely we don't have a situation that any link contains
    // an ahead-read subsequent packet, because GroupCheckPacketAhead
    // already handled that case.
    //
    // What we can have is that every link has:
    // - no known seq position yet (is not registered in the position map yet)
    // - the position equal to the latest delivered sequence
    // - the ahead position

    // Now the situation is that we don't have any packets
    // waiting for delivery so we need to wait for any to report one.
    // XXX We support blocking mode only at the moment.
    // The non-blocking mode would need to simply check the readiness
    // with only immediate report, and read-readiness would have to
    // be done in background.

    SrtPollState sready;

    // Poll on this descriptor until reading is available, indefinitely.
    if (UDT::epoll_swait(srt_epoll, sready, -1) == SRT_ERROR)
    {
        Error(UDT::getlasterror(), "UDT::epoll_swait(srt_epoll, group)");
    }
    if (Verbose::on)
    {
        Verb() << "RDY: {"
            << DisplayEpollResults(sready.rd(), "[R]")
            << DisplayEpollResults(sready.wr(), "[W]")
            << DisplayEpollResults(sready.ex(), "[E]")
            << "}";

    }

    LOGC(applog.Debug, log << "epoll_swait: "
            << DisplayEpollResults(sready.rd(), "[R]")
            << DisplayEpollResults(sready.wr(), "[W]")
            << DisplayEpollResults(sready.ex(), "[E]"));

    typedef set<SRTSOCKET> fset_t;

    // Handle sockets of pending connection and with errors.
    broken = sready.ex();

    // We don't do anything about sockets that have been configured to
    // poll on writing (that is, pending for connection). What we need
    // is that the epoll_swait call exit on that fact. Probably if this
    // was the only socket reported, no broken and no read-ready, this
    // will later check on output if still empty, if so, repeat the whole
    // function. This write-ready socket will be there already in the
    // connected state and will be added to read-polling.

    // Ok, now we need to have some extra qualifications:
    // 1. If a socket has no registry yet, we read anyway, just
    // to notify the current position. We read ONLY ONE PACKET this time,
    // we'll worry later about adjusting it to the current group sequence
    // position.
    // 2. If a socket is already position ahead, DO NOT read from it, even
    // if it is ready.

    // The state of things whether we were able to extract the very next
    // sequence will be simply defined by the fact that `output` is nonempty.

    // Here it's using int32_t because we need a trap representation -1.
    int32_t next_seq = m_group_wrapper->seqno();

    // If this set is empty, it won't roll even once, therefore output
    // will be surely empty. This will be checked then same way as when
    // reading from every socket resulted in error.
    for (fset_t::const_iterator i = sready.rd().begin(); i != sready.rd().end(); ++i)
    {
        // Listener socket is among those subscribed for READ-readiness.
        // Simply ignore it; this will be checked again without waiting
        // in UpdateGroupConnections().
        if (*i == m_listener)
            continue;

        // Check if this socket is in aheads
        // If so, don't read from it, wait until the ahead is flushed.

        SRTSOCKET id = *i;
        ReadPos* p = nullptr;
        auto pe = m_group_positions.find(id);
        if (pe != m_group_positions.end())
        {
            p = &pe->second;
            // Possible results of comparison:
            // x < 0: the sequence is in the past, the socket should be adjusted FIRST
            // x = 0: the socket should be ready to get the exactly next packet
            // x = 1: the case is already handled by GroupCheckPacketAhead.
            // x > 1: AHEAD. DO NOT READ.
            int seqdiff = SeqDiff(p->sequence, m_group_wrapper->seqno());
            if (seqdiff > 1)
            {
                Verb() << "EPOLL: @" << id << " %" << p->sequence << " AHEAD, not reading.";
                continue;
            }
        }


        // Read from this socket stubbornly, until:
        // - reading is no longer possible (AGAIN)
        // - the sequence difference is >= 1

        int fi = 1; // marker for Verb to display flushing
        for (;;)
        {
            bytevector data(chunk);
            int stat = srt_recvmsg(id, data.data(), chunk);
            if (stat == SRT_ERROR)
            {
                if (fi == 0)
                {
                    if (Verbose::on)
                    {
                        if (p)
                        {
                            uint16_t pktseq = p->sequence;
                            int seqdiff = SeqDiff(p->sequence, m_group_wrapper->seqno());
                            Verb() << ". %" << pktseq << " " << seqdiff << ")";
                        }
                        else
                        {
                            Verb() << ".)";
                        }
                    }
                    fi = 1;
                }
                int err = srt_getlasterror(0);
                if (err == SRT_EASYNCRCV)
                {
                    // Do not treat this as spurious, just stop reading.
                    break;
                }
                Verb() << "Error @" << id << ": " << srt_getlasterror_str();
                broken.insert(id);
                break;
            }
            uint16_t pktseq = 0;
            if (!m_group_wrapper)
                Error("Group not configured");

            m_group_wrapper->load(data, stat);

            // If PASSTHRU flag is set for a wrapper, the
            // payload extracted and sent to the output is the
            // original payload, that is, the wrapped one.
            if (!m_wrapper_passthru)
                data = m_group_wrapper->payload;
            pktseq = m_group_wrapper->seqno();

            Verb() << "[WRP seq=" << pktseq << " src=" << m_group_wrapper->srcid() << "] " << VerbNoEOL;

            // NOTE: checks against m_group_wrapper->seqno() and decisions based on it
            // must NOT be done if m_group_wrapper->seqno() is -1, which means that we
            // are about to deliver the very first packet and we take its
            // sequence number as a good deal.

            // The order must be:
            // - check discrepancy
            // - record the sequence
            // - check ordering.
            // The second one must be done always, but failed discrepancy
            // check should exclude the socket from any further checks.
            // That's why the common check for m_group_wrapper->seqno() != -1 can't
            // embrace everything below.

            // We need to first qualify the sequence, just for a case
            if (m_group_wrapper->seqno() != -1 && abs(m_group_wrapper->seqno() - pktseq) > CSeqNo::m_iSeqNoTH)
            {
                // This error should be returned if the link turns out
                // to be the only one, or set to the group data.
                // err = SRT_ESECFAIL;
                if (fi == 0)
                {
                    Verb() << ".)";
                    fi = 1;
                }
                Verb() << "Error @" << id << ": SEQUENCE DISCREPANCY: base=%" << m_group_wrapper->seqno() << " vs pkt=%" << pktseq << ", setting ESECFAIL";
                broken.insert(id);
                break;
            }

            // Rewrite it to the state for a case when next reading
            // would not succeed. Do not insert the buffer here because
            // this is only required when the sequence is ahead; for that
            // it will be fixed later.
            if (!p)
            {
                p = &(m_group_positions[id] = ReadPos { pktseq, {} });
            }
            else
            {
                p->sequence = pktseq;
            }

            if (m_group_wrapper->seqno() != -1)
            {
                // Now we can safely check it.
                int seqdiff = SeqDiff(pktseq, m_group_wrapper->seqno());

                if (seqdiff <= 0)
                {
                    if (fi == 1)
                    {
                        Verb() << "(@" << id << " FLUSH:" << VerbNoEOL;
                        fi = 0;
                    }

                    Verb() << "." << VerbNoEOL;

                    // The sequence is recorded, the packet has to be discarded.
                    // That's all.
                    continue;
                }

                // Finish flush reporting if fallen into here
                if (fi == 0)
                {
                    Verb() << ". %" << pktseq << " " << (-seqdiff) << ")";
                    fi = 1;
                }

                // Now we have only two possibilities:
                // seqdiff == 1: The very next sequence, we want to read and return the packet.
                // seqdiff > 1: The packet is ahead - record the ahead packet, but continue with the others.

                if (seqdiff > 1)
                {
                    Verb() << "@" << id << " %" << pktseq << " AHEAD";
                    p->packet = move(data);
                    break; // Don't read from that socket anymore.
                }
            }

            // We have seqdiff = 1, or we simply have the very first packet
            // which's sequence is taken as a good deal. Update the sequence
            // and record output.

            if (!output.empty())
            {
                Verb() << "@" << id << " %" << pktseq << " REDUNDANT";
                break;
            }


            Verb() << "@" << id << " %" << pktseq << " DELIVERING";
            output = move(data);

            // Record, but do not update yet, until all sockets are handled.
            next_seq = pktseq;
            break;
        }
    }

    // ready_len is only the length of currently reported
    // ready sockets, NOT NECESSARILY containing all sockets from the group.
    if (broken.size() == m_links.size())
    {
        // All broken
        Error("All sockets broken");
    }

    if (!broken.empty())
    {
        if (Verbose::on)
        {
            Verb() << "BROKEN: " << Printable(broken) << " - removing";
        }

        using namespace set_op;

        // Now remove all broken sockets from aheads, if any.
        // Even if they have already delivered a packet.
        for (auto& c: m_links)
        {
            if (c.socket / broken)
                continue;

            // This loops rolls over only broken sockets
            m_group_positions.erase(c.socket);
            srt_close(c.socket);
            c.socket = -1;
        }

        // May be required to be re-read.
        broken.clear();
    }

    if (!output.empty())
    {
        // We have extracted something, meaning that we have the sequence shift.
        // Update it now and don't do anything else with the sockets.

        // Sanity check
        if (next_seq == -1)
        {
            Error("IPE: next_seq not set after output extracted!");
        }
        m_group_wrapper->seqno() = next_seq;
        return output;
    }

    // Check if we have any sockets left :D

    // Here we surely don't have any more HORSES,
    // only ELEPHANTS and KANGAROOS. Qualify them and
    // attempt to at least take advantage of KANGAROOS.

    // In this position all links are either:
    // - updated to the current position
    // - updated to the newest possible possition available
    // - not yet ready for extraction (not present in the group)

    // If we haven't extracted the very next sequence position,
    // it means that we might only have the ahead packets read,
    // that is, the next sequence has been dropped by all links.

    if (!m_group_positions.empty())
    {
        // This might notify both lingering links, which didn't
        // deliver the required sequence yet, and links that have
        // the sequence ahead. Review them, and if you find at
        // least one packet behind, just wait for it to be ready.
        // Use again the waiting function because we don't want
        // the general waiting procedure to skip others.
        set<SRTSOCKET> elephants;

        // const because it's `typename decltype(m_group_positions)::value_type`
        pair<const SRTSOCKET, ReadPos>* slowest_kangaroo = nullptr;

        for (auto& sock_rp: m_group_positions)
        {
            // NOTE that m_group_wrapper->seqno() in this place wasn't updated
            // because we haven't successfully extracted anything.
            int seqdiff = SeqDiff(sock_rp.second.sequence, m_group_wrapper->seqno());
            if (seqdiff < 0)
            {
                elephants.insert(sock_rp.first);
            }
            // If seqdiff == 0, we have a socket ON TRACK.
            else if (seqdiff > 0)
            {
                if (!slowest_kangaroo)
                {
                    slowest_kangaroo = &sock_rp;
                }
                else
                {
                    // Update to find the slowest kangaroo.
                    int seqdiff = SeqDiff(slowest_kangaroo->second.sequence, sock_rp.second.sequence);
                    if (seqdiff > 0)
                    {
                        slowest_kangaroo = &sock_rp;
                    }
                }
            }
        }

        // Note that if no "slowest_kangaroo" was found, it means
        // that we don't have kangaroos.
        if (slowest_kangaroo)
        {
            // We have a slowest kangaroo. Elephants must be ignored.
            // Best case, they will get revived, worst case they will be
            // soon broken.
            //
            // As we already have the packet delivered by the slowest
            // kangaroo, we can simply return it.

            m_group_wrapper->seqno() = slowest_kangaroo->second.sequence;
            Verb() << "@" << slowest_kangaroo->first << " %" << m_group_wrapper->seqno() << " KANGAROO->HORSE";
            swap(output, slowest_kangaroo->second.packet);
            return output;
        }

        // Here ALL LINKS ARE ELEPHANTS, stating that we still have any.
        if (Verbose::on)
        {
            if (!elephants.empty())
            {
                // If we don't have kangaroos, then simply reattempt to
                // poll all elephants again anyway (at worst they are all
                // broken and we'll learn about it soon).
                Verb() << "ALL LINKS ELEPHANTS. Re-polling.";
            }
            else
            {
                Verb() << "ONLY BROKEN WERE REPORTED. Re-polling.";
            }
        }
        goto RETRY_READING;
    }

    // We have checked so far only links that were ready to poll.
    // Links that are not ready should be re-checked.
    // Links that were not ready at the entrance should be checked
    // separately, and probably here is the best moment to do it.
    // After we make sure that at least one link is ready, we can
    // reattempt to read a packet from it.

    // Ok, so first collect all sockets that are in
    // connecting state, make a poll for connection.
    bool have_connectors = false, have_ready = false;
    for (auto& d: m_links)
    {
        if (d.status < SRTS_CONNECTED)
        {
            // Not sure anymore if IN or OUT signals the connect-readiness,
            // but no matter. The signal will be cleared once it is used,
            // while it will be always on when there's anything ready to read.
            int modes = SRT_EPOLL_IN | SRT_EPOLL_OUT;
            srt_epoll_add_usock(srt_epoll, d.socket, &modes);
            have_connectors = true;
        }
        else if (d.status == SRTS_CONNECTED)
        {
            have_ready = true;
        }
    }

    if (have_ready || have_connectors)
    {
        Verb() << "(still have: " << (have_ready ? "+" : "-") << "ready, "
            << (have_connectors ? "+" : "-") << "conenctors).";
        goto RETRY_READING;
    }

    if (have_ready)
    {
        Verb() << "(connected in the meantime)";
        // Some have connected in the meantime, don't
        // waste time on the pending ones.
        goto RETRY_READING;
    }

    if (have_connectors)
    {
        Verb() << "(waiting for pending connectors to connect)";
        // Wait here for them to be connected.
        vector<SRTSOCKET> sready;
        sready.resize(m_links.size());
        int ready_len = m_links.size();
        if (srt_epoll_wait(srt_epoll, sready.data(), &ready_len, 0, 0, -1, 0, 0, 0, 0) == SRT_ERROR)
        {
            Error("All sockets in the group disconnected");
        }

        goto RETRY_READING;
    }

    Error("No data extracted");
    return output; // Just a marker - this above function throws an exception
}


SrtTarget::SrtTarget(std::string host, int port, std::string path, const std::map<std::string,std::string>& par)
{
    Init(host, port, path, par, SRT_EPOLL_OUT);
}


int SrtTarget::ConfigurePre(SRTSOCKET sock)
{
    int result = SrtCommon::ConfigurePre(sock);
    if ( result == -1 )
        return result;

    int yes = 1;
    // This is for the HSv4 compatibility; if both parties are HSv5
    // (min. version 1.2.1), then this setting simply does nothing.
    // In HSv4 this setting is obligatory; otherwise the SRT handshake
    // extension will not be done at all.
    result = srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof yes);
    if ( result == -1 )
        return result;

    return 0;
}

void SrtTarget::Write(const bytevector& data)
{
    bool have_group = m_group_type != "";

    if (have_group || m_listener_group)
    {
        return GroupWrite(data);
    }

    ::transmit_throw_on_interrupt = true;

    // Check first if it's ready to write.
    // If not, wait indefinitely.
    if ( !m_blocking_mode )
    {
        int ready[2];
        int len = 2;
        if ( srt_epoll_wait(srt_epoll, 0, 0, ready, &len, -1, 0, 0, 0, 0) == SRT_ERROR )
            Error(UDT::getlasterror(), "srt_epoll_wait(srt_epoll)");
    }

    SRT_MSGCTRL mctrl = srt_msgctrl_default;

    int stat = srt_sendmsg2(m_links[0].socket, data.data(), data.size(), &mctrl);

    // For a socket group, the error is reported only
    // if ALL links from the group have failed to perform
    // the operation. If only one did, the result will be
    // visible in the status array.
    if ( stat == SRT_ERROR )
        Error(UDT::getlasterror(), "srt_sendmsg");
    ::transmit_throw_on_interrupt = false;
}

void SrtTarget::GroupWrite(const bytevector& data)
{
    if ( !m_blocking_mode )
    {
        int ready[2];
        int len = 2;
        if ( srt_epoll_wait(srt_epoll, 0, 0, ready, &len, -1, 0, 0, 0, 0) == SRT_ERROR )
            Error(UDT::getlasterror(), "srt_epoll_wait(srt_epoll)");
    }

    if (!m_group_wrapper)
        Error("Group not configured");

    bytevector packet;
    m_group_wrapper->payload = data;
    // sequence is internally managed

    // XXX Temporary; it should be set to some uniq id in the beginning.
    m_group_wrapper->srcid() = 0xCAFEB1BA;
    Verb() << "[WRP seq=" << m_group_wrapper->seqno() << " size=" << packet.size() << "] " << VerbNoEOL;
    m_group_wrapper->save(packet);

    bool ok = false;

    for (auto c: m_links)
    {
        // Send the same payload over all sockets
        SRT_MSGCTRL mctrl = srt_msgctrl_default;
        Verb() << "@" << c.socket << " " << VerbNoEOL;
        c.result = srt_sendmsg2(c.socket, packet.data(), packet.size(), &mctrl);
        c.status = srt_getsockstate(c.socket);
        if (c.result > 0 && c.status == SRTS_CONNECTED)
        {
            c.errorcode = SRT_SUCCESS;
            ok = true;
        }
        else
        {
            c.errorcode = srt_getlasterror(nullptr);
            srt_close(c.socket);
            c.socket = SRT_INVALID_SOCK;
            // The socket will be reconnected soon
        }
    }

    if (!ok)
    {
        Error("srt_sendmsg2(group)");
    }

    UpdateGroupConnections();
}

SrtRelay::SrtRelay(std::string host, int port, std::string path, const std::map<std::string,std::string>& par)
{
    Init(host, port, path, par, SRT_EPOLL_IN | SRT_EPOLL_OUT);
}

SrtModel::SrtModel(string host, int port, map<string,string> par)
{
    InitParameters(host, "", par);
    if (m_mode == "caller")
        is_caller = true;
    else if (m_mode == "rendezvous")
        is_rend = true;
    else if (m_mode != "listener")
        throw std::invalid_argument("Wrong 'mode' attribute; expected: caller, listener, rendezvous");

    m_host = host;
    m_port = port;
}

void SrtModel::Establish(ref_t<std::string> name)
{
    // This does connect or accept.
    // When this returned true, the caller should create
    // a new SrtSource or SrtTaget then call StealFrom(*this) on it.

    // If this is a connector and the peer doesn't have a corresponding
    // medium, it should send back a single byte with value 0. This means
    // that agent should stop connecting.

    int sock = SRT_INVALID_SOCK;
    if (is_rend)
    {
        sock = OpenRendezvous(m_adapter, m_host, m_port);
    }
    else if (is_caller)
    {
        // Establish a connection

        sock = PrepareClient(m_blocking_mode);

        if (name.get() != "")
        {
            Verb() << "Connect with requesting stream [" << name.get() << "]";
            UDT::setstreamid(sock, *name);
        }
        else
        {
            Verb() << "NO STREAM ID for SRT connection";
        }

        if (m_outgoing_port)
        {
            Verb() << "Setting outgoing port: " << m_outgoing_port;
            SetupAdapter(sock, "", m_outgoing_port);
        }

        ConnectClient(sock, m_host, m_port);

        m_links.push_back(sock);
        if (m_outgoing_port == 0)
        {
            // Must rely on a randomly selected one. Extract the port
            // so that it will be reused next time.
            sockaddr_any s(AF_INET);
            int namelen = s.size();
            if ( srt_getsockname(sock, &s, &namelen) == SRT_ERROR )
            {
                Error(UDT::getlasterror(), "srt_getsockname");
            }

            m_outgoing_port = s.hport();
            Verb() << "Extracted outgoing port: " << m_outgoing_port;
        }
    }
    else
    {
        // Listener - get a socket by accepting.
        // Check if the listener is already created first
        if (Listener() == SRT_INVALID_SOCK)
        {
            Verb() << "Setting up listener: port=" << m_port << " backlog=5";
            PrepareListener(m_adapter, m_port, 5);
        }

        Verb() << "Accepting a client...";
        Connection& cc = AcceptNewClient();
        // This rewrites m_links with a new SRT socket ("accepted" socket)
        *name = UDT::getstreamid(cc.socket);
        Verb() << "... GOT CLIENT for stream [" << name.get() << "]";
    }
}


template <class Iface> struct Srt;
template <> struct Srt<Source> { typedef SrtSource type; };
template <> struct Srt<Target> { typedef SrtTarget type; };
template <> struct Srt<Relay> { typedef SrtRelay type; };

template <class Iface>
Iface* CreateSrt(const string& host, int port, std::string path, const map<string,string>& par)
{ return new typename Srt<Iface>::type (host, port, path, par); }

bytevector ConsoleRead(size_t chunk)
{
    bytevector data(chunk);
    bool st = cin.read(data.data(), chunk).good();
    chunk = cin.gcount();
    if ( chunk == 0 && !st )
        return bytevector();

    if ( chunk < data.size() )
        data.resize(chunk);
    if ( data.empty() )
        throw Source::ReadEOF("CONSOLE device");

    return data;
}

class ConsoleSource: public virtual Source
{
public:

    ConsoleSource()
    {
    }

    bytevector Read(size_t chunk) override
    {
        return ConsoleRead(chunk);
    }

    bool IsOpen() override { return cin.good(); }
    bool End() override { return cin.eof(); }
};

class ConsoleTarget: public virtual Target
{
public:

    ConsoleTarget()
    {
    }

    void Write(const bytevector& data) override
    {
        cout.write(data.data(), data.size());
    }

    bool IsOpen() override { return cout.good(); }
    bool Broken() override { return cout.eof(); }
};

class ConsoleRelay: public Relay, public ConsoleSource, public ConsoleTarget
{
public:
    ConsoleRelay() = default;

    bool IsOpen() override { return cin.good() && cout.good(); }
};

template <class Iface> struct Console;
template <> struct Console<Source> { typedef ConsoleSource type; };
template <> struct Console<Target> { typedef ConsoleTarget type; };
template <> struct Console<Relay> { typedef ConsoleRelay type; };

template <class Iface>
Iface* CreateConsole() { return new typename Console<Iface>::type (); }


// More options can be added in future.
SocketOption udp_options [] {
    { "iptos", IPPROTO_IP, IP_TOS, SocketOption::PRE, SocketOption::INT, nullptr },
    // IP_TTL and IP_MULTICAST_TTL are handled separately by a common option, "ttl".
    { "mcloop", IPPROTO_IP, IP_MULTICAST_LOOP, SocketOption::PRE, SocketOption::INT, nullptr }
};

static inline bool IsMulticast(in_addr adr)
{
    unsigned char* abytes = (unsigned char*)&adr.s_addr;
    unsigned char c = abytes[0];
    return c >= 224 && c <= 239;
}


class UdpCommon
{
protected:
    int m_sock = -1;
    sockaddr_in sadr;
    string adapter;
    map<string, string> m_options;

    void Setup(string host, int port, map<string,string> attr)
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == -1)
            Error(SysError(), "UdpCommon::Setup: socket");

        int yes = 1;
        ::setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

        sadr = CreateAddrInet(host, port);

        bool is_multicast = false;

        if ( attr.count("multicast") )
        {
            if (!IsMulticast(sadr.sin_addr))
            {
                throw std::runtime_error("UdpCommon: requested multicast for a non-multicast-type IP address");
            }
            is_multicast = true;
        }
        else if (IsMulticast(sadr.sin_addr))
        {
            is_multicast = true;
        }

        if (is_multicast)
        {
            adapter = attr.count("adapter") ? attr.at("adapter") : string();
            sockaddr_in maddr;
            if ( adapter == "" )
            {
                Verb() << "Multicast: home address: INADDR_ANY:" << port;
                maddr.sin_family = AF_INET;
                maddr.sin_addr.s_addr = htonl(INADDR_ANY);
                maddr.sin_port = htons(port); // necessary for temporary use
            }
            else
            {
                Verb() << "Multicast: home address: " << adapter << ":" << port;
                maddr = CreateAddrInet(adapter, port);
            }

            ip_mreq mreq;
            mreq.imr_multiaddr.s_addr = sadr.sin_addr.s_addr;
            mreq.imr_interface.s_addr = maddr.sin_addr.s_addr;
#ifdef _WIN32
            const char* mreq_arg = (const char*)&mreq;
            const auto status_error = SOCKET_ERROR;
#else
            const void* mreq_arg = &mreq;
            const auto status_error = -1;
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
            // On Windows it somehow doesn't work when bind()
            // is called with multicast address. Write the address
            // that designates the network device here.
            // Also, sets port sharing when working with multicast
            sadr = maddr;
            int reuse = 1;
            int shareAddrRes = setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            if (shareAddrRes == status_error)
            {
                throw runtime_error("marking socket for shared use failed");
            }
            Verb() << "Multicast(Windows): will bind to home address";
#else
            Verb() << "Multicast(POSIX): will bind to IGMP address: " << host;
#endif
            int res = setsockopt(m_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq_arg, sizeof(mreq));

            if ( res == status_error )
            {
                throw runtime_error("adding to multicast membership failed");
            }
            attr.erase("multicast");
            attr.erase("adapter");
        }

        // The "ttl" options is handled separately, it maps to both IP_TTL
        // and IP_MULTICAST_TTL so that TTL setting works for both uni- and multicast.
        if (attr.count("ttl"))
        {
            int ttl = stoi(attr.at("ttl"));
            int res = setsockopt(m_sock, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof ttl);
            if (res == -1)
                Verb() << "WARNING: failed to set 'ttl' (IP_TTL) to " << ttl;
            res = setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
            if (res == -1)
                Verb() << "WARNING: failed to set 'ttl' (IP_MULTICAST_TTL) to " << ttl;

            attr.erase("ttl");
        }

        m_options = attr;

        for (auto o: udp_options)
        {
            // Ignore "binding" - for UDP there are no post options.
            if ( m_options.count(o.name) )
            {
                string value = m_options.at(o.name);
                bool ok = o.apply<SocketOption::SYSTEM>(m_sock, value);
                if ( !ok )
                    Verb() << "WARNING: failed to set '" << o.name << "' to " << value;
            }
        }
    }

    void Error(int err, string src)
    {
        char buf[512];
        string message = SysStrError(err, buf, 512u);

        if ( Verbose::on )
            Verb() << "FAILURE\n" << src << ": [" << err << "] " << message;
        else
            cerr << "\nERROR #" << err << ": " << message << endl;

        throw TransmissionError("error: " + src + ": " + message);
    }

    ~UdpCommon()
    {
#ifdef _WIN32
        if (m_sock != -1)
        {
            shutdown(m_sock, SD_BOTH);
            closesocket(m_sock);
            m_sock = -1;
        }
#else
        close(m_sock);
#endif
    }
};


class UdpSource: public virtual Source, public virtual UdpCommon
{
    bool eof = true;
public:

    UdpSource(string host, int port, const map<string,string>& attr)
    {
        Setup(host, port, attr);
        int stat = ::bind(m_sock, (sockaddr*)&sadr, sizeof sadr);
        if ( stat == -1 )
            Error(SysError(), "Binding address for UDP");
        eof = false;
    }

    bytevector Read(size_t chunk) override
    {
        bytevector data(chunk);
        sockaddr_in sa;
        socklen_t si = sizeof(sockaddr_in);
        int stat = recvfrom(m_sock, data.data(), chunk, 0, (sockaddr*)&sa, &si);
        if ( stat == -1 )
            Error(SysError(), "UDP Read/recvfrom");

        if ( stat < 1 )
        {
            eof = true;
            return bytevector();
        }

        chunk = size_t(stat);
        if ( chunk < data.size() )
            data.resize(chunk);

        return data;
    }

    bool IsOpen() override { return m_sock != -1; }
    bool End() override { return eof; }
};

class UdpTarget: public virtual Target, public virtual UdpCommon
{
public:
    UdpTarget(string host, int port, const map<string,string>& attr )
    {
        Setup(host, port, attr);
    }

    void Write(const bytevector& data) override
    {
        int stat = sendto(m_sock, data.data(), data.size(), 0, (sockaddr*)&sadr, sizeof sadr);
        if ( stat == -1 )
            Error(SysError(), "UDP Write/sendto");
    }

    bool IsOpen() override { return m_sock != -1; }
    bool Broken() override { return false; }
};

class UdpRelay: public Relay, public UdpSource, public UdpTarget
{
public:
    UdpRelay(string host, int port, const map<string,string>& attr):
        UdpSource(host, port, attr),
        UdpTarget(host, port, attr)
    {
    }

    bool IsOpen() override { return m_sock != -1; }
};

template <class Iface> struct Udp;
template <> struct Udp<Source> { typedef UdpSource type; };
template <> struct Udp<Target> { typedef UdpTarget type; };
template <> struct Udp<Relay> { typedef UdpRelay type; };

template <class Iface>
Iface* CreateUdp(const string& host, int port, const map<string,string>& par) { return new typename Udp<Iface>::type (host, port, par); }

template<class Base>
inline bool IsOutput() { return false; }

template<>
inline bool IsOutput<Target>() { return true; }

template <class Base>
extern unique_ptr<Base> CreateMedium(const string& uri)
{
    unique_ptr<Base> ptr;

    UriParser u(uri);

    int iport = 0;
    switch ( u.type() )
    {
    default:
        break; // do nothing, return nullptr
    case UriParser::FILE:
        if ( u.host() == "con" || u.host() == "console" )
        {
            if ( IsOutput<Base>() && (
                        (Verbose::on && transmit_cverb == &cout)
                        || transmit_bw_report) )
            {
                cerr << "ERROR: file://con with -v or -r would result in mixing the data and text info.\n";
                cerr << "ERROR: HINT: you can stream through a FIFO (named pipe)\n";
                throw invalid_argument("incorrect parameter combination");
            }
            ptr.reset( CreateConsole<Base>() );
        }
        else
            ptr.reset( CreateFile<Base>(u.path()));
        break;

    case UriParser::SRT:
        ptr.reset( CreateSrt<Base>(u.host(), u.portno(), u.path(), u.parameters()) );
        break;


    case UriParser::UDP:
        iport = atoi(u.port().c_str());
        if ( iport <= 1024 )
        {
            cerr << "Port value invalid: " << iport << " - must be >1024\n";
            throw invalid_argument("Invalid port number");
        }
        ptr.reset( CreateUdp<Base>(u.host(), iport, u.parameters()) );
        break;

    }

    ptr->uri = move(u);
    return ptr;
}


std::unique_ptr<Source> Source::Create(const std::string& url)
{
    return CreateMedium<Source>(url);
}

std::unique_ptr<Target> Target::Create(const std::string& url)
{
    return CreateMedium<Target>(url);
}

// Parse the RTP packet and extract the header and payload
void RTPTransportPacket::load(const bytevector& data, size_t size)
{
    static const auto nosize = ~size_t();

    if (size == nosize)
        size = data.size();

    if (size < HDR_SIZE + 1)
        throw std::invalid_argument("RTPTransportPacket: load: too small for a header");

    union Extractor
    {
        char begin;
        uint16_t field16;
        uint32_t field32;
    };

    Extractor* p = (Extractor*)data.data();

    // Indata. Ignore
    uint16_t xdata = ntohs(p->field16);

    if (xdata != indata)
        throw std::invalid_argument("RTPTransportPacket: load: incorrect header");

    p = (Extractor*)(data.data() + 2);

    seq = ntohs(p->field16);

    p = (Extractor*)(data.data() + 4);
    pkt_ts = ntohl(p->field32);

    p = (Extractor*)(data.data() + 8);
    src = ntohl(p->field32);

    // Rest of the data is the payload
    payload.clear();
    payload.reserve(size);
    payload.assign(data.begin() + 12, data.end());
}

// Write the RTP packet out of collected header and payload
void RTPTransportPacket::save(bytevector& out)
{
    out.clear();
    out.reserve(HDR_SIZE + payload.size() + 1);
    // We need the time value anyway
    timeval tv;
    gettimeofday(&tv, 0);

    uint16_t seqv;
    if (seq == -1)
    {
        // Sequence not initialized. Initialize.
        seqv = uint32_t(tv.tv_usec) & 0xFFFF; // 16-bit, even though externally it's 32-bit
    }
    else
    {
        seqv = seq;
    }

    char ii [2] = { char(uint8_t(indata >> 8)), indata & 0xFF };
    copy(ii, ii + sizeof indata, back_inserter(out));

    union
    {
        char begin;
        uint16_t field16;
        uint32_t field32;
    };

    // Sequence
    field16 = htons(seqv);
    copy(&begin, &begin + sizeof(field16), back_inserter(out));

    // Timestamp
    uint32_t timestamp;
    if (timebase == 0)
    {
        // Lazy initialization
        timestamp = 0;
        timebase = uint64_t(tv.tv_sec)*1000000 + uint64_t(tv.tv_usec);
    }
    else
    {
        int64_t now = int64_t(tv.tv_sec)*1000000 + int64_t(tv.tv_usec);
        int64_t tb = timebase;
        timestamp = (now - tb) & 0xFFFFFFFF; // ignore negatives
    }

    field32 = htonl(timestamp);
    copy(&begin, &begin + sizeof(field32), back_inserter(out));

    // Source ID. Just print whatever is set
    field32 = htonl(src);
    copy(&begin, &begin + sizeof(field32), back_inserter(out));

    // And now the payload
    out.insert(out.end(), payload.begin(), payload.end());

    // After writing the data to the output, update the sequence number

    // Note: seqv as uint16_t gets automatically overflown
    ++seqv;
    seq = seqv;
}


