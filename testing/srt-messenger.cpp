#include <list>
#include "srt-messenger.h"
#include "uriparser.hpp"
#include "testmedia.hpp"

using namespace std;

static unique_ptr<SrtModel> s_rcv_srt_model;
static unique_ptr<SrtModel> s_snd_srt_model;

static list<SRTSOCKET> s_rcv_sockets;
static list<SRTSOCKET> s_snd_socket;
static int s_rcv_epoll_id;



int srt_msngr_connect(char *uri, size_t message_size)
{
    UriParser ut(uri);
    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["blocking"]   = string("true");
    ut["sndbuf"]     = to_string(3 * (message_size * 1472 / 1456 + 1472));

    s_snd_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    try
    {
        string connection_id;
        s_snd_srt_model->Establish(Ref(connection_id));
    }
    catch (TransmissionError &err)
    {
        cerr << "ERROR! While setting up a listener: " << err.what();
        return -1;
    }

    return s_snd_srt_model->Socket();
}


int srt_msngr_listen(char *uri, size_t message_size)
{

    UriParser ut(uri);
    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["blocking"]   = string("true");

    int maxconn = 5;
    if (ut["maxconn"].exists())
    {
        maxconn = std::stoi(ut.queryValue("maxconn"));
    }

    ut["rcvbuf"]     = to_string(3 * (message_size * 1472 / 1456 + 1472));
    s_rcv_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    // Prepare a listener to accept up to 5 conections
    try
    {
        s_rcv_srt_model->PrepareListener(maxconn);
    }
    catch (TransmissionError &err)
    {
        cerr << "ERROR! While setting up a listener: " << err.what();
        return -1;
    }

    return 0;
}


int srt_msngr_send(const char *buffer, size_t buffer_len)
{
    if (!s_snd_srt_model)
        return -1;

    const int n = srt_send(s_snd_srt_model->Socket(), buffer, buffer_len);
    return n;
}


int srt_msngr_recv(char *buffer, size_t buffer_len)
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
            cerr << "ERROR! While accepting a connection: " << err.what();
            return -1;
        }
    }

    const int n = srt_recv(s_rcv_srt_model->Socket(), buffer, buffer_len);
    return n;
}


const char* srt_msngr_getlasterror_str(void)
{
    return srt_getlasterror_str();
}


int srt_msngr_getlasterror(void)
{
    return srt_getlasterror(NULL);
}


int srt_msngr_destroy()
{
    s_snd_srt_model.release();
    s_rcv_srt_model.release();
    return 0;
}


