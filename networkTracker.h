/*
    SysmonForLinux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//====================================================================
//
// networkTracker.h
//
// Supporting classes for networkTracker.cpp
//
//====================================================================

#ifndef NETWORK_TRACKER_H
#define NETWORK_TRACKER_H

#ifdef __cplusplus

#include <unordered_map>
#include <unordered_set>
#include <map>
extern "C" {
#include <libsysinternalsEBPF.h>
}
#include "linuxTypes.h"
#include "ioctlcmd.h"
#include "sysmon_defs.h"

#define INODE_SOCK_PRE1 "socket:["
#define INODE_SOCK_PRE2 "[0000]:"
#define PROC_TCP_FNAME "/proc/net/tcp"
#define PROC_TCP6_FNAME "/proc/net/tcp6"
#define PROC_UDP_FNAME "/proc/net/udp"
#define PROC_UDP6_FNAME "/proc/net/udp6"
#define FNV_INIT 0xcbf29ce484222325
#define FNV_MULT 0x100000001b3

//--------------------------------------------------------------------
//
// AddrAndPort
//
// Class that represents an IPv4/6 address and port.  Includes a
// comparison operator.
//
//--------------------------------------------------------------------
class AddrAndPort
{
public:
    AddrAndPort(const BYTE *addr_in, bool IPv4, unsigned short port) :
        IPv4(IPv4), port(port)
    {
        if (IPv4) memcpy(addr, addr_in, 4);
        else memcpy(addr, addr_in, 16);
    };

    bool operator ==(const AddrAndPort& rhs) const
    {
        if (IPv4 != rhs.IPv4) return false;
        if (IPv4) {
            return (memcmp(addr, rhs.addr, 4) == 0 && port == rhs.port);
        }
        return (memcmp(addr, rhs.addr, 16) == 0 && port == rhs.port);
    }

    BYTE addr[16];
    bool IPv4;
    unsigned short port;
};

//--------------------------------------------------------------------
//
// PacketAddresses
//
// Class that represents local and remote IPv4/6 addresses and ports.
// Includes comparison operators.
//
//--------------------------------------------------------------------
class PacketAddresses
{
public:
    PacketAddresses()
    {
        IPv4 = true;
        memset(localAddr, 0, sizeof(localAddr));
        memset(remoteAddr, 0, sizeof(remoteAddr));
        localPort = 0;
        remotePort = 0;
    }

    PacketAddresses(bool IPv4, const BYTE *localAddrIn, unsigned short localPort,
            const BYTE *remoteAddrIn, unsigned short remotePort) :
        IPv4(IPv4), localPort(localPort), remotePort(remotePort)
    {
        if (IPv4) {
            memcpy(localAddr, localAddrIn, 4);
            memcpy(remoteAddr, remoteAddrIn, 4);
        } else {
            memcpy(localAddr, localAddrIn, 16);
            memcpy(remoteAddr, remoteAddrIn, 16);
        }
    };

    bool operator ==(const PacketAddresses& rhs) const
    {
        if (IPv4 != rhs.IPv4) return false;
        if (IPv4) {
            return (memcmp(localAddr, rhs.localAddr, 4) == 0 &&
                    localPort == rhs.localPort &&
                    memcmp(remoteAddr, rhs.remoteAddr, 4) == 0 &&
                    remotePort == rhs.remotePort);
        }
        return (memcmp(localAddr, rhs.localAddr, 16) == 0 &&
                localPort == rhs.localPort &&
                memcmp(remoteAddr, rhs.remoteAddr, 16) == 0 &&
                remotePort == rhs.remotePort);
    }

    bool operator !=(const PacketAddresses& rhs) const
    {
        return !(*this == rhs);
    }

    bool IPv4;
    BYTE localAddr[16];
    BYTE remoteAddr[16];
    unsigned short localPort, remotePort;
};

namespace std {

//--------------------------------------------------------------------
//
// hash<AddrAndPort>
//
// Hash function for AddrAndPort class allowing it to be used as a
// map key.
//
//--------------------------------------------------------------------
    template <>
    struct hash<AddrAndPort>
    {
        std::size_t operator()(const AddrAndPort& k) const
        {
            // hash using Fowler–Noll–Vo hash function
            uint64_t hash = FNV_INIT;
            for (unsigned int i=0; i<16; i++) {
                hash = hash * FNV_MULT;
                if (!k.IPv4 || i<4)
                    hash = hash ^ k.addr[i];
            }
            hash = (hash * FNV_MULT) ^ k.IPv4;
            hash = (hash * FNV_MULT) ^ (k.port >> 8);
            hash = (hash * FNV_MULT) ^ (k.port & 0xff);
            return hash;
        }
    };

//--------------------------------------------------------------------
//
// hash<PacketAddresses>
//
// Hash function for PacketAddresses class allowing it to be used as a
// map key.
//
//--------------------------------------------------------------------
    template <>
    struct hash<PacketAddresses>
    {
        std::size_t operator()(const PacketAddresses& k) const
        {
            // hash using Fowler–Noll–Vo hash function
            uint64_t hash = FNV_INIT;
            hash = (hash * FNV_MULT) ^ k.IPv4;
            for (unsigned int i=0; i<16; i++) {
                hash = hash * FNV_MULT;
                if (!k.IPv4 || i<4)
                    hash = hash ^ k.localAddr[i];
            }
            for (unsigned int i=0; i<16; i++) {
                hash = hash * FNV_MULT;
                if (!k.IPv4 || i<4)
                    hash = hash ^ k.remoteAddr[i];
            }
            hash = (hash * FNV_MULT) ^ (k.localPort >> 8);
            hash = (hash * FNV_MULT) ^ (k.localPort & 0xff);
            hash = (hash * FNV_MULT) ^ (k.remotePort >> 8);
            hash = (hash * FNV_MULT) ^ (k.remotePort & 0xff);
            return hash;
        }
    };
}


class NetworkTracker
{
    friend class NetworkTrackerTest;

    // connect tracking
    std::unordered_map<const void *, std::pair<pid_t, LONGLONG>> connectTracker;
    std::map<LONGLONG, const void *> connectTrackerTimes;

    // accept tracking
    static constexpr size_t MAX_ACCEPT_ENTRIES = 65536;
    using AcceptTrackerTimes = std::multimap<LONGLONG, AddrAndPort>;
    std::unordered_map<AddrAndPort, std::pair<AddrAndPort, AcceptTrackerTimes::iterator>> acceptTracker;
    AcceptTrackerTimes acceptTrackerTimes;
    bool acceptLimitReported = false;

    // UDP recv tracking
    std::map<LONGLONG, uint64_t> udpRecvTrackerTimes;
    std::unordered_map<pid_t, std::unordered_map<int, std::pair<LONGLONG, PacketAddresses>>> udpRecvTrackerPidFds;

    // UDP send tracking
    std::map<LONGLONG, packetAddrs> udpSendTrackerTimes;
    std::unordered_map<pid_t, std::unordered_map<PacketAddresses, LONGLONG>> udpSendTrackerPidAddrs;

    // parameters
    LONGLONG staleDuration, checkDuration; // in 100ns intervals
    LARGE_INTEGER timeLastChecked; // time we last checked in 100ns intervals from epoch

    void PurgeStale();
    LONGLONG FindOrEraseUdpPidFd(pid_t pid, int fd, const PacketAddresses *p, bool erase);
    LONGLONG UpdateUdpPidFd(pid_t pid, int fd, LONGLONG ctime, const PacketAddresses *p);
    void PurgeUdp(LONGLONG curTime);
    uint64_t PathToInode(const char *path);
    bool InodeToAddr(PacketAddresses *p, bool IPv4, uint64_t inodeIn);
    bool GetUdp(PacketAddresses *p, pid_t pid, int fd);
    LONGLONG FindOrEraseUdpPidAddr(pid_t pid, const PacketAddresses *p, bool erase);
    LONGLONG UpdateUdpPidAddr(pid_t pid, LONGLONG ctime, const PacketAddresses *p);

public:

    NetworkTracker(LONGLONG staleSeconds, LONGLONG checkSeconds);
    pid_t SeenConnect(PSYSMON_EVENT_HEADER event);

    void SeenAccept(AddrAndPort sourceAddrAndPort, AddrAndPort destAddrAndPort, LONGLONG eventTime);
    bool SeenAccept(AddrAndPort sourceAddrAndPort, AddrAndPort *destAddrAndPort);
    void CloseAccept(AddrAndPort sourceAddrAndPort, AddrAndPort destAddrAndPort);

    bool SeenUdp(PacketAddresses *p, pid_t pid, int fd);
    bool SeenUdp(const PacketAddresses *p, pid_t pid);
    void UdpProgramTermination(pid_t pid);
};

#else
struct NetworkTracker;
struct NetworkTracker *NetworkTrackerInit(LONGLONG staleSeconds, LONGLONG checkSeconds);
pid_t NetworkTrackerSeenConnect(struct NetworkTracker *n, PSYSMON_EVENT_HEADER eventHdr);

void NetworkTrackerSeenFullAccept(struct NetworkTracker *n, bool IPv4, BYTE *sourceAddr, unsigned short sourcePort,
        BYTE *destAddr, unsigned short destPort, LONGLONG eventTime);
bool NetworkTrackerSeenAccept(struct NetworkTracker *n, bool IPv4, BYTE *sourceAddr, unsigned short sourcePort,
        BYTE *destAddr, unsigned short *destPort);
void NetworkTrackerCloseAccept(struct NetworkTracker *n, bool IPv4, BYTE *sourceAddr, unsigned short sourcePort,
        BYTE *destAddr, unsigned short destPort);

bool NetworkTrackerSeenUdpRecv(struct NetworkTracker *n, bool *IPv4, BYTE *sourceAddr,
        unsigned short *sourcePort, BYTE *destAddr, unsigned short *destPort, pid_t pid, int fd);
bool NetworkTrackerSeenUdpSend(struct NetworkTracker *n, bool IPv4, const BYTE *sourceAddr,
        unsigned short sourcePort, const BYTE *destAddr, unsigned short destPort, pid_t pid);
void NetworkTrackerUdpProgramTermination(struct NetworkTracker *n, pid_t pid);
#endif

#endif

