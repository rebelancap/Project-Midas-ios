FROM ubuntu:24.04

ARG SDL2_VERSION=2.30.11
ARG SDL2_SRC_SHA1=f3cdd91df44653518d4f3db5fdc775170016aa85
ARG SDL2_MINGW_SHA1=a44b17a6521b40e04d7beaae90b4b3ce19e62e7b

ARG HOST_UID=1000
ARG HOST_GID=1000

RUN echo ">> install distro packages ..." &&  \
    apt update && \
    apt -y install bash-completion sudo wget make git python3 build-essential

RUN echo ">> install PC port (Linux i686) build dependencies ..." &&  \
    dpkg --add-architecture i386 && \
    apt update && \
    apt -y install cmake gcc-multilib g++-multilib \
        libx11-dev:i386 libxext-dev:i386 libxrandr-dev:i386 libxcursor-dev:i386 \
        libxfixes-dev:i386 libxi-dev:i386 libxss-dev:i386 libasound2-dev:i386 \
        libudev-dev:i386 libgl-dev:i386

RUN echo ">> install PC port (Windows x86) cross-build dependencies ..." &&  \
    apt -y install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686

RUN echo ">> setup workspace ..." &&  \
    echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers && \
    if getent passwd "${HOST_UID}" >/dev/null; then \
        userdel -r "$(getent passwd "${HOST_UID}" | cut -d: -f1)" 2>/dev/null || \
        userdel "$(getent passwd "${HOST_UID}" | cut -d: -f1)"; \
    fi && \
    if getent group "${HOST_GID}" >/dev/null; then \
        groupdel "$(getent group "${HOST_GID}" | cut -d: -f1)" 2>/dev/null || true; \
    fi && \
    groupadd -g "${HOST_GID}" dev && \
    useradd -m -s /bin/bash -u "${HOST_UID}" -g "${HOST_GID}" dev && \
    usermod -aG sudo dev
USER dev

RUN echo ">> build SDL2 (i686) from source ..." && \
    cd /home/dev && \
    wget -O sdl2-src.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz" && \
    echo "${SDL2_SRC_SHA1}  sdl2-src.tar.gz" | sha1sum -c - && \
    mkdir -p sdl2-src && \
    tar -xzf sdl2-src.tar.gz --strip-components=1 -C sdl2-src && \
    cmake -S sdl2-src -B sdl2-build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
        -DCMAKE_SHARED_LINKER_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
        -DCMAKE_INSTALL_PREFIX=/home/dev/sdl2-linux-i686 -DCMAKE_INSTALL_LIBDIR=lib/i386-linux-gnu \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_WAYLAND=OFF -DSDL_IBUS=OFF -DSDL_FCITX=OFF -DSDL_DBUS=OFF \
        -DSDL_PULSEAUDIO=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBSAMPLERATE=OFF \
        -DSDL_X11=ON -DSDL_OPENGL=ON -DSDL_ALSA=ON && \
    cmake --build sdl2-build -j$(nproc) && \
    cmake --install sdl2-build && \
    rm -rf sdl2-src sdl2-build sdl2-src.tar.gz

RUN echo ">> get SDL2 (Windows x86, MinGW development package) ..." && \
    cd /home/dev && \
    wget -O sdl2-mingw.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz" && \
    echo "${SDL2_MINGW_SHA1}  sdl2-mingw.tar.gz" | sha1sum -c - && \
    mkdir -p sdl2-mingw && \
    tar -xzf sdl2-mingw.tar.gz -C sdl2-mingw && \
    rm sdl2-mingw.tar.gz

WORKDIR /home/dev/project

CMD ["/bin/bash"]
