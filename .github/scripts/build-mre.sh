#!/bin/sh
# +----------------------------------------------------------------------------+
# | build-mre.sh — build the mre (mysql-redis-extend) PHP extension inside an   |
# | official PHP container image and emit loadable release artifacts.           |
# +----------------------------------------------------------------------------+
#
# Intended runtime environments (both are plain POSIX sh, no bash needed):
#   * php:<ver>-cli          Debian (bullseye/trixie)  -> glibc
#   * php:<ver>-cli-alpine   Alpine                    -> musl
#
# The extension itself has no external library dependencies; the only things it
# needs are phpize/php-config, a C toolchain and the target PHP's mysqlnd
# headers (shipped with every official image). Build deps are installed here,
# per distro — or skipped when the image already ships the whole toolchain.
#
# Usage:
#   sh .github/scripts/build-mre.sh [SRC_DIR] [OUT_DIR]
#
#   SRC_DIR  extension source tree, must contain config.m4
#            (default: /src when it looks like the source tree, else $PWD)
#   OUT_DIR  artifact destination (default: /out)
#
# Environment knobs (all optional):
#   MRE_PHP_SERIES          PHP series used in file names, e.g. "7.4".
#                           Default: derived from the running php binary.
#                           When set (CI sets it from the matrix) a mismatch with
#                           the real php binary aborts the build.
#   MRE_LIBC                glibc|musl. When set it must match the detected libc
#                           (CI sets it from the matrix) — a mismatch aborts.
#   MRE_SKIP_DEPS=1         do not install build dependencies (already present).
#   MRE_GIT_SHA             commit sha for BUILD-INFO (CI: $GITHUB_SHA).
#                           Default: git in SRC, then "unknown".
#   MRE_GIT_REF             git ref for BUILD-INFO (CI: $GITHUB_REF).
#   MRE_ALLOW_NO_MYSQLND=1  do not fail when the build lacks mysqlnd headers
#                           (the extension then builds without MySQL capture).
#
# Output (one build per OUT_DIR):
#   mre-<php>-<libc>-<arch>-<zts>.so                 the loadable module
#   mre-<php>-<libc>-<arch>-<zts>.so.sha256          sha256 of the module
#   BUILD-INFO                                       key=value build provenance
#   mre-<php>-<libc>-<arch>-<zts>.tar.gz             all three, for release
#
# Exit status: 0 only when phpize/configure/make, the load smoke test and the
# packaging all succeeded. Any failure exits non-zero (CI job fails).
# -----------------------------------------------------------------------------

set -eu

log() { printf '\n==> %s\n' "$*"; }
die() { printf '\n!! FAILED: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. arguments and source tree
# ---------------------------------------------------------------------------
SRC=${1:-}
OUT=${2:-}

if [ -z "$SRC" ]; then
	if [ -f /src/config.m4 ]; then
		SRC=/src
	else
		SRC=$(pwd)
	fi
fi
OUT=${OUT:-/out}

[ -d "$SRC" ] || die "source directory does not exist: $SRC"
SRC=$(cd "$SRC" && pwd)
[ -f "$SRC/config.m4" ] || die "config.m4 not found in $SRC — not an mre source tree?"
[ -f "$SRC/php_mre.h" ] || die "php_mre.h not found in $SRC"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

log "build-mre: SRC=$SRC OUT=$OUT"

# ---------------------------------------------------------------------------
# 1. platform facts
# ---------------------------------------------------------------------------
if [ -f /etc/alpine-release ]; then
	DISTRO=alpine
else
	DISTRO=debian
fi

detect_libc() {
	if [ "$DISTRO" = alpine ]; then
		printf '%s' musl
		return 0
	fi
	if command -v getconf >/dev/null 2>&1 && getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
		printf '%s' glibc
		return 0
	fi
	if ldd --version 2>&1 | head -n 1 | grep -qi musl; then
		printf '%s' musl
		return 0
	fi
	if ldd --version 2>&1 | head -n 1 | grep -qi 'glibc\|GNU libc'; then
		printf '%s' glibc
		return 0
	fi
	printf '%s' unknown
}

LIBC=$(detect_libc)
if [ -n "${MRE_LIBC:-}" ] && [ "${MRE_LIBC}" != "$LIBC" ]; then
	die "MRE_LIBC=$MRE_LIBC but this container is $LIBC (distro=$DISTRO) — wrong matrix entry or wrong image"
fi

libc_version() {
	case "$LIBC" in
		musl) ldd --version 2>&1 | head -n 2 | tr '\n' ' ' | sed 's/ *$//' ;;
		glibc) getconf GNU_LIBC_VERSION 2>/dev/null | head -n 1 || ldd --version 2>&1 | head -n 1 ;;
		*) printf '%s' unknown ;;
	esac
}

OS_NAME=unknown
if [ -f /etc/os-release ]; then
	OS_NAME=$(
		# shellcheck disable=SC1091
		. /etc/os-release
		printf '%s' "${PRETTY_NAME:-${NAME:-unknown}}"
	)
fi

