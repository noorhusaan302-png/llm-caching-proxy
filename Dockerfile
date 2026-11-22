# Build stage: compile the proxy (Alpine uses musl, so a static binary resolves hostnames fine)
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY main.c .
RUN gcc -O2 -pthread -static -o proxy main.c

# Run stage: a tiny image with just the binary
FROM alpine:3.20
COPY --from=build /src/proxy /usr/local/bin/proxy
ENV PROXY_PORT=8080 \
    UPSTREAM=host.docker.internal:11434 \
    CACHE_CAPACITY=100
EXPOSE 8080
USER nobody
ENTRYPOINT ["proxy"]
