#!/bin/sh

# Build yosh ("Fil's yosh", the bash 5.3.20 fork with the "yo" LLM feature)
# with Peregrine Fil-C: the build lands in pizfix as a fat APE, i.e. ONE file
# (MZqFpD magic) that runs on x86_64 and aarch64 Linux/macOS/BSD and on
# x86_64 Windows, plus the ELF side files <out>.com.dbg (x86_64) and
# <out>.aarch64.elf (aarch64).  Everything is statically linked - cosmo mode
# has no dynamic linker and no shared libraries at all.
#
# Run from the fil-c checkout root, like the other build scripts:
#
#     cd fil-c-5 && ../yosh/build_static.sh
#
# REQUIRES a cosmo-flavor pizfix tree: run ./build_base_cosmo.sh first.  That
# builds the compiler, the yolo cosmo libc (pizfix/lib/libyolocosmo.a + the
# APE tooling in pizfix/libexec), the pizlonated cosmo user libc
# (pizfix/lib/libc.a + crt1.o + pizfix/include), and libc++/libc++abi.  The
# old musl-flavor build path (musl/glibc userlibc + build_static.sh's musl
# dependency stack) is gone: yosh is now a Peregrine project, and the dep
# recipes below are the cosmo-flavor ones (the same flags
# static_deps/install_deps.sh uses to re-install the dep stack after a
# ./build_usercosmo.sh, which is worth re-running after any usercosmo rebuild
# because usercosmo's install-headers wipes pizfix/include).
#
# What the script does, in order:
#
# 1. Builds every dependency of the "yo" feature (zlib, zstd, ncurses,
#    openssl, libunistring, libidn2, nghttp2, curl) from its projeny checkout
#    with the Fil-C compiler in cosmo mode and installs static archives +
#    headers + pkgconfig files into pizfix.  Everything that would use
#    assembly is built with no-asm/ZSTD_NO_ASM (and openssl additionally with
#    no-engine): the driver only creates aarch64 shadow objects for .c
#    compiles (the gate is source-file-type + -c; .s/.o/.a inputs get none),
#    so every object of every archive must come from a .c compile for the
#    shadow archives to be craftable.
# 2. Crafts the aarch64 shadow archive of every installed archive and
#    installs it into BOTH pizfix/lib/.aarch64/<name>.a and
#    pizfix/lib-aarch64/<name>.a.  Without a shadow archive, the aarch64 half
#    of a fat-APE link degrades to an x86_64-only APE with a warning (see
#    cosmo.txt, "Fat APEs").  The shadow is assembled in the primary archive's
#    member order from the per-object .aarch64/ shadow objects that the
#    driver left next to the dep's own objects; the exact search semantics
#    (and what makes it a hard error) are documented in craft_shadows.sh,
#    which this script sources for all of its shadow crafting.
# 3. Regenerates the yolo-cosmo closure make fragments
#    (projects/yolocosmo/filc/filc-objects{,-aarch64}.mk) with
#    compute-closure.py, and rebuilds libyolocosmo.a via ./build_yolocosmo.sh
#    only if a fragment actually changed.
# 4. Builds GNU readline 8.3.6 (with the yo feature) and installs it into
#    pizfix, then crafts its shadow archives.
# 5. Builds bash 5.3.20 (bash's Makefile.in names the program yosh) and
#    installs it as pizfix/bin/yosh (plus the yosh.com.dbg /
#    yosh.aarch64.elf side files), then pechecks the fat APE.

. libpas/common.sh

set -e
set -x

FILCSRC=$PWD
PIZFIX=$FILCSRC/pizfix
YOSH=$FILCSRC/../yosh

# Shadow-archive crafting lives in craft_shadows.sh, shared with
# build_incremental.sh (which sources the same file).  Source it by the
# script's own directory, since this script is invoked as ../yosh/... from
# the fil-c root.  It must be sourced after FILCSRC/PIZFIX are set - see the
# header of craft_shadows.sh for the contract and for craft_shadow's search
# semantics (verbatim the same function that used to be defined here, with
# the per-dir ambiguity hard error).
. "$(cd "$(dirname "$0")" && pwd)/craft_shadows.sh"

