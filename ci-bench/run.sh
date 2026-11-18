#!/bin/bash
# Temporary: kernel microbenchmark, master vs PR #64 head vs this branch's
# variant, 7 interleaved rounds; reports each kernel's minimum (robust to
# noisy neighbours on shared runners).
set -e
mkdir -p old/utils pr/utils
git show origin/master:include/utils/VectorizedDecoders.h > old/utils/VectorizedDecoders.h
git show 86e9f77c:include/utils/VectorizedDecoders.h > pr/utils/VectorizedDecoders.h
FLAGS="-std=c++20 -O3 -DNDEBUG -include array -include utility -I. -Iinclude"
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-old ci-bench/kernels.cpp
g++ $FLAGS -DHDR='"pr/utils/VectorizedDecoders.h"' -o bench-pr ci-bench/kernels.cpp
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-ctl ci-bench/kernels.cpp   # A/A control: master again
g++ $FLAGS -DHDR='<utils/VectorizedDecoders.h>' -o bench-var ci-bench/kernels.cpp     # this branch
uname -m; lscpu | grep -E "Model name" || true
for r in 1 2 3 4 5 6 7; do ./bench-old > old-$r.txt; ./bench-pr > pr-$r.txt; ./bench-ctl > ctl-$r.txt; ./bench-var > var-$r.txt; done
python3 - <<'PY'
d = {}
for v in ("old", "pr", "ctl", "var"):
    for r in range(1, 8):
        for line in open(f"{v}-{r}.txt"):
            k, x = line.split(); d.setdefault((k, v), []).append(float(x))
print("| Kernel | master min ms | A/A control | #64 86e9f77c | revised |")
print("|---|---|---|---|---|")
for k in [l.split()[0] for l in open("old-1.txt")]:
    o, p, c, v = (min(d[(k, x)]) for x in ("old", "pr", "ctl", "var"))
    f = lambda x: "%+.0f%%" % ((x / o - 1) * 100)
    print("| %s | %.2f | %s | %s | %s |" % (k, o, f(c), f(p), f(v)))
PY
