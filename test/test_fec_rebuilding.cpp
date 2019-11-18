#include <vector>

#include "gtest/gtest.h"
#include "packet.h"
#include "fec.h"
#include "packetfilter.h"
#include "packetfilter_api.h"

using namespace std;

class TestFECRebuilding: public testing::Test
{
protected:
    FECFilterBuiltin* fec = nullptr;
    vector<SrtPacket> provided;
    vector<unique_ptr<CPacket>> source;
    int sockid = 54321;
    int isn = 123456;
    size_t plsize = 1316;

    TestFECRebuilding()
    {
        // Required to make ParseCorrectorConfig work
        PacketFilter::globalInit();
    }

    void SetUp() override
    {
        int timestamp = 10;

        SrtFilterInitializer init = {
            sockid,
            isn - 1, // It's passed in this form to PacketFilter constructor, it should increase it
            isn - 1, // XXX Probably this better be changed.
            plsize
        };


        // Make configuration row-only with size 7
        string conf = "fec,rows:1,cols:7";

        provided.clear();

        fec = new FECFilterBuiltin(init, provided, conf);

        int32_t seq = isn;

        for (int i = 0; i < 7; ++i)
        {
            source.emplace_back(new CPacket);
            CPacket& p = *source.back();

            p.allocate(SRT_LIVE_MAX_PLSIZE);

            uint32_t* hdr = p.getHeader();

            // Fill in the values
            hdr[SRT_PH_SEQNO] = seq;
            hdr[SRT_PH_MSGNO] = 1 | MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);
            hdr[SRT_PH_ID] = sockid;
            hdr[SRT_PH_TIMESTAMP] = timestamp;

            // Fill in the contents.
            // Randomly chose the size

            int minsize = 732;
            int divergence = plsize - minsize - 1;
            size_t length = minsize + rand() % divergence;

            p.setLength(length);
            for (size_t b = 0; b < length; ++b)
            {
                p.data()[b] = rand() % 255;
            }

            timestamp += 10;
            seq = CSeqNo::incseq(seq);
        }
    }

    void TearDown() override
    {
        delete fec;
    }
};


TEST_F(TestFECRebuilding, Prepare)
{
    // Stuff in prepared packets into the source fec.
    int32_t seq;
    for (int i = 0; i < 7; ++i)
    {
        CPacket& p = *source[i].get();

        // Feed it simultaneously into the sender FEC
        fec->feedSource(p);
        seq = p.getSeqNo();
    }

    SrtPacket fec_ctl(SRT_LIVE_MAX_PLSIZE);

    // Use the sequence number of the last packet, as usual.
    bool have_fec_ctl = fec->packControlPacket(fec_ctl, seq);

    EXPECT_EQ(have_fec_ctl, true);
}

TEST_F(TestFECRebuilding, NoRebuild)
{
    // Stuff in prepared packets into the source fec.
    int32_t seq;
    for (int i = 0; i < 7; ++i)
    {
        CPacket& p = *source[i].get();

        // Feed it simultaneously into the sender FEC
        fec->feedSource(p);
        seq = p.getSeqNo();
    }

    SrtPacket fec_ctl(SRT_LIVE_MAX_PLSIZE);

    // Use the sequence number of the last packet, as usual.
    bool have_fec_ctl = fec->packControlPacket(fec_ctl, seq);

    ASSERT_EQ(have_fec_ctl, true);
    // By having all packets and FEC CTL packet, now stuff in
    // these packets into the receiver

    FECFilterBuiltin::loss_seqs_t loss; // required as return, ignore

    for (int i = 0; i < 7; ++i)
    {
        // SKIP packet 4 to simulate loss
        if (i == 4 || i == 6)
            continue;

        // Stuff in the packet into the FEC filter
        bool want_passthru = fec->receive(*source[i], loss);
        EXPECT_EQ(want_passthru, true);
    }

    // Prepare a real packet basing on the SrtPacket.

    // XXX Consider packing this into a callable function as this
    // is a code directly copied from PacketFilter::packControlPacket.

    unique_ptr<CPacket> fecpkt ( new CPacket );

    uint32_t* chdr = fecpkt->getHeader();
    memcpy(chdr, fec_ctl.hdr, SRT_PH__SIZE * sizeof(*chdr));

    // The buffer can be assigned.
    fecpkt->m_pcData = fec_ctl.buffer;
    fecpkt->setLength(fec_ctl.length);

    // This sets only the Packet Boundary flags, while all other things:
    // - Order
    // - Rexmit
    // - Crypto
    // - Message Number
    // will be set to 0/false
    fecpkt->m_iMsgNo = MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);

    // ... and then fix only the Crypto flags
    fecpkt->setMsgCryptoFlags(EncryptionKeySpec(0));

    // And now receive the FEC control packet

    bool want_passthru_fec = fec->receive(*fecpkt, loss);
    EXPECT_EQ(want_passthru_fec, false); // Confirm that it's been eaten up
    EXPECT_EQ(provided.size(), 0); // Confirm that nothing was rebuilt

    /*
    // XXX With such a short sequence, losses will not be reported.
    // You need at least one packet past the row, even in 1-row config.
    // Probably a better way for loss collection should be devised.

    ASSERT_EQ(loss.size(), 2);
    EXPECT_EQ(loss[0].first, isn + 4);
    EXPECT_EQ(loss[1].first, isn + 6);
     */
}

