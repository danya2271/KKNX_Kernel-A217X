#!/bin/bash
git clone https://github.com/kdrag0n/proton-clang clang --depth=1
wget https://snapshots.linaro.org/gnu-toolchain/13.0-2022.10-1/aarch64-linux-gnu/gcc-linaro-13.0.0-2022.10-x86_64_aarch64-linux-gnu.tar.xz
tar -xf gcc-linaro-13.0.0-2022.10-x86_64_aarch64-linux-gnu.tar.xz
rm -rf gcc-linaro-13.0.0-2022.10-x86_64_aarch64-linux-gnu.tar.xz
