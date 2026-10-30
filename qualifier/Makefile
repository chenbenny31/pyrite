# SPDX-License-Identifier: GPL-2.0-only
#
# Go's module and build caches live in the repo, like ./linux and ./.venv

export GOMODCACHE := $(abspath ../.cache/go/mod)
export GOCACHE := $(abspath ../.cache/go/build)

.PHONY: all test clean

all:
	CGO_ENABLED=0 go build -trimpath -o bin/pyrite-qualifier .

# The race detector needs cgo
test:
	go vet ./...
	CGO_ENABLED=1 go test -race ./...

clean:
	rm -rf bin
