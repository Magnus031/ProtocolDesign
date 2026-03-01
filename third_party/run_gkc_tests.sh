#!/bin/bash
# Script to run GKC tests via Bazel
# Tests are executed inside the build sandbox via postfix_script

set -e

echo "Building GKC and running tests..."
bazel build //third_party:gkc_with_tests --subcommands

echo "Done. Tests were executed as part of the build (see output above)."
