#include "smoother.h"
#include "core.h"

//#include "ext_hash_map.h"
#include <cmath>
#include <map>

class FlowSmoother: public SmootherBase
{
    // Fields from CUDTCC
    const int m_iRCInterval;			// UDT Rate control interval
    uint64_t m_LastRCTime;		// last rate increase time
    int32_t m_iLastRCTimeAck;			// last ACKed seq no
    int32_t m_iLastRCTimeSndSeq;		// max pkt seq no sent out when last decrease happened
    double m_dLastDecPeriod;		// value of pktsndperiod when last decrease happened
    int m_iNAKCount;                     // NAK counter
    int m_iDecRandom;                    // random threshold on decrease by number of loss events
    int m_iAvgNAKNum;                    // average number of NAKs per congestion
    int m_iDecCount;			// number of decreases in a congestion epoch

    uint64_t m_LastAckTime;     // last time when ACK was received
    int32_t m_LastAckSeq;      // ACK sequence number recorded last time

    // Loss tracking.
    //
    // When a loss is spotted, it usually means that the packets
    // are being sent too fast. However we can't say what exactly
    // happened without getting more information about the loss.
    // Things that need to be done are:
    //
    // 1. Exit the FS_WARMUP state immediately - sending with maximum
    //    possible speed might choke the transmission completely.
    // 2. Start doing the short loss measurement (LossSpeed), that is
    //    initialize the index and collect losses in consecutive short cells.
    //    When the FS_SAMPLE_WIDTH is reached earlier (which means that
    //    packet loss is below 25%) it means that the loss is not tragic,
    //    can be measured with the normal measurement process.
    // 3. Do the 4-times loss measurement:
    //    - with every loss, check if the number of lost packets exceed 16.
    //      if so, its treated as a choke point and decreasing the speed must
    //      happen immediately
    //    - Fill the loss information for every range of 16 packets, by
    //      checking the sequence numbers of the loss.
    //    - When filled the whole 4 cells, check the percentage of packet loss.
    //      if the percentage of the packet loss increases, switch to FS_SLIDE.
    // 4. When the loss has no increasing tendency, switch to FS_CLIMB and start
    //    long term measurements.

    struct SeqRange
    {
        int32_t begin, end;
        size_t size;
        SeqRange(): begin(), end(), size() {}

        void set_including(int32_t from, int32_t to)
        {
            begin = from;
            end = to;
            // seqlen's precondition is that the 1st sequence is earlier than the 2nd.
            size = CSeqNo::seqlen(begin, end);
        }

        void set_excluding(int32_t from, int32_t to)
        {
            if (from == to)
                return; // ignore empty ranges
            begin = from;
            end = CSeqNo::decseq(to);
            size = CSeqNo::seqlen(begin, end);
        }

        void set_one(int32_t val)
        {
            begin = val;
            end = val;
            size = 1;
        }
    };

    // These fields collect the lost and acknowledged
    // sequence ranges reported at the LOSSREPORT event.
    // They collect the extra packet loss rate information
    // that is unavailable yet for this range of sent packets
    // (because it comes only with ACK message).
    vector<SeqRange> m_UnackLossRange;
    vector<SeqRange> m_UnackAckRange;

    double m_dLastPktSndPeriod;
    double m_dTimedLossRate;         // Timed loss rate (packets lost per microsecond)
    double m_dLossRate;              // Percentage loss rate (packets lost in the number of packets transmitted)
    uint64_t m_SenderRTT_us;   // Sender RTT (time between sending data packet and receiving ACK for it)

    int m_SenderSpeed_pps;
    int m_MaxSenderSpeed_pps;
    int64_t m_SenderSpeed_bps;

    JumpingAverage<int, 3, 4> m_SenderSpeedMeasure;
    JumpingAverage<int, 3, 4> m_SenderRTTMeasure;
    JumpingAverage<double, 3, 2> m_LossRateMeasure;

    int m_LastRcvVelocity;
    int m_LastRcvSpeed;
    int m_LastSenderRTT;

    bool m_bLoss; // XXX temporarily kept for the time of experiments

    size_t m_zTimerCounter;
    double m_dLastCWNDSize;

    // The long time measurements rely on collecting information with every
    // received UMSG_ACK and handled in the TEV_ACK handler.
    //
    // This should take the short-term values of:
    // - delivery speed: 1/sender_RTT, where sender_RTT is time distance bw. sending packet SEQ=X, where ACK_SEQ=X+1.
    // - receiving speed: the momentary (NOT average) receiving speed as calculated by receiver (ACKD_RCVVELOCITY)
    // - sending speed: locally measured through time distance between sending consecutive packets
    // - loss rate: how many percent of the packets were lost in this ack period
    // - congestion rate: how the flight size has changed towards the previous period

    struct Probe
    {
        double snd_period;
        int64_t tx_velocity, rx_velocity, rx_speed;
        uint64_t sender_rtt;
        double lossrate;
        double congestion_rate;
    };

    // 16 consecutive ACK events are used to make measurements, with
    // jumping averate with segment size of 4. Decisions are made depending
    // on the control state:

    // FS_WARMUP.
    //
    // No decision is undertaken until the first loss is found or until
    // the flight size reaches the size of the congestion window. If a clean
    // exit was done from the FS_WARMUP state, it switches to FS_SLIDE.
    // When a loss happened during this period, this causes immediate switch
    // to FS_CLIMB.
    //
    // FS_CLIMB.
    //
    // This happens when the WARMUP was interrupted abnormally with the prediction
    // that further speeding up may choke the link completely. In this state the
    // speed in increased with every 2nd ACK, and the results are constantly measured.
    // In the beginning there is no top speed found, so the speedup is finished if
    // the consecutive 16 speed increases caused consequent decreasing of the receiver
    // speed, delivery speed (decreased RTT), or packet loss.
    //
    // FS_SLIDE.
    //
    // The speed is first set to the level of the received value of ACKD_RCVSPEED.
    // Then the speed is being slowed down with expectation to find the maximum value
    // of the reciving speed as measured through ACKD_RCVVELOCITY and delivery speed
    // (1/RTT). If no maximum found during this measurement, return to the previous
    // speed and switch to FS_CLIMB (as this means that no maximum speed has been measured).
    // Otherwise switch to FS_KEEP.
    //
    // FS_KEEP.
    //
    // In this state there should be the already calculated optimal speed used,
    // and it's already known the Pv speed range. The next 4 consecutive
    // measurements should state as to whether the speed has decreased to the
    // level of the lowest of the speeds at the Pv borders, in such a situation the
    // measurements start anew. If the maximum was found exactly at the highest
    // speed, set this speed as current and switch to FS_CLIMB. If at the lowest
    // speed - switch to FS_SLIDE.

