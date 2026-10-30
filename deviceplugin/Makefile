# SPDX-License-Identifier: GPL-2.0-only
#
# Go's module and build caches live in the repo, like ./linux and ./.venv

export GOMODCACHE := $(abspath ../.cache/go/mod)
export GOCACHE := $(abspath ../.cache/go/build)

.PHONY: all test image clean

IMAGE := localhost/pyrite-device-plugin:dev

all:
	CGO_ENABLED=0 go build -trimpath -o bin/pyrite-device-plugin .

# The race detector needs cgo
test:
	go vet ./...
	CGO_ENABLED=1 go test -race ./...

image: all
	podman build -q -t $(IMAGE) -f Containerfile . > /dev/null

clean:
	rm -rf bin
