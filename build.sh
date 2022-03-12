#!/usr/bin/env bash

set -e

BASE_DIR="$PWD"
BUILD_DIR="${BASE_DIR}/build"
DEPLOY_DIR="${BASE_DIR}/../bin"

# NOTE(dgl): Load env variables if file is available
[ -f "${BASE_DIR}/ENV" ] && . "${BASE_DIR}/ENV"

# NOTE(dgl): check if tools are available
[ -d "${BUILD_DIR}" ] || mkdir -p "${BUILD_DIR}"

CommonCompilerFlags="-O0 -g -ggdb -fdiagnostics-color=always -fno-rtti -fno-exceptions -ffast-math -msse4.1 -msse2
-Wall -Werror -Wconversion
-Wno-writable-strings -Wno-gnu-anonymous-struct
-Wno-padded -Wno-string-conversion
-Wno-error=sign-conversion
-Wno-error=unused-variable
-Wno-error=unused-function
-Wno-error=unused-command-line-argument"

CommonDefines=""
CommonLinkerFlags="-Wl,--gc-sections -nostdinc++ -ldl"

fetch() {
    echo "Fetching dependencies"
}

build() {
    FILES="$1"
    [ -z "${FILES}" ] && FILES=$(ls *.c)

    for entry in $FILES
    do
        echo "Build application ${entry}"
        # 64 Bit Build
        # PIC = Position Independent Code
        if [ ${entry: -4} == ".cpp" ]; then
            clang++ -std=c++11 $CommonCompilerFlags $CommonDefines $BASE_DIR/${entry} -o ${BUILD_DIR}/${entry%.*} $CommonLinkerFlags
        else
            clang -std=c11 $CommonCompilerFlags $CommonDefines $BASE_DIR/${entry} -o ${BUILD_DIR}/${entry%.*} $CommonLinkerFlags
        fi
    done
}

deploy() {
    FILES="$1"
    [ -z "${FILES}" ] && FILES=$(ls *.c)
    for entry in $FILES
    do
        echo "Deploying application ${entry%.*}"
        cp ${BUILD_DIR}/${entry%.*} ${DEPLOY_DIR}/${entry%.*}
        strip ${DEPLOY_DIR}/${entry%.*}
    done
}

launch() {
    FILE="$1"
    [ -z "${FILE}" ] && { echo "Please pass the c file of the tool you want to debug."; exit 1; }

    ARGS="${@:2}"
    echo "Launch application ${FILE%.*}"
    "${BUILD_DIR}/${FILE%.*}" $ARGS
}

debug() {
    FILE="$1"
    [ -z "${FILE}" ] && { echo "Please pass the c file of the tool you want to debug."; exit 1; }

    ARGS="${@:2}"
    echo "Debug application ${FILE%.*}"

    GDB_FRONTEND="$(which gf2)"

    if [ -z "${GDB_FRONTEND}" ]; then
        gdb "${BUILD_DIR}/${FILE%.*}"
    else
        gf2 "${BUILD_DIR}/${FILE%.*}"
    fi
}

clean() {
    echo "Removing build directory"
    rm -rf ${BUILD_DIR}
}

COMMAND="$1"

if [ -z ${COMMAND} ]; then
    fetch
    build
else
    shift
    ARGS="$@"

    case "${COMMAND}" in
    fetch)
        fetch ${ARGS}
    ;;

    run)
        fetch  ${ARGS}
        build  ${ARGS}
        launch ${ARGS}
    ;;

    build)
        fetch ${ARGS}
        build ${ARGS}
    ;;

    launch)
        launch ${ARGS}
    ;;

    debug)
        fetch ${ARGS}
        build ${ARGS}
        debug ${ARGS}
    ;;

    deploy)
        fetch  ${ARGS}
        build  ${ARGS}
        deploy ${ARGS}
    ;;

    all)
        fetch
        build
        deploy
    ;;

    help)
        echo "Usage: $(basename "$0") [command]"
        echo "By default build, install and run ${BINARY}."
        echo
        echo "Optional [command] can be:"
        echo "  fetch                     - download dependent libraries"
        echo "  run                       - build and launch application"
        echo "  build                     - build application"
        echo "  deploy                     - deploy application"
        echo "  launch                    - only start application"
        echo "  debug                     - run in gdb"
    ;;

    *)
        fetch
        build
    ;;

    esac
fi
