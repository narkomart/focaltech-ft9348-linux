#!/bin/bash
# Add fingerprint to the GNOME lock screen / gdm (screen unlock works reliably; full greeter login may vary).
# Fingerprint = sufficient before the password; password stays as fallback.
# PREREQUISITE: run install-pam-sudo.sh first (installs /opt/focaltech/ftbio and matcher.so, masks fprintd).
# Run: sudo bash setup-gdm.sh
set -e
OPT=/opt/focaltech
WRAP="$OPT/ftbio-gdm"
PAM=/etc/pam.d/gdm-password
MARK="# focaltech-fingerprint"

[ "$(id -u)" = 0 ] || { echo "root required: sudo bash $0"; exit 1; }
[ -x "$OPT/ftbio" ] || { echo "run install-pam-sudo.sh first"; exit 1; }
[ -f "$PAM" ] || { echo "no $PAM (is this a gdm system?)"; exit 1; }

echo "1) wrapper $WRAP (6s timeout so the greeter doesn't hang long)"
printf '#!/bin/sh\nFT_TIMEOUT=6000 exec %s/ftbio verify\n' "$OPT" > "$WRAP"; chmod 0755 "$WRAP"

echo "2) edit $PAM"
LINE="auth    sufficient    pam_exec.so    quiet $WRAP"
cp -a "$PAM" "$PAM.bak.$(date +%s)"
tmp=$(mktemp); tmp2=$(mktemp)
awk -v mark="$MARK" 'prev==1{prev=0;next} $0==mark{prev=1;next} {print}' "$PAM" > "$tmp"
awk -v mark="$MARK" -v line="$LINE" '
  !done && /^@include[ \t]+common-auth/ { print mark; print line; done=1 } { print }
' "$tmp" > "$tmp2"
if grep -qF "$MARK" "$tmp2"; then cat "$tmp2" > "$PAM"; echo "   done (backup: $PAM.bak.*)";
else echo "   !! no '@include common-auth' in $PAM — not modified"; rm -f "$tmp" "$tmp2"; exit 1; fi
rm -f "$tmp" "$tmp2"

echo; echo "Test: lock screen (Super+L) and unlock with the finger (no touch in ~6s -> password field)."
echo "Recovery: TTY (Ctrl+Alt+F3) -> password -> sudo cp $PAM.bak.<ts> $PAM"
