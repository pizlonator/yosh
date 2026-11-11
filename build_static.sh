#!/bin/sh

# Build a static yosh. Run from the fil-c checkout root, like the other build
# scripts: `cd fil-c-5 && ../yosh/build_static.sh`.

. libpas/common.sh

set -e
set -x

# All dependencies are built with the projeny flow (see build_zlib.sh, which is
# the canonical example) and installed into pizfix, statically where possible.

cd projects

# zlib
rm -rf zlib/extracted-source
../filc/projeny extract zlib.projeny zlib/extracted-source
cd zlib/extracted-source
CC="$PWD/../../../build/bin/clang" CFLAGS="-O3 -g" ./configure --prefix=$PWD/../../../pizfix
make -j $NCPU
make install
cd ../..
rm -rf zlib/extracted-source

# zstd (no projeny file; vendored git tree, same as build_zstd.sh)
cd zstd-1.5.7
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j $NCPU
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j $NCPU prefix=$PWD/../../../pizfix install
cd ../..

# ncurses (no projeny file; vendored git tree, same as build_ncurses.sh, but
# --without-shared and --disable-db-install for the static build)
cd ncurses-6.6
extract_source
PATH=$PWD/../../../pizfix/bin:$PATH CC="$PWD/../../../build/bin/clang -O -g" CXX="$PWD/../../../build/bin/clang++ -O -g" ./configure \
    --prefix="$PWD/../../../pizfix" --disable-lib-suffixes --without-shared --without-ada --disable-db-install --with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo:/etc/terminfo
make -j $NCPU
make -j $NCPU install
ln -fs ncurses6-config ../../../pizfix/bin/ncursesw6-config
cd ../..

# openssl (extracted outside the projeny workdir, like build_zunsafe_openssl.sh;
# no-shared so that we only install the static libssl.a/libcrypto.a)
rm -rf openssl-build/extracted-source
mkdir -p openssl-build
../filc/projeny extract openssl.projeny openssl-build/extracted-source
cd openssl-build/extracted-source
CC="$PWD/../../../build/bin/clang -g -O2 -yolo-assembler" ./Configure \
    zlib --prefix=$PWD/../../../pizfix --libdir=lib no-shared
make -j $NCPU
make -j $NCPU install_sw
make -j $NCPU install_ssldirs
cd ../..

# libunistring
rm -rf libunistring/extracted-source
../filc/projeny extract libunistring.projeny libunistring/extracted-source
cd libunistring/extracted-source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --disable-shared --sysconfdir=/etc
make -j $NCPU
make -j $NCPU install
cd ../..
rm -rf libunistring/extracted-source

# libidn2
rm -rf libidn2/extracted-source
../filc/projeny extract libidn2.projeny libidn2/extracted-source
cd libidn2/extracted-source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --sysconfdir=/etc --disable-shared
make -j $NCPU
make -j $NCPU install
cd ../..
rm -rf libidn2/extracted-source

# nghttp2
rm -rf nghttp2/extracted-source
../filc/projeny extract nghttp2.projeny nghttp2/extracted-source
cd nghttp2/extracted-source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ./configure --prefix=$PWD/../../../pizfix --sysconfdir=/etc --disable-shared --enable-lib-only
make -j $NCPU
make -j $NCPU install
cd ../..
rm -rf nghttp2/extracted-source

# curl
rm -rf curl/extracted-source
../filc/projeny extract curl.projeny curl/extracted-source
cd curl/extracted-source
CC=$PWD/../../../build/bin/clang LIBS="-lidn2 -lunistring" \
    ./configure --with-openssl --with-nghttp2 \
                --prefix=$PWD/../../../pizfix --disable-shared \
                --with-ca-path=/etc/ssl/certs --sysconfdir=/etc \
                --enable-threaded-resolver
$MAKE -j $NCPU
$MAKE -j $NCPU install
cd ../..
rm -rf curl/extracted-source

cd ..
FILCSRC=$PWD

# readline: distclean first so that stale objects from any previous build
# (possibly built with an ancient Fil-C ABI) can never survive into the link.
cd ../yosh/readline-8.3.6
make distclean || true
make clean || true
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$FILCSRC/pizfix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install

# bash: distclean for the same reason. CPPFLAGS=-I$FILCSRC/pizfix/include makes
# the installed-readline version test compile, so configure genuinely uses
# readline 8.3 from pizfix instead of silently falling back to the bundled one.
cd ../bash-5.3.20
make distclean || true
make clean || true
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ \
    CPPFLAGS="-I$FILCSRC/pizfix/include" \
    LDFLAGS="-static" LIBS="-lreadline -lncurses -lcurl -lnghttp2 -lidn2 -lunistring -lssl -lcrypto -lz -lzstd -lm" \
    ./configure --prefix=$FILCSRC/pizfix --without-bash-malloc --with-installed-readline
make -j `nproc`
make -j `nproc` install


