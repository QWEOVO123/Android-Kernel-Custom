#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Generate the one-boot Root authorization key and the phone import bundle.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 NEW_PRIVATE_DIRECTORY" >&2
    exit 2
fi

umask 077
mkdir -- "$1"
cd -- "$1"
trap 'rm -f -- setup-key.pem setup-cert.pem' EXIT

openssl rand -hex 16 > import-passphrase.txt
openssl genpkey -algorithm RSA \
    -pkeyopt rsa_keygen_bits:2048 -pkeyopt rsa_keygen_pubexp:65537 \
    -aes-256-cbc -pass file:import-passphrase.txt -out setup-key.pem
openssl req -new -x509 -key setup-key.pem -passin file:import-passphrase.txt \
    -subj '/CN=Meow Root Authorization' -days 3650 -out setup-cert.pem
openssl pkey -in setup-key.pem -passin file:import-passphrase.txt \
    -pubout -outform DER -out sukisu-auth-public.der
openssl pkcs12 -export -inkey setup-key.pem -in setup-cert.pem \
    -passin file:import-passphrase.txt -passout fd:3 \
    -name meow-sukisu-authorization -out sukisu-auth-import.p12 3< import-passphrase.txt

printf 'Root authorization fingerprint (SHA-256 of PKCS#1 DER): '
openssl rsa -pubin -inform DER -in sukisu-auth-public.der \
    -RSAPublicKey_out -outform DER 2>/dev/null | sha256sum | cut -d ' ' -f 1