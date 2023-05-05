#!/bin/bash

REPO_ROOT="$(realpath "$(dirname "$0")"/..)"

BUILDER=(docker run --rm -it -v "$REPO_ROOT:/src" -u "$(id -u):$(id -g)" libusb-proxy-builder:latest)

docker build -t libusb-proxy-builder "$REPO_ROOT"/docker

"${BUILDER[@]}" cmake \
    --fresh \
    -S /src \
    -B /src/build \
    -G Ninja \
    -DCMAKE_C_COMPILER=clang-16 \
    -DCMAKE_CXX_COMPILER=clang++-16 \
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE=1 -std=gnu99" \
    -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
    -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -static-libstdc++" \
    -DCMAKE_SHARED_LINKER_FLAGS="-stdlib=libc++ -static-libstdc++" \
    -Dlibusb_PROXY=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
"${BUILDER[@]}" cmake --build /src/build
