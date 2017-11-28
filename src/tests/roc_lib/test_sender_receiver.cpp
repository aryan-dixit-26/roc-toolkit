/*
 * Copyright (c) 2015 Mikhail Baranov
 * Copyright (c) 2015 Victor Gaydov
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <CppUTest/TestHarness.h>

#include <stdio.h>
#include <unistd.h>

#include "roc_core/buffer_pool.h"
#include "roc_core/heap_allocator.h"
#include "roc_core/log.h"
#include "roc_core/random.h"
#include "roc_core/stddefs.h"
#include "roc_core/thread.h"
#include "roc_netio/transceiver.h"
#include "roc_packet/address_to_str.h"
#include "roc_packet/packet_pool.h"
#include "roc_packet/parse_address.h"

#include "roc/receiver.h"
#include "roc/sender.h"

namespace roc {

namespace {

enum {
    MaxBufSize = 4096,

    NumChans = 2,

    SourcePackets = 10,
    RepairPackets = 5,

    NumPackets = SourcePackets * 5,

    PacketSamples = 100,
    FrameSamples = PacketSamples * 2,
    TotalSamples = PacketSamples * NumPackets
};

core::HeapAllocator allocator;
packet::PacketPool packet_pool(allocator, 1);
core::BufferPool<uint8_t> byte_buffer_pool(allocator, MaxBufSize, 1);

class Sender : public core::Thread {
public:
    Sender(roc_sender_config& config,
           packet::Address dst_source_addr,
           packet::Address dst_repair_addr,
           float* samples,
           size_t len,
           size_t frame_size)
        : samples_(samples)
        , sz_(len)
        , frame_size_(frame_size) {
        packet::Address addr;
        CHECK(packet::parse_address("127.0.0.1:0", addr));
        sndr_ = roc_sender_new(&config);
        CHECK(sndr_);
        CHECK(roc_sender_bind(sndr_, addr.saddr()) == 0);
        CHECK(
            roc_sender_connect(sndr_, ROC_PROTO_RTP_RSM8_SOURCE, dst_source_addr.saddr())
            == 0);
        CHECK(roc_sender_connect(sndr_, ROC_PROTO_RSM8_REPAIR, dst_repair_addr.saddr())
              == 0);
        CHECK(roc_sender_start(sndr_) == 0);
    }

    ~Sender() {
        roc_sender_stop(sndr_);
        roc_sender_delete(sndr_);
    }

private:
    virtual void run() {
        for (size_t off = 0; off < sz_; off += frame_size_) {
            if (off + frame_size_ > sz_) {
                off = sz_ - frame_size_;
            }
            const ssize_t ret = roc_sender_write(sndr_, samples_ + off, frame_size_);
            LONGS_EQUAL(frame_size_, ret);
        }
    }

    roc_sender* sndr_;
    float* samples_;
    const size_t sz_;
    const size_t frame_size_;
};

class Receiver {
public:
    Receiver(roc_receiver_config& config,
             const float* samples,
             size_t len,
             size_t frame_size)
        : samples_(samples)
        , sz_(len)
        , frame_size_(frame_size) {
        CHECK(packet::parse_address("127.0.0.1:0", source_addr_));
        CHECK(packet::parse_address("127.0.0.1:0", repair_addr_));
        recv_ = roc_receiver_new(&config);
        CHECK(recv_);
        CHECK(roc_receiver_bind(recv_, ROC_PROTO_RTP_RSM8_SOURCE, source_addr_.saddr())
              == 0);
        CHECK(roc_receiver_bind(recv_, ROC_PROTO_RSM8_REPAIR, repair_addr_.saddr()) == 0);
        CHECK(roc_receiver_start(recv_) == 0);
    }

    ~Receiver() {
        roc_receiver_stop(recv_);
        roc_receiver_delete(recv_);
    }

    packet::Address source_addr() {
        return source_addr_;
    }

    packet::Address repair_addr() {
        return repair_addr_;
    }

    void run() {
        float rx_buff[MaxBufSize];
        size_t s_first = 0;
        size_t inner_cntr = 0;
        bool seek_first = true;
        size_t s_last = 0;

        size_t ipacket = 0;
        while (s_last == 0) {
            size_t i = 0;
            ipacket++;
            LONGS_EQUAL(frame_size_, roc_receiver_read(recv_, rx_buff, frame_size_));
            if (seek_first) {
                for (; i < frame_size_ && is_zero_(rx_buff[i]); i++, s_first++) {
                }
                CHECK(s_first < sz_);
                if (i < frame_size_) {
                    seek_first = false;
                }
            }
            if (!seek_first) {
                for (; i < frame_size_; i++) {
                    if (inner_cntr >= sz_) {
                        CHECK(is_zero_(rx_buff[i]));
                        s_last = inner_cntr + s_first;
                        roc_log(LogInfo,
                                "finish: s_first: %lu, s_last: %lu, inner_cntr: %lu",
                                (unsigned long)s_first, (unsigned long)s_last,
                                (unsigned long)inner_cntr);
                        break;
                    } else if (!is_zero_(samples_[inner_cntr] - rx_buff[i])) {
                        char sbuff[256];
                        int sbuff_i =
                            snprintf(sbuff, sizeof(sbuff),
                                     "failed comparing sample #%lu\n\npacket_num: %lu\n",
                                     (unsigned long)inner_cntr, (unsigned long)ipacket);
                        snprintf(&sbuff[sbuff_i], sizeof(sbuff) - (size_t)sbuff_i,
                                 "original: %f,\treceived: %f\n",
                                 (double)samples_[inner_cntr], (double)rx_buff[i]);
                        FAIL(sbuff);
                    } else {
                        inner_cntr++;
                    }
                }
            }
        }
    }

private:
    static inline bool is_zero_(float s) {
        return fabs(double(s)) < 1e-9;
    }

    roc_receiver* recv_;

    packet::Address source_addr_;
    packet::Address repair_addr_;

    const float* samples_;
    const size_t sz_;
    const size_t frame_size_;
};

class Proxy : private packet::IWriter {
public:
    Proxy(const packet::Address& dst_source_addr,
          const packet::Address& dst_repair_addr,
          const size_t block_size)
        : trx_(packet_pool, byte_buffer_pool, allocator)
        , dst_source_addr_(dst_source_addr)
        , dst_repair_addr_(dst_repair_addr)
        , block_size_(block_size)
        , num_(0) {
        CHECK(packet::parse_address("127.0.0.1:0", send_addr_));
        CHECK(packet::parse_address("127.0.0.1:0", recv_source_addr_));
        CHECK(packet::parse_address("127.0.0.1:0", recv_repair_addr_));
        writer_ = trx_.add_udp_sender(send_addr_);
        CHECK(writer_);
        CHECK(trx_.add_udp_receiver(recv_source_addr_, *this));
        CHECK(trx_.add_udp_receiver(recv_repair_addr_, *this));
    }

    packet::Address source_addr() {
        return recv_source_addr_;
    }

    packet::Address repair_addr() {
        return recv_repair_addr_;
    }

    void start() {
        trx_.start();
    }

    void stop() {
        trx_.stop();
        trx_.join();
    }

private:
    virtual void write(const packet::PacketPtr& ptr) {
        if (num_++ % block_size_ == 1) {
            // packet loss
            return;
        }
        ptr->udp()->src_addr = send_addr_;
        if (ptr->udp()->dst_addr == recv_source_addr_) {
            ptr->udp()->dst_addr = dst_source_addr_;
        } else {
            ptr->udp()->dst_addr = dst_repair_addr_;
        }
        writer_->write(ptr);
    }

    netio::Transceiver trx_;

    packet::Address send_addr_;

    packet::Address recv_source_addr_;
    packet::Address recv_repair_addr_;

    packet::Address dst_source_addr_;
    packet::Address dst_repair_addr_;

    packet::IWriter* writer_;

    const size_t block_size_;
    size_t num_;
};

} // namespace

TEST_GROUP(sender_receiver) {
    roc_sender_config sender_conf;
    roc_receiver_config receiver_conf;

    float samples[TotalSamples];

    void setup() {
        memset(&sender_conf, 0, sizeof(sender_conf));
        sender_conf.flags |= ROC_FLAG_DISABLE_INTERLEAVER;
        sender_conf.flags |= ROC_FLAG_ENABLE_TIMER;
        sender_conf.samples_per_packet = (unsigned int)PacketSamples / NumChans;
        sender_conf.fec_scheme = ROC_FEC_RS8M;
        sender_conf.n_source_packets = SourcePackets;
        sender_conf.n_repair_packets = RepairPackets;

        memset(&receiver_conf, 0, sizeof(receiver_conf));
        receiver_conf.flags |= ROC_FLAG_DISABLE_RESAMPLER;
        receiver_conf.flags |= ROC_FLAG_ENABLE_TIMER;
        receiver_conf.samples_per_packet = (unsigned int)PacketSamples / NumChans;
        receiver_conf.fec_scheme = ROC_FEC_RS8M;
        receiver_conf.n_source_packets = SourcePackets;
        receiver_conf.n_repair_packets = RepairPackets;
        receiver_conf.latency = PacketSamples * 20;
        receiver_conf.timeout = PacketSamples * 300;

        init_samples();
    }

    void init_samples() {
        const float sstep = 1. / 32768.;
        float sval = -1 + sstep;
        for (size_t i = 0; i < TotalSamples; ++i) {
            samples[i] = sval;
            sval += sstep;
            if (sval >= 1) {
                sval = -1 + sstep;
            }
        }
    }
};

TEST(sender_receiver, simple) {
    Receiver receiver(receiver_conf, samples, TotalSamples, FrameSamples);

    Sender sender(sender_conf, receiver.source_addr(), receiver.repair_addr(), samples,
                  TotalSamples, FrameSamples);

    sender.start();
    receiver.run();
    sender.join();
}

#ifdef ROC_TARGET_OPENFEC
TEST(sender_receiver, losses) {
    Receiver receiver(receiver_conf, samples, TotalSamples, FrameSamples);

    Proxy proxy(receiver.source_addr(), receiver.repair_addr(),
                SourcePackets + RepairPackets);

    Sender sender(sender_conf, proxy.source_addr(), proxy.repair_addr(), samples,
                  TotalSamples, FrameSamples);

    proxy.start();

    sender.start();
    receiver.run();
    sender.join();

    proxy.stop();
}
#endif // ROC_TARGET_OPENFEC

} // namespace roc
