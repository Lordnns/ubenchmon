#!/bin/sh
# ubenchmon installer — fetches the latest .deb from GitHub Releases,
# verifies its checksum, and installs it (dependencies and all).
#
#   curl -fsSL https://raw.githubusercontent.com/Lordnns/ubenchmon/main/install.sh | sudo sh
#
set -eu

REPO="Lordnns/ubenchmon"
DEB="ubenchmon.deb"
BASE="https://github.com/${REPO}/releases/latest/download"

# --- must be root (installs system-wide) ---------------------------------
if [ "$(id -u)" -ne 0 ]; then
    echo "This installer needs root. Re-run with sudo:" >&2
    echo "  curl -fsSL https://raw.githubusercontent.com/${REPO}/main/install.sh | sudo sh" >&2
    exit 1
fi

# --- amd64 only (that is what the .deb is built for) ----------------------
ARCH="$(uname -m)"
if [ "$ARCH" != "x86_64" ]; then
    echo "ubenchmon packages are built for x86_64 (amd64); detected ${ARCH}." >&2
    echo "Build from source instead — see the README." >&2
    exit 1
fi

# --- pick a downloader ----------------------------------------------------
if command -v curl >/dev/null 2>&1; then
    dl() { curl -fsSL -o "$1" "$2"; }
elif command -v wget >/dev/null 2>&1; then
    dl() { wget -qO "$1" "$2"; }
else
    echo "Need curl or wget installed." >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

echo "Downloading latest ${DEB}..."
dl "$DEB" "${BASE}/${DEB}"

# --- verify checksum if the release publishes one -------------------------
if dl "${DEB}.sha256" "${BASE}/${DEB}.sha256" 2>/dev/null; then
    echo "Verifying checksum..."
    sha256sum -c "${DEB}.sha256" || { echo "Checksum FAILED — aborting." >&2; exit 1; }
fi

# --- install (apt resolves the .deb's dependencies; dpkg -i would not) ----
echo "Installing..."
apt-get update -qq || true
apt-get install -y "./${DEB}"

echo
echo "ubenchmon installed.  Run:  sudo ubenchmon"
