#!/bin/sh

. libpas/common.sh

set -e
set -x

rm -rf static_deps
mkdir static_deps

cd projects/zlib-1.3.2
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix
make -j `nproc`
make -j `nproc` install

cd ../../zstd-1.5.7
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j `nproc` prefix=$PWD/../../../pizfix
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j `nproc` prefix=$PWD/../../../pizfix install

cd ../../ncurses-6.6
extract_source
PATH=$PWD/../../../pizfix/bin:$PATH CC="$PWD/../../../build/bin/clang -O -g" CXX="$PWD/../../../build/bin/clang++ -O -g" ./configure \
    --prefix="$PWD/../../../pizfix" --disable-lib-suffixes --without-shared --without-ada --disable-db-install --with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo:/etc/terminfo
make -j $NCPU
make -j $NCPU install
ln -fs ncurses6-config ../../../pizfix/bin/ncursesw6-config

cd ../../
rm -rf openssl-build/extracted-source
mkdir -p openssl-build
../filc/projeny extract openssl.projeny openssl-build/extracted-source
cd openssl-build/extracted-source
CC="$PWD/../../../build/bin/clang -g -O2 -yolo-assembler" ./Configure \
    zlib --prefix=$PWD/../../../pizfix --libdir=lib no-shared
make -j $NCPU
make -j $NCPU install_sw
make -j $NCPU install_ssldirs

cd ../../../static_deps
tar -xf ../pizlix/libunistring-1.4.2.tar.xz
cd libunistring-1.4.2
CC=$PWD/../../build/bin/clang CXX=$PWD/../../build/bin/clang++ ./configure --prefix=$PWD/../../pizfix --disable-shared --sysconfdir=/etc
make -j `nproc`
make -j `nproc` install
cd ..
rm -rf libunistring-1.4.2

cd ../projects
rm -rf libidn2/extracted-source
../filc/projeny extract libidn2.projeny libidn2/extracted-source
cd libidn2/extracted-source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --sysconfdir=/etc --disable-shared
make -j `nproc`
make -j `nproc` install

cd ../../nghttp2-1.70.0
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --sysconfdir=/etc --disable-shared --enable-lib-only
make -j `nproc`
make -j `nproc` install

cd ../../curl-8.22.0
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

