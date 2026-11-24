#!/bin/sh
# ---------------------------------------------------------------------------
# Verify the 12 merged build artifacts BEFORE publishing them as a GitHub
# Release. Shared by the `release` (v* tags) and `latest` (main pushes) jobs so
# the two publishing paths cannot drift apart.
#
# Usage: sh .github/scripts/verify-release-assets.sh [dir]
#   dir  directory holding the merged artifacts (default: release)
#
# Expected matrix — keep in sync with the build job in build.yml:
#   PHP 7.4 8.0 8.1 8.2 8.3 8.4  x  glibc musl  = 12 packages
# Each package contributes:
#   mre-<php>-<libc>-x86_64-nts.so
#   mre-<php>-<libc>-x86_64-nts.so.sha256
#   mre-<php>-<libc>-x86_64-nts.tar.gz   (.so + .so.sha256 + BUILD-INFO)
#   BUILD-INFO-<php>-<libc>.txt
# The script also (re)generates the merged SHA256SUMS and runs
# `sha256sum -c SHA256SUMS`, so the published checksum file covers exactly the
# 12 modules published alongside it.
#
# Exit status: 0 only when the whole matrix is present, consistent and
# checksums verify; any problem prints "FAIL: <reason>" to stderr and exits 1.
# ---------------------------------------------------------------------------
set -eu

DIR=${1:-release}
PHP_SERIES="7.4 8.0 8.1 8.2 8.3 8.4"
LIBCS="glibc musl"
ARCH="x86_64"
ZTS="nts"

# Expand the expected package list once; word splitting below is intentional.
EXPECTED=
for php in $PHP_SERIES; do
	for libc in $LIBCS; do
		EXPECTED="$EXPECTED mre-$php-$libc-$ARCH-$ZTS"
	done
done
EXPECTED_COUNT=0
for _ in $EXPECTED; do
	EXPECTED_COUNT=$((EXPECTED_COUNT + 1))
done

die() {
	printf 'verify-release-assets: FAIL: %s\n' "$*" >&2
	exit 1
}

field() { # field <BUILD-INFO file> <key>
	awk -F= -v key="$2" '$1 == key { print $2; exit }' "$1"
}

[ -d "$DIR" ] || die "artifact directory not found: $DIR"
cd "$DIR"
printf -- '--- artifact directory: %s ---\n' "$(pwd)"
ls -l

# ---------------------------------------------------------------------------
# 1. every matrix package is complete and its BUILD-INFO agrees with both its
#    own file name and the artifact it declares (catches merge mix-ups and
#    mislabelled/cross-wired packages)
# ---------------------------------------------------------------------------
for php in $PHP_SERIES; do
	for libc in $LIBCS; do
		base="mre-$php-$libc-$ARCH-$ZTS"
		info="BUILD-INFO-$php-$libc.txt"

		for f in "$base.so" "$base.so.sha256" "$base.tar.gz" "$info"; do
			[ -f "$f" ] || die "missing $f (expected for $base)"
		done

		[ "$(field "$info" artifact)" = "$base" ] \
			|| die "$info declares artifact=$(field "$info" artifact), expected $base"
		[ "$(field "$info" php_series)" = "$php" ] \
			|| die "$info declares php_series=$(field "$info" php_series), expected $php"
		[ "$(field "$info" libc)" = "$libc" ] \
			|| die "$info declares libc=$(field "$info" libc), expected $libc"
		[ "$(field "$info" arch)" = "$ARCH" ] \
			|| die "$info declares arch=$(field "$info" arch), expected $ARCH"
		[ "$(field "$info" php_zts)" = "$ZTS" ] \
			|| die "$info declares php_zts=$(field "$info" php_zts), expected $ZTS"

		# One-line checksum file whose basename resolves from this directory,
		# exactly as `sha256sum -c` expects it.
		[ "$(wc -l <"$base.so.sha256" | tr -d ' ')" -eq 1 ] \
			|| die "$base.so.sha256 must contain exactly one line"
		sum=$(awk 'NR == 1 { print $1 }' "$base.so.sha256")
		file=$(awk 'NR == 1 { print $2 }' "$base.so.sha256")
		[ "$file" = "$base.so" ] \
			|| die "$base.so.sha256 names '$file', expected '$base.so'"
		[ "${#sum}" -eq 64 ] || die "$base.so.sha256 has a malformed digest"
		case $sum in
			*[!0-9a-f]*) die "$base.so.sha256 has a non-hexadecimal digest" ;;
		esac

		# The .tar.gz must carry that package's .so, checksum and BUILD-INFO
		# under their original names.
		want=$(printf '%s\n' "$base.so" "$base.so.sha256" BUILD-INFO | sort)
		got=$(tar -tzf "$base.tar.gz" | sort) \
			|| die "$base.tar.gz is not a readable gzip tar"
		[ "$got" = "$want" ] || die "$base.tar.gz entries differ: got [$(printf '%s ' $got)] expected [$(printf '%s ' $want)]"
	done
done

# ---------------------------------------------------------------------------
# 2. no duplicates or stray packages: counts must equal the matrix exactly
#    (missing entries were already reported above)
# ---------------------------------------------------------------------------
count() { find . -maxdepth 1 -type f -name "$1" | wc -l | tr -d ' '; }
for pair in '*.so:so' '*.so.sha256:so.sha256' '*.tar.gz:tar.gz' 'BUILD-INFO-*.txt:BUILD-INFO'; do
	glob=${pair%%:*}
	label=${pair#*:}
	n=$(count "$glob")
	[ "$n" -eq "$EXPECTED_COUNT" ] \
		|| die "expected $EXPECTED_COUNT $label files, found $n"
done

# ---------------------------------------------------------------------------
# 3. merged SHA256SUMS over the 12 published modules, then verify the actual
#    bytes (the download itself could be truncated/corrupted)
# ---------------------------------------------------------------------------
rm -f SHA256SUMS
for base in $EXPECTED; do
	cat "$base.so.sha256" >>SHA256SUMS
done
[ "$(wc -l <SHA256SUMS | tr -d ' ')" -eq "$EXPECTED_COUNT" ] \
	|| die "SHA256SUMS must have $EXPECTED_COUNT lines"

printf -- '--- SHA256SUMS ---\n'
cat SHA256SUMS
printf -- '--- sha256sum -c SHA256SUMS ---\n'
sha256sum -c SHA256SUMS

printf 'verify-release-assets: OK — %s packages verified, SHA256SUMS covers all of them\n' "$EXPECTED_COUNT"
