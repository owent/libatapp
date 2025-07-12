#!/bin/bash

cd "$(cd "$(dirname $0)" && pwd)/.."

set -ex

if [[ -z "$CONFIGURATION" ]]; then
  CONFIGURATION=RelWithDebInfo
fi

if [[ "x$USE_CC" == "xclang-latest" ]]; then
  echo '#include <iostream>
  int main() { std::cout<<"Hello"; }' >test-libc++.cpp
  SELECT_CLANG_VERSION=""
  SELECT_CLANG_HAS_LIBCXX=1
  clang -x c++ -stdlib=libc++ test-libc++.cpp -lc++ -lc++abi || SELECT_CLANG_HAS_LIBCXX=0
  if [[ $SELECT_CLANG_HAS_LIBCXX -eq 0 ]]; then
    CURRENT_CLANG_VERSION=$(clang -x c /dev/null -dM -E | grep __clang_major__ | awk '{print $NF}')
    for ((i = $CURRENT_CLANG_VERSION + 5; $i >= $CURRENT_CLANG_VERSION; --i)); do
      SELECT_CLANG_HAS_LIBCXX=1
      SELECT_CLANG_VERSION="-$i"
      clang$SELECT_CLANG_VERSION -x c++ -stdlib=libc++ test-libc++.cpp -lc++ -lc++abi || SELECT_CLANG_HAS_LIBCXX=0
      if [[ $SELECT_CLANG_HAS_LIBCXX -eq 1 ]]; then
        break
      fi
    done
  fi
  SELECT_CLANGPP_BIN=clang++$SELECT_CLANG_VERSION
  LINK_CLANGPP_BIN=0
  which $SELECT_CLANGPP_BIN || LINK_CLANGPP_BIN=1
  if [[ $LINK_CLANGPP_BIN -eq 1 ]]; then
    mkdir -p .local/bin
    ln -s "$(which "clang$SELECT_CLANG_VERSION")" "$PWD/.local/bin/clang++$SELECT_CLANG_VERSION"
    export PATH="$PWD/.local/bin:$PATH"
  fi
  export USE_CC=clang$SELECT_CLANG_VERSION
elif [[ "x$USE_CC" == "xgcc-latest" ]]; then
  CURRENT_GCC_VERSION=$(gcc -x c /dev/null -dM -E | grep __GNUC__ | awk '{print $NF}')
  echo '#include <iostream>
  int main() { std::cout<<"Hello"; }' >test-gcc-version.cpp
  let LAST_GCC_VERSION=$CURRENT_GCC_VERSION+10
  for ((i = $CURRENT_GCC_VERSION; $i <= $LAST_GCC_VERSION; ++i)); do
    TEST_GCC_VERSION=1
    g++-$i -x c++ test-gcc-version.cpp || TEST_GCC_VERSION=0
    if [[ $TEST_GCC_VERSION -eq 0 ]]; then
      break
    fi
    CURRENT_GCC_VERSION=$i
  done
  export USE_CC=gcc-$CURRENT_GCC_VERSION
  echo "Using $USE_CC"
fi

if [[ "$1" == "format" ]]; then
  python3 -m pip install --user -r ./ci/requirements.txt
  export PATH="$HOME/.local/bin:$PATH"
  bash ./ci/format.sh
  CHANGED="$(git -c core.autocrlf=true ls-files --modified)"
  if [[ ! -z "$CHANGED" ]]; then
    echo "The following files have changes:"
    echo "$CHANGED"
    git diff
    # exit 1 ; # Just warning, some versions of clang-format have different default style for unsupport syntax
  fi
  exit 0
elif [[ "$1" == "coverage" ]]; then
  CONFIGURATION=Debug
  bash cmake_dev.sh -lus -b $CONFIGURATION -r build_jobs_coverage_prepare -c $USE_CC -- \
    "-DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON" "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON"
  bash cmake_dev.sh -lus -b $CONFIGURATION -r build_jobs_coverage -c $USE_CC -- "-DCMAKE_C_FLAGS=$GCOV_FLAGS" "-DCMAKE_CXX_FLAGS=$GCOV_FLAGS" \
    "-DCMAKE_EXE_LINKER_FLAGS=$GCOV_FLAGS" "-DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON" "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON"
  cd build_jobs_coverage
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
  lcov --directory $PWD --capture --output-file coverage.info
elif [[ "$1" == "gcc.test" ]]; then
  bash cmake_dev.sh -lus -b $CONFIGURATION -r build_jobs_ci -c $USE_CC -- -DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON \
    "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON"
  cd build_jobs_ci
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
elif [[ "$1" == "codeql.configure" ]]; then
  bash cmake_dev.sh -l -b $CONFIGURATION -r build_jobs_ci -c $USE_CC -- -DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON \
    "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON"
elif [[ "$1" == "codeql.build" ]]; then
  cd build_jobs_ci
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
elif [[ "$1" == "gcc.legacy.test" ]]; then
  bash cmake_dev.sh -lus -b $CONFIGURATION -r build_jobs_ci -c $USE_CC -- \
    -DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
  cd build_jobs_ci
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
elif [[ "$1" == "clang.test" ]]; then
  bash cmake_dev.sh -lus -b $CONFIGURATION -r build_jobs_ci -c $USE_CC -- -DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON \
    "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON"
  cd build_jobs_ci
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
elif [[ "$1" == "msys2.mingw.test" ]]; then
  pacman -S --needed --noconfirm mingw-w64-x86_64-cmake mingw-w64-x86_64-make \
    mingw-w64-x86_64-curl mingw-w64-x86_64-wget mingw-w64-x86_64-perl \
    mingw-w64-x86_64-nasm \
    mingw-w64-x86_64-git-lfs mingw-w64-x86_64-toolchain mingw-w64-x86_64-libtool \
    mingw-w64-x86_64-python mingw-w64-x86_64-python-pip mingw-w64-x86_64-python-setuptools || true
  export ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_ALLOW_LOCAL=1
  # pacman -S --needed --noconfirm mingw-w64-x86_64-protobuf
  echo "PATH=$PATH"
  # git config --global http.sslBackend openssl || true
  mkdir -p build_jobs_ci
  cd build_jobs_ci
  cmake .. -G 'MinGW Makefiles' "-DBUILD_SHARED_LIBS=$BUILD_SHARED_LIBS" -DPROJECT_ENABLE_UNITTEST=ON -DPROJECT_ENABLE_SAMPLE=ON \
    -DPROJECT_ENABLE_TOOLS=ON -DATBUS_MACRO_ABORT_ON_PROTECTED_ERROR=ON "-DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_LOW_MEMORY_MODE=ON" \
    -DATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_ALLOW_SHARED_LIBS=OFF
  cmake --build . -j2 --config $CONFIGURATION || cmake --build . --config $CONFIGURATION
  for EXT_PATH in $(find "$PWD" -name "*.dll" | xargs dirname | sort -u); do
    export PATH="$EXT_PATH:$PATH"
  done
  for EXT_PATH in $(find "$(readlink -f "$PWD/..")/third_party/install/" -name "*.dll" | xargs dirname | sort -u); do
    export PATH="$EXT_PATH:$PATH"
  done
  echo "PATH=$PATH"
  ctest -VV . -C $CONFIGURATION -L libatapp.unit_test
fi
