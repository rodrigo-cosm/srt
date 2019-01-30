#include <list>
#include "srt-messenger.h"
#include "uriparser.hpp"
#include "testmedia.hpp"

using namespace std;

static unique_ptr<SrtModel> s_rcv_srt_model;
static unique_ptr<SrtModel> s_snd_srt_model;

list<SRTSOCKET> s_rcv_sockets;
list<SRTSOCKET> s_snd_socket;



int srt_msngr_connect(char *uri, size_t message_size)
{
    UriParser ut(uri);
    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["sndbuf"]     = to_string(8 * 1061313);

    s_snd_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    string dummy;
    s_snd_srt_model->Establish(Ref(dummy));

    return s_snd_srt_model->Socket();
}


int srt_msngr_listen(char *uri, size_t message_size)
{
    UriParser ut(uri);
    ut["transtype"] = string("file");
    ut["messageapi"] = string("true");
    ut["sndbuf"] = to_string(8 * 1061313);
    s_rcv_srt_model = std::make_unique<SrtModel>(SrtModel(ut.host(), ut.portno(), ut.parameters()));

    string dummy;
    s_rcv_srt_model->Establish(Ref(dummy));

    return s_snd_srt_model->Socket();
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


