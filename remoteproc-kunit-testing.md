# Remoteproc KUnit and runtime test notes

## What is available

The working tree contains a KUnit suite at:

```text
drivers/remoteproc/remoteproc_kunit.c
drivers/remoteproc/.kunitconfig
```

The suite is enabled by:

```text
CONFIG_KUNIT=y
CONFIG_REMOTEPROC=y
CONFIG_RPROC_KUNIT_TEST=y
CONFIG_ARCH_NXP=y
CONFIG_ARCH_MXC=y
CONFIG_IMX_DSP_REMOTEPROC=y
CONFIG_IMX_DSP_RPROC_KUNIT_TEST=y
CONFIG_OF_OVERLAY=y
CONFIG_MFD_SYSCON=y
```

Current coverage:

- ELF sanity checks: valid header, truncated section-header table,
  truncated program-header table.
- ELF segment loading: valid copy/zero-fill and ELF64 `p_offset` overflow.
- Resource-table loading: valid table, bad string-table range,
  non-NUL-terminated section names, invalid section-name offsets.
- `rsc_table_for_each_entry()`: valid iteration and malformed
  header/offset-array/entry cases.
- `rproc_da_to_va()`: start/end boundaries, overlong ranges, and a
  >4 GB address alias.
- `rproc_parse_vring()`: power-of-two queue size and alignment validation.
- Synthetic `rproc-virtio` lifecycle: zero-vring and one-vring vdev probe and
  remove paths, verifying teardown only touches initialized vrings.
- i.MX DSP test-only platform device: a DT overlay creates a
  `fsl,imx8ulp-hifi4` node and a test syscon, then verifies probe and removal
  through the real `imx-dsp-rproc` driver.

This suite covers core parsing, translation, vdev teardown, and i.MX DSP
probe/remove. It does not exercise i.MX mailbox, reset, runtime-PM, or
removal races under fault injection.

## Prerequisites

- A clean Linux source tree. `kunit.py` builds with `O=` but rejects a source
  tree that still has `arch/arm64/include/generated` or an in-tree `.config`.
  Use a clean worktree or remove stale in-tree build output only in a
  disposable tree.
- `aarch64-linux-gnu-gcc` and `qemu-system-aarch64`.
- Python 3 and the in-tree KUnit tool.

If the working tree has uncommitted fixes, create a disposable worktree and
apply the patch series there:

```sh
git worktree add /tmp/remoteproc-kunit HEAD
for p in /tmp/opencode/remoteproc-patches/*.patch; do
	git -C /tmp/remoteproc-kunit apply "$p" || exit 1
done
cd /tmp/remoteproc-kunit
```

Do not run the commands below in a tree with an in-tree build if you want to
preserve that build; use the worktree or a separate clean checkout.

## Run the KUnit suite on arm64 QEMU

From the clean source tree:

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-build \
	--kunitconfig=drivers/remoteproc \
	--jobs=2 \
	--timeout=300 \
	'remoteproc-core'
```

Notes:

- The final positional argument is the KUnit **filter glob**. Use it to select
  `remoteproc-core` or a single test such as
  `remoteproc-core.rproc_kunit_elf_sanity_valid`.
- `--filter` is an attribute filter (`speed>slow`, `module=...`), not a suite
  filter. Passing a suite name to `--filter` fails with
  `filter operation not found`.
- `kunit.py` starts QEMU automatically. There is no separate manual QEMU step
  for normal runs.
- The first run builds an arm64 kernel and takes several minutes; subsequent
  runs reuse `/tmp/opencode/kunit-build` and are much faster.
- `--timeout` limits test execution, not the build.

Expected result:

```text
remoteproc-core (12 subtests)
[PASSED] rproc_kunit_elf_sanity_valid
...
Testing complete. Ran 12 tests: passed: 12
```

Run the i.MX DSP test-only platform device with the same build directory:

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-imx \
	--kunitconfig=drivers/remoteproc \
	--jobs=2 \
	--timeout=300 \
	'imx-dsp-rproc'
```

Expected result:

```text
imx-dsp-rproc (1 subtest)
[PASSED] imx_dsp_rproc_kunit_probe
Testing complete. Ran 1 tests: passed: 1
```

For boot/error messages, add:

```sh
--raw_output=all
```

To force a clean rebuild:

```sh
rm -rf /tmp/opencode/kunit-build
```

## Run with KASAN

Use a separate build directory so the normal build configuration is not
replaced:

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-kasan \
	--kunitconfig=drivers/remoteproc \
	--kconfig_add CONFIG_KASAN=y \
	--jobs=2 \
	--timeout=300 \
	'remoteproc-core'