    size_t m_zProbeSize;
    size_t m_zProbeSpan;
    // This is for collecting results from consecutive S times (S = m_zProbeSize).
    // Values here will be updated with IIR-calculated average, then reset. When
    // resetting the calculated value will be placed in m_adStats.

    static const size_t FS_SAMPLE_WIDTH = 32;
    static const size_t FS_HISTORY_SIZE = 4;

    // After collecting consecutive results in m_adProbe, collect them with snapping
    // to grid into this array.
    Probe m_adStats[FS_SAMPLE_WIDTH];

    enum FlowState { FS_WARMUP, FS_SLIDE, FS_CLIMB, FS_KEEP, FS_BITE };

    struct State
    {
        FlowState state;
        unsigned probe_index;
        unsigned span_index;

        State(FlowState s): state(s), probe_index(), span_index() {}
    } m_State;

    struct ProbeHistoryEntry
    {
        Probe probe; // Final "best" values.
        FlowState state; // State that was there previously
        std::pair<double, double> speedRange;
        double stepfactor;
    };

    RingLossyStack<ProbeHistoryEntry, FS_HISTORY_SIZE> m_ProbeHistory;

    std::string DisplayState(const State& st)
    {
        static std::string stnames[5] = {"WARMUP", "SLIDE", "CLIMB", "KEEP", "BITE"};
        std::ostringstream os;
        os << stnames[st.state] << "[" << setfill('_') << setw(2) << st.probe_index << "." << st.span_index << "]";
        return os.str();
    }

    int64_t m_llMaxBW;

    double m_dPktSndPeriod_LO_us;
    double m_dPktSndPeriod_HI_us;

    static constexpr double MAX_SPEED_SND_PERIOD = 1;      // 1ms bw packets
    static constexpr double MIN_SPEED_SND_PERIOD = 250000; // 0.25s bw packets

public:
    FlowSmoother(CUDT* parent):
        SmootherBase(parent),
        m_iRCInterval(CUDT::COMM_SYN_INTERVAL_US), // == 10ms
        m_State(FS_WARMUP)
    {
        // Note that this function is called at the moment of
        // calling m_Smoother.configure(this). It is placed more less
        // at the same position as the series-of-parameter-setting-then-init
        // in the original UDT code. So, old CUDTCC::init() can be moved
        // to constructor.

        m_LastRCTime = CTimer::getTime();

        m_iLastRCTimeAck = m_parent->sndSeqNo();
        m_iLastRCTimeSndSeq = CSeqNo::decseq(m_iLastRCTimeAck);
        m_dLastDecPeriod = 1;
        m_iAvgNAKNum = 0;
        m_iNAKCount = 0;
        m_iDecRandom = 1;
        m_dLastPktSndPeriod = 1;
        m_dLossRate = 0;
        m_dTimedLossRate = 0;
        m_bLoss = false;
        m_SenderRTT_us = 0;
        m_SenderSpeed_pps = 0;
        m_MaxSenderSpeed_pps = 0;
        m_SenderSpeed_bps = 0;
        m_LastRcvVelocity = 0;
        m_LastRcvSpeed = 0;
        m_LastSenderRTT = 0;
        m_zTimerCounter = 0;

        m_LastAckTime = 0; // Don't measure anything at the first ever ACK
        m_LastAckSeq = m_iLastRCTimeAck;

        memset(&m_adStats, 0, sizeof(m_adStats));

        m_zProbeSize = FS_SAMPLE_WIDTH; // Initial for FS_WARMUP, might be changed later.
        m_zProbeSpan = 1;

        // SmotherBase
        m_dCongestionWindow = 16;
        m_dPktSndPeriod_us = 1;

        m_dLastCWNDSize = 16;

        // Initial zero to declare them "not measured yet"
        m_dPktSndPeriod_HI_us = 0;
        m_dPktSndPeriod_LO_us = 0;

        m_llMaxBW = 0;

#define CONNECT(signal, slot) m_parent->ConnectSignal<signal>(MakeEventSlot(this, &FlowSmoother:: slot))
// Maybe a syntax in C++20:
// this->onACK  <==>  bind(this, &FlowSmoother::onACK)

        CONNECT(TEV_ACK, onACK);
        CONNECT(TEV_LOSSREPORT, onLOSS_CollectLoss);
        CONNECT(TEV_CHECKTIMER, onTIMER);
#undef CONNECT

        HLOGC(mglog.Debug, log << "Creating FlowSmoother");
    }

    bool checkTransArgs(Smoother::TransAPI, Smoother::TransDir, const char* , size_t , int , bool ) ATR_OVERRIDE
    {
        // XXX
        // The FlowSmoother has currently no restrictions, although it should be
        // rather required that the "message" mode or "buffer" mode be used on both sides the same.
        // This must be somehow checked separately.
        return true;
    }

    bool needsQuickACK(const CPacket& pkt) ATR_OVERRIDE
    {
        // For FlowSmoother, treat non-full-buffer situation as an end-of-message situation;
        // request ACK to be sent immediately.
        if (pkt.getLength() < m_parent->maxPayloadSize())
            return true;

        return false;
    }

    void updateBandwidth(int64_t maxbw, int64_t) ATR_OVERRIDE
    {
        if (maxbw != 0)
        {
            m_llMaxBW = maxbw;
            HLOGC(mglog.Debug, log << "FlowSmoother: updated BW: " << m_llMaxBW);
        }
    }

private:

    void updateCongestionWindowSize(int32_t ack)
    {
        if (m_State.state == FS_WARMUP)
        {
            // During Slow Start period, update with the number of
            // newly ack-ed packets, and if exceeds the maximum, exit Slow Start.
            // Otherwise update the size from delivery rate and RTT.
            m_dCongestionWindow += CSeqNo::seqlen(m_iLastRCTimeAck, ack);
            m_iLastRCTimeAck = ack;

            if (m_dCongestionWindow > m_dMaxCWndSize)
            {
                reachCWNDTop("UPD/ACK");
            }
            else
            {
                HLOGC(mglog.Debug, log << "FlowSmoother: UPD/ACK (slowstart:KEPT) wndsize="
                    << m_dCongestionWindow << "/" << m_dMaxCWndSize
                    << " sndperiod=" << m_dPktSndPeriod_us << "us");
            }
        }
        else
        {
            m_dCongestionWindow = m_parent->deliveryRate() / 1000000.0 * (m_parent->RTT() + m_iRCInterval) + 16;
        }
    }

    void reachCWNDTop(const char* hdr)
    {
        // If we have reached the top window, set the speed
        // the the receiver speed, then keep it for the next 16 probes.
        // Set the current speed to keep for the next 16 ACKs
        // then try to speed up.

        if (m_parent->deliveryRate() > 0)
        {
            m_dPktSndPeriod_us = 1000000.0 / m_parent->deliveryRate();
            HLOGC(mglog.Debug, log << "FlowSmoother: " << hdr << " (slowstart:ENDED) CWND="
                    << setprecision(6) << m_dCongestionWindow << "/" << m_dMaxCWndSize
                    << " sndperiod=" << m_dPktSndPeriod_us << "us [EQUAL TO DELIVERY RATE: "
                    << m_parent->deliveryRate() << "p/s]");
        }
        else
        {
            m_dPktSndPeriod_us = m_dCongestionWindow / (m_parent->RTT() + m_iRCInterval);
            HLOGC(mglog.Debug, log << "FlowSmoother: " << hdr << " (slowstart:ENDED) CWND="
                    << setprecision(6) << m_dCongestionWindow << "/" << m_dMaxCWndSize
                    << " sndperiod=" << m_dPktSndPeriod_us
                    << "us [BASED ON RTT+RCI=" << ((m_parent->RTT() + m_iRCInterval)/1000.0)
                    << "ms = " << (1000000.0/(m_parent->RTT() + m_iRCInterval)) << "Hz of CWND]");
        }

        switchState(FS_BITE, 16, 1, m_dPktSndPeriod_us, 0);
    }

    void switchState(FlowState state, size_t probesize, size_t probespan,
            uint64_t slowest, uint64_t fastest)
    {
        m_State = State(state);
        m_zProbeSize = probesize;
        m_zProbeSpan = probespan;

        m_dPktSndPeriod_LO_us = slowest;
        m_dPktSndPeriod_HI_us = fastest;
    }

    void emergencyBrake()
    {
        // This should be done when the loss has exceeded 50%.
        // What we do is to set the sender period to twice that value
        // and switch to keep to observe the results. This might end
        // up with another emergency break until the rate drops below 50%.
        // When reached the minimum speed, do not do any further slowdown,
        // no matter how big the loss rate will result. Worst case scenario,
        // if we have a 50% packet loss with 0.25s distance in time, we'll get
        // connection timeout very soon.

        double slower = 2*m_dPktSndPeriod_us;
        if (slower > MIN_SPEED_SND_PERIOD)
            slower = MIN_SPEED_SND_PERIOD;

        HLOGC(mglog.Debug, log << "FlowSmoother: LOSS >50%, EMERGENCY BRAKE. sndperiod=" << slower);

        // Set the lowest speed as the current speed and highest as 0 (to be measured).
        //switchState(FS_KEEP, 16, 1, slower, 0);
        switchState(FS_BITE, 16, 1, slower, 0);
        m_dPktSndPeriod_us = slower;

        analyzeSpeed();
    }

    void measureSenderSpeed()
    {
        int pps;
        int64_t bps;
        bool got = m_parent->updateSenderSpeed(Ref(pps), Ref(bps));
        if (!got)
            return;

        // First time measured
        if (m_MaxSenderSpeed_pps == 0)
        {
            m_MaxSenderSpeed_pps = pps;
            m_SenderSpeed_pps = pps;
            m_SenderSpeed_bps = bps;
        }
        else
        {
            m_SenderSpeed_bps = avg_iir<4>(m_SenderSpeed_bps, bps);
            m_SenderSpeed_pps = avg_iir<4>(m_SenderSpeed_pps, pps);
        }

    }

