#!/usr/bin/env bash
set -e

echo "Cleaning old executables..."
rm -f client
rm -f server

echo "Building client..."
g++ client.cpp -o client
echo "Client Build Done."
echo "Building server..."
g++ server.cpp -o server
echo "Server Build Done."
echo "All Object Built Succesfully."
