#include <vector>
#include <iostream>
#include <thread>
#include "srt-messenger.h"

using namespace std;


void main(int argc, char** argv)
{
    const size_t message_size = 1024 * 1024;

    srt_msngr_listen("srt://:4200?maxconn=4", message_size * 2);

    vector<char> message_rcvd(message_size);
    bool rcv_error = false;

    auto rcv_thread = std::thread([&message_rcvd, &rcv_error, &message_size]
    {
        const int recv_res = srt_msngr_recv(message_rcvd.data(), message_rcvd.size());
        if (recv_res != message_size)
        {
            cerr << "ERROR: Receiving " << message_size << ", received " << recv_res << "\n";
            cerr << srt_msngr_getlasterror_str();
            rcv_error = true;
        }
    });

    // This should block untill we get connected
    srt_msngr_connect("srt://127.0.0.1:4200", message_size);

    // Now we are connected, start sending the message
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

    // Wait for another thread to receive the message (receiving thread)
    rcv_thread.join();

    if (rcv_error)
    {
        // There was an error when receiving. No need to check the message
        return;
    }

    bool mismatch_found = false;
    for (size_t i = 0; i < message_sent.size(); ++i)
    {
        if (message_sent[i] == message_rcvd[i])
            continue;
        
        mismatch_found = true;
        cerr << "ERROR: Pos " << i
                << " received " << int(message_rcvd[i]) << ", actually sent " << int(message_sent[i]) << "\n";
    }

    if (!mismatch_found)
        cerr << "Check passed\n";

    srt_msngr_destroy();
}

