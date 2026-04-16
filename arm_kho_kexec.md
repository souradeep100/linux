# ARM64 KHO/Kexec Changes Analysis

## Overview

Analysis of changes between commit `4af0231aafb9` (Drivers: hv: vmbus: fix hyperv_cpuhp_online variable shadowing) and HEAD (`b3bbbe72b141`, branch `schakrabarti/arm64-mshv-kho-for-l1vh`) to identify what work is needed for ARM64 KHO (Kexec Handover) support for MSHV L1VH.

> **Note:** The SharePoint link (https://microsoft.sharepoint.com/:p:/s/LPGTechnicalPresentations/...) requires authentication and could not be accessed.

---

## Commits in Scope (15 total)

| # | Commit | Title | Arch Scope |
|---|--------|-------|------------|
| 1 | `7099133ba192` | x86/hyperv: move stimer cleanup to hv_machine_shutdown() | **x86 only** |
| 2 | `4f6dd20e704f` | x86/hyperv: Skip LP/VP creation on kexec | **x86 only** |
| 3 | `21abba29df09` | mshv: limit SynIC management to MSHV-owned resources | Arch-independent (drivers/hv) |
| 4 | `a4ea6f0b2235` | mshv: clean up SynIC state on kexec for L1VH | Arch-independent (drivers/hv) |
| 5 | `800d9d397787` | mshv: unmap debugfs stats pages on kexec | Arch-independent (drivers/hv) |
| 6 | `2423a7132136` | DONOTMERGE: trace SynIC MSR state during init and kexec shutdown | **x86 only** (debug traces) |
| 7 | `d21ccd52d074` | kho: adopt radix tree for preserved memory tracking | Arch-independent (kernel/liveupdate) |
| 8 | `887744d84a8c` | kho: remove finalize state and clients | Arch-independent (kernel/liveupdate) |
| 9 | `ce6252935ab1` | kho: Add radix tree initializer and metadata walk callback | Arch-independent (kernel/liveupdate) |
| 10 | `d50c6a3b7fa6` | mshv: Use page tracker to manage MSHV-owned pages and preserve with KHO | Arch-independent (drivers/hv) |
| 11 | `21b3c95fd70c` | mshv: Add debugfs interface to page tracker | Arch-independent (drivers/hv) |
| 12 | `e22ec81e7215` | kho: Add crash-kernel-safe radix tree presence check | Arch-independent (kernel/liveupdate) |
| 13 | `8eb272b130d8` | hyperv: Reserve crash MSR P2 for page preservation root PA | **Both x86 & arm64** |
| 14 | `63f01cbba335` | mshv: Exclude Hyper-V donated pages from crash dump collection | Arch-independent (drivers/hv) |
| 15 | `b3bbbe72b141` | mshv: freeze and vacuum partitions across kexec | Arch-independent (drivers/hv) |

---

## Changes Already Done for ARM64

### 1. Crash MSR P2 Reservation (`8eb272b130d8`)
- **File:** `arch/arm64/hyperv/hv_core.c`
- **Change:** Reserves `HV_REGISTER_GUEST_CRASH_P2` for KHO preserved-pages tree root PA (instead of `regs->pc`). Shifts PC to P3 and SP to P4.
- **Status:** ✅ Done

---

## Changes Required for ARM64

### Category 1: Architecture-Specific Port Required

#### 1.1 stimer Cleanup Before Kexec (port of `7099133ba192`)

**x86 implementation** (`arch/x86/kernel/cpu/mshyperv.c`):
```c
static void hv_machine_shutdown(void)
{
    if (kexec_in_progress) {
        hv_stimer_global_cleanup();
        if (hv_kexec_handler)
            hv_kexec_handler();
    }
    if (kexec_in_progress)
        cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE);
    native_machine_shutdown();
    if (kexec_in_progress)
        hyperv_cleanup();
}
```

**ARM64 gap:**
- ARM64 has no `hv_machine_shutdown()` override. The default `machine_shutdown()` in `arch/arm64/kernel/process.c` just calls `smp_shutdown_nonboot_cpus(reboot_cpu)`.
- `hv_setup_kexec_handler()` / `hv_setup_crash_handler()` on arm64 call the `__weak` empty stubs in `hv_common.c` — the function pointers are **never stored**, so vmbus's `hv_kexec_handler` and `hv_crash_handler` are **never called**.
- For L1VH (`hv_curr_partition_type == HV_PARTITION_TYPE_L1VH`), `hv_root_partition()` returns **false**, so VMBus **IS** initialized (the early `return 0` in `hv_acpi_init()` is NOT taken). This means vmbus cleanup IS needed during kexec for L1VH.
- No mechanism to call `hv_stimer_global_cleanup()` before kexec on arm64.
- No `cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE)` teardown on kexec for arm64 (but secondary CPUs DO get cleaned up via `smp_shutdown_nonboot_cpus()` walking cpuhp states — only CPU0 is missed).
- No `hyperv_cleanup()` implementation for arm64 — falls through to `__weak` empty stub in `hv_common.c`. Arm64 needs its own implementation to zero Guest OS ID via `hv_set_vpreg(HV_REGISTER_GUEST_OS_ID, 0)`.
- No `vmbus_initiate_unload()` call during kexec on arm64 — VMBus connection left stale.

**What's needed:**
- ARM64 needs a way to hook into the kexec shutdown path. Options:
  - a) Use a reboot notifier (like MSHV does in `mshv_root_main.c`) from the hyperv platform code — works for pre-CPU-stop cleanup (stimer, vmbus) but NOT for post-CPU-stop cleanup (hyperv_cleanup)
  - b) Override `machine_shutdown()` by adding `__weak` hooks — allows both pre-CPU-stop and post-CPU-stop cleanup
  - c) Implement arm64 overrides for `hv_setup_kexec_handler()` and `hv_setup_crash_handler()` in `arch/arm64/hyperv/mshyperv.c` (stores the fn ptrs), plus hook `machine_shutdown()` to call them
