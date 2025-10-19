/**
 * @file llpacketring.cpp
 * @brief implementation of LLPacketRing class for a packet.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llpacketring.h"

#if LL_WINDOWS
    #include "llwin32headerslean.h"
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

// linden library includes
#include "llerror.h"
#include "lltimer.h"
#include "llproxy.h"
#include "llrand.h"
#include "message.h"
#include "u64.h"
#include "llmessagelog.h"

// ===== AYAchemy: UDP send pacing & burst control (cpp-local, header untouched) =====
#include <cstdlib>   // std::getenv, std::strtoul
#include <cstring>   // std::strlen

// If this cpp is built in a viewer layer that exposes Debug Settings, prefer them.
#if defined(LL_VIEWER)
#   include "llviewercontrol.h" // gSavedSettings
#endif

namespace {
    struct UdpPacingState {
        bool loaded = false;
        // Tunables (can be overridden via Debug Settings or env vars)
        U32 min_gap_us = 0;   // UdpMinSendGapUsec, 0 = disabled (preserve legacy behavior)
        U32 win_ms     = 10;  // UdpBurstWindowMs
        U32 per_win    = 20;  // UdpBurstPerWindow
        // Runtime state
        U64 last_send_us = 0;
        U64 win_start_us = 0;
        U32 sent_in_win  = 0;
    };
    static UdpPacingState s_udp;

    inline U64 now_us()
    {
        // LLTimer::getTotalTime() returns 100-ns units
        return (U64)(LLTimer::getTotalTime() / 10);
    }

    inline U32 getenv_u32(const char* key, U32 def)
    {
        const char* v = std::getenv(key);
        if (!v || !*v) return def;
        char* endp = NULL;
        unsigned long val = std::strtoul(v, &endp, 10);
        if (endp == v) return def;
        if (val > 0xFFFFFFFFul) val = 0xFFFFFFFFul;
        return (U32)val;
    }

    inline void load_tunables_once()
    {
        if (s_udp.loaded) return;

        // 1) Prefer Debug Settings when available (viewer layer)
        #if defined(LL_VIEWER)
        if (gSavedSettings.controlExists("UdpMinSendGapUsec"))
            s_udp.min_gap_us = gSavedSettings.getU32("UdpMinSendGapUsec");
        if (gSavedSettings.controlExists("UdpBurstWindowMs"))
            s_udp.win_ms = gSavedSettings.getU32("UdpBurstWindowMs");
        if (gSavedSettings.controlExists("UdpBurstPerWindow"))
            s_udp.per_win = gSavedSettings.getU32("UdpBurstPerWindow");
        #endif

        // 2) Allow env var overrides (works in any layer)
        s_udp.min_gap_us = getenv_u32("AYACHEMY_UDP_MIN_GAP_USEC",      s_udp.min_gap_us);
        s_udp.win_ms     = getenv_u32("AYACHEMY_UDP_BURST_WINDOW_MS",   s_udp.win_ms);
        s_udp.per_win    = getenv_u32("AYACHEMY_UDP_BURST_PER_WINDOW",  s_udp.per_win);

        s_udp.loaded = true;
    }

    inline bool should_queue_now()
    {
        load_tunables_once();
        const U64 t = now_us();

        // Minimum inter-send gap
        if (s_udp.min_gap_us && s_udp.last_send_us)
        {
            if (t - s_udp.last_send_us < (U64)s_udp.min_gap_us)
            {
                return true;
            }
        }

        // Burst limiter (sliding window)
        if (s_udp.per_win && s_udp.win_ms)
        {
            const U64 win_us = (U64)s_udp.win_ms * 1000ULL;
            if (s_udp.win_start_us == 0 || (t - s_udp.win_start_us) >= win_us)
            {
                s_udp.win_start_us = t;
                s_udp.sent_in_win  = 0;
            }
            if (s_udp.sent_in_win >= s_udp.per_win)
            {
                return true;
            }
        }
        return false;
    }

    inline void note_sent_ok()
    {
        s_udp.last_send_us = now_us();
        if (s_udp.per_win) ++s_udp.sent_in_win;
    }
} // namespace
// ===== end AYAchemy pacing block =====

///////////////////////////////////////////////////////////
LLPacketRing::LLPacketRing () :
    mUseInThrottle(FALSE),
    mUseOutThrottle(FALSE),
    mInThrottle(256000.f),
    mOutThrottle(64000.f),
    mActualBitsIn(0),
    mActualBitsOut(0),
    mMaxBufferLength(64000),
    mInBufferLength(0),
    mOutBufferLength(0),
    mDropPercentage(0.0f),
    mPacketsToDrop(0x0)
{
}

///////////////////////////////////////////////////////////
LLPacketRing::~LLPacketRing ()
{
    cleanup();
}

///////////////////////////////////////////////////////////
void LLPacketRing::cleanup ()
{
    LLPacketBuffer *packetp;

    while (!mReceiveQueue.empty())
    {
        packetp = mReceiveQueue.front();
        delete packetp;
        mReceiveQueue.pop();
    }

    while (!mSendQueue.empty())
    {
        packetp = mSendQueue.front();
        delete packetp;
        mSendQueue.pop();
    }
}

///////////////////////////////////////////////////////////
void LLPacketRing::dropPackets (U32 num_to_drop)
{
    mPacketsToDrop += num_to_drop;
}

///////////////////////////////////////////////////////////
void LLPacketRing::setDropPercentage (F32 percent_to_drop)
{
    mDropPercentage = percent_to_drop;
}

void LLPacketRing::setUseInThrottle(const BOOL use_throttle)
{
    mUseInThrottle = use_throttle;
}

void LLPacketRing::setUseOutThrottle(const BOOL use_throttle)
{
    mUseOutThrottle = use_throttle;
}

void LLPacketRing::setInBandwidth(const F32 bps)
{
    mInThrottle.setRate(bps);
}

void LLPacketRing::setOutBandwidth(const F32 bps)
{
    mOutThrottle.setRate(bps);
}
///////////////////////////////////////////////////////////
S32 LLPacketRing::receiveFromRing (S32 socket, char *datap)
{

    if (mInThrottle.checkOverflow(0))
    {
        // We don't have enough bandwidth, don't give them a packet.
        return 0;
    }

    LLPacketBuffer *packetp = NULL;
    if (mReceiveQueue.empty())
    {
        // No packets on the queue, don't give them any.
        return 0;
    }

    S32 packet_size = 0;
    packetp = mReceiveQueue.front();
    mReceiveQueue.pop();
    packet_size = packetp->getSize();
    if (packetp->getData() != NULL)
    {
        memcpy(datap, packetp->getData(), packet_size); /*Flawfinder: ignore*/
    }
    // need to set sender IP/port!!
    mLastSender = packetp->getHost();
    mLastReceivingIF = packetp->getReceivingInterface();
    delete packetp;

    this->mInBufferLength -= packet_size;

    // Adjust the throttle
    mInThrottle.throttleOverflow(packet_size * 8.f);
    return packet_size;
}

