# Multi-stage: build FalconGateway, ship minimal runtime + dev image
FROM ubuntu:24.04 AS builder
RUN apt-get update -qq && apt-get install -y -qq cmake g++ && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DFALCON_MARCH_NATIVE=OFF \
    && cmake --build build -j$(nproc) \
    && cmake --install build --prefix /opt/falcon

# --- Runtime image (library + CTP SDK only) ---
FROM ubuntu:24.04 AS runtime
COPY --from=builder /opt/falcon /opt/falcon
ENV LD_LIBRARY_PATH=/opt/falcon/lib:$LD_LIBRARY_PATH
LABEL org.opencontainers.image.description="FalconGateway — CTP futures gateway (SHFE/CZCE/DCE/GFEX)"
LABEL org.opencontainers.image.source="https://github.com/SwainGao/FalconGateway"
