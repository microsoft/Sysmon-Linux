# Sysmon For Linux Developer Details

## Introduction

Sysmon For Linux is a port of the Windows Sysmon tool, with the driver
replaced by eBPF programs.  This document describes developer details to
assist people who wish to modify or extend it.

## Build environment

The build directory, created as part of the initial compilation, contains all
the Makefiles needed to recompile Sysmon For Linux.

If you are refactoring between the main program and the library, refactoring
eBPF programs, or modifying the CMakeLists.txt files, then you will probably
benefit from removing the build directory and recreating it, to ensure that
cmake properly updates its cache.
```
rm -rf build
mkdir build
cd build
cmake ..
make
```

## Debugging

### Pending TCP accepts

The network tracker retains at most 65,536 pending accept transitions and their
timestamp records. At capacity, a new source evicts the oldest event, so unusually
delayed accepts may lose their network-event correlation during saturation.
The first eviction emits a diagnostic to stderr (once per tracker lifetime).
Replacing an existing source refreshes its record without evicting another source.
Equal timestamps are tracked independently. Stale records are purged on traffic
using the existing 15-minute lifetime and one-minute check interval; continuous
traffic does not postpone the next check.

Run the focused regressions from the configured Linux build directory:
```
cmake --build . --target sysmonUnitTests
./sysmonUnitTests --gtest_filter=NetworkTrackerTest.*
```
These tests exercise the bound without generating network traffic. They also
cover duplicate timestamps, source replacement, expiry, and match/close cleanup.
For a manual compatibility check on an isolated Linux VM, enable NetworkConnect
logging without excluding loopback, start
`python3 -m http.server 18080 --bind 127.0.0.1`, and make ten requests:
```
for i in $(seq 1 10); do
    curl --noproxy '*' --max-time 3 -fsS http://127.0.0.1:18080/ -o /dev/null
done
```
Verify inbound Event ID 3 records have the Python server PID, `Initiated=false`,
and destination port 18080. Stop the server with Ctrl-C. Repeat with the server
bound to `::1` on port 18081 and URL `http://[::1]:18081/` for IPv6.
Use the synthetic regressions, not a live network flood, to check saturation.

### Attaching a debugger

Both the main program and the libary can be set to build with symbols by
setting the CMAKE\_BUILD\_TYPE to Debug in the CMakeLists.txt files.  You may
need to remove the build directory and rebuild from scratch to be sure that
everything is rebuilt with symbols (or without).
```
set(CMAKE_BUILD_TYPE Debug)
```
The eBPF programs can be configured to enable BPF\_PRINTK() by setting the
DEBUG\_K option on in the CMakeLists.txt files.  Messages outputted with
BPF\_PRINTK() are sent to /sys/kernel/debug/tracing/trace\_pipe.  Cat this
pseduo-file to monitor the output.  When DEBUG\_K is set to Off, BPF\_PRINTK()
becomes a NOP.  As before, you may need to remove the build directory and
rebuild from scratch.
```
option(DEBUG_K "Enter debug mode" On)
```

Having built with symbols, use gdb to debug the program.  Easiest is to attach
to a running instance:
```
ps -ef | grep sysmon
sudo gdb ./sysmon -pid <Sysmon PID>
```

