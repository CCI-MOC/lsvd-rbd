FROM ubuntu:24.04

# TODO(knikolla): Move (or remove) dependencies in the appropriate Makefile section
RUN apt update && apt install -y git curl build-essential sudo cmake python3 python3-pip clang-18 lld-18

WORKDIR /app
COPY Makefile /app/Makefile
COPY meson.* /app/
COPY src /app/src
COPY subprojects /app/subprojects
COPY test /app/test

RUN make install-deps
RUN make release