    // SLOTS
    void onACK(ETransmissionEvent, TevAckData ad)
    {
        m_zTimerCounter = 0; // Clear the timer that counts TIMERS between ACKs

        uint64_t currtime = CTimer::getTime();

        uint64_t ack_period = currtime - m_LastAckTime;
        if (ack_period == 0)
        {
            HLOGC(mglog.Debug, log << "FlowSmoother: IPE: impossible same-time ACK as previous @ " << logging::FormatTime(m_LastAckTime));
            // Some mistake, would make it div/0. Ignore the event.
            return;
        }

        int32_t farthest_ack = CSeqNo::incseq(m_parent->sndSeqNo());
        int flight_span = CSeqNo::seqlen(ad.ack, farthest_ack) - 1;

        // THIS is the only thing done in the update
        bool continue_stats = updateSndPeriod(currtime, ad.ack);

        int32_t* ackdata;
        int acksize;
        Tie2(ackdata, acksize) = m_parent->rcvAckDataCache();

        if (acksize == 1)
        {
            HLOGC(mglog.Debug, log << "FlowSmoother: LITE ACK DETECTED, not doing anything");
            // For LITE ACK don't make any measurements.
            // (you don't have appropriate data to do it).
            return;
        }

        int32_t last_ack = m_LastAckSeq;
        m_LastAckSeq = ad.ack;
        int32_t last_rcv = acksize > ACKD_RCVLASTSEQ ? ackdata[ACKD_RCVLASTSEQ] : 0;

        // XXX Not used currently - might be used here to
        // calculate the "ack speed", but there's no use of 
        // this data so far.
        // uint64_t last_time = m_LastAckTime;
        m_LastAckTime = currtime;

        double last_cwnd_size = m_dLastCWNDSize;
        m_dLastCWNDSize = m_dCongestionWindow;

        // Measure the sender speed
        measureSenderSpeed();

        int number_loss, number_ack;
        double loss_rate;
        calculateLossData(Ref(loss_rate), Ref(number_loss), Ref(number_ack), last_ack);

        double timed_loss_rate = double(number_loss)/ack_period;
        m_dTimedLossRate = avg_iir<4>(m_dTimedLossRate, timed_loss_rate);

        // Alroght, clear the loss stats.
        m_UnackAckRange.clear();
        m_UnackLossRange.clear();

        /* XXX Temporary keep UDT behavior
        // Here we need to measure the losses.
        // Trigger, if the loss rate exceeds 50%
        if (2*loss_rate > 1)
        {
            HLOGC(mglog.Debug, log << "FlowSmoother(ACK): Loss rate " << int(100*loss_rate) << "%, EMERGENCY BREAK");
            emergencyBrake();
            return;
        }
        */

        // Stats will not continue in FS_BITE state, if speed
        // was not updated.
        /*
        if (!continue_stats)
        {
            continue_stats = false;
            return;
        }
        */

        // During warmup time (barely possible, but still)

        uint64_t acked_rcv_time = 0;

        std::ostringstream rttform;

        // Extract the acked-to-ack delay from the ack data
        int ack_delay = acksize > ACKD_ACKDELAY ? ackdata[ACKD_ACKDELAY] : -1; // Also received ACKDELAY may be -1
        if (ack_delay != -1)
        {
            // Now we have the previous data, the difference can be calculated.
            int ack_span = CSeqNo::seqcmp(ad.ack, last_ack);
            uint64_t acked_rcv_period = ack_period - ack_delay;

            // ACK period is the time between the previous ACK and current ACK.
            // By ACK delay we know how much time elapsed between reception of
            // the ACK-ed packet and sending the ACK for it.
            //
            // So, the ACK period decreased by this time delay gives a time that
            // elapsed since last ACK and the moment when the newly ACK-ed packet
            // was received. Then we simply use the proportional equation:

            // acked_rcv_period     ack_period
            // ---------------- == ---------------
            // ack_span              ack_size (X)

            int ack_size = (number_loss > 0) // lost packets collected since last ACK
                ? ack_span : ack_span * (double(ack_period)/acked_rcv_period);

            // By having this value, we have a loss rate calculated with time-predicted
            // number of packets to be received during this ACK period.
            // If the ack_size is greated by more than twice the loss size,
            // ignore it, otherwise take the greated loss rate of these two.
            if (ack_size > 0 && ack_size < number_ack*2)
            {
                double number_loss_rate = double(number_loss)/ack_size;
                loss_rate = max(loss_rate, number_loss_rate);
            }

            // Order of events in time:
            //
            //   ad.send_time                    currtime
            //   |                               |
            //   sent     received    sent ACK   received ACK
            //     S->       ->D ...   A->         ->A
            //     \\--------//---------\\----------//
            //  snd duration  ack_delay   rcv duration
            //
            //  RTT = snd duration + rcv duration
            //
            // This calculates the time distance between NOW (the last item here)
            // and the send time (the first item here), with exclusion of the time
            // distance between reception of given packet and the time when the receiver
            // decided to send the ACK message for this packet. This 
            acked_rcv_time = currtime - ad.send_time - ack_delay;

            // this value is not to be calculated if the ack_delay could not
            // be delivered.

            // Presentation:
            rttform << "(" << (currtime - ad.send_time) << "-" << ack_delay << ")=" << acked_rcv_time;
        }
        else if (number_loss == 0)
        {
            // When we had no loss since last ACK, then in case of absence of ack_delay
            // we can state that it is negligible and we can still have a reliable measurement
            // of Sender RTT.
            acked_rcv_time = currtime - ad.send_time;
            rttform << "(directly)=" << acked_rcv_time;
        }
        else
        {
            // We can fall back to measuring the time of the last sequence number, find the
            // packet with that sequence number and get its sending time. We state here that
            // the "ack delay time" here is negligible.
            acked_rcv_time = currtime - m_parent->sndTimeOf(ackdata[ACKD_RCVLASTSEQ]);
            rttform << "(@snd." << ackdata[ACKD_RCVLASTSEQ] << ")=" << acked_rcv_time;
        }

        m_LossRateMeasure.update(loss_rate);
        m_dLossRate = m_LossRateMeasure.currentAverage();

        if (m_dTimedLossRate == 0)
            m_dTimedLossRate = timed_loss_rate;
        else
            m_dTimedLossRate = avg_iir<4>(m_dTimedLossRate, timed_loss_rate);


        // Ok, we have a loss rate and sender speed.
        // Receiver velocity will be extracted from the data
        int32_t rcv_velocity = acksize > ACKD_RCVVELOCITY ? ackdata[ACKD_RCVVELOCITY] : 0;
        int32_t rcv_speed = acksize > ACKD_RCVSPEED ? ackdata[ACKD_RCVSPEED] : 0;
        if (rcv_velocity)
            m_LastRcvVelocity = rcv_velocity;
        if (rcv_speed)
            m_LastRcvSpeed = rcv_speed;

        // Zero means "not measured".
        //
        // And we need also the Sender RTT in order to have a delivery speed.
        if (acked_rcv_time)
        {
            m_SenderRTTMeasure.update(acked_rcv_time);
            m_SenderRTT_us = m_SenderRTTMeasure.currentAverage();
            m_LastSenderRTT = acked_rcv_time;
        }

        // Alright, we have the following data now:
        // - Sender Speed: m_SenderSpeed_pps (updated from the sender events)
        // - Receiver Velocity: rcv_velocity (as received from the receiver)
        // - Delivery Speed = 1/Sender RTT: m_SenderRTT_us (as calculated from sender RTT)
        // - Loss rate: loss_rate
        m_adStats[m_State.probe_index] = {
            m_dPktSndPeriod_us,
            m_SenderSpeed_pps,
            rcv_velocity,
            rcv_speed,
            m_SenderRTT_us,
            loss_rate,
            (m_dCongestionWindow - last_cwnd_size)/m_dCongestionWindow
        };

        int32_t last_contig = CSeqNo::decseq(ad.ack);

        HLOGC(mglog.Debug, log << "FlowSmoother: ACK STATS: {" << DisplayState(m_State) << "}"
                << " Snd: Period:" << m_dPktSndPeriod_us << "us"
                << " sRTT:" << rttform.str() << "(" << m_SenderRTT_us << ")"
                << "us Speed=" << m_SenderSpeed_pps << "p/s"
                << " ACKspan:[" << last_ack << "-"
                    << last_contig << ">" << CSeqNo::seqcmp(last_rcv, last_contig)
                    << "](" << CSeqNo::seqlen(last_ack, last_contig) << ")"
                << " Flight:" << flight_span << "p"
                << " CWND:" << setprecision(6) << std::fixed << m_dCongestionWindow
                << " RcvVelocity=" << rcv_velocity << "p/s RcvSpeed=" << rcv_speed << "(" << m_parent->deliveryRate() << ")"
                << "p/s" << " pRTT=" << m_parent->RTT() << "us BW=" << m_parent->bandwidth() << "p/s"
                << " LOSS n=" << number_loss
                << setprecision(6) << std::fixed
                << "p rate:" << loss_rate << "("
                << m_dLossRate << ")p/ack, freq:"
                << int(1000.0*m_dTimedLossRate) << "p/ms "
                << (continue_stats ? "[UPD]" : "[PASS]")
                );

        ++m_State.probe_index;

        if (m_State.probe_index == m_zProbeSize)
        {
            // Reached end of probe size. Analyze results.
            // This shall always RESET THE INDEX.
            analyzeSpeed();
        }
    }

