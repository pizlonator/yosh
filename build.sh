extract_source()
{
    rm -rf extracted-source
    git archive --format=tar HEAD --prefix=extracted-source/ | tar -xf -
    git diff --relative HEAD . | (cd extracted-source && patch -p1)
    cd extracted-source
}

cd readline-8.2.13
extract_source
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ ./configure --prefix=$PWD/../../prefix --with-curses --disable-shared
make -j `nproc`
make -j `nproc` install
cd ../..

cd bash-5.2.32
extract_source
CC=/opt/fil/bin/filcc CXX=/opt/fil/bin/fil++ CPPFLAGS=-I$PWD/../../prefix/include LDFLAGS=-L$PWD/../../prefix/lib ./configure --prefix=$PWD/../../prefix --without-bash-malloc --with-installed-readline
make -j `nproc`
make -j `nproc` install

