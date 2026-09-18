#!/bin/bash

set -e
set -x

cd readline-8.3.6
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ ./configure --prefix=$PWD/../prefix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install
cd ../bash-5.3.20
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ CPPFLAGS=-I$PWD/../prefix/include LDFLAGS=-L$PWD/../prefix/lib LIBS="-lcurl -lm" ./configure --prefix=$PWD/../prefix --without-bash-malloc --with-installed-readline
make -j `nproc`
make -j `nproc` install

