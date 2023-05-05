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

Use the `-a` and `-p` command line options to set the binding address and port to listent to, and the `LIBUSB_PROXY_LOG_LEVEL` environment variable (with values from `0` to `4`) to control the log level, e.g.,
```bash
sudo LIBUSB_PROXY_LOG_LEVEL=3 ./build/proxy/server -p 1234 -a 0.the 0.0.0
```

## Run the client

Preload the shared library when running an application that uses libusb:
```bash
LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```

Use the `LIBUSB_PROXY_HOST` and `LIBUSB_PROXY_PORT` environment variables to set the host and port to connecto to, and `libusb`'s `LIBUSB_DEBUG` environment variable to control the log level, e.g.,
```bash
LIBUSB_PROXY_HOST=localhost LIBUSB_PROXY_PORT=1234 LD_PRELOAD=./build/proxy/libusb-1.0.so lsusb -v
```
