#!/bin/bash
# Builds the CMake project.
mkdir -p build
cd build
cmake ..
make -j$(nproc)