///////////////////////////////////////////////////////////
S32 LLPacketRing::receivePacket (S32 socket, char *datap)
{
    S32 packet_size = 0;

    // If using the throttle, simulate a limited size input buffer.
    if (mUseInThrottle)
    {
        BOOL done = FALSE;

        // push any current net packet (if any) onto delay ring
        while (!done)
        {
            LLPacketBuffer *packetp;
            packetp = new LLPacketBuffer(socket);

            if (packetp->getSize())
            {
                mActualBitsIn += packetp->getSize() * 8;

                // Fake packet loss
                if (mDropPercentage && (ll_frand(100.f) < mDropPercentage))
                {
                    mPacketsToDrop++;
                }

                if (mPacketsToDrop)
                {
                    delete packetp;
                    packetp = NULL;
                    packet_size = 0;
                    mPacketsToDrop--;
                }
            }

            // If we faked packet loss, then we don't have a packet
            // to use for buffer overflow testing
            if (packetp)
            {
                if (mInBufferLength + packetp->getSize() > mMaxBufferLength)
                {
                    // Toss it.
                    LL_WARNS() << "Throwing away packet, overflowing buffer" << LL_ENDL;
                    delete packetp;
                    packetp = NULL;
                }
                else if (packetp->getSize())
                {
                    mReceiveQueue.push(packetp);
                    mInBufferLength += packetp->getSize();
                }
                else
                {
                    delete packetp;
                    packetp = NULL;
                    done = true;
                }
            }
            else
            {
                // No packetp, keep going? - no packetp == faked packet loss
            }
        }

        // Now, grab data off of the receive queue according to our
        // throttled bandwidth settings.
        packet_size = receiveFromRing(socket, datap);
    }
    else
    {
        // no delay, pull straight from net
        if (LLProxy::isSOCKSProxyEnabled())
        {
            U8 buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];
            packet_size = receive_packet(socket, static_cast<char*>(static_cast<void*>(buffer)));

            if (packet_size > SOCKS_HEADER_SIZE)
            {
                // *FIX We are assuming ATYP is 0x01 (IPv4), not 0x03 (hostname) or 0x04 (IPv6)
                memcpy(datap, buffer + SOCKS_HEADER_SIZE, packet_size - SOCKS_HEADER_SIZE);
                proxywrap_t * header = static_cast<proxywrap_t*>(static_cast<void*>(buffer));
                mLastSender.setAddress(header->addr);
                mLastSender.setPort(ntohs(header->port));

                packet_size -= SOCKS_HEADER_SIZE; // The unwrapped packet size
            }
            else
            {
                packet_size = 0;
            }
        }
        else
        {
            packet_size = receive_packet(socket, datap);
            mLastSender = ::get_sender();
        }

        mLastReceivingIF = ::get_receiving_interface();

        if (packet_size)  // did we actually get a packet?
        {
            if (mDropPercentage && (ll_frand(100.f) < mDropPercentage))
            {
                mPacketsToDrop++;
            }

            if (mPacketsToDrop)
            {
                packet_size = 0;
                mPacketsToDrop--;
            }
        }
    }

    return packet_size;
}

