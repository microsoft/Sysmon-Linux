/*
    SysmonForLinux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

    THE SOFTWARE IS* PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//====================================================================
//
// networkTracker.cpp
//
// Class for tracking the various network connection telemetry,
// including managing eBPF maps (ageing off, handling saturation).
//
//====================================================================

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "networkTracker.h"
#include "linuxHelpers.h"

#include "missingdefs.h"

extern int mapFds[];

//--------------------------------------------------------------------
//
// NetworkTracker constructor
//
// Initialise values.
//
//--------------------------------------------------------------------
NetworkTracker::NetworkTracker(LONGLONG staleSeconds, LONGLONG checkSeconds)
{
    staleDuration = staleSeconds * 1000 * 1000 * 10; // in ns intervals
    checkDuration = checkSeconds * 1000 * 1000 * 10; // in ns intervals
    GetSystemTimeAsLargeInteger(&timeLastChecked);
}

//--------------------------------------------------------------------
//
// PurgeStale
//
// Purge old entries in the Connect and Accept trackers.  Uses a
// time-based ordered map to walk the entries, which holds keys to the
// other data structures.
//
//--------------------------------------------------------------------
void NetworkTracker::PurgeStale()
{
    LARGE_INTEGER curTime;

    // periodically remove stale entries
    GetSystemTimeAsLargeInteger(&curTime);
    if (curTime.QuadPart - timeLastChecked.QuadPart > checkDuration) {
        auto it = connectTrackerTimes.cbegin();
        while (it != connectTrackerTimes.cend()) {
            if (curTime.QuadPart - it->first > staleDuration) {
                connectTracker.erase(it->second);
                it = connectTrackerTimes.erase(it);
            } else {
                break; // as this is an ordered map, we have exhausted all stale items
            }
        }
        auto it2 = acceptTrackerTimes.cbegin();
        while (it2 != acceptTrackerTimes.cend()) {
            if (curTime.QuadPart - it2->first > staleDuration) {
                auto entry = acceptTracker.find(*(it2->second));
                it2 = acceptTrackerTimes.erase(it2);
                acceptTracker.erase(entry);
            } else {
                break; // as this is an ordered map, we have exhausted all stale items
            }
        }
        timeLastChecked = curTime;
    }
}

//--------------------------------------------------------------------
//
// SeenConnect
//
// Connects are observed in three transitions: CLOSE->SYN_SENT,
// SYN_SENT->ESTABLISHED, ESTABLISHED->CLOSE. The first transistion
// is in the process context and the associated process ID is of the
// process making the connection - this is stored, but the connection
// hasn't yet been properly established (maybe the port is closed,
// for example). The second transition establishes the connection, but
// the process ID is of a daemon as this happens asynchronously, hence
// retrieve the PID from the first transition. The third transition
// is the connection closing, so remove any SYN_SENT transitions.
//
// Returns the PID for established connections, otherwise 0.
//
//--------------------------------------------------------------------
pid_t NetworkTracker::SeenConnect(PSYSMON_EVENT_HEADER eventHdr)
{
    if (eventHdr == NULL) {
        fprintf(stderr, "NetworkTracker::SeenConnect invalid params\n");
        return 0;
    }

    pid_t pid = 0;
    PSYSMON_LINUX_NETWORK_EVENT event = (PSYSMON_LINUX_NETWORK_EVENT)&eventHdr->m_EventBody;

    if (event->m_NewState == TCP_CLOSE) {
        // remove state
        auto it = connectTracker.find(event->m_SockId);
        if (it != connectTracker.end()) {
            connectTrackerTimes.erase(it->second.second);
            connectTracker.erase(it);
        }
        pid = 0;
    } else {

        // attempt to insert - bool indicates success/failure
        auto ret = connectTracker.emplace(event->m_SockId, std::pair<pid_t, LONGLONG>(event->m_ProcessId, event->m_EventTime.QuadPart));

        if (ret.second) {
            // successful insertion - didn't already exist
            connectTrackerTimes.emplace(event->m_EventTime.QuadPart, event->m_SockId);
            pid = 0;
        } else {
            // already exists
            pid = ret.first->second.first;
            if (event->m_NewState == TCP_ESTABLISHED) {
                // connection is established, so remove it
                connectTracker.erase(event->m_SockId);
                connectTrackerTimes.erase(event->m_EventTime.QuadPart);
            }
        }
    }

    PurgeStale();

    return pid;
}

//--------------------------------------------------------------------
//
// SeenAccept - transition of port state
//
// Accept transitions all happen asynchronously, so store the
// established transitions (which contains full source/dest
// addr/port).
//
//--------------------------------------------------------------------
void NetworkTracker::SeenAccept(AddrAndPort sourceAddrAndPort, AddrAndPort destAddrAndPort, LONGLONG eventTime)
{
    auto it = acceptTracker.find(sourceAddrAndPort);
    if (it != acceptTracker.end()) {
        acceptTrackerTimes.erase(it->second.second);
        acceptTracker.erase(it);
    }

    if (acceptTracker.size() >= MAX_ACCEPT_ENTRIES) {
        auto oldest = acceptTrackerTimes.begin();
        auto entry = acceptTracker.find(*(oldest->second));
        acceptTrackerTimes.erase(oldest);
        acceptTracker.erase(entry);
        if (!acceptLimitReported) {
            fprintf(stderr, "NetworkTracker::SeenAccept capacity reached; oldest pending accepts will be evicted (reported once)\n");
            acceptLimitReported = true;
        }
    }

    auto ret = acceptTracker.emplace(sourceAddrAndPort, std::make_pair(destAddrAndPort, acceptTrackerTimes.end()));
    ret.first->second.second = acceptTrackerTimes.emplace(eventTime, &(ret.first->first));

    PurgeStale();
}

//--------------------------------------------------------------------
//
// SeenAccept - successful call to accept() syscall
//
// Calls to accept() don't provide easy access to full source/dest
// addr/port, but do happen in the context of the process, so the
// process ID is valid. This function matches the successful call to
// accept() to the previously stored port transition from SYN_RECV to
// ESTABLISHED.
//
// Returns true on success, false otherwise.
//
//--------------------------------------------------------------------
bool NetworkTracker::SeenAccept(AddrAndPort sourceAddrAndPort, AddrAndPort *destAddrAndPort)
{
    if (destAddrAndPort == NULL) {
        fprintf(stderr, "NetworkTracker::SeenAccept invalid params\n");
        return false;
    }

    auto it = acceptTracker.find(sourceAddrAndPort);
    if (it == acceptTracker.end()) {
        return false;
    }
    acceptTrackerTimes.erase(it->second.second);
    memcpy(destAddrAndPort, &it->second.first, sizeof(AddrAndPort));
    acceptTracker.erase(it);
    return true;
}

//--------------------------------------------------------------------
//
// CleseAccept
//
// Remove entries where port was closed.
//
//--------------------------------------------------------------------
void NetworkTracker::CloseAccept(AddrAndPort sourceAddrAndPort, AddrAndPort destAddrAndPort)
{
    auto it = acceptTracker.find(sourceAddrAndPort);
    if (it == acceptTracker.end()) {
        return;
    }
    if (!(it->second.first == destAddrAndPort)) {
        return;
    }
    acceptTrackerTimes.erase(it->second.second);
    acceptTracker.erase(it);
}

//--------------------------------------------------------------------
//
// FindOrEraseUdpPidFd
//
// For inbound UDP connections, PID and file descriptors are stored.
// This function locates and optionally erases them from the data
// structures.
//
// Returns the last time they were reported, or 0 if not found.
//
//--------------------------------------------------------------------
LONGLONG NetworkTracker::FindOrEraseUdpPidFd(pid_t pid, int fd, const PacketAddresses *p, bool erase)
{
    LONGLONG lastTime = 0;

    auto it = udpRecvTrackerPidFds.find(pid);
    if (it == udpRecvTrackerPidFds.end())
        // no existing pid entry
        return 0;
    auto it2 = it->second.find(fd);
    if (it2 == it->second.end())
        // no existing fd entry
        return 0;

    if (p == NULL || it2->second.second != *p) {
        // packet doesn't match
        lastTime = 0;
    } else {
        lastTime = it2->second.first;
    }

    if (erase) {
        // remove entries
        it->second.erase(it2);
        if (it->second.empty()) {
            udpRecvTrackerPidFds.erase(it);
        }
    }
    return lastTime;
}

//--------------------------------------------------------------------
//
// FindOrEraseUdpPidAddr
//
// For outbound UDP connections, PID and addresses are stored.
// This function locates and optionally erases them from the data
// structures.
//
// Returns the last time they were reported, or 0 if not found.
//
//--------------------------------------------------------------------
LONGLONG NetworkTracker::FindOrEraseUdpPidAddr(pid_t pid, const PacketAddresses *p, bool erase)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::FindOrEraseUdpPidAddr invalid params\n");
        return 0;
    }

    LONGLONG lastTime = 0;

    auto it = udpSendTrackerPidAddrs.find(pid);
    if (it == udpSendTrackerPidAddrs.end())
        // no existing pid entry
        return 0;
    auto it2 = it->second.find(*p);
    if (it2 == it->second.end())
        // no existing address entry
        return 0;

    lastTime = it2->second;

    if (erase) {
        // remove entries
        it->second.erase(it2);
        if (it->second.empty()) {
            udpSendTrackerPidAddrs.erase(it);
        }
    }
    return lastTime;
}

//--------------------------------------------------------------------
//
// UpdateUdpPidFd
//
// For inbound UDP connections, updates a PID/FD entry.
//
// Returns the last time they were reported, or 0 if not previous
// entry.
//
//--------------------------------------------------------------------
LONGLONG NetworkTracker::UpdateUdpPidFd(pid_t pid, int fd, LONGLONG ctime, const PacketAddresses *p)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::UpdateUdpPidFd invalid params\n");
        return 0;
    }

    LONGLONG lastTime = 0;

    std::pair<LONGLONG, PacketAddresses> newEntry(ctime, *p);

    auto it = udpRecvTrackerPidFds.find(pid);
    if (it == udpRecvTrackerPidFds.end()) {
        // no existing pid entry
        udpRecvTrackerPidFds.emplace(pid, std::unordered_map<int, std::pair<LONGLONG, PacketAddresses>>({{fd, newEntry}}));
        return 0;
    }
    auto it2 = it->second.find(fd);
    if (it2 == it->second.end()) {
        // no existing fd entry
        it->second.emplace(fd, newEntry);
        return 0;
    }

    // update entry and return previous time
    if (it2->second.second == *p) {
        lastTime = it2->second.first;
        it2->second.first = ctime;
        return lastTime;
    }

    // entry was for a previous socket
    it->second.erase(it2);
    it->second.emplace(fd, newEntry);
    return 0;
}

//--------------------------------------------------------------------
//
// UpdateUdpPidAddr
//
// For outbound UDP connections, updates a PID/Address entry.
//
// Returns the last time they were reported, or 0 if not previous
// entry.
//
//--------------------------------------------------------------------
LONGLONG NetworkTracker::UpdateUdpPidAddr(pid_t pid, LONGLONG ctime, const PacketAddresses *p)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::UpdateUdpPidAddr invalid params\n");
        return 0;
    }

    LONGLONG lastTime = 0;

    auto it = udpSendTrackerPidAddrs.find(pid);
    if (it == udpSendTrackerPidAddrs.end()) {
        // no existing pid entry
        udpSendTrackerPidAddrs.emplace(pid, std::unordered_map<PacketAddresses, LONGLONG>({{*p, ctime}}));
        return 0;
    }
    auto it2 = it->second.find(*p);
    if (it2 == it->second.end()) {
        // no existing address entry
        it->second.emplace(*p, ctime);
        return 0;
    }

    // update entry and return previous time
    lastTime = it2->second;
    it2->second = ctime;
    return lastTime;
}

//--------------------------------------------------------------------
//
// PurgeUdp
//
// Purge old entries in the outbound and inbound UDP trackers.  Uses a
// time-based ordered map to walk the entries, which holds keys to the
// other data structures.
//
//--------------------------------------------------------------------
void NetworkTracker::PurgeUdp(LONGLONG curTime)
{
    LONGLONG lastTime = 0;
    pid_t pid = 0;
    int fd;

    // traverse all PID/FD entries chronologically
    auto it = udpRecvTrackerTimes.cbegin();
    while (it != udpRecvTrackerTimes.cend()) {
        // if an entry is older than our reporting interval
        if (curTime - it->first > UDP_REPORT_INTERVAL) {
            pid = it->second >> 32;
            fd = it->second & 0xFFFFFFFF;
            // check if the kernel version is older than our reporting interval or not
            if (telemetryMapLookupElem(mapFds[UDP_PIDFD_HASH], &it->second, &lastTime) < 0) {
                FindOrEraseUdpPidFd(pid, fd, NULL, true);
            } else {
                if (curTime - lastTime > UDP_REPORT_INTERVAL) {
                    // remove the element from the kernel hash
                    telemetryMapDeleteElem(mapFds[UDP_PIDFD_HASH], &it->second);
                    // remove from the pid and fd tracker
                    FindOrEraseUdpPidFd(pid, fd, NULL, true);
                } else {
                    // update our records with latest seen time
                    PacketAddresses p;
                    if (GetUdp(&p, pid, fd)) {
                        UpdateUdpPidFd(pid, fd, lastTime, &p);
                        udpRecvTrackerTimes.emplace(lastTime, it->second);
                    } else {
                        FindOrEraseUdpPidFd(pid, fd, NULL, true);
                    }
                }
            }
            // remove existing time entry and iterate
            it = udpRecvTrackerTimes.erase(it);
        } else {
            break; // as this is an ordered map, we have exhausted all stale items
        }
    }

    // traverse all packetAddrs entries chronologically
    auto it2 = udpSendTrackerTimes.cbegin();
    while (it2 != udpSendTrackerTimes.cend()) {
        // if an entry is older than our reporting interval
        if (curTime - it2->first > UDP_REPORT_INTERVAL) {
            PacketAddresses p(it2->second.IPv4, it2->second.srcAddr, it2->second.srcPort,
                    it2->second.dstAddr, it2->second.dstPort);
            // check if the kernel version is older than our reporting interval or not
            if (telemetryMapLookupElem(mapFds[UDP_ADDRS_HASH], &it2->second, &lastTime) < 0) {
                FindOrEraseUdpPidAddr(pid, &p, true);
            } else {
                if (curTime - lastTime > UDP_REPORT_INTERVAL) {
                    // remove the element from the kernel hash
                    telemetryMapDeleteElem(mapFds[UDP_ADDRS_HASH], &it2->second);
                    // remove from the pid and addr tracker
                    FindOrEraseUdpPidAddr(pid, &p, true);
                } else {
                    // update our records with latest seen time
                    UpdateUdpPidAddr(pid, lastTime, &p);
                    udpSendTrackerTimes.emplace(lastTime, it2->second);
                }
            }
            // remove existing time entry and iterate
            it2 = udpSendTrackerTimes.erase(it2);
        } else {
            break; // as this is an ordered map, we have exhausted all stale items
        }
    }
}

//--------------------------------------------------------------------
//
// PathToInode
//
// Returns the inode associated with a socket FD, or 0 if the FD isn't
// an inode path.
//
//--------------------------------------------------------------------
uint64_t NetworkTracker::PathToInode(const char *path)
{
    if (path == NULL) {
        fprintf(stderr, "NetworkTracker::PathToInode invalid params\n");
        return 0;
    }

    if (strncmp(path, INODE_SOCK_PRE1, strlen(INODE_SOCK_PRE1)) == 0) {
        return atoi(path + strlen(INODE_SOCK_PRE1));
    } else if (strncmp(path, INODE_SOCK_PRE2, strlen(INODE_SOCK_PRE2)) == 0) {
        return atoi(path + strlen(INODE_SOCK_PRE2));
    }
    return 0;
}

//--------------------------------------------------------------------
//
// InodeToAddr
//
// Looks up a socket inode in the UDP and UDP6 tables in /proc.
//
// Returns true on success, othewise false.
//
//--------------------------------------------------------------------
bool NetworkTracker::InodeToAddr(PacketAddresses *p, bool IPv4, uint64_t inodeIn)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::InodeToAddr invalid params\n");
        return false;
    }

    FILE *fp = NULL;
    char *line = NULL;
    size_t len = 0;
    int numTokens = 0;
    char lAddrText[33];
    char rAddrText[33];
    uint32_t sl, st, tr, uid, timeout;
    uint64_t txQueue, rxQueue, tmWhen, retrnsmt, inode;
    char file[PATH_MAX];

    if (IPv4) {
        snprintf(file, PATH_MAX, "%s", PROC_UDP_FNAME);
    } else {
        snprintf(file, PATH_MAX, "%s", PROC_UDP6_FNAME);
    }

    memset(p->localAddr, 0, sizeof(p->localAddr));
    memset(p->remoteAddr, 0, sizeof(p->remoteAddr));

    fp = fopen(file, "r");
    if (fp == NULL)
        return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    getline(&line, &len, fp); // skip first line
#pragma GCC diagnostic pop

    while ((getline(&line, &len, fp)) >= 0) {
        numTokens = sscanf(line, "%u: %32[0-9A-Fa-f]:%hX %32[0-9A-Fa-f]:%hX %X %lX:%lX %X:%lX %lX %u %u %lu %*s\n",
                &sl, lAddrText, &p->localPort, rAddrText, &p->remotePort, &st, &txQueue, &rxQueue, &tr, &tmWhen,
                &retrnsmt, &uid, &timeout, &inode);
        if (numTokens < 14 || inode != inodeIn)
            continue;
        if (strlen(lAddrText) == 8) {
            // IPv4
            p->IPv4 = true;
            *(uint32_t *)p->localAddr = strtoul(lAddrText, NULL, 16);
            *(uint32_t *)p->remoteAddr = strtoul(rAddrText, NULL, 16);
            free(line);
            fclose(fp);
            return true;
        }
        if (strlen(lAddrText) == 32) {
            // IPv6
            p->IPv4 = false;
            sscanf(lAddrText, "%08X%08X%08X%08X", (uint32_t *)p->localAddr, (uint32_t *)(p->localAddr + 4),
                    (uint32_t *)(p->localAddr + 8), (uint32_t *)(p->localAddr + 12));
            sscanf(rAddrText, "%08X%08X%08X%08X", (uint32_t *)p->remoteAddr, (uint32_t *)(p->remoteAddr + 4),
                    (uint32_t *)(p->remoteAddr + 8), (uint32_t *)(p->remoteAddr + 12));
            free(line);
            fclose(fp);
            return true;
        }
    }
    free(line);
    fclose(fp);
    return false;
}

//--------------------------------------------------------------------
//
// GetUdp
//
// Maps a PID and FD to a UDP connection.
//
// Returns true on success, otherwise false.
//
//--------------------------------------------------------------------
bool NetworkTracker::GetUdp(PacketAddresses *p, pid_t pid, int fd)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::GetUdp invalid params\n");
        return false;
    }

    char fdPath[PATH_MAX];
    char fdSocket[PATH_MAX];
    uint64_t inode = 0;

    snprintf(fdPath, sizeof(fdPath), "/proc/%d/fd/%d", pid, fd);
    if (readlink(fdPath, fdSocket, sizeof(fdSocket)) == -1)
        return false;

    inode = PathToInode(fdSocket);
    if (inode == 0)
        return false;

    if (InodeToAddr(p, true, inode))
        return true;

    if (InodeToAddr(p, false, inode))
        return true;

    return false;
}

/*
  These functions are called when the EBPF program sees a socket read or write.
  The EBPF program should have logged it together with the current time; if it
  hasn't then the EBPF hash is full and needs rapid purging, and then the
  recent observation needs storing.
  Otherwise (the usual case), we need to check if our existing entry is old
  enough to qualify sending this one as an event - it is possible the last
  observation was purged from the EBPF hash and therefore this observation has
  occurred within the cool-off window.
  If so, we need to update our local version with the new time. Regardless
  we need to purge aged events from both hashes.

  The EBPF recv hash maps PID|FD to time, and is used to limit the observations
  sent to userland.
  The udpRecvTrackerTimes hash maps time to PID|FD, and is used to purge old
  observations (ordered map).
  The udpRecvTrackerPidsFd hash maps PID to FD to time, and is used to remove
  entries in the other hashes when processes terminate (map).
  An EBPF program attahed to close() deletes entries from the EBPF recv hash
  as sockets are closed.

  The send hashes work a similar way, with the EBPF send hash mapping a
  packetAddr (struct) to time, the udpSendTrackerTimes mapping times to
  packetAddrs, and the udpSendTrackerPidsAddr mapping PID to PacketAddresses to
  time.

  Returns whether to send event - because we haven't seen this one recently.
*/

