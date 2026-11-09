[![Build Status](https://dev.azure.com/sysinternals/Tools/_apis/build/status/Sysinternals.SysmonForLinux?repoName=Sysinternals%2FSysmonForLinux&branchName=main)](https://dev.azure.com/sysinternals/Tools/_build/latest?definitionId=340&repoName=Sysinternals%2FSysmonForLinux&branchName=main)

# Test CI Sysmon for Linux
Sysmon for Linux is a tool that monitors and logs system activity including process lifetime, network connections, file system writes, and more. Sysmon works across reboots and uses advanced filtering to help identify malicious activity as well as how intruders and malware operate on your network.
Sysmon for Linux is part of [Sysinternals](https://sysinternals.com).

![Sysmon in use](sysmon.gif "Sysmon in use")

## Installation
The packages are available in the official Microsoft Linux repositories and instructions on how to install the packages for the different Linux distributions can be found in the [Installation instructions](INSTALL.md).

This project contains the code for build and installing [Sysmon](https://docs.microsoft.com/en-us/sysinternals/downloads/sysmon) on Linux.

## Build
Please see build instructions [here](BUILD.md).

## Autodiscovery of Offsets
On systems that are BTF enabled, Sysmon will use BTF for accurate kernel offsets.
Sysmon also supports specifying standalone BTF files (using /BTF switch). There are
several ways to generate BTF files and [BTFHub](https://github.com/aquasecurity/btfhub)
has a number of standalone BTF files for different distributions/kernels.

If BTF isn't available, Sysmon attempts to automatically discover the offsets of some
members of some kernel structs. If this fails, please provide details of the kernel
version (and config if possible) plus the error message to the GitHub issues page.

You can then generate a configuration file to override the autodiscovery by
building the getOffsets module in the /opt/sysinternals/getOffsets directory.
See the README.md in that directory for more information.

## Manual Page
A man page for Sysmon can be found in the package directory, and is installed
by both deb and rpm packages.

Use 'find' on the package directory to locate it manually.

## Output
```
sudo tail -f /var/log/syslog
```
or more human-readable
```
sudo tail -f /var/log/syslog | sudo /opt/sysmon/sysmonLogView
```

SysmonLogView has options to filter the output to make it easy to identify
specific events or reduce outputted fields for brevity.

SysmonLogView is built when Sysmon is built and is installed into /opt/sysmon
when sysmon is installed.

*Important*: You may wish to modify your Syslogger config to ensure it can
handle particularly large events (e.g. >64KB, as defaults are often between 1KB
and 8KB), and/or use the FieldSizes configuration entry to limit the length of
output for some fields, such as CommandLine, Image, CurrentDirectory, etc.

Example:

Add \<FieldSizes\>CommandLine:100,Image:20\</FieldSizes\> under
\<Sysmon\> in your configuration file.

## Developer Details
See DEVELOP.md

## License
Sysmon For Linux is licensed under MIT, with the eBPF programs licensed under
GPL2.  SysinternalsEBPF (on which Sysmon For Linux depends) is licensed under
LGPL2.1, with the eBPF code library licensed under GPL2.