TEST_F(TestFECRebuilding, Rebuild)
{
    // Stuff in prepared packets into the source fec->
    int32_t seq;
    for (int i = 0; i < 7; ++i)
    {
        CPacket& p = *source[i].get();

        // Feed it simultaneously into the sender FEC
        fec->feedSource(p);
        seq = p.getSeqNo();
    }

    SrtPacket fec_ctl(SRT_LIVE_MAX_PLSIZE);

    // Use the sequence number of the last packet, as usual.
    bool have_fec_ctl = fec->packControlPacket(fec_ctl, seq);

    ASSERT_EQ(have_fec_ctl, true);
    // By having all packets and FEC CTL packet, now stuff in
    // these packets into the receiver

    FECFilterBuiltin::loss_seqs_t loss; // required as return, ignore

    for (int i = 0; i < 7; ++i)
    {
        // SKIP packet 4 to simulate loss
        if (i == 4)
            continue;

        // Stuff in the packet into the FEC filter
        bool want_passthru = fec->receive(*source[i], loss);
        EXPECT_EQ(want_passthru, true);
    }

    // Prepare a real packet basing on the SrtPacket.

    // XXX Consider packing this into a callable function as this
    // is a code directly copied from PacketFilter::packControlPacket.

    unique_ptr<CPacket> fecpkt ( new CPacket );

    uint32_t* chdr = fecpkt->getHeader();
    memcpy(chdr, fec_ctl.hdr, SRT_PH__SIZE * sizeof(*chdr));

    // The buffer can be assigned.
    fecpkt->m_pcData = fec_ctl.buffer;
    fecpkt->setLength(fec_ctl.length);

    // This sets only the Packet Boundary flags, while all other things:
    // - Order
    // - Rexmit
    // - Crypto
    // - Message Number
    // will be set to 0/false
    fecpkt->m_iMsgNo = MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);

    // ... and then fix only the Crypto flags
    fecpkt->setMsgCryptoFlags(EncryptionKeySpec(0));

    // And now receive the FEC control packet

    bool want_passthru_fec = fec->receive(*fecpkt, loss);
    EXPECT_EQ(want_passthru_fec, false); // Confirm that it's been eaten up

    EXPECT_EQ(loss.size(), 0);
    ASSERT_EQ(provided.size(), 1);

    SrtPacket& rebuilt = provided[0];
    CPacket& skipped = *source[4];

    // Set artificially the SN_REXMIT flag in the skipped source packet
    // because the rebuilt packet shall have REXMIT flag set.
    skipped.m_iMsgNo |= MSGNO_REXMIT::wrap(true);

    // Compare the header
    EXPECT_EQ(skipped.getHeader()[SRT_PH_SEQNO], rebuilt.hdr[SRT_PH_SEQNO]);
    EXPECT_EQ(skipped.getHeader()[SRT_PH_MSGNO], rebuilt.hdr[SRT_PH_MSGNO]);
    EXPECT_EQ(skipped.getHeader()[SRT_PH_ID], rebuilt.hdr[SRT_PH_ID]);
    EXPECT_EQ(skipped.getHeader()[SRT_PH_TIMESTAMP], rebuilt.hdr[SRT_PH_TIMESTAMP]);

    // Compare sizes and contents
    ASSERT_EQ(skipped.size(), rebuilt.size());

    EXPECT_EQ(memcmp(skipped.data(), rebuilt.data(), rebuilt.size()), 0);
}

