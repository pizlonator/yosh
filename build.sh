#!/bin/sh
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
# Thin wrapper: yosh is now a Peregrine-only project (it builds against the
# cosmo flavor of Fil-C into a fat APE), and the old musl-flavor build this
# script used to drive - the /opt/fil/bin/filcc toolchain and the local
# prefix/ tree - no longer exists.  All this does is run the canonical
# pipeline: ../yosh/build_static.sh from the fil-c checkout root next to
# this tree.  Paths are computed from the script's own location, so it may
# be invoked from any directory.

yosh_dir=`cd "$(dirname "$0")" && pwd`
filc_root=$yosh_dir/../fil-c-5
if ! test -d "$filc_root"
then
    echo "build.sh: cannot find the fil-c checkout (expected at $filc_root)" 1>&2
    exit 1
fi
cd "$filc_root" || exit 1
exec "$yosh_dir/build_static.sh"

