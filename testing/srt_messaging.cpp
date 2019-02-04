#include <list>
#include <thread>
#include "srt_messaging.h"
#include "uriparser.hpp"
#include "testmedia.hpp"

using namespace std;

static unique_ptr<SrtModel> s_rcv_srt_model;
static unique_ptr<SrtModel> s_snd_srt_model;

static list<SRTSOCKET> s_rcv_sockets;
static list<SRTSOCKET> s_snd_socket;
static int s_rcv_epoll_id;



int srt_msgn_connect(const char *uri, size_t message_size)
{
    UriParser ut(uri);

    if (ut.port().empty())
    {
        cerr << "ERROR! Check the URI provided: " << uri << endl;
        return -1;
    }

    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["blocking"]   = string("true");
    ut["mode"]       = string("caller");

    // If we have this parameter provided, probably someone knows better
    if (!ut["sndbuf"].exists())
    {
        ut["sndbuf"] = to_string(3 * (message_size * 1472 / 1456 + 1472));
    }

    s_snd_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    try
    {
        string connection_id;
        s_snd_srt_model->Establish(Ref(connection_id));
    }
    catch (TransmissionError &err)
    {
        cerr << "ERROR! While setting up a listener: " << err.what() << endl;
        return -1;
    }

    return s_snd_srt_model->Socket();
}


int srt_msgn_listen(const char *uri, size_t message_size)
{
    UriParser ut(uri);

    if (ut.port().empty())
    {
        cerr << "ERROR! Check the URI provided: " << uri << endl;
        return -1;
    }

    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["blocking"]   = string("true");
    ut["mode"]       = string("listener");

    int maxconn = 5;
    if (ut["maxconn"].exists())
    {
        maxconn = std::stoi(ut.queryValue("maxconn"));
    }
    
    // If we have this parameter provided, probably someone knows better
    if (!ut["rcvbuf"].exists())
    {
        ut["rcvbuf"] = to_string(3 * (message_size * 1472 / 1456 + 1472));
    }
    s_rcv_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    // Prepare a listener to accept up to 5 conections
    try
    {
        s_rcv_srt_model->PrepareListener(maxconn);
    }
    catch (TransmissionError &err)
    {
        cerr << "ERROR! While setting up a listener: " << err.what() << endl;
        return -1;
    }

    return 0;
}


int srt_msgn_send(const char *buffer, size_t buffer_len)
{
    if (!s_snd_srt_model)
        return -1;

    const int n = srt_send(s_snd_srt_model->Socket(), buffer, buffer_len);
    return n;
}


int srt_msgn_recv(char *buffer, size_t buffer_len)
{
    if (!s_rcv_srt_model)
        return -1;

    if (s_rcv_srt_model->Socket() == SRT_INVALID_SOCK)
    {
        try
        {
            s_rcv_srt_model->AcceptNewClient();
        }
        catch (TransmissionError &err)
        {
            cerr << "ERROR! While accepting a connection: " << err.what() << endl;
            return -1;
        }
    }

    const int n = srt_recv(s_rcv_srt_model->Socket(), buffer, buffer_len);
    return n;
}


const char* srt_msgn_getlasterror_str(void)
{
    return srt_getlasterror_str();
}


int srt_msgn_getlasterror(void)
{
    return srt_getlasterror(NULL);
}


int srt_msgn_destroy()
{
    if (s_snd_srt_model)
    {
        // We have to check if the sending buffer is empty.
        // Or we will loose this data.
        const SRTSOCKET sock = s_snd_srt_model->Socket();
        size_t blocks = 0;
        do
        {
            if (SRT_ERROR == srt_getsndbuffer(sock, &blocks, nullptr))
                break;

            if (blocks)
                this_thread::sleep_for(5ms);
        } while (blocks != 0);
    }
    s_snd_srt_model.reset();
    s_rcv_srt_model.reset();
    return 0;
}