    void analyzeSpeed()
    {
        // Ok, don't do really speed analysis.
        // Just keep the current conditions for stats - experimental
        unsigned last_index = m_State.probe_index;
        m_State.probe_index = 0;

#ifdef ENABLE_HEAVY_LOGGING
        if ( last_index)
            HLOGC(mglog.Debug, log << "SPEED STATS: SP[us]| Tx Veloc | Rx Veloc | Rx Speed |Snd RTT[ms]| Lossrate | Congestion Rate");
        for (unsigned i = 0; i < last_index; ++i)
        {
            Probe& p = m_adStats[i];
            HLOGF(mglog.Debug, " %17f | %8d | %8d | %8d | %9.6f | %8.6f | %f",
                    p.snd_period, int(p.tx_velocity), int(p.rx_velocity), int(p.rx_speed), double(p.sender_rtt/1000.0), p.lossrate, p.congestion_rate);
        }
#endif

        // No decision taken yet. Keep the same state and continue analyzing stats.
    }

    /*
    void lockTopSpeed()
    {
        if (m_dPktSndPeriod_LO_us == 0)
        {
            // No lowest speed yet, use the previous increased speed,
            // or the current speed, whichever is lower.
            // (Note that LO/HI refer to the sending speed terms, but
            // the variable keeps the value using unit INVERTED towards
            // the speed).
            m_dPktSndPeriod_HI_us = std::max(m_dLastDecPeriod, m_dPktSndPeriod_us);
        }
        else
        {
            m_dPktSndPeriod_HI_us = m_dPktSndPeriod_LO_us;
            m_dPktSndPeriod_LO_us = 0; // about to be measured anew
        }

        // Set the SLIDE state and reset the probe index.
        m_State = State(FS_SLIDE);
    }
    */

    void slowIncrease()
    {
        int mss = m_parent->MSS();
        int64_t B = (m_parent->bandwidth() - 1000000.0 / m_dPktSndPeriod_us);
        int64_t B9 = m_parent->bandwidth() / 9;
        if ((m_dPktSndPeriod_us > m_dLastDecPeriod) && (B > B9))
        {
            B = B9;
            HLOGC(mglog.Debug, log << "FlowSmoother: INCREASE: continued, using B=" << B);
        }
        else
        {
            HLOGC(mglog.Debug, log << "FlowSmoother: INCREASE: initial, using B=" << B << " with BW/sndperiod="
                    << (m_parent->bandwidth() / m_dPktSndPeriod_us));
        }

        double inc = 0;
        double i_mss = 1.0/mss;

        if (B <= 0)
            inc = i_mss;
        else
        {
            // inc = max(10 ^ ceil(log10( B * MSS * 8 ) * Beta / MSS, 1/MSS)
            // Beta = 1.5 * 10^(-6)

            inc = pow(10.0, ceil(log10(B * mss * 8.0))) * 0.0000015 / mss;

            if (inc < i_mss)
                inc = i_mss;
        }

        double new_period = (m_dPktSndPeriod_us * m_iRCInterval) / (m_dPktSndPeriod_us * inc + m_iRCInterval);

        /*
        if (m_dPktSndPeriod_HI_us != 0 && m_dPktSndPeriod_us - m_dPktSndPeriod_HI_us > 1)
        {
            // Divide the range between the highest speed and current speed into 10.
            // Take the 9/10 of the distance between current and max and set to the
            // middle point between these values.

            double distance = (m_dPktSndPeriod_us - m_dPktSndPeriod_HI_us)/10*9;
            HLOGC(mglog.Debug, log << "FlowSmoother: FAST SPEEDUP by " << (distance/2) << " -> " << (m_dPktSndPeriod_us - (distance/2)));
            m_dPktSndPeriod_us -= distance/2;
        } else */ {

            HLOGC(mglog.Debug, log << "FlowSmoother: INCREASE factor: " << inc << " with 1/MSS=" << i_mss);

            m_dPktSndPeriod_us = new_period;
        }
    }