- Implement `hyperv_cleanup()` for arm64 in `arch/arm64/hyperv/hv_core.c` (zero Guest OS ID at minimum).
- Ensure `hv_stimer_global_cleanup()` runs before vmbus unload on arm64 kexec. The function is available via `include/clocksource/hyperv_timer.h` and is arch-independent.

**Priority:** HIGH — without this, stale stimer state can cause issues in kexec'd kernel, CPU0 VP Assist Page will be corrupted, VMBus connection will be stale.

---

#### 1.2 Skip LP/VP Creation on Kexec (port of `4f6dd20e704f`)

**x86 implementation** (`arch/x86/kernel/cpu/mshyperv.c`):
```c
static void __init hv_smp_prepare_cpus(unsigned int max_cpus)
{
    ...
    /* If AP LPs exist, we are in a kexec'd kernel and VPs already exist */
    if (num_present_cpus() == 1 || hv_lp_exists(1))
        return;

    for_each_present_cpu(i) {
        ret = hv_call_add_logical_proc(numa_cpu_node(i), i, cpu_physical_id(i));
        BUG_ON(ret);
    }
    ret = hv_call_notify_all_processors_started();
    ...
    for_each_present_cpu(i) {
        ret = hv_call_create_vp(numa_cpu_node(i), hv_current_partition_id, i, i);
        BUG_ON(ret);
    }
}
```

**ARM64 gap:**
- ARM64 hyperv code (`arch/arm64/hyperv/mshyperv.c`) has NO `hv_smp_prepare_cpus()` override.
- ARM64 currently does NOT call `hv_call_add_logical_proc()` or `hv_call_create_vp()` at all.
- The helper functions `hv_lp_exists()` and `hv_call_notify_all_processors_started()` are in `drivers/hv/hv_proc.c` and are arch-independent.
- New declarations added to `include/asm-generic/mshyperv.h` and `include/hyperv/hvhdk_mini.h` are available for arm64.

**What's needed:**
- Determine if ARM64 root/L1VH partitions need LP/VP creation during boot (similar to x86 `#ifdef CONFIG_X86_64` block).
- If ARM64 does LP/VP creation (possibly via a different path), need to add `hv_lp_exists()` guard to skip re-creation on kexec.
- If ARM64 does NOT do LP/VP creation (relies on hypervisor), this may not be needed — **needs verification**.

**Priority:** HIGH — if arm64 does LP/VP creation, re-creating them after kexec will BUG_ON or cause MCEs.

---

#### 1.3 SynIC Trace Debugging (port of `2423a7132136`)

**x86 implementation** (`arch/x86/hyperv/hv_init.c`, `arch/x86/kernel/cpu/mshyperv.c`):
- Adds `SYNIC-TRACE` pr_info statements throughout `hyperv_cleanup()`, `hv_machine_shutdown()`, and `hv_cpu_die()`.

