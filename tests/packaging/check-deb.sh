#!/usr/bin/env bash
# Packaging regression check for a generated HyperHDR .deb.
#
#   tests/packaging/check-deb.sh <HyperHDR-*.deb> [--version-file version] [--arch arm64]
#
# Reads the package with dpkg-deb and checks that the file name, the control
# Version, the source "version" file and the expected architecture agree, and
# that prerelease builds order before the final release.
set -u

DEB=""; VERSION_FILE="$(dirname "$0")/../../version"; ARCH=""
while [ $# -gt 0 ]; do
	case "$1" in
		--version-file) VERSION_FILE=$2; shift 2;;
		--arch) ARCH=$2; shift 2;;
		*) DEB=$1; shift;;
	esac
done
[ -f "$DEB" ] || { echo "usage: $0 <file.deb> [--version-file version] [--arch arch]" >&2; exit 2; }
command -v dpkg-deb >/dev/null && command -v dpkg >/dev/null || { echo "dpkg-deb and dpkg are required" >&2; exit 2; }

FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
pass() { echo "ok:   $*"; }

APP=$(head -n1 "$VERSION_FILE" | tr -d '[:space:]')
BASE=${APP%%-*}
PRE=""; [ "$APP" != "$BASE" ] && PRE=${APP#*-}
NAME=$(basename "$DEB")
PKG=$(dpkg-deb -f "$DEB" Package)
VER=$(dpkg-deb -f "$DEB" Version)
PARCH=$(dpkg-deb -f "$DEB" Architecture)
echo "file=$NAME package=$PKG version=$VER architecture=$PARCH application=$APP"

[ "$PKG" = "hyperhdr" ] && pass "Package: hyperhdr" || fail "Package is '$PKG'"

if dpkg --validate-version "$VER" 2>/dev/null; then pass "Version '$VER' is valid Debian syntax"; else fail "Version '$VER' is not a valid Debian version"; fi

case "$VER" in "$BASE"*) pass "Version starts with $BASE";; *) fail "Version '$VER' does not start with $BASE";; esac

if [ -n "$PRE" ]; then
	squash() { echo "$1" | tr -d '.~+-'; }
	case "$(squash "$VER")" in
		*"$(squash "$PRE")"*) pass "Version keeps prerelease '$PRE'";;
		*) fail "Version '$VER' lost prerelease '$PRE'";;
	esac
	dpkg --compare-versions "$VER" lt "$BASE" && pass "$VER sorts before release $BASE" || fail "$VER does not sort before release $BASE"
else
	dpkg --compare-versions "$VER" ge "$BASE" && pass "release version $VER" || fail "$VER sorts before $BASE"
fi

case "$NAME" in
	*"$APP"*|*"$VER"*) pass "file name carries the version";;
	*) fail "file name '$NAME' carries neither '$APP' nor '$VER'";;
esac

if [ -n "$ARCH" ]; then
	[ "$PARCH" = "$ARCH" ] && pass "Architecture: $ARCH" || fail "Architecture '$PARCH', expected '$ARCH'"
fi

if dpkg-deb -c "$DEB" | grep -Eq '\./usr/bin/hyperhdr$|\./usr/bin/hyperhdr -> '; then pass "contains /usr/bin/hyperhdr"; else fail "missing /usr/bin/hyperhdr"; fi

if [ "$FAILS" -ne 0 ]; then echo "$FAILS packaging check(s) failed"; exit 1; fi
echo "all packaging checks passed"