    bool updateSndPeriod(uint64_t currtime, int32_t ack)
    {
        if (currtime - m_LastRCTime < (uint64_t)m_iRCInterval)
            return false;

        m_LastRCTime = currtime;
        updateCongestionWindowSize(ack);

        // Increase rate only in "climb" state (when the rate gets
        // increased in order to pick up the maximum receiver speed 

        // This is currently the experimental version with only FS_BITE
        // after exiting FS_WARMUP, that is, it's still done the old way.
        if (m_State.state == FS_WARMUP)
        {
            goto RATE_LIMIT;
        }

        if (m_bLoss)
        {
            m_bLoss = false;
            goto RATE_LIMIT;
        }

        slowIncrease();

RATE_LIMIT:

        showUpdateLog();

        //set maximum transfer rate
        if (m_llMaxBW)
        {
            // bandwidth_pktps = m_llMaxBW/MSS;
            // bandwidth_pktpus = bandwidth_pktpus/1000000.0
            // minSP = 1 / bandwidth_pktpus  [us/pkt]
            double minSP = 1000000.0 / (double(m_llMaxBW) / m_parent->MSS());
            if (m_dPktSndPeriod_us < minSP)
            {
                m_dPktSndPeriod_us = minSP;
                HLOGC(mglog.Debug, log << "FlowSmoother: BW limited to " << m_llMaxBW
                    << " - SLOWDOWN sndperiod=" << m_dPktSndPeriod_us << "us");
            }
        }

        return true;
    }

    void showUpdateLog()
    {
#if ENABLE_HEAVY_LOGGING
        // Try to do reverse-calculation for m_dPktSndPeriod_us, as per minSP below
        // sndperiod = mega / (maxbw / MSS)
        // 1/sndperiod = (maxbw/MSS) / mega
        // mega/sndperiod = maxbw/MSS
        // maxbw = (MSS*mega)/sndperiod
        uint64_t usedbw = (m_parent->MSS() * 1000000.0) / m_dPktSndPeriod_us;

#if defined(unix) && defined (SRT_ENABLE_SYSTEMBUFFER_TRACE)
        // Check the outgoing system queue level
        int udp_buffer_size = m_parent->sndQueue()->sockoptQuery(SOL_SOCKET, SO_SNDBUF);
        int udp_buffer_level = m_parent->sndQueue()->ioctlQuery(TIOCOUTQ);
        int udp_buffer_free = udp_buffer_size - udp_buffer_level;
#else
        int udp_buffer_free = -1;
#endif

        HLOGC(mglog.Debug, log << "FlowSmoother: UPD (state:"
            << DisplayState(m_State) << ") wndsize=" << m_dCongestionWindow
            << " sndperiod=" << m_dPktSndPeriod_us
            << "us BANDWIDTH USED:" << usedbw << " (limit: " << m_llMaxBW
            << ") SYSTEM BUFFER LEFT: " << udp_buffer_free);
#endif
    }

    size_t addLossRanges(TevSeqArray ar)
    {
        size_t nloss = 0;
        for (size_t i = 0; i < ar.second; ++i)
        {
            uint32_t val = ar.first[i];
            if ( IsSet(val, LOSSDATA_SEQNO_RANGE_FIRST) )
            {
                val = SEQNO_VALUE::unwrap(val);
                ++i;
                if (i == ar.second) // fatal, but ignore
                    break;
                uint32_t vlast = ar.first[i];

                SeqRange r;
                r.set_including(val, vlast);

                // Add the sequence difference
                nloss += r.size;
                m_UnackLossRange.push_back(r);
                HLOGC(mglog.Debug, log << "FlowSmoother: ... LOSS [" << r.begin << " - " << vlast
                        << "] (" << r.size << ")");
            }
            else
            {
                SeqRange r;
                r.set_one(val);
                ++nloss;
                m_UnackLossRange.push_back(r);
                HLOGC(mglog.Debug, log << "FlowSmoother: ... LOSS [" << r.begin << "]");
            }
        }

        return nloss;
    }

    // This function extracts the loss information from the given array.
    // Any already present loss information will be ignored.
    // All new loss information is additionally collected and returned.
    size_t collectLossAndAck(TevSeqArray ar)
    {
        // Check the current state.
        int32_t first_loss = SEQNO_VALUE::unwrap(ar.first[0]);

        // Check first if this is already collected.
        // Enough to check the first one.
        for (size_t i = 0; i < m_UnackLossRange.size(); ++i)
        {
            if (first_loss == m_UnackLossRange[i].begin)
                return 0; // This is a NAKREPORT
        }

        // Stating this is not a NAKREPORT, we have a guarantee that
        // the loss sequence follows the last one, or it's the first one.

        // Collect first the range between the last reported loss
        // and this one.

        // This variable holds the sequence number of the first packet
        // following the acknowledged one:
        int last_unseen;
        if (m_UnackLossRange.empty())
        {
            // If no loss, this is the ACK sequence, that is, the sequence
            // following the last contiguous packet.
            last_unseen = m_LastAckSeq;

        }
        else
        {
            // Example:
            // One loss reported before:

            //ACK |      prev-loss   now-loss
            //    @ | | | . . . | | | . . |
            //
            // This means that we should have:
            // - packets A ... A+3 recorded in m_UnackAckRange (we don't check it)
            // - packets A+4 ... A+6 recorded in m_UnackLossRange
            // Therefore:
            // - We take the LAST record in the m_UnackLossRange.
            //   the .end field should == A+7
            // - The new loss starts with A+10, so
            //   the new entry in m_UnackAckRange: A+7 (last-loss.end) ... A+10 (new loss)
            // - The value for last_ack = A+4

            // prev-loss.end
            last_unseen = m_UnackLossRange.back().end;
        }

        SeqRange r;
        r.set_excluding(last_unseen, first_loss);
        m_UnackAckRange.push_back(r);

        // And add the loss range.
        // If this happened to be a NAKREPORT sent after a lost LOSSREPORT,
        // ignore packets that are received, but happen to be between
        // various lost packets.
        HLOGC(mglog.Debug, log << "FlowSmoother: collecting loss: ACK [" << last_unseen << " - "
                << CSeqNo::decseq(first_loss) << "] (" << CSeqNo::seqoff(last_unseen, first_loss) << ") ...");
        return addLossRanges(ar);
    }

