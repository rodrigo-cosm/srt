#!/usr/bin/tclsh
#*
#* SRT - Secure, Reliable, Transport
#* Copyright (c) 2020 Haivision Systems Inc.
#* 
#* This Source Code Form is subject to the terms of the Mozilla Public
#* License, v. 2.0. If a copy of the MPL was not distributed with this
#* file, You can obtain one at http://mozilla.org/MPL/2.0/.
#* 
#*/
#
#*****************************************************************************
#written by
#  Haivision Systems Inc.
#*****************************************************************************

set statsfields {

	# Phase 0
	# General stats
	long	timestamp 		msTimeStamp
	long	sndperiod		usPktSndPeriod
	int		window-flow		pktFlowWindow
	int		window-cong 	pktCongestionWindow
	int 	flightsize		pktFlightSize
	double 	rtt				msRTT
	double 	bandwidth		mbpsBandwidth
	double 	maxbw			mbpsMaxBW
	int 	mss				byteMSS
	int 	rdistance		pktReorderDistance
	int 	rtolerance		pktReorderTolerance

	# Buffer - sender stats
    int     snd-pkt-buf         pktSndBuf
    int     snd-byte-buf        byteSndBuf
    int     snd-ms-buf          msSndBuf
    int     snd-byte-avail-buf  byteAvailSndBuf
    int     snd-pkt-buf-avg     pktSndBufAvg
    int     snd-byte-buf-avg    byteSndBufAvg
    int     snd-ms-buf-avg      msSndBufAvg
    int     snd-byte-avail-buf-avg byteAvailSndBufAvg
    int     snd-ms-latency      msSndTsbPdDelay

	# Buffer - receiver stats
	int		rcv-pkt-buf	        pktRcvBuf
	int		rcv-byte-buf        byteRcvBuf
	int		rcv-ms-buf	        msRcvBuf
    int     rcv-byte-avail-buf  byteAvailRcvBuf
	int		rcv-pkt-buf-avg     pktRcvBufAvg
	int		rcv-byte-buf-avg    byteRcvBufAvg
	int		rcv-ms-buf-avg      msRcvBufAvg
    int     rcv-byte-avail-buf-avg  byteAvailRcvBufAvg
	int		rcv-ms-latency	    msRcvTsbPdDelay

	# Sender stats
	long    snd-pkt-local   pktSent
	long    snd-pkt-total   pktSentTotal
	ulong   snd-byte-local byteSent
	ulong   snd-byte-total byteSentTotal
	int		snd-pkt-loss-local pktSndLoss
	int		snd-pkt-loss-total pktSndLossTotal

	# Receiver stats
}

set here [file dirname [file normalize $argv0]]
source $here/utilities.tcl

set outfile ../srtcore/srt.stats.h

set statsfields [no_comments $statsfields]

set fd [open $outfile w]

puts $fd "

enum SrtStatsKey
\{"

foreach {type symname username} $statsfields {
	puts $fd "    SRTM_[screamcase $symname],"
}

puts $fd "    SRTM_E_SIZE
};


// Types
"

foreach {type symname username} $statsfields {
	puts $fd "#define SRTM_TYPE_[screamcase $symname] [screamcase $type]"
}

close $fd

# Generate labels for stats

set outfile ../apps/statlabels.cpp

set fd [open $outfile w]

puts $fd "
#include <string>
#include <srt.h>

extern const std::string transmit_stats_labels \[SRT_STATS_SIZE\] = \{
"

set com ""
foreach {type symname username} $statsfields {
	puts -nonewline $fd "$com\n    \"$username\""
	set com ","
}

puts $fd "\n};"

close $fd

