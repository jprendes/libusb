# USB network proxy

## Build

Build using cmake. You will need a C++ compiler with support for C++20.
```bash
cmake \
    -S . \
    -B build \
    -G Ninja \
    -Dlibusb_PROXY=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

Alternatively you can build using the provided container:
```bash
./proxy/build.sh
```

## Run the server

Simply run
```bash
sudo ./build/proxy/server
```

Use the `-l` command line option to set the listening address, and the `LIBUSB_PROXY_LOG_LEVEL` environment variable (with values from `0` to `4`) to control the log level, e.g.,
```bash
sudo LIBUSB_PROXY_LOG_LEVEL=3 ./build/proxy/server -l tcp://localhost:1234 -l local:///tmp/libusb.socket
```

## Run the client

Preload the shared library when running an application that uses libusb:
```bash
LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```

Use the `LIBUSB_PROXY_HOST` environment variable to set the host to connecto to, and `libusb`'s `LIBUSB_DEBUG` environment variable to control the log level, e.g.,
```bash
LIBUSB_PROXY_HOST="tcp://localhost:1234;local:///tmp/libusb.socket" LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```
