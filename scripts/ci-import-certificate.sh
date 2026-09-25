#!/usr/bin/env bash
#
# CI only: import a Developer ID Application certificate into a throwaway
# keychain and export TETHERKIT_SIGN_IDENTITY (and the notary API key path) to
# the following workflow steps.
#
# Inputs (repository secrets, passed as env):
#   MACOS_CERTIFICATE_P12        base64 of the exported .p12 (certificate + key)
#   MACOS_CERTIFICATE_PASSWORD   its export password
#   NOTARY_KEY_P8                base64 of the App Store Connect API key (.p8), optional
#
# Does nothing (and exits 0) when the certificate secret is absent, e.g. on
# pull requests from forks, where GitHub withholds secrets. Builds then fall
# back to ad-hoc signing.
set -euo pipefail

if [[ -z "${MACOS_CERTIFICATE_P12:-}" ]]; then
  echo "No signing certificate configured; building ad-hoc signed."
  exit 0
fi

work="${RUNNER_TEMP:?}/signing"
mkdir -p "${work}"
keychain="${work}/build.keychain-db"
keychain_password="$(uuidgen)"

printf '%s' "${MACOS_CERTIFICATE_P12}" | base64 --decode > "${work}/cert.p12"
security create-keychain -p "${keychain_password}" "${keychain}"
security set-keychain-settings -lut 21600 "${keychain}"
security unlock-keychain -p "${keychain_password}" "${keychain}"
security import "${work}/cert.p12" -k "${keychain}" -P "${MACOS_CERTIFICATE_PASSWORD:-}" \
  -T /usr/bin/codesign -T /usr/bin/security
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "${keychain_password}" \
  "${keychain}" >/dev/null
# Prepend to the search list so codesign finds the identity.
# shellcheck disable=SC2046
security list-keychains -d user -s "${keychain}" $(security list-keychains -d user | tr -d '"')
rm -f "${work}/cert.p12"

identity="$(security find-identity -v -p codesigning "${keychain}" \
  | awk -F'"' '/Developer ID Application/ {print $2; exit}')"
if [[ -z "${identity}" ]]; then
  echo "::error::No 'Developer ID Application' identity in the provided certificate"
  exit 1
fi
echo "Signing identity: ${identity}"
echo "TETHERKIT_SIGN_IDENTITY=${identity}" >> "${GITHUB_ENV}"

if [[ -n "${NOTARY_KEY_P8:-}" ]]; then
  printf '%s' "${NOTARY_KEY_P8}" | base64 --decode > "${work}/notary.p8"
  chmod 600 "${work}/notary.p8"
  echo "NOTARY_KEY_PATH=${work}/notary.p8" >> "${GITHUB_ENV}"
fi
