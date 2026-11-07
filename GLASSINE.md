# GlassineOS ABL

This is the Android bootloader (ABL) that GlassineOS runs on the Onyx Boox Note Air
(Snapdragon 636). It is Qualcomm's open-source `LinuxLoader` with GlassineOS's changes on
top. The [Note Air page](https://docs.glassine.org/devices/onyx-noteair/#boot-chain)
describes the boot chain and what each change is for. Upstream's files describe the
EDK II tree itself.

**Status:** this is what the Note Air runs. The tag below rebuilds the flashed image
byte for byte.

Written largely by AI agents and not reviewed by anyone who knows this bootloader well.
None of it has been sent upstream (`Glassine-Upstream: not-submitted (unreviewed)` in
each commit).

## Branches and tags

| Ref | What |
|---|---|
| `glassine/LA.UM.7.2.c25-12600-sdm660.0` | the default branch: the upstream tree, then GlassineOS's commits |
| `LA.UM.7.2.c25-12600-sdm660.0-glassine1` | the build flashed on the Note Air (see [Reproducing the flashed build](#reproducing-the-flashed-build)) |

The first commit imports the upstream tree, without its history:

| | |
|---|---|
| Upstream | `https://git.codelinaro.org/clo/la/abl/tianocore/edk2.git` |
| Tag | `LA.UM.7.2.c25-12600-sdm660.0` |
| Commit | `0503c04ea480ded66a23274e7c115ee96c4249ff` |
| Tree | `a48099efdfe2cb0f02006f273db1f2caf7989f96` (the import commit has the same tree) |

## Changes

One commit per change, in this order:

| Commit | Change |
|---|---|
| BaseTools: build with current GCC and Clang | two warnings off that newer compilers turn into errors |
| QcomModulePkg: no graphical fastboot menu | the menu hangs on the e-ink panel and blocks fastboot's USB |
| FastbootLib: add oem reboot-edl and oem reboot-recovery | reach EDL and recovery from fastboot |
| FastbootLib: add oem test-sensor | reads the hall sensor under a watchdog; a diagnostic |
| LinuxLoader: closed cover at power-on enters fastboot | a way into the bootloader without a working OS |
| LinuxLoader: send the Root of Trust as the stock bootloader does | Android's disk encryption key unseals; recovery can read the data |
| LinuxLoader: give Android the stock dm-verity command line | what Android's init expects |
| BootLib, avb: boot the recovery partition on an A/B table | stock A/B bootloaders only boot `boot_<slot>` |
| LinuxLoader: point the verity device at the active slot's system | stock hard-wires slot A's system partition |

Each commit message says how the change can fail and the way back. Only the tag is
build-tested: the commits before it were split by hand and haven't been built one by
one.

Identifiers and log messages in the changes keep the `NA1` prefix (`NA1_HALL_GPIO_INOUT`,
`"NA1: VBSendRot success"`). The log strings are compiled into the image, so renaming
them would change the flashed build.

`.safety-scan` lists, one by one, the upstream files that the organization's safety scan
flags (prebuilt UEFI Shell binaries, Python's test certificates, EDK II's test signing
key, board data of other platforms). They are upstream's, public in the upstream tree,
and none goes into the ABL.

The changes are under the license of the file they're in: BSD-3-Clause for
`QcomModulePkg`, the BSD license of the EDK II package elsewhere. `glassine/` is
BSD-3-Clause.

## Building

Needs Docker and git.

```sh
glassine/build                 # output in ~/.cache/glassine/abl
glassine/build --out DIR
```

The script exports `HEAD` into the output folder, builds it in the image from
`glassine/Dockerfile` (Ubuntu 20.04, Clang 10), and signs it with
[qtestsign](https://github.com/msm8916-mainline/qtestsign) at a fixed commit. It writes
`abl-unsigned.elf` and `abl.elf` and prints both checksums. The first build takes about
five minutes.

**Signing format v5 is required.** The Note Air's XBL checks the ABL's signature
format but not its key, so qtestsign's dummy certificate chain is accepted. An image
signed in format v3 (qtestsign's default) is rejected without a message, and the device
drops into EDL. The script always passes `-v 5`. No signing key is involved, and none
is in this repository.

## Reproducing the flashed build

Upstream prints the build date and time (`__DATE__`, `__TIME__`) in "Loader Build Info"
and "Fastboot Build Info", so two builds of the same commit differ in those bytes.
`--stamps` sets them. With the stamps of the flashed build, the tag builds the flashed
image byte for byte:

```sh
git checkout LA.UM.7.2.c25-12600-sdm660.0-glassine1
glassine/build --stamps "Jun  3 2026 15:38:04" "Mar 30 2026 16:48:45"
```

| File | sha256 |
|---|---|
| `abl-unsigned.elf` | `dcb234f4dc4e0b217c3c9d312cb2f167460ea1c3a78ca7c9b122921ab95a369a` |
| `abl.elf` | `348be699ae2b5c58a4953e003cc8b2e0b7cc3601375aadadfb9676b4c3092183` |

The two stamps differ because the flashed build compiled FastbootLib in an earlier,
incremental build.

## Flashing

This README doesn't cover flashing. Write the
[gate](https://docs.glassine.org/guides/irreversible-operations/) first. The short
version: an ABL that doesn't boot leaves the device without fastboot and without
Android, so the only way back is the hardware EDL route with the last known-good build
on disk ([Undoing each
step](https://docs.glassine.org/devices/onyx-noteair/#undoing-each-step)). Before
flashing, have that route tried on the device, the battery charged, and the image you
restore checked against its sha256.

## Image size

`glassine/build` prints GenFv's size lines for each firmware volume. For the flashed
build:

| Volume | Size | Used | Free | Size set by |
|---|---|---|---|---|
| `FVMAIN` (uncompressed, holds `LinuxLoader.efi`) | 311,488 | 311,472 | 16 | its contents: `NumBlocks = 0` in `QcomModulePkg/QcomModulePkg.fdf` |
| `FVMAIN_COMPACT` (compressed `FVMAIN`, the image in `abl.elf`) | 139,264 | 89,800 | 49,464 | `BlockSize` × `NumBlocks` of the flash device in the same file |

`FVMAIN` is sized to fit what it holds, so its 16 free bytes are alignment padding, not
a limit. A build with 8 KiB of data added to `LinuxLoader` gave an `FVMAIN` of 319,680
bytes, again with 16 free, and built and signed without an error. The limit that applies is `FVMAIN_COMPACT`'s: about
48 KiB of compressed room in a flash device region of 0x22000 bytes, set in the FDF.
Don't take the region's size as free to grow: the image is loaded at `0x9FA00000`, and
what follows the region in memory isn't known. A RELEASE build, without the DEBUG
strings, would be smaller (untested on the device). A change that
grows the image should still be flashed only under the
[irreversible-operation gate](https://docs.glassine.org/guides/irreversible-operations/).