# All dependencies are built with the projeny flow (see build_zlib.sh, which is
# the canonical example) and installed into pizfix, statically (cosmo mode has
# no shared libraries, and `clang -shared` refuses outright).

cd projects

# zlib
rm -rf zlib/extracted-source
../filc/projeny extract zlib.projeny zlib/extracted-source
cd zlib/extracted-source
CC="$PWD/../../../build/bin/clang" CFLAGS="-O3 -g" ./configure --prefix=$PWD/../../../pizfix
make -j $NCPU
make install
cd ../..
# Craft the shadow before the build tree goes away (its .aarch64/ shadow
# objects are the shadow archive's members).
craft_shadow $PIZFIX/lib/libz.a zlib/extracted-source
rm -rf zlib/extracted-source

# zstd (no projeny file; vendored git tree, same as build_zstd.sh).  Build
# only the static library (ZSTD_NO_ASM=1 so every object has an aarch64
# shadow; cosmo refuses -shared) and install only the pc/static/includes
# targets - the default `install` also wants install-shared, which would try
# to link libzstd.so and fail.
cd zstd-1.5.7
extract_source
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j $NCPU -C lib libzstd.a
CC=$PWD/../../../build/bin/clang CXX=$PWD/../../../build/bin/clang++ ZSTD_NO_ASM=1 make -j $NCPU -C lib install-pc install-static install-includes PREFIX=$PWD/../../../pizfix
cd ../..
craft_shadow $PIZFIX/lib/libzstd.a zstd-1.5.7/extracted-source/lib

# ncurses (the Peregrine projeny checkout: cosmo's B-constants are extern
# const variables whose per-OS values are selected at runtime, so the static
# speed tables had to switch to runtime reads; see the .projeny header).
# --without-shared because cosmo mode has no shared libraries, and
# --without-tests because the test/demo programs do not link in cosmo mode
# (they reference libc symbols the slim pizlonated cosmo libc does not carry,
# e.g. ctime, and their aarch64 halves cannot resolve the in-tree archives -
# they are not part of what yosh needs anyway).  In this
# configuration the termlib objects are merged into libncurses (there is no
# libtinfo), and the normal + _g (debug) model objects land in objects/ and
# obj_g/, which is exactly what the shadow crafting needs.
rm -rf peregrine-ncurses .peregrine-ncurses.projeny.status
# setup, not extract: the checkout dir IS the projeny workdir, so a clean
# rebuild means erase-setup (rm the checkout + its status file) + fresh setup.
../filc/projeny setup peregrine-ncurses.projeny
cd peregrine-ncurses
PATH=$PIZFIX/bin:$PATH CC="$FILCSRC/build/bin/clang -O -g" CXX="$FILCSRC/build/bin/clang++ -O -g" ./configure \
    --prefix="$PIZFIX" --disable-lib-suffixes --without-shared --without-ada --disable-db-install --without-tests --with-terminfo-dirs=/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo:/etc/terminfo
make -j $NCPU
make -j $NCPU install
ln -fs ncurses6-config $PIZFIX/bin/ncursesw6-config
cd ..
# The five normal-model archives are built from objects/, the five _g
# (debug-model) archives from obj_g/; searching both at once would make every
# member ambiguous (the same member name exists in both models).
for lib in libncurses.a libpanel.a libmenu.a libform.a libncurses++.a
do
    craft_shadow $PIZFIX/lib/$lib peregrine-ncurses/objects
done
for lib in libncurses_g.a libpanel_g.a libmenu_g.a libform_g.a libncurses++_g.a
do
    craft_shadow $PIZFIX/lib/$lib peregrine-ncurses/obj_g
done

