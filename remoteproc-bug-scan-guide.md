# Scanning the remoteproc subsystem for bugs

This is a practical, repeatable audit workflow tailored to this `linux-next` tree. It assumes the goal is both **robustness bugs** (malformed firmware, races, cleanup) and **security bugs** reachable from firmware, userspace, or the remote processor.

## 0. Threat model first

Remoteproc has several trust boundaries:

| Input | Trust | Notes |
|---|---|---|
| ELF firmware + resource table | Usually root-supplied, but treat as malformed-capable | Highest-yield parser bugs |
| Shared resource table after attach | Remote processor can modify it | `rproc->table_ptr` may point into device memory |
| `vring` / virtqueue contents | Remote processor and rpmsg clients | Memory corruption, index bugs |
| `/dev/remoteprocN` cdev | Root, but lifetime matters | Write/ioctl/release races |
| sysfs/debugfs | Root; debugfs may be broader | State changes, debugfs read UAF |
| Device tree | Usually bootloader-trusted | Missing/odd properties on probe |

The **attach path** is especially interesting because the resource table comes from memory the remote processor controls. The **firmware-load path** is privileged but should still never crash the kernel on a bad image.

## 1. Prepare a test kernel and build matrix

Use at least two architectures because `u32`/`size_t` truncation and compat paths differ:

```sh
make ARCH=arm64 allmodconfig
make ARCH=arm64 W=1 M=drivers/remoteproc
make ARCH=arm allmodconfig
make ARCH=arm W=1 M=drivers/remoteproc
make LLVM=1 ARCH=arm64 allmodconfig
make LLVM=1 ARCH=arm64 W=1 M=drivers/remoteproc
```

Enable these for runtime work:

```text
CONFIG_REMOTEPROC=y
CONFIG_RPMSG=y
CONFIG_RPMSG_CHAR=y
CONFIG_DEBUG_FS=y
CONFIG_KASAN=y
CONFIG_KCSAN=y
CONFIG_UBSAN=y
CONFIG_KFENCE=y
CONFIG_PROVE_LOCKING=y
CONFIG_DEBUG_ATOMIC_SLEEP=y
CONFIG_DEBUG_MUTEXES=y
CONFIG_DEBUG_SPINLOCK=y
CONFIG_DEBUG_LIST=y
CONFIG_DEBUG_OBJECTS_WORK=y
CONFIG_DEBUG_OBJECTS_TIMERS=y
CONFIG_REFCOUNT_FULL=y
CONFIG_SLUB_DEBUG=y
CONFIG_DMA_API_DEBUG=y
CONFIG_IOMMU_DEBUG=y
CONFIG_DEBUG_KMEMLEAK=y
CONFIG_FAULT_INJECTION=y
CONFIG_FAULT_INJECTION_DEBUG_FS=y
CONFIG_FAILSLAB=y
CONFIG_FAIL_PAGE_ALLOC=y
CONFIG_FAIL_USERCOPY=y
CONFIG_HARDENED_USERCOPY=y
```

Also test `CONFIG_REMOTEPROC=m`: module init/exit and remove paths often differ from built-in paths.

## 2. Automated static analysis

None of `sparse`, `smatch`, `clang`, `cppcheck`, or `spatch` are currently installed here. Install what you can:

```sh
sudo apt install sparse clang cppcheck coccinelle
# smatch: build from source, then put it in PATH
```

Run from a configured tree:

```sh
# sparse
make C=2 CHECK=sparse M=drivers/remoteproc

# smatch
make CHECK='smatch -p=kernel' C=2 M=drivers/remoteproc

# clang static analyzer
scan-build --use-cc=clang --use-c++=clang++ \
    make LLVM=1 M=drivers/remoteproc

# Coccinelle's built-in kernel rules
make coccicheck MODE=report M=drivers/remoteproc

# GCC analyzer (noisy, but catches leaks and null derefs)
make KCFLAGS=-fanalyzer M=drivers/remoteproc

# warnings
make W=1 M=drivers/remoteproc

# cppcheck, with kernel headers on the include path
cppcheck --enable=warning,performance,portability \
    --suppress=missingIncludeSystem \
    -I include -I arch/arm64/include \
    drivers/remoteproc/*.c
```

Then write targeted Coccinelle rules for patterns that matter here:

- `of_find_node_by_*()` / `of_parse_phandle()` without `of_node_put()`
- Ignored returns from `of_property_read_u32()`, `platform_get_irq()`, `devm_clk_get()`, `regulator_get()`, `reset_control_get()`, `mbox_request_channel()`
- `pm_runtime_get_sync()` without a matching `pm_runtime_put*()`
- `dma_map_*()` / `iommu_map()` / `ioremap()` without matching unmap on every error path
- `list_for_each_entry()` with deletion inside the loop
- `kfree()` of devm-managed memory
- `copy_from_user()` with a length not bounded by the destination
- `snprintf()`/`strcpy()`/`strcat()` on firmware- or DT-derived strings

Useful greps for manual triage:

```sh
rg -n "of_property_read_|of_get_|of_parse_|of_find_" drivers/remoteproc
rg -n "platform_get_irq|devm_clk_get|clk_get|regulator_get|reset_control_get|mbox_request" drivers/remoteproc
rg -n "pm_runtime_|pm_stay_awake|pm_relax" drivers/remoteproc
rg -n "dma_alloc|dma_map|iommu_map|ioremap|memremap|kzalloc|devm_kzalloc" drivers/remoteproc
rg -n "copy_from_user|copy_to_user|sysfs_streq|debugfs_create" drivers/remoteproc
rg -n "TODO|FIXME|XXX|WARN_ON|BUG_ON|BUG\(" drivers/remoteproc
rg -n "list_for_each_entry|list_add|list_del" drivers/remoteproc
rg -n "mutex_lock|spin_lock|rcu_read_lock|synchronize_rcu" drivers/remoteproc
rg -n "->state|state ==|state !=" drivers/remoteproc
```

## 3. Manual audit: highest-yield areas

### A. ELF and resource-table parsing

Start with `drivers/remoteproc/remoteproc_elf_loader.c` and `include/linux/rsc_table.h:303`.

Check every arithmetic operation for overflow and every array index for bounds:

- `rproc_elf_sanity_check()` at `remoteproc_elf_loader.c:37`
  - `shoff + elf_shdr_get_size` can overflow before the comparison.
  - It checks `phoff > fw->size`, but not that the entire program-header array (`phoff + phnum * phdr_size`) is inside the firmware.
  - It does not validate `e_shstrndx < e_shnum`, section-header table bounds, or the string-table bounds.
- `rproc_elf_load_segments()` at `:160`
  - It walks `phnum` program headers directly from `fw->data`; verify whether a short/malformed header table can cause out-of-bounds reads.
  - Check `offset + filesz`, `filesz > memsz`, `memsz` fitting in `size_t`, and `rproc_da_to_va()` length handling.
- `find_table()` at `:247`
  - `name_table = elf_data + sh_offset` and `strcmp(name_table + name, ...)` are used before validating the string table is inside the file and NUL-terminated.
  - Verify the section-header bounds and `shstrndx` handling.
- `rproc_elf_load_rsc_table()` at `:326`
  - Confirm `tablesz` is fully validated before `kmemdup()`, and that `table->num`/`table->offset[]` are consistent.
- `rsc_table_for_each_entry()` in `include/linux/rsc_table.h:303`
  - It indexes `table->offset[i]` before proving the offset array itself fits in `table_sz`. On the boot path `find_table()` checks this; on the attach path the table comes from remote memory, so verify each driver supplies a validated `table_sz`.

For every resource handler, check `avail` against the exact struct size, zero `reserved` fields, string termination, and arithmetic:

- `rproc_handle_vdev()` `remoteproc_core.c:471`
- `rproc_handle_trace()` `:541`
- `rproc_handle_devmem()` `:615`
- `rproc_handle_carveout()` `:819`

### B. vring, virtio, and rpmsg

Look at `rproc_parse_vring()`, `rproc_alloc_vring()` (`remoteproc_core.c:321`), and `remoteproc_virtio.c`.

- `num` and `align` are only checked for non-zero. Verify power-of-two requirements, maximum queue size, and `vring_size()`/`PAGE_ALIGN()` overflow.
- `da`, `len`, `notifyid`, and `idr_alloc()` interactions.
- `rproc_vq_interrupt()` at `remoteproc_virtio.c:90`: `notifyid` from hardware/firmware, `idr_find()`, RCU/`synchronize_cbs()` lifetime of `rvring->vq`.
- `rproc_free_vring()`, `rproc_remove_rvdev()`, and `rproc_vdev_release()`: use-after-free when the remote processor or a recovery cycle tears down vdevs while userspace still holds rpmsg endpoints.
- `vring` memory mapping and IOMMU/DMA teardown ordering.

