FROM --platform=linux/amd64 mcr.microsoft.com/devcontainers/base:ubuntu-22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      build-essential \
      clang \
      bear \
      qemu-user \
      qemu-user-static \
      gcc-aarch64-linux-gnu \
      g++-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*
