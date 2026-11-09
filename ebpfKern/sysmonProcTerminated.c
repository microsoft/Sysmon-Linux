/*
    SysmonForLinux

    Copyright (c) Microsoft Corporation

    All rights reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//====================================================================
//
// sysmonProcTerminated.c
//
// Report process termination events.
//
//====================================================================

#include "sysmonEBPF_common.h"
#include <sysinternalsEBPF_helpers.c>
#include "sysmonHelpers.c"

struct tracepoint__sched__sched_process_exit {
    __uint64_t pad;
    char       comm[16];
    pid_t      pid;
    int        prio;
};

// process terminated
SEC("tracepoint/sched/sched_process_exit")
__attribute__((flatten))
int ProcTerminated(struct tracepoint__sched__sched_process_exit *args)
{
    uint64_t pidTid;
    uint32_t cpuId = bpf_get_smp_processor_id();
    PSYSMON_EVENT_HEADER eventHdr = NULL;
    const ebpfConfig *config;
    char *ptr = NULL;
    const void *task = NULL;
    const void *cred = NULL;

    if (!getConfig(&config, &pidTid))
        return 0;

    if (!getEventHdr(&eventHdr, cpuId))
        return 0;

    if (!config->active[__NR_PROCTERM])
        return 0;

    // initialise event
    eventHdr->m_FieldFiltered = 0;
    eventHdr->m_PreFiltered = 0;
    eventHdr->m_SequenceNumber = 0;
    eventHdr->m_SessionId = 0;

    eventHdr->m_EventType = ProcessTerminate;
    PSYSMON_PROCESS_TERMINATE event = (PSYSMON_PROCESS_TERMINATE)&eventHdr->m_EventBody.m_ProcessTerminateEvent;

    event->m_ProcessId = pidTid >> 32;
    event->m_EventTime.QuadPart = (bpf_ktime_get_ns() + config->bootNsSinceEpoch) / 100;

    ptr = (char *)(event + 1);
    memset(event->m_Extensions, 0, sizeof(event->m_Extensions));

    // get the task struct
    task = (const void *)bpf_get_current_task();

    // Insert the UID as the SID
    if (task) {
        *(uint64_t *)ptr = getUid((struct task_struct*) task, config) & 0xFFFFFFFF;
        if(*(uint64_t *)ptr!=0)
        {
            event->m_Extensions[PT_Sid] = sizeof(uint64_t);
            ptr += sizeof(uint64_t);
        }
    }

    eventHdr->m_EventSize = (uint32_t)((void *)ptr - (void *)eventHdr);
    checkAndSendEvent((void *)args, eventHdr, config);

    return 0;
}