#if ENABLE_DEVEL_API

class TestFECRecovery: public testing::Test
{
protected:
    FECFilterBuiltin* fec = nullptr;
    vector<SrtPacket> provided;
    vector<unique_ptr<CPacket>> source;
    int sockid = 54321;
    int isn = 123456;
    size_t plsize = 1316;
    string fec_conf = "fec,rows:20,cols:20,layout:even";

    // A spare container where FEC control packets will be stored
    vector<unique_ptr<CPacket>> fecpkts;

    // A container that will contain packets as prepared for sending.
    // Contains pointers due to required ordering, while packets
    // will be taken in their respective containers.
    vector<CPacket*> sendqueue;
    static const int NUMBER_PACKETS = 20000;

    TestFECRecovery()
    {
        // Required to make ParseCorrectorConfig work
        PacketFilter::globalInit();
    }

    void SetUp() override
    {
        srt_setloglevel(LOG_DEBUG);
        int timestamp = 10;

        SrtFilterInitializer init = {
            sockid,
            isn - 1, // It's passed in this form to PacketFilter constructor, it should increase it
            isn - 1, // XXX Probably this better be changed.
            plsize
        };

        provided.clear();

        fec = new FECFilterBuiltin(init, provided, fec_conf);

        int32_t seq = isn;

        cout << "---- TEST: Preparing " << NUMBER_PACKETS << " packets with random data\n";

        for (int i = 0; i < NUMBER_PACKETS; ++i)
        {
            source.emplace_back(new CPacket);
            CPacket& p = *source.back();

            p.allocate(SRT_LIVE_MAX_PLSIZE);

            uint32_t* hdr = p.getHeader();

            // Fill in the values
            hdr[SRT_PH_SEQNO] = seq;
            hdr[SRT_PH_MSGNO] = (i+1) | MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);
            hdr[SRT_PH_ID] = sockid;
            hdr[SRT_PH_TIMESTAMP] = timestamp;

            // Fill in the contents.
            // Randomly chose the size

            int minsize = 732;
            int divergence = plsize - minsize - 1;
            size_t length = minsize + rand() % divergence;

            p.setLength(length);
            for (size_t b = 0; b < length; ++b)
            {
                p.data()[b] = rand() % 255;
            }

            timestamp += 10;
            seq = CSeqNo::incseq(seq);
        }

        // Temporary storage for a newly extracted FEC control packet.
        SrtPacket fec_ctl(SRT_LIVE_MAX_PLSIZE);

        cout << " ----- TEST: Stuffing in data onto the filter feed\n";

        int nfec = 0;
        int32_t nseq = -1;
        // Stuff in prepared packets into the source fec.
        for (size_t i = 0; i < source.size(); ++i)
        {
            CPacket& p = *source[i].get();

            if (nseq == -1)
                nseq = p.getSeqNo();
            else if (p.getMsgSeq() != 0)
            {
                int seqd = CSeqNo::seqoff(nseq, p.getSeqNo());

                if (seqd % 20 == 0)
                    cout << endl;
            }

            cout << "=%" << p.getSeqNo() << "!" << BufferStamp(p.m_pcData, p.getLength()) << " ";

            // Feed it into the sender FEC
            fec->feedSource(p);
            seq = p.getSeqNo();

            // Now store it into the alleged "sender queue"
            sendqueue.push_back(&p);

            // Use the sequence number of the last packet, as usual.
            for (;;)
            {
                bool have_fec_ctl = fec->packControlPacket(fec_ctl, seq);
                if (!have_fec_ctl)
                    break;

                // Prepare a real packet basing on the SrtPacket.

                // XXX Consider packing this into a callable function as this
                // is a code directly copied from PacketFilter::packControlPacket.
                fecpkts.emplace_back(new CPacket);
                CPacket* pktview = fecpkts.back().get();

                unique_ptr<CPacket> fecpkt ( new CPacket );

                uint32_t* chdr = pktview->getHeader();
                memcpy(chdr, fec_ctl.hdr, SRT_PH__SIZE * sizeof(*chdr));

                // This sets only the Packet Boundary flags, while all other things:
                // - Order
                // - Rexmit
                // - Crypto
                // - Message Number
                // will be set to 0/false
                pktview->m_iMsgNo = MSGNO_PACKET_BOUNDARY::wrap(PB_SOLO);

                // ... and then fix only the Crypto flags
                pktview->setMsgCryptoFlags(EncryptionKeySpec(0));

                // Allocate buffer for the packet and copy the contents
                pktview->allocate(SRT_LIVE_MAX_PLSIZE);
                memcpy(pktview->m_pcData, fec_ctl.buffer, fec_ctl.length);
                pktview->setLength(fec_ctl.length);

                cout << "[%" << pktview->getSeqNo() << "!" << BufferStamp(pktview->m_pcData, pktview->getLength())
                    << "(" << int(fec_ctl.data()[0]) << ")] ";

                sendqueue.push_back(pktview);
                ++nfec;
            }
        }