PHP_BIN=$(command -v php) || die "php binary not found in PATH"
PHP_FULL=$("$PHP_BIN" -r 'echo PHP_VERSION;')
PHP_SERIES=$("$PHP_BIN" -r 'echo PHP_MAJOR_VERSION, ".", PHP_MINOR_VERSION;')
if [ -n "${MRE_PHP_SERIES:-}" ] && [ "${MRE_PHP_SERIES}" != "$PHP_SERIES" ]; then
	die "MRE_PHP_SERIES=$MRE_PHP_SERIES but the php binary is $PHP_FULL (series $PHP_SERIES) — wrong matrix entry or wrong image"
fi

if "$PHP_BIN" -i | grep -q '^Thread Safety => enabled'; then
	PHP_ZTS=zts
else
	PHP_ZTS=nts
fi

PHP_API=$("$PHP_BIN" -i | awk -F'=> ' '/^PHP API/{gsub(/ /, "", $2); print $2; exit}')
PHP_API=${PHP_API:-unknown}

ARCH=$(uname -m)
case "$ARCH" in
	amd64) ARCH=x86_64 ;;
	arm64) ARCH=aarch64 ;;
esac

CC_BIN=${CC:-cc}

log "platform: distro=$DISTRO libc=$LIBC os=\"$OS_NAME\" arch=$ARCH"
log "php: $PHP_FULL (series $PHP_SERIES, $PHP_ZTS, api $PHP_API) at $PHP_BIN"

# ---------------------------------------------------------------------------
# 2. build dependencies (phpize itself needs autoconf; config.m4 needs a C
#    toolchain).
#
#    Two paths, because Debian images behave very differently:
#    * php:7.4-cli (Debian bullseye, EOL) already ships the whole PHPIZE_DEPS
#      toolchain, but its bullseye-security archive is partially gone: an
#      `apt-get install` of already-installed packages would try to *upgrade*
#      them and abort with 404s. So when phpize/make/autoconf/cc are all
#      present we install nothing.
#    * php:8.x-cli (Debian trixie) only ships phpize, so the missing pieces are
#      installed with --no-upgrade (never upgrade what is already there).
# ---------------------------------------------------------------------------
have_build_tools() {
	command -v phpize >/dev/null 2>&1 &&
		command -v make >/dev/null 2>&1 &&
		command -v autoconf >/dev/null 2>&1 &&
		command -v "$CC_BIN" >/dev/null 2>&1
}

if [ "${MRE_SKIP_DEPS:-0}" = "1" ]; then
	log "MRE_SKIP_DEPS=1 — assuming build dependencies are already installed"
elif have_build_tools; then
	log "build tools already present (phpize/make/autoconf/$CC_BIN) — skipping package installation"
