#!/usr/bin/env bash

set -e

# build specified file or all files
curDir=${PWD}
buildDir="$curDir/../bin"

if [ -z $1 ]; then
  files=`ls *.c`
else
  files=./$1
fi

echo $files

CommonCompilerFlags="-O2 -ggdb -fdiagnostics-color=always -fno-rtti -fno-exceptions -msse4.1 -ffast-math
-Wall -Werror -Wconversion -Wno-writable-strings -Wno-gnu-anonymous-struct -Wno-padded -Wno-string-conversion
-Wno-error=unused-variable -Wno-unused-function"

CommonDefines=""
CommonLinkerFlags="-Wl,--gc-sections -nostdinc++ -ldl"


[ -d $buildDir ] || buildDir="$curDir"

pushd $buildDir > /dev/null

for entry in $files
do
  # 64 Bit Build
  # PIC = Position Independent Code
  if [ ${entry: -4} == ".cpp" ]; then
    clang++ -std=c++11 $CommonCompilerFlags $CommonDefines $curDir/${entry} -o ${entry%.*} $CommonLinkerFlags
  else
    clang $CommonCompilerFlags $CommonDefines $curDir/${entry} -o ${entry%.*} $CommonLinkerFlags
  fi
done

popd > /dev/null
