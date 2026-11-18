#!/bin/bash
# Temporary: P010 decoders, master vs this branch: assembly diff, and the
# kernel microbenchmark (minimum of 7 interleaved rounds, with an A/A control).
set -e
mkdir -p old/utils
git show origin/master:include/utils/VectorizedDecoders.h > old/utils/VectorizedDecoders.h
uname -m; g++ --version | head -1; lscpu | grep -E "Model name" || true
for v in old new; do inc=$([ $v = old ] && echo old || echo include); g++ -std=c++20 -O3 -I$inc -S -o p010-$v.s ci-bench/p010-asm.cpp; grep -vE '^\s*\.(file|ident|section|loc|cfi|size|type|globl|p2align|text)|^\.L(FB|FE|VL|LVL|BB|BE|C)[0-9]|^\s*$' p010-$v.s > p010-$v.t; done
echo "P010 decoders, instructions differing between master and this branch: $(diff p010-old.t p010-new.t | grep -c '^[<>]') (of $(wc -l < p010-new.t) lines)"
diff p010-old.t p010-new.t | head -20 || true
FLAGS="-std=c++20 -O3 -DNDEBUG -include array -include utility -I. -Iinclude"
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-old ci-bench/kernels.cpp
g++ $FLAGS -DHDR='"old/utils/VectorizedDecoders.h"' -o bench-ctl ci-bench/kernels.cpp
g++ $FLAGS -DHDR='<utils/VectorizedDecoders.h>' -o bench-var ci-bench/kernels.cpp
for r in 1 2 3 4 5 6 7; do ./bench-old > old-$r.txt; ./bench-ctl > ctl-$r.txt; ./bench-var > var-$r.txt; done
python3 - <<'PY'
d = {}
for v in ("old", "ctl", "var"):
    for r in range(1, 8):
        for line in open(f"{v}-{r}.txt"):
            k, x = line.split(); d.setdefault((k, v), []).append(float(x))
print("| Kernel | master min ms | A/A control | this branch |")
print("|---|---|---|---|")
for k in [l.split()[0] for l in open("old-1.txt")]:
    o, c, v = (min(d[(k, x)]) for x in ("old", "ctl", "var"))
    f = lambda x: "%+.0f%%" % ((x / o - 1) * 100)
    print("| %s | %.2f | %s | %s |" % (k, o, f(c), f(v)))
PY
