#!/bin/bash

set -e # stop executing after error

# Print help and syntax
print_help() {
    echo "Script for building srsGNB"
    echo
    echo "Syntax: builder.sh -args /folder"
    echo "args:"
    echo "  -h | --help                 - Print his message"
    echo "  -b | --build-folder         - Build folder. default \$workdir/build"
    echo "  -x | --cache-folder         - Cache folder. default \$workdir/ccache"
    echo "  -r | --clean-build          - Clean build folder before build. default True"
    echo "  -c | --compiler             - Compiler tool (gcc or clang)"
    echo "  -m | --make                 - Args to send to make tool"
    echo "  -u | --uhd                  - UHD version"
    echo "  -d | --dpdk                 - DPDK version"
    echo "  others                      - Extra args will be sent to 'cmake'"
    echo "folder:"
    echo "  Folder where source code is located. It should be the last parameter"
    echo ""
}

# Variables
FOLDER=""
BUILD_FOLDER="build"
CACHE_FOLDER="ccache"
CLEAN_BUILD="True"
MAKE_EXTRA="-j $(nproc)"
COMPILER="gcc"
UHD_VERSION=""
DPDK_VERSION=""

# Begin the parsing
while (("$#")); do
    case "$1" in
    -h | --help)
        print_help
        exit 0
        ;;
    -b | --build-folder)
        if [ -n "$2" ]; then
            BUILD_FOLDER="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -r | --clean-build)
        if [ -n "$2" ]; then
            CLEAN_BUILD="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -m | --make)
        if [ -n "$2" ]; then
            MAKE_EXTRA="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -x | --cache-folder)
        if [ -n "$2" ]; then
            CACHE_FOLDER="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -c | --compiler)
        if [ -n "$2" ]; then
            COMPILER=$(echo "$2" | tr '[:upper:]' '[:lower:]')
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -u | --uhd)
        if [ -n "$2" ]; then
            UHD_VERSION="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    -d | --dpdk)
        if [ -n "$2" ]; then
            DPDK_VERSION="$2"
            shift 2
        else
            echo "Error: Argument for $1 is missing" >&2
            echo "" >&2
            print_help
            exit 1
        fi
        ;;
    *)
        break
        ;;
    esac
done
FOLDER="${*: -1}"
set -- "${@:1:$#-1}"

case $COMPILER in
gcc)
    export CC=/usr/bin/gcc
    export CXX=/usr/bin/g++
    ;;
clang)
    export CC=/usr/bin/clang
    export CXX=/usr/bin/clang++
    ;;
*)
    echo "Error: Invalid compiler $COMPILER" >&2
    echo "" >&2
    print_help
    exit 1
    ;;
esac

cd "$FOLDER" || exit

# Setup UHD
uhd_system_version=$(uhd_config_info --version) && regex="^UHD ([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+).*" && [[ $uhd_system_version =~ $regex ]]
uhd_system_version="${BASH_REMATCH[1]}"
if [[ -z "$UHD_VERSION" || "$UHD_VERSION" == "$uhd_system_version" ]]; then
    echo "Using default UHD $uhd_system_version"
else
    [ ! -d "/opt/uhd/$UHD_VERSION" ] && echo "UHD version not found" && exit 1
    export UHD_DIR="/opt/uhd/$UHD_VERSION"
    echo "UHD_DIR set to $UHD_DIR"
fi

# Setup DPDK
if [[ -n "$DPDK_VERSION" ]]; then
    [ ! -d "/opt/dpdk/$DPDK_VERSION" ] && echo "DPDK version not found" && exit 1
    export DPDK_DIR="/opt/dpdk/$DPDK_VERSION"
    echo "DPDK_DIR set to $DPDK_DIR"
else
    echo "DPDK not set"
fi

# Setup cache dir
mkdir -p "$CACHE_FOLDER"
export CCACHE_BASEDIR=${PWD}
export CCACHE_DIR=${PWD}/${CACHE_FOLDER}
export CCACHE_COMPILERCHECK=content

# Clean build dir
if [[ "$CLEAN_BUILD" == "True" ]]; then
    echo "Cleaning build directory: $BUILD_FOLDER"
    rm -Rf "$BUILD_FOLDER"
else
    echo "Skip cleaning of build directory: $BUILD_FOLDER"
fi
# Use ccache if available
(which ccache >/dev/null) && CCACHE_CMAKE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DAUTO_DETECT_ISA=False" || CCACHE_CMAKE_ARGS="-DENABLE_EXPORT=ON -DENABLE_ZEROMQ=ON -DAUTO_DETECT_ISA=False"

# Build process
mkdir -p "$BUILD_FOLDER"
cd "$BUILD_FOLDER" || exit
cmake $CCACHE_CMAKE_ARGS "$@" ..
make $MAKE_EXTRA
