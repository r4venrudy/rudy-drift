FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure \
    && DESTDIR=/package cmake --install build

FROM debian:bookworm-slim
ARG VCS_REF=unknown
LABEL org.opencontainers.image.source="https://github.com/r4venrudy/rudy-drift" \
      org.opencontainers.image.description="Offline C17 network anomaly triage: fit, score, measure drift and export evidence." \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.revision="${VCS_REF}"
COPY --from=build /package/usr/local/ /usr/local/
RUN mkdir /data && chown 10001:10001 /data
USER 10001:10001
WORKDIR /data
ENTRYPOINT ["rudy-drift"]
CMD ["--help"]