//--------------------------------------------------------------------
//
// SeenUdp - inbound read/recv/recvmsg/recvmmsg
//
// Checks if an inbound UDP connection should be reported.
//
// Returns true if it should be reported, otherwise false.
//
//--------------------------------------------------------------------
bool NetworkTracker::SeenUdp(PacketAddresses *p, pid_t pid, int fd)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::SeenUdp invalid params\n");
        return false;
    }

    LARGE_INTEGER curTime;
    LONGLONG lastTime;
    uint64_t pidFd = ((uint64_t)pid << 32) | fd;

    GetSystemTimeAsLargeInteger(&curTime);

    if (!GetUdp(p, pid, fd)) {
        // failed to look up in /proc
        return false;
    }

    // get the most recent sent time from the kernel
    if (telemetryMapLookupElem(mapFds[UDP_PIDFD_HASH], &pidFd, &lastTime) < 0) {
        // kernel has sent this entry but it's not in the hash. This means
        // the hash is full. So rapidly empty a significant proportion of it
        // to make space for additional observations.
        auto it = udpRecvTrackerTimes.cbegin();
        for (unsigned int i=0; i<UDP_HASH_SIZE * UDP_HASH_RPP && it != udpRecvTrackerTimes.cend(); i++) {
            telemetryMapDeleteElem(mapFds[UDP_PIDFD_HASH], &it->second);
            it++;
        }

        // insert the one we have just received

        // find the last time we sent it
        lastTime = FindOrEraseUdpPidFd(pid, fd, p, false);
        if (lastTime > 0) {
            // copy to kernel table
            telemetryMapUpdateElem(mapFds[UDP_PIDFD_HASH], &pidFd, &lastTime, MAP_UPDATE_CREATE_OR_OVERWRITE);
        } else {
            // insert new element
            telemetryMapUpdateElem(mapFds[UDP_PIDFD_HASH], &pidFd, &curTime.QuadPart, MAP_UPDATE_CREATE_OR_OVERWRITE);
            UpdateUdpPidFd(pid, fd, curTime.QuadPart, p);
            udpRecvTrackerTimes.emplace(curTime.QuadPart, pidFd);
        }
        PurgeUdp(curTime.QuadPart);
        return (curTime.QuadPart - lastTime > UDP_REPORT_INTERVAL);
    }

    LONGLONG prevTime = FindOrEraseUdpPidFd(pid, fd, p, false);
    if (prevTime > 0) {
        if (curTime.QuadPart - prevTime > UDP_REPORT_INTERVAL) {
            // not sent this entry for long enough - update records and send event
            telemetryMapUpdateElem(mapFds[UDP_PIDFD_HASH], &pidFd, &curTime.QuadPart, MAP_UPDATE_CREATE_OR_OVERWRITE);
            lastTime = UpdateUdpPidFd(pid, fd, curTime.QuadPart, p);
            udpRecvTrackerTimes.erase(lastTime);
            udpRecvTrackerTimes.emplace(curTime.QuadPart, pidFd);
            PurgeUdp(curTime.QuadPart);
            return true;
        } else {
            // our local entry is older than the kernel one - update it
            telemetryMapUpdateElem(mapFds[UDP_PIDFD_HASH], &pidFd, &prevTime, MAP_UPDATE_CREATE_OR_OVERWRITE);
            PurgeUdp(curTime.QuadPart);
            return false;
        }
    } else {
        // we don't have an existing entry
        UpdateUdpPidFd(pid, fd, lastTime, p);
        udpRecvTrackerTimes.emplace(lastTime, pidFd);
        PurgeUdp(curTime.QuadPart);
        return true;
    }
}

