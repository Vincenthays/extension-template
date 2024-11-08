FROM ubuntu:24.04

RUN apt update && apt install -y build-essential cmake git curl zip unzip tar pkg-config

WORKDIR /opt
RUN git clone https://github.com/Microsoft/vcpkg.git
RUN ./vcpkg/bootstrap-vcpkg.sh 
ENV VCPKG_TOOLCHAIN_PATH=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake

WORKDIR /app
COPY vcpkg.json ./
RUN vcpkg install
COPY . ./
RUN make

FROM google/cloud-sdk:slim

WORKDIR /app
COPY --from=0 /app/build/release/extension/bigtable2/bigtable2.duckdb_extension .
RUN gzip bigtable2.duckdb_extension
