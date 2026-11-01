# Sourced by setup.sh and port.sh: sets cxx_flags to the CMake arguments for a
# clang C++ compiler that has <format>. $CXX wins when it works; otherwise the
# newest clang++ (with its own libc++ if the system libstdc++ predates GCC 13).
# Only clang (Clang, AppleClang, llvm-mingw) builds the project: the vector units
# use clang's __builtin_elementwise_min and __builtin_reduce_or (vu_ops.hpp),
# which GCC lacks, so the probe uses them too and g++ is not tried.
# The choice is kept in build/cxx_flags and reused while $CXX and the compiler
# file are unchanged (delete the file to probe again), which saves the 4 s probe.
cxx_flags=()
cxx_stamp=
cxx_cache="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}/build/cxx_flags"
cxx_stat() { stat -c '%s %Y' "$1" 2> /dev/null || stat -f '%z %m' "$1" 2> /dev/null; }
cached_env=; cached_cxx=; cached_stamp=; cached_stdlib=; cached_probe=
[ ! -f "$cxx_cache" ] || { IFS= read -r cached_env; IFS= read -r cached_cxx; IFS= read -r cached_stamp; IFS= read -r cached_stdlib; IFS= read -r cached_probe; } < "$cxx_cache" || true
# the fifth line names the probe; a file the older probe wrote (it also took g++) means probing again
if [ -n "$cached_cxx" ] && [ "$cached_probe" = clang-vector ] && [ "$cached_env" = "${CXX:-}" ] && [ -f "$cached_cxx" ] && [ "$(cxx_stat "$cached_cxx")" = "$cached_stamp" ]; then
  cxx_flags=(-DCMAKE_CXX_COMPILER="$cached_cxx")
  cxx_bin=$cached_cxx
  cxx_stdlib=$cached_stdlib
  cxx_stamp=$cached_stamp
else
  cxx_probe=$(mktemp -d)
  cat > "$cxx_probe/t.cpp" << 'PROBE'
