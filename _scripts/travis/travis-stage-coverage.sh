#!/bin/env bash

# This script runs the coverage stage on Travis.

set -e # make the script exit with error if any executed command exits with error

export LLVM_PROFILE_FILE="/root/coverage_profiles/inet-%h-%p.profraw"

llvm-profdata merge -sparse /root/coverage_profiles/inet-*.profraw -o inet.profdata
llvm-cov report /root/coverage_profiles/libINET.so -instr-profile=inet.profdata
