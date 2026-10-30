# Remoteproc 测试完整说明（中文）

本文记录当前工作树中 remoteproc 相关修复的全部测试方法，包括：

- KUnit 核心解析测试；
- i.MX DSP 的 test-only platform device 测试；
- arm64 QEMU + KASAN 运行方法；
- 实际硬件板卡测试；
- 故障注入测试的设计和当前实现状态；
- 常见问题、预期结果和限制。

英文版本见 `remoteproc-kunit-testing.md`。

## 1. 当前测试资产

### 1.1 核心 KUnit 测试

文件：

```text
drivers/remoteproc/remoteproc_kunit.c
drivers/remoteproc/.kunitconfig
```

覆盖范围：

- ELF sanity check：合法 ELF、截断的 section header table、截断的
  program header table。
- ELF segment 加载：正常拷贝和清零、ELF64 `p_offset` 溢出。
- Resource table：合法表、越界 string table、非 NUL 结尾的 section 名、
  非法 section name offset。
- `rsc_table_for_each_entry()`：合法遍历、非法 header、非法 offset
  array、截断 entry。
- `rproc_da_to_va()`：起始/结束边界、超长范围、大于 4 GB 的地址别名。
- `rproc_parse_vring()`：queue size 和 alignment 的 2 的幂校验。
- `rproc-virtio` 生命周期：零 vring 和一个 vring 的 probe/remove。

当前共有 12 个核心测试。

### 1.2 i.MX DSP test-only platform device

文件：

```text
drivers/remoteproc/imx_dsp_rproc_kunit.c
drivers/remoteproc/imx_dsp_rproc_kunit.dtso
```

DT overlay 会创建一个 test-only platform device：

- 一个 dummy `syscon@10000000`；
- 一个 `fsl,imx8ulp-hifi4` 节点；
- 节点带有 `firmware-name` 和 `fsl,dsp-ctrl` 属性。

测试会应用 overlay，等待设备 probe，然后用
`of_find_device_by_node()` 找到 platform device，确认
`dev_get_drvdata()` 返回真实的 `imx-dsp-rproc` 实例。

当前有 1 个 i.MX DSP 测试。

### 1.3 配置开关

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

这些配置已经写入 `drivers/remoteproc/.kunitconfig`。

## 2. 环境准备

### 2.1 必需工具

```text
aarch64-linux-gnu-gcc
qemu-system-aarch64
python3
```

确认工具存在：

```sh
command -v aarch64-linux-gnu-gcc
command -v qemu-system-aarch64
python3 --version
```

### 2.2 使用干净的工作树

`kunit.py` 使用 `O=` 构建，但会拒绝带有 in-tree 构建产物的源码树，例如：

```text
arch/arm64/include/generated
.config
```

推荐使用一个独立的干净 worktree，并把补丁应用进去：

```sh
git worktree add /tmp/remoteproc-kunit HEAD

for p in /tmp/opencode/remoteproc-patches/*.patch; do
	git -C /tmp/remoteproc-kunit apply "$p" || exit 1
done

cd /tmp/remoteproc-kunit
```

如果不想使用 worktree，也可以使用一个干净的单独 checkout。

不要在带有重要 in-tree 构建产物的源码树中直接运行，因为 KUnit 可能
要求清理这些产物。不要为了运行测试随意删除未提交的工作。

### 2.3 当前补丁系列

补丁文件位于：

```text
/tmp/opencode/remoteproc-patches/
```

其中：

- `series` 是补丁顺序；
- `cover-letter.md` 是补丁说明；
- `0020-remoteproc-add-kunit-tests.patch` 包含 KUnit 和 test-only
  platform device。

补丁按顺序应用：

```sh
for p in /tmp/opencode/remoteproc-patches/*.patch; do
	git -C /tmp/remoteproc-kunit apply "$p" || exit 1
done
```

## 3. 运行核心 KUnit 测试

### 3.1 标准运行

在干净的源码树中执行：

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

说明：

- 最后一个参数是 KUnit 的 **filter glob**，不是 `--filter` 属性过滤器。
- `--filter` 用于属性过滤，例如 `speed>slow`、`module=...`。
  把 `remoteproc-core` 传给 `--filter` 会报
  `filter operation not found`。