    void calculateLossData(ref_t<double> loss_rate,
            ref_t<int> number_loss,
            ref_t<int> number_acked,
            int32_t acksplit = -1)
    {
        // This collects all the lengths of the period that is yet
        // about to be ACK-ed, but lossreport has already collected
        // some fragmentary information
        if (m_UnackLossRange.empty())
        {
            *number_loss = 0;
            *number_acked = 0;
            *loss_rate = 0;
            return;
        }

        // Calculate the size of all loss ranges
        int lossno = 0;
        int ackno = 0;

        // Cursor indices in the arrays, used for shift
        // for acksplit
        size_t xa = 0, xl = 0;
        int ackbase = 0;

        if (acksplit != -1)
        {
            // Shift the pointer to point to the first after acksplit
            for (; xa != m_UnackAckRange.size(); ++xa)
                if (CSeqNo::seqcmp(m_UnackAckRange[xa].begin, m_LastAckSeq) > 0)
                    break;

            for (; xl != m_UnackLossRange.size(); ++xl)
                if (CSeqNo::seqcmp(m_UnackLossRange[xl].begin, m_LastAckSeq) > 0)
                    break;

            // Collect only since given sequence number,
            // all previous treat as contiguous.
            ackbase = CSeqNo::seqlen(acksplit, m_LastAckSeq);

            HLOGC(mglog.Debug, log << "FlowSmoother: LOSS DATA COLLECTION SINCE "
                    << m_LastAckSeq << " (prev " << acksplit << "): LOSS-ACKed: "
                    << m_UnackAckRange.size() << " LOST: "
                    << m_UnackLossRange.size());

        }
        else
        {
            HLOGC(mglog.Debug, log << "FlowSmoother: LOSS DATA COLLECTION: LOSS-ACKed: "
                    << m_UnackAckRange.size() << " LOST: "
                    << m_UnackLossRange.size());
        }

        // Extract all collected data received so far
        for (; xa < m_UnackAckRange.size(); ++xa)
        {
            HLOGC(mglog.Debug, log << "... ACK: " << m_UnackAckRange[xa].begin << " - " << m_UnackAckRange[xa].end);
            ackno += m_UnackAckRange[xa].size;
        }
        for (; xl < m_UnackLossRange.size(); ++xl)
        {
            HLOGC(mglog.Debug, log << "...LOSS: " << m_UnackLossRange[xl].begin << " - " << m_UnackLossRange[xl].end);
            lossno += m_UnackLossRange[xl].size;
        }

        *number_loss = lossno;
        *number_acked = ackbase + ackno;
        *loss_rate = double(lossno)/double(ackbase + ackno + lossno);
        HLOGC(mglog.Debug, log << "FlowSmoother: Total: ACK=" << ackno << " + " << ackbase << " LOSS=" << lossno << " LOSS RATE: "
                << (*loss_rate));
    }

