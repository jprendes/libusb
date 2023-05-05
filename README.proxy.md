# USB network proxy

## Build

```bash
CC=clang CXX=clang++ cmake \
    -S . \
    -B build \
    -G Ninja \
    -Dlibusb_PROXY=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

## Run the server

Simply run
```bash
sudo ./build/proxy/server
```

Use the `-a` and `-p` command line options to set the binding address and port to listent to, e.g.,
```bash
sudo ./build/proxy/server -p 1234 -a 0.0.0.0
```

## Run the client

Preload the shared library when running an application that uses libusb:
```bash
LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```

Use the `LIBUSB_PROXY_HOST` and `LIBUSB_PROXY_PORT` environment variables to set the host and port to connecto to, e.g.,
```bash
LIBUSB_PROXY_HOST=localhost LIBUSB_PROXY_PORT=1234 LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```

## Introduction

USB redirection is the process of running a program (called the user)
that access the USB devices through another program (called the provider)
that potentially runs on another machine. The user exchanges messages with
the provider through a transport, typically unix sockets or TCP.

To achieve this, libusb can be recompile with different options to enable
a different backend than the usual OS backend. This way, the user can
use the standard libusb API and does not need to be aware of this redirection.
This libusb backend talks to a program (examples/redir_server) that itself
uses a normal version of libusb to actually perform the operations.

Picture:

```
+----+           +--------+                +-------+           +--------+             +------+
|    |           | libusb |                |redir  |           |libusb  |             |USB   |
|user|--(uses)-->|(redir  |--(transport)-->|server |--(uses)-->|(normal |--(access)-->|device|
|    |           |backend)|                |       |           |backend)|             |      |
+----+           +--------+                +-------+           +--------+             +------+
```

## How-to

You need to compile a version of libusb with the redir backend.
The following assumes that your system already has a normal version
of libusb install and will just compile another one locally.

```bash
# get a copy of source
# if you get one through git, you might need to bootstrap
# to generate the configure script
./bootstrap.sh
# run configure, you can add option such as install prefix
./configure --enable-redir --enable-tests-build --enable-examples-build
# compile
make -j3
```

To test this, you need to run the server on one side and an application
on the other side. For the following test, we will run both on the same machine
and use LD_PRELOAD (unix only) to intercept the libusb calls.
