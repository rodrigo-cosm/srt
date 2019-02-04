#include <thread>
#include <list>
#include <atomic>
#include "srt.h"
#include "uriparser.hpp"
#include "testmedia.hpp"
#include "utilities.h"


class SrtReceiver
    //: public SrtCommon
{

public:

    SrtReceiver(std::string host, int port, std::map<string, string> par);

    ~SrtReceiver();

    int Listen(int max_conn);

    int Receive(char *buffer, size_t buffer_len);


private:

    void AcceptingThread();

    SRTSOCKET AcceptNewClient();

    int ConfigurePre(SRTSOCKET sock);
    int ConfigureAcceptedSocket(SRTSOCKET sock);


private:

    std::list<SRTSOCKET>      m_accepted_sockets;
    SRTSOCKET                 m_bindsock      = SRT_INVALID_SOCK;
    int                       m_epoll_accept  = -1;
    int                       m_epoll_receive = -1;

private:

    std::atomic<bool> m_stop_accept = { false };

    std::thread m_accepting_thread;

private:    // Configuration

    std::string m_host;
    int m_port;
    std::map<string, string> m_options; // All other options, as provided in the URI

};


