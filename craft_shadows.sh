# craft_shadows.sh - shared aarch64 shadow-archive crafting for yosh's build
# scripts.  This file is a sourced sh fragment, NOT an executable script:
# build_static.sh and build_incremental.sh both do
#
#     . "$(cd "$(dirname "$0")" && pwd)/craft_shadows.sh"
#
# to get the craft_shadow / craft_readline_shadows / craft_bash_shadows
# functions below.
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
# Environment expectations - the sourcing script must set, before sourcing:
#
#     FILCSRC  the fil-c checkout root.  The shadow objects that these
#              functions assemble into shadow archives were produced by
#              $FILCSRC/build/bin/clang (the Fil-C driver creates an
#              aarch64 shadow object next to every .c compile in cosmo
#              mode).  The functions below consult only $PIZFIX, but the
#              sourcing scripts set both so the pair stays consistent.
#     PIZFIX   $FILCSRC/pizfix - the cosmo sysroot whose lib/ holds the
#              primary (x86_64) archives, whose lib/.aarch64/ holds the
#              shadow archives next to their primaries, and whose
#              lib-aarch64/ mirrors the shadows of everything under lib/.
# Crafts the aarch64 shadow archive of a static archive:
#
#     craft_shadow <archive> <search-dir>...
#
# <archive> is the primary (x86_64) archive, e.g. $PIZFIX/lib/libz.a; its
# shadow is installed next to it, at <archive's dir>/.aarch64/<name>.a.  (An
# archive under pizfix/lib additionally gets the shadow mirrored to
# pizfix/lib-aarch64/<name>.a, the default library root of the aarch64 half
# of a cosmo-mode link.)  For every member of the primary, this locates the
# aarch64 shadow object that the Fil-C driver created next to the dep's own
# object (always in a .aarch64/ subdirectory), and assembles the shadow
# archive in the primary's member order.  The search dirs are consulted in
# order, and each dir is searched ONLY for the member's exact shadow path
# <dir>/**/.aarch64/<member>: the find below looks at nothing but files
# under a .aarch64/ directory, so it can never match the non-shadow primary
# object, and the per-dir candidate filter additionally requires the path to
# END in /.aarch64/<member>, so a member whose name is a suffix of another
# member's name (fsync.o vs. __fsync.o) cannot steal that member's object.
# Exactly one match in a dir is used; more than one match in a dir is a hard
# error immediately (silently falling through to the next dir would be
# guessing); no match in a dir just moves on to the next dir; exhausting
# all dirs without a match anywhere is a hard error too; and a missing
# primary archive is a hard error as well, checked before anything else
# (`ar t` on a missing archive would otherwise fail inside the member
# word-list and silently yield an empty member list).  Any of these hard
# errors is fatal (the function returns nonzero), since a shadow archive
# built from the wrong objects, or with missing members, or empty, would
# make the aarch64 half of a link silently mismatched or degraded.
craft_shadow()
{
    archive=$1
    shift
    name=`basename $archive .a`
    # Hard-error guard, before ANY temp/index files are created so the error
    # path needs no cleanup: without it, `ar t $archive` below fails inside
    # the member word-list, yielding zero members with ok=1, and the
    # function would craft (and mirror) an EMPTY shadow archive as if fine.
    # return 1, not exit, so a sourcing script under set -e aborts with a
    # clear message.
    test -f "$archive" || { echo "craft_shadow $name: primary archive $archive does not exist" 1>&2; return 1; }
    dir=`dirname $archive`
    shadowdir=$dir/.aarch64
    idxbase=$PIZFIX/.craft-shadow-index
    nidx=0
    for sdir in "$@"
    do
        nidx=`expr $nidx + 1`
        find $sdir -type f -path "*/.aarch64/*" -name "*.o" > $idxbase.$nidx 2>/dev/null || true
    done
    list=
    ok=1
    for member in `ar t $archive`
    do
        found=
        i=0
        for sdir in "$@"
        do
            i=`expr $i + 1`
            cands=`grep -F "/.aarch64/$member" $idxbase.$i 2>/dev/null || true`
            # Keep only the candidates whose path ends exactly in
            # /.aarch64/<member>: grep -F alone would also match the shadow
            # object of a longer member whose name has <member> as a suffix.
            count=0
            exact=
            for cand in $cands
            do
                case $cand in
                */.aarch64/$member)
                    exact="$exact $cand"
                    count=`expr $count + 1`
                    ;;
                esac
            done
            if test "$count" -gt 1
            then
                echo "craft_shadow $name: search dir $sdir has ambiguous aarch64 shadow objects for member $member:" 1>&2
                for cand in $exact
                do
                    echo "  $cand" 1>&2
                done
                ok=0
                break
            fi
            if test "$count" = "1"
            then
                found=$exact
                break
            fi
            # No match in this dir; fall through to the next search dir.
        done
        if test "$ok" != "1"
        then
            break
        fi
        if test -z "$found"
        then
            echo "craft_shadow $name: no aarch64 shadow object for member $member in any of the search dirs:" 1>&2
            for sdir in "$@"
            do
                echo "  $sdir" 1>&2
            done
            ok=0
            break
        fi
        list="$list $found"
    done
    i=0
    while test $i -lt $nidx
    do
        i=`expr $i + 1`
        rm -f $idxbase.$i
    done
    if test "$ok" != "1"
    then
        return 1
    fi
    mkdir -p $shadowdir
    rm -f $shadowdir/$name.a
    # shellcheck disable=SC2086 -- the member paths never contain whitespace.
    ar crs $shadowdir/$name.a $list
    # A link accepts a shadow only if it is at least as new as its primary
    # object (see cosmo.txt); touch so a freshly installed primary can never
    # make the shadow look stale.
    touch $shadowdir/$name.a
    if test "$dir" = "$PIZFIX/lib"
    then
        mkdir -p $PIZFIX/lib-aarch64
        cp -f $shadowdir/$name.a $PIZFIX/lib-aarch64/$name.a
        touch $PIZFIX/lib-aarch64/$name.a
    fi
}

