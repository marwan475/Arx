#!/bin/sh
set -eu
mode=$1
compiler=$2
runtime=$3

command -v "$compiler" >/dev/null 2>&1 || { echo "Error: x86_64 builds require musl compiler $compiler (set MUSL_CC)." >&2; exit 1; }
test -f "$runtime" || { echo "Error: x86_64 builds require musl runtime $runtime (set MUSL_RUNTIME)." >&2; exit 1; }
readelf -h "$runtime" | grep -F 'Advanced Micro Devices X86-64' >/dev/null || { echo "Error: musl runtime must be x86_64." >&2; exit 1; }
grep -a 'musl libc' "$runtime" >/dev/null || { echo "Error: runtime is not musl: $runtime" >&2; exit 1; }
printf '#include <features.h>\n#ifdef __GLIBC__\n#error musl compiler required\n#endif\n' | "$compiler" -E -x c -o /dev/null -
if test "$mode" = --check; then
    exit 0
fi
test "$mode" = --build || exit 1
output=$4
sources=$5
mkdir -p "$output/lib" "$output/licenses"
cp "$sources/musl-COPYRIGHT" "$output/licenses/musl-COPYRIGHT"
"$compiler" -fPIC -shared -Wl,-soname,libarx_dynamic_test.so -o "$output/lib/libarx_dynamic_test.so" "$sources/dynamic_library.c"
"$compiler" -fPIE -pie -Wl,--dynamic-linker,/lib/ld-musl-x86_64.so.1 -Wl,-rpath,/lib -L"$output/lib" -o "$output/dynamic_fixture.elf" "$sources/dynamic_main.c" -larx_dynamic_test
cp "$runtime" "$output/lib/ld-musl-x86_64.so.1"
ln -sf "ld-musl-x86_64.so.1" "$output/lib/libc.so"
