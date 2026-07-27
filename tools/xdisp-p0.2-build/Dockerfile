FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL https://repo.ubports.com/keyring.gpg | gpg --dearmor -o /usr/share/keyrings/ubports.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/ubports.gpg] https://repo.ubports.com/ 24.04-1.x main" > /etc/apt/sources.list.d/ubports.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      abi-compliance-checker \
      android-headers-28 \
      build-essential \
      cmake \
      cmake-extras \
      doxygen \
      googletest \
      graphviz \
      libandroid-properties-dev \
      libcapnp-dev \
      libdeviceinfo-dev \
      libdrm-dev \
      libegl1-mesa-dev \
      libgles2-mesa-dev \
      libglib2.0-dev \
      libglm-dev \
      libgoogle-glog-dev \
      libgtest-dev \
      libhardware-dev \
      libhybris-dev \
      liblttng-ust-dev \
      libmir1client-dev \
      libmir1common-dev \
      libmir1core-dev \
      libmir1al-dev \
      libmir1platform-dev \
      libmir1renderer-dev \
      libmir1server-dev \
      libprotobuf-dev \
      libudev-dev \
      libumockdev-dev \
      libwayland-dev \
      mir1-renderer-gl-dev \
      mir1test-dev \
      ninja-build \
      pkg-config \
      python3 \
      umockdev \
      valgrind \
      xsltproc \
 && rm -rf /var/lib/apt/lists/*
