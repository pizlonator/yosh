#!/bin/bash

set -e
set -x

cd readline-8.2.13
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ ./configure --prefix=$PWD/../prefix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install
cd ../bash-5.2.32
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ CPPFLAGS=-I$PWD/../prefix/include LDFLAGS=-L$PWD/../prefix/lib ./configure --prefix=$PWD/../prefix --without-bash-malloc --with-installed-readline
make -j `nproc`
make -j `nproc` install

