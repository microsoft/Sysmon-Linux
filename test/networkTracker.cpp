/*
    SysmonForLinux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    Licensed under the MIT License.
*/

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <limits.h>
#include "networkTracker.h"

// Accept tracking does not access the eBPF maps.
int mapFds[] = {-1, -1};

class NetworkTrackerTest : public ::testing::Test
{
protected:
    static constexpr LONGLONG SECOND = 10000000;
    NetworkTracker tracker{900, 60};
    LONGLONG now = tracker.timeLastChecked.QuadPart;

    static AddrAndPort Address(size_t id, bool ipv4 = true)
    {
        BYTE addr[16] = {};
        addr[0] = ipv4 ? 127 : 0xfd;
        size_t offset = ipv4 ? 1 : 13;
        addr[offset] = (id >> 16) & 0xff;
        addr[offset + 1] = (id >> 8) & 0xff;
        addr[offset + 2] = id & 0xff;
        return AddrAndPort(addr, ipv4, 12345);
    }

    static size_t Capacity()
    {
        return NetworkTracker::MAX_ACCEPT_ENTRIES;
    }

    void ExpectSize(size_t size)
    {
        EXPECT_EQ(size, tracker.acceptTracker.size());
        EXPECT_EQ(size, tracker.acceptTrackerTimes.size());
    }

    void ExpectIndexConsistent()
    {
        for (auto it = tracker.acceptTrackerTimes.begin(); it != tracker.acceptTrackerTimes.end(); ++it) {
            auto entry = tracker.acceptTracker.find(it->second);
            ASSERT_NE(tracker.acceptTracker.end(), entry);
            EXPECT_EQ(it, entry->second.second);
        }
    }

    void MakePurgeDue()
    {
        tracker.timeLastChecked.QuadPart = now - 61 * SECOND;
    }

    void ExpectCheckTime(LONGLONG expected)
    {
        EXPECT_EQ(expected, tracker.timeLastChecked.QuadPart);
    }
};

TEST_F(NetworkTrackerTest, MatchesAndClosesIPv4AndIPv6)
{
    for (bool ipv4 : {true, false}) {
        auto source = Address(1, ipv4);
        auto dest = Address(2, ipv4);
        auto result = Address(0, ipv4);
        EXPECT_FALSE(tracker.SeenAccept(source, &result));
        tracker.SeenAccept(source, dest, now);
        tracker.CloseAccept(source, Address(3, ipv4));
        ExpectSize(1);
        ASSERT_TRUE(tracker.SeenAccept(source, &result));
        EXPECT_TRUE(dest == result);
        EXPECT_FALSE(tracker.SeenAccept(source, &result));
        ExpectSize(0);
        tracker.SeenAccept(source, dest, now);
        tracker.CloseAccept(source, dest);
        ExpectSize(0);
        EXPECT_FALSE(tracker.SeenAccept(source, &result));
    }
}

TEST_F(NetworkTrackerTest, EqualTimestampsHaveIndependentLifetimes)
{
    auto dest = Address(100);
    auto result = Address(0);
    for (size_t i = 1; i <= 4; ++i) {
        tracker.SeenAccept(Address(i), dest, now - 901 * SECOND);
    }
    ExpectSize(4);
    ExpectIndexConsistent();
    ASSERT_TRUE(tracker.SeenAccept(Address(1), &result));
    tracker.CloseAccept(Address(2), dest);
    ExpectSize(2);
    ExpectIndexConsistent();
    MakePurgeDue();
    tracker.SeenAccept(Address(5), dest, now);
    ExpectSize(1);
    EXPECT_FALSE(tracker.SeenAccept(Address(3), &result));
    EXPECT_FALSE(tracker.SeenAccept(Address(4), &result));
    EXPECT_TRUE(tracker.SeenAccept(Address(5), &result));
    ExpectSize(0);
}

TEST_F(NetworkTrackerTest, ReplacingSourceRemovesOldTimestamp)
{
    auto source = Address(1);
    auto dest = Address(2);
    for (size_t i = 0; i < 4096; ++i) {
        tracker.SeenAccept(source, dest, now - 901 * SECOND + i);
        ExpectSize(1);
    }
    auto replacement = Address(3);
    tracker.SeenAccept(source, replacement, now);
    ExpectSize(1);
    ExpectIndexConsistent();
    MakePurgeDue();
    tracker.SeenAccept(Address(4), dest, now);
    ExpectSize(2);
    auto result = Address(0);
    ASSERT_TRUE(tracker.SeenAccept(source, &result));
    EXPECT_TRUE(replacement == result);
    ExpectIndexConsistent();
}

TEST_F(NetworkTrackerTest, BoundsBothIndexesWithEqualTimestamps)
{
    EXPECT_EQ(65536u, Capacity());
    auto dest = Address(0);
    const size_t count = 2 * Capacity() + 7;
    for (size_t i = 0; i < count; ++i) {
        tracker.SeenAccept(Address(i, i % 2 == 0), dest, now);
        ExpectSize(std::min(i + 1, Capacity()));
    }
    ExpectIndexConsistent();
    auto result = Address(0);
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(i >= count - Capacity(), tracker.SeenAccept(Address(i, i % 2 == 0), &result));
    }
    ExpectSize(0);
}

TEST_F(NetworkTrackerTest, EvictsOldestEventRatherThanInsertionOrder)
{
    auto dest = Address(0);
    for (size_t i = 0; i < Capacity(); ++i) {
        tracker.SeenAccept(Address(i), dest, now + Capacity() - i);
    }
    tracker.SeenAccept(Address(Capacity()), dest, now + Capacity() + 1);
    ExpectSize(Capacity());
    ExpectIndexConsistent();
    auto result = Address(0);
    EXPECT_FALSE(tracker.SeenAccept(Address(Capacity() - 1), &result));
    EXPECT_TRUE(tracker.SeenAccept(Address(0), &result));
    EXPECT_TRUE(tracker.SeenAccept(Address(Capacity()), &result));
}

TEST_F(NetworkTrackerTest, RefreshAtCapacityDoesNotEvictAnotherSource)
{
    auto dest = Address(0);
    for (size_t i = 0; i < Capacity(); ++i) {
        tracker.SeenAccept(Address(i), dest, now + i);
    }
    auto replacement = Address(Capacity() + 1);
    tracker.SeenAccept(Address(0), replacement, now + Capacity());
    ExpectSize(Capacity());
    ExpectIndexConsistent();
    tracker.SeenAccept(Address(Capacity()), dest, now + Capacity() + 1);
    ExpectSize(Capacity());
    auto result = Address(0);
    EXPECT_FALSE(tracker.SeenAccept(Address(1), &result));
    ASSERT_TRUE(tracker.SeenAccept(Address(0), &result));
    EXPECT_TRUE(replacement == result);
}

TEST_F(NetworkTrackerTest, TrafficDoesNotPostponeScheduledPurge)
{
    auto dest = Address(0);
    tracker.SeenAccept(Address(1), dest, now - 901 * SECOND);
    for (size_t i = 0; i < 100; ++i) {
        tracker.SeenAccept(Address(2), dest, now);
        ExpectCheckTime(now);
    }
    MakePurgeDue();
    tracker.SeenAccept(Address(3), dest, now);
    ExpectSize(2);
    ExpectIndexConsistent();
    auto result = Address(0);
    EXPECT_FALSE(tracker.SeenAccept(Address(1), &result));
    EXPECT_TRUE(tracker.SeenAccept(Address(2), &result));
    EXPECT_TRUE(tracker.SeenAccept(Address(3), &result));
}