    // When a lossreport has been received, it might be due to having
    // reached the available bandwidth limit. Slowdown to avoid further losses.
    void onLOSS_CollectLoss(ETransmissionEvent, TevSeqArray arg)
    {
        const int32_t* losslist = arg.first;
        size_t losslist_size = arg.second;

        // Sanity check. Should be impossible that TEV_LOSSREPORT event
        // is called with a nonempty loss list.
        if ( losslist_size == 0 )
        {
            LOGC(mglog.Error, log << "IPE: FlowSmoother: empty loss list!");
            return;
        }

        m_bLoss = true;
        collectLossAndAck(arg);
        double loss_rate;
        int number_loss, number_acked;
        calculateLossData(Ref(loss_rate), Ref(number_loss), Ref(number_acked));

        /* XXX Temporary experimental: don't use emergency break yet, we react on
           the loss like UDT did before.

        // Make an average towards the current loss rate ( AVG(cur, cur, cur, loss_rate) )
        if (avg_iir<3>(m_dLossRate, loss_rate) > 1)
        {
            // Loss rate over 50%, emergency brake
            emergencyBrake();
            return;
        }
        */

        FlowState prevstate = m_State.state;
        double avg_loss_rate = avg_iir<4>(m_dLossRate, loss_rate);

        if (m_State.state == FS_WARMUP)
        {
            // Enter the keep state with probe size == 4.
            // Stop when probe size reached or when loss rate exceeds 50%.

            // XXX Temporary experimental: turn on the "old measurement method from UDT"
            analyzeSpeed();
            reachCWNDTop("LOSS");
        }

        if (m_State.state != FS_SLIDE)
        {
            HLOGC(mglog.Debug, log << "FlowSmoother: LOSS, state {" << DisplayState(m_State) << "} - NOT decreasing speed YET.");
        }

        if (m_dPktSndPeriod_us > 5) // less than 5 is "ridiculously high", so disregard it
        {
            m_dPktSndPeriod_HI_us = m_dPktSndPeriod_us;
        }

        // Needed for old algo marked by FS_BITE
        int lossbegin = SEQNO_VALUE::unwrap(losslist[0]);

        // Do not undertake any action on the loss. This will be collected
        // and a decision taken later after statistics are collected.
        string decision = "WARMUP";
        if (m_State.state == FS_BITE)
        {
            //*  Old code from UDT, slowdown basing on some calculations and random conditions.

            // In contradiction to UDT, TEV_LOSSREPORT will be reported also when
            // the lossreport is being sent again, periodically, as a result of
            // NAKREPORT feature. You should make sure that NAKREPORT is off when
            // using FlowSmoother, so relying on SRTO_TRANSTYPE rather than
            // just SRTO_SMOOTHER is recommended.

            int loss_span = CSeqNo::seqcmp(lossbegin, m_iLastRCTimeSndSeq);

            double slowdown_factor = 1.125;
            /*
            if (prevstate == FS_WARMUP)
            {
                HLOGC(mglog.Debug, log << "FlowSmoother: exitting WARMUP, lossrate=" << (100*avg_loss_rate) << "%");
            }
            else
            {
                if (avg_loss_rate/2 < 0.125)
                {
                    slowdown_factor = 1 + avg_loss_rate/2;
                }
                HLOGC(mglog.Debug, log << "FlowSmoother: still BITE, lossrate="
                        << setprecision(6) << fixed
                        << (100*avg_loss_rate) << "%"
                        << " slowdown by " << (100*(slowdown_factor-1)) << "%" );
            }
            */

            if (loss_span > 0)
            {
                m_dLastDecPeriod = m_dPktSndPeriod_us;
                m_dPktSndPeriod_us = ceil(m_dPktSndPeriod_us * slowdown_factor);

                m_iAvgNAKNum = (int)ceil(m_iAvgNAKNum * 0.875 + m_iNAKCount * 0.125);
                m_iNAKCount = 1;
                m_iDecCount = 1;

                m_iLastRCTimeSndSeq = m_parent->sndSeqNo();

                // remove global synchronization using randomization
                srand(m_iLastRCTimeSndSeq);
                m_iDecRandom = (int)ceil(m_iAvgNAKNum * (double(rand()) / RAND_MAX));
                if (m_iDecRandom < 1)
                    m_iDecRandom = 1;
                HLOGC(mglog.Debug, log << "FlowSmoother: LOSS:NEW: rand=" << m_iDecRandom
                        << " avg NAK:" << m_iAvgNAKNum
                        << ", sndperiod=" << m_dPktSndPeriod_us << "us");
                decision = "SLOW-OVER";
            }
            else if ((m_iDecCount ++ < 5) && (0 == (++ m_iNAKCount % m_iDecRandom)))
            {
                // 0.875^5 = 0.51, rate should not be decreased by more than half within a congestion period
                m_dPktSndPeriod_us = ceil(m_dPktSndPeriod_us * slowdown_factor);
                m_iLastRCTimeSndSeq = m_parent->sndSeqNo();
                HLOGC(mglog.Debug, log << "FlowSmoother: LOSS:PERIOD: loss_span=" << loss_span
                        << ", decrnd=" << m_iDecRandom
                        << ", sndperiod=" << m_dPktSndPeriod_us << "us");
                decision = "SLOW-STILL";
            }
            else
            {
                HLOGC(mglog.Debug, log << "FlowSmoother: LOSS:STILL: loss_span=" << loss_span
                        << ", decrnd=" << m_iDecRandom
                        << ", sndperiod=" << m_dPktSndPeriod_us << "us");
                decision = "KEEP-SPEED";
            }
            // */
        }

        int32_t last_contig = CSeqNo::decseq(lossbegin);
        int32_t farthest_ack = CSeqNo::incseq(m_parent->sndSeqNo());
        int flight_span = CSeqNo::seqlen(last_contig, farthest_ack) - 1;

        int32_t last_rcv = (!m_UnackLossRange.empty())
            // If there is a loss, take the last loss and increase it.
            // This is the seq received that has triggered lossreport
            ? CSeqNo::incseq(m_UnackLossRange.back().end)
            // Otherwise place the last contiguous as a fallback.
            // Rather impossible because it would mean that a loss
            // report didn't report any loss :)
            : last_contig;

        HLOGC(mglog.Debug, log << "FlowSmoother: LOSS STATS: {" << DisplayState(m_State) << "}"
                << " Snd: Period:" << m_dPktSndPeriod_us << "us"
                << " sRTT:(last)=" << m_LastSenderRTT << "(" << m_SenderRTT_us << ")"
                << "us Speed=" << m_SenderSpeed_pps << "p/s"
                << " ACKspan:[" << m_LastAckSeq << "-"
                    << last_contig << ">" << CSeqNo::seqcmp(last_rcv, last_contig)
                    << "](0)" // Keep consistent format, but LOSS event could not increase ACK size here.
                << " Flight:" << flight_span << "p"
                << " CWND:" << setprecision(6) << std::fixed << m_dCongestionWindow
                << " RcvVelocity=" << m_LastRcvVelocity << "p/s RcvSpeed=" << m_LastRcvSpeed << "(" << m_parent->deliveryRate() << ")"
                << "p/s" << " pRTT=" << m_parent->RTT() << "us BW=" << m_parent->bandwidth() << "p/s"
                << " LOSS n=" << number_loss
                << setprecision(6) << std::fixed
                << "p rate:" << loss_rate << "("
                << m_dLossRate << ")p/ack, freq:"
                << int(1000.0*m_dTimedLossRate) << "p/ms "
                << "[" << decision << "]"
                );

    }

    void onTIMER(ETransmissionEvent, ECheckTimerStage stg)
    {
        // TEV_INIT is in the beginning of checkTimers(), used
        // only to synchronize back the values (which is done in updateCC
        // after emitting the signal).
        if (stg == TEV_CHT_INIT)
            return;

        HLOGC(mglog.Debug, log << "FlowSmoother: TIMER EVENT. State: " << DisplayState(m_State));

        ++m_zTimerCounter;

        // XXX Not sure about that, looxlike it will exit the WARMUP
        // before even reaching the top window size, or whichever comes first.
        if (m_State.state == FS_WARMUP)
        {
            // This is the very first update of sender speed.
            // Next sender speed update will be done at ACK events.
            if ( !m_parent->updateSenderSpeed(Ref(m_SenderSpeed_pps), Ref(m_SenderSpeed_bps)))
            {
                // Too early
                m_SenderSpeed_bps = 0;
                m_SenderSpeed_pps = 0;
            }
            else
            {
                // This is the last moment when the maximum sender speed can be notified
                // because the packets are being sent with highest possible speed. If there's
                // any slowdown applied to the sender speed, the sender speed will be slower than
                // the maximum possible anyway.
                m_MaxSenderSpeed_pps = m_SenderSpeed_pps;
            }
            reachCWNDTop("TIMER");
            return;
        }

        if (m_zTimerCounter > 32)
        {
            HLOGC(mglog.Warn, log << "32 CHECKTIMER events without ACK - resetting stats!");
            m_zTimerCounter = 0;

            measureSenderSpeed();
        }
    }

    Smoother::RexmitMethod rexmitMethod() ATR_OVERRIDE
    {
        return Smoother::SRM_LATEREXMIT;
    }
};