//--------------------------------------------------------------------
//
// SeenUdp - outbound packet capture
//
// Checks if an outbound UDP connection should be reported.
//
// Returns true if it should be reported, otherwise false.
//
//--------------------------------------------------------------------
bool NetworkTracker::SeenUdp(const PacketAddresses *p, pid_t pid)
{
    if (p == NULL) {
        fprintf(stderr, "NetworkTracker::SeenUdp invalid params\n");
        return false;
    }

    LARGE_INTEGER curTime;
    LONGLONG lastTime;
    packetAddrs pa;

    pa.IPv4 = p->IPv4;
    pa.srcPort = p->localPort;
    pa.dstPort = p->remotePort;
    memcpy(pa.srcAddr, p->localAddr, sizeof(pa.srcAddr));
    memcpy(pa.dstAddr, p->remoteAddr, sizeof(pa.dstAddr));

    GetSystemTimeAsLargeInteger(&curTime);

    // get the most recent sent time from the kernel
    if (telemetryMapLookupElem(mapFds[UDP_ADDRS_HASH], &pa, &lastTime) < 0) {
        // kernel has sent this entry but it's not in the hash. This means
        // the hash is full. So rapidly empty a significant proportion of it
        // to make space for additional observations.
        auto it = udpSendTrackerTimes.cbegin();
        for (unsigned int i=0; i<UDP_HASH_SIZE * UDP_HASH_RPP && it != udpSendTrackerTimes.cend(); i++) {
            telemetryMapDeleteElem(mapFds[UDP_PIDFD_HASH], &it->second);
            it++;
        }

        // insert the one we have just received

        // find the last time we sent it
        lastTime = FindOrEraseUdpPidAddr(pid, p, false);
        if (lastTime > 0) {
            // copy to kernel table
            telemetryMapUpdateElem(mapFds[UDP_ADDRS_HASH], &pa, &lastTime, MAP_UPDATE_CREATE_OR_OVERWRITE);
        } else {
            // insert new element
            telemetryMapUpdateElem(mapFds[UDP_ADDRS_HASH], &pa, &curTime.QuadPart, MAP_UPDATE_CREATE_OR_OVERWRITE);
            UpdateUdpPidAddr(pid, curTime.QuadPart, p);
            udpSendTrackerTimes.emplace(curTime.QuadPart, pa);
        }
        PurgeUdp(curTime.QuadPart);
        return (curTime.QuadPart - lastTime > UDP_REPORT_INTERVAL);
    }

    LONGLONG prevTime = FindOrEraseUdpPidAddr(pid, p, false);
    if (prevTime > 0) {
        if (curTime.QuadPart - prevTime > UDP_REPORT_INTERVAL) {
            // not sent this entry for long enough - update records and send event
            telemetryMapUpdateElem(mapFds[UDP_ADDRS_HASH], &pa, &curTime.QuadPart, MAP_UPDATE_CREATE_OR_OVERWRITE);
            lastTime = UpdateUdpPidAddr(pid, curTime.QuadPart, p);
            udpSendTrackerTimes.erase(lastTime);
            udpSendTrackerTimes.emplace(curTime.QuadPart, pa);
            PurgeUdp(curTime.QuadPart);
            return true;
        } else {
            // our local entry is older than the kernel one - update it
            telemetryMapUpdateElem(mapFds[UDP_ADDRS_HASH], &pa, &prevTime, MAP_UPDATE_CREATE_OR_OVERWRITE);
            PurgeUdp(curTime.QuadPart);
            return false;
        }
    } else {
        // we don't have an existing entry
        UpdateUdpPidAddr(pid, lastTime, p);
        udpSendTrackerTimes.emplace(lastTime, pa);
        PurgeUdp(curTime.QuadPart);
        return true;
    }
}

