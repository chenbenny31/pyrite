# SPDX-License-Identifier: GPL-2.0-only
#
# tests/hold, linked statically, on Alpine so the pod has a shell to find
# its one /dev/pyriteN

FROM docker.io/library/alpine:3.24 AS build
RUN apk add --no-cache gcc musl-dev linux-headers
WORKDIR /src
COPY tests/hold.c tests/
COPY uapi/pyrite.h uapi/
RUN gcc -static -O2 -Wall -Wextra -o /hold tests/hold.c

FROM docker.io/library/alpine:3.24
COPY --from=build /hold /hold
