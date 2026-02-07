#!/bin/sh

. libpas/common.sh

set -e
set -x

rm -rf static_deps
mkdir static_deps
cd static_deps
tar -xf ../pizlix/zlib-1.3.1.tar.gz
cd zlib-1.3.1
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ./configure --prefix=$PWD/../../pizfix
make -j `nproc`
make -j `nproc` install
cd ..
rm -rf zlib-1.3.1

tar -xf ../pizlix/zstd-1.5.6.tar.gz
cd zstd-1.5.6
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ZSTD_NO_ASM=1 make -j `nproc` prefix=$PWD/../../pizfix
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ZSTD_NO_ASM=1 make -j `nproc` prefix=$PWD/../../pizfix install
cd ..
rm -rf zstd-1.5.6

cd ../projects/ncurses-6.5-20240720
extract_source
PATH=$PWD/../../../pizfix/bin:$PATH CC="$PWD/../../../build/bin/clang -O -g" CXX="$PWD/../../../build/bin/clang++ -O -g" ./configure \
    --prefix="$PWD/../../../pizfix" --disable-lib-suffixes --without-shared --without-ada --disable-db-install --with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo:/etc/terminfo
make -j $NCPU
make -j $NCPU install
ln -fs ncurses6-config ../../../pizfix/bin/ncursesw6-config

cd ../../openssl-3.3.1
extract_source
CC="$PWD/../../../build/bin/clang -g -O2" ./Configure \
    zlib --prefix=$PWD/../../../pizfix --libdir=lib no-shared
make -j $NCPU
make -j $NCPU install_sw
make -j $NCPU install_ssldirs

cd ../../../static_deps
tar -xf ../pizlix/libunistring-1.2.tar.xz
cd libunistring-1.2
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ./configure --prefix=$PWD/../../pizfix --disable-shared --sysconfdir=/etc
make -j `nproc`
make -j `nproc` install
cd ..
rm -rf libunistring-1.2

cd ../projects/libidn2-2.3.7
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --sysconfdir=/etc --disable-shared
make -j `nproc`
make -j `nproc` install

cd ../../../static_deps
tar -xf ../pizlix/nghttp2-1.62.1.tar.xz
cd nghttp2-1.62.1
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ./configure --prefix=$PWD/../../pizfix --sysconfdir=/etc --disable-shared --enable-lib-only
make -j `nproc`
make -j `nproc` install
cd ..
rm -rf nghttp2-1.62.1

cd ../projects/curl-8.9.1
extract_source
CC=$PWD/../../../build/bin/clang LIBS="-lidn2 -lunistring" \
    ./configure --with-openssl --with-nghttp2 \
                --prefix=$PWD/../../../pizfix --disable-shared \
                --with-ca-path=/etc/ssl/certs --sysconfdir=/etc \
                --enable-threaded-resolver
$MAKE -j $NCPU
$MAKE -j $NCPU install
cd ../../../

FILCSRC=$PWD
cd ../yosh/readline-8.2.13
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$FILCSRC/pizfix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install

cd ../bash-5.2.32
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ LDFLAGS="-static" LIBS="-lreadline -lncurses -lcurl -lnghttp2 -lidn2 -lunistring -lssl -lcrypto -lz -lzstd -lm" ./configure --prefix=$FILCSRC/pizfix --without-bash-malloc --with-installed-readline
make -j `nproc`
make -j `nproc` install