If, instead, you want to debug program start-up issues, such as BPF loading,
offsets loading, searching or discovering, or configuration setting, then you
will need to make gdb launch Sysmon For Linux and coach it through to the
place you want to break and/or step.
```
sudo gdb ./sysmon
```
Set a breakpoint on the line in main() that is "pid = fork()".  This line sets
it up to be a daemon so we need to follow the child process here (as the
parent will quit once the child has signalled that it has finished starting
up.  For reasons unknown, gdb either fails to follow the child, or fails to
trigger the breakpoint, when continuing through the fork. Instead break on the
fork itself and 'next' through it (which correctly follows the child).
```
b sysmonforlinux.c:1329
    (or whatever line is "pid = fork()")
set follow-fork-mode child
r -i /opt/sysmon/config.xml -service
    (will run sysmon with the current config file, but will not trigger
     systemd to restart sysmon)
n
    (will step to next source code line)
```
Repeat the 'n' command until gdb reaches the startEBPF line, then set the
follow mode back to parent - otherwise if it goes into offsets discovery, it
will launch a temporary child and gdb will follow it instead of staying with
the main program.
```
set follow-fork-mode parent
```
Now you can set a breakpoint where you need, and 'continue' ('c') until it
hits it.

## Debugging eBPF Programs

The easiest way to debug eBPF programs is 'printf debugging', except as the
programs run in the kernel, we actually need 'printk debugging' instead. First
make sure the environment is set to enable DEBUG\_K (see above), then simply
insert BPF\_PRINTK() calls where necessary.
```
{BPF_PRINTK(format, args [,args ...]);}
```
Note that these calls are inside {} curly braces as BPF\_PRINTK is a macro that
converts the format string into a local character array, supplying that and
its size to the underlying bpf\_trace\_printk() helper, along with the arguments.
As BPF helpers are limited to 5 arguments total, you can therefore only supply
3 optional arguments after the format string.  Note the limitations on
conversion specifiers in the bpf\_helpers man page; additionally note that %p
only prints 32 bits on older kernels, so use %lx instead.

Be aware that you can grep the output of 
```
cat /sys/kernel/debug/tracing/trace_pipe
```
to limit the volume of information if your program gets triggered by multiple
processes.

Additionally, it is possible to debug eBPF programs with gdb by running the
kernel inside a VM, and attaching gdb to it.

See:
https://ebpf.io/summit-2020-slides/eBPF_Summit_2020-Lightning-Lorenzo_Fontana-Debugging_the_BPF_Virtual_Machine.pdf
for an overview.

## Project Layout

### Sysmon For Linux

Sysmon For Linux relies on the libsysinternalsEBPF.so shared library. Its main
function parses the command line with Sysmon functions ported from, and shared
with, the Windows version.  Starting it up with the '-c' switch causes it to
store its command line in /opt/sysmon/arg{c,v} and trigger the running Sysmon
For Linux to cause it to drop its current config, load this new command line,
and parse it to build the new config.

Starting it with the '-i' switch causes it to stop any running copies, parse
the command line for validity and then start up depending on whether the
'-service' switch is present or not. A user would not typically supply this
switch (unless they are debugging, see above), the absence of which indicates
that Sysmon For Linux was started from the command line.

In this situation, the program will store the command line in /opt/sysmon as
for configuration changes, and copy the configuration file (if supplied) to
/opt/sysmon/config.xml. It will then exec the shell command 'systemctl start
sysmon', essentially quitting as it does so.  This causes systemd to start it
up as a system daemon, with the '-service' switch.

The command line arguments will be reloaded from /opt/sysmon, with the config
filename replaced with /opt/sysmon/config.xml, and the original returned
separately for reporting in a config change event.  This allows Sysmon For
Linux to be launched from the command line with '-i' and for the whole config
to be stored, and reloaded, when the command line version instructs systemd to
start it up again as a standard system daemon.

The main function then creates a sysinternalsEBPF configuration that specifies
the eBPF programs to be used for the different kernels, and for the different
types of attachment (syscall tracepoints, syscall raw tracepoints, non-syscall
tracepoints). It also specifies which syscalls should be active based on the
loaded Sysmon config (with pseudo-syscalls specifying non-syscall activity),
and which programs should be active based on those syscalls.

Together this configuration informs sysinternalsEBPF what to load for the
different kernel versions it could be running on, which programs to attach,
and which events to make active. The configuration also specifies a callback
function that will handle generated events, and a callback that signifies that
the start up has completed.

SysinternalsEBPF calls the event callback every time an event is received from
the eBPF programs. Sysmon For Linux processes the event (if necessary, e.g.
adding extra information, userland additional checks, etc) and then sends it to
DispatchEvent() ported from the Windows version. Here it is processed and
filtered, and eventually reported to Syslog via FormatSyslogString() in
outputxml.c.

* installer.{c,h} - functions for installing Sysmon, and related to start up.
* linuxHelpers.{cpp,h} - functions to replicate missing Windows functions.
* linuxTypes.h - types/defines to repliate missing Windows types/defines.
* linuxWideChar.{c,h} - functions to handle UTF16 strings (Windows WCHAR).
* networkTracker.{cpp,h} - correlation engine for network events.
* outputxml.{c,h} - output message formatter.

### Sysmon For Linux ebpfKern

There are a number (currently 6) eBPF source files, made up from programs and
inline functions in 30+ source files.  The main source files are:

* sysmonEBPFkern4.15.c
* sysmonEBPFkern4.16.c
* sysmonEBPFkern4.17-5.1.c
* sysmonEBPFkern5.2.c
* sysmonEBPFkern5.3-5.5.c
* sysmonEBPFkern5.6-.c

The numbers in the filenames specify the kernel versions that they support.
They set specific defines and include specific source files. Notable source
files are:

* sysmonEBPF\_common.h - includes, defines, and map definitions.
* sysmonHelpers.c - inline functions that do useful things.
* sysmonGenericEntry\_rawtp.c - program to attach to raw\_syscalls/sys\_enter
    that stores arguments.
* sysmonGenericEntry\_tp.c - programs to attach to syscalls/sys\_enter
    tracepoints that stores arguments.

Typically, userland memory can't be guaranteed to be paged in at the entry of
a syscall; as such, generic entry programs are attached to the entry points of
syscalls (a single entry point for raw syscalls, and each entry point for
traditional syscalls).  These generic entry programs simply store the arguments
provided to the syscalls so that they can be retrieved by programs attached to
the exit points, by which time the userland memory will have been paged in.

(Side note: eBPF programs aren't permitted to 'sleep' on most kernels except
the very recent. Usually, when code attempts to access a memory location that
is not paged in, a BRK happens and the memory pager takes control, pages it in
and returns control to the point where the BRK happened so that the memory
access can be reattempted. By not being allowed to 'sleep', eBPF programs don't
benefit from this technology and all that happens is that the memory read
fails.)

The programs attached to the syscall and raw syscall exit points are made up
of three files:

* sysmonEVENT.c - the inline function that processes the associated syscall,
    and generates the Sysmon event.
* sysmonEVENT\_rawtp.c - the eBPF program that attaches to the raw syscalls exit
    tracepoint, and calls the inline function.
* sysmonEVENT\_tp.c - the eBPF program that attaches to the specific traditional
    tracepoint exit, and calls the inline function.

This approach reduces code duplication. These files can be created from the
sysmonTEMPLATE.c, sysmonTEMPLATE\_rawtp.c and sysmonTEMPLATE\_tp.c files by
running the makeEvent.sh script with the event name as parameter. Minimal edits
should be required for the rawtp and tp files, with the majority of the work
going into the sysmonEVENT.c file. Ensure to include the rawtp and tp files in
the main source files mentioned above.

In addition to syscall tracepoints, Sysmon also attaches to non-syscall
tracepoints, such as sched/sched\_process\_exit. These don't have an entry and
exit, but a tracepoint that happens immediately before the relevant code in
the kernel (akin to entry in reality). In these cases there is just the
relevant program, which can be constructed by combining the sysmonEVENT.c and
sysmonEVENT\_rawtp.c files. Current sources for non-syscall tracepoints are:

* sysmonProcTerminated.c - sched/sched\_process\_exit.
* sysmonTCPconnection\_4\_15.c - tcp/tcp\_set\_state (v4.15 only).
* sysmonTCPconnection\_4\_15\_5\_5.c - inet/inet\_sock\_set\_state (<= v5.6).
* sysmonTCPconnection\_5\_6\_.c - inet/inet\_sock\_set\_state (>= v5.6).
* sysmonUDPsend.c - skb/consume\_skb.

## So You Want To Implement An Existing Event?

If you want to implement an event that already exists in the Windows version of
Sysmon, then the process is relatively straight forward. Note that it depends
entirely on the layout specified above so maybe give that some attention if
things get complicated. If you want to implement an entirely new event then see
the next section instead.

The process is broadly:

* Identify the event you wish to add from the schema - run sysmon -s to see the
    event schema.
* Identify the associated event struct in sysmonCommon/ioctlcmd.h.
* Identify the syscall(s) or tracepoint(s) that will provide information needed
    to complete the event struct. Check /sys/kernel/debug/tracing/events for
    the event classes and the actual events within them. Cat the format file to
    see the parameters available to the tracepoints.
* Identify whether one or more pseudo-event types are needed or whether the
    existing event struct is suitable. Psuedo-event structs and EventTypes
    should be added to linuxTypes.h.
* Make the eBPF program(s) that generate the telemetry in ebpfKern. Use the
    format file from the event in /sys/kernel/debug/tracing/events to create a
    suitable input parameter struct.
* Add the eBPF program(s) to the main eBPF source files.
* Add the eBPF program(s) to the config in sysmonforlinux.c.
* Add a case to the switch in SetActiveSyscalls() in sysmonforlinux.c to
    specify which syscalls and pseudo-syscalls are required. New pseudo-
    syscalls should be added to linuxTypes.h.
* If necessary, add a corrolation engine to combine multiple tracepoint data -
    see networkTracker as an example.
* If necessary, add a case to the switch in handle\_event() in sysmonforlinux.c
    to post-process or correlate the new event. Ultimately, this new case
    should (usually) eventually pass the event to DispatchEvent().

This process can be best understood by analysing the following examples.

### Simple Example

SysmonFileDelete is a good example of an existing event that only required a
simple solution; it attaches to the unlink() syscall and fills in a
SYSMON\_FILE\_DELETE struct. No pseudo-events or pseudo-syscalls are
necessary. In the eBPF config in sysmonforlinux.c the programs are added to
the unlink() syscall, and the File Delete event is set in SetActiveSyscalls()
in sysmonforlinux.c to require this syscall. No correlation engine in required,
nor any post processing. In handle\_event() in sysmonforlinux.c, the event
simply hits the Default case and is dispatched directly to DispatchEvent().

### Slightly More Complex Example

A slightly more complex example would be ProcessAccessed. Like File Delete, it
only relies on real syscalls - ptrace() in this case - so the mechcanics match
mostly to the previous case. Where it differs, however, is in needing some
post-processing to add the image file of the target process (which couldn't be
captured in the syscall).

The event struct received from eBPF (provided to handle\_event()) should be
treated as read-only. In order to add information to it, a new one should be
created on the stack and the data copied in. (Stack is used as it is faster
than malloc() and avoids potential memory leaks.)

### Much More Complex Example

More complex examples are the network events. These require multiple
tracepoints and/or information arriving split over multiple reports, and hence
a correlation engine (networkTracker). Because the information sent from eBPF
doesn't match a Sysmon struct, a new pseudo-event struct, pseudo-event type,
and pseudo-syscall were created. These are connected up in the config and
SetActiveSyscalls() in sysmonforlinux.c.

The handle\_event() function processes the pseudo-events by adding or
retrieving information from the networkTracker correlation engine. When
sufficient information has been gathered, an event is generated by calling
CreateNetworkEvent() instead of DispatchEvent() as network events are handled
differently in Sysmon.

## So You Want To Implement An Entirely New Event?

To implement an entirely new event that doesn't already exist in the Windows
version would involve modifying the schema to document the new event. This
process requires a planned enhancement where the schema will contain an
additional switch to indicate whether an event is for Windows, Linux, or both.

The process outlined for implementing an existing event can then be followed,
making the assumption that a pseudo-event struct and pseudo-event type will
certainly be required as suitable ones will not already exist. In addition,
extra code will be required in DispatchEvent() in order to handle it
appropriately. This process will be documented in an updated version of this
file when the planned enhancement to the schema has been implemented.

## Writing Good eBPF Programs

All new eBPF programs should follow the format and approach found in the
existing ones - these are written in a consistent manner that meets the
requirements of the eBPF verifier. The following tips are offered to help write
eBPF programs.

### Coding Tips

* eBPF code is heavily optimised during compilation so write simpler, more
    understandable code and let the compiler make it optimal.
* Use the helpers in ebpfKern and in sysinternalsEBPF/ebpfKern. These have
    been tested fairly well and are believed to work correctly.
* If you need functions to make your code more readable, inline them all. (See
    existing code for examples.)
* Bound all loops with an upper bound. e.g. Use 'for' loops with simple exit
    conditions ("i < N" where N is a compile-time static value). To accommodate
    kernels <=5.2 were loops are not permitted, add the following construction
    directly above each loop:
```
#ifdef NOLOOPS
    #pragma unroll
#endif
```
* Bound all array access with an upper bound. There are many ways to achieve
    this, notably with 'if' statements preceding the access, but the simplest
    and most reliable method is to make array sizes equal to a power of 2,
    and then bound the index with "& (SIZE - 1)". e.g. "x = a[i & (SIZE - 1)];"
    so that the index is always constrained between 0 and SIZE-1, even if the
    index variable (i in this case) could take arbitrary values. Expanding an
    array to a size that is a power of 2 wastes memory at the expense of easier
    and more reliable compiled code (across clang verions and kernel versions).
* For syscalls, store arguments on entry and retrieve them on exit, using a
    hash based on PID/TID. Exit programs should be able to access userland
    memory buffers referenced by arguments (as memory has been recently
    accessed), but the same guarantee cannot be made for entry programs.
* memset all structs you intend to store in maps to 0 before use to ensure that
    every memory location in the struct (including any padding gaps) has been
    initialised prior to attempting to store it.
* eBPF maps can be accessed from userland - move all heavy data structure
    management to userland, especially where it requires loops.

### Debugging Tips

* If programs fail to load due to a verifier error, it can be helpful to
    compare the assembler of the eBPF object against the C source. Dump the
    assembler using:
```
% llvm-objdump -source <EBPF OBJECT>
```
* eBPF helpers are called by number in eBPF assember - "call #4" would be the
    assembler for "bpf_probe_read()" in C. Find bpf helper call numbers in
    bpf_helper_defs.h. It's possible to make a reference list with:
```
cat bpf_helper_defs.h | grep -e '^static' | sed -e 's/^[^(]*(\*\([^)]*\)).*)\([^();]*\);$/\2 \1/'
```
* Match eBPF assembler to C source by aligning on bpf helper calls ("call #n")
    and constant values, e.g. PATH_MAX or an array size.
* eBPF verfifier errors can be difficult to understand but most errors have
    already been posted to online forums and the answers or help may provide
    good clues.

### Inline Assembler

If clang optimises away the array bound "& (SIZE - 1)" or some other code
required to satisfy the veriifer, or innocently stores the bounded value on
the stack and then 'forgets' it was bounded after retrieval, then it may be
possible to replace the C code with inline assembler, swapping C statements
1-for-1 with assembler statements.

First break the C statement into a series of very simple C statements, e.g.
```
x = a[i & (SIZE - 1)]
```
could become:
```
uint32_t index = i;
index &= (SIZE - 1);
x = a[index];
```
Next, replace the bounding statement with inline assembler so:
```
index &= (SIZE - 1);
```
becomes:
```
asm volatile("%[index] &= " XSTR(SIZE - 1) "\n"
    :[index]"+&r"(index)
    );
```
The 'volatile' keyword prevents the compiler from optimising the assembler
instructions to a different location. The XSTR macro permits the use of
defined values and expressions in asm statements - it is defined in
sysinternalsEBPF\_helpers.c.

