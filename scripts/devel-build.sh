#!/bin/bash

set -euxo pipefail
cd "${0%/*}"
cd ..

# Sanity checks
if [ ! -e "CMakeLists.txt" ] || [ ! -e "libubus.c" ]; then
	echo "ubus checkout not found" >&2
	exit 1
fi

if [ $# -eq 0 ]; then
	BUILD_ARGS="-DBUILD_LUA=ON -DUNIT_TESTING=ON"
else
	BUILD_ARGS="$@"
fi

# Create build dirs
UBUSDIR="$(pwd)"
BUILDDIR="${UBUSDIR}/build"
DEPSDIR="${BUILDDIR}/depends"
[ -e "${BUILDDIR}" ] || mkdir "${BUILDDIR}"
[ -e "${DEPSDIR}" ] || mkdir "${DEPSDIR}"

# Prepare env
export LD_LIBRARY_PATH="${BUILDDIR}/lib:${LD_LIBRARY_PATH:-}"
export PATH="${BUILDDIR}/bin:${PATH:-}"

# Download deps
cd "${DEPSDIR}"
[ -e "json-c" ] || git clone https://github.com/json-c/json-c.git
[ -e "libubox" ] || git clone https://github.com/openwrt/libubox.git
if [ ! -e "lua" ]; then
	mkdir -p lua
	wget -qO- https://www.lua.org/ftp/lua-5.1.5.tar.gz | \
		tar zxvf - -C lua --strip-components=1
	sed -i '/#define LUA_USE_READLINE/d' ./lua/src/luaconf.h
	sed -i 's/ -lreadline -lhistory -lncurses//g' ./lua/src/Makefile
fi

# Build lua
cd "${DEPSDIR}/lua"
make linux install \
	INSTALL_TOP="${BUILDDIR}"

# Build json-c
cd "${DEPSDIR}/json-c"
cmake							\
	-S .						\
	-B .						\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	-DBUILD_SHARED_LIBS=OFF				\
	-DDISABLE_EXTRA_LIBS=ON				\
	-DBUILD_TESTING=OFF				\
	--install-prefix "${BUILDDIR}"
make
make install

# Build libubox
cd "${DEPSDIR}/libubox"
cmake							\
	-S .						\
	-B .						\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	-DBUILD_LUA=ON					\
	-DBUILD_EXAMPLES=OFF				\
	-DLUAPATH=${BUILDDIR}/lib/lua			\
	--install-prefix "${BUILDDIR}"
make
make install

# Build ubus
cd "${UBUSDIR}"
cmake							\
	-S .						\
	-B "${BUILDDIR}"				\
	-DCMAKE_PREFIX_PATH="${BUILDDIR}"		\
	-DLUAPATH=${BUILDDIR}/lib/lua			\
	--install-prefix "${BUILDDIR}"			\
	${BUILD_ARGS}
make -C "${BUILDDIR}"
make -C "${BUILDDIR}" install

# Test ubus
make -C "${BUILDDIR}" test CTEST_OUTPUT_ON_FAILURE=1

set +x
echo "✅ Success - ubus is available at ${BUILDDIR}"
echo "👷 You can rebuild ubus by running 'make -C build'"

exit 0