### C. Lifecycle, state machine, and concurrency

The state machine is `RPROC_OFFLINE/SUSPENDED/RUNNING/CRASHED/ATTACHED/DETACHED`. Audit transitions and lifetimes across:

- `rproc_boot()` `:1932`, `__rproc_shutdown()` `:1991`, `rproc_detach()` `:2086`
  - `atomic_inc_return(&rproc->power)` / `atomic_dec_and_test()` balance on every error path.
  - State changes under `rproc->lock`; identify reads that occur without it.
- `rproc_trigger_recovery()` `:1845` and `rproc_crash_handler_work()` `:1882`
  - Crash can arrive from IRQ context; recovery runs on a workqueue.
  - `pm_stay_awake()`/`pm_relax()` balance.
  - Race with `rproc_del()` and the `deleting` flag.
- `rproc_del()` `:2628`, `rproc_free()`, `devm_rproc_add()`
  - Open cdev/debugfs/sysfs file across remove/unbind.
  - Pending `crash_handler`, `attach_work`, recovery work, subdevice work.
  - Recent commits in this tree (`74ee3b2f5767`, `4059c64d78b5`, `0ec167625b4f`) explicitly fix crash/del races; inspect those changes for incomplete fixes and regression potential.
- cdev paths in `remoteproc_cdev.c`
  - `rproc_cdev_write()` and `rproc_cdev_release()` read `rproc->state` without `rproc->lock`; test concurrent `start`/`stop`/`detach` and `RPROC_SET_SHUTDOWN_ON_RELEASE` with `rproc_del()`.
- sysfs paths in `remoteproc_sysfs.c`
  - `recovery_store()`, `coredump_store()`, `state_store()` can initiate boot/shutdown/recovery concurrently.
- debugfs paths in `remoteproc_debugfs.c`
  - Open file + device removal; trace/carveout/resource-table reads while shutdown frees `table_ptr`/mem entries; crash injection.
- `rproc_get_by_phandle()` / `rproc_put()`
  - Device and module refcount balance.

### D. Coredump

`remoteproc_coredump.c`:

- Segment list additions (`rproc_coredump_add_segment()`), cleanup, and lifetime across recovery.
- `rproc_coredump_read()` `:164`: offset arithmetic, zero-length segments, `da + offset`, `rproc_da_to_va()` failure handling, and whether `count > 0` can loop forever.
- Inline dumping blocks recovery until userspace reads; check shutdown/remove while a dump is pending.
- `rproc_coredump_using_sections()` and driver-supplied dump callbacks.

### E. Per-driver audit

After the core, repeat the same checklist for each driver. Highest complexity/risk first:

- `qcom_q6v5_mss.c`, `qcom_q6v5_pas.c`, `qcom_q6v5_adsp.c`, `qcom_q6v5_wcss.c`
  - TrustZone/PAS calls, IOMMU map/unmap, handover IRQ, reset, regulators, clocks, mailbox, SMEM, GLINK, sysmon.
  - Probe/remove error unwinding and runtime PM.
- `ti_k3_r5_remoteproc.c`, `ti_k3_common.c`, `imx_rproc.c`, `imx_dsp_rproc.c`, `stm32_rproc.c`, `mtk_scp.c`, `pru_rproc.c`, `xlnx_r5_remoteproc.c`
  - DT property parsing, address/size bounds, mailbox, reset, memory regions, firmware-name parsing.
- `remoteproc_core.c` callers of `dma_alloc_coherent()`, `iommu_map()`, `devm_ioremap()`, `of_reserved_mem_device_init()`.

For each driver, walk every `goto` label in probe/remove/start/stop and ask:

1. Is every acquired resource released on every path?
2. Is release order the reverse of acquisition?
3. Can the operation sleep in atomic context?
4. Is `of_node_put()` missing?
5. Is a return value ignored?
6. Can userspace trigger start/stop while remove is running?

## 4. Dynamic testing, fault injection, and fuzzing

### Build a synthetic rproc harness

There is no mainline dummy remoteproc driver, so the most effective approach is a small test module or KUnit/selftest that:

1. Allocates an `rproc` with `rproc_alloc()` and dummy `rproc_ops`.
2. Registers it with `rproc_add()`.
3. Serves a synthetic ELF/resource table via the firmware loader or sysfs.
4. Exercises `start`, `stop`, `detach`, `crash`, coredump, and remove.
5. Registers a `platform_device` so unbind/bind and module unload can be tested.

