FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL http://repo.ubports.com/keyring.gpg | gpg --dearmor -o /usr/share/keyrings/ubports.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/ubports.gpg] http://repo.ubports.com/ focal main" > /etc/apt/sources.list.d/ubports.list \
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
      libmirclient-dev \
      libmircommon-dev \
      libmircore-dev \
      libmiral-dev \
      libmirplatform-dev \
      libmirrenderer-dev \
      libmirserver-dev \
      libprotobuf-dev \
      libudev-dev \
      libumockdev-dev \
      libwayland-dev \
      mir-renderer-gl-dev \
      mirtest-dev \
      ninja-build \
      pkg-config \
      python3 \
      umockdev \
      valgrind \
      xsltproc \
 && rm -rf /var/lib/apt/lists/*