//--------------------------------------------------------------------
//
// UdpProgramTermination
//
// Purges UDP connection data when a process exits.
//
//--------------------------------------------------------------------
void NetworkTracker::UdpProgramTermination(pid_t pid)
{
    auto it = udpRecvTrackerPidFds.find(pid);
    if (it == udpRecvTrackerPidFds.end())
        // no existing pid entry
        return;

    auto it2 = it->second.begin();
    while (it2 != it->second.end()) {
        LONGLONG lastTime = it2->second.first;
        auto it3 = udpRecvTrackerTimes.find(lastTime);
        if (it3 != udpRecvTrackerTimes.end()) {
            telemetryMapDeleteElem(mapFds[UDP_PIDFD_HASH], &it3->second);
        }
        udpRecvTrackerTimes.erase(lastTime);
        it2 = it->second.erase(it2);
    }

    udpRecvTrackerPidFds.erase(it);

    auto it4 = udpSendTrackerPidAddrs.find(pid);
    if (it4 == udpSendTrackerPidAddrs.end())
        // no existing pid entry
        return;

    auto it5 = it4->second.begin();
    while (it5 != it4->second.end()) {
        LONGLONG lastTime = it5->second;
        auto it6 = udpSendTrackerTimes.find(lastTime);
        if (it6 != udpSendTrackerTimes.end()) {
            telemetryMapDeleteElem(mapFds[UDP_ADDRS_HASH], &it6->second);
        }
        udpSendTrackerTimes.erase(lastTime);
        it5 = it4->second.erase(it5);
    }
}

