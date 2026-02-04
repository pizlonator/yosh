#!/bin/bash

set -e
set -x

cd readline-8.2.13
make -j `nproc`
make -j `nproc` install
cd ../bash-5.2.32
touch bashline.c # force relink
make -j `nproc`
make -j `nproc` install