**ARM64 gap:**
- No equivalent tracing in arm64 hyperv code.
- This is a `DONOTMERGE` commit — debug only.

**What's needed:**
- Optional: Add similar traces for arm64 debugging if needed during development.
- Do NOT merge these traces; they're temporary debug aids.

**Priority:** LOW — debug only, not for production.

---

### Category 2: Arch-Independent (Should Work on ARM64 as-is)

These changes are in `drivers/hv/` and `kernel/liveupdate/` and use arch-independent APIs. They should work on ARM64 **provided the Kconfig prerequisites are met**.

| Commit | Component | Notes |
|--------|-----------|-------|
| `21abba29df09` | `drivers/hv/mshv_synic.c` | Uses `hv_get_non_nested_msr()`/`hv_set_non_nested_msr()` — arm64 has these in `arch/arm64/include/asm/mshyperv.h` |
| `a4ea6f0b2235` | `drivers/hv/mshv_root_main.c`, `mshv_synic.c` | SynIC cleanup via reboot notifier — arch-independent |
| `800d9d397787` | `drivers/hv/mshv_debugfs.c`, `mshv_root_main.c` | Stats page unmap — arch-independent |
| `d21ccd52d074` | `kernel/liveupdate/kexec_handover.c` | KHO radix tree — arch-independent |
| `887744d84a8c` | `kernel/liveupdate/` | Remove finalize state — arch-independent |
| `ce6252935ab1` | `kernel/liveupdate/`, `include/linux/kho_radix_tree.h` | Radix tree helpers — arch-independent |
| `d50c6a3b7fa6` | `drivers/hv/mshv_page_preserve.c` (new file) | Page tracker — no arch guards, arch-independent |
| `21b3c95fd70c` | `drivers/hv/mshv_debugfs.c` | Debugfs for page tracker — arch-independent |
| `e22ec81e7215` | `kernel/liveupdate/kexec_handover.c` | Crash-safe radix check — arch-independent |
| `63f01cbba335` | `drivers/hv/mshv_page_preserve.c` | Crash dump exclusion — arch-independent |
| `b3bbbe72b141` | `drivers/hv/mshv_root_main.c` | Partition freeze/vacuum — arch-independent |

---

### Category 3: Kconfig / defconfig Changes Required

#### 3.1 Enable KHO in ARM64 defconfig

**Current state:**
- `arch/arm64/Kconfig` already has `ARCH_SUPPORTS_KEXEC_HANDOVER` (`def_bool y`) — arm64 **does** support KHO at the Kconfig level.
- `arch/arm64/configs/mshv_defconfig` does **NOT** have `CONFIG_KEXEC_HANDOVER=y`.
- The defconfig has `CONFIG_KEXEC_FILE=y` and `CONFIG_KEXEC_CORE=y`.

**What's needed:**
Add to `arch/arm64/configs/mshv_defconfig`:
```
CONFIG_KEXEC_HANDOVER=y
CONFIG_KEXEC_HANDOVER_ENABLE_DEFAULT=y
CONFIG_KEXEC_HANDOVER_DEBUGFS=y
```

**Priority:** HIGH — KHO won't work without this.

---

## Summary: ARM64 Action Items

| # | Action | Priority | Effort | Files | Path |
|---|--------|----------|--------|-------|------|
| 1 | **Enable KHO in mshv_defconfig** | HIGH | Low | `arch/arm64/configs/mshv_defconfig` | Kexec |
| 2 | **Implement arm64 `hv_setup_kexec_handler()` / `hv_setup_crash_handler()`** — store fn ptrs so vmbus handlers get called | HIGH | Low | `arch/arm64/hyperv/mshyperv.c` | Kexec + Crash |
| 3 | **Hook arm64 `machine_shutdown()` for kexec cleanup** — call `hv_stimer_global_cleanup()`, `hv_kexec_handler()`, `cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE)`, and `hyperv_cleanup()` | HIGH | Medium | `arch/arm64/kernel/process.c` or `arch/arm64/hyperv/mshyperv.c` | Kexec |
| 4 | **Hook arm64 `machine_crash_shutdown()` for crash cleanup** — call `hv_crash_handler()`, `hv_stimer_cleanup(cpu)`, `hv_hyp_synic_disable_regs(cpu)`, `hyperv_cleanup()` | HIGH | Medium | `arch/arm64/kernel/machine_kexec.c` or `arch/arm64/hyperv/mshyperv.c` | Crash |
| 5 | **Implement `hyperv_cleanup()` for arm64** — zero Guest OS ID via `hv_set_vpreg(HV_REGISTER_GUEST_OS_ID, 0)` | HIGH | Low | `arch/arm64/hyperv/hv_core.c` | Kexec + Crash |
| 6 | **Investigate LP/VP creation on arm64** — determine if arm64 does LP/VP creation and needs the skip-on-kexec guard | HIGH | Medium | `arch/arm64/hyperv/mshyperv.c` | Kexec |
| 7 | **Verify all drivers/hv changes compile on arm64** | MEDIUM | Low | Build test | Both |
| 8 | **Add SynIC debug traces for arm64** (optional, do not merge) | LOW | Low | `arch/arm64/hyperv/hv_core.c` | Debug |

