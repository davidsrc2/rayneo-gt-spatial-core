#!/usr/bin/env bash
# Once, on your Mac: creates "Nimbo Signing", the self-signed code signing identity of the
# released macOS builds, puts it in your login keychain (local builds sign with it too) and
# stores it in the repository's GitHub secrets for the CI (MAC_SIGNING_P12 and
# MAC_SIGNING_PASSWORD). Every build signed with it has the same designated requirement, so
# macOS keeps Nimbo's Screen Recording / Accessibility permissions across updates.
# Needs gh logged in (gh auth login). Without an Apple Developer ID, Gatekeeper still asks
# once per Mac on the first install (System Settings > Privacy & Security > Open Anyway).
set -euo pipefail
cd "$(dirname "$0")/.."

NAME="Nimbo Signing"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"
REPO="davidsrc2/rayneo-gt-spatial-core"

if security find-certificate -c "$NAME" "$KEYCHAIN" >/dev/null 2>&1; then
    echo "\"$NAME\" is already in your keychain: nothing created (delete it in Keychain Access to start over)"
    exit 1
fi
command -v gh >/dev/null || { echo "gh not found: brew install gh && gh auth login"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pass=$(/usr/bin/openssl rand -hex 24)
cat > "$tmp/cfg" <<CFG
[req]
distinguished_name = dn
[dn]
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CFG
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -subj "/CN=$NAME" \
    -keyout "$tmp/key.pem" -out "$tmp/cert.pem" -config "$tmp/cfg" -extensions ext 2>/dev/null
# -legacy: the PKCS#12 encryption the macOS keychain imports (OpenSSL 3's default it rejects).
/usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$NAME" \
    -out "$tmp/id.p12" -passout "pass:$pass" -legacy 2>/dev/null ||
/usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$NAME" \
    -out "$tmp/id.p12" -passout "pass:$pass"
security import "$tmp/id.p12" -k "$KEYCHAIN" -P "$pass" -T /usr/bin/codesign >/dev/null
base64 -i "$tmp/id.p12" | gh secret set MAC_SIGNING_P12 --repo "$REPO"
printf '%s' "$pass" | gh secret set MAC_SIGNING_PASSWORD --repo "$REPO"
echo "\"$NAME\" created, in your keychain and in the GitHub secrets of $REPO."
echo "Reconfigure your build so it signs with it: rm -rf build && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
