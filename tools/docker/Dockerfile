# Build environment for Tether on QNX 8.0 from an Apple Silicon Mac.
# SDP 8.0 host tools are x86_64 Linux binaries, so this image is linux/amd64
# and runs under Docker's emulation. The SDP itself is NOT in the image: it is
# installed on the Mac by QNX Software Center and mounted read-only.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build make git file xz-utils unzip curl ca-certificates \
      python3 python3-venv rsync openssh-client sshpass netcat-openbsd less vim-tiny \
      libncurses6 libtinfo6 zlib1g libssl3t64 locales sudo \
    && rm -rf /var/lib/apt/lists/* && locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8

# A user that matches the Mac user, so files written to mounted folders keep
# the right owner. Ubuntu already has a group with gid 20 (dialout), so reuse it.
ARG USER_NAME=dev
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN (getent group ${GROUP_ID} >/dev/null || groupadd --gid ${GROUP_ID} grp) \
    && (id -u ubuntu >/dev/null 2>&1 && userdel -r ubuntu || true) \
    && useradd --uid ${USER_ID} --gid ${GROUP_ID} --create-home --shell /bin/bash ${USER_NAME} \
    && echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/dev
USER ${USER_NAME}
WORKDIR /home/${USER_NAME}
ENV QNX_SDP=/opt/qnx800
CMD ["/bin/bash", "-l"]