- `kunit.py` 会自动启动 QEMU；正常测试不需要手动执行 QEMU 命令。
- 第一次运行会构建 arm64 内核，通常需要数分钟。
- 后续运行复用 `/tmp/opencode/kunit-build`，速度会快很多。
- `--timeout=300` 只限制测试运行时间，不限制构建时间。

预期输出：

```text
remoteproc-core (12 subtests)
[PASSED] rproc_kunit_elf_sanity_valid
[PASSED] rproc_kunit_elf_sanity_bad_sections
[PASSED] rproc_kunit_elf_sanity_bad_program_headers
[PASSED] rproc_kunit_elf_load_segments_valid
[PASSED] rproc_kunit_elf_load_segments_offset_overflow
[PASSED] rproc_kunit_elf_rsc_table_valid
[PASSED] rproc_kunit_elf_rsc_table_bad_string_table
[PASSED] rproc_kunit_rsc_table_iterator
[PASSED] rproc_kunit_da_to_va
[PASSED] rproc_kunit_parse_vring
[PASSED] rproc_kunit_vdev_zero_vrings
[PASSED] rproc_kunit_vdev_one_vring
================= [PASSED] remoteproc-core =================
Testing complete. Ran 12 tests: passed: 12
```

### 3.2 只运行单个测试

例如只运行 ELF sanity 的合法路径：

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-build \
	--kunitconfig=drivers/remoteproc \
	--jobs=2 \
	--timeout=300 \
	'remoteproc-core.rproc_kunit_elf_sanity_valid'
```

### 3.3 查看完整启动日志

如果测试失败，需要看内核日志：

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-build \
	--kunitconfig=drivers/remoteproc \
	--jobs=2 \
	--timeout=300 \
	--raw_output=all \
	'remoteproc-core'
```

### 3.4 强制重新构建

```sh
rm -rf /tmp/opencode/kunit-build
```

然后重新执行标准运行命令。

## 4. 运行 i.MX DSP test-only platform device 测试

### 4.1 标准运行

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

预期输出：

```text
imx-dsp-rproc (1 subtest)
[PASSED] imx_dsp_rproc_kunit_probe
================== [PASSED] imx-dsp-rproc ==================
Testing complete. Ran 1 tests: passed: 1
```

### 4.2 这个测试实际做了什么

1. `of_overlay_apply_kunit()` 应用
   `imx_dsp_rproc_kunit.dtso`。
2. overlay 创建 dummy syscon 和 `fsl,imx8ulp-hifi4` platform device。
3. `imx-dsp-rproc` 驱动 probe：
   - 找到 `fsl,dsp-ctrl` syscon；
   - 解析 `firmware-name`；
   - 初始化 remoteproc；
   - 调用 `rproc_add()`。
4. 测试调用 `wait_for_device_probe()`。
5. 测试通过 `of_find_device_by_node()` 找到 platform device。
6. 测试检查 `dev_get_drvdata()` 得到 `struct rproc *`，并确认
   `rproc->name` 为 `imx-dsp-rproc`。
7. 测试结束后 KUnit 自动移除 overlay，平台设备被 remove。

该测试不需要 DSP 硬件、不需要真实 firmware，也不访问真实寄存器。
它只验证 i.MX DSP driver 的 probe/remove 生命周期。

## 5. KASAN 测试

使用独立的 build 目录，避免覆盖普通配置。

### 5.1 核心 KUnit + KASAN

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

预期：12/12 通过，且没有 KASAN report。

### 5.2 i.MX DSP platform device + KASAN

```sh
python3 tools/testing/kunit/kunit.py run \
	--arch=arm64 \
	--cross_compile=aarch64-linux-gnu- \
	--build_dir=/tmp/opencode/kunit-imx-kasan \
	--kunitconfig=drivers/remoteproc \
	--kconfig_add CONFIG_KASAN=y \
	--jobs=2 \
	--timeout=300 \
	'imx-dsp-rproc'
```

预期：1/1 通过，且没有 KASAN report。

当前已验证：

```text
remoteproc-core: 12/12 passed, normal + KASAN
imx-dsp-rproc: 1/1 passed, normal + KASAN
```

## 6. 编译和静态检查

### 6.1 只编译 KUnit 对象