# openssl (the Peregrine projeny checkout: cosmo's AF_INET6 & co are extern
# const variables, so the AF-case-label switches had to become comparisons;
# see the .projeny header).  no-asm so every object has an aarch64 shadow,
# no-shared because cosmo refuses -shared, no-engine because engines are DSOs.
# install_dev (NOT install_sw: install_sw's install_modules builds the .so
# providers, which cannot exist in cosmo mode) installs the headers, the
# static libs, and the pkgconfig files.  The generated Makefile's CNF_EX_LIBS
# references -ldl, which does not exist in cosmo mode (no shared libraries,
# no libdl.a) and would break the build of anything that links -lcrypto, so
# strip it.
rm -rf peregrine-openssl .peregrine-openssl.projeny.status
# setup, not extract: the checkout dir IS the projeny workdir, so a clean
# rebuild means erase-setup (rm the checkout + its status file) + fresh setup.
../filc/projeny setup peregrine-openssl.projeny
cd peregrine-openssl
CC="$FILCSRC/build/bin/clang -g -O2 -yolo-assembler" perl ./Configure \
    zlib --prefix=$PIZFIX --libdir=lib no-shared no-asm no-engine
sed -i 's/-ldl//g' Makefile
make -j $NCPU build_libs
make -j $NCPU install_dev
# install_dev stops short of the ssl dir (the full install_ssldirs dies on the
# unbuilt apps/ scripts in cosmo mode); just install the config file.
mkdir -p $PIZFIX/ssl
cp -f apps/openssl.cnf $PIZFIX/ssl/openssl.cnf
cd ..
craft_shadow $PIZFIX/lib/libcrypto.a peregrine-openssl
craft_shadow $PIZFIX/lib/libssl.a peregrine-openssl

# libunistring (the Peregrine projeny checkout: cosmo is musl-like but its
# headers #undef __linux__, so gnulib's langinfo selection needed a __FILC__
# clause; see the .projeny header).  All of its shadow objects live under
# lib/ in the checkout.
rm -rf peregrine-libunistring .peregrine-libunistring.projeny.status
# setup, not extract: the checkout dir IS the projeny workdir, so a clean
# rebuild means erase-setup (rm the checkout + its status file) + fresh setup.
../filc/projeny setup peregrine-libunistring.projeny
cd peregrine-libunistring
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$PIZFIX --disable-shared --sysconfdir=/etc
make -j $NCPU
make -j $NCPU install
cd ..
craft_shadow $PIZFIX/lib/libunistring.a peregrine-libunistring/lib

# libidn2 (the base projeny checkout needed no Peregrine variant).  Its
# archive is assembled by libtool from libidn2's own lib/ and gl/ objects,
# the vendored unistring/ convenience library, and objects lifted from the
# installed libunistring ($(LTLIBUNISTRING) - e.g. libunistring_la-categ_none.o),
# so the shadow crafting searches all of those trees, in that order (the
# vendored unistring wins over the installed one, matching what libtool put
# in the archive).
rm -rf libidn2/extracted-source
../filc/projeny extract libidn2.projeny libidn2/extracted-source
cd libidn2/extracted-source
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$PIZFIX --sysconfdir=/etc --disable-shared
make -j $NCPU
make -j $NCPU install
cd ../..
craft_shadow $PIZFIX/lib/libidn2.a libidn2/extracted-source/lib libidn2/extracted-source/gl libidn2/extracted-source/unistring peregrine-libunistring/lib
rm -rf libidn2/extracted-source

# nghttp2 (the base projeny checkout needed no Peregrine variant)
rm -rf nghttp2/extracted-source
../filc/projeny extract nghttp2.projeny nghttp2/extracted-source
cd nghttp2/extracted-source
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$PIZFIX --sysconfdir=/etc --disable-shared --enable-lib-only
make -j $NCPU
make -j $NCPU install
cd ../..
craft_shadow $PIZFIX/lib/libnghttp2.a nghttp2/extracted-source/lib