//--------------------------------------------------------------------
//
// C wrappers for C++ methods.
//
//--------------------------------------------------------------------
extern "C" NetworkTracker *NetworkTrackerInit(LONGLONG staleSeconds, LONGLONG checkSeconds)
{
    NetworkTracker *n = new NetworkTracker(staleSeconds, checkSeconds);
    return n;
}

extern "C" pid_t NetworkTrackerSeenConnect(NetworkTracker *n, PSYSMON_EVENT_HEADER eventHdr)
{
    if (n == NULL || eventHdr == NULL) {
        fprintf(stderr, "NetworkTrackerSeenConnect invalid params\n");
        return 0;
    }

    return n->SeenConnect(eventHdr);
}

extern "C" void NetworkTrackerSeenFullAccept(NetworkTracker *n, bool IPv4, const BYTE *sourceAddr,
        unsigned short sourcePort, const BYTE *destAddr, unsigned short destPort, LONGLONG eventTime)
{
    if (n == NULL || sourceAddr == NULL || destAddr == NULL) {
        fprintf(stderr, "NetworkTrackerSeenFullAccept invalid params\n");
        return;
    }

    n->SeenAccept(AddrAndPort(sourceAddr, IPv4, sourcePort), AddrAndPort(destAddr, IPv4, destPort), eventTime);
}

