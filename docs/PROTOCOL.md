# FocalTech FT9348 — reverse-engineered protocol & matcher notes

Interoperability documentation (finding facts obtained by reverse engineering for the purpose of
making the device work under Linux). No third-party code is reproduced here.

Device: USB `2808:9348`, internal chip id `0x9362` (FT9366/9368 family), sensor **64×80**, 8-bit gray.
Endpoints: bulk OUT `0x02`, bulk IN `0x83`. Direct USB (not an RTS bridge).

## Register access is via USB control transfers (NOT bulk)
The common mistake (all community drivers and stock libfprint's `ft9366` path) is talking to the chip
with bulk commands `02 00 <len> <cmd> …` — that path **times out** on this chip. The real mechanism:

- **read register**  = control IN  `bmRequestType=0xC0, bRequest=<reg>, wValue, wIndex, wLength=N`
- **write register** = control OUT `bmRequestType=0x40, bRequest=<reg>, wValue, wIndex`

Verified registers:
- `0x43` — **finger present**: read 1 byte, `0x00` = no finger, `0x01` = finger. (Works over EP0 always.)
- internal SFR/SRAM: read via `bReq 0x3a`, write via `bReq 0x3b`, `wIndex` = internal address.

## Frame capture recipe (64×80 = 5120 bytes)
1. Wake:        write `0x34=0xff`; `0x34=2`; `0x6f (wIndex=0xff00, wValue=0)`
2. Sensor mode: write `0x3b (wIndex=0x1f, wValue=1)`; `0x3b (wIndex=0x1e, wValue=1)`; sleep 10 ms
3. Read frame:  write `0x34=0xff`; `0x34=3`; `0x6f (wIndex=0x9080, wValue=5120)`; then **bulk-IN 5120 bytes on EP 0x83**

Key detail: use reg `0x6f` (not `0x35`) and send `0x34=0xff` first — otherwise you get a flat baseline.
Reference implementation: `tools/ftgrab2.c`, `tools/ftctl.c`, `tools/ftenroll.c`.

## Matching
The 64×80 sensor is too small for classic minutiae matching (NBIS/bozorth is unreliable here — a
swipe-assembly + NBIS driver enrolls but verifies at score ~0). FocalTech's own matcher is a
**convolutional neural network (GhostNet)** with anti-spoof / non-finger detectors. It lives in
FocalTech's proprietary libfprint `.so` (see README on how to obtain it — it is NOT distributed here).

### Calling the proprietary matcher
The algorithm in that `.so` is transport-agnostic: you register callbacks and call `focal_*` functions.
We call the non-exported functions by address = `dlopen` base (`dlinfo` `l_addr`) + symbol offset.

`ftbio` resolves these **by name** from the `.so`'s ELF `.symtab` at runtime (see `bio/ftbio.c`), so you
never hardcode addresses — any compatible non-stripped build works. To inspect them manually:

```sh
nm your-focaltech-libfprint.so | grep -E 'ff_algo_do_config|focal_(InitFunc|EnrollByImage|IdentifyByImage|GetMatchFingerId|GetImageTemplate|SaveAlgTplData|InitAlgTplData|VerifyTwoTemplate)'
```

⚠️ If you ever DO hardcode an address, use `nm` values, **not** Ghidra's — Ghidra loads the image at base
`+0x100000`, so its addresses are `0x100000` higher. (That one-hex-digit discrepancy was the single
hardest bug in the whole project — runtime name resolution sidesteps it.)

Functions used (signatures reverse-engineered):
- `ff_algo_do_config(void)` — configures the algorithm (reads a global `g_device`: chip_id + sensor cols/rows).
  Calls `SensorResourceCreate` + `InitFpAlg` internally; also resets state (`SensorResourceDestroy`) at start.
- `focal_InitFunc*` — register callbacks (Log, GetImage, SpiRead/Write, Usleep, GetSystemTime, GetMcuStatus, …).
- `focal_GetImageTemplate(u8 *img, u32 *size /*IN=0*/, u8 *tpl)` — extract a template from a frame. `*size` is IN/OUT.
- `focal_EnrollByImage(u16 fingerId, u16 index, u8 *img)` — add a sub-template (call repeatedly, index 0..N).
- `focal_SaveAlgTplData(u16 fid, u8 *type, u32 *size, u8 *data)` — serialize the enrolled template.
- `focal_InitAlgTplData(u16 fid, u8 type, u32 size, u8 *data)` — load a saved template into the match DB.
- `focal_IdentifyByImage(u8 *img, u8 actMode, u8 *outFid, u8 *outUpdate)` — match a frame; 0 = matched.
- `focal_GetMatchFingerId(void)`, `focal_VerifyTwoTemplate(...)`.

### Enroll / verify flow that works
1. `ff_algo_do_config()` → register callbacks → set fake `g_device` (chip_id=0x9362, cols=64, rows=80).
2. Enroll: capture N frames of the finger, `focal_EnrollByImage(fid, i, frame)` for each; `focal_SaveAlgTplData` → store bytes.
3. Verify (**fresh algorithm context** — a new process, or re-run `ff_algo_do_config` which resets state):
   `focal_InitAlgTplData(stored template)` → capture a frame → `focal_IdentifyByImage`. Accept if it returns 0
   and `focal_GetMatchFingerId()` == the enrolled id.

Measured accuracy on real captures: enrolled finger matched on all frames; a different finger rejected on
all frames.

`RTLD_LAZY` is enough to `dlopen` the `.so` (we only call the `focal_*` algorithm, never its gusb device layer),
so the community libgusb shim is not required.
