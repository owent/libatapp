#!/bin/bash

cd "$(dirname "0")/.."

EXCEPT_TOOLSET_VERSION=$(git submodule status atframework/cmake-toolset | awk '{print $1}')

if [[ -e "third_party/install" ]]; then
  if [[ -e "third_party/install/.cmake-toolset-version" ]]; then
    REAL_TOOLSET_VERSION=$(cat "third_party/install/.cmake-toolset-version")
    if [[ "$EXCEPT_TOOLSET_VERSION" != "$REAL_TOOLSET_VERSION" ]]; then
      rm -rf "third_party/install"
    fi
  else
    rm -rf "third_party/install"
  fi
fi

mkdir -p "third_party/install"
echo "$EXCEPT_TOOLSET_VERSION" >"third_party/install/.cmake-toolset-version"
echo "third_party/install/.cmake-toolset-version = $EXCEPT_TOOLSET_VERSION"
