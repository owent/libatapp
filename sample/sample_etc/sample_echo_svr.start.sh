#!/bin/sh
# Copyright 2026 atframework

ETC_DIR="$(dirname $0)";

./sample_echo_svr -id 1 -c "$ETC_DIR/sample_echo_svr.conf" -p sample_echo_svr.pid start -echo "hello world!"

# ./sample_echo_svr -id 1 -c "$ETC_DIR/sample_echo_svr.conf" run echo "say hello."