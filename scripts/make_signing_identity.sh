#!/usr/bin/env bash
# Creates "Nimbo Local Signing", a self-signed code signing identity in the login keychain.
# Builds signed with it keep the same designated requirement (bundle id + this certificate),
# so macOS keeps the Screen Recording / Accessibility permissions across rebuilds and
# reinstalls; ad-hoc signatures change with every build and make it ask again.
# CMake picks the identity up automatically (RAYNEO_CODESIGN_IDENTITY). Run once per Mac.
set -euo pipefail

NAME="Nimbo Local Signing"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

if security find-certificate -c "$NAME" "$KEYCHAIN" >/dev/null 2>&1; then
    echo "$NAME already exists"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/cfg" <<CFG
[req]
distinguished_name = dn
[dn]
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CFG
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=$NAME" \
    -keyout "$tmp/key.pem" -out "$tmp/cert.pem" -config "$tmp/cfg" -extensions ext 2>/dev/null
/usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$NAME" \
    -out "$tmp/id.p12" -passout pass:nimbo
security import "$tmp/id.p12" -k "$KEYCHAIN" -P nimbo -T /usr/bin/codesign >/dev/null
echo "$NAME created in the login keychain. Reconfigure the build: cmake -S . -B build"