# curl (the Peregrine projeny checkout: same AF_INET6-const treatment as
# openssl; see the .projeny header).  PKG_CONFIG_PATH is how configure finds
# the dep stack's pkgconfig files (openssl, nghttp2, zlib, zstd, libidn2);
# the libcrypto/libssl link tests in configure cannot link in cosmo mode
# (static archives with their own private deps), so preseed those cache vars
# (and the QUIC ones: the SSL_set_quic_* functions do not exist in this
# no-asm openssl build).  --disable-ipv6 keeps the remaining AF_INET6 case
# labels out of the build.  Only the lib/ and include/ subtrees are installed
# (matching the original build; the src/ subtree, i.e. the curl binary, does
# not build in the cosmo flavor - config2setopts.c has an AF_INET6 case
# label).
rm -rf peregrine-curl .peregrine-curl.projeny.status
# setup, not extract: the checkout dir IS the projeny workdir, so a clean
# rebuild means erase-setup (rm the checkout + its status file) + fresh setup.
../filc/projeny setup peregrine-curl.projeny
cd peregrine-curl
CC=$FILCSRC/build/bin/clang LIBS="-lidn2 -lunistring" \
    PKG_CONFIG_PATH=$PIZFIX/lib/pkgconfig \
    ac_cv_lib_crypto_HMAC_Update=yes \
    ac_cv_lib_ssl_SSL_connect=yes \
    ac_cv_func_SSL_set_quic_tls_cbs=no \
    ac_cv_func_SSL_set_quic_use_legacy_codepoint=no \
    ./configure --with-openssl --with-nghttp2 \
                --prefix=$PIZFIX --disable-shared \
                --with-ca-path=/etc/ssl/certs --sysconfdir=/etc \
                --enable-threaded-resolver --disable-ipv6
$MAKE -j $NCPU -C lib
$MAKE -j $NCPU -C lib install
$MAKE -j $NCPU -C include install
cd ..
craft_shadow $PIZFIX/lib/libcurl.a peregrine-curl/lib

cd ..
FILCSRC=$PWD

# ── yolo-cosmo closure ──────────────────────────────────────────────────────
# The slim libyolocosmo.a only carries the cosmo objects that the Fil-C
# payload (libpizlo.a, the yolo crt chain, the pizlonated libc.a/libc++*.a)
# can reference; the object list is computed by compute-closure.py from the
# consumers currently in pizfix.  Regenerate both make fragments and, only if
# one actually changed, rebuild + reinstall libyolocosmo.a (both
# architectures) so the slim archive tracks the new closure.
# Clean up the snapshot copies no matter how the script exits: under set -e
# a compute-closure.py or build_yolocosmo.sh failure would otherwise skip a
# standalone rm -f and leave the /tmp snapshots behind (the trap covers the
# success path too).  Registered here, immediately before the snapshots are
# created; this is the script's only EXIT trap.
trap 'rm -f /tmp/yosh-filc-objects.before /tmp/yosh-filc-objects-aarch64.before' EXIT
cp projects/yolocosmo/filc/filc-objects.mk /tmp/yosh-filc-objects.before 2>/dev/null || true
cp projects/yolocosmo/filc/filc-objects-aarch64.mk /tmp/yosh-filc-objects-aarch64.before 2>/dev/null || true
python3 projects/yolocosmo/filc/compute-closure.py --mode x86_64-ape
python3 projects/yolocosmo/filc/compute-closure.py --mode aarch64-ape
if ! cmp -s projects/yolocosmo/filc/filc-objects.mk /tmp/yosh-filc-objects.before \
   || ! cmp -s projects/yolocosmo/filc/filc-objects-aarch64.mk /tmp/yosh-filc-objects-aarch64.before
then
    echo "yolo-cosmo closure changed; rebuilding libyolocosmo.a"
    ./build_yolocosmo.sh
else
    echo "yolo-cosmo closure unchanged; not rebuilding libyolocosmo.a"
fi
# The snapshot copies were only needed for the comparison above; the EXIT
# trap registered before they were created removes them on every path.