BOOL LLPacketRing::sendPacket(int h_socket, char * send_buffer, S32 buf_size, const LLHost& host)
{
#define LOCALHOST_ADDR 16777343
    LLMessageLog::log(LLHost(LOCALHOST_ADDR, gMessageSystem->getListenPort()), host, (U8*)send_buffer, buf_size);
#undef LOCALHOST_ADDR
    BOOL status = TRUE;
    if (!mUseOutThrottle)
    {
        // ===== AYAchemy: apply pacing even when throttle is off =====
        if (should_queue_now())
        {
            if (mOutBufferLength + buf_size > mMaxBufferLength)
            {
                // Overflow: drop to preserve legacy behavior on overrun
                LL_WARNS() << "Throwing away outbound packet, overflowing buffer" << LL_ENDL;
                return TRUE;
            }
            LLPacketBuffer* packetp = new LLPacketBuffer(host, send_buffer, buf_size);
            mOutBufferLength += packetp->getSize();
            mSendQueue.push(packetp);
            return TRUE; // queued; will be flushed by subsequent calls
        }

        status = sendPacketImpl(h_socket, send_buffer, buf_size, host );
        if (status) { note_sent_ok(); }
        return status;
    }
    else
    {
        mActualBitsOut += buf_size * 8;

        // ===== AYAchemy: also honor pacing when throttle is ON =====
        if (should_queue_now())
        {
            if (mOutBufferLength + buf_size > mMaxBufferLength)
            {
                LL_WARNS() << "Throwing away outbound packet, overflowing buffer" << LL_ENDL;
                return TRUE;
            }
            LLPacketBuffer* qp = new LLPacketBuffer(host, send_buffer, buf_size);
            mOutBufferLength += qp->getSize();
            mSendQueue.push(qp);
            // Do not return here: the while-loop below will try draining queue as bandwidth allows
        }

        LLPacketBuffer *packetp = NULL;
        // See if we've got enough throttle to send a packet.
        while (!mOutThrottle.checkOverflow(0.f))
        {
            // While we have enough bandwidth, send a packet from the queue or the current packet

            S32 packet_size = 0;
            if (!mSendQueue.empty())
            {
                // Send a packet off of the queue
                packetp = mSendQueue.front();
                mSendQueue.pop();

                mOutBufferLength -= packetp->getSize();
                packet_size = packetp->getSize();

                status = sendPacketImpl(h_socket, packetp->getData(), packet_size, packetp->getHost());
                if (status) { note_sent_ok(); }

                delete packetp;
                // Update the throttle
                mOutThrottle.throttleOverflow(packet_size * 8.f);
            }
            else
            {
                // If the queue's empty, we can just send this packet right away.
                status =  sendPacketImpl(h_socket, send_buffer, buf_size, host );
                if (status) { note_sent_ok(); }
                packet_size = buf_size;

                // Update the throttle
                mOutThrottle.throttleOverflow(packet_size * 8.f);

                // This was the packet we're sending now, there are no other packets
                // that we need to send
                return status;
            }

        }

        // We haven't sent the incoming packet, add it to the queue
        if (mOutBufferLength + buf_size > mMaxBufferLength)
        {
            // Nuke this packet, we overflowed the buffer.
            // Toss it.
            LL_WARNS() << "Throwing away outbound packet, overflowing buffer" << LL_ENDL;
        }
        else
        {
            static LLTimer queue_timer;
            if ((mOutBufferLength > 4192) && queue_timer.getElapsedTimeF32() > 1.f)
            {
                // Add it to the queue
                LL_INFOS() << "Outbound packet queue " << mOutBufferLength << " bytes" << LL_ENDL;
                queue_timer.reset();
            }
            packetp = new LLPacketBuffer(host, send_buffer, buf_size);

            mOutBufferLength += packetp->getSize();
            mSendQueue.push(packetp);
        }
    }

    return status;
}

BOOL LLPacketRing::sendPacketImpl(int h_socket, const char * send_buffer, S32 buf_size, const LLHost& host)
{

    if (!LLProxy::isSOCKSProxyEnabled())
    {
        return send_packet(h_socket, send_buffer, buf_size, host.getAddress(), host.getPort());
    }

    char headered_send_buffer[NET_BUFFER_SIZE + SOCKS_HEADER_SIZE];

    proxywrap_t *socks_header = static_cast<proxywrap_t*>(static_cast<void*>(&headered_send_buffer));
    socks_header->rsv   = 0;
    socks_header->addr  = host.getAddress();
    socks_header->port  = htons(host.getPort());
    socks_header->atype = ADDRESS_IPV4;
    socks_header->frag  = 0;

    memcpy(headered_send_buffer + SOCKS_HEADER_SIZE, send_buffer, buf_size);

    return send_packet( h_socket,
                        headered_send_buffer,
                        buf_size + SOCKS_HEADER_SIZE,
                        LLProxy::getInstance()->getUDPProxy().getAddress(),
                        LLProxy::getInstance()->getUDPProxy().getPort());
}
