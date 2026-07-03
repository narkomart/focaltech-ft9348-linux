#!/bin/bash
# Install FocalTech's proprietary matcher (.so) as /opt/focaltech/matcher.so — with validation.
# We do NOT redistribute FocalTech's binary; you supply it. This just extracts/validates/installs it.
#
# Sources (download the .deb / .rpm / .so yourself, then pass it here):
#   - https://github.com/ftfpteams   (FocalTech's official proprietary libfprint drivers)
#   - https://github.com/leopalladium/focaltech-ft9366-arch-shim   (patched driver + shim for modern distros)
#
# Usage:
#   sudo bash get-matcher.sh <path-to.so | .deb | .rpm | dir>   # extract/validate/install a supplied file
#   sudo bash get-matcher.sh --search                            # scan the system for a compatible .so
set -e
DEST=/opt/focaltech/matcher.so
NEED='ff_algo_do_config focal_EnrollByImage focal_IdentifyByImage focal_SaveAlgTplData focal_InitAlgTplData focal_GetMatchFingerId g_device'
[ "$(id -u)" = 0 ] || { echo "root required: sudo bash $0 …"; exit 1; }
command -v nm >/dev/null || { echo "need 'nm' (binutils): apt install binutils"; exit 1; }

# does $1 look like our matcher? (ELF + all required symbols in its symbol table)
validate(){ local f="$1"; [ -f "$f" ] || return 1; file "$f" 2>/dev/null | grep -q ELF || return 1
  local s; for s in $NEED; do nm "$f" 2>/dev/null | grep -qE " $s\$" || return 1; done; return 0; }

pick_from_dir(){ # echo first validating libfprint .so under $1
  local f; while IFS= read -r f; do validate "$f" && { echo "$f"; return 0; }; done \
    < <(find "$1" -type f \( -name 'libfprint-2.so*' -o -name '*.so*' \) 2>/dev/null); return 1; }

extract(){ # $1 archive -> extraction dir on stdout
  local a="$1" d; d=$(mktemp -d)
  case "$a" in
    *.deb) (cd "$d" && { dpkg-deb -x "$a" . 2>/dev/null || { ar x "$a" 2>/dev/null && tar xf data.tar.* 2>/dev/null; }; });;
    *.rpm) (cd "$d" && { rpm2cpio "$a" 2>/dev/null | cpio -idm 2>/dev/null || bsdtar xf "$a" 2>/dev/null; });;
    *) rm -rf "$d"; return 1;;
  esac
  echo "$d"; }

SRC=""
if [ "$1" = "--search" ]; then
  echo "scanning common locations…"
  for d in /usr/lib /usr/lib64 /usr/local/lib /opt "$HOME/Downloads" "$HOME"; do
    [ -d "$d" ] || continue; SRC=$(pick_from_dir "$d") && { echo "found: $SRC"; break; }
  done
  [ -n "$SRC" ] || { echo "no compatible FocalTech .so found. Download one (see header) and pass it explicitly."; exit 1; }
elif [ -n "$1" ]; then
  arg="$1"
  if [ -d "$arg" ]; then SRC=$(pick_from_dir "$arg")
  elif validate "$arg"; then SRC="$arg"
  else
    case "$arg" in *.deb|*.rpm) d=$(extract "$arg") && SRC=$(pick_from_dir "$d");; esac
  fi
  [ -n "$SRC" ] || { echo "'$arg' is not (and does not contain) a compatible FocalTech matcher .so."; echo "Required symbols: $NEED"; exit 1; }
else
  echo "usage: sudo bash $0 <.so|.deb|.rpm|dir> | --search"; exit 1
fi

echo "validated: $SRC"
mkdir -p "$(dirname "$DEST")"; install -m 0644 "$SRC" "$DEST"
echo "installed -> $DEST"
if command -v "$(dirname "$0")/ftbio" >/dev/null 2>&1 || [ -x "$(dirname "$0")/ftbio" ]; then
  echo "running ftbio check…"; "$(dirname "$0")/ftbio" check || true
elif [ -x /opt/focaltech/ftbio ]; then
  /opt/focaltech/ftbio check || true
else
  echo "build ftbio (bash build.sh) then run: ftbio check"
fi
