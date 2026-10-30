# Remoteproc audit log

- Date: 2026-10-28
- Tree: `/home/freenix/sda/linux-next`
- Branch: `b4/remoteproc-bug`
- HEAD: `f8aa0512a932 EDITME: cover title for remoteproc-bug` (`v7.3-17319-gf8aa0512a932`)
- Auditor: opencode (single agent)
- Method: source audit guided by `remoteproc-bug-scan-guide.md`; findings recorded as found

## Scope and plan

Scope was narrowed by the user on 2026-10-28:

- **In scope:** core framework and i.MX drivers only.
  - Core: `remoteproc_core.c`, `remoteproc_elf_loader.c`,
    `remoteproc_cdev.c`, `remoteproc_sysfs.c`, `remoteproc_debugfs.c`,
    `remoteproc_coredump.c`, `remoteproc_virtio.c`,
    `include/linux/remoteproc.h`, `include/linux/rsc_table.h`,
    `uapi/linux/remoteproc_cdev.h`
  - i.MX: `imx_rproc.c`, `imx_dsp_rproc.c`, `imx_rproc.h`
- **Out of scope:** qcom, stm32, ti, xlnx, mtk, pru, omap, and all other
  vendor drivers. Earlier cross-driver observations are context only and are
  not counted as findings.

Priority order:

1. ELF and resource-table parsing (core)
2. Lifecycle/state-machine/concurrency (core)
3. vring/virtio/rpmsg (core)
4. Coredump/debugfs (core)
5. i.MX driver audit (`imx_rproc.c`, `imx_dsp_rproc.c`)

For each candidate record:

- ID, file:line, description, trigger/reachability, impact, confidence, status
- Whether it is a real bug, a hardening gap, or only a suspicion needing runtime proof

## Progress log

### 2026-10-28 - Setup

- Confirmed subsystem layout: 30 C files, ~30k lines.
- Confirmed no `.config`, no `compile_commands.json`.
- Checked for static-analysis tools: `smatch`, `sparse`, `cppcheck`, `clang`, `coccinelle`/`spatch` are not installed.
- Will do source-level audit first; static-analysis commands recorded for later once tools are available.
- Started with Pass 1: ELF/resource-table parsing.

### 2026-10-28 - Scope narrowed; Pass 1 in progress

- User restricted the audit to core + i.MX.
- Read `remoteproc_elf_loader.c`, `remoteproc_elf_helpers.h`,
  `include/linux/rsc_table.h`, `include/linux/remoteproc.h`, and the relevant
  core call paths: `rproc_fw_boot()`, `rproc_attach()`, `rproc_start()`,
  `rproc_handle_resources()`, `rproc_resource_cleanup()`, `rproc_del()`.
- Findings below are from source review; no runtime reproducer has been run
  yet. "Confidence" means confidence that the code path exists, not that the
  bug has been observed on hardware.
- Started the i.MX pass by listing probe/start/stop/da_to_va/parse_fw and
  reviewing recent i.MX commits.

### 2026-10-28 - i.MX and core lifecycle pass

- Read `imx_rproc.c` in full and `imx_dsp_rproc.c` through probe/PM paths.
- Reviewed core lifecycle, vdev/virtio cleanup, coredump, cdev, sysfs, and
  debugfs paths.
- Found a core vdev cleanup NULL dereference for vdevs with fewer than two
  vrings, plus several i.MX memory/error-path bugs.
- `749eed0c7178` ("Allow mappings ending at region boundary") is present in the
  repository but is not an ancestor of HEAD; the i.MX DA-translation overflow
  and boundary checks below therefore remain in this tree.
- No audit-log entries had been written for this pass yet; findings RPROC-010
  through RPROC-022 are recorded now.

## Findings

### RPROC-001: ELF sanity check does not bound the section-header table

- **File:** `drivers/remoteproc/remoteproc_elf_loader.c:95`
- **Code:** `rproc_elf_sanity_check()` checks only
  `fw->size < shoff + elf_shdr_get_size`.
- **Problem:** it never checks that the whole section-header array
  (`e_shoff + e_shnum * e_shentsize`) fits in the firmware. `e_shnum` is a
  firmware-controlled `u16`; a file with a valid first header but a large
  `e_shnum` passes the check. The addition `shoff + elf_shdr_get_size` can
  also wrap for a very large `e_shoff`.
- **Reachability:** any driver that uses `rproc_elf_sanity_check()` and then
  parses a resource table, including i.MX.
- **Impact:** out-of-bounds read when `find_table()` walks the section headers;
  kernel crash/DoS on malformed firmware.
- **Confidence:** high (code path is direct).
- **Status:** static finding, needs runtime confirmation with KASAN.

