/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

/*****************************************************************************
written by
   Haivision Systems Inc.
 *****************************************************************************/

#ifndef INC__SRT_MESSENGER_H
#define INC__SRT_MESSENGER_H
#include "srt.h"


SRT_API extern int         srt_msngr_create();


/**
 * Establish SRT connection to a a remote host.
 *
 * @param uri           a null terminated string representing remote URI
 *                      (e.g. "srt://192.168.0.12:4200")
 * @param messahe_size  payload size of one message to send
 */
SRT_API extern SRTSOCKET   srt_msngr_connect(char *uri, size_t message_size);


/**
 * Listen for the incomming SRT connections.
 *
 * @param port          port number to listen on (e.g. 4200)
 * @param messahe_size  payload size of one message to send
 */
SRT_API extern SRTSOCKET   srt_msngr_listen (int port, size_t message_size);


/**
 * Receive a message.
 *
 * @param socket        SRT socket to receive a message on (was returned by srt_msngr_listen())
 * @param buffer        a buffer to send (has be less then the `message_size` used in srt_msngr_listen())
 * @param buffer_len    length of the buffer
 *
 * @return              number of bytes actually sent
 */
SRT_API extern int         srt_msngr_send(SRTSOCKET sock, const char *buffer, size_t buffer_len);


/**
 * Send a message.
 *
 * @param socket        SRT socket to receive a message on (was returned by srt_msngr_connect())
 * @param buffer        a buffer to send (has be less then the `message_size` used in srt_msngr_connect())
 * @param buffer_len    length of the buffer
 */
SRT_API extern int         srt_msngr_recv(SRTSOCKET sock, char *buffer, size_t buffer_len);



SRT_API extern const char* srt_msngr_getlasterror_str(void);


SRT_API extern int         srt_msngr_getlasterror(void);


SRT_API extern int         srt_msngr_destroy();


#endif // INC__SRT_MESSENGER_H