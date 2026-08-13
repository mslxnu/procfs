# mSL/ProcFS
[![C/C++ CI](https://github.com/somestupidgirl/mSL-ProcFS/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/somestupidgirl/mSL-ProcFS/actions/workflows/c-cpp.yml) 
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-macOS-lightgrey)](#build)
[![Architecture](https://img.shields.io/badge/Architecture-arm64-blue)](#status)
[![Architecture](https://img.shields.io/badge/Architecture-x86_64-blue)](#status)
[![GitHub stars](https://img.shields.io/github/stars/somestupidgirl/mSL-ProcFS.svg?style=social&label=Star&maxAge=2592000)](https://GitHub.com/somestupidgirl/mSL-ProcFS/stargazers/)

**macOS Subsystem for Linux / ProcFS** — a native kernel-extension implementation of the
`/proc` file system for macOS, exposing running processes and threads as a filesystem
with BSD- and Linux-compatible per-process information.

<p align="center">
<img width="1470" height="956" alt="procfs" src="https://github.com/user-attachments/assets/2e7502d2-40a7-424b-9551-f2038516d02e" />
</p>

mSL/ProcFS is a kernel-extension implementation of the `/proc` file system for macOS,
exposing running processes and threads as a filesystem with BSD- and Linux-compatible
per-process information.

I started this project several years ago as a fork of the [`ProcFS`](https://github.com/kimtopley/ProcFS)
kernel patch for XNU by Kim Topley--a bare minimum procfs implementation for XNU. I had
been trying to port some Linux utilities over to macOS but they relied heavily on Linux'
/proc filesystem. It annoyed me that /proc didn't exist on macOS so, using the patch as
a foundation (with the original author's permission), I turned it into a kext and began
expanding upon it. It has since diverged heavily from the original kernel patch, with
various feature additions and a FreeBSD-style Linux-compatibility layer.

The code was written entirely by hand from 2022 to 2024 and I put a lot of research and
effort into it. I picked it up again in 2026 to port it over to Apple Silicon and add
more BSD and Linux-compatibility features, using Claude to speed up development and
perform large batch operations. It now also features a GUI and can be used to supplement
my [mSL/NABI](https://github.com/somestupidgirl/mSL-NABI) project.

Originally I used an x86_64 symbol resolver for resolving private kernel symbols. This
has turned out not to be ideal for Apple Silicon so it now also features a user-space
daemon for feeding the relevant data to the kernel extension.

Since this is a kernel extension it does require partially disabling SIP via recovery.
It may also run into issues on older kernel versions. mSL/ProcFS is currently only
officially supported for Apple Silicon running macOS 26.5.2 (Darwin 25.5.0), retaining
untested legacy support for x86_64.

I might try to port this over to FSKit later down the road depending on how feasible
it is but for now it is not supported.

## The larger project

mSL/XNU — macOS Subsystem for Linux / X is Now UNIX — is a set of compatibility layers
and utilities for macOS that aim at improving system compatibility with ported Linux,
BSD and other Unix-like code.

It also aims at native, seamless execution of Linux ELF binaries on macOS: not in a
container and not in a virtual machine, but as ordinary processes on the running system.

Reaching that needs several independent pieces, which is why the project is modular
rather than one monolith. Each is useful on its own, and each can be installed, replaced
or omitted:

| Piece | What it does | Where |
|-------|--------------|-------|
| **Syscall translation** | Linux system calls onto Darwin's, over `Hypervisor.framework` | [mSL/NABI](https://github.com/mslxnu/mSL-NABI) |
| **Filesystem Hierarchy Standard** | Native Linux-style filesystem layout | [mSL/FHS](https://github.com/mslxnu/fhs) |
| **procfs** | `/proc`, as a native pseudo-filesystem | **this repository** |
| **sysfs** | `/sys`, as a native pseudo-filesystem | [mSL/SysFS](https://github.com/mslxnu/sysfs) |
| **devfs** | `/dev` driver add-ons for macOS | [mSL/DevFS](https://github.com/mslxnu/devfs) |

**This repository is the ProcFS piece**, and it is largely done. The
rest of this document describes it.

## What is ProcFS?
*procfs* lets you view the processes running on a UNIX system as nodes in the file system,
where each process is represented by a single directory named from its process id. Typically,
the file system is mounted at `/proc`, so the directory for process 1 would be called `/proc/1`.
Beneath a process’ directory are further directories and files that give more information about
the process, such as its process id, its active threads, the files that it has open, and so on.
*procfs* first appeared in an early version of AT&T’s UNIX and was later implemented in various
forms in System V, BSD, Solaris and Linux. You can find a history of the implementation of
*procfs* at https://en.wikipedia.org/wiki/Procfs.

In addition to letting you visualize running processes, *procfs* also allows some measure of
control over them, at least to suitably privileged users. By writing specific data structures to
certain files, you could do such things as set breakpoints and read and write process memory and
registers. In fact, on some systems, this was how debugging facilities were provided. However,
more modern operating systems do this differently, so some UNIX variants no longer include an
implementation of *procfs*. In particular, macOS doesn’t provide *procfs* so, although it’s not
strictly needed, I thought that implementing it would be an interesting side project. The code in
this repository provides an implementation of *procfs* for macOS. You can use it to see what
processes and threads are running on the system and what files they have open. Later, I plan to
add more features, beginning with the ability to inspect a thread’s address space to see which
executable it is running and what shared libraries it has loaded.

Tested on:

    - macOS 26.5.2 (Tahoe), Darwin 25.5.0, Apple Silicon (arm64e) — primary target
    - Builds as a universal binary (arm64e + x86_64)

> **Note on Apple Silicon:** under Pointer Authentication (PAC) the kernel's
> private symbols cannot be linked from a kext, and the in-memory symbol table is
> jettisoned after boot. Affected features are recovered by one of three routes,
> in order of preference: a public KPI (or public sysctl), an in-kext
> forward-port against public interfaces (e.g. `fd/`, `threads/`), or the
> **`procfsd` userspace daemon**, which supplies data the kext cannot obtain
> in-kernel (full task/VM/argument/register info, per-CPU counters, IOKit) via
> ordinary userspace APIs. Anything still out of reach degrades gracefully
> (returns `ENOTSUP`/`EPERM`/empty) rather than failing the mount. See
> [Feature status](#feature-status) below.

### Root directory

At the root of the file system, alongside the per-process directories, are a few
Linux-compatible files and helpers:

| Entry        | Summary                                                              |
|--------------|---------------------------------------------------------------------|
|`allocinfo`   | Linux-style memory-allocation profiling; macOS has no code-tag profiling, so this is the zone allocator (`mach_zone_info`, via `procfsd`): one row per zone with live bytes, live count and the zone name in place of Linux's `file:line func:` tag |
|`apm`         | Linux-style advanced-power-management line (AC status, battery charge %, time remaining) mapped from IOKit power sources via the `procfsd` daemon |
|`bootconfig`  | Linux-style boot configuration; macOS has no bootconfig blob, so this is the boot loader's boot-args (`kern.bootargs`) with the Linux `# Parameters from bootloader:` note |
|`buddyinfo`   | Linux-style buddy-allocator free-block counts; macOS is not a buddy allocator, so the free page count is decomposed into buddy orders (maximally coalesced) — one `Node 0, zone Normal` line |
|`bus/`        | Bus-specific info directory; macOS provides PCI via IOKit — `bus/pci/devices` is the Linux PCI device table (bus/devfn, vendor:device, name, and BAR base addresses/sizes from `IOPCIDevice`; via the `procfsd` daemon) |
|`byname/`     | Directory of symbolic links, one per process, named by command name |
|`cmdline`     | Kernel boot command line (macOS boot-args / `kern.bootargs`; Linux's root `/proc/cmdline`) |
|`cpuinfo`     | Linux-style CPU information (text)                                   |
|`curproc/`     | Symbolic link to the calling process's directory (BSD name)         |
|`devices`     | Linux-style char/block device major-number listing (driver families reconstructed from `/dev` by the `procfsd` daemon) |
|`diskstats`   | Linux-style block-device I/O statistics (per whole disk, 14-field format; from IOKit `IOBlockStorageDriver`) |
|`dma`         | Linux-style list of ISA DMA channels in use; an x86-only concept — shows the reserved `cascade` channel on x86, empty on Apple Silicon (no 8237 ISA DMA) |
|`driver/`     | Directory grouping driver-specific files; currently `driver/rtc`, the same real-time-clock state as `/proc/rtc` |
|`execdomains` | Linux-style registered execution personalities; macOS has no exec-domain subsystem, so the single native personality is reported (`0-0  Darwin  [kernel]`) |
|`extensions`  | macOS-style list of loaded kernel extensions (kextstat-like: index, refs, address, size, name/version; via the `procfsd` daemon) |
|`fb`          | Linux-style framebuffer device list (`<index> <name>`); macOS IOKit framebuffers (`IOFramebuffer`/`IOMobileFramebuffer`) via the `procfsd` daemon |
|`filesystems` | Linux-style filesystem-type list (the mounted types, deduped; `nodev` for device-less) |
|`fs/`         | Filesystem-parameters directory; currently `fs/nfs/exports`, the NFS export table (macOS `/etc/exports`, read by the `procfsd` daemon) |
|`ide/`        | IDE (ATA/PATA) subsystem directory; macOS has no IDE subsystem (storage is NVMe/AHCI/USB, handled via IOKit — see `partitions`/`diskstats`), so `ide/drivers` is empty and there are no `ideN`/`hdX` subdirectories, as on a Linux host with no IDE hardware |
|`interrupts`  | Linux-style interrupt table (IRQ → controller → device, via the `procfsd` daemon); per-device counts are 0, but the `LOC`/`RES` summary lines carry real per-CPU timer/IPI counts from the softirq layer (`libkprocfs/cpu.c`, via the daemon) |
|`iomem`       | Linux-style physical memory map; `System RAM` + `Reserved` sized from `hw.memsize`/`hw.memsize_usable` (macOS publishes no full physical map, so the base is nominal) |
|`ioports`     | Linux-style I/O port map; an x86-only concept — the fixed legacy PC ports on x86, empty on Apple Silicon (ARM has no port-mapped I/O) |
|`irq/`        | IRQ-to-CPU affinity directory; `default_smp_affinity`/`_list` (all online CPUs). macOS routes IRQs via the AIC with no user-settable per-IRQ affinity, so per-IRQ subdirectories are omitted |
|`isapnp`      | ISA Plug-and-Play device listing; the ISA bus is obsolete and macOS has no ISA/ISA-PnP support on any platform, so this is empty, as on a modern Linux host with no ISA hardware |
|`kallsyms`    | The modern, fuller kernel symbol table. Everything `ksyms` has (the exported kernel symbols with their real static addresses, `kptr_restrict`-gated) **plus** the non-exported/local symbols — but macOS ships the arm64 kernel stripped of its locals, so the `procfsd` daemon recovers their **names** straight from the XNU source (`ctags` over the tree, baked into `tools/kallsyms_extra.h`) and emits them with address `0` (there is no address for them in the running image; regenerate with `tools/gen_kallsyms_extra.sh`). Output is Linux's `address type name [module]`; macOS exposes no loadable-module symbol addresses, so no line carries a `[module]` tag. Present alongside `ksyms` by default; a spoofed Linux version >2.5.47 exposes only this node (see *Spoofing the Linux kernel version*). Empty without a connected daemon |
|`kcore`       | On Linux, an ELF core image of live kernel memory for debuggers. macOS deliberately does not expose kernel memory to userspace (SIP/KASLR/PPL protect it; there is no KPI to enumerate or safely read kernel VM, and doing so would defeat the kernel's security model), so this is a well-formed but empty core — a valid ELF64 `ET_CORE` header for the running architecture with zero program headers (no `PT_LOAD` segments, no memory exposed). `file`/`readelf -h` still recognise it as an ELF 64-bit core file, as with a hardened/lockdown Linux where `kcore` is present but restricted |
|`kmsg`        | Linux-style kernel log buffer (the data behind `dmesg`). macOS keeps the same classic kernel printf buffer; its contents come from `proc_kmsgbuf()` — the root-only `libproc` call `dmesg(1)` uses — via the `procfsd` daemon (an unprivileged in-kernel read gets `EPERM`, hence the daemon). A snapshot, not Linux's blocking/consuming stream: repeated reads return the current buffer, like `dmesg`. Empty without a connected daemon |
|`ksyms`       | Linux-style kernel symbol table (`address type name` per line; modern Linux calls it `kallsyms`). macOS doesn't expose the running kernel's symbols at runtime (KASLR/SIP), so the `procfsd` daemon reads them from the running kernel's on-disk image — the arch/SoC-specific Mach-O under `/System/Library/Kernels` (e.g. `kernel.release.t8103` on an M1, selected from `kern.version`) — and returns `nm`-style lines with the static (unslid) link addresses. Mirroring Linux's `kptr_restrict`, root sees the real addresses and every other reader gets the address column zeroed. Present alongside `kallsyms` by default; a spoofed Linux version of exactly 2.5.47 exposes only this node (see *Spoofing the Linux kernel version*). Empty without a connected daemon |
|`last_kmsg`   | Linux/Android's kernel log from before the last reboot. macOS has no per-reboot RAM console, but it does persist one cross-boot kernel log — the kernel panic report — so the `procfsd` daemon returns the newest `panic-full-*.panic` from `/Library/Logs/DiagnosticReports`, which is exactly what `last_kmsg` is for (diagnosing the prior crash). Empty when the machine has no panic report, or without a connected daemon |
|`loadavg`     | Linux-style load averages (text; the kernel's true 1/5/15-minute values via the `procfsd` daemon's `getloadavg`, `0.00` without a connected daemon — see below) |
|`locks`       | Linux-style table of held byte-range (advisory) file locks. XNU keeps these per-vnode (`vp->v_lockf`) with no global registry, so `libkprocfs` enumerates every vnode directly with the public VFS iterators (`vfs_iterate` over mounts, `vnode_iterate` over each mount's vnodes) and emits each vnode's lock list, snapshotting under the vnode mutex — a fully in-kernel forward-port. macOS has no mandatory locking, so every lock is `ADVISORY`. Empty (no locks held) is the normal result |
|`meminfo`     | Linux-style memory summary (text; `MemFree` is the FreeBSD non-wired estimate on Apple Silicon — see below) |
|`misc`        | Linux-style registry of miscellaneous character devices (`<minor> <name>` per line). macOS has no misc-device framework, but it has plenty of miscellaneous single-purpose character devices (`autofs`, `bpf`, `dtrace`, `fsevents`, `klog`, `oslog`, `pf`, `auditpipe`, …); since XNU keeps no in-kernel named-device registry (device names live in devfs), the `procfsd` daemon enumerates them from `/dev` — one row per driver family, excluding the tty/disk/mem families that belong to other majors — exactly as `/proc/devices` and `/proc/tty/drivers` do. Empty without a connected daemon |
|`modules`     | Linux-style `/proc/modules` view of the same loaded kexts (`name size refcount deps state address`) |
|`mounts`      | The Linux name for the same mounted-filesystem table as `mtab`       |
|`mtab`        | Linux-style mounted-filesystem table (`/etc/mtab` format: `device mountpoint fstype options 0 0`) |
|`net/dev`     | Linux-style per-interface network statistics (in-kernel via the `ifnet` KPIs; fifo/frame/compressed/carrier columns are 0 — macOS keeps no such counters) |
|`net/tcp`, `net/tcp6` | Linux TCP connection table (IPv4 / IPv6). XNU has no in-kernel KPI to walk the `inpcb` tables, so — as `netstat(1)` does — the `procfsd` daemon reads them via the `net.inet.tcp.pcblist_n` sysctl and reformats each connection into the Linux `sl local_address rem_address st …` layout. Addresses print byte-reversed hex exactly as Linux does (the `in_addr` is already network-order); the `st` column maps macOS `TCPS_*` to Linux `TCP_*` state numbers. `uid` is the socket owner; the `inode` column is `0` (macOS exposes no socket inode — use `lsof` for fd↔socket correlation). Without a connected daemon the node shows just the header |
|`net/udp`, `net/udp6` | Linux UDP socket table (IPv4 / IPv6), same source (`net.inet.udp.pcblist_n`) and layout as `net/tcp`. UDP has no connection state, so a connected socket shows `01` (ESTABLISHED) and an unconnected one `07` (CLOSE), as on Linux |
|`net/unix`    | Linux unix-domain socket table (`Num RefCount Protocol Flags Type St Inode Path`). The `procfsd` daemon merges `net.local.stream.pcblist_n` and `net.local.dgram.pcblist_n` (the data behind `netstat`'s "Active LOCAL (UNIX) domain sockets"). macOS supplies a real fake-inode and bound path, so those columns are exact; `RefCount` is the socket use count, `Flags` carries `SO_ACCEPTCONN` as Linux's `SO_ACCEPTON` (`00010000`), `Type` is stream/dgram/seqpacket and `St` is `03` (connected) or `01` (unconnected/listening). Without a connected daemon the node shows just the header |
|`net/route`   | Linux IPv4 routing table (`Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT`). The `procfsd` daemon walks the `PF_ROUTE` `NET_RT_DUMP` sysctl (the data behind `netstat -rn`/`route`): destination/gateway/mask print in Linux's byte-reversed hex and flags map to the Linux-meaningful bits (`UP`/`GATEWAY`/`HOST`/`DYNAMIC`/`MODIFIED`). Cloned per-host cache routes and link-layer/ARP entries are excluded (Linux lists only configured routes; ARP goes to `net/arp`). `RefCnt`/`Use`/`Metric`/`MTU`/`Window`/`IRTT` have no macOS source (`0`). Without a connected daemon the node shows just the header |
|`net/arp`     | Linux IPv4 ARP table (`IP address / HW type / Flags / HW address / Mask / Device`). On macOS the ARP cache lives in the routing table as `RTF_LLINFO` link-layer routes; the `procfsd` daemon dumps them via `PF_ROUTE`/`NET_RT_FLAGS` (the data behind `arp -a`), reading the MAC from each entry's `AF_LINK` `sockaddr_dl` gateway. HW type is `0x1` (`ARPHRD_ETHER`), Flags `0x2` (`ATF_COM`) plus `0x4` (`ATF_PERM`) for a non-expiring entry, Mask `*`. Without a connected daemon the node shows just the header |
|`net/netstat` | Linux extended TCP/IP statistics (the `TcpExt:`/`IpExt:` two-line SNMP-style blocks read by `netstat -s`/`nstat`). The `procfsd` daemon reads the BSD counter structs behind `net.inet.{tcp,ip}.stats` (as root, bypassing the `net.inet.tcp.disable_access_to_stats` gate) and maps the ones with a genuine macOS analog — e.g. `DelayedACKs`, `ListenDrops`, `EmbryonicRsts`, `TCPTimeouts`, `PAWSEstab`, `InNoRoutes`, `InCsumErrors`. Linux-only extended counters with no macOS equivalent are `0` (as on a Linux host where those events haven't occurred); the standard MIB counters live in the richer `net/snmp`. Empty without a connected daemon |
|`net/snmp`    | Standard RFC-1213 SNMP MIB counters (`Ip:`/`Icmp:`/`Tcp:`/`Udp:` two-line blocks, as `netstat -s` and `snmpd` read). These map almost one-to-one onto macOS's BSD stat structs, so most fields carry real data: the `procfsd` daemon reads `net.inet.{ip,icmp,tcp,udp}.stats` (as root) — IP receive/deliver/forward/reassembly/fragment counts, the ICMP per-type In/Out histograms (`inhist[]`/`outhist[]`), TCP opens/segments/retransmits, UDP datagrams/errors. A few fields with no macOS counter (`CurrEstab`, `OutRsts`, TCP `RtoMin`/`RtoMax`) are `0`/`-1`. Empty without a connected daemon |
|`pagetypeinfo`| Additional buddy-allocator info: free pages and pageblocks by page-migrate type. macOS is not a buddy allocator and has no migrate types, so (like `buddyinfo`) the daemon's free-page count is decomposed into buddy orders and reported under the default `Movable` type (others 0); block counts derive from `hw.memsize`. One synthetic `Node 0, zone Normal` |
|`partitions`  | Linux-style partition table (text; all block devices via IOKit — see below) |
|`rtc`         | Linux-style real-time-clock state; `rtc_time`/`rtc_date` are the UTC calendar time (`clock_get_calendar_microtime`), alarm/IRQ fields report their inactive defaults |
|`scsi/`       | SCSI subsystem directory; `scsi/scsi` is the attached-device list — macOS SCSI-protocol peripherals (USB/external/Thunderbolt storage through the SCSI Architecture Model, `IOSCSIPeripheralDeviceType*` via the `procfsd` daemon) in the Linux `Host:`/`Vendor:`/`Type:` layout. Internal NVMe is not SCSI and is excluded, so a Mac with no external SCSI storage shows just `Attached devices:` |
|`self/`        | Symbolic link to the calling process's directory (Linux name)       |
|`slabinfo`    | Linux-style slab-cache statistics. macOS has no slab allocator, but its zone allocator (`zalloc`) is the direct analog — each zone is a fixed-size-element cache. The `procfsd` daemon enumerates zones via `mach_zone_info` (the data behind `zprint`) and maps them onto the slabinfo columns (active/total objects, object size, objects and pages per allocation chunk, slab counts); the SLUB `tunables` have no zone equivalent and are 0. Empty without a connected daemon or without root (`mach_zone_info` needs the host privilege port) |
|`softirqs`    | Linux-style per-CPU softirq counts; XNU has no softirqs, but `libkprocfs/cpu.c` maps the daemon's per-CPU timer/IPI counters (`host_processor_info`) onto the vectors, so `TIMER`/`HRTIMER`/`SCHED` carry real counts (others 0) |
|`stat`        | Linux-style kernel/system statistics (`cpu`/`cpuN` ticks, `btime`, `processes`; see below) |
|`swaps`       | Linux-style swap-area table (aggregate `vm.swapusage`; macOS swaps dynamically under `/private/var/vm`) |
|`sys/`        | Dynamic mirror of the kernel sysctl MIB tree (Linux `/proc/sys`); directories are sysctl nodes, leaves read their value as text |
|`sysvipc/`    | System V IPC object tables (`msg`/`sem`/`shm`). macOS lacks Linux's `SHM_STAT`/`MSG_STAT`/`SEM_STAT` enumeration, so the `procfsd` daemon lists the live objects via the `kern.sysv.ipcs.*` sysctl that `ipcs(1)` uses, formatted in the Linux layout (key/id/perms/… per object). Falls back to the header line (an empty table) without a connected daemon |
|`tty/`        | TTY info directory: `tty/drivers` (the tty driver table, derived from `/dev` by the `procfsd` daemon) and `tty/ldiscs` (line disciplines) |
|`uptime`      | Linux-style uptime (seconds since boot; idle field `0.00`)          |
|`version`     | Kernel version string (text)                                        |
|`video/`      | Legacy `bttv` (Bt848/878 capture-card) subsystem directory; macOS has no bttv/Video4Linux subsystem (video capture is CoreMediaIO/AVFoundation), so `video/bttv/` is an empty directory with no per-card entries, as on a Linux host with the bttv module present but no capture hardware |
|`vmallocinfo` | Kernel virtually-allocated areas; macOS has no `vmalloc`, so this shows XNU's non-zone kernel VM allocations by tagged site (`mach_memory_info`, the data behind `zprint -v`, via the `procfsd` daemon) — real sizes and site names, sorted by size, with `0x0` address ranges since macOS does not expose per-site kernel virtual addresses (complements `allocinfo`, which covers the zone allocator) |
|`vmstat`      | Linux-style virtual-memory statistics (daemon-backed `host_statistics64`; see below) |

### Per-process files

Each directory named for a process id represents one process on the system. By default you can only see your own processes, although it is possible to set an option (`noprocperms`) when mounting the file system that will let you see and get details for every process. Obviously this is a security risk, so it’s not the default mode of operation. Within each process directory are the following files and two further directories. Most files contain binary structures rather than text, so they are intended to be used in applications rather than for direct human consumption. You’ll find definitions of the structures in this table in the file */usr/include/sys/proc_info.h*.

| File    | Summary                          | Structure                       |
|---------|----------------------------------|---------------------------------|
|`auxv`     | XNU's auxiliary-vector equivalent — dyld's `apple[]` array (`key=value` strings) | text (NUL-separated) |
|`clear_refs`| Linux working-set knob: write `1`–`4` to reset the page referenced/soft-dirty bits `smaps` reports. macOS exposes no way (to userspace or a third-party kext) to clear another task's pmap reference bits, so the write is validated and accepted for tool compatibility but has no effect; `smaps` `Referenced` is synthesized regardless | write-only (read returns `EINVAL`; mode `0200`) |
|`cmdline`  | Process argument vector (NUL-separated, Linux format) | text |
|`comm`     | Process (command) name | text |
|`cpu`      | Linux 2.4 per-CPU task accounting: a `cpu` total line (user/system time in `USER_HZ` ticks) then one `cpuN` line per online CPU. XNU accounts task time as a whole without a per-CPU split, so the total is carried on the `cpu` line and reported on `cpu0` (0 elsewhere), keeping the per-CPU times summing to the total | text |
|`cwd/`      | Symlink to the current working directory | symlink |
|`environ`  | Process environment (NUL-separated, Linux format) | text |
|`exe`      | Symlink to the executable | symlink |
|`fd/`       | File descriptor| directory |
|`fpregs`   | Representative thread's FP/SIMD registers, native `arm_neon_state64_t` (x86_64: `x86_float_state64_t`) — served by the daemon | binary |
|`limit`    | Process resource limits, one `<name> <cur> <max>` line per limit (`-1` = unlimited) | text |
|`map`      | Process virtual-memory regions, NetBSD `procfs` format (address range, cur/max prot, sharing, wired) | text |
|`maps`     | Process virtual-memory regions, Linux `/proc/<pid>/maps` format | text |
|`mem`      | Process memory; the read offset is the virtual address (NetBSD/Linux `mem` semantics). Resident pages only — see below | binary |
|`note`     | Write a note to the process (NetBSD/Plan 9-style) | write-only (read returns `EINVAL`); a note delivers a signal to the process — see below |
|`numa_maps`| Linux `/proc/<pid>/numa_maps`: per-mapping NUMA locality (single-node: policy `default`, `N0=`) | text |
|`pagemap`  | Linux `/proc/<pid>/pagemap`: one 64-bit entry per virtual page (indexed by `vaddr/PAGE_SIZE`) — bit 63 present, 62 swapped, 61 file-backed, 55 dirty, from `mach_vm_page_query` via the `procfsd` daemon. The physical frame number (bits 0–54) is left `0` (macOS doesn't expose it, as Linux does for a reader without `CAP_SYS_ADMIN`). Read by seeking to a page's offset (as on Linux); `task_for_pid`-denied (SIP/AMFI/hardened) targets or no daemon → `EPERM`/empty | binary (8 bytes/page) |
|`pid`      | Process id                       | `pid_t` (binary `int32`)         |
|`pgid`     | Process group id                 | `pid_t` (binary `int32`)         |
|`ppid`     | Parent process id                | `pid_t` (binary `int32`)         |
|`regs`     | Representative thread's general registers, native Mach `arm_thread_state64_t` (x86_64: `x86_thread_state64_t`) — served by the `procfsd` daemon | binary |
|`root/`     | Symlink to the root directory | symlink |
|`sid`      | Session id                       | `pid_t` (binary `int32`)         |
|`io`       | Linux per-process I/O accounting; `read_bytes`/`write_bytes` are real disk I/O (via the `procfsd` daemon's `proc_pid_rusage`), the `rchar`/`wchar`/`syscr`/`syscw`/`cancelled_write_bytes` fields are 0 (untracked on macOS) | text |
|`smaps`    | Linux `/proc/<pid>/smaps`: per-region memory detail (`Rss`/`Pss`/dirty/`Swap`/`VmFlags`) | text |
|`smaps_rollup` | Linux `/proc/<pid>/smaps_rollup`: the `smaps` fields summed across all mappings (one `[rollup]` block) | text |
|`stack`    | Linux `/proc/<pid>/stack`: the task's kernel-side call stack, one frame per line as `[<0>] symbol+0xoff/0xsize`. On Linux this is a full frame-pointer unwind of the thread's kernel stack; macOS threads almost always block via a **continuation** (`mach_msg`, semaphore/event waits, sleep), which by design discards the kernel stack and resumes at a single continuation function — so there is no preserved call chain and the continuation *is* the complete kernel state. That one frame is emitted, resolved exactly as `wchan` (same runtime slide + `procfsd`/KDK-dSYM symbolication); the address is hidden as `[<0>]` (as Linux does under `kptr_restrict`) and the offset/size are `0` (a continuation is a function entry). A thread not blocked on a continuation (on-CPU, or blocked in place keeping its stack) yields empty output, as on Linux for a running task | text |
|`stat`     | Linux single-line process stat (52 space-separated fields) | text |
|`statm`    | Linux memory usage in pages (`size resident shared text lib data dt`) | text |
|`status`   | Basic process info (mode-switched) | native: `struct proc_bsdshortinfo`; linux: `Name:/State:/Pid:/Uid:/VmRSS:…` text |
|`task/`       | Linux-style task directory | directory |
|`taskinfo` | Info for the process’s Mach task | `struct proc_taskinfo` — exact via the `procfsd` daemon; falls back to the kext’s partial fill without it (see Feature status) |
|`threads/`   | Thread directory (BSD name), one subdirectory per thread id | directory |
|`tty`      | Controlling terminal device path (e.g. `/dev/ttys001`) | text |
|`wchan`    | The kernel symbol the task is blocked in (Linux `CONFIG_KALLSYMS` wchan), or `0` if not blocked. XNU has no KPI for this, so it is recovered directly: `libkprocfs` reads the representative blocked thread's `continuation` (the function it resumes at, i.e. the wchan), PAC-stripped — the offset of `continuation` in the opaque `struct thread` is discovered once at **runtime** (a helper thread is blocked with a known continuation and its field located), so no fragile hard-coded offsets. The running kernel's slide is measured from a reference symbol (`proc_pid`), and `continuation − slide` is named by the `procfsd` daemon. The daemon symbolizes against the matching **Kernel Debug Kit dSYM** when installed (full symbols, so internal continuations like `_ipc_mqueue_receive_continue`, `_semaphore_wait_continue` resolve); without a matching KDK it falls back to the stripped kernel image (exported symbols only, so most continuations yield `0`). No matching daemon/KDK, no continuation, or an unresolvable address → `0` | text (no trailing newline) |

The `fd` directory contains one entry for each file that the process has open. Each entry is a directory that’s numbered for the corresponding file descriptor. Within each subdirectory you’ll find two files called `details` and `socket`. The `details` file contains a `vnode_fdinfowithpath` structure, which contains information about the file including its path name if it is a file system file. If the file is a socket endpoint, you can read a `socket_fdinfo` structure from the `socket` file.

The `threads` directory contains a subdirectory for each of the process’ threads, named by thread id (TID). Each thread directory contains a file called `info` that holds a `proc_threadinfo` structure. Thread *enumeration* works on Apple Silicon (the directory lists the real thread ids), and the per-thread `info` *contents* are now supplied exactly by the `procfsd` daemon (`proc_pidinfo(PROC_PIDTHREADID64INFO)`); they read zero only when no daemon is connected (the private `fill_taskthreadinfo` is stripped from the arm64 kernel).

The `task` directory mirrors the `threads` directory except that its TID sub-directories also contain Linux-like files. 

| File    | Summary                          | Structure                       |
|---------|----------------------------------|---------------------------------|
|`comm`   | Exposes and allows modification of a specific thread's command name (comm value) | text |
|`info`   | The `proc_threadinfo` structure| `pid_t` (binary `int32`)   |
|`maps`   | Displays the memory layout and mapped memory regions for a specific thread within a process | text |
|`sched`   | Provides real-time scheduler statistics and execution data for a specific thread | text |
|`stat`   | Provides detailed status and performance statistics for a specific thread | text |
|`status`   | Provides human-readable status information for a specific thread | text |

## Feature status

Verified with `tests/test_features.sh`.

**Working (real data):**

  - Directory listing of the root and per-process directories via `ls`, `find`, `readdir(3)` and `getdirentries64(2)`
  - `version` — kernel version string
  - `cpuinfo` — Linux-style CPU information. On Apple Silicon the `Features` line
    comes from `hw.optional.arm.FEAT_*` (real capabilities), `CPU variant`/`CPU
    revision` are read from `MIDR_EL1`, and each core reports the correct `CPU
    part` for its cluster — efficiency cores (Icestorm `0x023`) vs performance
    cores (Firestorm `0x022`), from the device-tree cluster type via the daemon.
    With Linux compatibility on, `Features` are ordered like a real
    `/proc/cpuinfo` (AArch64 HWCAP order)
  - `loadavg` — process count plus the true 1/5/15-minute load averages from the
    `procfsd` daemon (`getloadavg`); the load values read `0.00` when no daemon is
    connected (see Apple Silicon note)
  - `meminfo` — Linux-style memory summary; `MemTotal` and `MemFree` are
    populated (`MemFree` via the FreeBSD non-wired estimate — see Apple Silicon note)
  - `partitions` — Linux-style table of block devices via IOKit (`IOMedia`):
    every whole disk and partition, mounted or not, with real major/minor, 1 K
    block counts and BSD names (falls back to the mounted-only list without
    IOKit — see note below)
  - `mtab` — Linux `/etc/mtab`-style table of every mounted filesystem
    (`device mountpoint fstype options 0 0`), via `vfs_iterate` + `vfs_statfs`
  - `stat` — Linux `/proc/stat`: per-CPU `cpu`/`cpuN` user/nice/system/idle
    ticks (from `processor_info`, the loadavg per-CPU source), `btime`
    (`kern.boottime`) and `processes`; interrupt/ctxt/fork counters read 0
  - `vmstat` — Linux `/proc/vmstat`: VM page counters from the `procfsd` daemon's
    `host_statistics64(HOST_VM_INFO64)`, mapped onto Linux keys (`nr_free_pages`,
    `pgpgin`/`pgpgout`, `pgfault`, …); zero without the daemon
  - `curproc` symlink and the `byname/` directory of per-process symlinks
  - Per-process `pid`, `ppid`, `pgid`, `sid` (binary `int32`)
  - Per-process `status` — `proc_bsdshortinfo` (pid/ppid/pgid, status, command
    name, real/effective/saved uids and gids, process flags)
  - `cmdline` — the process's argument vector, NUL-separated (the Linux
    `/proc/<pid>/cmdline` format); zombies and system processes report `(comm)`
  - `environ` — the process's environment, NUL-separated (Linux
    `/proc/<pid>/environ`), read from the same argument region as `cmdline`
  - `comm`, `stat`, `statm` — the process's Linux `/proc/<pid>/comm` name,
    single-line 52-field `stat`, and page-count `statm`
  - `status` — native binary `proc_bsdshortinfo`, or Linux `Name:/State:/Pid:/
    Uid:/VmRSS:…` text when the `procfs.linux` sysctl is set (see below)
  - `exe`, `cwd`, `root` — per-process symlinks to the executable, current
    directory and root directory (`vn_getpath` on `p_textvp` / `fd_cdir` /
    `fd_rdir`)
  - `self` (root) — symlink to the caller's own process directory (Linux name
    for `curproc`); `uptime`, `mounts` (root) — Linux `/proc/uptime` and the
    Linux name for `mtab`
  - `swaps`, `filesystems` (root) — Linux `/proc/swaps` (aggregate
    `vm.swapusage`) and `/proc/filesystems` (mounted types, deduped, with the
    `nodev` prefix)
  - `sys/` (root) — a live mirror of the kernel's sysctl MIB tree (Linux
    `/proc/sys`): every sysctl node is a directory and every leaf a text file
    holding its formatted value (`kern/ostype`, `kern/hostname`, `hw/ncpu`, …).
    Values come from the `procfsd` daemon for full coverage, with an in-kernel
    fallback that reaches the `CTLFLAG_KERN` subset (see below)
  - `fd/` — enumerates the process's open file descriptors; per-fd `details`
    (`vnode_fdinfowithpath`) and `socket` (`socket_fdinfo`, common fields plus
    UNIX/IPv4 addresses)
  - `threads/` — enumerates the process's threads (one directory per thread id)
  - `task/` — the Linux name for the same per-thread view (`/proc/<pid>/task/<tid>`),
    one directory per thread id, with Linux-format per-thread files: `comm`,
    `stat`, `status`, `sched`, `maps` (and the binary `info`). Per-thread data
    (name, run state, user/system CPU time, priority, policy) comes from the
    `procfsd` daemon via `proc_pidinfo(PROC_PIDTHREADID64INFO)`; fields with no
    macOS source (Linux fault counters, CFS scheduler internals, register
    addresses, ctxt-switch counts) are reported as 0
  - `tty` — the process's controlling terminal device path (e.g. `/dev/ttys001`),
    empty when it has none
  - `limit` — the process's resource limits (FreeBSD `procfs_rlimit` format: one
    `<name> <cur> <max>` line per limit, `-1` for unlimited)
  - `mem` — the process's memory; the read offset is the virtual address (the
    NetBSD/Linux `mem` semantics). Only resident pages are returned (see Apple
    Silicon note)
  - `map` / `maps` — the process's virtual-memory regions (`map` in NetBSD
    procfs format, `maps` in Linux `/proc/<pid>/maps` format), with address
    ranges and protections (see Apple Silicon note)
  - `smaps` — Linux `/proc/<pid>/smaps`: each `maps` region followed by
    per-region memory detail (`Size`, `Rss`, `Pss`, shared/private clean/dirty,
    `Anonymous`, `Swap`, `VmFlags`) from `VM_REGION_EXTENDED_INFO`; `Pss` and
    `Referenced` approximate `Rss`, and the region's share mode classifies its
    whole `Rss` as shared or private (see Apple Silicon note)
  - `smaps_rollup` — the same `smaps` fields summed across every mapping into a
    single `[rollup]` block (Linux's faster-than-parsing-`smaps` accumulation)
  - `numa_maps` — one line per mapping with its NUMA locality; Apple Silicon is
    single-node, so the policy is always `default` and resident pages report as
    `N0=` (with `anon=`/`dirty=` from `VM_REGION_EXTENDED_INFO`)
  - `regs` / `fpregs` — the representative thread's general and FP/SIMD register
    state as the native Mach `arm_thread_state64_t` / `arm_neon_state64_t`
    (x86_64: `x86_thread_state64_t` / `x86_float_state64_t`), supplied by the
    `procfsd` daemon; `EPERM` for Apple platform/hardened binaries, `ENOTSUP`
    without a daemon (see below)
  - `auxv` — XNU's auxiliary-vector equivalent: dyld's `apple[]` array
    (`executable_path=`, `stack_guard=`, `dyld_file=`, `malloc_entropy=`,
    `arm64e_abi=`, …), from the daemon's `KERN_PROCARGS2` region

`fd/` and `threads/` required forward-porting work to function under PAC on Apple
Silicon rather than relying on the unavailable private KPIs: `fd/` walks the
process's file-descriptor table directly, and `threads/` enumerates threads via
the BSD `proc->p_uthlist` instead of the inaccessible Mach `task->threads` queue.

Everything the kext cannot reach in-kernel is delegated to the **`procfsd`
daemon** (see [The `procfsd` daemon](#the-procfsd-daemon)), which obtains it
through ordinary userspace APIs. The nodes below are its main consumers.

`cmdline`, `environ` and `auxv` come from the argument region the daemon reads
with the `KERN_PROCARGS2` sysctl (the in-kernel `vm_map_copyin` path is
`com.apple.kpi.private`). The kext requests the flattened region and splits it
into the `argv` / `envp` / `apple[]` sections. Because `KERN_PROCARGS2` is a root
sysctl rather than `task_for_pid`, these three keep working even for
SIP/AMFI-protected and hardened processes. Without a daemon, `cmdline` falls back
to the parenthesised command name and `environ`/`auxv` are empty.

`tty` returns the controlling terminal. Its in-kernel accessor `proc_gettty`
reaches the terminal through the SMR-protected `p->p_pgrp` and is
`com.apple.kpi.private`, so the daemon reads the device from
`proc_pidinfo(PROC_PIDTBSDINFO)`'s `e_tdev` and maps it to its `/dev` path
(`devname`) instead. Empty when the process has no controlling terminal;
`ENOTSUP` without a daemon.

`loadavg`'s load values come from the daemon's `getloadavg()` — the kernel's true
1/5/15-minute averages. They are not reachable from the kext itself
(`averunnable`, `compute_averunnable`, `host_statistics` and `processor_set_info`
are all stripped and unexported), so without a connected daemon the node reads
`0.00`.

`meminfo` reports `MemTotal` from the `hw.memsize` sysctl and `MemFree` using
FreeBSD's `linprocfs_domeminfo` estimate (`MemFree = MemTotal − wired`). The
wired-page count comes from the daemon's `host_statistics64(HOST_VM_INFO64)`,
because the `vm.*` page-count sysctls are not readable from kernel context and
most `vm_page_*_count` globals are stripped on arm64. `Cached`, `Buffers` and
swap have no kernel-reachable source there and read 0 (`MemFree` reads 0 without
a daemon).

`mem` reads the target's memory through the daemon's
`task_for_pid()` + `mach_vm_read_overwrite()`, page by page. Only resident memory
is returned: reading an unmapped or paged-out address (including offset 0, the
NULL page) returns `EIO`, and a read stops cleanly at the first hole. Because it
needs `task_for_pid`, `mem` returns `EPERM` for SIP/AMFI-protected and hardened
processes; `ENOTSUP` without a daemon. Access is also gated by the same
credential check as the rest of the filesystem.

`map` and `maps` enumerate the process's VM regions through the daemon's
`task_for_pid()` + `mach_vm_region()` walk (macOS exports no region-enumeration
KPI a kext may link, and the internal walkers are stripped on arm64). The daemon
returns the raw region records and the kext formats them; both nodes share one
walk and differ only in formatting (`map` NetBSD-style, `maps` Linux-style). Like
`mem`, they return `EPERM` for protected/hardened processes. Backing-file paths
(the trailing Linux column) are not resolved — the region's object→vnode is not
reachable here — so the device/inode/path columns read `00:00 0`.

`regs` and `fpregs` expose a thread's register state. `thread_get_state()` is
neither a bindable KPI symbol nor present in the arm64 kernelcache symbol table,
so the kext cannot read register state itself. The `procfsd` daemon does it from
userspace instead —
`task_for_pid()` + `task_threads()` + `thread_get_state()` on the process's
representative thread — and returns the native Mach state struct over the
kernel-control bridge. Because `task_for_pid` is denied to root for Apple
platform/hardened binaries under SIP/AMFI, those processes return `EPERM` — the
same permission model as `ptrace` on Linux. On arm64e the saved `pc`/`lr` carry
their PAC bits and are emitted raw, leaving stripping to the consumer.

`auxv` reports XNU's equivalent of the ELF auxiliary vector. macOS binaries are
Mach-O, so there is no `AT_*` array on the stack; the closest analog is dyld's
`apple[]` array — the `key=value` strings the kernel places after `argv`/`envp`,
which the node emits from the tail of the `KERN_PROCARGS2` region described
above. Zombies and system processes report empty. In Linux presentation mode it
synthesises the familiar `AT_*` key/value lines instead.

`partitions` enumerates block devices through IOKit, matching the `IOMedia`
class — every whole disk and partition, mounted or not, exactly as Linux's
`/proc/partitions` does. That data lives in the IORegistry, reachable only via
the C++ IOKit runtime (the C IOKit KPI exposes no registry matching), so this is
the kext's one C++ translation unit (`procfs_iokit.cpp`): it matches `IOMedia`
by name and reads each object's `BSD Name`/`BSD Major`/`BSD Minor`/`Size`
properties off the base `IORegistryEntry`, needing only the base IOKit and
libkern KPIs (no dependency on `IOStorageFamily`'s `IOMedia` class). Each row is
`major minor #blocks name` with the 1 K block count from the media size. If
IOKit matching fails, the node falls back to the mounted-filesystem list
(`vfs_iterate()` + `vfs_statfs()`), which shows mounted volumes only.

`diskstats` shares that same C++ IOKit translation unit for the Linux
`/proc/diskstats` node. It enumerates the whole-disk `IOMedia` entries
(`Whole` = true) and reads the cumulative I/O counters from each disk's
`IOBlockStorageDriver` provider (the `Statistics` dictionary: `Operations`,
`Bytes` and `Total Time` for reads and writes) off the base `IORegistryEntry`.
Each line is the classic 14-field Linux format (`major minor name reads
rd_merged rd_sectors rd_ticks writes wr_merged wr_sectors wr_ticks in_flight
io_ticks time_in_queue`); bytes become 512-byte sectors and nanosecond service
times become milliseconds. macOS keeps no per-device merge, in-flight or queue
accounting, so those fields read 0 and `io_ticks` is approximated as the sum of
read and write service times.

`sys/` is a dynamic mirror of the kernel's sysctl MIB — the macOS counterpart
to Linux's `/proc/sys`. Rather than static structure nodes, every `/proc/sys`
vnode is one shared marker node distinguished by the kernel address of its
`struct sysctl_oid` (carried in the node id; the tree root is `sysctl__children`);
a `CTLTYPE_NODE` oid is a directory and any other oid is a leaf. Directory
listing and lookup walk the live `oid_link` sibling lists, so the tree always
reflects the kernel's current MIB (internal `__anchor__(…)` and `CTLFLAG_MASKED`
oids are hidden). A leaf read builds the dotted MIB name and formats the value by
the oid's declared type (`int`/`quad`/`string`; opaque/struct oids read empty).
The value itself is fetched from the `procfsd` daemon (userspace `sysctlbyname`,
which serves *every* oid) and falls back to in-kernel `sysctlbyname()` when no
daemon is connected. The kernel path only serves oids marked `CTLFLAG_KERN`, so
without the daemon the non-`KERN` leaves (e.g. `kern.hostname`, `kern.maxproc`)
read empty; invoking the oid handler in-kernel to bypass that gate is unsafe —
the custom handlers assume the sysctl lock is held and panic — so the daemon is
the path to full coverage. Because every sysctl vnode shares one structure node,
`..` traversal is resolved specially: from a nested `/proc/sys` directory it walks
back to the enclosing sysctl oid (and from `/proc/sys` itself to the `/proc`
root), so relative paths through the tree behave normally. As a Linux-compat
convenience, `/proc/sys/kernel` is a lookup alias for the macOS `kern` MIB node
(Linux groups these sysctls under `kernel`; macOS calls the node `kern`), so
`/proc/sys/kernel/ostype` and friends resolve to the same `kern.*` oids.

`extensions` lists the loaded kernel extensions, macOS's answer to the module
list, in a `kextstat`-style format (load index, reference count, load address,
size, wired size, bundle id and version). A kext cannot enumerate loaded kexts
itself — that needs the private C++ `OSKext` class, which is neither a bindable
KPI nor safe to call — so the listing is produced by the `procfsd` daemon via
`KextManagerCopyLoadedKextInfo()` and delivered over the control bridge. The
listing (200+ kexts, tens of KB) far exceeds one bridge payload, so it is
streamed in `PROCFS_CTL_MAXPAYLOAD`-sized chunks (`procfs_ctl_request_blob()`):
the daemon rebuilds the listing when a read starts at offset 0 and returns the
slice for each subsequent offset, and the kext reassembles them into an `sbuf`.
Without a connected daemon the node is empty. `modules` is the same listing in
Linux `/proc/modules` format (`name size refcount deps state address`, with
`deps` always `-` and state always `Live`, since macOS exposes no per-kext
dependency string here); it shares the entire chunked-transfer path and differs
only in the daemon-side formatting.

`allocinfo` is the Linux memory-allocation profiling node (`/proc/allocinfo`).
Linux keys it on code tags (`file:line func:name`); macOS has no code-tag
allocation profiling, so the closest faithful source is the **zone allocator** —
the same `mach_zone_info()` data `zprint` reports. Because `mach_zone_info`
requires the privileged host port, the `procfsd` daemon gathers it and emits one
row per zone — `<live bytes> <live count> zone:<name>`, where live bytes is the
zone's element count times its element size and the zone name stands in for
Linux's code tag — sorted by size descending, streamed over the same chunked
transfer as `extensions`/`modules`/`devices`. Empty without a connected daemon.

`apm` is the Linux advanced-power-management line (`/proc/apm`). macOS has no APM,
so the power state comes from IOKit power sources (the data `pmset -g batt`
reports): the `procfsd` daemon reads the AC/battery state, charge percentage,
charging flag and time remaining, and the kext maps them onto the classic APM
format — `driver_ver bios_ver bios_flags ac_status battery_status battery_flag
percentage% time units` — using the Linux apm-emulation status/flag byte
encoding (e.g. `0x01`/`0x00` AC on/off; battery status high/low/critical/charging;
time in minutes). A machine with no battery reports the no-battery flag `0x80`;
the node is empty without a connected daemon.

`bootconfig` is Linux's boot-configuration node (`/proc/bootconfig`). Linux fills
it from a bootconfig blob appended to the initrd, followed by a `# Parameters
from bootloader:` note carrying the original bootloader command line. macOS has
no such blob — its only boot configuration is the boot-args the boot loader
(iBoot / NVRAM) passes to the kernel (`kern.bootargs`, the same source as
`/proc/cmdline`). So the boot-args are emitted as the boot config and, because on
macOS the kernel parameters come from the boot loader, repeated in the
`# Parameters from bootloader:` form. Read in-kernel with the same `procfsd`
sysctl-bridge fallback as `/proc/cmdline`; empty when no boot-args are set.

`buddyinfo` is Linux's buddy-allocator free-block report (`/proc/buddyinfo`):
one line per node and zone giving the number of free blocks of each order
(`2^order` pages), orders 0 through `MAX_ORDER-1`. macOS is not a buddy allocator
and exposes no per-order free lists, so there is no real fragmentation data.
Instead the free page count (`host_statistics64`'s `free_count`, from the daemon
via the same request as `vmstat`) is decomposed greedily into buddy orders,
largest first — a valid buddyinfo whose blocks account for exactly the free pages
(`Σ count[order] × 2^order = free_pages`), i.e. free memory presented as maximally
coalesced. Apple Silicon is single-node UMA, so a single `Node 0, zone Normal`
line; all-zero without a connected daemon.

`bus/` is Linux's directory of bus-specific information. The classic content is
`bus/pci/devices`, the PCI device table. macOS exposes PCI through the IORegistry
(`IOPCIDevice`) rather than a `/proc` table, so the `procfsd` daemon enumerates
those devices and formats the Linux line for each: `bus/devfn`, `vendor/device`
id (from the little-endian `vendor-id`/`device-id` properties), the device name
(from `IOName`), and the base addresses and region sizes — BAR0–5 plus the
expansion ROM, with the PCI region flags (I/O, 64-bit, prefetchable) in the low
bits — decoded from IOKit's `assigned-addresses` property, in the standard
18-field layout. IRQ reports 0: macOS routes PCI interrupts via MSI/GIC with no
legacy per-device IRQ line to report. `/proc/bus` and `/proc/bus/pci` are plain
directories; the `devices` listing is empty without a connected daemon.

`dma` lists the ISA DMA channels in use (one `%2d: <owner>` line per busy channel
of the legacy 8237 controller). This is an x86-only concept: on x86 the DMA
subsystem always reserves channel 4 as the cascade wiring the two 8237
controllers, so `/proc/dma` there is never empty. macOS/XNU uses no 8237 ISA DMA,
and Apple Silicon has no such hardware at all, so the node shows only the
reserved `cascade` channel on x86 and is empty on arm64 — matching Linux's own
per-architecture behaviour.

`ioports` is Linux's map of allocated I/O port regions (`<start>-<end> : name`).
Port-mapped I/O is an x86 concept; ARM (Apple Silicon) has no I/O port space at
all — it is memory-mapped only — so the node is empty on arm64, as on ARM Linux.
On x86 the legacy ISA controllers (PIC, PIT, 8237 DMA, keyboard, RTC, FPU) sit at
architecturally-fixed ports on all PC-compatible hardware, so those standard
regions are reported there. Fully in-kernel.

`iomem` is Linux's physical address-space map (`<start>-<end> : name`), dominated
by System RAM. macOS publishes no complete physical map to a kext — the
device-tree memory node redacts the DRAM base and device regions need multi-level
`ranges` translation — so the one solid fact is the RAM size. The node reports
`System RAM` (the OS-usable amount, `hw.memsize_usable`) and `Reserved` (the
firmware carve-out, `hw.memsize` − usable). The base is a nominal `0` (macOS does
not expose the true physical base), so this is a size-accurate representation of
RAM rather than a literal address map. Fully in-kernel.

`rtc` reports the real-time-clock state (Linux `drivers/rtc/proc.c`). The core is
the current RTC time and date; macOS keeps its hardware RTC in UTC, exposed via
`clock_get_calendar_microtime()`, so `rtc_time`/`rtc_date` are the UTC calendar
time (epoch seconds converted to a civil date in-kernel, since the kernel has no
`gmtime`). macOS does not expose the RTC's alarm or periodic-interrupt state to a
kext, so the `alrm_*`/`*IRQ*` fields report their inactive defaults (no alarm,
IRQs off, 24-hour mode, `batt_status: okay`). Fully in-kernel. The same content
is also served at `/proc/driver/rtc`, where Linux groups driver-specific files.

`execdomains` lists the kernel's registered execution personalities (the legacy
`personality(2)` / `exec_domain` mechanism for running foreign-OS binaries).
Modern Linux keeps only the native personality and emits a single fixed line,
`0-0\tLinux\t[kernel]`. macOS has no exec-domain subsystem; its native
personality is Darwin/Mach-O, so — as `/proc/version` reports Darwin rather than
Linux — the sole domain is reported with the native name: `0-0  Darwin  [kernel]`,
becoming `0-0  Linux  [kernel]` when a Linux kernel version is spoofed (see
*Spoofing the Linux kernel version*). Fully in-kernel.

`fb` lists the registered framebuffer devices, one `<index> <name>` line each.
macOS drives displays through IOKit framebuffers — `IOFramebuffer` on Intel,
`IOMobileFramebuffer` on Apple Silicon — so the `procfsd` daemon enumerates both
classes and formats a line per device, using the device's IORegistry name as the
Linux `fix.id` (e.g. `0 AppleCLCD2`). Streamed over the same chunked transfer as
`/proc/bus/pci/devices`; empty without a connected daemon.

`interrupts` is the Linux per-CPU interrupt table (`/proc/interrupts`), one line
per IRQ with the controller and owning device. macOS does not expose per-CPU
interrupt counts to userspace or a kext (only the private IOReporting framework
has them), so the count columns are 0. The IRQ *topology*, though, is real: the
`procfsd` daemon walks the IORegistry for each device's `IOInterruptSpecifiers`
(the IRQ numbers) and `IOInterruptControllers`, producing genuine
IRQ → controller → device rows (e.g. `cpu0`, `spi2`, `i2c1`, `pci-bridge0`),
sorted by IRQ. The per-device lines carry no counts (macOS has no per-IRQ-line
per-CPU data), but the kext appends the architecture summary lines `LOC` (local
timer interrupts) and `RES` (rescheduling IPIs) with real per-CPU counts from the
softirq layer (`libkprocfs/cpu.c`, `PROCFS_REQ_CPUSTAT` → the daemon's
`host_processor_info(PROCESSOR_CPU_STAT)`). The per-device topology is streamed
over the same chunked transfer as `/proc/bus/pci/devices`; both the topology and
the summary counts require a connected daemon.

`softirqs` is Linux's per-CPU softirq-count table, one line per softirq type
(`HI`, `TIMER`, `NET_TX`, `NET_RX`, `BLOCK`, `IRQ_POLL`, `TASKLET`, `SCHED`,
`HRTIMER`, `RCU`). Softirqs are a Linux-specific bottom-half mechanism; XNU has
no softirq layer, but every processor keeps per-CPU interrupt-event counters —
hardware IRQs, IPIs and timer interrupts — that `host_processor_info`'s
`PROCESSOR_CPU_STAT` flavor exposes. That flavor reads zero in-kernel for a kext,
so `libkprocfs/cpu.c` fetches the counters from the `procfsd` daemon (userspace
`host_processor_info`, `PROCFS_REQ_CPUSTAT`) and maps them onto the vectors
(`procfs_cpu_softirq_map()`): `TIMER` and `HRTIMER` from the timer interrupt,
`SCHED` from reschedule IPIs. So those three carry real per-CPU numbers when the
daemon is connected; the vectors with no XNU counter (network, block, tasklet,
RCU, …) stay 0, and the whole table is 0 without a daemon.

`tty/` is Linux's TTY-information directory. `tty/drivers` is the tty driver
table; Linux lists registered `tty_driver` structs, but macOS has no such
registry, so the `procfsd` daemon derives it from the tty devices in `/dev`,
grouped by major — one `<name> /dev/<name> <major> <minor-range> <type>` line per
major, with the type inferred from the device family (`console`, `system` for
`/dev/tty`, `serial` for the `cu.*` callout devices, `pty:master`/`pty:slave` for
the BSD and unix98 pty majors). `tty/ldiscs` lists the line disciplines; macOS's
tty layer uses the standard terminal discipline (`TTYDISC`, number 0) and exposes
no enumerable table to a kext, so it reports that one canonical entry. The
`drivers` listing is empty without a connected daemon.

`irq/` is Linux's IRQ-to-CPU affinity tree. On Linux each `/proc/irq/<N>/` holds
an `smp_affinity` bitmask naming the CPUs that may service IRQ `<N>`, plus a
`default_smp_affinity` for new IRQs. macOS routes interrupts through the AIC with
no user-settable or per-IRQ-queryable CPU affinity — every IRQ may run on any
CPU — so only the default masks are exposed and the per-IRQ subdirectories are
omitted. The directory holds `default_smp_affinity` (hex cpumask of all online
CPUs, e.g. `ff` for 8 CPUs, comma-grouped in 32-bit words for more) and
`default_smp_affinity_list` (the CPU range, e.g. `0-7`). Fully in-kernel.

`fs/` is Linux's directory of filesystem parameters; the classic entry is
`fs/nfs/exports`, the NFS export table. Linux shows the kernel NFS server's
active exports there; macOS keeps the export configuration in `/etc/exports`
(which `nfsd` registers with the kernel), so the `procfsd` daemon serves that
file's contents. `/proc/fs` and `/proc/fs/nfs` are plain directories; the
`exports` listing is empty when no NFS server is configured (no `/etc/exports`)
or when no daemon is connected.

### The `procfsd` daemon

Several fields are unreachable from a kext on Apple Silicon: the kernel
functions that produce them (`fill_taskprocinfo`, `task_info`, per-thread info,
the VM-statistics globals) are stripped from the kernel symbol table, and the
data lives in per-CPU/`recount` structures with no linkable accessor. The
`procfsd` userspace daemon supplies these via `libproc`'s `proc_pidinfo()` and
`host_statistics64()` — the same interfaces `top`/Activity Monitor use — over a
privileged `PF_SYSTEM` kernel control (`procfs_ctl.c`): a node read sends a
request and the daemon replies. So `taskinfo` (all 18 fields exact),
`task/<tid>/{info,comm,stat,status,sched}`, `vmstat`, the `sys/` sysctl values
and the `extensions`/`modules` kext listings are fully populated when the daemon
runs, and fall back to the kext's best-effort values (or zero, the `CTLFLAG_KERN`
sysctl subset, or an empty node) when it does not. `taskinfo`'s
`pti_resident_size` is then the exact `phys_footprint` from the daemon (the
kext's own estimate is only the fallback). Without the daemon the per-thread
`info` reads zero (`fill_taskthreadinfo` is stripped from the arm64 kernel).

The daemon's role expanded over time until it became the sole route for
everything the kext cannot reach in-kernel: besides the fields above it serves
`maps`/`map`/`smaps` (region walk), `mem` (memory read), `cmdline`/`environ`/`auxv`
(`KERN_PROCARGS2`), `tty`, `loadavg`, `stat`/`softirqs`/`interrupts` (per-CPU
counters), `partitions`/`diskstats`/`apm`/`bus/` (IOKit), and the `regs`/`fpregs`
register nodes — all described above. This is what made it possible to remove the
kernel-symbol-resolution machinery the project used to rely on (see
[Feature status](#feature-status)). When armed, the daemon also loads the kext
and keeps `/proc` mounted; see *Installing*.

The register nodes are a good example of the `task_for_pid` coverage limit:
`thread_get_state()` is unreachable from the kext, so `regs`/`fpregs` require a
connected daemon (`task_for_pid` + `thread_get_state`) and return `ENOTSUP`
without one, or `EPERM` for a `task_for_pid`-denied (SIP/AMFI/hardened) target —
as do `mem` and the `maps` family, which need the same task port.

`note` is a writable per-process node (NetBSD `procfs_donote()` lineage). macOS
has no native "note" primitive, so — following the Plan 9 origin of procfs notes,
where a note is the signal mechanism — writing a note delivers a signal to the
target: a recognised name (`hup`, `int`/`interrupt`, `quit`, `kill`, `term`,
`stop`, `cont`, `usr1`, `usr2`) or a numeric signal is posted via `proc_signal()`,
and anything else returns `EINVAL`. Reads return `EINVAL` (the node is
write-only), as on NetBSD. Permission is the filesystem's own model: the node's
write mode is owner/group, so only a user who owns
the target process — or root — can open it for writing. Writing is via the newly
added `vnop_write`; a companion `vnop_setattr` accepts the `O_TRUNC` that shells
issue on `>` so `echo kill > /proc/<pid>/note` works. Because a read-only mount
makes the VFS reject every write before it reaches the filesystem, the mount is
no longer `MNT_RDONLY` (as with Linux `/proc`); non-writable nodes still reject
writes themselves.

**Presentation mode (native vs Linux):** the `procfs.linux` sysctl selects how
nodes that have both renderings present themselves — `0` (default) = native
BSD/XNU, `1` = Linux-compatible. It is a live global toggle:

    sudo sysctl -w procfs.linux=1     # Linux-compatible
    sudo sysctl -w procfs.linux=0     # native (default)

Currently `status`, `regs`, `fpregs` and `auxv` honour it: in native mode
`status` emits the binary `proc_bsdshortinfo` and the register/auxv nodes emit
the binary Mach state / raw `apple[]` array; in Linux mode they emit the
human-readable text forms (`Name:/State:/…`, `x0 0x…`, `q0 0x…`, `AT_PAGESZ …`)
from `procfs_linux.c`. Other nodes keep their single format for now.

The sysctl lives in the kext, so it resets to `0` every time the kext loads.
To make the choice **persist across reboots**, the setting is saved to
`/var/db/procfs.linux` (just `0` or `1`) and `procfsd` re-applies it via
`sysctlbyname` each time the kext (re)appears — at boot and after any reload.
Toggling *Linux compatibility* in the menu-bar app writes that file for you (it
already runs the change with administrator rights); to persist a change made on
the command line, write the file yourself, e.g. `echo 1 | sudo tee
/var/db/procfs.linux`. Removing the file reverts to the native-by-default
behaviour.

**Spoofing the Linux kernel version:** a companion sysctl, `procfs.linux_version`,
makes procfs report a Linux *identity* instead of Darwin. `0` = None (native
Darwin, the default); `1..N` select a preset Linux release. When set, `/proc/version`
becomes a Linux banner, `/proc/execdomains` reads `0-0  Linux  [kernel]`, and the
`/proc/sys/kern/{ostype,osrelease,version}` mirror reports `Linux` / the release
/ the banner:

    $ sudo sysctl -w procfs.linux_version=2
    $ cat /proc/version
    Linux version 6.6.0 (builder@linux-build-env) (gcc version 10.3.0 (Ubuntu 10.3.0-1ubuntu1)) #1 SMP PREEMPT_DYNAMIC Sat Dec 14 12:00:00 UTC 2024
    $ cat /proc/sys/kernel/ostype
    Linux

The `/proc/sys` tree mirrors macOS's real sysctl MIB, whose top node is `kern`,
but Linux groups these under `kernel`, so `/proc/sys/kernel` is provided as a
lookup alias for `/proc/sys/kern` — Linux paths like `/proc/sys/kernel/ostype`
resolve to the same `kern.*` oids (the alias is always available, independent of
the spoof). `ls /proc/sys` still lists the real `kern` node.

The presets (`6.12.0`, `6.6.0`, `6.1.0`, `5.15.0`, `5.10.0`, `2.5.47`) live in
`procfs_linux_versions[]` in the kext and must stay in sync with the menu-bar app's
version menu. Like the presentation mode, the choice persists across reboots via
`/var/db/procfs.linux_version` (the preset index), restored by `procfsd`. This is
independent of `procfs.linux` (which only changes binary-vs-text rendering); the
menu-bar app just presents the version dropdown underneath *Linux compatibility*.

The spoofed release also picks which kernel-symbol node is exposed, matching the
Linux history where `/proc/ksyms` was renamed to `/proc/kallsyms` after 2.5.47:
with **no spoof** (native Darwin) **both** `ksyms` and `kallsyms` are present;
spoofing **`2.5.47`** exposes only `/proc/ksyms`; spoofing any **newer** release
(all the other presets) exposes only `/proc/kallsyms`. The hidden node is absent
from `readdir` and resolves as `ENOENT`.

## Menu-bar app

`ProcFS.app` is a lightweight menu-bar (status-bar) app for controlling procfs
without the command line. Its menu shows live status — whether `/proc` is
mounted, whether the `procfsd` daemon is running, and whether Linux presentation
mode is on — and offers one-click toggles for each (mount/unmount, Linux
compatibility on/off, start/stop daemon), plus the current version. When Linux
compatibility is on, a *Spoof Linux Kernel Version* submenu appears with a few
preset Linux releases and a *None (Darwin)* entry at the bottom (the default).
The mutating actions need root, so they go through the standard macOS
administrator-authorization prompt; status is read unprivileged. A *Preferences…*
item at the top opens the preference pane (below). It is built as part of `make`
(into `out/ProcFS.app`, version stamped from `VERSION`) and installed to
`/Applications` by `sudo make install`.

## Preference pane

`ProcFS.prefPane` is a System Settings preference pane (`NSPreferencePane`),
opened from the menu-bar app's *Preferences…* item (or directly from System
Settings). It shows the ProcFS icon, name and description, followed by a
**Settings** section:

  - **Run on System Startup** — launches the menu-bar app at login (a per-user
    `LaunchAgent`, `~/Library/LaunchAgents/com.beako.procfs.gui.plist`)
  - **Daemon** — a *Start*/*Stop* button (reads *Stop* while `procfsd` is running)
  - **Linux Compatibility Mode** — the `procfs.linux` toggle
  - **Spoof Linux Kernel Version** — the `procfs.linux_version` preset dropdown
    (enabled while Linux compatibility is on)
  - **Check for Updates on Startup** — when on, the menu-bar app checks GitHub
    Releases at launch and prompts if a newer version exists
  - **Check for Updates** — a button that checks GitHub Releases now

and a footer with the repository link and copyright. Update checking queries
`https://api.github.com/repos/somestupidgirl/mSL-ProcFS/releases/latest`,
compares the tag with the running version, and (if newer) offers to open the
releases page. The privileged actions use the same administrator-auth prompt as
the menu-bar app. The pane's principal binary is a loadable Mach-O bundle
(`swiftc -emit-library -Xlinker -bundle`); it is built by `make` and installed
to `/Library/PreferencePanes` by `sudo make install` (removed by
`sudo make uninstall`). The *Check for Updates on Startup* setting is shared with
the menu-bar app through the `com.beako.filesystems.procfs` preferences domain.

## How to build procfs
`make` builds the kext, the `procfs.fs` mount bundle, the `procfsd` daemon, the
LaunchDaemon plist, the `ProcFS.app` menu-bar app, the preference pane and the
`Uninstall-ProcFS.app` uninstaller into `bin/`:

    make                    # native arch (arm64e on Apple Silicon)
    make ARCH=universal     # fat arm64e + x86_64

To build and install in one step, use the install script — it runs
`make clean && make && sudo make install` and prompts for your password:

    ./install.sh                # native arch
    ./install.sh ARCH=universal

`sudo make install` **only copies** the prebuilt artifacts into place (it never
compiles, so `bin/` and the build tree stay owned by you and `make clean` never
needs sudo). It installs, with `root:wheel`/`755`: the kext to
`/Library/Extensions`, the `procfs.fs` bundle to `/Library/Filesystems`,
`procfsd` to `/usr/local/sbin`, the LaunchDaemon plist to
`/Library/LaunchDaemons`, and `ProcFS.app` and `Uninstall-ProcFS.app` to
`/Applications/mSL`. It also adds `proc` to `/etc/synthetic.conf` (so `/proc` is
created at boot), enables the LaunchDaemon, and launches `ProcFS.app` so its
menu-bar icon appears.

To remove ProcFS, either open **Uninstall mSL/ProcFS** from `/Applications/mSL`
(it confirms, asks for an administrator password, and deletes itself along with
everything else) or run `sudo make uninstall` from the source tree. Both unmount
`/proc`, unload the kext, and remove the installed files and daemon state; the
removal steps live in `installer/uninstall.sh`, which the uninstaller app carries
as a bundled resource.

Code signing is optional; the kext is ad-hoc signed by default. To sign with
your own certificate instead, edit `Makefile.inc` and set the `SIGNCERT`
variable to the identity in your keychain.

Auto-load of the kext and auto-mount of `/proc` stay **disarmed** until you
create the arm flag, so a kext panic during development cannot boot-loop the
machine:

    sudo touch /var/db/procfs.enabled
    sudo reboot

After the reboot, `procfsd` loads the kext and mounts `/proc` for all users.

### Loading the kext (Apple Silicon, macOS 26)

Third-party kexts are loaded from the Auxiliary Kernel Collection (AuxKC). After
the first install you must approve the extension in **System Settings → Privacy
& Security** and reboot.

When **re-installing a rebuilt kext**, the build's code identity (cdhash)
changes, and a stale staging record will cause `kernelmanagerd` to reject it
with *"tried to insert an invalid codeful kernel extension in the restricted
lookup table."* `./install.sh` handles this: `make install`'s preinstall step
unmounts procfs, unloads the old kext and clears the staging cache before
copying the new build. To load into the running kernel without a reboot:

    sudo kmutil load -p /Library/Extensions/procfs.kext
    kextstat | grep procfs

## Mounting
With the arm flag set (see *How to build procfs*), `procfsd` mounts `/proc`
automatically at boot. `/proc` itself is created by `/etc/synthetic.conf`.

To mount manually (the kext must be loaded):

    sudo mount -t procfs procfs /proc

## Exploring the file system
Once mounted you can execute the `ls` command to list the contents:

    ls -l /proc

Or recursively:

    ls /proc/*/*/*/*

Likewise you can use the `cat` command to get the contents of a file:

    cat /proc/version
    cat /proc/sys/kern/ostype        # like Linux's /proc/sys/kern/ostype
    ls  /proc/sys/kern
    cat /proc/extensions             # kextstat-style loaded-kext listing

For per-process files that contain binary structures rather than text, you must pipe them
through `hexdump` to read the raw contents:

    cat /proc/curproc/taskinfo | hexdump -C
    cat /proc/self/regs | hexdump -C
    cat /proc/self/auxv | tr '\0' '\n'

## TODO:
 - Extend the `procfs.linux` presentation-mode switch to more nodes as native
   and Linux renderings diverge (only `regs`/`fpregs`/`auxv` differ today).
 - Implement more linux-compatible features (see the roadmap).

## Issues
Currently known issues:

- On Apple Silicon, `cmdline`, `fd/`, `threads/` and `tty` need private kernel state unavailable to a kext under PAC. `fd/` and `threads/` are forward-ported against public interfaces; `cmdline` and `tty` are served by the `procfsd` daemon (via `KERN_PROCARGS2` and `proc_pidinfo(PROC_PIDTBSDINFO)` respectively). Without a connected daemon, `cmdline` falls back to the parenthesised command name and `tty` to `ENOTSUP`.
- `taskinfo` and per-thread `info` are populated by the `procfsd` daemon (`proc_pidinfo`); they read the kext fallback / zero only when no daemon is connected, since the private `fill_taskprocinfo` / `fill_taskthreadinfo` are stripped from the arm64 kernel.
- `note` is writable: a note is delivered to the process as a signal (Plan 9 semantics — recognised name or numeric signal via `proc_signal()`, else `EINVAL`); reads return `EINVAL`. Gated by the node's owner/group write mode.
- `regs`/`fpregs`, `mem` and the `maps` family require the `procfsd` daemon (they need a task port the kext cannot get: `thread_get_state`/`mach_vm_region`/`mach_vm_read` are unreachable in-kernel) and return `EPERM` for Apple platform/hardened binaries, whose task ports `task_for_pid` denies even to root under SIP/AMFI — analogous to `ptrace` permissions on Linux. `cmdline`/`environ`/`auxv` avoid this because their daemon path is a root sysctl (`KERN_PROCARGS2`), not `task_for_pid`.
- `partitions` now enumerates all block devices (whole disks and partitions, mounted or not) via IOKit's `IOMedia` class (`kext/procfs_iokit.cpp`, the kext's one C++ file). It falls back to the mounted-filesystem list (`vfs_iterate`) only if IOKit matching fails.
- The x86 `/proc/cpuinfo` `bugs` and `power management` fields are now populated (`kext/lib/cpu.c`): `bugs` from `IA32_ARCH_CAPABILITIES` plus CPU vendor/family (the common speculative-execution/errata classes; Linux's full per-model whitelist tables are not reproduced), and `power management` from CPUID `0x80000007`. The x86 flag getters (`get_cpu_flags` etc.) were also rewritten to use proper static buffers, fixing the earlier dangling-stack-pointer bug where flags did not "stick."
- AMD CPUs now emit their extended feature flags: on AMD the `flags` line is built from CPUID `0x80000001` EDX/ECX (`get_amd_feature_flags`/`get_amd_feature2_flags` - `svm`, `sse4a`, `3dnowprefetch`, `xop`, `fma4`, `tbm`, `topoext`, `perfctr_core`, `mwaitx`, `nx`, `lm`, ...) with Linux-compatible names, replacing the generic Intel-named extended getter for that vendor. AMD leaf-7 extended features are also read now (Zen, family >= 23). The x86 `/proc/cpuinfo` fills its `i386_cpu_info` from an in-kext `cpuid`-instruction forward-port (`procfs_cpuid_info` in `cpu.c`) plus `machdep.cpu.*`/`machdep.tsc.frequency` sysctls, since the kernel's own `cpuid_info()` is not exported to kexts; `set_microcode_version()` reads that struct rather than the earlier null stub.

## Contributing and Bug Reporting
If you wish to contribute to this project then feel free to make a pull request. If you encounter any undocumented bugs then you may also file an issue under the "Issues" tab.

## Credits
This project builds on the work of others, with thanks to:

- **Kim Topley** — the original proof-of-concept `procfs` for XNU that this project started from.
  <https://github.com/kimtopley/ProcFS>
- **leiless** — the `libkext` kernel-extension helper library.
  <https://github.com/leiless/libkext>
- **Syncretic** — the `klookup` kernel symbol-resolution code from the `latebloom` project (0BSD), which earlier versions used to reach private kernel symbols on Apple Silicon; since removed in favour of the `procfsd` daemon, but gratefully acknowledged.
  <https://github.com/reenigneorcim/latebloom>
- **Linus Henze** - for filesystem code pulled from his `Unrootless-Kext`
  <https://github.com/LinusHenze/Unrootless-Kext>
- **Acidanthera** - the `MacKernelSDK` macOS kernel SDK targeting various XNU versions.
  <https://github.com/acidanthera/MacKernelSDK>
- **Apple** — the XNU kernel source, headers and reference implementation.
  <https://github.com/apple-oss-distributions/xnu>
- **Apple** — the `libutil source and headers, needed by the mount_procfs tool.
  <https://github.com/apple-oss-distributions/libutil>
- **The NetBSD project** — `procfs` design and reference implementation.
  <https://github.com/NetBSD/src>
- **The NextBSD project** — additional `procfs` reference.
  <https://github.com/NextBSD/NextBSD/tree/NextBSD-CURRENT>
- **The FreeBSD project** — `procfs`/`pseudofs` reference implementation.
  <https://github.com/freebsd/freebsd-src>
