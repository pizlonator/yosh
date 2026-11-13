#!/bin/bash
#
# Copyright (c) 2026 Filip Pizlo. All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY FILIP PIZLO ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL FILIP PIZLO OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Incremental rebuild of yosh: rebuilds just readline and bash (the halves
# of yosh that live in this tree) and reinstalls them into pizfix, for
# code-only changes, after the first full build (cd fil-c-5 &&
# ../yosh/build_static.sh).
#
# Contract: run against the yosh tree (the script cds to its own directory,
# which is the yosh root, so it may be invoked from anywhere as long as
# FILCSRC resolves).  It is a plain make-based rebuild: the readline and
# bash configure results from the full build are reused as-is.
#
# NOTE: this script does NOT regenerate the yolo-cosmo closure.  If a future
# link fails on an undefined yolo-cosmo symbol, re-running build_static.sh
# is the fix: it regenerates the closure via compute-closure.py and rebuilds
# libyolocosmo.a when the closure changed.
#
# FILCSRC  the fil-c checkout root.  Defaults to the fil-c-5 checkout next
#          to this tree (i.e. <yosh>/../fil-c-5); override via the
#          environment.  PIZFIX is then $FILCSRC/pizfix.
#
# Why this script recrafts shadow archives: `make install` installs fresh
# primary (x86_64) archives into pizfix, and the Fil-C driver does NOT
# staleness-check aarch64 shadow archives for -l<name> links - without the
# craft_readline_shadows / craft_bash_shadows calls (shared with
# build_static.sh via craft_shadows.sh), an incremental rebuild would relink
# yosh against stale aarch64 shadow archives and silently ship an
# x86_64/aarch64 APE whose aarch64 half is built from old code.  Hence each
# half crafts its fresh shadows right after its `make install`, bash is
# re-linked (rm -f yosh && make yosh, exactly like build_static.sh), and the
# result is hard-gated with pizfix/libexec/pecheck so a degraded
# x86_64-only relink fails loudly instead of shipping a single-arch yosh.

set -e
set -x

yosh_dir=`cd "$(dirname "$0")" && pwd`
cd "$yosh_dir"

FILCSRC=${FILCSRC:-$yosh_dir/../fil-c-5}
if ! test -d "$FILCSRC"
then
    echo "build_incremental: FILCSRC='$FILCSRC' is not a directory; point it at the fil-c checkout root" 1>&2
    exit 1
fi
FILCSRC=`cd "$FILCSRC" && pwd`
PIZFIX=$FILCSRC/pizfix

. "$yosh_dir/craft_shadows.sh"

# The cosmo sysroot must exist: build_base_cosmo.sh builds the compiler, the
# yolo cosmo libc, and the APE tooling; build_static.sh (run once) builds the
# pizlonated cosmo libc, the yosh dep stack, and the initial yosh itself.
if ! test -f $PIZFIX/lib/libyolocosmo.a
then
    echo "build_incremental: $PIZFIX/lib/libyolocosmo.a does not exist." 1>&2
    echo "Run fil-c's ./build_base_cosmo.sh first, then the initial full build:" 1>&2
    echo "    cd $FILCSRC && ../yosh/build_static.sh" 1>&2
    exit 1
fi

# ── readline ────────────────────────────────────────────────────────────────
cd readline-8.3.6
make -j `nproc`
make -j `nproc` install
# Fresh primaries are in pizfix/lib now; craft their aarch64 shadows so the
# yosh relink below cannot pick up stale ones (-l links skip the staleness
# check; see the header comment).
craft_readline_shadows $PWD

# ── bash ────────────────────────────────────────────────────────────────────
cd ../bash-5.3.20
touch bashline.c # force relink
make -j `nproc`
# The first yosh link above may emit -Wfilc-ape noise: bash's helper archives
# have no shadow archives until they are crafted below, and its first link
# happens before that.  Expected; the relink after crafting fixes it.
craft_bash_shadows $PWD
rm -f yosh
make yosh
make -j `nproc` install

# Bring the ELF side files (written next to the link output in the build
# tree) along under the yosh name too, so the APE and both of its halves are
# runnable straight from pizfix/bin (same post-install steps as
# build_static.sh).
rm -f $PIZFIX/bin/yosh.com.dbg $PIZFIX/bin/yosh.aarch64.elf
cp -f yosh.com.dbg $PIZFIX/bin/yosh.com.dbg
cp -f yosh.aarch64.elf $PIZFIX/bin/yosh.aarch64.elf

# Hard gate: the APE tooling validates the PE headers of the fat build, so a
# degraded x86_64-only relink fails the build instead of shipping silently.
$PIZFIX/libexec/pecheck $PIZFIX/bin/yosh
echo "yosh fat APE updated: $PIZFIX/bin/yosh"