        cout << " ----- TEST: Total of " << sendqueue.size() << "packets, including " << nfec << " FEC control packets\n";

        delete fec;
        fec = nullptr;
    }

    void TearDown() override
    {
    }
};

// Bigger sizes of drop, predicted the following way:
//
// 1. Small: fits in the size of one matrix.
// 2. Medium: just slightly more than one matrix of packets dropped
// 3. Large: at least 10 x matrix packets dropped
//
// For 20x20 configuration, it would be matrix size 400, so:
// 1. Small: drop 380 packets.
// 2. Medium: drop 410 packets.
// 3. Large: drop 4000 packets.

TEST_F(TestFECRecovery, Small)
{
    SrtFilterInitializer init = {
        sockid,
        isn - 1, // It's passed in this form to PacketFilter constructor, it should increase it
        isn - 1, // XXX Probably this better be changed.
        plsize
    };

    fec = new FECFilterBuiltin(init, provided, fec_conf);

    FECFilterBuiltin::loss_seqs_t loss; // required as return, ignore

    size_t received_count = 0;
    size_t skipped_count = 0;
    const size_t LOSS_TOTAL = 380;
    bool in_loss = false;
    size_t loss_count = 0;
    size_t loss_fec_count = 0;

    cout << " ----- TEST: RECEIVING packets\n";

    for (size_t i = 0; i < sendqueue.size(); ++i)
    {
        CPacket* pkt = sendqueue[i];
        // We have 20'000 packets; allow at least 2* the
        // matrix size to pass through, then make a drop.
        if (in_loss)
        {
            if (pkt->getMsgSeq() != 0)
                ++loss_count;
            else
                ++loss_fec_count;

            cout << "*%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
            if (i % 10 == 0)
                cout << endl;
            if (loss_count >= LOSS_TOTAL)
            {
                cout << " ---- TEST: END DROP, continue sending after " << pkt->getSeqNo() << "\n";
                in_loss = false;
            }
            continue;
        }

        if (pkt->getMsgSeq())
            cout << "." << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
        else
            cout << "/" << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength())
                << "(" << int(pkt->m_pcData[0]) << ") ";

        if (i % 10 == 0)
            cout << endl;

        if (i == 1000)
        {
            cout << "\n ---- TEST: DROPPING 380 packets since: " << pkt->getSeqNo() << "\n";
            in_loss = true;
        }

        // Stuff in the packet into the FEC filter
        if (fec->receive(*pkt, loss))
            ++received_count;
        else
            ++skipped_count;
    }

    cout << "\n ----- TEST: received=" << received_count << " skipped=" << skipped_count
        << " lost {data=" << loss_count << " fec=" << loss_fec_count << "}\n";

    ASSERT_EQ(received_count + loss_count, source.size());
    ASSERT_EQ(skipped_count + loss_fec_count, fecpkts.size());

    delete fec;
}

