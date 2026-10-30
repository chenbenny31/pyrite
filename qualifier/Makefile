# SPDX-License-Identifier: GPL-2.0-only
#
# Go's module and build caches live in the repo, like ./linux and ./.venv

export GOMODCACHE := $(abspath ../.cache/go/mod)
export GOCACHE := $(abspath ../.cache/go/build)

.PHONY: all test image clean

IMAGE := localhost/pyrite-qualifier:dev

all:
	CGO_ENABLED=0 go build -trimpath -o bin/pyrite-qualifier .

# The race detector needs cgo
test:
	go vet ./...
	CGO_ENABLED=1 go test -race ./...

# The context holds the agent and qualify.c, not the repo with ./linux in it
image: all
	rm -rf bin/ctx && mkdir bin/ctx
	cp bin/pyrite-qualifier ../tools/qualify.c bin/ctx/
	podman build -q -t $(IMAGE) -f Containerfile bin/ctx > /dev/null

clean:
	rm -rf bin
