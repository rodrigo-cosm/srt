#include "srt-messenger.h"
#include "uriparser.hpp"
#include "testmedia.hpp"


static std::unique_ptr<SrtModel> s_model;


int srt_msngr_create()
{
    return 0;
}


SRTSOCKET srt_msngr_connect(char *uri, size_t message_size)
{
    UriParser ut(uri);
    ut["transtype"]  = string("file");
    ut["messageapi"] = string("true");
    ut["sndbuf"]     = to_string(8 * 1061313);
    SrtModel m(ut.host(), ut.portno(), ut.parameters());

    string dummy;
    m.Establish(Ref(dummy));

    return m.Socket();
}


SRTSOCKET srt_msngr_listen(int port, size_t message_size)
{
    UriParser ut("srt://:" + std::to_string(port));
    ut["transtype"] = string("file");
    ut["messageapi"] = string("true");
    ut["sndbuf"] = to_string(8 * 1061313);
    SrtModel m(ut.host(), ut.portno(), ut.parameters());

    string dummy;
    m.Establish(Ref(dummy));

    return m.Socket();
}


int srt_msngr_send(SRTSOCKET sock, const char *buffer, size_t buffer_len)
{
    const int n = srt_send(sock, buffer, buffer_len);
    return n;
}


int srt_msngr_recv(SRTSOCKET sock, char *buffer, size_t buffer_len)
{
    const int n = srt_recv(sock, buffer, buffer_len);
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
    return 0;
}


