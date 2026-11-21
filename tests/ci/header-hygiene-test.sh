#!/usr/bin/env bash
# Headers that tests include directly must compile on their own, without the
# precompiled header, and survive being included twice (#68).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cat >"$out/twice.cpp" <<'CPP'
#include <utils/FrameDecoderUtils.h>
#include <utils/FrameDecoderUtils.h>
int main()
{
	const auto& y = FrameDecoderUtils::instance().getLutP010_y();
	return y.size() == 1024 ? 0 : 1;
}
CPP
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -I"$root/include" "$out/twice.cpp" -o "$out/twice"
"$out/twice"
echo "PASS FrameDecoderUtils.h: self-contained and include-guarded"