```sh
make O=/tmp/opencode/kbuild ARCH=arm64 \
	CROSS_COMPILE=aarch64-linux-gnu- \
	drivers/remoteproc/remoteproc_kunit.o

make O=/tmp/opencode/kbuild ARCH=arm64 \
	CROSS_COMPILE=aarch64-linux-gnu- \
	drivers/remoteproc/imx_dsp_rproc_kunit.o
```

配置中必须有：

```text
CONFIG_KUNIT=y
CONFIG_REMOTEPROC=y
CONFIG_RPROC_KUNIT_TEST=y
CONFIG_IMX_DSP_RPROC_KUNIT_TEST=y
```

### 6.2 编译整个 remoteproc 目录

```sh
make O=/tmp/opencode/kbuild ARCH=arm64 \
	CROSS_COMPILE=aarch64-linux-gnu- -j2 drivers/remoteproc/
```

### 6.3 checkpatch

```sh
git diff --check

git diff | scripts/checkpatch.pl --no-signoff --no-tree -
```

当前结果：

- 0 errors；
- 1 个通用 warning：新增文件时提示检查 MAINTAINERS。
- `drivers/remoteproc/` 已经在 MAINTAINERS 的 remoteproc entry 中，
  因此这个 warning 不需要修改 MAINTAINERS。

## 7. 硬件测试

KUnit 只能证明核心数据路径和 test-only platform device。真实的 i.MX
mailbox、reset、runtime PM、firmware handshake 和 secure boot 路径必须
在硬件上验证。

### 7.1 板卡和兼容性

可用组合包括：

- i.MX8MP EVK：
  - M7：`fsl,imx8mp-cm7`；
  - DSP：`fsl,imx8mp-hifi4`。
- i.MX8QM/QXP MEK：
  - DSP：`fsl,imx8qm-hifi4` / `fsl,imx8qxp-hifi4`。
- i.MX7ULP、i.MX8MM、i.MX8MN、i.MX8MQ：
  - M4/M7：对应的 `fsl,*-cm4` / `fsl,*-cm7`。
- i.MX8ULP、i.MX93、i.MX94、i.MX95：
  - 对应 CM33/CM7/CM70/CM71 节点。

需要：

- 板卡正常启动的 bootloader 和 DTB；
- 与板卡匹配、可启动的 remote firmware；
- 串口 console；
- 一个已知可用的 firmware 和一个单独的测试 firmware。

如果板卡启用 secure boot，测试 firmware 必须按板卡的安全启动流程签名，
否则 remote core 可能无法启动。

### 7.2 内核配置

至少启用：

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

`CONFIG_KCSAN` 对异步 reload 和 work 取消测试尤其有用。

### 7.3 基本 smoke test

启动后：

```sh
dmesg | grep -i remoteproc
ls -l /sys/class/remoteproc/
cat /sys/class/remoteproc/remoteproc0/name
cat /sys/class/remoteproc/remoteproc0/firmware
cat /sys/class/remoteproc/remoteproc0/state
```

启动和停止：

```sh
echo start > /sys/class/remoteproc/remoteproc0/state
cat /sys/class/remoteproc/remoteproc0/state
echo stop > /sys/class/remoteproc/remoteproc0/state
```

如果需要在 offline 状态切换 firmware：

```sh
cat /sys/class/remoteproc/remoteproc0/state
echo test-firmware.elf > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
```

挂载 debugfs 并查看 resource table：

```sh
mount -t debugfs none /sys/kernel/debug
cat /sys/kernel/debug/remoteproc/remoteproc0/resource_table
cat /sys/kernel/debug/remoteproc/remoteproc0/carveout_memories
```

如果启用了 `CONFIG_REMOTEPROC_CDEV=y`，也可以使用：

```text
/dev/remoteproc0
```

字符设备接口提供 boot/shutdown 控制。

### 7.4 按 finding 设计硬件测试

#### RPROC-001..004：ELF 解析和 segment 边界

- 使用测试 ELF 触发截断 section header、截断 program header、
  非法 string table、`p_offset` 溢出。
- 在 KASAN 内核上启动，观察 `dmesg`。
- 预期：驱动返回错误，不出现 OOB read。

#### RPROC-005：carveout 名字格式字符串

- 构造带 `%n`、`%s` 或非 NUL 结尾 name 的 firmware resource table。
- 启动并查看 `dmesg`。
- 预期：name 被当作普通字符串处理，不执行格式字符串。

