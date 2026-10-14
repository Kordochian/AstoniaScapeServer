#!/bin/sh
#
# Builds the Windows server from source, including the libraries it needs.
#
# On Windows: open the "MSYS2 MINGW64" shell, go to the server folder and run
#
#     pacman -S --needed git make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
#     ./build-windows.sh
#
# From Linux with the MinGW-w64 cross compiler:
#
#     CROSS=x86_64-w64-mingw32 ./build-windows.sh
#
# The libraries (zlib, argon2, MariaDB Connector/C) are downloaded into
# windeps/src, built once and installed into windeps. They are linked into the
# executables, so the finished server needs no extra DLLs. Delete windeps to
# rebuild them.

set -e

ZLIB_TAG=v1.3.2
ARGON2_TAG=20190702
MARIADB_TAG=v3.4.9

TOP=$(pwd)
DEPS=$TOP/windeps
JOBS=${JOBS:-4}

if [ -n "$CROSS" ]; then
    CC=${CC:-$CROSS-gcc-posix}
    AR=${AR:-$CROSS-ar}
    mkdir -p "$DEPS"
    cat > "$DEPS/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_RC_COMPILER $CROSS-windres)
set(CMAKE_FIND_ROOT_PATH $DEPS)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    CMAKE_TC="-DCMAKE_TOOLCHAIN_FILE=$DEPS/toolchain.cmake"
else
    CC=${CC:-gcc}
    AR=${AR:-ar}
    CMAKE_TC=""
fi

fetch() { # fetch <name> <url> <tag>
    if [ ! -d "$DEPS/src/$1" ]; then
        git clone --depth 1 --branch "$3" "$2" "$DEPS/src/$1"
    fi
}

mkdir -p "$DEPS/src" "$DEPS/include" "$DEPS/lib"

if [ ! -f "$DEPS/lib/libzs.a" ]; then
    echo "=== zlib $ZLIB_TAG"
    fetch zlib https://github.com/madler/zlib "$ZLIB_TAG"
    cmake -S "$DEPS/src/zlib" -B "$DEPS/src/zlib/build" -G Ninja $CMAKE_TC \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_SHARED=OFF
    cmake --build "$DEPS/src/zlib/build" -j "$JOBS"
    cmake --install "$DEPS/src/zlib/build"
    # zlib names its static library libzs.a on Windows from 1.3.2 on, but
    # libz.a in older versions
    [ -f "$DEPS/lib/libzs.a" ] || cp "$DEPS/lib/libz.a" "$DEPS/lib/libzs.a"
fi

if [ ! -f "$DEPS/lib/libargon2.a" ]; then
    echo "=== argon2 $ARGON2_TAG"
    fetch argon2 https://github.com/P-H-C/phc-winner-argon2 "$ARGON2_TAG"
    A=$DEPS/src/argon2
    mkdir -p "$A/build"
    for f in argon2 core encoding thread blake2/blake2b ref; do
        $CC -O2 -I"$A/include" -I"$A/src" -c "$A/src/$f.c" -o "$A/build/$(basename $f).o"
    done
    rm -f "$DEPS/lib/libargon2.a"
    $AR rcs "$DEPS/lib/libargon2.a" "$A"/build/*.o
    cp "$A/include/argon2.h" "$DEPS/include/"
fi

if [ ! -f "$DEPS/lib/mariadb/libmariadbclient.a" ]; then
    echo "=== MariaDB Connector/C $MARIADB_TAG"
    fetch mariadb https://github.com/mariadb-corporation/mariadb-connector-c "$MARIADB_TAG"
    # SCHANNEL is the TLS library built into Windows, so no OpenSSL is needed.
    cmake -S "$DEPS/src/mariadb" -B "$DEPS/src/mariadb/build" -G Ninja $CMAKE_TC \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DWITH_SSL=SCHANNEL -DWITH_UNIT_TESTS=OFF -DWITH_CURL=OFF \
        -DWITH_EXTERNAL_ZLIB=ON -DZLIB_INCLUDE_DIR="$DEPS/include" -DZLIB_LIBRARY="$DEPS/lib/libzs.a" \
        -DCLIENT_PLUGIN_DIALOG=OFF -DCLIENT_PLUGIN_AUTH_GSSAPI_CLIENT=OFF \
        -DDEFAULT_SSL_VERIFY_SERVER_CERT=OFF
    cmake --build "$DEPS/src/mariadb/build" -j "$JOBS"
    cmake --install "$DEPS/src/mariadb/build"
    # the server includes <mysql/mysql.h>
    rm -rf "$DEPS/include/mysql"
    cp -r "$DEPS/include/mariadb" "$DEPS/include/mysql"
fi

echo "=== server"
make -f Makefile.win -j "$JOBS" CC="$CC" WINDEPS="$DEPS"

echo
echo "Done: server35.exe chatserver.exe create_account.exe create_character.exe runtime/"
