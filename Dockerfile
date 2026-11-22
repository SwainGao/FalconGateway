# Multi-stage: build FalconGateway, ship minimal runtime image
FROM ubuntu:24.04 AS builder
RUN apt-get update -qq && apt-get install -y -qq cmake g++ && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DFALCON_MARCH_NATIVE=OFF \
    && cmake --build build -j$(nproc) \
    && cmake --install build --prefix /opt/falcon

# --- Runtime image ---
# Mount a config at /config/config.json (see config.json.example). Secrets can
# be passed as FALCON_USER_ID / FALCON_PASSWORD / FALCON_AUTH_CODE instead.
# Default mode only connects and monitors; append --run-order-test to place
# the 4-exchange test orders.
FROM ubuntu:24.04 AS runtime
COPY --from=builder /opt/falcon /opt/falcon
ENV LD_LIBRARY_PATH=/opt/falcon/lib \
    PATH=/opt/falcon/bin:$PATH
RUN useradd --system --create-home --home-dir /data falcon \
    && mkdir -p /config && chown falcon /config
USER falcon
WORKDIR /data
VOLUME ["/config", "/data"]
ENTRYPOINT ["falcon_main", "--config", "/config/config.json"]
LABEL org.opencontainers.image.description="FalconGateway — CTP futures gateway (SHFE/CZCE/DCE/GFEX)"
LABEL org.opencontainers.image.source="https://github.com/SwainGao/FalconGateway"
