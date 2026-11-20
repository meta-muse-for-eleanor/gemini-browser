#!/bin/sh
# Build the Gemini client. Run from this directory or pass no arguments.
# Requires gcc and OpenSSL development libraries (libssl-dev on Debian).
set -e
cd "$(dirname "$0")"
gcc -Wall -O2 -o gemini gemini.c -lssl -lcrypto
echo "Built ./gemini"
./gemini -h