#ifndef __clang__
#error not clang
#endif
#include <format>
typedef int v4i __attribute__((vector_size(16)));
int main() {
  v4i a = {1, 2, 3, 4};
  return __builtin_reduce_or(__builtin_elementwise_min(a, a)) != 7 || std::format("{}", 1).size() != 1;
}
PROBE
  cxx_candidates=()
  [ -n "${CXX:-}" ] && cxx_candidates+=("$CXX")
  cxx_candidates+=($(compgen -c clang++- | grep -E '^clang\+\+-[0-9]+$' | sort -t- -k2 -rn | uniq || true) clang++)
  for cxx in "${cxx_candidates[@]}"; do
    command -v "$cxx" > /dev/null || continue
    for stdlib in "" -stdlib=libc++; do
      if "$cxx" -std=c++20 $stdlib "$cxx_probe/t.cpp" -o "$cxx_probe/t" 2> /dev/null; then
        cxx=$(command -v "$cxx")
        [ -f "$cxx.exe" ] && cxx="$cxx.exe"
        cxx_flags=(-DCMAKE_CXX_COMPILER="$cxx")
        cxx_bin=$cxx
        cxx_stdlib=$stdlib
        break 2
      fi
    done
  done
  rm -rf "$cxx_probe"
  if [ -n "${CXX:-}" ] && [ ${#cxx_flags[@]} -gt 0 ] && [ "$cxx_bin" != "$(command -v "$CXX" 2> /dev/null)" ] && [ "$cxx_bin" != "$(command -v "$CXX" 2> /dev/null).exe" ]; then
    echo "CXX=$CXX is not a working clang (GCC lacks the vector builtins the project uses), so $cxx_bin builds instead" >&2
  fi
  if [ ${#cxx_flags[@]} -eq 0 ]; then
    [ -z "${CXX:-}" ] || echo "CXX=$CXX does not work here: it must be clang (GCC lacks the vector builtins the project uses)" >&2
    echo "no clang C++ compiler with <format> found: install clang 17+ with libc++ (e.g. clang-19 libc++-19-dev; macOS: Xcode's clang), or point CXX at one" >&2
    exit 1
  fi
  cxx_stamp=$(cxx_stat "$cxx")
  if [ -n "$cxx_stamp" ] && mkdir -p "$(dirname "$cxx_cache")" 2> /dev/null; then
    printf '%s\n%s\n%s\n%s\nclang-vector\n' "${CXX:-}" "$cxx" "$cxx_stamp" "$stdlib" > "$cxx_cache.$$" 2> /dev/null && mv -f "$cxx_cache.$$" "$cxx_cache" 2> /dev/null || rm -f "$cxx_cache.$$"
  fi
fi
# Compile flags: the user's $CXXFLAGS, the standard library choice and, for the game and the runtime (not the tools,
# which set cxx_tools=1 first), the vector-unit flags below. CMake ignores $CXXFLAGS once -DCMAKE_CXX_FLAGS is given,
# so everything goes through -DCMAKE_CXX_FLAGS; a changed $CXXFLAGS or switch then reconfigures and rebuilds.
#
# AVX2 (docs/speed.md, "AVX2 is a build flag, not code"): when the PC that builds has AVX2, the build uses -mavx2, which
# makes the vector-unit thread about 5 % cheaper. -ffp-contract=off goes with it and is essential: without it the
# compiler may fuse multiply and add (FMA), which rounds once instead of twice and changes the vector units' results.
# The result runs only on processors with AVX2, which is why it is chosen by asking the compiler on the building PC
# (the game is built on the PC that plays it). PS2_AVX2=0 switches it off, PS2_AVX2=1 forces it. A user's own
# -march=... or -mfma in $CXXFLAGS is kept, with -ffp-contract=off added behind it for the same reason.
cxx_extra=
if [ "${cxx_tools:-0}" != 1 ]; then
  case "${PS2_AVX2:-auto}" in
    0) cxx_avx2_msg="avx2: off (PS2_AVX2=0)" ;;
    1) cxx_extra="-mavx2 -ffp-contract=off"; cxx_avx2_msg="avx2: on (PS2_AVX2=1): -mavx2 -ffp-contract=off" ;;
    *)
      # the output goes into a variable and no pipe reads it: "grep -q" ends a pipe early, and under pipefail (port.sh) the
      # writer's SIGPIPE then made the probe fail at random when several port.sh ran at once
      cxx_macros=$("$cxx_bin" -march=native -dM -E -x c++ - < /dev/null 2> /dev/null || true)
      if [[ $'\n'"$cxx_macros" == *$'\n#define __AVX2__ 1'* ]]; then
        cxx_extra="-mavx2 -ffp-contract=off"; cxx_avx2_msg="avx2: on (this PC has it): -mavx2 -ffp-contract=off, PS2_AVX2=0 turns it off"
      else
        cxx_avx2_msg="avx2: off (this PC or compiler has none)"
      fi ;;
  esac
  case " ${CXXFLAGS:-} " in
    *" -ffp-contract"*) ;;
    *" -march="*|*" -mfma"*|*" -mcpu="*) [ -n "$cxx_extra" ] || cxx_extra="-ffp-contract=off" ;;
  esac
  if [ "${cxx_avx2_said:-}" != 1 ]; then echo "$cxx_avx2_msg"; cxx_avx2_said=1; fi
fi
cxx_all="${CXXFLAGS:-} $cxx_stdlib $cxx_extra"
cxx_all=$(echo $cxx_all)
# always passed, even empty: a flag taken out must also leave the CMake cache of an existing build folder
cxx_flags+=(-DCMAKE_CXX_FLAGS="$cxx_all")
# ccache, when installed, keeps compiled objects across build folders and git worktrees. base_dir (the main
# checkout, which holds the worktrees in wt/) turns the absolute paths below it into relative ones, so a worktree
# hits what the main checkout already compiled. PS2_CCACHE=0 builds without it.
if [ "${PS2_CCACHE:-1}" != 0 ] && command -v ccache > /dev/null; then
  ccache_base=$(git -C "${ROOT:-.}" rev-parse --path-format=absolute --git-common-dir 2> /dev/null || true)
  ccache_base=${ccache_base%/.git}
  [ -n "$ccache_base" ] || ccache_base=${ROOT:-$(pwd)}
  ccache_exe=$(command -v ccache)
  if command -v cygpath > /dev/null; then  # Git Bash: ninja and ccache need Windows paths
    [ ! -f "$ccache_exe.exe" ] || ccache_exe="$ccache_exe.exe"
    ccache_exe=$(cygpath -m "$ccache_exe")
    ccache_base=$(cygpath -m "$ccache_base")
  fi
  cxx_flags+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$ccache_exe;base_dir=$ccache_base")
fi
