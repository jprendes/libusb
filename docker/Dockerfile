FROM ubuntu:20.04

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    wget \
    git \
    ninja-build \
    libudev-dev \
    lsb-release \
    software-properties-common \
    gnupg \
    gpg

RUN echo "Acquire { https::apt.kitware.com::Verify-Peer \"false\"; }" > /etc/apt/apt.conf.d/99verify-peer.conf

RUN DEBIAN_FRONTEND=noninteractive bash -c "$(wget -O - https://apt.kitware.com/kitware-archive.sh)"
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    cmake

RUN DEBIAN_FRONTEND=noninteractive bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" -- 16 all