else
	if [ -z "${PHPIZE_DEPS:-}" ]; then
		if [ "$DISTRO" = alpine ]; then
			PHPIZE_DEPS="autoconf file g++ gcc libc-dev make pkg-config re2c"
		else
			PHPIZE_DEPS="autoconf dpkg-dev file g++ gcc libc-dev make pkg-config re2c"
		fi
	fi

	case "$DISTRO" in
		alpine)
			log "installing build deps via apk: $PHPIZE_DEPS"
			apk add --no-cache --virtual .mre-build-deps $PHPIZE_DEPS
			;;
		debian)
			log "installing build deps via apt: $PHPIZE_DEPS"
			apt-get update
			DEBIAN_FRONTEND=noninteractive apt-get install -y --no-upgrade --no-install-recommends $PHPIZE_DEPS
			apt-get clean
			rm -rf /var/lib/apt/lists/*
			;;
		*)
			die "unsupported distro: $DISTRO"
			;;
	esac
fi

command -v phpize >/dev/null 2>&1 || die "phpize not found (is this an official php dev image?)"
command -v make >/dev/null 2>&1 || die "make not found after dependency installation"
command -v "$CC_BIN" >/dev/null 2>&1 || die "C compiler '$CC_BIN' not found after dependency installation"
CC_VERSION=$("$CC_BIN" --version 2>/dev/null | head -n 1 || true)
CC_VERSION=${CC_VERSION:-unknown}
log "compiler: $CC_VERSION"

# ---------------------------------------------------------------------------
# 3. clean any generated build state left in the tree
#    (CI checks out a clean tree, but a local copy may carry configured
#    artifacts from a previous in-place build — those must not leak in)
# ---------------------------------------------------------------------------
log "removing previous generated build state in $SRC"
cd "$SRC"
rm -rf autom4te.cache build .libs modules include \
	configure configure.ac config.h config.h.in config.log config.status config.nice \
	config.guess config.sub libtool ltmain.sh aclocal.m4 mkinstalldirs install-sh \
	missing Makefile Makefile.fragments Makefile.objects Makefile.global run-tests.php
rm -f -- ./*.lo ./*.o ./*.dep ./*.la

# ---------------------------------------------------------------------------
# 4. phpize / configure / make
# ---------------------------------------------------------------------------
log "phpize"
phpize

log "./configure --enable-mre"
./configure --enable-mre

JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
log "make -j$JOBS"
make -j"$JOBS"

MOD="$SRC/modules/mre.so"
[ -f "$MOD" ] || die "build finished but $MOD does not exist"

# ---------------------------------------------------------------------------
# 5. smoke test: the module must actually load in this very PHP binary
#    (called for the build-tree module and again for the packaged artifact)
# ---------------------------------------------------------------------------
smoke_load() {
	smoke_mod=$1
	smoke_label=$2
	log "smoke test ($smoke_label): $smoke_mod"
	smoke_out=$(
		"$PHP_BIN" -d "extension=$smoke_mod" -r '
			if (!extension_loaded("mre")) {
				fwrite(STDERR, "LOAD-FAIL: extension_loaded(\"mre\") is false\n");
				exit(1);
			}
			printf("LOAD-OK mre=%s php=%s\n", phpversion("mre") ? phpversion("mre") : "unknown", PHP_VERSION);
		' 2>&1
	) || {
		printf '%s\n' "$smoke_out" >&2
		die "smoke test failed: php -d extension=$smoke_mod could not load the extension"
	}
	printf '%s\n' "$smoke_out"
	case "$smoke_out" in
		*LOAD-OK*) ;;
		*) die "smoke test did not print LOAD-OK: $smoke_out" ;;
	esac
}

smoke_load "$MOD" "build tree module"

MINFO=$("$PHP_BIN" -d "extension=$MOD" -i)
printf '%s\n' "$MINFO" | grep -q 'mre support => enabled' ||
	die "MINFO does not report 'mre support => enabled'"

MYSQLND_STATE=$(printf '%s\n' "$MINFO" | awk -F' => ' '/^mysqlnd capture/{print $2; exit}')
MYSQLND_STATE=${MYSQLND_STATE:-unknown}
log "mysqlnd capture: $MYSQLND_STATE"
case "$MYSQLND_STATE" in
	available) ;;
	*)
		if [ "${MRE_ALLOW_NO_MYSQLND:-0}" = "1" ]; then
			printf '\n!! WARNING: built without mysqlnd headers — MySQL capture is disabled\n' >&2
		else
			die "built without mysqlnd headers (mysqlnd capture: $MYSQLND_STATE); official php images ship them — check the image"
		fi
		;;
esac

# ---------------------------------------------------------------------------
# 6. provenance and packaging
# ---------------------------------------------------------------------------
GIT_SHA=${MRE_GIT_SHA:-${GITHUB_SHA:-}}
if [ -z "$GIT_SHA" ] && command -v git >/dev/null 2>&1; then
	GIT_SHA=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)
fi
GIT_SHA=${GIT_SHA:-unknown}

GIT_REF=${MRE_GIT_REF:-${GITHUB_REF:-unknown}}
BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
MRE_VERSION=$(printf '%s\n' "$MINFO" | awk -F' => ' '/^version/{print $2; exit}')
MRE_VERSION=${MRE_VERSION:-unknown}

BASE="mre-${PHP_SERIES}-${LIBC}-${ARCH}-${PHP_ZTS}"
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/mre-pkg.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM

cp "$MOD" "$STAGE/$BASE.so"
(
	cd "$STAGE"
	sha256sum "$BASE.so" >"$BASE.so.sha256"
)

{
	printf 'name=mre\n'
	printf 'mre_version=%s\n' "$MRE_VERSION"
	printf 'artifact=%s\n' "$BASE"
	printf 'php_version=%s\n' "$PHP_FULL"
	printf 'php_series=%s\n' "$PHP_SERIES"
	printf 'php_api=%s\n' "$PHP_API"
	printf 'php_zts=%s\n' "$PHP_ZTS"
	printf 'php_binary=%s\n' "$PHP_BIN"
	printf 'libc=%s\n' "$LIBC"
	printf 'libc_version=%s\n' "$(libc_version)"
	printf 'arch=%s\n' "$ARCH"
	printf 'os=%s\n' "$OS_NAME"
	printf 'distro_family=%s\n' "$DISTRO"
	printf 'cc=%s\n' "$CC_VERSION"
	printf 'mysqlnd_capture=%s\n' "$MYSQLND_STATE"
	printf 'git_sha=%s\n' "$GIT_SHA"
	printf 'git_ref=%s\n' "$GIT_REF"
	printf 'built_at=%s\n' "$BUILT_AT"
	printf 'built_by=.github/scripts/build-mre.sh\n'
} >"$STAGE/BUILD-INFO"

tar -czf "$STAGE/$BASE.tar.gz" -C "$STAGE" "$BASE.so" "$BASE.so.sha256" BUILD-INFO

cp "$STAGE/$BASE.so" "$STAGE/$BASE.so.sha256" "$STAGE/BUILD-INFO" "$STAGE/$BASE.tar.gz" "$OUT/"

(
	cd "$OUT"
	sha256sum -c "$BASE.so.sha256"
)

# re-load the exact bytes that were published (covers the copy into OUT, not
# just the build tree)
smoke_load "$OUT/$BASE.so" "packaged artifact"

log "artifacts in $OUT"
ls -l "$OUT"

printf '\nBUILD-OK: %s\n' "$BASE"
