/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

#include "socketoptions.hpp"

using namespace std;


extern const set<string> true_names = { "1", "yes", "on", "true" };
extern const set<string> false_names = { "0", "no", "off", "false" };

extern const std::map<std::string, int> enummap_transtype = {
    { "live", SRTT_LIVE },
    { "file", SRTT_FILE }
};


bool set_opt_transtype(int socket, int proto, int sym, const std::string &value)
{
    const std::map<std::string, int> enummap_transtype = {
        { "live", SRTT_LIVE },
        { "file", SRTT_FILE }
    };

    auto p = enummap_transtype.find(value);
    if (p != enummap_transtype.end())
    {
        const int set_res
            = srt_setsockopt(socket, proto, SRT_SOCKOPT(sym), &p->second, sizeof(p->second));
        return set_res != SRT_ERROR;
    }
    return false;
}


bool set_opt_linger(int socket, int proto, int sym, const std::string &value)
{
    int int_val = 0;
    try
    {
        int_val = stoi(value);
    }
    catch (const std::invalid_argument& ia)
    {
        return false;
    }
    catch (const std::out_of_range& oor)
    {
        return false;
    }

    linger lin;
    lin.l_linger = int_val;
    lin.l_onoff  = lin.l_linger > 0 ? 1 : 0;
    const int set_res
        = srt_setsockopt(socket, proto, SRT_SOCKOPT(sym), &lin, sizeof(linger));
    return set_res != SRT_ERROR;
}


SocketOption::Mode SrtConfigurePre(SRTSOCKET socket, string host, map<string, string> options, vector<string>* failures)
{
    vector<string> dummy;
    vector<string>& fails = failures ? *failures : dummy;

    if ( options.count("passphrase") )
    {
        /*
        // Insert default
        if ( options.count("pbkeylen") == 0 )
        {
            options["pbkeylen"] = "16"; // m_output_direction ? "16" : "0";
        }
        */
    }

    SocketOption::Mode mode;
    string modestr = "default";

    if ( options.count("mode") )
    {
        modestr = options["mode"];
    }

    if ( modestr == "client" || modestr == "caller" )
    {
        mode = SocketOption::CALLER;
    }
    else if ( modestr == "server" || modestr == "listener" )
    {
        mode = SocketOption::LISTENER;
    }
    else if ( modestr == "default" )
    {
        // Use the following convention:
        // 1. Server for source, Client for target
        // 2. If host is empty, then always server.
        if ( host == "" )
            mode = SocketOption::LISTENER;
        //else if ( !dir_output )
        //mode = "server";
        else
        {
            // Host is given, so check also "adapter"
            if ( options.count("adapter") )
                mode = SocketOption::RENDEZVOUS;
            else
                mode = SocketOption::CALLER;
        }
    }
    else
    {
        mode = SocketOption::FAILURE;
        fails.push_back("mode");
    }

    if (options.count("linger"))
    {
        linger lin;
        lin.l_linger = stoi(options["linger"]);
        lin.l_onoff  = lin.l_linger > 0 ? 1 : 0;
        srt_setsockopt(socket, SocketOption::PRE, SRTO_LINGER, &lin, sizeof(linger));
    }


    bool all_clear = true;
    for (auto o: srt_options)
    {
        if ( o.binding == SocketOption::PRE && options.count(o.name) )
        {
            string value = options.at(o.name);
            bool ok = o.apply<SocketOption::SRT>(socket, value);
            if ( !ok )
            {
                fails.push_back(o.name);
                all_clear = false;
            }
        }
    }

    return all_clear ? mode : SocketOption::FAILURE;
}

void SrtConfigurePost(SRTSOCKET socket, map<string, string> options, vector<string>* failures)
{
    vector<string> dummy;
    vector<string>& fails = failures ? *failures : dummy;

    for (auto o: srt_options)
    {
        if ( o.binding == SocketOption::POST && options.count(o.name) )
        {
            string value = options.at(o.name);
            bool ok = o.apply<SocketOption::SRT>(socket, value);
            if ( !ok )
                fails.push_back(o.name);
        }
    }
}


