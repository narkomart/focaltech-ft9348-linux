# FocalTech FT9348 fingerprint under Linux

Making the **FocalTech FT9348** fingerprint sensor (USB `2808:9348`, internal chip `0x9362`,
64×80 sensor, commonly built into the laptop **power button**) work under Linux — where it has
**no working driver**.

**Status:** capture, enrollment and matching work; fingerprint login is wired via PAM.
Verified: **sudo** login by fingerprint and **GNOME screen unlock** by fingerprint. On real captures
the enrolled finger matched on every frame and a different finger was rejected on every frame.

> This began because stock `libfprint`/`fprintd` only ship a Goodix driver, community kernel drivers
> target other FocalTech revisions (`9338`/`93a9`/`9e65`, not `9348`), and the chip speaks a
> **register-over-control-transfer** protocol that the existing bulk-command code never matched.

## ⚠️ Important: this repo is not self-contained

Matching uses FocalTech's **proprietary** libfprint `.so`, which contains a CNN (GhostNet) matcher.
That binary is **copyrighted and is NOT included here.** You must supply it yourself and place it at:

```
/opt/focaltech/matcher.so
```

Where to get it: FocalTech publish proprietary Linux fingerprint drivers (libfprint forks) for their
sensors; community repositories re-package them. Obtain the libfprint `.so` for the FT9366/FT9368
family and use it as `matcher.so`. This project only contains **our own** code (USB capture, glue to
the matcher, PAM integration) plus reverse-engineering notes.

**The `focal_*` offsets in the code are specific to one `.so` build.** For a different build, re-derive
them with `nm` (see [`docs/PROTOCOL.md`](docs/PROTOCOL.md)) — and use `nm` addresses, not Ghidra's
(Ghidra adds a `+0x100000` image base; that off-by-`0x100000` was the hardest bug in the whole project).

## How it works

Image capture is our own code over libusb using the chip's control-transfer register protocol.
Matching is FocalTech's proprietary CNN, called by loading `matcher.so` (`dlopen`, `RTLD_LAZY`) and
invoking its non-exported `focal_*` functions by `base+offset`. Everything runs in one process, so
there is no clash with the system `libfprint`. Login goes through `pam_exec` — no `fprintd`.

Full protocol and matcher API: [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Layout

```
tools/     libusb tools:  ftctl (read register), ftgrab2 (capture a frame), ftenroll (capture N finger frames)
matcher/   research harnesses:  verify (template compare), enroll_test (enroll/save/load/identify)
bio/       ftbio.c — standalone enroll/verify (capture + matcher);  install scripts for PAM
driver/    focaltech9348.c — a libfprint image driver (capture + swipe-assembly + NBIS). NBIS matching is
           unreliable on this tiny sensor; kept for reference. The reliable path is bio/ftbio + the CNN matcher.
docs/      PROTOCOL.md — reverse-engineered protocol and matcher notes
```

## Build & use

```sh
# 0) place FocalTech's proprietary libfprint .so:
sudo mkdir -p /opt/focaltech && sudo cp /path/to/focaltech-libfprint.so /opt/focaltech/matcher.so

# 1) build the standalone tool
cd bio && bash build.sh          # -> ./ftbio   (needs libusb-1.0 dev headers)

# 2) free the device (no fprintd holding it)
sudo systemctl stop fprintd

# 3) enroll your finger (touch the power-button sensor ~12 times, moving slightly)
sudo ./ftbio enroll "$USER"      # template -> /etc/focaltech/<user>.tpl

# 4) verify (touch once)
sudo ./ftbio verify "$USER"      # prints "совпадение" / exit 0 on match
```

### Fingerprint for sudo
```sh
sudo bash bio/install-pam-sudo.sh      # adds fingerprint as `sufficient`; password stays as fallback
# test in a NEW terminal:
sudo -k; sudo id                        # touch the sensor
```

### Fingerprint for the GNOME lock screen / login
```sh
sudo bash bio/setup-gdm.sh
# lock with Super+L and unlock by touching the sensor (no touch in ~6s -> password field)
```
Screen unlock works reliably. The full gdm greeter login may not run `pam_exec` before the password is
submitted (GNOME-specific); if you need native greeter/settings integration, the proper path is to give
the libfprint driver the CNN matcher instead of NBIS.

`ftbio` env vars: `FT_SO` (path to the matcher `.so`), `FT_TPLDIR` (template dir, default `/etc/focaltech`),
`FT_TIMEOUT` (verify timeout ms), `FTLOG=1` (verbose algorithm log).

## Recovery

Fingerprint is configured as `sufficient`, so the password always remains a fallback — you cannot lock
yourself out. PAM edits are backed up (`/etc/pam.d/<svc>.bak.<ts>`). If a GUI login misbehaves, switch to
a TTY (Ctrl+Alt+F3), log in with your password, and restore the backup.

## Security / privacy

Do not commit biometric data (`*.tpl`, `*.raw`, `cap*/`) or the proprietary `.so` — see `.gitignore`.
Fingerprint templates are personal data; keep `/etc/focaltech` root-only (the tool sets `0600`).

## License

GPLv2-or-later (see [`LICENSE`](LICENSE)). `driver/focaltech9348.c` derives from libfprint and is
GPL/LGPL-compatible accordingly. This project does not include or relicense any FocalTech code.

## Credits

Reverse engineering and integration for the FT9348. The proprietary matcher and the Windows/Linux
FocalTech drivers are © FocalTech; this project interoperates with a user-supplied copy and does not
redistribute it.