### RPROC-002: `find_table()` trusts `e_shnum`, `e_shstrndx`, and the string table

- **File:** `drivers/remoteproc/remoteproc_elf_loader.c:247`
- **Code:** `find_table()` computes
  `name_table_shdr = shdr + shstrndx * shdr_size`, then
  `name_table = elf_data + sh_offset`, and calls
  `strcmp(name_table + name, ".resource_table")` for every section.
- **Problem:** none of these are validated against `fw->size`:
  - `e_shstrndx < e_shnum` is not checked.
  - the string-table section header being inside the section-header array is
    not checked.
  - the string table range (`sh_offset`, `sh_size`) being inside the firmware
    is not checked.
  - the section name offset (`sh_name`) being inside the string table is not
    checked.
  - the string is not required to be NUL-terminated; `strcmp()` can read past
    the firmware buffer.
- **Impact:** out-of-bounds reads on malformed firmware; crash/DoS and
  potential kernel-memory disclosure through subsequent parsing.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-003: `rproc_elf_load_segments()` does not bound the program-header array

- **File:** `drivers/remoteproc/remoteproc_elf_loader.c:160`
- **Code:** `phdr = elf_data + e_phoff;` then the loop reads `phnum` program
  headers without checking `e_phoff + phnum * sizeof(*phdr) <= fw->size`.
  The sanity check only rejects `phoff > fw->size`, not equality or a short
  header table.
- **Impact:** out-of-bounds read of program-header fields; values read from
  beyond the buffer can drive later segment loading logic.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-004: segment `offset + filesz` overflow bypasses the truncation check

- **File:** `drivers/remoteproc/remoteproc_elf_loader.c:197`
- **Code:** `if (offset + filesz > fw->size) { ... }` where both are `u64`.
- **Problem:** a crafted `p_offset` near `U64_MAX` makes the addition wrap to a
  small value, so the check passes. The code then executes
  `elf_data + offset` and copies `filesz` bytes from that address.
- **Impact:** out-of-bounds read from kernel memory; if the copy destination is
  a carveout visible to the remote processor, this can disclose memory
  preceding the firmware buffer. A page fault gives a kernel crash/DoS.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-005: firmware-controlled format string in carveout names

- **Files:**
  - `drivers/remoteproc/remoteproc_core.c:844`
    (`rproc_find_carveout_by_name(rproc, rsc->name)`)
  - `drivers/remoteproc/remoteproc_core.c:866`
    (`rproc_mem_entry_init(..., rsc->name)`)
  - `drivers/remoteproc/remoteproc_core.c:258`
    (`vsnprintf(_name, sizeof(_name), name, args)`)
  - `drivers/remoteproc/remoteproc_core.c:932`
    (`vsnprintf(mem->name, sizeof(mem->name), name, args)`)
- **Problem:** `struct fw_rsc_carveout.name` is a 32-byte firmware array with no
  NUL guarantee. It is passed as the **format string** to `vsnprintf()` with no
  variadic arguments. Format specifiers in the firmware name are interpreted:
  - `%n` writes through an argument slot that was never supplied, i.e. an
    attacker-influenced/uninitialized pointer;
  - `%s`/`%p` read from such a slot;
  - even a plain non-NUL-terminated name makes `vsnprintf()` read beyond the
    32-byte array.
- **Reachability:** `RSC_CARVEOUT` in a booted firmware resource table, and also
  on the attach path if the remote processor modifies its resource table. The
  i.MX driver uses the common resource handlers.
- **Impact:** potential kernel memory corruption or crash; at minimum an
  out-of-bounds read. This is the highest-impact core finding so far.
- **Confidence:** high.
- **Status:** static finding; should be reproduced with a crafted carveout
  name such as `%n` under KASAN.

### RPROC-006: `rsc_table_for_each_entry()` trusts `table->num` against `table_sz`

- **File:** `include/linux/rsc_table.h:312`
- **Code:** `offset = table->offset[i]` is read before proving that the
  `table->offset[]` array itself fits in `table_sz`.
- **Problem:** on the boot path `find_table()` validates
  `struct_size(table, offset, table->num) <= size`. On the attach path,
  `rproc_set_rsc_table()` (`remoteproc_core.c:1450`) takes the table pointer
  and size directly from the driver. i.MX `imx_rproc_get_loaded_rsc_table()`
  hardcodes `*table_sz = SZ_1K` and returns `priv->rsc_table`, which points at
  memory the remote processor can modify. A modified `table->num` therefore
  makes the iterator read `table->offset[i]` beyond the 1 KB region.
- **Impact:** out-of-bounds read from mapped remote memory; can cross a page
  boundary and crash, or feed bogus offsets into resource handlers.
