#!/bin/sh
# ubenchmon apt-repository setup — registers the GitHub Pages apt repo, then
# installs ubenchmon. After this, `apt upgrade` keeps ubenchmon up to date.
#
#   curl -fsSL https://lordnns.github.io/ubenchmon/setup.sh | sudo sh
#
set -eu

REPO_URL="https://lordnns.github.io/ubenchmon"
KEYRING="/usr/share/keyrings/ubenchmon.gpg"
LIST="/etc/apt/sources.list.d/ubenchmon.list"

# --- must be root (writes to /etc/apt and /usr/share/keyrings) ------------
if [ "$(id -u)" -ne 0 ]; then
    echo "This setup needs root. Re-run with sudo:" >&2
    echo "  curl -fsSL ${REPO_URL}/setup.sh | sudo sh" >&2
    exit 1
fi

# --- pick a downloader ----------------------------------------------------
if command -v curl >/dev/null 2>&1; then
    dl() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
    dl() { wget -qO- "$1"; }
else
    echo "Need curl or wget installed." >&2
    exit 1
fi

command -v gpg >/dev/null 2>&1 || { echo "Need gnupg installed (apt install gnupg)." >&2; exit 1; }

# --- trust the repo signing key ------------------------------------------
echo "Installing signing key -> ${KEYRING}"
dl "${REPO_URL}/ubenchmon-archive-keyring.asc" | gpg --dearmor -o "$KEYRING"
chmod 644 "$KEYRING"

# --- register the repository ---------------------------------------------
echo "Registering apt source -> ${LIST}"
echo "deb [signed-by=${KEYRING}] ${REPO_URL} stable main" > "$LIST"

# --- install --------------------------------------------------------------
echo "Updating package lists and installing ubenchmon..."
apt-get update
apt-get install -y ubenchmon

echo
echo "ubenchmon installed via apt.  Update anytime with:  sudo apt upgrade"
echo "Run:  sudo ubenchmon"