extern "C" bool NetworkTrackerSeenAccept(NetworkTracker *n, bool IPv4, const BYTE *sourceAddr,
        unsigned short sourcePort, BYTE *destAddr, unsigned short *destPort)
{
    if (n == NULL || sourceAddr == NULL || destAddr == NULL) {
        fprintf(stderr, "NetworkTrackerSeenAccept invalid params\n");
        return false;
    }

    BYTE empty[16] = {0};
    AddrAndPort d(empty, IPv4, 0);

    if (n->SeenAccept(AddrAndPort(sourceAddr, IPv4, sourcePort), &d)) {
        memcpy(destAddr, d.addr, sizeof(d.addr));
        *destPort = d.port;
        return true;
    }
    return false;
}

extern "C" void NetworkTrackerCloseAccept(NetworkTracker *n, bool IPv4, const BYTE *sourceAddr,
        unsigned short sourcePort, const BYTE *destAddr, unsigned short destPort)
{
    if (n == NULL || sourceAddr == NULL || destAddr == NULL) {
        fprintf(stderr, "NetworkTrackerCloseAccept invalid params\n");
        return;
    }

    n->CloseAccept(AddrAndPort(sourceAddr, IPv4, sourcePort), AddrAndPort(destAddr, IPv4, destPort));
}

