#!/bin/bash
#
# SessionStart hook — make this repo build- and test-ready for native_sim in
# Claude Code on the web.
#
# The web container is ephemeral and starts from a bare clone: no west, no
# Zephyr tree, no host-build prerequisites. This script reproduces the setup
# that `native_sim` (host toolchain) needs so `west twister -p native_sim`
# works the moment the session opens. It is web-only and idempotent.
#
# See doc/architecture.md; the manual bring-up this automates is documented in
# doc/impl/l1_bucketlog.md §12.
set -euo pipefail

# Web-only: local dev machines already have a Zephyr environment set up.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
	exit 0
fi

# Keep install noise out of the session context (stdout); logs go to stderr.
exec 1>&2

REPO="${CLAUDE_PROJECT_DIR:-$(pwd)}"
WS="$(dirname "$REPO")"          # west T2 topdir: zephyr/ sits beside the repo
VENV="$WS/.zephyr-venv"

echo "[session-start] preparing Zephyr native_sim environment in $WS"

# 1. System packages for a native_sim host build:
#    dtc     — devicetree compiler
#    gperf   — perfect-hash generation used by the build
#    *multilib — 32-bit libc; native_sim compiles with -m32 by default
APT="apt-get"
command -v sudo >/dev/null 2>&1 && APT="sudo apt-get"
if ! command -v dtc >/dev/null 2>&1 || ! command -v gperf >/dev/null 2>&1 \
		|| [ ! -f /usr/include/bits/libc-header-start.h ]; then
	echo "[session-start] installing system packages"
	DEBIAN_FRONTEND=noninteractive $APT update -qq || true
	DEBIAN_FRONTEND=noninteractive $APT install -y --no-install-recommends \
		device-tree-compiler gperf gcc-multilib g++-multilib
fi

# 2. Python venv with west. Zephyr `main` tooling requires Python >= 3.12.
PY="$(command -v python3.12 || command -v python3)"
if [ ! -x "$VENV/bin/west" ]; then
	echo "[session-start] creating venv with west ($PY)"
	"$PY" -m venv "$VENV"
	# west's docopt dependency (via pykwalify) used to ship sdist-only and
	# fail to build under setuptools >= 66, which this step once pinned
	# around. docopt now publishes a wheel, so nothing is built from source
	# here and the pin is not merely unnecessary but fatal: setuptools < 66
	# calls pkgutil.ImpImporter, removed in Python 3.12, so the pinned
	# install aborts and the session gets no west at all.
	"$VENV/bin/pip" install -q --upgrade pip setuptools wheel
	"$VENV/bin/pip" install -q west
fi
# shellcheck disable=SC1091
. "$VENV/bin/activate"

# 3. Zephyr tree + allowlisted modules (T2 workspace). Shallow to stay fast.
if [ ! -e "$WS/.west/config" ]; then
	echo "[session-start] west init"
	west init -l "$REPO"
fi
if [ ! -d "$WS/zephyr/.git" ]; then
	echo "[session-start] west update (shallow) — first run only, ~650 MB"
	( cd "$WS" && west update --narrow -o=--depth=1 )
fi

# 4. Zephyr Python requirements. base = core build; build-test = the `west
#    twister` extension deps (pytest, junitparser, ...). The full run-test set
#    is skipped on purpose — it pulls opencv/numpy/pyocd that native_sim does
#    not need — but twister's hardware-map module imports these three
#    unconditionally, so add just them.
echo "[session-start] installing Python requirements"
pip install -q -r "$WS/zephyr/scripts/requirements-base.txt"
pip install -q -r "$WS/zephyr/scripts/requirements-build-test.txt"
pip install -q natsort tabulate pyserial

# 5. Persist the environment into the session.
{
	echo "export ZEPHYR_BASE=\"$WS/zephyr\""
	echo "export ZEPHYR_TOOLCHAIN_VARIANT=host"
	echo "export VIRTUAL_ENV=\"$VENV\""
	echo "export PATH=\"$VENV/bin:\$PATH\""
} >> "$CLAUDE_ENV_FILE"

echo "[session-start] ready — build with: west build -b native_sim <app>"
