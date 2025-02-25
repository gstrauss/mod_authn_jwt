FROM ubuntu:22.04 as builder

RUN apt-get update && apt-get install -y \
    build-essential \
    bzip2 \
    cmake \
    libjansson-dev \
    libpcre2-dev \
    libssl-dev \
    ninja-build \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Download and build libjwt
ADD https://github.com/benmcollins/libjwt/releases/download/v1.17.0/libjwt-1.17.0.tar.bz2 libjwt-1.17.0.tar.bz2
RUN tar -xjf libjwt-1.17.0.tar.bz2
RUN \
    cd libjwt-1.17.0 && \
    mkdir out && \
    cd out && \
    cmake -GNinja -DBUILD_SHARED_LIBS=ON .. && \
    ninja && \
    ninja install

# Download lighttpd
ADD https://download.lighttpd.net/lighttpd/releases-1.4.x/lighttpd-1.4.75.tar.gz lighttpd-1.4.75.tar.gz
RUN tar -xzf lighttpd-1.4.75.tar.gz

# Patch lighttpd
COPY . src
RUN cp src/mod_authn_jwt.c /lighttpd-1.4.75/src
RUN cp src/CMakeLists.txt.patch /lighttpd-1.4.75
RUN \
    cd /lighttpd-1.4.75 && \
    patch -p1 <CMakeLists.txt.patch

# Build lighttpd
RUN cmake -S lighttpd-1.4.75 -B lighttpd-build -DWITH_JWT=ON
RUN cmake --build lighttpd-build

FROM ubuntu:22.04 as runner

RUN apt-get update && apt-get install -y \
    curl \
    libjansson4 \
    && rm -rf /var/lib/apt/lists/*

# Install libjwt
COPY --from=builder /usr/local/lib/libjwt.so /usr/lib

# Install lighttpd
COPY --from=builder lighttpd-build/build/lighttpd /usr/local/bin

# Install modules
RUN install -d /usr/local/lib/lighttpd
COPY --from=builder lighttpd-build/build/mod_*.so /usr/local/lib/lighttpd

# Configure lighttpd
ADD lighttpd.conf /etc/lighttpd/lighttpd.conf
ADD conf.d /etc/lighttpd/conf.d

CMD lighttpd -D -f /etc/lighttpd/lighttpd.conf