---

## KHO Concepts Reference

From [kernel.org KHO documentation](https://docs.kernel.org/next/kho/concepts.html):

- **KHO Device Tree:** Carries serialized state across kexec using FDT format with native endianness. Contains `mem` properties for physical memory ranges that must be reserved after kexec.
- **Scratch Regions:** Physically contiguous memory regions allocated on first boot, declared as CMA. Used as target for kexec kernel/initrd placement. Reused across recursive KHO kexecs.
- **KHO Active Phase:** State where the system has serialized its device state into the FDT for kexec. Some properties become immutable.
- **Radix Tree:** New approach (in this patch series) replaces xarray-based tracking. Leaf nodes are bitmaps of preserved pages. Root PA passed via FDT.

---

## Execution Paths: Reboot, Kexec, and Crash

### Path 1: Normal Reboot / Shutdown (no kexec)

Hardware reset at the end — hypervisor reinitializes everything, so stale Hyper-V state is harmless.

```
kernel_restart(cmd)                                   [kernel/reboot.c]
  → kernel_restart_prepare(cmd)
      → blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART, cmd)
          ← mshv reboot notifier fires here (KHO preservation, partition freeze)
          ← NO vmbus/hyperv cleanup needed — hardware reset follows
      → device_shutdown()
          ← vmbus_shutdown() called on each vmbus device
  → migrate_to_reboot_cpu()
  → syscore_shutdown()
  → machine_restart(cmd)                              [arch/arm64/kernel/process.c]
      → local_irq_disable()
      → smp_send_stop()                               ← IPI halt, NO cpuhp teardown
      → efi_reboot() / PSCI reset                     ← HARDWARE RESET — HV cleans up
```

**ARM64 L1VH status:** Works — no changes needed for normal reboot.

---

### Path 2: Kexec Reboot (kexec -e)

No hardware reset — hypervisor state persists. All cleanup must be done in software.

**x86 L1VH path (working):**
```
kernel_kexec()                                        [kernel/kexec_core.c]
  → kexec_in_progress = true
  → kernel_restart_prepare("kexec reboot")
      → blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART)
          ← mshv_reboot_notify(): cpuhp_remove_state(mshv_cpuhp_online), debugfs teardown
          ← mshv reboot_cb(): freeze partitions, preserve pages via KHO FDT
      → device_shutdown()
          ← vmbus_shutdown() on each vmbus device
  → migrate_to_reboot_cpu()
  → syscore_shutdown()
  → cpu_hotplug_enable()
  → machine_shutdown()
      → machine_ops.shutdown = hv_machine_shutdown()   [arch/x86/kernel/cpu/mshyperv.c]
          → hv_stimer_global_cleanup()                 ← stops stimers, removes CPUHP_AP_HYPERV_TIMER_STARTING
          → hv_kexec_handler()                         ← vmbus_initiate_unload(false), cpuhp_remove_state(hyperv_cpuhp_online)
          → cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE) ← calls hv_cpu_die() on ALL CPUs incl CPU0
          → native_machine_shutdown()                  ← stop_other_cpus()
          → hyperv_cleanup()                           ← zeros Guest OS ID, disables hypercall page
  → machine_kexec(kexec_image)                         ← jump to new kernel
```

**arm64 L1VH path (BROKEN — current state):**
```
kernel_kexec()                                        [kernel/kexec_core.c]
  → kexec_in_progress = true
  → kernel_restart_prepare("kexec reboot")
      → blocking_notifier_call_chain(&reboot_notifier_list, SYS_RESTART)
          ← mshv_reboot_notify(): cpuhp_remove_state(mshv_cpuhp_online), debugfs teardown  ✅
          ← mshv reboot_cb(): freeze partitions, preserve pages via KHO FDT                ✅
      → device_shutdown()
          ← vmbus_shutdown() on each vmbus device                                          ✅
  → migrate_to_reboot_cpu()
  → syscore_shutdown()
  → cpu_hotplug_enable()
  → machine_shutdown()                                [arch/arm64/kernel/process.c]
      → smp_shutdown_nonboot_cpus(reboot_cpu)
          → cpu_down_maps_locked() for each secondary CPU
              ← walks cpuhp states DOWN including CPUHP_AP_HYPERV_ONLINE                   ✅ (secondary CPUs only)
              ← walks cpuhp states DOWN including hyperv_cpuhp_online (vmbus SynIC)        ✅ (secondary CPUs only)
          ← CPU0 is NOT taken through cpuhp teardown                                       ❌ GAP
      ← NO hv_stimer_global_cleanup()                                                     ❌ GAP
      ← NO vmbus_initiate_unload()                                                        ❌ GAP
      ← NO cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE) for CPU0                            ❌ GAP
      ← NO hyperv_cleanup()                                                                ❌ GAP (no arm64 impl exists)
  → machine_kexec(kexec_image)                         ← jump to new kernel with stale HV state
```

**Gaps for arm64 kexec reboot:**

| # | Missing Cleanup | Impact | When (relative to CPU stop) |
|---|----------------|--------|---------------------------|
| 1 | `hv_stimer_global_cleanup()` | Stale timer interrupts crash new kernel | BEFORE stopping CPUs |
| 2 | `vmbus_initiate_unload(false)` | VMBus connection left stale for new kernel | BEFORE stopping CPUs |
| 3 | `cpuhp_remove_state(hyperv_cpuhp_online)` | VMBus SynIC on CPU0 not cleaned | BEFORE stopping CPUs |
| 4 | `cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE)` | CPU0 VP Assist Page corrupted by hypervisor | BEFORE stopping CPUs |
| 5 | `hyperv_cleanup()` | Guest OS ID and hypercall page not reset | AFTER stopping CPUs (single CPU) |

---

### Path 3: Crash Kexec (kdump)

Triggered by panic/oops. Runs in atomic context with interrupts disabled — cannot schedule, cannot do cpuhp teardown.

**x86 L1VH path:**
```
panic() / oops
  → crash_kexec(regs)
      → __crash_kexec(regs)
          → machine_crash_shutdown(regs)
              → machine_ops.crash_shutdown
              → For L1VH: hv_root_partition() == false, so hv_guest_crash_shutdown() IS set
              → hv_guest_crash_shutdown(regs)           [arch/x86/kernel/cpu/mshyperv.c]
                  → hv_crash_handler(regs)              ← vmbus crash handler (registered via hv_setup_crash_handler)
                      → vmbus_initiate_unload(true)     ← unload with crash flag
                      → hv_stimer_cleanup(cpu)          ← current CPU only (can't schedule)
                      → hv_hyp_synic_disable_regs(cpu)  ← current CPU SynIC disable
                  → native_machine_crash_shutdown(regs) ← crash_smp_send_stop(), save regs
                  → hyperv_cleanup()                    ← zeros Guest OS ID, disables hypercall page
          → machine_kexec(kexec_crash_image)            ← jump to kdump kernel
```

**arm64 L1VH path (BROKEN — current state):**
```
panic() / oops
  → crash_kexec(regs)
      → __crash_kexec(regs)
          → machine_crash_shutdown(regs)                [arch/arm64/kernel/machine_kexec.c]
              → local_irq_disable()
              → crash_smp_send_stop()                   ← halt secondary CPUs (IPI, no cpuhp)
              → crash_save_cpu(regs, cpu)
              → machine_kexec_mask_interrupts()
              ← NO vmbus_initiate_unload()              ❌ GAP
              ← NO hv_stimer_cleanup(cpu)               ❌ GAP
              ← NO hv_hyp_synic_disable_regs(cpu)       ❌ GAP
              ← NO hyperv_cleanup()                     ❌ GAP (no arm64 impl exists)
          → machine_kexec(kexec_crash_image)            ← jump to kdump kernel with stale HV state
```

**Gaps for arm64 crash kexec:**

| # | Missing Cleanup | Impact | Notes |
|---|----------------|--------|-------|
| 1 | `vmbus_initiate_unload(true)` | VMBus stale for kdump kernel | vmbus `hv_crash_handler` never called |
| 2 | `hv_stimer_cleanup(smp_processor_id())` | Stale stimer on crash CPU | Only current CPU matters (crash context) |
| 3 | `hv_hyp_synic_disable_regs(smp_processor_id())` | Stale SynIC on crash CPU | Only current CPU matters |
| 4 | `hyperv_cleanup()` | Guest OS ID / hypercall page stale | No arm64 implementation exists |
| 5 | `hv_setup_crash_handler()` is a no-op | vmbus crash handler never registered | `__weak` empty stub in `hv_common.c` |

**Note:** In crash context, only minimal cleanup is possible — no scheduling, no cross-CPU operations. The x86 code deliberately only cleans up the current CPU's stimer and SynIC.

---

### Path Summary: What Fires When (arm64 L1VH)

| Mechanism | Normal Reboot | Kexec Reboot | Crash Kexec |
|-----------|:---:|:---:|:---:|
| Reboot notifiers (mshv KHO) | ✅ | ✅ | ❌ (no notifiers in panic) |
| `device_shutdown()` (vmbus devices) | ✅ | ✅ | ❌ |
| `smp_shutdown_nonboot_cpus()` cpuhp teardown | ❌ (uses `smp_send_stop`) | ✅ (secondary CPUs) | ❌ (uses `crash_smp_send_stop`) |
| `hv_stimer_global_cleanup()` | ❌ (not needed) | ❌ **GAP** | N/A (use per-cpu) |
| `hv_stimer_cleanup(cpu)` for crash CPU | N/A | N/A | ❌ **GAP** |
| `vmbus_initiate_unload()` | ❌ (not needed) | ❌ **GAP** | ❌ **GAP** |
| `cpuhp_remove_state(CPUHP_AP_HYPERV_ONLINE)` CPU0 | ❌ (not needed) | ❌ **GAP** | N/A (can't schedule) |
| `hyperv_cleanup()` | ❌ (not needed) | ❌ **GAP** | ❌ **GAP** |
| Hardware reset | ✅ | ❌ | ❌ |

---

## Architecture Differences: x86 vs ARM64

| Aspect | x86 | ARM64 |
|--------|-----|-------|
| Machine shutdown override | `hv_machine_shutdown()` via `machine_ops.shutdown` | No override mechanism — `machine_shutdown()` is a direct function |
| Machine crash shutdown override | `hv_guest_crash_shutdown()` via `machine_ops.crash_shutdown` (guest/L1VH only) | No override — `machine_crash_shutdown()` is a direct function |
| `hv_setup_kexec_handler()` | Stores fn ptr in static variable, called from `hv_machine_shutdown()` | `__weak` empty stub in `hv_common.c` — fn ptr discarded, never called |
| `hv_setup_crash_handler()` | Stores fn ptr in static variable, called from `hv_guest_crash_shutdown()` | `__weak` empty stub in `hv_common.c` — fn ptr discarded, never called |
| `hyperv_cleanup()` | Full impl in `hv_init.c`: zeros Guest OS ID, disables hypercall page, TSC ref page | **No arm64 impl** — `__weak` empty stub in `hv_common.c` |
| SMP prepare hook | `hv_smp_prepare_cpus()` overrides `x86_platform` | No SMP prepare override |
| LP/VP creation | Done in `hv_smp_prepare_cpus()` | **Not done** — needs investigation |
| MSR access | `wrmsrq()`/`rdmsrq()`, `native_read_msr()` | `hv_set_vpreg()`/`hv_get_vpreg()` |
| Non-nested MSR | `hv_get_non_nested_msr()` / `hv_set_non_nested_msr()` | Same API, different impl in `asm/mshyperv.h` |
| KHO Kconfig | `ARCH_SUPPORTS_KEXEC_HANDOVER` = X86_64 | `ARCH_SUPPORTS_KEXEC_HANDOVER` = y (always) |
| CPU hotplug state | `CPUHP_AP_HYPERV_ONLINE` with `hv_cpu_die`/`hv_cpu_init` | `CPUHP_AP_HYPERV_ONLINE` with `hv_common_cpu_init`/`hv_common_cpu_die` |
| Partition type for L1VH | `hv_root_partition()` = false, `hv_l1vh_partition()` = true | Same — `hv_curr_partition_type == HV_PARTITION_TYPE_L1VH` |
| VMBus initialized for L1VH? | Yes — `hv_acpi_init()` early return is `hv_root_partition() && !hv_nested`, L1VH is not root | Yes — same logic, VMBus IS initialized |