# ── readline ────────────────────────────────────────────────────────────────
# readline: distclean first so that stale objects from any previous build
# (possibly built with an ancient Fil-C ABI or the other libc flavor) can
# never survive into the link.
cd $YOSH/readline-8.3.6
make distclean || true
make clean || true
# readline's distclean doesn't know about the driver's .aarch64/ shadow
# directories either, so a previous run's stale shadow objects would survive
# and get crafted into the fresh shadow archives below (the same sweep the
# bash section does - this just keeps the build honest).
find . -name .aarch64 -type d -prune -exec rm -rf {} +
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ ./configure --prefix=$FILCSRC/pizfix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install
craft_readline_shadows $YOSH/readline-8.3.6

# ── bash ────────────────────────────────────────────────────────────────────
# bash: distclean for the same reason. CPPFLAGS=-I$FILCSRC/pizfix/include makes
# the installed-readline version test compile, so configure genuinely uses
# readline 8.3 from pizfix instead of silently falling back to the bundled one.
# The cosmo driver links fat APEs by default, so no extra flags are needed
# for the fat build: -static is a no-op (cosmo links are static non-PIE) and
# the link produces yosh plus the yosh.com.dbg / yosh.aarch64.elf side files
# next to it (bash's Makefile.in names the program yosh, so `make install`
# already puts the fat APE at pizfix/bin/yosh).
cd $YOSH/bash-5.3.20
make distclean || true
make clean || true
# bash's distclean doesn't know about the driver's .aarch64/ shadow
# directories, so a previous run's stale shadow archives would survive and
# make the first yosh link degrade to an x86_64-only APE with a -Wfilc-ape
# warning (the shadow crafting + re-link below would fix it anyway - this
# just keeps the build honest).
find . -name .aarch64 -type d -prune -exec rm -rf {} +
CC=$FILCSRC/build/bin/clang CXX=$FILCSRC/build/bin/clang++ \
    CPPFLAGS="-I$FILCSRC/pizfix/include" \
    LDFLAGS="-static" LIBS="-lreadline -lncurses -lcurl -lnghttp2 -lidn2 -lunistring -lssl -lcrypto -lz -lzstd -lm" \
    ./configure --prefix=$FILCSRC/pizfix --without-bash-malloc --with-installed-readline
# The checked-in configure is the source of truth (the tree does not require
# autoconf at build time); keep make from deciding to regenerate it just
# because config.h.in was edited more recently than configure's checkout.
touch ./configure
make -j `nproc`
# bash archives its helper libraries (libbuiltins, libglob, libsh, libtilde,
# libintl) with plain ar, so the aarch64 half of the fat-APE link finds no
# shadow archives for them (the driver only creates shadow objects, not
# archives); craft those shadows from the .aarch64/ shadow objects next to
# bash's own objects and re-link, so the build produces a real fat APE
# instead of degrading to an x86_64-only APE with a -Wfilc-ape warning.
craft_bash_shadows $YOSH/bash-5.3.20
rm -f yosh
make yosh
make -j `nproc` install

# Bring the ELF side files (written next to the link output in the build
# tree) along under the yosh name too, so the APE and both of its halves are
# runnable straight from pizfix/bin.
rm -f $FILCSRC/pizfix/bin/yosh.com.dbg $FILCSRC/pizfix/bin/yosh.aarch64.elf
cp -f $YOSH/bash-5.3.20/yosh.com.dbg $FILCSRC/pizfix/bin/yosh.com.dbg
cp -f $YOSH/bash-5.3.20/yosh.aarch64.elf $FILCSRC/pizfix/bin/yosh.aarch64.elf

# The APE tooling validates the PE headers of the fat build; fail the build
# if the APE is malformed.
$FILCSRC/pizfix/libexec/pecheck $FILCSRC/pizfix/bin/yosh
echo "yosh fat APE installed: $FILCSRC/pizfix/bin/yosh"