#### RPROC-006：resource table offset 数组

- 修改 loaded resource table 的 `num` 或 `offset[]`。
- 启动并读取 debugfs `resource_table`。
- 预期：驱动检测到 truncated table，不越界读取。

#### RPROC-007：vring geometry

- 构造 `num` 或 `align` 不是 2 的幂的 vdev。
- 启动并观察 vring 创建。
- 预期：`rproc_parse_vring()` 返回 `-EINVAL`。

#### RPROC-008：`rproc_da_to_va()` 地址别名

- 使用大于 4 GB 的 device address 和 carveout 组合。
- 启动加载 segment 或触发 coredump。
- 预期：地址不会因 `int` 截断而匹配到错误 carveout。

#### RPROC-009：debugfs resource table dump

- 在 running 状态反复读取：

```sh
cat /sys/kernel/debug/remoteproc/remoteproc0/resource_table
```

- 同时执行 stop/start。
- 预期：无 KASAN/KCSAN report，非法 vring count 不会造成越界。

#### RPROC-010：vdev 少于两个 vring

- 使用带 0 个和 1 个 vring 的测试 firmware。
- 反复 start/stop。
- 预期：stop 时只清理已初始化的 vring，不出现 NULL dereference。
- KUnit 已经覆盖该路径。

#### RPROC-011/012：DSP memcpy/memset tail

- 构造 BSS/segment 长度不是 4 的倍数的 DSP firmware。
- 让 firmware 自己校验 BSS，或在允许的情况下用 `devmem` 读取 DSP
  IRAM。
- 预期：tail 字节准确，没有整体偏移，也没有多写一个 bit。

#### RPROC-013：地址边界

- 使用 upstream commit `749eed0c7178` 中的 linker script 布局，
  让 LOAD segment 正好结束在映射区域边界。
- 预期：不再出现 `bad phdr da ...`，边界地址被接受。

#### RPROC-014：rsc-table 区域大小

- 在测试 DTB 中缩小 `rsc-table` reserved-memory 区域，使其小于
  firmware resource table。
- 启动。
- 预期：解析/启动失败，KASAN 不报告越界读写。

#### RPROC-015：mailbox 失败

- 在测试 DTB 中删除 `mboxes`/`mbox-names`，或只保留部分通道。
- 启动 remote core。
- 预期：mailbox 请求失败时指针被清理，不出现 `ERR_PTR` 或 double-free。
- 也可以在 remote core 停止时 unbind mailbox controller，再重新 start。

#### RPROC-016：ready timeout

- 使用不发送 ready handshake 的 firmware，或在测试 DTB 中删除
  `rxdb` 通道。
- 执行 start。
- 预期：返回 `-ETIMEDOUT`，DSP 被 stop，而不是继续运行。

#### RPROC-017：异步 reload 和 unbind

- 在 DSP running 时触发 suspend/resume：

```sh
echo mem > /sys/power/state
```

- 在 reload 尚未完成时，另开终端 unbind DSP platform device。
- 重复 unbind/probe。
- 预期：没有 use-after-free，devres wait 生效，下一次 probe 仍正常。
- 需要足够大的 firmware，使异步请求保持 pending 一段时间。

#### RPROC-018：reset 失败

- reset controller 失败通常不能从用户态注入。
- 使用测试内核补丁或 test reset provider 返回 `-EIO`。
- 启动 DSP。
- 预期：start 失败，DSP 不被留在半 reset 状态。

#### RPROC-019：runtime PM 失败

- 在测试 PM-domain 或测试补丁中让 resume 失败。
- 启动 DSP。
- 预期：`prepare` 失败，runtime PM 计数平衡，后续 start 仍可用。

#### RPROC-020：auto-boot work 和删除竞争

- 在测试 DT 中启用 `fsl,auto-boot`，或构造 DETACHED 状态。
- 在 auto-boot work 尚未完成时 unbind rproc platform device。
- 预期：unbind 等待/取消 work，KASAN 不报告 UAF。

#### RPROC-021：virtio config 越界

- 使用会在启动后修改 `num_of_vrings` 或 `config_len` 的测试 remote
  firmware。
- 通过 rpmsg/virtio client 访问 config space。
- 预期：访问被拒绝，不出现越界读写。

#### RPROC-022：carveout 地址和 PM

