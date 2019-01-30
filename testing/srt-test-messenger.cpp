#include "srt-messenger.h"


void main(int argc, char** argv)
{
    srt_msngr_listen ("srt://4200", 1024);

    srt_msngr_connect("srt://127.0.0.1:4200", 1024);
}

