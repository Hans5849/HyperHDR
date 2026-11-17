#!/bin/bash
# Temporary: build the kernel microbenchmark against master's and #64's
# VectorizedDecoders.h and run them alternately, 3 times each.
set -e
mkdir -p old/utils
git show origin/master:include/utils/VectorizedDecoders.h > old/utils/VectorizedDecoders.h
FLAGS="-std=c++20 -O3 -DNDEBUG -include array -include utility -I. -Iinclude"
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-old ci-bench/kernels.cpp
g++ $FLAGS -DHDR='<utils/VectorizedDecoders.h>' -o bench-new ci-bench/kernels.cpp
uname -m; g++ --version | head -1; lscpu | grep -E "Model name|^CPU\(s\)" || true
for r in 1 2 3 4 5; do ./bench-old > old-$r.txt; ./bench-new > new-$r.txt; done
python3 - <<'PY'
import statistics as st
d = {}
for v in ("old", "new"):
    for r in (1, 2, 3, 4, 5):
        for line in open(f"{v}-{r}.txt"):
            k, x = line.split(); d.setdefault((k, v), []).append(float(x))
print("| Kernel | master ms | #64 ms | change |")
print("|---|---|---|---|")
for k in [l.split()[0] for l in open("old-1.txt")]:
    o, n = st.median(d[(k, "old")]), st.median(d[(k, "new")])
    print("| %s | %.2f | %.2f | %+.0f%% |" % (k, o, n, (n / o - 1) * 100))
PY
