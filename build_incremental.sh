#!/bin/bash

set -e
set -x

cd readline-8.3.6
make -j `nproc`
make -j `nproc` install
cd ../bash-5.3.20
touch bashline.c # force relink
make -j `nproc`
make -j `nproc` install

