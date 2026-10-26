# Build
Please see the history of this file for instructions for older, unsupported versions.

## Prerequisites
- SysinternalsEBPF being installed:
library `libsysinternalsEBPF.so`, header `libsysinternalsEBPF.h`, plus
resource files in `/opt/sysinternalsEBPF`. These can be installed from
the
[SysinternalsEBPF](https://github.com/Microsoft/SysinternalsEBPF)
project or via the `sysinternalsebpf` DEB package from the
_packages.microsoft.com_ repository (see [INSTALL.md](INSTALL.md)).
If you installed SysinternalsEBPF via make install, you may need to add /usr/local/lib to the loader library path (LD_LIBRARY_PATH).

- .NET SDK. Please see [.NET Installation](https://learn.microsoft.com/en-us/dotnet/core/install/linux). Note: If during installation you encounter "A fatal error occurred. The folder [/usr/share/dotnet/host/fxr] does not exist", please see [here](https://stackoverflow.com/questions/73753672/a-fatal-error-occurred-the-folder-usr-share-dotnet-host-fxr-does-not-exist)

- clang/llvm v10+

### Azure Linux
```
sudo dnf update
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo dnf install gcc gcc-c++ make cmake llvm clang elfutils-libelf-devel rpm-build json-glib-devel python3 libxml2-devel gtest-devel gmock gmock-devel openssl-devel perl
```

### Ubuntu 20.04+
```
sudo apt update
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo apt -y install build-essential gcc g++ make cmake libelf-dev llvm clang libxml2 libxml2-dev libzstd1 git libgtest-dev apt-transport-https dirmngr googletest google-mock libgmock-dev  libjson-glib-dev libssl-dev
```

### Rocky 9+
```
sudo dnf install dnf-plugins-core
sudo dnf config-manager --set-enabled crb
sudo dnf install epel-release

sudo dnf update
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo yum install gcc gcc-c++ make cmake llvm clang elfutils-libelf-devel rpm-build json-glib-devel python3 libxml2-devel gtest-devel gmock gmock-devel openssl-devel perl
```

### Rocky 8
```
sudo dnf install dnf-plugins-core
sudo dnf install epel-release
sudo dnf config-manager --set-enabled powertools

sudo dnf update
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo yum install gcc gcc-c++ make cmake llvm clang elfutils-libelf-devel rpm-build json-glib-devel python3 libxml2-devel gtest-devel gmock gmock-devel openssl-devel
```

### Fedora
```
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo yum install gcc gcc-c++ make cmake llvm clang elfutils-libelf-devel rpm-build json-glib-devel python3 libxml2-devel gtest-devel gmock gmock-devel openssl-devel perl
```

### Debian
```
wget https://packages.microsoft.com/config/debian/11/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
rm packages-microsoft-prod.deb
sudo apt update
dotnet tool install --global dotnet-t4 --version 2.3.1
sudo apt -y install build-essential gcc g++ make cmake libelf-dev llvm clang libzstd1 git libjson-glib-dev libxml2 libxml2-dev googletest google-mock libgmock-dev libssl-dev
```

## Build
```
cd
git clone --recurse-submodules https://github.com/microsoft/Sysmon-Linux.git SysmonForLinux
cd SysmonForLinux
mkdir build
cd build
cmake ..
make
```

## Cross-build for aarch64 on Ubuntu

First cross-build and stage SysinternalsEBPF as described in its `BUILD.md`.
The commands below assume its staged installation is in
`../SysinternalsEBPF/build-arm64/staging`.

Install the cross compiler and target development libraries:

```shell
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install crossbuild-essential-arm64 binutils-aarch64-linux-gnu \
    libc6-dev-arm64-cross linux-libc-dev-arm64-cross \
    libelf-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64 \
    libjson-glib-dev:arm64 libgtest-dev:arm64 libgmock-dev:arm64
```

Build the eBPF size checker for the build host. It inspects architecture-neutral
eBPF objects during the cross-build and avoids executing aarch64 programs on
the build host:

```shell
cmake -S . -B build-host
cmake --build build-host --target checkEBPFsizes
```

Configure and build the aarch64 targets:

```shell
EBPF_STAGE="$(realpath ../SysinternalsEBPF/build-arm64/staging)"

cmake -S . -B build-arm64 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
    -DOPENSSL_CROSS_COMPILE=aarch64-linux-gnu- \
    -DSYSINTERNALS_EBPF_ROOT="$EBPF_STAGE/opt/sysinternalsEBPF" \
    -DSYSINTERNALS_EBPF_INCLUDE_DIR="$EBPF_STAGE/usr/local/include" \
    -DSYSINTERNALS_EBPF_LIBRARY="$EBPF_STAGE/usr/local/lib/libsysinternalsEBPF.so" \
    -DSYSMON_HOST_CHECK_EBPF_SIZES="$PWD/build-host/checkEBPFsizes"
cmake --build build-arm64 --parallel
```

The resulting `sysmon`, `sysmonLogView`, `sysmonUnitTests`, and
`checkEBPFsizes` executables target aarch64. Run those executables and all
runtime tests on an aarch64 Linux host; QEMU is not required.

## Test
```
./sysmonUnitTests
```

## Run
```
sudo ./sysmon -?
```

## Install
```
sudo ./sysmon -i CONFIG_FILE
```
This will install sysmon and associated files into the /opt/sysmon directory.
The binary is portable and self-contained - the build process packs the
required files into the binary for installation with '-i'. Sysmon will restart
on reboot with the same configuration.

Change the configuration with
```
sudo /opt/sysmon/sysmon -c CONFIG_FILE
```

Uninstall sysmon with
```
sudo /opt/sysmon/sysmon -u
```

## Make Packages
Packages can be generated with:
```
make deb
```
or
```
make rpm
```

The directories build/deb and build/rpm will be populated with the required
files. If dpkg-deb is available, the build/deb directory will be used to create
a deb package. Similarly if rpmbuild is available, the build/rpm directory will
be used to create an rpm package.
