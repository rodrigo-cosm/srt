#include <vector>
#include <iostream>
#include <thread>
#include "srt-messenger.h"

using namespace std;


void main(int argc, char** argv)
{
    const size_t message_size = 1024 * 1024;

    auto server = std::thread([&message_size]
    {
        srt_msngr_listen("srt://4200", message_size * 2);
    });

    srt_msngr_connect("srt://127.0.0.1:4200", message_size);

    server.join();

    vector<char> message_sent(message_size);
    char c = 0;
    for (size_t i = 0; i < message_sent.size(); ++i)
    {
        message_sent[i] = c++;
    }

    const int sent_res = srt_msngr_send(message_sent.data(), message_sent.size());
    if (sent_res != message_size)
    {
        cerr << "ERROR: Sending " << message_size << ", sent " << sent_res << "\n";
        return;
    }

    vector<char> message_rcvd(message_size);
    const int recv_res = srt_msngr_recv(message_rcvd.data(), message_rcvd.size());
    if (recv_res != message_size)
    {
        cerr << "ERROR: Receiving " << message_size << ", received " << recv_res << "\n";
        cerr << srt_msngr_getlasterror_str();
        return;
    }

    for (size_t i = 0; i < message_sent.size(); ++i)
    {
        if (message_sent[i] != message_rcvd[i])
            cerr << "ERROR: Pos " << i
                 << " received " << int(message_rcvd[i]) << ", actually sent " << int(message_sent[i]) << "\n";
    }

}

