// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// Device is one pyrite device as the plugin advertises it.
type Device struct {
	ID   string // sysfs uuid, e.g. "GPU-20081800-c819-4400-a406-204214828200"
	Path string // "/dev/pyriteN"
}

// Discover reads <sysClass>/pyriteN/uuid for N = 0, 1, ... until one is
// missing. pyrite numbers its devices 0..ndev-1, so there are no gaps.
// Finding no device is an error.
func Discover(sysClass string) ([]Device, error) {
	var devs []Device
	for i := 0; ; i++ {
		name := fmt.Sprintf("pyrite%d", i)
		uuid, err := os.ReadFile(filepath.Join(sysClass, name, "uuid"))
		if errors.Is(err, fs.ErrNotExist) {
			break
		}
		if err != nil {
			return nil, err
		}
		devs = append(devs, Device{
			ID:   strings.TrimSpace(string(uuid)),
			Path: "/dev/" + name,
		})
	}

	if len(devs) == 0 {
		return nil, fmt.Errorf("no pyrite devices with a uuid under %s; is the module loaded?", sysClass)
	}
	return devs, nil
}
