#!/bin/bash
# Temporary: kernel microbenchmark, master vs PR #64 head vs this branch's
# variant, 7 interleaved rounds; reports each kernel's minimum (robust to
# noisy neighbours on shared runners).
set -e
mkdir -p old/utils pr/utils
git show origin/master:include/utils/VectorizedDecoders.h > old/utils/VectorizedDecoders.h
git show origin/claude/vector-decoder-safety:include/utils/VectorizedDecoders.h > pr/utils/VectorizedDecoders.h
FLAGS="-std=c++20 -O3 -DNDEBUG -include array -include utility -I. -Iinclude"
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-old ci-bench/kernels.cpp
g++ $FLAGS -DHDR='"pr/utils/VectorizedDecoders.h"' -o bench-pr ci-bench/kernels.cpp
g++ $FLAGS -DHDR='<utils/VectorizedDecoders.h>' -o bench-var ci-bench/kernels.cpp
uname -m; lscpu | grep -E "Model name" || true
for r in 1 2 3 4 5 6 7; do ./bench-old > old-$r.txt; ./bench-pr > pr-$r.txt; ./bench-var > var-$r.txt; done
python3 - <<'PY'
d = {}
for v in ("old", "pr", "var"):
    for r in range(1, 8):
        for line in open(f"{v}-{r}.txt"):
            k, x = line.split(); d.setdefault((k, v), []).append(float(x))
print("| Kernel | master min ms | #64 min ms | change | variant min ms | change |")
print("|---|---|---|---|---|---|")
for k in [l.split()[0] for l in open("old-1.txt")]:
    o, p, v = min(d[(k, "old")]), min(d[(k, "pr")]), min(d[(k, "var")])
    print("| %s | %.2f | %.2f | %+.0f%% | %.2f | %+.0f%% |" % (k, o, p, (p / o - 1) * 100, v, (v / o - 1) * 100))
PY
