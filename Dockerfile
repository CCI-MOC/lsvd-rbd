FROM ubuntu:22.04

# TODO(knikolla): Move (or remove) dependencies in the appropriate Makefile section
RUN apt-get update \
    && apt-get install -y git curl build-essential sudo cmake python3 python3-pip libssl-dev \
    && pip install -U meson

RUN sudo apt-get install -y wget lsb-release software-properties-common gnupg \
    && wget https://apt.llvm.org/llvm.sh \
    && chmod +x llvm.sh \
    && ./llvm.sh 18

WORKDIR /app
COPY Makefile /app/Makefile
COPY meson.* /app/
COPY src /app/src
COPY subprojects /app/subprojects
COPY test /app/test

RUN make install-deps \
    && make release