- **Confidence:** high for the missing validation; runtime impact depends on
  the mapping around the 1 KB table.
- **Status:** static finding; i.MX attach path makes it reachable.

### RPROC-007: vring `num` and `align` are not validated

- **Files:**
  - `drivers/remoteproc/remoteproc_core.c:401` (`rproc_parse_vring()`)
  - `drivers/remoteproc/remoteproc_core.c:321` (`rproc_alloc_vring()`)
  - `drivers/remoteproc/remoteproc_virtio.c:142` (`rp_find_vq()`)
- **Problem:** `rproc_parse_vring()` only rejects zero `num`/`align`. It does
  not enforce the virtio requirement that `num` be a power of two, nor that
  `align` be a power of two or bounded. `rproc_alloc_vring()` and `rp_find_vq()`
  then use those values with `vring_size()`/`vring_init()`, whose alignment
  arithmetic assumes a power of two. A non-power-of-two `align` can make
  `vring_init()` compute a `used` pointer outside the allocated carveout.
- **Impact:** virtio ring corruption or out-of-bounds access from a malformed
  firmware resource table; remote-triggered on attach.
- **Confidence:** high that validation is missing; exact memory-safety impact
  needs a runtime test with crafted `align`/`num`.
- **Status:** static finding, candidate memory-safety bug.

### RPROC-008: `rproc_da_to_va()` truncates the address delta to `int`

- **File:** `drivers/remoteproc/remoteproc_core.c:201`
- **Code:** `int offset = da - carveout->da;` where `da` is `u64` and
  `carveout->da` is `u32`.
- **Problem:** the 64-bit subtraction is truncated to a signed 32-bit `int`.
  A 64-bit device address equal to `carveout->da + k * 2^32` aliases offset
  `k` and can match a carveout it does not belong to. The negative check then
  does not see the high bits.
- **Impact:** incorrect device-address translation; segment loading or coredump
  reads/writes can target the wrong carveout offset.
- **Confidence:** high for the truncation; whether a driver passes a >4 GB
  `da` depends on firmware/ELF64 usage.
- **Status:** static finding; hardening issue.

### RPROC-009: debugfs resource-table dump has signed-offset and bounds bugs

- **File:** `drivers/remoteproc/remoteproc_debugfs.c:275`
- **Code:** `rproc_rsc_table_show()` uses
  `int offset = table->offset[i]; struct fw_rsc_hdr *hdr = table + offset;`
  and dereferences `hdr->type` without checking `offset` or `table->num`
  against `rproc->table_sz`.
- **Problem:** this is the same signed-offset flaw that was fixed in
  `rsc_table_for_each_entry()` by commit `bb840ea69347`, but the debugfs copy
  was not fixed. It also has no truncation checks. The RSC_VDEV path loops to
  the table-supplied `v->num_of_vrings` (`:339`) without validating it or the
  surrounding resource-entry size, and the carveout/devmem/trace names are
  printed with `%s` without requiring the fixed-size firmware name arrays to
  be NUL-terminated (`:305`, `:315`, `:323`). The function reads `table_ptr`
  and `rproc_carveouts_show()` walks `rproc->carveouts` without `rproc->lock`,
  while `rproc_shutdown()`/`rproc_detach()` can free them.
- **Impact:** out-of-bounds read from a malformed/remote-modified table,
  unbounded vring dump, non-NUL name overread, and a use-after-free race
  against shutdown; root/debugfs-only.
- **Confidence:** high for the missing checks; the UAF is a race.
- **Status:** static finding (expanded).

### RPROC-010: vdev cleanup frees uninitialized vring slots

- **Files:**
  - `drivers/remoteproc/remoteproc_virtio.c:598`
    (`rproc_virtio_remove()`)
  - `drivers/remoteproc/remoteproc_core.c:406`
    (`rproc_free_vring()`)
- **Code:** `rproc_virtio_probe()` initializes `rvdev->vring[i]` only for
  `i < rsc->num_of_vrings` (`remoteproc_virtio.c:556`, `:566`), but
  `rproc_virtio_remove()` unconditionally loops
  `for (id = 0; id < ARRAY_SIZE(rvdev->vring); id++)` and calls
  `rproc_free_vring()` for both slots. `rproc_free_vring()` immediately
  dereferences `rvring->rvdev->rproc`.
- **Problem:** `rvdev` is `devm_kzalloc()`-allocated, so any vring slot not
  parsed by `rproc_parse_vring()` has `rvdev == NULL`. The core only rejects
  `rsc->num_of_vrings > 2` (`remoteproc_core.c:498`); `0` and `1` are accepted.
  A vdev with one vring therefore dereferences a NULL `rvring->rvdev` on the
  second cleanup iteration. A zero-vring vdev fails on the first.