Use `tools/testing/selftests/` or a temporary out-of-tree module. KUnit is good for pure parser tests (ELF headers, resource tables, vring arithmetic); a platform module is better for lifetime/race testing.

### Stress and race testing

With a test device at `/sys/class/remoteproc/remoteproc0` and `/dev/remoteproc0`:

```sh
for i in $(seq 1 10000); do
    echo start  > /sys/class/remoteproc/remoteproc0/state
    echo stop   > /sys/class/remoteproc/remoteproc0/state
done
```

In parallel, from other shells:

```sh
# crash/recovery loop
while :; do
    echo 1 > /sys/kernel/debug/remoteproc/remoteproc0/crash
    echo recover > /sys/class/remoteproc/remoteproc0/recovery
done

# coredump read loop
while :; do cat /sys/kernel/debug/remoteproc/remoteproc0/coredump > /dev/null; done

# cdev start/stop loop
while :; do
    printf start > /dev/remoteproc0
    printf stop  > /dev/remoteproc0
done

# ioctl loop
while :; do
    # use a small program calling RPROC_SET_SHUTDOWN_ON_RELEASE /
    # RPROC_GET_SHUTDOWN_ON_RELEASE
done
```

Also loop driver unbind/bind:

```sh
echo <device> > /sys/bus/platform/drivers/<driver>/unbind
echo <device> > /sys/bus/platform/drivers/<driver>/bind
```

Run with KASAN, KCSAN, KFENCE, lockdep, and `panic_on_warn=1`. KCSAN is especially useful for the unlocked `rproc->state` reads and recovery/remove races.

### Fault injection

```sh
mount -t debugfs none /sys/kernel/debug
echo 100 > /sys/kernel/debug/failslab/probability
echo 1000 > /sys/kernel/debug/failslab/times
echo 1 > /sys/kernel/debug/failslab/verbose
```

Do the same for `fail_page_alloc` and `fail_usercopy`. This is the fastest way to find missing cleanup in probe/remove, boot/shutdown, and resource-table handling.

### Fuzzing

- Search syzbot/lore for existing `remoteproc`, `rpmsg`, and `virtio_rpmsg` reports and replay reproducers.
- syzkaller can reach `/dev/remoteprocN` and `/dev/rpmsg*` if your harness exposes them. Enable KCOV.
- Fuzz the ELF/resource-table parser directly with a userspace harness that calls the same validation logic, or via a test module accepting firmware blobs through configfs/debugfs. Keep the harness out of the final patch.
- Fuzz `rpmsg_char` and virtio-rpmsg paths separately; they are a common source of remote-triggered bugs.

## 5. Triage and reporting

For every candidate:

1. Reproduce on a clean tree with the sanitizer that caught it.
2. Get a symbolized trace: `scripts/decode_stacktrace.sh` or `scripts/faddr2line`.
3. Check whether it is already fixed upstream/stable: `git log -S`, `git log --oneline -- drivers/remoteproc`, lore.kernel.org search.
4. Classify reachability: root-only firmware load vs remote-processor-triggered vs user-triggered.
5. Minimize the input or race, then verify the fix under the same sanitizer.
6. Get maintainers: `./scripts/get_maintainer.pl -f drivers/remoteproc/<file>`.
7. For security issues, follow `Documentation/process/security-bugs.rst`; for normal bugs, report to the remoteproc list and maintainers.
8. If you use an LLM to write the patch, follow `Documentation/process/coding-assistants.rst`.

## 6. Suggested first pass

Do this in order; it gives the best bug-per-hour:

1. **ELF/resource parser**: `rproc_elf_sanity_check()`, `rproc_elf_load_segments()`, `find_table()`, `rsc_table_for_each_entry()`, and all four resource handlers. This is the highest-yield area for OOB and integer bugs.
2. **Lifecycle races**: cdev/sysfs/debugfs vs `rproc_del()`/`rproc_free()`, crash/recovery vs remove, `power` refcount errors.
3. **vring/virtio**: `num`/`align`/`da` validation, `vring_size()` overflow, notifyid/IDR, vq teardown.
4. **Driver probe/remove error paths**: qcom first, then ti/imx/stm32/mtk.
5. **Coredump/debugfs**: offset arithmetic and lifetime across shutdown/remove.
6. **Fault injection + KCSAN stress** on the harness, then replay any sanitizer reports.

If you want, I can start with pass 1 on this tree and produce a list of concrete candidates with file:line references and proposed reproducers.
