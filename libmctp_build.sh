#!/bin/bash

sudo apt install build-essential ninja-build pipx cmake  -y
pipx install meson
pipx ensurepath

cd libmctp
meson setup build-meson
meson compile -C build-meson