```

Expected result: the same 12 core tests pass, with no KASAN report.
Run the `imx-dsp-rproc` filter with the same `CONFIG_KASAN=y` build directory
to validate the i.MX platform-device probe under KASAN as well.

## Compile the test object without booting

For a fast compile check using an existing arm64 build directory:

```sh
make O=/tmp/opencode/kbuild ARCH=arm64 \
	CROSS_COMPILE=aarch64-linux-gnu- \
	drivers/remoteproc/remoteproc_kunit.o
```

The configuration must have `CONFIG_KUNIT=y`, `CONFIG_REMOTEPROC=y`, and
`CONFIG_RPROC_KUNIT_TEST=y`.

## What the driver/fault-injection suite is

The KUnit parser tests use synthetic firmware and pure data structures. They
cannot prove i.MX-specific behavior such as mailbox allocation failure,
runtime-PM failure, reset failure, start timeout, or removal while an
asynchronous firmware request is pending.

A driver/fault-injection suite is a **test-only kernel harness** that
deterministically forces those paths. It is not a QEMU board model and it is
not random fuzzing. It uses a synthetic remoteproc driver (or a test wrapper
around factored i.MX helpers) with controllable fakes:

- a fake `rproc_ops` implementation whose `start`, `stop`, `kick`,
  `da_to_va`, and firmware callbacks can return chosen errors, time out, or
  delay;
- a fake mailbox controller that can fail `mbox_request_channel_byname()` on
  the first, second, or third channel, and can hold or drop callbacks;
- fake reset and regmap backends that return `-EIO` or delay;
- a fake PM/runtime-PM path that fails resume, or removes the device while a
  firmware callback is still pending;
- test hooks for the asynchronous firmware request so the test can schedule
  removal before the callback completes;
- KASAN and KCSAN to catch use-after-free, double-free, and races.

Two synthetic lifecycle tests are already present: a real `rproc-virtio`
platform device for a vdev with zero and one vrings, and a test-only DT overlay
that creates a `fsl,imx8ulp-hifi4` platform device. They exercise RPROC-010 and
the i.MX DSP probe/remove path without hardware.

Typical tests still to add:

- RPROC-015: fail tx, rx, and rxdb mailbox allocation independently; verify
  all pointers are cleared and cleanup does not free an `ERR_PTR`.
- RPROC-016: make the ready handshake time out and verify `stop` was called.
- RPROC-017: hold the firmware callback, unbind the device, then release the
  callback; verify the devres wait ordering and no use-after-free.
- RPROC-018/019: force `ioremap_wc()` and `pm_runtime_resume_and_get()`
  failures and verify cleanup.
- RPROC-020: leave `boot_work` pending while removing the rproc and verify
  `cancel_work_sync()` ordering.
- RPROC-022: force a >4 GB carveout address, coredump allocation failure, and
  probe PM failure.

The suite can run as KUnit under arm64 QEMU if the i.MX helpers are factored
into testable units or exercised through a test-only platform device. Actual
i.MX DT probe paths, mailbox hardware behavior, and DSP firmware handshakes
still require hardware-in-loop testing or a QEMU model that implements those
peripherals.

## Hardware testing

KUnit proves core parsing and the synthetic vdev path. Hardware is needed for
the i.MX mailbox, reset, runtime-PM, firmware-handshake, and secure-boot
paths.

### Board and firmware matrix

Use a board whose DT node is supported by the driver:

- i.MX8MP EVK: `fsl,imx8mp-cm7` (M7) or `fsl,imx8mp-hifi4` (DSP).
- i.MX8QM/QXP MEK: `fsl,imx8qm-hifi4` / `fsl,imx8qxp-hifi4`.
- i.MX7ULP, i.MX8MM, i.MX8MN, i.MX8MQ: `fsl,*-cm4` / `fsl,*-cm7`.
- i.MX8ULP/93/94/95: the corresponding CM33/CM7/CM70/CM71 nodes.

You need a signed/bootable firmware image for the specific board, plus the
board's normal bootloader and DTB. Keep a known-good image and a separate
test image so a failed test does not brick the boot flow.

### Kernel configuration

Enable at least:

```text
CONFIG_REMOTEPROC=y
CONFIG_IMX_REMOTEPROC=y
CONFIG_IMX_DSP_REMOTEPROC=y
CONFIG_REMOTEPROC_CDEV=y
CONFIG_DEBUG_FS=y
CONFIG_KASAN=y
CONFIG_KASAN_INLINE=y
CONFIG_KCSAN=y
CONFIG_DEBUG_KERNEL=y
CONFIG_DYNAMIC_DEBUG=y
```

`CONFIG_KCSAN` is optional but useful for the async/reload and work
cancellation tests. Keep a serial console and capture the complete boot log.

### Basic smoke test

After boot:

```sh
dmesg | grep -i remoteproc
ls -l /sys/class/remoteproc/
cat /sys/class/remoteproc/remoteproc0/name
cat /sys/class/remoteproc/remoteproc0/firmware
cat /sys/class/remoteproc/remoteproc0/state
```

Start and stop the remote processor:

```sh
echo start > /sys/class/remoteproc/remoteproc0/state
cat /sys/class/remoteproc/remoteproc0/state
echo stop > /sys/class/remoteproc/remoteproc0/state
```

Mount debugfs and inspect the parsed table:

```sh
mount -t debugfs none /sys/kernel/debug
cat /sys/kernel/debug/remoteproc/remoteproc0/resource_table
cat /sys/kernel/debug/remoteproc/remoteproc0/carveout_memories
```

If `CONFIG_REMOTEPROC_CDEV=y`, `/dev/remoteproc0` provides the same
boot/shutdown controls through the character-device interface.

### Finding-specific hardware tests

Use a custom firmware/DTB where noted. Keep KASAN/KCSAN enabled and inspect
`dmesg` after every start/stop/unbind.

- RPROC-010: build a test firmware whose vdev has zero vrings, then one
  vring. Start and stop it repeatedly. The expected result is a clean stop
  with no NULL dereference in `rproc_free_vring()`. A synthetic KUnit test
  already covers this without hardware.
- RPROC-011/012: build a DSP firmware with a BSS/segment size that is not a
  multiple of four and a tail that is not zero-filled correctly. Have the
  firmware check its own BSS, or read the mapped DSP IRAM with `devmem` only
  if the board/DT and security state permit it. Expected: byte-accurate tail
  data, no shifted writes.
- RPROC-013: use the linker-script layout from upstream commit
  `749eed0c7178` so a LOAD segment ends exactly at the end of a mapped
  region. Expected: the segment loads without `bad phdr da ...` and the
  region boundary is accepted.
- RPROC-014: shrink the `rsc-table` reserved-memory region in a test DTB
  below the firmware table size. Expected: parse/start fails cleanly with a
  size error; KASAN must not report an overread or overwrite.
- RPROC-015: boot a test DTB with the `mboxes`/`mbox-names` properties
  removed, or unbind the mailbox controller while the remote processor is
  stopped, then start it. Expected: a clean `-EPROBE_DEFER`/error path and
  no `ERR_PTR`/double-free report. Repeat with only one mailbox channel
  available if the board supports it.
- RPROC-016: use a firmware that does not send the ready handshake, or remove
  the `rxdb` channel from the test DTB. Expected: `start` returns
  `-ETIMEDOUT` and the DSP is actually stopped, not left running.
- RPROC-017: with the DSP running, trigger suspend/resume (`echo mem >
  /sys/power/state`) while unbinding the DSP platform device in parallel.
  Repeat the unbind while a firmware reload is in flight. Expected: no
  use-after-free, the devres wait completes, and the next probe still works.
  This test needs a firmware image that is large enough to keep the async
  request pending for a measurable interval.
- RPROC-018: reset-controller failure is not normally injectable from
  userspace. Use a test kernel patch or a test reset provider to return
  `-EIO`, then start the DSP. Expected: probe/start fails and the DSP is not
  left half-reset.
- RPROC-019: enable runtime PM for the DSP device, force a resume failure
  with a test PM-domain/patch, then start the DSP. Expected: `prepare`
  fails, runtime-PM usage is balanced, and a later start still works.
- RPROC-020: enable `fsl,auto-boot` on a DETACHED test node, or otherwise
  arrange for the auto-boot work to be pending, then unbind the rproc
  platform device immediately. Expected: unbind waits for/cancels the work
  and KASAN reports no use-after-free.
- RPROC-021: use a custom remote firmware that changes `num_of_vrings` or
  `config_len` after the resource table has been validated, then exercise
  the virtio config space through an rpmsg/virtio client. Expected: access
  is rejected, not an out-of-bounds read/write.
- RPROC-022: put a test carveout above 4 GB in a test DTB; confirm it is
  rejected. Force coredump allocation failure with `CONFIG_FAILSLAB` or
  memory pressure and confirm only a warning is emitted. Force probe PM
  failure with a test patch and confirm `pm_runtime_disable()` runs on the
  error path.

### What cannot be proven on stock hardware

Some paths are not reachable without a test kernel patch, a test DT node, a
test firmware, or hardware fault injection:

- reset-controller and PM-domain failures;
- malformed ELF/resource tables after secure firmware validation;
- remote modification of the validated resource table;
- exact asynchronous reload timing.

Use the synthetic driver/fault-injection suite for those, and reserve hardware
for final boot, mailbox, reset, PM, and firmware-handshake validation.
