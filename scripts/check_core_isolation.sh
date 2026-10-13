#!/usr/bin/env bash
# Fails if the OS-agnostic core includes anything beyond the C++ standard library and its
# own headers, or if hal/ uses an OS header outside a platform guard.
set -euo pipefail
cd "$(dirname "$0")/.."

std_headers='algorithm|array|atomic|cassert|cmath|cstddef|cstdint|cstdio|cstdlib|cstring|functional|limits|locale|memory|numeric|optional|sstream|string|string_view|tuple|type_traits|utility|vector'
status=0

bad=$(grep -rhoE '#include *[<"][^>"]+[>"]' include/rayneo/core src/core \
        | sed -E 's/#include *[<"]([^>"]+)[>"]/\1/' | sort -u \
        | grep -vE "^(rayneo/core/.*|(${std_headers}))$" || true)
if [[ -n "$bad" ]]; then
    echo "core includes non-standard headers:"; echo "$bad"; status=1
fi

# OS headers in hal/ must sit inside #if defined(__APPLE__) / _WIN32 / __linux__ blocks.
for f in include/rayneo/hal/*.h src/hal/*.cpp; do
    awk -v file="$f" '
        /^#if/    { depth++; guard[depth] = ($0 ~ /__APPLE__|_WIN32|__linux__/) }
        /^#elif/  { guard[depth] = ($0 ~ /__APPLE__|_WIN32|__linux__/) }
        /^#endif/ { depth-- }
        /^#include *<(IOKit|CoreFoundation|Foundation|Metal|pthread|windows\.h|hidapi_darwin)/ {
            ok = 0; for (i = 1; i <= depth; i++) if (guard[i]) ok = 1
            if (!ok) { print file ": unguarded OS header: " $0; bad = 1 }
        }
        END { exit bad }' "$f" || status=1
done

[[ $status -eq 0 ]] && echo "core/hal isolation OK"
exit $status