# Crafts the shadow archives of readline's two installed archives:
#
#     craft_readline_shadows <readline-tree>
#
# <readline-tree> is readline's build tree (e.g. $YOSH/readline-8.3.6, the
# same search-dir argument build_static.sh uses): every object that lands in
# libreadline.a or libhistory.a lives directly in that tree, with its
# .aarch64/ shadow object next to it.  Both primaries are installed into
# $PIZFIX/lib by readline's `make install`, so both shadows land in
# $PIZFIX/lib/.aarch64/ and mirror to $PIZFIX/lib-aarch64/ (see craft_shadow).
craft_readline_shadows()
{
    tree=$1
    craft_shadow $PIZFIX/lib/libreadline.a $tree
    craft_shadow $PIZFIX/lib/libhistory.a $tree
}

# Crafts the shadow archives of bash's five helper archives:
#
#     craft_bash_shadows <bash-tree>
#
# <bash-tree> is bash's build tree (e.g. $YOSH/bash-5.3.20).  bash's
# Makefile archives its helper libraries (libbuiltins, libglob, libsh,
# libtilde, libintl) with plain ar, so the aarch64 half of the fat-APE link
# finds no shadow archives for them (the Fil-C driver only creates shadow
# objects, not archives); this crafts each one from the .aarch64/ shadow
# objects next to the archive's own objects.  The helper archives are linked
# by explicit path (unlike readline/curl/etc., which resolve via -l), so
# their shadows must exist in the build tree, not in pizfix (see cosmo.txt).
craft_bash_shadows()
{
    tree=$1
    for lib in builtins/libbuiltins.a lib/glob/libglob.a lib/sh/libsh.a lib/tilde/libtilde.a lib/intl/libintl.a
    do
        craft_shadow $tree/$lib $tree/`dirname $lib`
    done
}