- 在测试 DTB 中放置大于 4 GB 的 carveout，确认被拒绝。
- 用 `CONFIG_FAILSLAB` 或内存压力让 coredump 分配失败，确认只产生
  warning。
- 用测试补丁让 probe PM 失败，确认错误路径执行
  `pm_runtime_disable()`。

### 7.5 硬件测试的局限

以下路径通常不能只靠 stock 硬件和用户态完成：

- reset controller 和 PM domain 的精确失败注入；
- secure firmware 验证之后的畸形 ELF/resource table；
- remote 在验证后修改 resource table；
- 精确控制异步 reload 的时序。

这些路径应使用 test-only platform device、fake mailbox/reset/regmap/PM
后端或测试内核补丁。硬件测试主要用于最终确认：

- mailbox 和 MU 通路；
- reset 和 runtime PM；
- firmware handshake；
- 真实 DSP/M7 启动和停止；
- secure boot 和 firmware 签名流程。

## 8. Driver/fault-injection suite 是什么

它不是 QEMU 板卡模型，也不是随机 fuzzing。它是一套 **test-only 内核
测试装置**，通过可控的假后端确定性地触发错误路径。

### 8.1 当前已经实现

- `rproc-virtio` 的 test-only platform device；
- i.MX DSP 的 DT overlay test-only platform device；
- 真实 `imx-dsp-rproc` 的 probe/remove；
- 零/一个 vring 的 vdev 生命周期。

### 8.2 还需要增加的后端

- fake mailbox controller：
  - 让 tx/rx/rxdb 请求分别失败；
  - 制造 partial allocation；
  - 延迟或丢弃 callback。
- fake reset controller：
  - assert/deassert 返回 `-EIO`；
  - 延迟或超时。
- fake regmap：
  - read/write 返回错误；
  - 模拟 syscon 访问失败。
- fake runtime PM：
  - resume 失败；
  - suspend 失败；
  - 在 reload pending 时 remove。
- fake firmware loader：
  - 延迟 callback；
  - 返回 NULL；
  - 在 callback 前触发 unbind；
  - 用于 RPROC-017 和 RPROC-020 的 KCSAN 测试。

### 8.3 推荐实现方式

1. 在 test-only platform device 的 platform data 中加入 fault flags；
2. 用 KUnit 的 `kunit_add_action()` 管理 cleanup；
3. 每个测试只注入一个故障，确保失败点明确；
4. 同时开启 KASAN 和 KCSAN；
5. 对异步路径使用 completion 控制 callback 时序，不要用 sleep 猜测；
6. 对 i.MX static helper，优先通过 test-only platform device 或
   test-only wrapper 调用，不要把 `.c` 文件直接 include 到测试中。

## 9. 推荐测试顺序

1. 核心 KUnit：

```sh
kunit.py run ... 'remoteproc-core'
```

2. 核心 KUnit + KASAN。

3. i.MX DSP test-only platform device：

```sh
kunit.py run ... 'imx-dsp-rproc'
```

4. i.MX DSP + KASAN。

5. 编译整个 `drivers/remoteproc/`。

6. 在硬件上做 smoke test：probe、start、stop、debugfs。

7. 在硬件上做 finding-specific 测试。

8. 最后再运行 fault-injection suite（mailbox/reset/PM/async）。

## 10. 当前状态汇总

```text
核心 KUnit:                 12/12 通过
核心 KUnit + KASAN:         12/12 通过
i.MX DSP platform device:    1/1  通过
i.MX DSP + KASAN:            1/1  通过
arm64 remoteproc 交叉编译:   通过
checkpatch:                  0 errors，1 个 MAINTAINERS 通用 warning
硬件测试:                    尚未执行
mailbox/reset/PM 故障注入:    尚未实现
```

## 11. 注意事项

- 不要在真实硬件上直接加载未签名或未验证的 firmware，尤其是 secure
  boot 板卡。
- 测试前备份已知可用的 firmware 和 DTB。
- 使用独立 build 目录，避免 KASAN 配置覆盖普通配置。
- KUnit filter 使用位置参数；`--filter` 是属性过滤器。
- 如果源码树不干净，先使用 worktree，不要随意删除用户文件。
- 当前补丁没有提交，也没有 `Signed-off-by`；正式提交前必须由人工
  审核并添加 DCO sign-off。