extern "C" bool NetworkTrackerSeenUdpRecv(NetworkTracker *n, bool *IPv4, BYTE *sourceAddr,
        unsigned short *sourcePort, BYTE *destAddr, unsigned short *destPort, pid_t pid, int fd)
{
    if (n == NULL || sourceAddr == NULL || sourcePort == NULL || destAddr == NULL || destPort == NULL) {
        fprintf(stderr, "NetworkTrackerSeenUdpRecv invalid params\n");
        return false;
    }

    PacketAddresses p;

    if (!n->SeenUdp(&p, pid, fd))
        return false;
    *IPv4 = p.IPv4;
    *sourcePort = p.localPort;
    *destPort = p.remotePort;
    memcpy(sourceAddr, p.localAddr, sizeof(p.localAddr));
    memcpy(destAddr, p.remoteAddr, sizeof(p.remoteAddr));
    return true;
}

extern "C" bool NetworkTrackerSeenUdpSend(NetworkTracker *n, bool IPv4, const BYTE *sourceAddr,
        unsigned short sourcePort, const BYTE *destAddr, unsigned short destPort, pid_t pid)
{
    if (n == NULL || sourceAddr == NULL || destAddr == NULL) {
        fprintf(stderr, "NetworkTrackerSeenUdpSend invalid params\n");
        return false;
    }

    PacketAddresses p(IPv4, sourceAddr, sourcePort, destAddr, destPort);

    return n->SeenUdp(&p, pid);
}

extern "C" void NetworkTrackerUdpProgramTermination(NetworkTracker *n, pid_t pid)
{
    if (n == NULL) {
        fprintf(stderr, "NetworkTrackerUdpProgramTermination invalid params\n");
        return;
    }

    n->UdpProgramTermination(pid);
}

