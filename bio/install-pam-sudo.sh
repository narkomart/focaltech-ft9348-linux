#!/bin/bash
# Fingerprint login for sudo (standalone ftbio + PAM pam_exec).
# Fingerprint = sufficient: touch logs you in; on timeout you get the normal password (fallback preserved).
# PREREQUISITE: put FocalTech's proprietary libfprint .so at /opt/focaltech/matcher.so (see README).
# Run: sudo bash install-pam-sudo.sh
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
OPT=/opt/focaltech
PAM=/etc/pam.d/sudo
MARK="# focaltech-fingerprint"

[ "$(id -u)" = 0 ] || { echo "root required: sudo bash $0"; exit 1; }
[ -f "$OPT/matcher.so" ] || { echo "missing $OPT/matcher.so — install it first:  sudo bash $SRC/get-matcher.sh <driver.deb|.rpm|.so|--search>"; exit 1; }

echo "1) build ftbio"; bash "$SRC/build.sh"
echo "2) install to $OPT"; mkdir -p "$OPT"; install -m 0755 "$SRC/ftbio" "$OPT/ftbio"
echo "3) disable fprintd (so it doesn't hold the device)"; systemctl disable --now fprintd 2>/dev/null || true; systemctl mask fprintd 2>/dev/null || true

echo "4) edit $PAM"
LINE="auth    sufficient    pam_exec.so    quiet $OPT/ftbio verify"
cp -a "$PAM" "$PAM.bak.$(date +%s)"
tmp=$(mktemp); tmp2=$(mktemp)
awk -v mark="$MARK" 'prev==1{prev=0;next} $0==mark{prev=1;next} {print}' "$PAM" > "$tmp"
awk -v mark="$MARK" -v line="$LINE" '
  !done && /^@include[ \t]+common-auth/ { print mark; print line; done=1 } { print }
' "$tmp" > "$tmp2"
if grep -qF "$MARK" "$tmp2"; then cat "$tmp2" > "$PAM"; echo "   done (backup: $PAM.bak.*)";
else echo "   !! no '@include common-auth' in $PAM — not modified"; rm -f "$tmp" "$tmp2"; exit 1; fi
rm -f "$tmp" "$tmp2"

echo; echo "Enroll a finger:  sudo $OPT/ftbio enroll \$USER"
echo "Test in a NEW terminal:  sudo -k; sudo id   (touch the sensor; ~8s then password fallback)"
echo "Revert:  sudo cp $PAM.bak.<ts> $PAM"
