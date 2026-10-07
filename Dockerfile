# syntax=docker/dockerfile:1

# ---- build ------------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates python3 linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
WORKDIR /src

# Dependencies first, so this layer is cached until vcpkg.json changes.
COPY vcpkg.json .
RUN git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout "$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' vcpkg.json)" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics \
    && "$VCPKG_ROOT/vcpkg" install --x-install-root=/src/build/release/vcpkg_installed

COPY . .
RUN cmake --preset release -DVALSTREAM_BUILD_TESTS=OFF && cmake --build --preset release

# ---- runtime ----------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /app valstream

WORKDIR /app
COPY --from=build /src/build/release/valstream ./valstream
COPY config.json ./
COPY fixtures ./fixtures
COPY web ./web
RUN mkdir data && chown valstream:valstream data

USER valstream
ENV VALSTREAM_DB_PATH=/app/data/valstream.db
EXPOSE 8080
VOLUME ["/app/data"]
ENTRYPOINT ["/app/valstream"]
