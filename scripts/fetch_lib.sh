# Shared by fetch_sdl2.sh, fetch_wgpu.sh and fetch_dxc.sh (sourced; the caller sets $here to the repo root).
# Two things on top of a plain curl call (docs/fetch-retry.md):
#  1. A download is tried again several times with a pause, because GitHub's release servers time out now and then
#     (HTTP 504) for some players.
#  2. A file that the player saved by hand into <repo>/downloads/ (under the exact name from the URL) is used instead of
#     downloading, when its SHA-256 is the expected one.

# SHA-256 of a file: sha256sum on Linux, Git Bash and BusyBox, shasum on macOS, Python as the last resort.
fetch_sha256() {
  if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum > /dev/null; then shasum -a 256 "$1" | cut -d' ' -f1
  else python -I -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; fi
}

# fetch_download <label> <folder> <file name to write there> <url> <expected sha256>
# Returns 0 when <folder>/<file name> exists afterwards, 1 (after a message that says what to do by hand) when not.
# curl runs inside <folder> with a plain file name: curl from Git for Windows reads its command line in the ANSI code page
# and cannot open a folder with letters outside it (fetch_wgpu.sh).
fetch_download() {
  fd_label=$1; fd_dir=$2; fd_out=$3; fd_url=$4; fd_sha=$5
  fd_name=${fd_url##*/}
  fd_hand="$here/downloads/$fd_name"
  if [ -f "$fd_hand" ]; then
    if [ "$(fetch_sha256 "$fd_hand")" = "$fd_sha" ] && cp "$fd_hand" "$fd_dir/$fd_out"; then
      echo "$fd_label: using the file you saved by hand: $fd_hand"
      return 0
    fi
    echo "$fd_label: ignoring $fd_hand (it is not the expected file); downloading instead" >&2
  fi
  # --retry-all-errors needs curl 7.71 (2020); an older curl (Windows 10's own) still retries timeouts and HTTP 5xx with --retry.
  fd_flags="--retry 5 --retry-delay 5 --connect-timeout 30 --speed-limit 1000 --speed-time 60"
  if { curl --help all 2> /dev/null || curl --help 2> /dev/null; } | grep -q -e '--retry-all-errors'; then
    fd_flags="$fd_flags --retry-all-errors"
  fi
  # shellcheck disable=SC2086
  if (cd "$fd_dir" && curl -fL $fd_flags -o "$fd_out" "$fd_url"); then return 0; fi
  rm -f "$fd_dir/$fd_out"
  fd_show="$here/downloads"
  if command -v cygpath > /dev/null; then fd_show=$(cygpath -w "$fd_show"); fi
  echo "$fd_label: download failed: $fd_url" >&2
  echo "  To get it by hand: open that link in a browser, save the file as $fd_name, put it into $fd_show, then run this again (in the app: press Try again)." >&2
  return 1
}
