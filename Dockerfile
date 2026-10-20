# FunTrade bot image. Builds and runs the unit tests in the build stage, so an image only
# exists if its tests passed. Same distro as development (Ubuntu 24.04: Boost 1.83,
# Protobuf 3.21.12), built natively for the target architecture (arm64 for the Pi).
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
        libboost-system-dev libboost-thread-dev libssl-dev libprotobuf-dev protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04
# tzdata: the strategy converts to New York time (std::chrono::locate_zone).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 libprotobuf32t64 zlib1g tzdata ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/FunTrade /app/FunTrade
COPY config /app/config
# state/ and logs/ are volumes; the bot writes them relative to /app.
RUN mkdir -p /app/state /app/logs && chown ubuntu:ubuntu /app/state /app/logs
# uid 1000 (the image's ubuntu user) = the Pi user owning the mounted state/ and logs/.
USER ubuntu
ARG GIT_SHA=unknown
ENV FUNTRADE_GIT_SHA=$GIT_SHA
CMD ["/app/FunTrade"]
