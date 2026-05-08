#!/usr/bin/env bash
# download_components.sh
# Downloads / updates external components used by the emmc-fs crate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPONENTS_DIR="${SCRIPT_DIR}/components"

# ---------------------------------------------------------------------------
# lwext4 — EXT4 filesystem library
# ---------------------------------------------------------------------------
LWEXT4_DIR="${COMPONENTS_DIR}/lwext4_blockdev/lwext4"
LWEXT4_URL="https://github.com/gkostka/lwext4"
LWEXT4_BRANCH="master"

echo "=== lwext4 ==="

if [ -d "${LWEXT4_DIR}/.git" ]; then
    echo "Updating existing repository: ${LWEXT4_DIR}"
    git -C "${LWEXT4_DIR}" fetch origin
    git -C "${LWEXT4_DIR}" checkout "${LWEXT4_BRANCH}"
    git -C "${LWEXT4_DIR}" pull --ff-only origin "${LWEXT4_BRANCH}"
else
    echo "Cloning: ${LWEXT4_URL} → ${LWEXT4_DIR}"
    git clone --branch "${LWEXT4_BRANCH}" "${LWEXT4_URL}" "${LWEXT4_DIR}"
fi

echo ""
echo "Done: all components are up to date."