TEST_F(TestFECRecovery, Medium)
{
    SrtFilterInitializer init = {
        sockid,
        isn - 1, // It's passed in this form to PacketFilter constructor, it should increase it
        isn - 1, // XXX Probably this better be changed.
        plsize
    };

    fec = new FECFilterBuiltin(init, provided, fec_conf);

    FECFilterBuiltin::loss_seqs_t loss; // required as return, ignore

    size_t received_count = 0;
    size_t skipped_count = 0;
    const size_t LOSS_TOTAL = 480;
    bool in_loss = false;
    size_t loss_count = 0;
    size_t loss_fec_count = 0;

    cout << " ----- TEST: RECEIVING packets\n";

    for (size_t i = 0; i < sendqueue.size(); ++i)
    {
        CPacket* pkt = sendqueue[i];
        // We have 20'000 packets; allow at least 2* the
        // matrix size to pass through, then make a drop.
        if (in_loss)
        {
            if (pkt->getMsgSeq() != 0)
                ++loss_count;
            else
                ++loss_fec_count;

            cout << "*%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
            if (i % 10 == 0)
                cout << endl;
            if (loss_count >= LOSS_TOTAL)
            {
                cout << " ---- TEST: END DROP, continue sending after " << pkt->getSeqNo() << "\n";
                in_loss = false;
            }
            continue;
        }

        if (pkt->getMsgSeq())
            cout << "." << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
        else
            cout << "/" << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength())
                << "(" << int(pkt->m_pcData[0]) << ") ";

        if (i % 10 == 0)
            cout << endl;

        if (i == 1000)
        {
            cout << "\n ---- TEST: DROPPING 380 packets since: " << pkt->getSeqNo() << "\n";
            in_loss = true;
        }

        // Stuff in the packet into the FEC filter
        if (fec->receive(*pkt, loss))
            ++received_count;
        else
            ++skipped_count;
    }

    cout << "\n ----- TEST: received=" << received_count << " skipped=" << skipped_count
        << " lost {data=" << loss_count << " fec=" << loss_fec_count << "}\n";

    ASSERT_EQ(received_count + loss_count, source.size());
    ASSERT_EQ(skipped_count + loss_fec_count, fecpkts.size());

    delete fec;
}


TEST_F(TestFECRecovery, Large)
{
    SrtFilterInitializer init = {
        sockid,
        isn - 1, // It's passed in this form to PacketFilter constructor, it should increase it
        isn - 1, // XXX Probably this better be changed.
        plsize
    };

    fec = new FECFilterBuiltin(init, provided, fec_conf);

    FECFilterBuiltin::loss_seqs_t loss; // required as return, ignore

    size_t received_count = 0;
    size_t skipped_count = 0;
    const size_t LOSS_TOTAL = 13*400+282;
    bool in_loss = false;
    size_t loss_count = 0;
    size_t loss_fec_count = 0;

    cout << " ----- TEST: RECEIVING packets\n";

    for (size_t i = 0; i < sendqueue.size(); ++i)
    {
        CPacket* pkt = sendqueue[i];
        // We have 20'000 packets; allow at least 2* the
        // matrix size to pass through, then make a drop.
        if (in_loss)
        {
            if (pkt->getMsgSeq() != 0)
                ++loss_count;
            else
                ++loss_fec_count;

            cout << "*%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
            if (i % 10 == 0)
                cout << endl;
            if (loss_count >= LOSS_TOTAL)
            {
                cout << " ---- TEST: END DROP, continue sending after " << pkt->getSeqNo() << "\n";
                in_loss = false;
            }
            continue;
        }

        if (pkt->getMsgSeq())
            cout << "." << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength()) << " ";
        else
            cout << "/" << "%" << pkt->getSeqNo() << "!" << BufferStamp(pkt->m_pcData, pkt->getLength())
                << "(" << int(pkt->m_pcData[0]) << ") ";

        if (i % 10 == 0)
            cout << endl;

        if (i == 1000)
        {
            cout << "\n ---- TEST: DROPPING 380 packets since: " << pkt->getSeqNo() << "\n";
            in_loss = true;
        }

        // Stuff in the packet into the FEC filter
        if (fec->receive(*pkt, loss))
            ++received_count;
        else
            ++skipped_count;
    }

    cout << "\n ----- TEST: received=" << received_count << " skipped=" << skipped_count
        << " lost {data=" << loss_count << " fec=" << loss_fec_count << "}\n";

    ASSERT_EQ(received_count + loss_count, source.size());
    ASSERT_EQ(skipped_count + loss_fec_count, fecpkts.size());

    delete fec;
}

#endif