- **Reachability:** every `rproc_shutdown()`/`rproc_detach()`/`rproc_del()`
  calls `rproc_resource_cleanup()`, which unregisters the platform device and
  invokes `rproc_virtio_remove()`. Triggerable with a valid one-vring firmware
  or a malformed zero-vring resource table.
- **Impact:** kernel NULL-pointer dereference (DoS) during normal shutdown;
  also blocks recovery paths that call resource cleanup.
- **Confidence:** high (direct NULL write/read path).
- **Status:** static finding; easy to reproduce with a crafted resource table.

### RPROC-011: i.MX DSP memset advances the MMIO pointer one byte at a time

- **File:** `drivers/remoteproc/imx_dsp_rproc.c:843`
- **Code:**
  ```c
  while (q--)
          writel(tmp_val, tmp_dst++);
  ```
  where `tmp_dst` is `void __iomem *`.
- **Problem:** each `writel()` writes four bytes, but `tmp_dst++` advances only
  one byte. The loop writes at addresses `addr + 0, addr + 1, ...`, producing
  unaligned/overlapping 32-bit MMIO writes after the first iteration. The
  trailing partial-word path then reads/writes `tmp_dst` at
  `addr + q_original`, while the next untouched word is at
  `addr + 4 * q_original`.
- **Origin:** commit `3c497f624d40` ("remoteproc: imx_dsp_rproc: Fix kernel
  test robot sparse warning") changed `u32 *tmp_dst` to
  `void __iomem *tmp_dst` without scaling the increment.
- **Reachability:** `imx_dsp_rproc_elf_load_segments()` calls this for every
  PT_LOAD segment with `memsz > filesz` (BSS), which is normal firmware.
- **Impact:** corrupted DSP IRAM contents, unaligned peripheral writes, and
  possible bus faults during firmware load.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-012: i.MX DSP tail masks include one bit too many

- **Files:**
  - `drivers/remoteproc/imx_dsp_rproc.c:799` (`imx_dsp_rproc_memcpy()`)
  - `drivers/remoteproc/imx_dsp_rproc.c:847` (`imx_dsp_rproc_memset()`)
- **Code:** `affected_mask = GENMASK(8 * r, 0);` for `1 <= r <= 3`.
- **Problem:** `GENMASK(8 * r, 0)` covers `8 * r + 1` bits; the intended mask
  is `GENMASK(8 * r - 1, 0)`. For `r == 1` the mask is `0x1ff` instead of
  `0xff`, so bit 8 (the low bit of the next byte in the 32-bit word) is
  cleared and/or set. `memcpy()` clears that adjacent bit; `memset()` sets it
  from `tmp_val`.
- **Reachability:** any segment whose size is not a multiple of four, which is
  common for firmware sections.
- **Impact:** silent one-bit corruption of the byte adjacent to each
  partial-word tail.
- **Confidence:** high.
- **Status:** static finding; predates the upstream history in this tree
  (`408ec1ff0caa`).

### RPROC-013: i.MX DA-translation checks can overflow and reject exact boundaries

- **Files:**
  - `drivers/remoteproc/imx_rproc.c:555`
    (`imx_rproc_da_to_sys()`)
  - `drivers/remoteproc/imx_rproc.c:588`
    (`imx_rproc_da_to_va()`)
- **Code:** `da + len < att->da + att->size` and `sys + len <`
  `priv->mem[i].sys_addr + priv->mem[i].size`.
- **Problem:** `da + len` and `sys + len` can wrap in 64-bit arithmetic;
  `att->da + att->size` is 32-bit arithmetic and can wrap; and the strict `<`
  rejects a range ending exactly at the region boundary. The translation then
  uses a truncated `unsigned int offset`. A crafted segment with a huge
  address/length can pass the wrapped comparison and translate to an
  unintended system address. A legitimate segment that exactly fills a
  region is rejected, causing firmware boot failure.
- **Upstream status:** `749eed0c7178` ("remoteproc: imx_rproc: Allow mappings
  ending at region boundary") fixes the overflow and boundary checks, but it
  is not an ancestor of HEAD (`git merge-base --is-ancestor` fails).
  `imx_dsp_rproc_sys_to_da()` at `imx_dsp_rproc.c:468` has the same
  `sys + len <= att->sa + att->size` pattern.
- **Impact:** valid-firmware boot failure; malformed-firmware address
  translation to the wrong location, with downstream OOB reads/writes.
- **Confidence:** high for the arithmetic; exploitability depends on firmware
  and the translation table entry.
- **Status:** static finding; fix exists upstream but not in this tree.

### RPROC-014: i.MX resource-table size is fixed at 1 KB regardless of mapping

- **Files:**
  - `drivers/remoteproc/imx_rproc.c:748`
    (`imx_rproc_get_loaded_rsc_table()` sets `*table_sz = SZ_1K`)
  - `drivers/remoteproc/imx_rproc.c:761`
    (`imx_rproc_elf_find_loaded_rsc_table()` returns `priv->rsc_table`)
  - `drivers/remoteproc/imx_rproc.c:846` (mapping set from the reserved
    region named `rsc-table`)
  - `drivers/remoteproc/remoteproc_core.c:1287`
    (`memcpy(loaded_table, rproc->cached_table, rproc->table_sz)`)
  - `drivers/remoteproc/remoteproc_core.c:1480`
    (`kmemdup(table_ptr, table_sz)` for the attach/detach clean table)
- **Problem:** the driver never checks `resource_size()` of the mapped
  `rsc-table` region. It assumes 1 KB on the attach path, and on the boot path
  it copies `rproc->table_sz` (the ELF table section size, which can exceed
  1 KB) into `priv->rsc_table`. The DT binding has no minimum-size
  requirement; all known i.MX DTs happen to use 1 KB.
- **Impact:** out-of-bounds read when `kmemdup()`/`rsc_table_for_each_entry()`
  reads 1 KB from a smaller mapping, and out-of-bounds write when boot
  `memcpy()` copies a larger ELF table into a smaller reserved region.
- **Confidence:** high for the missing size check; actual impact depends on
  the DT region size and firmware table size.
- **Status:** static finding.

### RPROC-015: mailbox error paths leave ERR_PTR or freed channels in private data

- **Files:**
  - `drivers/remoteproc/imx_rproc.c:908`
    (`imx_rproc_xtr_mbox_init()`)
  - `drivers/remoteproc/imx_rproc.c:923`
    (`imx_rproc_free_mbox()`)
  - `drivers/remoteproc/imx_dsp_rproc.c:586`
    (`imx_dsp_rproc_mbox_alloc()`)
  - `drivers/remoteproc/imx_dsp_rproc.c:640`
    (`imx_dsp_rproc_free_mbox()`)
  - `drivers/mailbox/mailbox.c:512` (`mbox_free_channel()`)
- **Problem:** `mbox_request_channel_byname()` returns an `ERR_PTR` on failure,
  but the i.MX drivers store it directly in `priv->tx_ch`/`rx_ch`/`rxdb_ch`
  and return. On partial failure they also free previously acquired channels
  without setting the pointers to NULL. `imx_rproc_free_mbox()` and
  `imx_dsp_rproc_free_mbox()` then pass those non-NULL values to
  `mbox_free_channel()`, which only checks `!chan || !chan->cl` and therefore
  dereferences the `ERR_PTR` or the freed channel.
- **Reachability:** mailbox request failure (deferral, missing controller, or
  probe error), or `imx_dsp_rproc_runtime_resume()` mailbox failure:
  `imx_dsp_rproc_prepare()` ignores the `pm_runtime_get_sync()` failure, so
  the boot path can continue and later `pm_runtime_put_sync()` triggers
  runtime suspend with stale pointers. The i.MX `start`/`attach` failure
  paths also leave stale pointers for the devm cleanup action.
- **Impact:** kernel NULL/ERR_PTR dereference, double free, or use-after-free
  during probe failure, shutdown, or runtime PM transitions.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-016: i.MX DSP start-timeout leaves the DSP running

- **Files:**
  - `drivers/remoteproc/imx_dsp_rproc.c:382`
    (`imx_dsp_rproc_start()`)
  - `drivers/remoteproc/imx_dsp_rproc.c:388`
    (`imx_dsp_rproc_ready()` timeout)
  - `drivers/remoteproc/remoteproc_core.c:1299`
    (`rproc_start()` failure path)
- **Problem:** `imx_dsp_rproc_start()` first starts the DSP through
  `dcfg->ops->start()` and then waits for the firmware-ready mailbox message.
  If the wait times out, it returns `-ETIMEDOUT`. The core `rproc_start()`
  goes to `unprepare_subdevices` on an `ops->start` failure and does **not**
  call `ops->stop`. The DSP remains powered and running while the core leaves
  the rproc in the offline state, so a later `rproc_shutdown()` rejects the
  request without stopping it.
- **Impact:** remote processor left running after a failed boot; subsequent
  boot attempts operate on live hardware and final removal cannot power it
  down.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-017: i.MX DSP asynchronous reload does not handle NULL firmware or removal

- **Files:**
  - `drivers/remoteproc/imx_dsp_rproc.c:1204`
    (`imx_dsp_load_firmware()`)
  - `drivers/remoteproc/imx_dsp_rproc.c:878`
    (`imx_dsp_rproc_elf_load_segments()` dereferences `fw->data`)
  - `drivers/remoteproc/imx_dsp_rproc.c:1295`
    (`request_firmware_nowait()` in `imx_dsp_resume()`)
- **Problem:** `request_firmware_nowait()` invokes its callback with
  `fw == NULL` when the request fails. `imx_dsp_load_firmware()` passes that
  pointer to `rproc_load_segments()` without a NULL check; the i.MX DSP load
  callback starts with `const u8 *elf_data = fw->data` and dereferences it.
  The callback also calls `rproc->ops->start()` and `kick()` directly, without
  `rproc->lock` or state validation, and its `context` is `rproc` while the
  firmware request only pins the parent device. A concurrent unbind/remove or
  userspace `stop` can free carveouts/the resource table or `rproc` itself.
- **Impact:** NULL-pointer dereference on firmware-request failure; UAF or
  inconsistent state on a race with shutdown/removal.
- **Confidence:** high for the NULL dereference; medium for the UAF race.
- **Status:** static finding.

### RPROC-018: i.MX8MP DSP reset ignores ioremap failure and reset errors

- **File:** `drivers/remoteproc/imx_dsp_rproc.c:216`
- **Code:** `void __iomem *dap = ioremap_wc(...);` followed immediately by
  `readl(dap + IMX8M_DAP_PWRCTL)`, and `reset_control_assert(priv->run_stall)`
  whose return value is ignored.
- **Problem:** `ioremap_wc()` can return `NULL`; the function dereferences it
  without checking. `reset_control_assert()` failure is also silently
  ignored, so the DSP may be left in an unexpected reset state.
- **Impact:** kernel NULL-pointer dereference if the mapping fails (for
  example, resource conflict or low VMALLOC space); silent reset failure.
- **Confidence:** high for the missing check; low likelihood in normal
  operation.
- **Status:** static finding.

### RPROC-019: i.MX DSP prepare ignores pm_runtime_get_sync() failure

- **File:** `drivers/remoteproc/imx_dsp_rproc.c:735`
- **Code:** `pm_runtime_get_sync(dev); return 0;`
- **Problem:** the return value is ignored. If runtime resume fails, the boot
  path continues to `imx_dsp_rproc_load()` and accesses DSP OCRAM with clocks
  and power possibly disabled; `imx_dsp_rproc_unprepare()` then calls
  `pm_runtime_put_sync()` unconditionally, further unbalancing the runtime PM
  reference count. This also feeds the stale-mailbox-pointer problem in
  RPROC-015.
- **Impact:** hang or crash during firmware load when runtime PM fails; PM
  refcount imbalance.
- **Confidence:** high.
- **Status:** static finding.

### RPROC-020: `rproc_del()` does not cancel the asynchronous attach work

- **Files:**
  - `drivers/remoteproc/remoteproc_core.c:1678`
    (`rproc_attach_work()`)
  - `drivers/remoteproc/remoteproc_core.c:1690`
    (`schedule_work(&rproc->attach_work)`)
  - `drivers/remoteproc/remoteproc_core.c:2639`
    (`rproc_del()` cancels only `crash_handler`)
- **Problem:** commit `026a3fada432` made the DETACHED attach path
  asynchronous by scheduling `attach_work`, but `rproc_del()` does not cancel
  it. If the device is removed before the workqueue runs, `rproc_del()` and
  `rproc_free()` can release the rproc; the pending work then calls
  `rproc_boot()` on freed memory. The `rproc_add()` error path likewise only
  cancels `crash_handler`.
- **Impact:** use-after-free race during device removal, especially with
  auto-boot and a promptly removed DETACHED rproc.
- **Confidence:** medium-high; the race window exists, but timing must be
  demonstrated.
- **Status:** static candidate; needs KASAN/KCSAN or a forced-workqueue
  reproducer. (The auto-boot firmware callback is protected because
  `request_firmware_nowait()` takes a reference on `&rproc->dev`; the
  `attach_work` path has no equivalent reference.)

### RPROC-021: vdev config access trusts firmware-writable counts against no table size

- **Files:**
  - `drivers/remoteproc/remoteproc_virtio.c:313`
    (`rproc_virtio_get()`)
  - `drivers/remoteproc/remoteproc_virtio.c:331`
    (`rproc_virtio_set()`)
  - `drivers/remoteproc/remoteproc_virtio.c:556`
    (`rproc_virtio_probe()` re-reads `rsc->num_of_vrings`)
- **Problem:** `cfg = &rsc->vring[rsc->num_of_vrings]` and the bounds check
  `offset + len > rsc->config_len` trust two fields in the resource table.
  They are not validated against `rproc->table_sz`. On the attach path
  `rproc->table_ptr` points into memory the remote processor can modify, after
  `rproc_handle_vdev()` performed its one-time `struct_size()` check. The
  probe loops also re-read `rsc->num_of_vrings` without a local upper bound;
  a remote change between the core validation and probe can make
  `rproc_parse_vring()`/`rproc_alloc_vring()` index past `rvdev->vring[2]`.
- **Impact:** out-of-bounds read/write in host memory (virtio config space or
  `rproc_vdev`), leading to corruption or DoS from a malicious/racing remote.
- **Confidence:** medium; requires the remote side to alter its table after
  attach validation.
- **Status:** static candidate; hardening gap.

### RPROC-022: i.MX prepare/probe truncation and PM error-path leaks

- **Files:**
  - `drivers/remoteproc/imx_rproc.c:661` (`da = res.start;`, `da` is `u32`)
  - `drivers/remoteproc/imx_rproc.c:673`
    (`rproc_coredump_add_segment()` return ignored)
  - `drivers/remoteproc/imx_rproc.c:1338`
    (`pm_runtime_enable()` then `pm_runtime_resume_and_get()` failure returns
    without `pm_runtime_disable()`/`pm_runtime_put_noidle()`)
- **Problem:** a reserved-memory region above 4 GB is truncated into the
  device address; a coredump-segment allocation failure is ignored; and a
  probe failure after runtime PM is enabled leaves runtime PM enabled and the
  reference unbalanced. `imx_dsp_rproc_sys_to_da()` has a similar
  `sys + len`/`u32` overflow pattern at `imx_dsp_rproc.c:468`.
- **Impact:** wrong carveout addresses on large systems, incomplete coredumps,
  PM state leak/unbalanced runtime PM on probe failure.
- **Confidence:** high for the error paths; low practical impact on current
  i.MX DTs.
- **Status:** static finding; low/medium severity.

## Out-of-scope observations (not counted as findings)

- The attach path in stm32/ti/xlnx also supplies fixed-size tables, so the
  RPROC-006 pattern may apply there too. Excluded per user scope.
- qcom `qcom_pas_parse_firmware()` passes the firmware ELF table into
  `qcom_pas_get_rsc_table()`; further validation depends on that helper.
  Excluded per user scope.

## Deferred / needs runtime proof

- RPROC-001..004: build a small ELF corpus and boot i.MX (or a synthetic rproc
  harness) with KASAN to confirm the OOB reads.
- RPROC-005: craft a `fw_rsc_carveout.name` containing `%n`/`%s` and observe
  under KASAN; check whether `%n` can reach a controlled pointer.
- RPROC-006: modify the i.MX loaded resource table's `num` field and read
  `resource_table`/boot to confirm the OOB.
- RPROC-007: craft a vdev with non-power-of-two `align`/`num` and inspect
  `vring_init()` pointer math; confirm with a userspace arithmetic harness.
- RPROC-009: race `cat resource_table` with `echo stop` under KASAN/KCSAN.
- RPROC-010: boot/stop an i.MX firmware whose vdev has one (and separately
  zero) vrings; confirm the NULL dereference in `rproc_free_vring()`.
- RPROC-011/012: load an i.MX DSP firmware with non-word-sized BSS and
  non-multiple-of-four segment lengths; inspect IRAM for the shifted tail
  writes and the extra adjacent bit.
- RPROC-013: rerun the boundary/overflow cases from `749eed0c7178` against
  HEAD (a segment exactly filling a region, plus a huge length/address).
- RPROC-014: shrink the DT `rsc-table` reserved region below the kernel's
  assumed table size and load/attach; KASAN should see the overread/overwrite.
- RPROC-015: force `mbox_request_channel_byname()` to fail or partially fail
  on i.MX7ULP/i.MX8MP and then unbind/stop; watch for ERR_PTR/double-free.
- RPROC-016/017: force the DSP ready timeout and a firmware-request failure,
  then check whether the DSP is still running and whether the async callback
  races with unbind.
- RPROC-020: force the attach workqueue to be pending while unbinding a
  DETACHED auto-boot rproc; KASAN should report the UAF.
- RPROC-021: on the attach path, change `num_of_vrings`/`config_len` after
  core validation and exercise virtio config reads/writes.

## Patch drafts (working tree, uncommitted)

Drafted against `f8aa0512a932` on `b4/remoteproc-bug`. No commits, no
`Signed-off-by` tags.

- RPROC-010: `struct rproc_vdev` now records `num_of_vrings`; probe
  validates it against `ARRAY_SIZE(rvdev->vring)` and remove loops only over
  the initialized entries.
- RPROC-011: `imx_dsp_rproc_memset()` advances `tmp_dst` by four bytes per
  word instead of one byte.
- RPROC-012: both i.MX DSP copy helpers use `GENMASK(8 * r - 1, 0)`.
- RPROC-013: applied the upstream fix `749eed0c7178` (not an ancestor of
  HEAD) for overflow-safe DA translation and ranges ending at a boundary;
  applied the same overflow/boundary handling to
  `imx_dsp_rproc_sys_to_da()`.
- RPROC-015: i.MX mailbox error paths clear `ERR_PTR`/freed channel pointers,
  and DSP free clears all three channels.
- RPROC-016: DSP start timeout now stops the core before returning the error.
- RPROC-017: `imx_dsp_load_firmware()` checks for a NULL firmware pointer,
  tracks the in-flight request with a completion, and waits for it on
  suspend/resume and from a devres action ordered before `rproc_remove`.
- RPROC-018: i.MX8MP DSP reset checks `ioremap_wc()` and propagates
  `reset_control_assert()` failure.
- RPROC-019: applied the upstream fix `13df1fa4f1a3`, using
  `pm_runtime_resume_and_get()` and balancing it on carveout failure.
- RPROC-020: applied the upstream fix `5490279e8274`, replacing the
  `attach_work`/`request_firmware_nowait` split with `boot_work` and
  cancelling it in `rproc_del()`.
- RPROC-014: record the mapped `rsc-table` size, reject ELF tables larger
  than that region, and report the mapped size on attach.
- RPROC-021: validate vdev/vring/config bounds against `rproc->table_sz` and
  use the probe-recorded vring count for config-space access.
- RPROC-022: reject carveouts above 32-bit device addresses, check coredump
  segment registration, and disable runtime PM when probe-time resume fails.
- RPROC-001/RPROC-002/RPROC-003: applied upstream `b32177ead5f8`, added a
  program-header table bound, and validated the string-table range,
  section-name offsets, and name termination in `find_table()`.
- RPROC-004: replaced the `offset + filesz` check with subtraction-based
  bounds checks.
- RPROC-005: copy firmware carveout names into a NUL-terminated buffer and
  pass them through `"%s"` instead of as a format string.
- RPROC-006: validate the resource-table header and offset array against
  `table_sz` before iteration.
- RPROC-007: require vring queue size and alignment to be powers of two.
- RPROC-008: keep the `rproc_da_to_va()` carveout offset in `u64` and use
  overflow-safe range checks.
- RPROC-009: bound debugfs resource-table entries and vdev vring arrays,
  limit name printing, and serialize dumps with `rproc->lock`.
- KUnit: added `remoteproc_kunit.c` covering ELF sanity/loading, resource
  table parsing and iteration, vring geometry, `rproc_da_to_va()`, and
  synthetic zero/one-vring `rproc-virtio` probe/remove lifecycles.
- KUnit: added an i.MX DSP test-only platform device via DT overlay
  (`imx_dsp_rproc_kunit.c`/`.dtso`) that binds the real `imx-dsp-rproc`
  driver and verifies probe/remove.

Verification performed:

- `git diff --check`: clean.
- `scripts/checkpatch.pl --no-signoff --no-tree -` on the working-tree diff:
  0 errors; one generic new-file MAINTAINERS warning, covered by the existing
  `drivers/remoteproc/` entry.
- Cross-build of `drivers/remoteproc/` with an arm64 defconfig plus
  `CONFIG_REMOTEPROC`, `CONFIG_IMX_REMOTEPROC`, and
  `CONFIG_IMX_DSP_REMOTEPROC` enabled: passed.
- KUnit on arm64 QEMU: `remoteproc-core` 12/12 and `imx-dsp-rproc` 1/1
  tests passed.
- KUnit on arm64 QEMU with `CONFIG_KASAN=y`: `remoteproc-core` 12/12 and
  `imx-dsp-rproc` 1/1 tests passed.
- Reproduction commands and the driver/fault-injection design are recorded in
  `remoteproc-kunit-testing.md` (English) and `remoteproc-testing-zh.md`
  (Chinese, complete test instructions).

Still unfixed/deferred:

- i.MX-specific mailbox, PM, reset, and race paths still need a synthetic
  driver/fault-injection suite or hardware-in-loop testing.

### Split patch series

The working-tree changes were split into twenty plain-diff patches under
`/tmp/opencode/remoteproc-patches/`. See `cover-letter.md` and `series` in
that directory. The series applies cleanly in order to the base index; no
commits or `Signed-off-by` tags were created.
