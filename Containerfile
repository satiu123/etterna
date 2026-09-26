FROM docker.io/library/ubuntu:22.04

ARG APT_MIRROR=mirrors.ustc.edu.cn

ENV DEBIAN_FRONTEND=noninteractive

RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s@//.*archive.ubuntu.com@//$APT_MIRROR@g" /etc/apt/sources.list && \
        sed -i "s/security.ubuntu.com/$APT_MIRROR/g" /etc/apt/sources.list; \
    fi

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    nasm \
    git \
    pkg-config \
    ca-certificates \
    libssl-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxtst-dev \
    libglu1-mesa-dev \
    libgl1-mesa-dev \
    mesa-common-dev \
    libpulse-dev \
    libasound2-dev \
    libjack-jackd2-dev \
    libogg-dev \
    libvorbis-dev \
    libcurl4-openssl-dev \
    libglew-dev \
    libmp3lame-dev \
    libslang2-dev \
    xorg-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
