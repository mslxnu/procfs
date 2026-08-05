/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_linux.c
 *
 * Linux-compatible features.
 *
 * ARM64 support added 2024. On ARM64, procfs_docpuinfo() emits
 * output in Linux AArch64 /proc/cpuinfo format rather than the
 * x86 format, using hw.optional.arm.* sysctls and hw.cpufamily
 * for CPU identification.
 */
#include <ptrauth.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
#include <i386/cpuid.h>
#include <i386/tsc.h>
#elif defined(__arm64__) || defined(__aarch64__)
#include <arm/cpuid.h>
#endif
#if defined (__x86_64__)
#include <i386/proc_reg.h>
#elif defined(__arm64__) || defined(__aarch64__)
#include <arm/proc_reg.h>
#endif

#include <kern/clock.h>
#include <kern/thread_call.h>

#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>

#include <mach/machine.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/thread_status.h>
#include <mach/vm_prot.h>
#include <mach/vm_statistics.h>
#if defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif
#if defined(__x86_64__)
#include <mach/i386/vm_param.h>
#else
#include <mach/vm_param.h>
#endif

#include <net/kpi_interface.h>

#include <os/log.h>

#include <sys/disklabel.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <cpuft.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_iokit.h>
#include <fs/procfs/procfs_ctl.h>

#pragma mark -
#pragma mark Common Definitions and Macros

/*
 * A buffer size.
 */
#define LBFSZ (8 * 1024)

/*
 * Convert pages to bytes.
 */
#define PGTOB(p) \
        ((unsigned long)(p) << PAGE_SHIFT)
/*
 * Convert pages to kbytes
 */
#define PGTOKB(p) \
        ((unsigned long)(p) << (PAGE_SHIFT - 10))
/*
 * Convert bytes to pages.
 */
#define BTOPG(b) \
        ((b) >> PAGE_SHIFT)
/*
 * Convert bytes to kbytes
 */
#define BTOKB(b) \
        ((b) >> 10)
/*
 * XXX
 */
#define TVTOJ(x) \
        ((x)->tv_sec * 100UL + (x)->tv_usec / 10000)

/*
 * Various conversion macros
 */
#define T2J(x)  ((long)(((x) * 100ULL) / (stathz ? stathz : hz)))           /* ticks to jiffies */
#define T2CS(x) ((unsigned long)(((x) * 100ULL) / (stathz ? stathz : hz)))  /* ticks to centiseconds */
#define T2S(x)  ((x) / (stathz ? stathz : hz))                              /* ticks to seconds */
#define B2K(x)  ((x) >> 10)                                                 /* bytes to kbytes */
#define B2P(x)  ((x) >> PAGE_SHIFT)                                         /* bytes to pages */
#define P2B(x)  ((x) << PAGE_SHIFT)                                         /* pages to bytes */
#define P2K(x)  ((x) << (PAGE_SHIFT - 10))                                  /* pages to kbytes */
#define TV2J(x) ((x)->tv_sec * 100UL + (x)->tv_usec / 10000)

/**
 * @brief Mapping of ki_stat in struct kinfo_proc to the linux state
 *
 * The linux procfs state field displays one of the characters RSDZTW to
 * denote running, sleeping in an interruptible wait, waiting in an
 * uninterruptible disk sleep, a zombie process, process is being traced
 * or stopped, or process is paging respectively.
 *
 * Our struct kinfo_proc contains the variable ki_stat which contains a
 * value out of SIDL, SRUN, SSLEEP, SSTOP, SZOMB, SWAIT and SLOCK.
 *
 * This character array is used with ki_stati-1 as an index and tries to
 * map our states to suitable linux states.
 */
static char linux_state[] = "RRSTZDD";

#pragma mark -
#pragma mark Linux-emulation functions

/*
 * Linux-compatible /proc/cpuinfo
 */
int
procfs_docpuinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    int len = 0, xlen = 0;
    vm_offset_t pageno, uva;
    off_t page_offset = 0;
    size_t buffer_size = 0;
    char *buffer;

    /*
     * Overall processor count for the current CPU.
     *
     * Not to be conflated with cpu_cores (number of cores)
     * as these are not the same.
     */
    uint32_t cnt_cpus = 0;

    /* Overall logical-processor count. The kernel's processor_count global is
     * not linkable, so read hw.logicalcpu. */
    uint32_t max_cpus = 1;
    size_t   max_cpus_size = sizeof(max_cpus);
    sysctlbyname("hw.logicalcpu", &max_cpus, &max_cpus_size, NULL, 0);

    /*
     * Initialize the processor counter.
     * This should always begin at 0 and
     * add 1 for each loop according to the
     * number of processors present.
     */
    buffer_size = (LBFSZ * 4);
    buffer = malloc(buffer_size, M_TEMP, M_WAITOK);

    /*
     * Get the userspace virtual address.
     */
    uva = uio_offset(uio);

    /*
     * Get the page number of this segment.
     */
    pageno = trunc_page(uva);
    page_offset = uva - pageno;

/*
 * ============================================================
 * x86_64 /proc/cpuinfo
 * ============================================================
 */
#if defined(__x86_64__)

    /*
     * TSC frequency from the public machdep.tsc.frequency sysctl (the kernel's
     * tscFreq global is not linkable).
     */
    uint64_t freq = 0;
    size_t   freq_sz = sizeof(freq);

    sysctlbyname("machdep.tsc.frequency",
        &freq,
        &freq_sz,
        NULL,
        0
    );

    /* 
     * Set the TSC frequency variables
     */
    int fqmhz = 0,
        fqkhz = 0;
    if (freq != 0) {
        fqmhz = (freq + 4999) / 1000000;
        fqkhz = ((freq + 4999) / 10000) % 100;
    }

    /*
     * The apicid variable begins at 0 and get increased
     * by 2 for each loop until the number becomes greater
     * than max_cpus, in which case the loop resets the
     * variable to a value of 1 and then contines increasing
     * that number by 2 for each loop.
     */
    int apicid = 0,
        initial_apicid = 0;

    /*
     * The core id should always start at 0.
     */
    int core_id = 0;

    /*
     * Here we can utilize the i386_cpu_info structure in i386/cpuid.h
     * to get the information we need. The cpuid_info() function sets up
     * the i386_cpu_info structure and returns a pointer to the structure.
     */
    char *vendor_id             = cpuid_info()->cpuid_vendor;
    uint8_t cpu_family          = cpuid_info()->cpuid_family;
    uint8_t model               = cpuid_info()->cpuid_model
                                + (cpuid_info()->cpuid_extmodel << 4);
    char *model_name            = cpuid_info()->cpuid_brand_string;
    int microcode               = (int)get_microcode_version();
    uint32_t cache_size         = cpuid_info()->cpuid_cache_size;
    uint8_t stepping            = cpuid_info()->cpuid_stepping;
    uint32_t cpu_cores          = cpuid_info()->core_count;
    uint32_t cpuid_level        = cpu_cores;
    uint32_t tlb_size           = cpuid_info()->cache_linesize * 40;
    uint32_t clflush_size       = cpuid_info()->cache_linesize;
    uint32_t cache_alignment    = clflush_size;
    uint32_t addr_bits_phys     = cpuid_info()->cpuid_address_bits_physical;
    uint32_t addr_bits_virt     = cpuid_info()->cpuid_address_bits_virtual;

    /*
     * Check if the FPU feature is present.
     */
    char *fpu, *fpu_exception;
    if (cpuid_info()->cpuid_features & CPUID_FEATURE_FPU) {
        fpu = "yes";
        fpu_exception = "yes";
    } else {
        fpu = "no";
        fpu_exception = "no";
    }

    /*
     * Check for CPU write protection.
     */
    char *wp;
    if (get_cr0() & CR0_WP) {
        wp = "yes";
    } else {
        wp = "no";
    }

    /*
     * CPU flags.
     *
     * Bug note: not all of the flag variables stick
     * after the first iteration of the main loop if we,
     * define them here, likely due to a memory-related
     * issue that has been pointed out to me, though I'm
     * still unsure on how to go about fixing it.
     * For now, we'll declare the pointer variables here
     * and fetch the flags inside the loop.
     */
    char    *cpuflags,
            *cpuextflags,
            *leaf7flags,
            *leaf7extflags,
            *amdflags2;

    /*
     * Power-management line and CPU bug classes (CPUID/MSR-derived, cpu.c).
     * All bugs are reported through x86_bugs; x86_64_bugs is kept as a second
     * (currently empty) slot so the "bugs" format string stays unchanged.
     */
    char *pm            = get_pm_flags();
    char *x86_bugs      = get_cpu_bugs();
    char *x86_64_bugs   = "";

    /*
     * The main loop iterates over each processor number stored in the
     * max_cpus variable and prints out a list of data for each processor.
     *
     * For example:
     *
     * If your CPU only has one processor, it should print only a single list.
     * If your CPU has two processors, it will print out a set of two lists,
     * and so on and so forth. My CPU processor number is 32 so I get 32 lists
     * with processor numbers starting from 0 - 31.
     */
    while (cnt_cpus < max_cpus) {
        /*
         * Fetch the CPU flags.
         */
        cpuflags        = get_cpu_flags(procfs_linux_mode);

        /*
         * On AMD, the 0x80000001 flags come from the AMD getters (correct Linux
         * names, EDX + ECX) instead of the generic ext getter, avoiding both the
         * Intel-style names (xd/em64t/...) and duplicate entries.
         */
        cpuextflags     = is_amd_cpu() ? get_amd_feature_flags() : get_cpu_ext_flags();
        amdflags2       = get_amd_feature2_flags();   /* empty on Intel */
        leaf7flags      = get_leaf7_flags();
        leaf7extflags   = get_leaf7_ext_flags();

        if (cnt_cpus <= max_cpus) {
            /* 
             * The data which to copy over to userspace.
             */
            len += snprintf(buffer, buffer_size,
                "processor\t\t: %u\n"
                "vendor_id\t\t: %s\n"
                "cpu family\t\t: %u\n"
                "model\t\t\t: %u\n"
                "model name\t\t: %s\n"
                "microcode\t\t: 0x%07x\n"
                "stepping\t\t: %u\n"
                "cpu MHz\t\t\t: %d.%02d\n"
                "cache size\t\t: %d KB\n"
                "physical id\t\t: %u\n"
                "siblings\t\t: %u\n"
                "core id\t\t\t: %d\n"
                "cpu cores\t\t: %u\n"
                "apicid\t\t\t: %u\n"
                "initial apicid\t\t: %u\n"
                "fpu\t\t\t: %s\n"
                "fpu_exception\t\t: %s\n"
                "cpuid level\t\t: %u\n"
                "wp\t\t\t: %s\n"
                "flags\t\t\t: %s%s%s%s%s\n"
                "bugs\t\t\t: %s%s\n"
                "bogomips\t\t: %d.%02d\n"
                "TLB size\t\t: %u 4K pages\n"
                "clflush_size\t\t: %u\n"
                "cache_alignment\t\t: %d\n"
                "address sizes\t\t: %d bits physical, %d bits virtual\n"
                "power management\t: %s\n\n",
                cnt_cpus,
                vendor_id,
                cpu_family,
                model,
                model_name,
                microcode,
                stepping,
                fqmhz, fqkhz,
                cache_size,
                0,
                max_cpus,
                core_id,
                cpu_cores,
                apicid,
                initial_apicid,
                fpu,
                fpu_exception,
                cpuid_level,
                wp,
                cpuflags, cpuextflags, amdflags2, leaf7flags, leaf7extflags,
                x86_bugs, x86_64_bugs,
                fqmhz * 2, fqkhz,
                tlb_size,
                clflush_size,
                cache_alignment,
                addr_bits_phys,
                addr_bits_virt,
                pm
            );

            /*
             * How many bytes to copy.
             */
            xlen = (len - page_offset);

            /*
             * Do the move into userspace.
             */
            uiomove((const char *)buffer, xlen, uio);

            /* 
             * Set len back to 0 before entering into the next loop.
             */
            if (len != 0) {
                len = 0;
            }

            /*
             * Increase by 2 for each loop.
             */
            apicid += 2;
            if ((int)apicid >= (int)max_cpus) {
                /* If the number exceeds max_cpus, reset to 1. */
                apicid = 1;
            }

            /*
             * The initial apicid is the same as apicid.
             */
            initial_apicid = apicid;

            /*
             * Add 1 to the CPU counter for each iteration.
             */
            cnt_cpus++;

            /*
             * Add 1 to the core_id counter for each iteration.
             */
            core_id++;

            /*
             * Reset the core_id counter if it exceeds the maximum number of cores.
             */
            if ((int)core_id > (int)cpu_cores - 1) {
                core_id = 0;
            }
        } else if (cnt_cpus > max_cpus) {
            break;
        }
    }
/*
 * ============================================================
 * ARM64 /proc/cpuinfo
 * ============================================================
 */
#elif defined(__arm64__) || defined(__aarch64__)

    const char *bogomips     = arm64_bogomips();
    const char *features     = get_cpu_flags(procfs_linux_mode);
    const char *implementer  = arm64_cpu_implementer();
    const char *architecture = arm64_cpu_architecture();
    const char *variant      = arm64_cpu_variant();
    const char *revision     = arm64_cpu_revision();

    /*
     * Per-logical-CPU cluster type ('E'/'P') from the device tree (via the
     * daemon), so each core reports the correct part number - efficiency cores
     * (e.g. Icestorm 0x023) versus performance cores (Firestorm 0x022). Without
     * a daemon every entry is '?', and we fall back to the performance-core part.
     */
    char clusters[64];
    procfs_cpu_clusters(clusters, (max_cpus <= 64) ? max_cpus : 64);

    while (cnt_cpus < max_cpus) {
        if (cnt_cpus <= max_cpus) {
            const char *part = (cnt_cpus < 64 && clusters[cnt_cpus] == 'E')
                             ? arm64_cpu_part_ecore() : arm64_cpu_part();
            len += snprintf(buffer, buffer_size,
                "processor\t\t: %u\n"
                "BogoMIPS\t\t: %s\n"
                "Features\t\t: %s\n"
                "CPU implementer\t\t: %s\n"
                "CPU architecture\t: %s\n"
                "CPU variant\t\t: %s\n"
                "CPU part\t\t: %s\n"
                "CPU revision\t\t: %s\n\n",
                cnt_cpus,
                bogomips,
                features,
                implementer,
                architecture,
                variant,
                part,
                revision
            );

            xlen = (len - page_offset);
            uiomove((const char *)buffer, xlen, uio);

            if (len != 0) len = 0;

            cnt_cpus++;

            /*
             * Reset the max_cpus variable at the end of each loop.
             * An unknown bug is causing the variable to reduce the
             * count down to 23 unless we do this.
             */
        } else if (cnt_cpus > max_cpus) {
            /*
             * If the counter exceeds the processor count,
             * break the loop.
             */
            break;
        }
    }

#endif /* __x86_64__ / __arm64__ */

    free(buffer, M_TEMP);
    return error;
}

/*
 * Linux-compatible /proc/loadavg
 */
int
procfs_doloadavg(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;
    int len = 0, xlen = 0;

    vm_offset_t off = uio_offset(uio);
    vm_offset_t pgno = trunc_page(off);
    off_t pgoff = (off - pgno);

    char *buf = malloc(LBFSZ, M_TEMP, M_WAITOK);

    /*
     * The load averages come from the procfsd daemon, which returns the kernel's
     * true 1/5/15-minute averages (getloadavg), scaled x100. A run-queue load
     * average is otherwise unreachable on arm64 (averunnable and every run-queue
     * source are stripped). Without a connected daemon the values read 0.00.
     */
    int load1 = 0,
        load5 = 0,
        load15 = 0,
        running = 1,
        lastpid = 0;

    int total_procs = procfs_get_process_count(vfs_context_ucred(ctx));

    uint32_t la[3] = { 0, 0, 0 };
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_LOADAVG, 0, 0, &la,
        sizeof(la), &got) == 0 && got == sizeof(la)) {

        load1  = (int)la[0];
        load5  = (int)la[1];
        load15 = (int)la[2];
    }

    len = snprintf(buf, LBFSZ,
        "%d.%02d %d.%02d %d.%02d %d/%d %d\n",
        load1  / 100, load1  % 100,
        load5  / 100, load5  % 100,
        load15 / 100, load15 % 100,
        running,
        total_procs,
        lastpid
    );

    xlen = (len - pgoff);
    error = uiomove((const char *)buf, xlen, uio);

    free(buf, M_TEMP);
    return error;
}

/*
 * Linux-compatible /proc/stat. The cpu/cpuN lines (user/nice/system/idle ticks)
 * come from the procfsd daemon's host_processor_info(PROCESSOR_CPU_LOAD_INFO)
 * (PROCFS_REQ_CPULOAD); that flavor reads zero for an in-kernel caller, so it is
 * sourced from userspace like the softirq/interrupt counters. The macOS
 * cpu_ticks are already in 1/CLK_TCK units (USER_HZ jiffies), so they map
 * straight onto the Linux columns. btime comes from kern.boottime;
 * interrupt/ctxt/fork counters have no kernel-reachable source and are reported
 * as 0. Without a connected daemon the cpu lines read 0.
 */
int
procfs_dostat(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    int ncpu = 0;
    size_t sz = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &sz, NULL, 0) != 0 || ncpu <= 0) {
        ncpu = 1;
    }

    struct cpuline { uint64_t user, nice, sys, idle; };
    struct cpuline *cl = malloc((size_t)ncpu * sizeof(*cl), M_TEMP, M_WAITOK);
    if (cl == NULL) {
        sbuf_delete(&sb);
        return ENOMEM;
    }
    bzero(cl, (size_t)ncpu * sizeof(*cl));

    struct cpuline agg = { 0, 0, 0, 0 };
    struct procfs_cpu_load ld[64];
    bzero(ld, sizeof(ld));

    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_CPULOAD, 0, 0, ld, sizeof(ld), &got) == 0) {
        uint32_t nld = got / (uint32_t)sizeof(ld[0]);
        for (int i = 0; i < ncpu && (uint32_t)i < nld; i++) {
            cl[i].user  = ld[i].user;
            cl[i].nice  = ld[i].nice;
            cl[i].sys   = ld[i].sys;
            cl[i].idle  = ld[i].idle;
            agg.user   += cl[i].user;
            agg.nice   += cl[i].nice;
            agg.sys    += cl[i].sys;
            agg.idle   += cl[i].idle;
        }
    }

    /* cpu line columns: user nice system idle iowait irq softirq steal guest guest_nice */
    sbuf_printf(&sb, "cpu  %llu %llu %llu %llu 0 0 0 0 0 0\n",
        (unsigned long long)agg.user, (unsigned long long)agg.nice,
        (unsigned long long)agg.sys, (unsigned long long)agg.idle);
    for (int i = 0; i < ncpu; i++) {
        sbuf_printf(&sb, "cpu%d %llu %llu %llu %llu 0 0 0 0 0 0\n", i,
            (unsigned long long)cl[i].user, (unsigned long long)cl[i].nice,
            (unsigned long long)cl[i].sys, (unsigned long long)cl[i].idle);
    }
    free(cl, M_TEMP);

    long long btime = 0;
    struct timeval bt;
    sz = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &sz, NULL, 0) == 0) {
        btime = (long long)bt.tv_sec;
    }

    int total_procs = procfs_get_process_count(vfs_context_ucred(ctx));

    sbuf_printf(&sb, "intr 0\n");
    sbuf_printf(&sb, "ctxt 0\n");
    sbuf_printf(&sb, "btime %lld\n", btime);
    sbuf_printf(&sb, "processes %d\n", total_procs);
    sbuf_printf(&sb, "procs_running 1\n");
    sbuf_printf(&sb, "procs_blocked 0\n");

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/vmstat. The VM page-count globals are stripped on
 * arm64, so the figures come from the procfsd daemon's
 * host_statistics64(HOST_VM_INFO64) (a vm_statistics64). macOS and Linux model
 * memory differently, so the macOS counters are mapped onto the closest Linux
 * keys, plus a few macOS-specific lines (nr_wired/nr_compressed/...). All counts
 * are in pages. Without a daemon every value reads 0.
 */
int
procfs_dovmstat(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    vm_statistics64_data_t vm;
    bzero(&vm, sizeof(vm));
    uint32_t got = 0;
    (void)procfs_ctl_request(PROCFS_REQ_VMSTAT, 0, 0, &vm, sizeof(vm), &got);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb,
        "nr_free_pages %u\n"
        "nr_inactive_anon 0\n"
        "nr_active_anon 0\n"
        "nr_inactive_file %u\n"
        "nr_active_file %u\n"
        "nr_anon_pages %u\n"
        "nr_file_pages %u\n"
        "nr_wired %u\n"
        "nr_purgeable %u\n"
        "nr_speculative %u\n"
        "nr_throttled %u\n"
        "nr_compressed %u\n"
        "pgpgin %llu\n"
        "pgpgout %llu\n"
        "pswpin %llu\n"
        "pswpout %llu\n"
        "pgfault %llu\n"
        "pgmajfault %llu\n"
        "pgreactivate %llu\n"
        "cow_faults %llu\n"
        "decompressions %llu\n"
        "compressions %llu\n",
        (unsigned)vm.free_count,
        (unsigned)vm.inactive_count, (unsigned)vm.active_count,
        (unsigned)vm.internal_page_count, (unsigned)vm.external_page_count,
        (unsigned)vm.wire_count, (unsigned)vm.purgeable_count,
        (unsigned)vm.speculative_count, (unsigned)vm.throttled_count,
        (unsigned)vm.compressor_page_count,
        (unsigned long long)vm.pageins, (unsigned long long)vm.pageouts,
        (unsigned long long)vm.swapins, (unsigned long long)vm.swapouts,
        (unsigned long long)vm.faults, (unsigned long long)vm.pageins,
        (unsigned long long)vm.reactivations, (unsigned long long)vm.cow_faults,
        (unsigned long long)vm.decompressions, (unsigned long long)vm.compressions);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/buddyinfo - Linux buddy-allocator free-block counts, one line per node
 * and zone: the number of free blocks of each order (2^order pages), orders 0
 * through MAX_ORDER-1. macOS is not a buddy allocator and exposes no per-order
 * free lists, so there is no real fragmentation data. Instead the free page
 * count (host_statistics64's free_count, from the daemon via PROCFS_REQ_VMSTAT)
 * is decomposed greedily into buddy orders, largest first, which yields a valid
 * buddyinfo whose blocks account for exactly the free pages - i.e. free memory
 * presented as maximally coalesced. Apple Silicon is single-node UMA, so one
 * "Node 0, zone Normal" line. Zeros without a connected daemon.
 */
#define PROCFS_BUDDY_ORDERS 11          /* Linux MAX_ORDER: orders 0..10 */

int
procfs_dobuddyinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    vm_statistics64_data_t vm;
    bzero(&vm, sizeof(vm));

    uint32_t got = 0;
    (void)procfs_ctl_request(PROCFS_REQ_VMSTAT, 0, 0, &vm, sizeof(vm), &got);

    uint64_t remaining = (uint64_t)vm.free_count;
    uint64_t count[PROCFS_BUDDY_ORDERS];
    for (int order = PROCFS_BUDDY_ORDERS - 1; order >= 0; order--) {
        uint64_t blk = (uint64_t)1 << order;
        count[order] = remaining / blk;
        remaining   %= blk;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "Node 0, zone %8s ", "Normal");

    for (int order = 0; order < PROCFS_BUDDY_ORDERS; order++) {
        sbuf_printf(&sb, "%6llu ", (unsigned long long)count[order]);
    }
    sbuf_printf(&sb, "\n");

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-style /proc/pagetypeinfo - the buddy allocator's free pages and blocks
 * broken down by page-migrate type. macOS is not a buddy allocator and has no
 * migrate types, so - like /proc/buddyinfo - the free page count (from the
 * daemon's host_statistics64) is decomposed greedily into buddy orders and
 * reported under the default "Movable" type (the other types are 0). The block
 * counts derive from physical memory (hw.memsize). One synthetic "Node 0, zone
 * Normal". Column layout matches Linux's mm/vmstat.c. Zeros without a daemon.
 */
#define PROCFS_PAGEBLOCK_ORDER 9        /* synthetic: 2^9 = 512 pages per block */

int
procfs_dopagetypeinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    static const char *const mtype[] = {
        "Unmovable", "Movable", "Reclaimable", "HighAtomic", "Isolate"
    };

    const int nmtype = (int)(sizeof(mtype) / sizeof(mtype[0]));

    /* index of "Movable" in mtype[] */
    const int MOVABLE = 1;

    /* Free page count from host_statistics64 via the daemon (as buddyinfo). */
    vm_statistics64_data_t vm;
    bzero(&vm, sizeof(vm));

    uint32_t got = 0;
    (void)procfs_ctl_request(PROCFS_REQ_VMSTAT, 0, 0, &vm, sizeof(vm), &got);
    uint64_t remaining = (uint64_t)vm.free_count;
    uint64_t count[PROCFS_BUDDY_ORDERS];

    for (int order = PROCFS_BUDDY_ORDERS - 1; order >= 0; order--) {
        uint64_t blk = (uint64_t)1 << order;
        count[order] = remaining / blk;
        remaining   %= blk;
    }

    /* Total pageblocks from physical memory (hw.* sysctls work in kernel). */
    uint64_t memsize = 0;
    size_t   msz = sizeof(memsize);
    (void)sysctlbyname("hw.memsize", &memsize, &msz, NULL, 0);
    uint64_t blocks = (memsize / PAGE_SIZE) >> PROCFS_PAGEBLOCK_ORDER;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb, "Page block order: %d\n", PROCFS_PAGEBLOCK_ORDER);
    sbuf_printf(&sb, "Pages per block:  %lu\n\n",
        (unsigned long)((uint64_t)1 << PROCFS_PAGEBLOCK_ORDER));
    sbuf_printf(&sb, "Free pages count per migrate type at order ");

    for (int order = 0; order < PROCFS_BUDDY_ORDERS; order++) {
        sbuf_printf(&sb, "%6d ", order);
    }

    sbuf_printf(&sb, "\n");

    for (int t = 0; t < nmtype; t++) {
        sbuf_printf(&sb, "Node    0, zone   Normal, type %12s ", mtype[t]);
        for (int order = 0; order < PROCFS_BUDDY_ORDERS; order++) {
            sbuf_printf(&sb, "%6llu ",
                (t == MOVABLE) ? (unsigned long long)count[order] : 0ULL);
        }
        sbuf_printf(&sb, "\n");
    }

    sbuf_printf(&sb, "\nNumber of blocks type ");

    for (int t = 0; t < nmtype; t++) {
        sbuf_printf(&sb, "%12s ", mtype[t]);
    }

    sbuf_printf(&sb, "\n");
    sbuf_printf(&sb, "Node 0, zone   Normal ");

    for (int t = 0; t < nmtype; t++) {
        sbuf_printf(&sb, "%12llu ",
            (t == MOVABLE) ? (unsigned long long)blocks : 0ULL);
    }

    sbuf_printf(&sb, "\n");

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/dma - Linux's list of ISA DMA channels in use (one "%2d: <owner>" line
 * per busy channel of the legacy 8237 controller). This is an x86-only concept:
 * on x86 the DMA subsystem always reserves channel 4 as the cascade between the
 * two 8237 controllers, so /proc/dma there is never empty. macOS/XNU uses no
 * 8237 ISA DMA, and Apple Silicon has no such hardware at all, so the node is
 * empty on arm64 and shows only the reserved cascade channel on x86 - matching
 * Linux's own per-architecture behaviour.
 */
int
procfs_dodma(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 64, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

#if defined(__x86_64__)
    /* Channel 4 cascades the two 8237 controllers; it is the one channel the
     * x86 DMA init always reserves, so it is what /proc/dma always shows. */
    sbuf_printf(&sb, "%2d: %s\n", 4, "cascade");
#endif

    /* arm64: no 8237 ISA DMA controller -> no channels in use (empty). */
    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/ioports - Linux's map of allocated I/O port regions ("<start>-<end> :
 * name"). Port-mapped I/O is an x86 concept; ARM (Apple Silicon) has no I/O port
 * space at all - it is memory-mapped only - so the node is empty on arm64, as on
 * ARM Linux. On x86 the legacy ISA controllers (PIC, PIT, 8237 DMA, keyboard,
 * RTC, FPU) sit at architecturally-fixed ports present on all PC-compatible
 * hardware, so those standard regions are reported there.
 */
int
procfs_doioports(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

#if defined(__x86_64__)
    sbuf_printf(&sb,
        "0000-001f : dma1\n"
        "0020-0021 : pic1\n"
        "0040-0043 : timer0\n"
        "0060-0060 : keyboard\n"
        "0064-0064 : keyboard\n"
        "0070-0071 : rtc0\n"
        "0080-008f : dma page reg\n"
        "00a0-00a1 : pic2\n"
        "00c0-00df : dma2\n"
        "00f0-00ff : fpu\n");
#endif

    /* arm64: no port-mapped I/O -> empty. */
    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/iomem - Linux's physical address-space map ("<start>-<end> : name"),
 * dominated by System RAM. macOS publishes no complete physical map to a kext:
 * the device-tree memory node redacts the DRAM base and device regions need
 * multi-level "ranges" translation. The one solid fact is the RAM size, so this
 * reports System RAM (the OS-usable amount, hw.memsize_usable) and Reserved (the
 * firmware carve-out, hw.memsize - usable). The base is nominal 0 - macOS does
 * not expose the true physical base - so this is a size-accurate representation
 * of RAM rather than a literal address map.
 */
int
procfs_doiomem(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    size_t sz;
    uint64_t total = 0;
    uint64_t usable = 0;

    sz = sizeof(total);
    (void)sysctlbyname("hw.memsize", &total, &sz, NULL, 0);
    sz = sizeof(usable);
    (void)sysctlbyname("hw.memsize_usable", &usable, &sz, NULL, 0);

    if (usable == 0 || usable > total) {
        usable = total;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    if (total > 0) {
        sbuf_printf(&sb, "%08llx-%08llx : System RAM\n",
            0ULL, (unsigned long long)(usable - 1));
        if (usable < total) {
            sbuf_printf(&sb, "%08llx-%08llx : Reserved\n",
                (unsigned long long)usable, (unsigned long long)(total - 1));
        }
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

static uint32_t procfs_online_cpus(void);   /* defined with the /proc/irq nodes */

/*
 * /proc/softirqs - Linux's per-CPU softirq counts, one line per softirq type.
 * Softirqs are a Linux-specific bottom-half mechanism; XNU has no softirq layer,
 * but procfs_ctl.c surfaces the equivalent per-CPU event counters (timer
 * interrupts, reschedule IPIs) from the procfsd daemon's
 * host_processor_info(PROCESSOR_CPU_STAT) and maps them onto the softirq vectors
 * (procfs_cpu_softirq_counts). So TIMER/HRTIMER and SCHED carry real numbers;
 * vectors with no XNU counter (network, block, RCU, ...) read 0. Without a
 * connected daemon the whole table is 0.
 */
int
procfs_dosoftirqs(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    /* Per-CPU interrupt counters from the daemon (one request for all CPUs), then
     * map each CPU onto the softirq vectors. */
    uint32_t ncpu = procfs_online_cpus();
    struct procfs_cpu_stat cs[64];
    (void)procfs_cpu_stat_all(cs, ncpu);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* pad over the name column */
    sbuf_printf(&sb, "                    ");
    for (uint32_t c = 0; c < ncpu; c++) {
        sbuf_printf(&sb, "CPU%-8u", c);
    }
    sbuf_printf(&sb, "\n");

    for (int t = 0; t < PROCFS_NR_SOFTIRQ; t++) {
        sbuf_printf(&sb, "%12s:", procfs_softirq_names[t]);
        for (uint32_t c = 0; c < ncpu; c++) {
            uint64_t counts[PROCFS_NR_SOFTIRQ];
            procfs_cpu_softirq_map(&cs[c], counts);
            sbuf_printf(&sb, " %10llu", (unsigned long long)counts[t]);
        }
        sbuf_printf(&sb, "\n");
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/rtc - Linux's real-time-clock state (drivers/rtc/proc.c). The core is
 * the current RTC time and date; macOS keeps its hardware RTC in UTC, exposed
 * via clock_get_calendar_microtime(), so rtc_time/rtc_date are the UTC calendar
 * time. macOS does not expose the RTC's alarm or periodic-interrupt state to a
 * kext, so those fields report their inactive defaults (no alarm, IRQs off,
 * 24-hour mode). Epoch seconds are converted to a civil date in-kernel (the
 * kernel has no gmtime), using Howard Hinnant's days->y/m/d algorithm.
 */
int
procfs_dortc(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);

    uint64_t sod    = (uint64_t)secs % 86400;      /* seconds within the day */
    int hour        = (int)(sod / 3600);
    int min         = (int)((sod % 3600) / 60);
    int sec         = (int)(sod % 60);

    /* Days since the Unix epoch -> civil (year, month, day). */
    int64_t z       = (int64_t)((uint64_t)secs / 86400) + 719468;
    int64_t era     = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe    = (uint64_t)(z - era * 146097);                            /* [0, 146096] */
    uint64_t yoe    = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   /* [0,399] */
    int64_t  year   = (int64_t)yoe + era * 400;
    uint64_t doy    = doe - (365 * yoe + yoe / 4 - yoe / 100);                 /* [0, 365] */
    uint64_t mp     = (5 * doy + 2) / 153;                                     /* [0, 11] */
    int day         = (int)(doy - (153 * mp + 2) / 5 + 1);                        /* [1, 31] */
    int month       = (int)(mp < 10 ? mp + 3 : mp - 9);                           /* [1, 12] */
    year           += (month <= 2);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb,
        "rtc_time\t: %02d:%02d:%02d\n"
        "rtc_date\t: %04lld-%02d-%02d\n"
        "alrm_time\t: **:**:**\n"
        "alrm_date\t: ****-**-**\n"
        "alarm_IRQ\t: no\n"
        "alrm_pending\t: no\n"
        "update IRQ enabled\t: no\n"
        "periodic IRQ enabled\t: no\n"
        "periodic IRQ frequency\t: 0\n"
        "max user IRQ frequency\t: 64\n"
        "24hr\t\t: yes\n"
        "batt_status\t: okay\n",
        hour, min, sec, (long long)year, month, day);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/execdomains - Linux's registered execution personalities (personality(2)
 * / the legacy exec_domain mechanism for running foreign-OS binaries). Modern
 * Linux keeps only the native personality and emits a single fixed line
 * "0-0\tLinux\t[kernel]". macOS has no exec-domain subsystem; its native
 * personality is Darwin/Mach-O, so - like /proc/version reporting Darwin rather
 * than Linux - the sole domain is reported with the native name.
 */
int
procfs_doexecdomains(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    /*
     * pers_low-pers_high  name(left-justified)  [module]  - the classic
     * exec_domain print format. The personality is "Linux" when a Linux kernel
     * version is being spoofed, otherwise macOS's native "Darwin".
     */
    const char *name = (procfs_spoofed_release() != NULL) ? "Linux" : "Darwin";

    char buf[64];
    int  len = snprintf(buf, sizeof(buf), "0-0\t%-16s\t[kernel]\n", name);

    return procfs_copy_data(buf, len, uio);
}

/*
 * Linux-compatible /proc/meminfo. The field set follows Linux's classic layout
 * so tools that parse meminfo (procps free/top, etc.) find what they expect.
 *
 * Data sources on macOS:
 *   - MemTotal        hw.memsize sysctl (readable from kernel context).
 *   - MemFree         free page count       } from the procfsd daemon's
 *   - Cached          external (file) pages  } host_statistics64(HOST_VM_INFO64)
 *   - Active/Inactive active/inactive pages  } (vm_statistics64, PROCFS_REQ_VMSTAT).
 *   - Swap{Total,Free} vm.swapusage, fetched through the daemon's named-sysctl
 *                     bridge (vm.* sysctls are not readable from kernel context).
 *   - Low{Total,Free} mirror Mem{Total,Free}; High* are 0 (no high memory on 64-bit).
 *   - Hugepagesize    the arm64 2 MB superpage.
 *
 * The remaining Linux fields (Buffers, SwapCached, Dirty, Writeback, Mapped,
 * Slab, Committed_AS, PageTables, Vmalloc*, HugePages_*) have no comparable macOS
 * counter and are reported as 0, as FreeBSD's linprocfs does for the same reason.
 * Without a connected daemon the vm_statistics64 figures are 0.
 */
int
procfs_domeminfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    uint64_t memtotal = 0;                       /* total memory in bytes */
    size_t sz = sizeof(memtotal);
    (void)sysctlbyname("hw.memsize", &memtotal, &sz, NULL, 0);

    uint64_t memfree = 0, cached = 0, active = 0, inactive = 0;

    vm_statistics64_data_t vm;
    bzero(&vm, sizeof(vm));
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_VMSTAT, 0, 0, &vm, sizeof(vm), &got) == 0 &&
        got == sizeof(vm)) {
        memfree  = (uint64_t)vm.free_count          * PAGE_SIZE;
        cached   = (uint64_t)vm.external_page_count * PAGE_SIZE;
        active   = (uint64_t)vm.active_count        * PAGE_SIZE;
        inactive = (uint64_t)vm.inactive_count      * PAGE_SIZE;
    }

    /*
     * Swap: vm.swapusage returns a struct xsw_usage. It is not readable from
     * kernel context, so fetch it as raw bytes through the daemon's named-sysctl
     * request (the same bridge /proc/sys leaves use).
     */
    uint64_t swaptotal = 0, swapfree = 0;
    struct xsw_usage sw;
    bzero(&sw, sizeof(sw));
    uint32_t swgot = 0;
    if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, "vm.swapusage",
            &sw, sizeof(sw), &swgot) == 0 && swgot >= 3 * sizeof(uint64_t)) {
        swaptotal = sw.xsu_total;
        swapfree  = sw.xsu_avail;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb,
        "MemTotal:       %8llu kB\n"
        "MemFree:        %8llu kB\n"
        "Buffers:        %8llu kB\n"
        "Cached:         %8llu kB\n"
        "SwapCached:     %8llu kB\n"
        "Active:         %8llu kB\n"
        "Inactive:       %8llu kB\n"
        "HighTotal:      %8llu kB\n"
        "HighFree:       %8llu kB\n"
        "LowTotal:       %8llu kB\n"
        "LowFree:        %8llu kB\n"
        "SwapTotal:      %8llu kB\n"
        "SwapFree:       %8llu kB\n"
        "Dirty:          %8llu kB\n"
        "Writeback:      %8llu kB\n"
        "Mapped:         %8llu kB\n"
        "Slab:           %8llu kB\n"
        "Committed_AS:   %8llu kB\n"
        "PageTables:     %8llu kB\n"
        "VmallocTotal:   %8llu kB\n"
        "VmallocUsed:    %8llu kB\n"
        "VmallocChunk:   %8llu kB\n"
        "HugePages_Total:%8llu\n"
        "HugePages_Free: %8llu\n"
        "Hugepagesize:   %8llu kB\n",
        (unsigned long long)B2K(memtotal), (unsigned long long)B2K(memfree),
        0ULL, (unsigned long long)B2K(cached), 0ULL,
        (unsigned long long)B2K(active), (unsigned long long)B2K(inactive),
        0ULL, 0ULL,
        (unsigned long long)B2K(memtotal), (unsigned long long)B2K(memfree),
        (unsigned long long)B2K(swaptotal), (unsigned long long)B2K(swapfree),
        0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL,
        0ULL, 0ULL, 0ULL, 0ULL, 0ULL,
        (unsigned long long)(2 * 1024));                 /* 2 MB superpage in kB */

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/partitions. Linux lists every block device; macOS has
 * no kext-linkable way to enumerate raw/unmounted disks (that lives in IOKit),
 * so we list the mounted block-device filesystems instead - real major/minor,
 * block counts and names for everything backed by a /dev node. Each mounted
 * volume is reported (on APFS that means the container's volumes), which is the
 * closest faithful equivalent reachable from a VFS-only kext.
 */
struct procfs_part_ctx {
    struct sbuf *sb;
};

/*
 * vfs_iterate() callout: emit one partitions line per mounted /dev device.
 * Runs with the mount referenced; uses only vfs_statfs() (no blocking I/O or
 * vnode lookups) so it is safe inside the iteration.
 */
static int
procfs_partitions_cb(mount_t mp, void *arg)
{
    struct procfs_part_ctx *pc = (struct procfs_part_ctx *)arg;
    struct vfsstatfs *st = vfs_statfs(mp);

    if (st == NULL || strncmp(st->f_mntfromname, "/dev/", 5) != 0) {
        /* skip non-block-device mounts */
        return VFS_RETURNED;
    }

    dev_t dev = (dev_t)st->f_fsid.val[0];
    uint64_t blocks = ((uint64_t)st->f_blocks * st->f_bsize) >> 10;  /* 1K blocks */

    sbuf_printf(pc->sb, "%4d %7d %10llu %s\n",
        major(dev), minor(dev), (unsigned long long)blocks,
        st->f_mntfromname + 5); /* strip "/dev/" */

    return VFS_RETURNED;
}

int
procfs_dopartitions(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "major minor  #blocks  name\n\n");

    /*
     * Prefer true block-device enumeration via IOKit (procfs_iokit.cpp): every
     * whole disk and partition, mounted or not - the real Linux /proc/partitions
     * semantics. Fall back to the mounted-filesystem list (vfs_iterate) if IOKit
     * matching fails.
     */
    enum { PROCFS_MAX_PARTS = 128 };

    struct procfs_partition *parts = (struct procfs_partition *)
        OSMalloc(PROCFS_MAX_PARTS * sizeof(struct procfs_partition), procfs_osmalloc_tag);

    int n = 0;
    if (parts != NULL &&
        procfs_iokit_get_partitions(parts, PROCFS_MAX_PARTS, &n) == 0 && n > 0) {
        for (int i = 0; i < n; i++) {
            sbuf_printf(&sb, "%4u %7u %10llu %s\n",
                parts[i].major, parts[i].minor,
                (unsigned long long)(parts[i].size / 1024), parts[i].name);
        }
    } else {
        struct procfs_part_ctx pc = { &sb };
        vfs_iterate(0, procfs_partitions_cb, &pc);
    }

    if (parts != NULL) {
        OSFree(parts, PROCFS_MAX_PARTS * sizeof(struct procfs_partition), procfs_osmalloc_tag);
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/diskstats: per-whole-disk cumulative I/O statistics
 * from IOKit (IOBlockStorageDriver "Statistics", via procfs_iokit.cpp). Each line
 * is the classic 14-field Linux format:
 *   major minor name  reads rd_merged rd_sectors rd_ticks
 *                     writes wr_merged wr_sectors wr_ticks
 *                     in_flight io_ticks time_in_queue
 * macOS has no per-device merge/in-flight/queue accounting, so those fields are
 * 0 and io_ticks is approximated as the sum of read and write service times.
 */
int
procfs_dodiskstats(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    enum { PROCFS_MAX_DISKS = 64 };

    struct procfs_diskstat *ds = (struct procfs_diskstat *)
        OSMalloc(PROCFS_MAX_DISKS * sizeof(struct procfs_diskstat), procfs_osmalloc_tag);

    int n = 0;
    if (ds != NULL && procfs_iokit_get_diskstats(ds, PROCFS_MAX_DISKS, &n) == 0) {
        for (int i = 0; i < n; i++) {
            struct procfs_diskstat *d = &ds[i];
            sbuf_printf(&sb,
                "%4u %7u %s %llu 0 %llu %llu %llu 0 %llu %llu 0 %llu 0\n",
                d->major, d->minor, d->name,
                (unsigned long long)d->reads,
                (unsigned long long)d->read_sectors,
                (unsigned long long)d->read_ticks_ms,
                (unsigned long long)d->writes,
                (unsigned long long)d->write_sectors,
                (unsigned long long)d->write_ticks_ms,
                (unsigned long long)(d->read_ticks_ms + d->write_ticks_ms));
        }
    }

    if (ds != NULL) {
        OSFree(ds, PROCFS_MAX_DISKS * sizeof(struct procfs_diskstat), procfs_osmalloc_tag);
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/mtab (the /etc/mtab equivalent): one line per mounted
 * filesystem, "device mountpoint fstype options 0 0". Enumerated the same way as
 * partitions (vfs_iterate + vfs_statfs), but lists every mount, not just block
 * devices, with the mount options decoded from f_flags.
 */
static void
procfs_mtab_options(uint64_t flags, char *out, size_t cap)
{
    static const struct { uint64_t flag; const char *opt; } tab[] = {
        { MNT_NOSUID,      ",nosuid"     },
        { MNT_NODEV,       ",nodev"      },
        { MNT_NOEXEC,      ",noexec"     },
        { MNT_SYNCHRONOUS, ",sync"       },
        { MNT_ASYNC,       ",async"      },
        { MNT_NOATIME,     ",noatime"    },
        { MNT_UNION,       ",union"      },
        { MNT_JOURNALED,   ",journaled"  },
        { MNT_LOCAL,       ",local"      },
        { MNT_DONTBROWSE,  ",nobrowse"   },
    };

    strlcpy(out, (flags & MNT_RDONLY) ? "ro" : "rw", cap);
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (flags & tab[i].flag) {
            strlcat(out, tab[i].opt, cap);
        }
    }
}

struct procfs_mtab_ctx {
    struct sbuf *sb;
};

static int
procfs_mtab_cb(mount_t mp, void *arg)
{
    struct procfs_mtab_ctx *mc = (struct procfs_mtab_ctx *)arg;
    struct vfsstatfs *st = vfs_statfs(mp);

    if (st == NULL) {
        return VFS_RETURNED;
    }

    char opts[160];
    procfs_mtab_options(st->f_flags, opts, sizeof(opts));

    sbuf_printf(mc->sb, "%s %s %s %s 0 0\n",
        st->f_mntfromname, st->f_mntonname, st->f_fstypename, opts);

    return VFS_RETURNED;
}

int
procfs_domtab(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    struct procfs_mtab_ctx mc = { &sb };
    vfs_iterate(0, procfs_mtab_cb, &mc);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/<pid>/maps. Shares the VM-region walk in procfs_map.c
 * (procfs_map_render) with the NetBSD-format `map` node; only the per-region
 * line format is Linux-specific:
 *   start-end perms offset dev inode path
 * The dev/inode/path columns are reported as "00:00 0" with no path, since the
 * region's backing file is not reachable here (see procfs_map.c).
 */
static void
procfs_maps_fmt_linux(struct sbuf *sb, const struct procfs_region *r)
{
    sbuf_printf(sb, "%016llx-%016llx %c%c%c%c %016llx 00:00 0 \n",
        (unsigned long long)r->start, (unsigned long long)r->end,
        (r->prot & VM_PROT_READ)    ? 'r' : '-',
        (r->prot & VM_PROT_WRITE)   ? 'w' : '-',
        (r->prot & VM_PROT_EXECUTE) ? 'x' : '-',
         r->shared                  ? 's' : 'p',
        (unsigned long long)r->offset);
}

int
procfs_domaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    return procfs_map_render(pnp, uio, ctx, procfs_maps_fmt_linux);
}

/*
 * The smaps family - /proc/<pid>/smaps, smaps_rollup and numa_maps - reuse the
 * shared VM_REGION_EXTENDED_INFO walk (procfs_map_foreach_ext in procfs_map.c)
 * with a per-region callback here. macOS provides resident/dirtied/swapped page
 * counts and the share mode; fields without a source are approximated: Pss and
 * Referenced track Rss, the share mode classifies a region's whole Rss as shared
 * or private, the maps offset is 0, and NUMA is single-node (policy "default").
 */
#define PROCFS_PAGE_KB  ((uint64_t)PAGE_SIZE / 1024)

/* Per-region smaps block: the maps header line plus the memory detail. */
static void
procfs_smaps_cb(const struct procfs_ext_region *r, void *arg)
{
    struct sbuf *sb = (struct sbuf *)arg;

    uint64_t size_kb  = (r->end - r->start) / 1024;
    uint64_t rss_kb   = (uint64_t)r->resident_pages * PROCFS_PAGE_KB;
    uint64_t dirty_kb = (uint64_t)r->dirty_pages    * PROCFS_PAGE_KB;

    if (dirty_kb > rss_kb) {
        dirty_kb = rss_kb;
    }

    uint64_t clean_kb = rss_kb - dirty_kb;
    uint64_t swap_kb  = (uint64_t)r->swapped_pages  * PROCFS_PAGE_KB;
    uint64_t anon_kb  = r->anonymous ? rss_kb : 0;

    sbuf_printf(sb, "%016llx-%016llx %c%c%c%c %016llx 00:00 0 \n",
        (unsigned long long)r->start, (unsigned long long)r->end,
        (r->prot & VM_PROT_READ)    ? 'r' : '-',
        (r->prot & VM_PROT_WRITE)   ? 'w' : '-',
        (r->prot & VM_PROT_EXECUTE) ? 'x' : '-',
        r->shared                   ? 's' : 'p',
        0ULL);

    sbuf_printf(sb,
        "Size:           %8llu kB\n"
        "KernelPageSize: %8llu kB\n"
        "MMUPageSize:    %8llu kB\n"
        "Rss:            %8llu kB\n"
        "Pss:            %8llu kB\n"
        "Shared_Clean:   %8llu kB\n"
        "Shared_Dirty:   %8llu kB\n"
        "Private_Clean:  %8llu kB\n"
        "Private_Dirty:  %8llu kB\n"
        "Referenced:     %8llu kB\n"
        "Anonymous:      %8llu kB\n"
        "Swap:           %8llu kB\n",
        (unsigned long long)size_kb,
        (unsigned long long)PROCFS_PAGE_KB,
        (unsigned long long)PROCFS_PAGE_KB,
        (unsigned long long)rss_kb,
        (unsigned long long)rss_kb,
        (unsigned long long)(r->shared ? clean_kb : 0),
        (unsigned long long)(r->shared ? dirty_kb : 0),
        (unsigned long long)(r->shared ? 0 : clean_kb),
        (unsigned long long)(r->shared ? 0 : dirty_kb),
        (unsigned long long)rss_kb,
        (unsigned long long)anon_kb,
        (unsigned long long)swap_kb);

    sbuf_printf(sb, "VmFlags:%s%s%s%s\n",
        (r->prot & VM_PROT_READ)    ? " rd" : "",
        (r->prot & VM_PROT_WRITE)   ? " wr" : "",
        (r->prot & VM_PROT_EXECUTE) ? " ex" : "",
         r->shared                  ? " sh" : "");
}

int
procfs_dosmaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_map_foreach_ext(pnp, ctx, procfs_smaps_cb, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/* Accumulator for smaps_rollup: sums across every mapping. */
struct procfs_rollup {
    uint64_t    first;
    uint64_t    last;
    uint64_t    rss;
    uint64_t    shared_clean;
    uint64_t    shared_dirty;
    uint64_t    private_clean;
    uint64_t    private_dirty;
    uint64_t    anon;
    uint64_t    swap;
    int         seen;
};

static void
procfs_rollup_cb(const struct procfs_ext_region *r, void *arg)
{
    struct procfs_rollup *t = (struct procfs_rollup *)arg;

    if (!t->seen) {
        t->first = r->start;
        t->seen  = 1;
    }

    t->last = r->end;

    uint64_t rss_kb = (uint64_t)r->resident_pages * PROCFS_PAGE_KB;
    uint64_t dirty_kb = (uint64_t)r->dirty_pages * PROCFS_PAGE_KB;

    if (dirty_kb > rss_kb) {
        dirty_kb = rss_kb;
    }

    uint64_t clean_kb = rss_kb - dirty_kb;

    t->rss  += rss_kb;
    t->swap += (uint64_t)r->swapped_pages * PROCFS_PAGE_KB;

    if (r->shared) {
        t->shared_clean += clean_kb; t->shared_dirty += dirty_kb;
    } else {
        t->private_clean += clean_kb; t->private_dirty += dirty_kb;
    }

    if (r->anonymous) {
        t->anon += rss_kb;
    }
}

int
procfs_dosmaps_rollup(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    struct procfs_rollup t;
    bzero(&t, sizeof(t));

    error = procfs_map_foreach_ext(pnp, ctx, procfs_rollup_cb, &t);
    if (error != 0) {
        return error;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "%016llx-%016llx ---p 00000000 00:00 0 [rollup]\n",
        (unsigned long long)t.first, (unsigned long long)t.last);
    sbuf_printf(&sb,
        "Rss:            %8llu kB\n"
        "Pss:            %8llu kB\n"
        "Shared_Clean:   %8llu kB\n"
        "Shared_Dirty:   %8llu kB\n"
        "Private_Clean:  %8llu kB\n"
        "Private_Dirty:  %8llu kB\n"
        "Referenced:     %8llu kB\n"
        "Anonymous:      %8llu kB\n"
        "Swap:           %8llu kB\n",
        (unsigned long long)t.rss,
        (unsigned long long)t.rss,
        (unsigned long long)t.shared_clean,
        (unsigned long long)t.shared_dirty,
        (unsigned long long)t.private_clean,
        (unsigned long long)t.private_dirty,
        (unsigned long long)t.rss,
        (unsigned long long)t.anon,
        (unsigned long long)t.swap);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/* Per-region numa_maps line: address, policy, and single-node page counts. */
static void
procfs_numa_cb(const struct procfs_ext_region *r, void *arg)
{
    struct sbuf *sb = (struct sbuf *)arg;

    sbuf_printf(sb, "%llx default", (unsigned long long)r->start);

    if (r->anonymous && r->resident_pages > 0) {
        sbuf_printf(sb, " anon=%u", r->resident_pages);
    }
    if (r->dirty_pages > 0) {
        sbuf_printf(sb, " dirty=%u", r->dirty_pages);
    }
    if (r->swapped_pages > 0) {
        sbuf_printf(sb, " swapcache=%u", r->swapped_pages);
    }
    if (r->resident_pages > 0) {
        sbuf_printf(sb, " N0=%u", r->resident_pages);   /* single NUMA node */
    }

    sbuf_printf(sb, " kernelpagesize_kB=%llu\n", (unsigned long long)PROCFS_PAGE_KB);
}

int
procfs_donuma_maps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_map_foreach_ext(pnp, ctx, procfs_numa_cb, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible per-thread files: /proc/<pid>/task/<tid>/{comm,stat,status,
 * sched}. The per-thread data (run state, user/system time, name, priority,
 * policy) comes from the procfsd daemon via proc_pidinfo(PROC_PIDTHREADID64INFO)
 * keyed on the tid; process-level fields come from the kext directly. Fields
 * with no macOS source (Linux fault counters, CFS scheduler internals, register
 * addresses) are reported as 0 to keep the layout parseable.
 */
#define PROCFS_NS_PER_TICK 10000000ULL   /* 100 Hz: ns -> clock ticks */

/* Fetch per-thread info from the daemon (ti left zeroed if unavailable). */
static int
procfs_thread_info(pfsnode_t *pnp, struct proc_threadinfo *ti)
{
    int error = 0;

    bzero(ti, sizeof(*ti));
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_THREADINFO, pnp->node_id.nodeid_pid,
            pnp->node_id.nodeid_objectid, ti, sizeof(*ti), &got) == 0 &&
        got == sizeof(*ti)) {
        return error;
    }

    return ENOTSUP;     /* best-effort: callers format the zeroed struct */
}

/* Fetch proc_taskinfo from the daemon (ti left zeroed if unavailable). */
static int
procfs_task_info(pfsnode_t *pnp, struct proc_taskinfo *ti)
{
    int error = 0;

    bzero(ti, sizeof(*ti));
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_TASKINFO, pnp->node_id.nodeid_pid, 0,
            ti, sizeof(*ti), &got) == 0 && got == sizeof(*ti)) {
        return error;
    }

    return ENOTSUP;     /* best-effort: callers format the zeroed struct */
}

/* Map a BSD p_stat to the Linux process state character. */
static char
procfs_proc_state(int p_stat)
{
    switch (p_stat) {
    case SIDL:   return 'I';
    case SRUN:   return 'R';
    case SSTOP:  return 'T';
    case SZOMB:  return 'Z';
    case SSLEEP: return 'S';
    default:     return 'S';
    }
}

/* Process-level context for the thread's owning process. */
struct procfs_pctx {
    int      pid;
    int      ppid;
    int      pgid;
    int      sid;
    int      nthreads;
    uint64_t vsize, rsize;
    char     comm[MAXCOMLEN + 1];
    char     state;                 /* Linux process-state char from p_stat */
};

static void
procfs_pctx_get(pfsnode_t *pnp, struct procfs_pctx *c)
{
    bzero(c, sizeof(*c));
    c->state = 'S';
    c->pid = pnp->node_id.nodeid_pid;

    proc_t p = proc_find(c->pid);
    if (p == PROC_NULL) {
        return;
    }

    c->ppid     = proc_ppid(p);
    c->pgid     = proc_pgrpid(p);
    c->sid      = proc_sessionid(p);
    c->state    = procfs_proc_state(p->p_stat);

    /*
     * Memory sizes and thread count come from the daemon's proc_taskinfo
     * (proc_pidinfo, which needs no task port). The in-kernel alternatives -
     * the VM-map walk in procfs_task_vm_sizes and procfs_get_task_thread_count -
     * go through task_for_pid, so they return 0 for the SIP/hardened processes
     * whose task port is denied; we fall back to them only when no daemon is
     * connected. proc_taskinfo also reports the number of running threads, which
     * refines the coarse BSD p_stat (almost always SRUN) into Linux's
     * running/sleeping split the way ps and top do - so an idle daemon reports
     * 'S', not 'R'. Zombie/stopped/idle states from p_stat are left as-is.
     */
    struct proc_taskinfo ti;
    if (procfs_task_info(pnp, &ti) == 0) {
        c->vsize    = ti.pti_virtual_size;
        c->rsize    = ti.pti_resident_size;
        c->nthreads = ti.pti_threadnum;
        if (c->state == 'R' && ti.pti_numrunning == 0) {
            c->state = 'S';
        }
    } else {
        c->nthreads = procfs_get_task_thread_count(p);
        (void)procfs_task_vm_sizes(p, &c->vsize, &c->rsize);
    }

    proc_name(c->pid, c->comm, sizeof(c->comm));
    proc_rele(p);
}

static char
procfs_thread_state(int run_state)
{
    switch (run_state) {
    case TH_STATE_RUNNING:         return 'R';
    case TH_STATE_STOPPED:         return 'T';
    case TH_STATE_UNINTERRUPTIBLE: return 'D';
    case TH_STATE_WAITING:
    case TH_STATE_HALTED:
    default:                       return 'S';
    }
}

static const char *
procfs_thread_state_word(char c)
{
    switch (c) {
    case 'R': return "running";
    case 'T': return "stopped";
    case 'D': return "disk sleep";
    case 'Z': return "zombie";
    case 'I': return "idle";
    default:  return "sleeping";
    }
}

int
procfs_dothreadcomm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    procfs_thread_info(pnp, &ti);

    const char *name = ti.pth_name[0] ? ti.pth_name : NULL;
    char comm[MAXCOMLEN + 1] = { 0 };

    if (name == NULL) {
        proc_name(pnp->node_id.nodeid_pid, comm, sizeof(comm));
        name = comm;
    }

    char buf[MAXTHREADNAMESIZE + 2];
    int len = snprintf(buf, sizeof(buf), "%s\n", name);

    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadstat(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx c;

    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t tid = pnp->node_id.nodeid_objectid;

    const char *name  = ti.pth_name[0] ? ti.pth_name : c.comm;
    char state = procfs_thread_state(ti.pth_run_state);

    uint64_t utime = ti.pth_user_time / PROCFS_NS_PER_TICK;
    uint64_t stime = ti.pth_system_time / PROCFS_NS_PER_TICK;
    uint64_t rss_pages = c.rsize / PAGE_SIZE;

    /*
     * Linux /proc/<pid>/task/<tid>/stat: 52 space-separated fields. Field 41 is
     * the scheduling policy; fields with no macOS source are 0/-1.
     */
    char buf[640];

    int len = snprintf(buf, sizeof(buf),
        "%llu (%s) %c %d %d %d 0 -1 0 0 0 0 0 "     /* 1-13  */
        "%llu %llu 0 0 %d 0 %d 0 0 "                /* 14-22 */
        "%llu %llu 18446744073709551615 "           /* 23-25 */
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "            /* 26-40 */
        "%d "                                       /* 41 policy */
        "0 0 0 0 0 0 0 0 0 0 0\n",                  /* 42-52 */
        (unsigned long long)tid,
        name,
        state,
        c.ppid,
        c.pgid,
        c.sid,
        (unsigned long long)utime,
        (unsigned long long)stime,
        ti.pth_curpri,
        c.nthreads,
        (unsigned long long)c.vsize,
        (unsigned long long)rss_pages,
        ti.pth_policy);

    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadstatus(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx c;

    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t tid  = pnp->node_id.nodeid_objectid;
    const char *name = ti.pth_name[0] ? ti.pth_name : c.comm;
    char st = procfs_thread_state(ti.pth_run_state);

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "Name:\t%s\n"
        "State:\t%c (%s)\n"
        "Tgid:\t%d\n"
        "Pid:\t%llu\n"
        "PPid:\t%d\n"
        "VmSize:\t%8llu kB\n"
        "VmStk:\t%8llu kB\n"
        "Threads:\t%d\n"
        "voluntary_ctxt_switches:\t0\n"
        "nonvoluntary_ctxt_switches:\t0\n",
        name, st, procfs_thread_state_word(st),
        c.pid, (unsigned long long)tid, c.ppid,
        (unsigned long long)(c.vsize >> 10), 0ULL, c.nthreads);

    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadsched(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx c;
    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t tid  = pnp->node_id.nodeid_objectid;
    const char *name = ti.pth_name[0] ? ti.pth_name : c.comm;

    /* Linux's CFS-internal se.* metrics (vruntime, load.weight, avg.*) have no
     * macOS equivalent and are omitted; we report the metrics we do have. */
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "%s (%llu, #threads: %d)\n"
        "-------------------------------------------------------------------\n"
        "se.sum_exec_runtime  : %20llu\n"
        "policy               : %20d\n"
        "prio                 : %20d\n",
        name, (unsigned long long)tid, c.nthreads,
        (unsigned long long)(ti.pth_user_time + ti.pth_system_time),
        ti.pth_policy, ti.pth_curpri);

    return procfs_copy_data(buf, len, uio);
}

/*
 * Spoofed Linux kernel version. procfs_linux_version indexes procfs_linux_versions[]
 * (0 = None). When non-zero, the OS identity nodes report Linux instead of Darwin.
 * The preset list here MUST stay in sync with the menu-bar app's version menu.
 */
int procfs_linux_version = 0;

static const char *const procfs_linux_versions[] = {
    NULL,           /* 0 = None (native Darwin) */
    "6.12.0",
    "6.6.0",
    "6.1.0",
    "5.15.0",
    "5.10.0",
    "2.5.47",       /* pre-kallsyms era: exposes /proc/ksyms, not /proc/kallsyms */
};

#define PROCFS_LINUX_NVERSIONS \
    (int)(sizeof(procfs_linux_versions) / sizeof(procfs_linux_versions[0]))

const char *
procfs_spoofed_release(void)
{
    int v = procfs_linux_version;
    if (v > 0 && v < PROCFS_LINUX_NVERSIONS) {
        return procfs_linux_versions[v];
    }
    return NULL;
}

/* Dotted-decimal version compare: returns <0, 0, or >0. */
static int
procfs_vercmp(const char *a, const char *b)
{
    int error = 0;

    while (*a != '\0' || *b != '\0') {
        int na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') { na = na * 10 + (*a++ - '0'); }
        while (*b >= '0' && *b <= '9') { nb = nb * 10 + (*b++ - '0'); }
        if (na != nb) { return na - nb; }
        if (*a == '.') { a++; }
        if (*b == '.') { b++; }
    }

    return error;
}

/*
 * Version-spoof gating for the kernel symbol-table nodes. Linux renamed
 * /proc/ksyms to /proc/kallsyms after 2.5.47, so when a Linux release is spoofed
 * we expose only the era-appropriate name: <= 2.5.47 shows ksyms (hides
 * kallsyms), > 2.5.47 shows kallsyms (hides ksyms). With no spoof (native
 * Darwin) both nodes are visible. Other nodes are never hidden.
 */
boolean_t
procfs_node_version_hidden(const char *name)
{
    const char *rel = procfs_spoofed_release();

    if (rel == NULL) {
        return FALSE;                       /* spoof off -> show both */
    }

    boolean_t is_ksyms    = (strcmp(name, "ksyms") == 0);
    boolean_t is_kallsyms = (strcmp(name, "kallsyms") == 0);

    if (!is_ksyms && !is_kallsyms) {
        return FALSE;
    }

    if (procfs_vercmp(rel, "2.5.47") <= 0) {
        return is_kallsyms;                 /* ksyms era: hide kallsyms */
    }

    return is_ksyms;                        /* kallsyms era: hide ksyms */
}

int
procfs_build_linux_version(char *buf, size_t sz)
{
    const char *rel = procfs_spoofed_release();

    if (rel == NULL) {
        return 0;
    }

    return snprintf(buf, sz,
        "Linux version %s (builder@linux-build-env) "
        "(gcc version 10.3.0 (Ubuntu 10.3.0-1ubuntu1)) "
        "#1 SMP PREEMPT_DYNAMIC Sat Dec 14 12:00:00 UTC 2024\n", rel);
}

/*
 * Linux-compatible /proc/version. Reports the spoofed Linux version string when
 * procfs.linux_version is set, otherwise the native Darwin kernel version.
 */
int
procfs_doversion(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;
    int len = 0, xlen = 0;
    char vb[LBFSZ];

    int vlen = procfs_build_linux_version(vb, sizeof(vb));
    if (vlen > 0) {
        return procfs_copy_data(vb, (size_t)vlen, uio);
    }

    vm_offset_t off = uio_offset(uio);
    vm_offset_t pgno = trunc_page(off);
    off_t pgoff = (off - pgno);

    char *buf = malloc(LBFSZ, M_TEMP, M_WAITOK);

    /*
     * Print out the kernel version string.
     */
    len = snprintf(buf, LBFSZ, "%s\n", version);
    xlen = (len - pgoff);
    error = uiomove((const char *)buf, xlen, uio);

    free(buf, M_TEMP);
    return error;
}

/*
 * Linux-compatibility-mode register dump (human-readable text).
 *
 * This is the Linux-flavoured counterpart to the native binary /proc/<pid>/regs
 * node implemented in procfs_regs.c. It reports the same machine state - the
 * representative thread's, via thread_get_state() - but as "name 0xvalue" lines
 * instead of a raw struct.
 *
 * GUARDED FOR NOW: it is compiled but not attached to any node. It will be
 * selected by the planned userspace switch that toggles procfs between native
 * BSD/XNU and Linux-compatible presentation; until that selector exists, the
 * structure wires the native procfs_doregs() unconditionally.
 */
int
procfs_doregs_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    int sysproc = (p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0;
    proc_rele(p);

    if (sysproc) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* The register state comes from the procfsd daemon (see procfs_regs.c);
     * an empty body means the daemon is unavailable or task_for_pid was denied. */
#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_REGS, pnp->node_id.nodeid_pid, 0,
        &st, sizeof(st), &got) == 0 && got > 0) {

        /* non-opaque arm_thread_state64 layout; pc/lr carry PAC bits, emitted raw. */
        for (int i = 0; i < 29; i++) {
            sbuf_printf(&sb, "x%-2d  0x%016llx\n", i, (uint64_t)st.x[i]);
        }

        sbuf_printf(&sb, "fp   0x%016llx\n", (uint64_t)st.fp);
        sbuf_printf(&sb, "lr   0x%016llx\n", (uint64_t)st.lr);
        sbuf_printf(&sb, "sp   0x%016llx\n", (uint64_t)st.sp);
        sbuf_printf(&sb, "pc   0x%016llx\n", (uint64_t)st.pc);
        sbuf_printf(&sb, "cpsr 0x%08x\n",    (uint32_t)st.cpsr);
    }
#elif defined(__x86_64__)
    x86_thread_state64_t st;
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_REGS, pnp->node_id.nodeid_pid, 0,
                           &st, sizeof(st), &got) == 0 && got > 0) {
        sbuf_printf(&sb, "rax 0x%016llx\nrbx 0x%016llx\nrcx 0x%016llx\n"
                         "rdx 0x%016llx\nrsi 0x%016llx\nrdi 0x%016llx\n"
                         "rbp 0x%016llx\nrsp 0x%016llx\n",
            (uint64_t)st.rax, (uint64_t)st.rbx, (uint64_t)st.rcx,
            (uint64_t)st.rdx, (uint64_t)st.rsi, (uint64_t)st.rdi,
            (uint64_t)st.rbp, (uint64_t)st.rsp);

        sbuf_printf(&sb, "r8  0x%016llx\nr9  0x%016llx\nr10 0x%016llx\n"
                         "r11 0x%016llx\nr12 0x%016llx\nr13 0x%016llx\n"
                         "r14 0x%016llx\nr15 0x%016llx\n",
            (uint64_t)st.r8,  (uint64_t)st.r9,  (uint64_t)st.r10,
            (uint64_t)st.r11, (uint64_t)st.r12, (uint64_t)st.r13,
            (uint64_t)st.r14, (uint64_t)st.r15);

        sbuf_printf(&sb, "rip 0x%016llx\nrflags 0x%016llx\n",
            (uint64_t)st.rip, (uint64_t)st.rflags);
    }
#endif

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatibility-mode floating-point/SIMD register dump (human-readable
 * text). Counterpart to the native binary /proc/<pid>/fpregs node in
 * procfs_fpregs.c. GUARDED FOR NOW: compiled but not attached to any node (the
 * structure wires the native procfs_dofpregs() unconditionally); it will be
 * selected by the planned userspace native/linux mode switch.
 */
int
procfs_dofpregs_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    int sysproc = (p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0;
    proc_rele(p);

    if (sysproc) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* FP/SIMD state comes from the procfsd daemon (see procfs_fpregs.c). */
#if defined(__arm64__) || defined(__aarch64__)
    arm_neon_state64_t st;
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_FPREGS, pnp->node_id.nodeid_pid, 0,
                           &st, sizeof(st), &got) == 0 && got > 0) {
        /* Each q register is 128-bit; print as hi:lo 64-bit halves. */
        for (int i = 0; i < 32; i++) {
            const uint64_t *qw = (const uint64_t *)&st.q[i];
            sbuf_printf(&sb, "q%-2d  0x%016llx%016llx\n", i,
                        (uint64_t)qw[1], (uint64_t)qw[0]);
        }

        sbuf_printf(&sb, "fpsr 0x%08x\n", (uint32_t)st.fpsr);
        sbuf_printf(&sb, "fpcr 0x%08x\n", (uint32_t)st.fpcr);
    }
#elif defined(__x86_64__)
    /* Text formatting of the x86 FP/SSE state for the linux-compat mode is not
     * yet implemented; the native binary node provides the full state. */
    sbuf_printf(&sb, "fpregs: x86_64 text format not implemented\n");
#endif

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatibility-mode auxiliary vector (human-readable AT_* text).
 *
 * Counterpart to the native /proc/<pid>/auxv node in procfs_auxv.c, which emits
 * XNU's apple[] array. macOS (Mach-O) has no real ELF aux vector, so this
 * synthesizes the AT_* entries that are cheaply derivable from the kernel - a
 * best-effort Linux-style view. HWCAP/PHDR/ENTRY/RANDOM/EXECFN are not yet
 * synthesized (they need cpu-feature bits and the Mach-O image layout) and can
 * be added later.
 *
 * GUARDED FOR NOW: compiled but not attached to any node; the structure wires
 * the native procfs_doauxv() unconditionally until the userspace native/linux
 * mode switch exists.
 */
int
procfs_doauxv_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    kauth_cred_t cr = kauth_cred_proc_ref(p);
    sbuf_printf(&sb, "AT_PAGESZ\t%u\n", (unsigned)PAGE_SIZE);
    sbuf_printf(&sb, "AT_CLKTCK\t%d\n", 100);
    sbuf_printf(&sb, "AT_UID\t%u\n",    kauth_cred_getruid(cr));
    sbuf_printf(&sb, "AT_EUID\t%u\n",   kauth_cred_getuid(cr));
    sbuf_printf(&sb, "AT_GID\t%u\n",    kauth_cred_getrgid(cr));
    sbuf_printf(&sb, "AT_EGID\t%u\n",   kauth_cred_getgid(cr));
    sbuf_printf(&sb, "AT_SECURE\t%d\n", 0);
    kauth_cred_unref(&cr);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    proc_rele(p);
    return error;
}

#pragma mark -
#pragma mark Process-level Linux text nodes

/* /proc/<pid>/comm - the process (command) name, one line. */
int
procfs_docomm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    char buf[MAXCOMLEN + 2];
    int len = snprintf(buf, sizeof(buf), "%s\n", c.comm);

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/statm - memory usage in pages:
 *   size resident shared text lib data dt
 * Only size (virtual) and resident have a macOS source; the rest are 0.
 */
int
procfs_dostatm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%llu %llu 0 0 0 0 0\n",
        (unsigned long long)(c.vsize / PAGE_SIZE),
        (unsigned long long)(c.rsize / PAGE_SIZE));

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/io - Linux per-process I/O accounting. macOS tracks the process's
 * actual disk I/O bytes (proc_pid_rusage's ri_diskio_*, served by the daemon),
 * which map to read_bytes / write_bytes. It has no read()/write() character
 * counts or syscall tallies, so rchar/wchar/syscr/syscw and cancelled_write_bytes
 * are reported as 0. Owner-only, like the other permission-sensitive pid nodes.
 */
int
procfs_doio(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    error = procfs_check_can_access_process(vfs_context_ucred(ctx), p);
    proc_rele(p);

    if (error != 0) {
        return error;
    }

    uint64_t read_bytes = 0, write_bytes = 0;
    uint64_t io[2] = { 0, 0 };
    uint32_t got = 0;

    /* daemon absent/failed -> zeros (best-effort, like the other daemon nodes) */
    if (procfs_ctl_request(PROCFS_REQ_RUSAGE, pnp->node_id.nodeid_pid, 0,
        io, sizeof(io), &got) == 0 && got == sizeof(io)) {
        read_bytes  = io[0];
        write_bytes = io[1];
    }

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "rchar: 0\n"
        "wchar: 0\n"
        "syscr: 0\n"
        "syscw: 0\n"
        "read_bytes: %llu\n"
        "write_bytes: %llu\n"
        "cancelled_write_bytes: 0\n",
        (unsigned long long)read_bytes,
        (unsigned long long)write_bytes);

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/stat - the process's single-line stat (52 space-separated fields,
 * same layout as the per-thread stat). CPU time comes from the daemon's
 * proc_taskinfo; fields with no macOS source are 0/-1 (priority 20, policy 0).
 */
int
procfs_doprocstat(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    struct proc_taskinfo ti;

    procfs_pctx_get(pnp, &c);
    procfs_task_info(pnp, &ti);

    uint64_t utime = ti.pti_total_user / PROCFS_NS_PER_TICK;
    uint64_t stime = ti.pti_total_system / PROCFS_NS_PER_TICK;
    uint64_t rss_pages = c.rsize / PAGE_SIZE;

    char buf[640];
    int len = snprintf(buf, sizeof(buf),
        "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 "                   /* 1-13  */
        "%llu %llu 0 0 20 0 %d 0 0 "                            /* 14-22 */
        "%llu %llu 18446744073709551615 "                       /* 23-25 */
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "                        /* 26-40 */
        "0 "                                                    /* 41 policy */
        "0 0 0 0 0 0 0 0 0 0 0\n",                              /* 42-52 */
        c.pid, c.comm, c.state, c.ppid, c.pgid, c.sid,
        (unsigned long long)utime, (unsigned long long)stime, c.nthreads,
        (unsigned long long)c.vsize, (unsigned long long)rss_pages);

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/cpu - Linux 2.4's per-CPU accounting for a task: a "cpu" total line
 * (user and system time in USER_HZ ticks) followed by one "cpuN" line per online
 * CPU with the time spent on that CPU. XNU accounts a task's user/system time as
 * a whole (proc_pidtaskinfo) but does not attribute it per CPU, so the "cpu"
 * total carries the real figures and the whole of it is reported on cpu0 (0 on
 * the rest), which keeps the Linux invariant that the per-CPU times sum to the
 * total. Times come from the same source as /proc/<pid>/stat.
 */
int
procfs_docpu(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct proc_taskinfo ti;
    procfs_task_info(pnp, &ti);

    uint64_t utime = ti.pti_total_user / PROCFS_NS_PER_TICK;
    uint64_t stime = ti.pti_total_system / PROCFS_NS_PER_TICK;
    uint32_t ncpu = procfs_online_cpus();

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb, "cpu  %llu %llu\n",
        (unsigned long long)utime, (unsigned long long)stime);

    for (uint32_t i = 0; i < ncpu; i++) {
        sbuf_printf(&sb, "cpu%u %llu %llu\n", i,
            (unsigned long long)(i == 0 ? utime : 0),
            (unsigned long long)(i == 0 ? stime : 0));
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/<pid>/wchan - the kernel symbol the task is blocked in (Linux's
 * CONFIG_KALLSYMS wchan), or "0" if it is not blocked. XNU exposes no KPI for
 * this, so it is recovered directly:
 *
 *   1. procfs_kern.c reads the process's representative blocked thread's
 *      continuation - the function a blocked thread resumes at, i.e. exactly the
 *      wchan - PAC-stripped (procfs_thread_continuation). The offset of the
 *      continuation field in the opaque struct thread is discovered once at
 *      runtime (procfs_wchan_discover), so no fragile hard-coded offsets.
 *   2. The continuation is a slid runtime address. vm_kernel_unslide permutes it
 *      (useless), so instead we measure the running kernel's slide once from a
 *      reference symbol (proc_pid): slide = runtime(&proc_pid) - the daemon's
 *      address for _proc_pid in its symbol source (PROCFS_REQ_KSYM_REF).
 *   3. continuation - slide is the address in the daemon's symbol source, which
 *      the daemon names (PROCFS_REQ_KSYM_LOOKUP). The daemon uses the matching
 *      Kernel Debug Kit's dSYM (full symbols incl. locals) when installed, else
 *      the stripped kernel image (KPI symbols only - most continuations then
 *      won't resolve and yield "0").
 *
 * A thread with no continuation, no matching daemon/KDK, or an unresolvable
 * address yields "0". The Mach C-symbol leading underscore is stripped to match
 * Linux's C-name style; no trailing newline, as on Linux.
 */
/* Running kernel's slide relative to the daemon's wchan symbol source (dSYM),
 * measured once from the reference symbol proc_pid. */
static volatile int64_t g_wchan_slide       = 0;
static volatile int     g_wchan_slide_known = 0;

static void
procfs_wchan_init_slide(void)
{
    if (g_wchan_slide_known) {
        return;
    }

    uint64_t ref = 0;
    uint32_t got = 0;

    if (procfs_ctl_request(PROCFS_REQ_KSYM_REF, 0, 0, &ref, sizeof(ref), &got) == 0 &&
        got == sizeof(ref) && ref != 0) {
        uintptr_t rt = (uintptr_t)ptrauth_strip((void *)proc_pid,
            ptrauth_key_function_pointer);

        g_wchan_slide = (int64_t)((uint64_t)rt - ref);
        g_wchan_slide_known = 1;
    }
}

/*
 * Resolve the process's representative blocked thread's continuation to a bare
 * kernel symbol name (the Mach C-symbol leading underscore stripped, Linux
 * style). Fills buf and returns its length, or 0 if the thread is not blocked
 * on a continuation or the address is unresolvable (no matching daemon/KDK).
 * Shared by /proc/<pid>/wchan and /proc/<pid>/stack, which both need exactly
 * this continuation-to-symbol resolution; see the procfs_dowchan comment above
 * for the full slide/symbolication mechanism. Does not consume the proc ref.
 */
static size_t
procfs_wchan_resolve(proc_t p, char *buf, size_t cap)
{
    int error = 0;

    buf[0] = '\0';

    procfs_wchan_init_slide();

    uint64_t runtime = 0;
    (void)procfs_thread_continuation(p, &runtime);

    if (runtime == 0 || !g_wchan_slide_known) {
        return error;
    }

    /*
     * Map the runtime continuation to the symbol source's layout.
     */
    uint64_t img = runtime - (uint64_t)g_wchan_slide;
    uint32_t got = 0;
    char sym[128];

    if (procfs_ctl_request(PROCFS_REQ_KSYM_LOOKUP, 0, img,
            sym, sizeof(sym) - 1, &got) != 0 || got == 0) {
        return error;
    }

    if (got > sizeof(sym) - 1) {
        got = sizeof(sym) - 1;
    }

    sym[got] = '\0';
    if (sym[0] == '\0') {
        return error;
    }

    /*
     * Drop the Mach C-symbol leading underscore for Linux style.
     */
    const char *s = (sym[0] == '_') ? sym + 1 : sym;
    strlcpy(buf, s, cap);

    return strlen(buf);
}

int
procfs_dowchan(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    char name[128];
    name[0] = '0';
    name[1] = '\0';

    int outlen = 1;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        char sym[128];
        if (procfs_wchan_resolve(p, sym, sizeof(sym)) > 0) {
            strlcpy(name, sym, sizeof(name));
            outlen = (int)strlen(name);
        }
        proc_rele(p);
    }

    return procfs_copy_data(name, outlen, uio);
}

/*
 * /proc/<pid>/stack - Linux's kernel-side call stack of the task's
 * representative thread, each frame as "[<0>] symbol+0xoff/0xsize".
 *
 * On Linux this is a full frame-pointer unwind of the thread's preserved kernel
 * stack. macOS threads almost always block via a *continuation*
 * (thread_block_parameter: mach_msg, semaphore/event waits, sleep): the
 * continuation model deliberately discards the kernel stack while blocked and
 * resumes at a single continuation function, so there is no preserved call
 * chain to unwind - the continuation *is* the complete kernel state. We
 * therefore emit that one continuation frame, resolved exactly as /wchan is
 * (procfs_wchan_resolve). The address is hidden as "[<0>]" like Linux with
 * kptr_restrict; the offset/size are 0 since a continuation is a function entry.
 *
 * A thread not blocked on a continuation (running, or blocked in place keeping
 * its stack) yields empty output, as Linux's /proc/<pid>/stack does for a task
 * that is currently on-CPU.
 */
int
procfs_dostack(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    char out[160];
    int len = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        char sym[128];
        if (procfs_wchan_resolve(p, sym, sizeof(sym)) > 0) {
            len = snprintf(out, sizeof(out), "[<0>] %s+0x0/0x0\n", sym);
        }
        proc_rele(p);
    }

    return procfs_copy_data(out, (size_t)len, uio);
}

/*
 * /proc/<pid>/status in Linux text form - the linux-mode rendering of the status
 * node (native mode emits the binary proc_bsdshortinfo). Reuses procfs_pctx for
 * the process context and the credential for the Uid/Gid rows.
 */
int
procfs_doprocstatus_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    uid_t ruid = 0, euid = 0, svuid = 0;
    gid_t rgid = 0, egid = 0, svgid = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        kauth_cred_t cr = kauth_cred_proc_ref(p);
        ruid  = kauth_cred_getruid(cr);
        euid  = kauth_cred_getuid(cr);
        svuid = kauth_cred_getsvuid(cr);
        rgid  = kauth_cred_getrgid(cr);
        egid  = kauth_cred_getgid(cr);
        svgid = kauth_cred_getsvgid(cr);
        kauth_cred_unref(&cr);
        proc_rele(p);
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb,
        "Name:\t%s\n"
        "State:\t%c (%s)\n"
        "Tgid:\t%d\n"
        "Pid:\t%d\n"
        "PPid:\t%d\n"
        "TracerPid:\t0\n"
        "Uid:\t%u\t%u\t%u\t%u\n"
        "Gid:\t%u\t%u\t%u\t%u\n"
        "VmSize:\t%8llu kB\n"
        "VmRSS:\t%8llu kB\n"
        "Threads:\t%d\n"
        "voluntary_ctxt_switches:\t0\n"
        "nonvoluntary_ctxt_switches:\t0\n",
        c.comm,
        c.state,
        procfs_thread_state_word(c.state),
        c.pid,
        c.pid,
        c.ppid,
        ruid,
        euid,
        svuid,
        euid,                               /* Uid: real effective saved fs (fs~=eff) */
        rgid,
        egid,
        svgid,
        egid,                               /* Gid: real effective saved fs */
        (unsigned long long)(c.vsize >> 10),
        (unsigned long long)(c.rsize >> 10),
        c.nthreads);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/uptime - seconds since boot and (approximate) idle seconds.
 */
int
procfs_douptime(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct timeval tv;
    microuptime(&tv);

    /*
     * Field 2 is summed CPU idle time; macOS has no cheap kernel-context source
     * for it here, so report 0.00 (the common consumer, uptime(1), uses field 1).
     */
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%lld.%02d 0.00\n",
        (long long)tv.tv_sec, (int)(tv.tv_usec / 10000));

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/swaps - macOS uses dynamic swap files under /private/var/vm .
 * Report the aggregate usage from the vm.swapusage sysctl as a single swap area.
 */
int
procfs_doswaps(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    struct xsw_usage xsu;
    size_t len = sizeof(xsu);
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");

    if (sysctlbyname("vm.swapusage", &xsu, &len, NULL, 0) == 0 && xsu.xsu_total > 0) {
        sbuf_printf(&sb, "/private/var/vm/swapfile\tfile\t\t%llu\t%llu\t-1\n",
            (unsigned long long)(xsu.xsu_total / 1024),
            (unsigned long long)(xsu.xsu_used  / 1024));
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/* Filesystem types that mount without a backing block device (Linux "nodev"). */
static boolean_t
procfs_fs_is_nodev(const char *type)
{
    static const char *const nodev[] = {
        "devfs", "procfs", "autofs", "nullfs", "fdesc", "tmpfs", "mtmfs",
        "lifs", "bindfs", "webdav", "nfs", "smbfs", "ftp", NULL
    };

    for (int i = 0; nodev[i] != NULL; i++) {
        if (strncmp(type, nodev[i], MFSTYPENAMELEN) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

struct procfs_fstypes_ctx {
    char names[32][MFSTYPENAMELEN];
    int  count;
};

/* vfs_iterate() callout: collect each distinct mounted filesystem type. */
static int
procfs_fstypes_cb(mount_t mp, void *arg)
{
    struct procfs_fstypes_ctx *fc = (struct procfs_fstypes_ctx *)arg;

    struct vfsstatfs *st = vfs_statfs(mp);
    if (st == NULL) {
        return VFS_RETURNED;
    }

    for (int i = 0; i < fc->count; i++) {
        if (strncmp(fc->names[i], st->f_fstypename, MFSTYPENAMELEN) == 0) {
            /* Already seen this type. */
            return VFS_RETURNED;
        }
    }

    if (fc->count < (int)(sizeof(fc->names) / sizeof(fc->names[0]))) {
        strlcpy(fc->names[fc->count], st->f_fstypename, MFSTYPENAMELEN);
        fc->count++;
    }

    return VFS_RETURNED;
}

/*
 * /proc/filesystems - the distinct filesystem types currently in use (macOS has
 * no kernel-context enumeration of all *registered* types, so this lists the
 * mounted ones, deduplicated). "nodev" prefixes types that need no block device.
 */
int
procfs_dofilesystems(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct procfs_fstypes_ctx fc;
    bzero(&fc, sizeof(fc));
    vfs_iterate(0, procfs_fstypes_cb, &fc);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    for (int i = 0; i < fc.count; i++) {
        sbuf_printf(&sb, "%s\t%s\n",
            procfs_fs_is_nodev(fc.names[i]) ? "nodev" : "", fc.names[i]);
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/modules - the Linux-format view of the loaded kernel extensions
 * (name size refcount deps state address). The listing is produced by the
 * procfsd daemon and reassembled by the shared helper in procfs_extensions.c;
 * only the daemon request type differs from /proc/extensions.
 */
int
procfs_domodules(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_dokextlist(PROCFS_REQ_MODULES, uio);
}

/*
 * /proc/devices - the Linux char/block device major-number listing. macOS keeps
 * no named driver registry (cdevsw/bdevsw carry only function pointers), so the
 * procfsd daemon reconstructs the mapping from /dev - each node's rdev gives its
 * type and major, its name gives the driver family - and streams the formatted
 * text back over the same chunked blob transfer used by /proc/modules.
 */
int
procfs_dodevices(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_DEVICES, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/bus/pci/devices - Linux PCI device table. macOS exposes PCI through the
 * IORegistry (IOPCIDevice), not a /proc-style table, so the procfsd daemon
 * enumerates those devices and formats the Linux line for each - bus/devfn,
 * vendor/device id, the device name, and the base addresses/region sizes
 * (BAR0..5 + expansion ROM, with region flags) from IOKit's assigned-addresses.
 * IRQ reports 0 (macOS has no legacy per-device IRQ line). Streamed in chunks
 * like /proc/devices.
 */
int
procfs_dopcidevices(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_PCIDEVICES, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/fb - Linux framebuffer device list, one "<index> <name>" line per
 * registered framebuffer. macOS drives displays through IOKit framebuffers
 * (IOFramebuffer on Intel, IOMobileFramebuffer on Apple Silicon), so the procfsd
 * daemon enumerates those and formats a line per device using the device's
 * IORegistry name as the Linux fix.id. Streamed in chunks like /proc/devices.
 */
int
procfs_dofb(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_FBDEVICES, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/interrupts - Linux's per-CPU interrupt counts, one line per IRQ with
 * the controller and owning device. macOS does not expose per-CPU interrupt
 * counts to userspace or a kext (only private IOReporting has them), so the
 * counts are reported as 0. The IRQ topology, however, is real: the procfsd
 * daemon walks the IORegistry for each device's IOInterruptSpecifiers (the IRQ
 * numbers) and IOInterruptControllers, giving genuine IRQ -> controller ->
 * device rows. Streamed in chunks like /proc/devices.
 */
int
procfs_dointerrupts(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_INTERRUPTS, &sb);
    if (error != 0 && error != ENOTCONN) {
        sbuf_delete(&sb);
        return error;
    }

    /*
     * Append the architecture summary lines with real per-CPU counts - the
     * softirq/interrupt concept from procfs_ctl.c. The daemon supplies the
     * per-device IRQ topology (counts 0, no per-line data on macOS); the kext
     * adds LOC (local timer interrupts) and RES (rescheduling IPIs), which XNU
     * does count per CPU. When there is no daemon the topology is empty, so a
     * CPU-column header is emitted first to keep the summary aligned.
     */
    uint32_t ncpu = procfs_online_cpus();

    if (sbuf_len(&sb) == 0) {
        sbuf_printf(&sb, "     ");

        for (uint32_t c = 0; c < ncpu; c++) {
            sbuf_printf(&sb, "CPU%-8u", c);
        }

        sbuf_printf(&sb, "\n");
    }

    struct procfs_cpu_stat cs[64];
    (void)procfs_cpu_stat_all(cs, ncpu);

    sbuf_printf(&sb, "%4s:", "LOC");

    for (uint32_t c = 0; c < ncpu; c++) {
        sbuf_printf(&sb, " %10llu",
        (unsigned long long)cs[c].timer);
    }

    sbuf_printf(&sb, "   Local timer interrupts\n");
    sbuf_printf(&sb, "%4s:", "RES");

    for (uint32_t c = 0; c < ncpu; c++) {
        sbuf_printf(&sb, " %10llu",
        (unsigned long long)cs[c].ipi);
    }

    sbuf_printf(&sb, "   Rescheduling interrupts\n");

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/irq/ - Linux's IRQ-to-CPU affinity masks. On Linux each /proc/irq/<N>/
 * holds an smp_affinity bitmask naming the CPUs that may service IRQ <N>, plus a
 * default_smp_affinity for new IRQs. macOS routes interrupts through the AIC with
 * no user-settable or per-IRQ-queryable CPU affinity - every IRQ may run on any
 * CPU - so only the default masks are exposed (all online CPUs), and the per-IRQ
 * subdirectories are omitted. These two nodes are default_smp_affinity (hex
 * cpumask) and default_smp_affinity_list (CPU range).
 */
static uint32_t
procfs_online_cpus(void)
{
    uint32_t ncpu = 1;
    size_t sz = sizeof(ncpu);

    if (sysctlbyname("hw.logicalcpu", &ncpu, &sz, NULL, 0) != 0 || ncpu < 1) {
        ncpu = 1;
    }

    return (ncpu > 64) ? 64 : ncpu;
}

int
procfs_doirq_affinity(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    uint32_t ncpu = procfs_online_cpus();

    /* cpumask in hex, comma-separated 32-bit groups (high group first), all
     * bits set for the online CPUs. */
    char buf[64];
    int len;

    if (ncpu <= 32) {
        uint32_t mask = (ncpu == 32) ? 0xffffffffu : ((1u << ncpu) - 1u);
        len = snprintf(buf, sizeof(buf), "%x\n", mask);
    } else {
        uint32_t hi = (ncpu == 64) ? 0xffffffffu : ((1u << (ncpu - 32)) - 1u);
        len = snprintf(buf, sizeof(buf), "%x,ffffffff\n", hi);
    }

    return procfs_copy_data(buf, len, uio);
}

int
procfs_doirq_affinity_list(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    uint32_t ncpu = procfs_online_cpus();

    char buf[32];
    int len = (ncpu <= 1) ? snprintf(buf, sizeof(buf), "0\n")
        : snprintf(buf, sizeof(buf), "0-%u\n", ncpu - 1);

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/tty/drivers - Linux's registered tty-driver table. macOS has no such
 * registry, so the procfsd daemon derives it from the tty devices in /dev,
 * grouped by major (one "<name> /dev/<name> <major> <minor-range> <type>" line
 * per major, like Linux). Chunked transfer like /proc/devices.
 */
int
procfs_dotty_drivers(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_TTYDRIVERS, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/tty/ldiscs - the line disciplines. macOS's tty layer uses the standard
 * terminal discipline (TTYDISC, number 0); it exposes no enumerable table of
 * registered disciplines to a kext, so report the one canonical entry.
 */
int
procfs_dotty_ldiscs(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char text[] = "tty\t\t0\n";

    return procfs_copy_data(text, sizeof(text) - 1, uio);
}

/*
 * /proc/vmallocinfo - Linux lists the kernel's vmalloc'd (virtually-contiguous,
 * non-zone) areas. macOS has no vmalloc; the nearest analog is XNU's kernel VM
 * allocations tagged by site (mach_memory_info, the data behind `zprint -v`),
 * which the procfsd daemon enumerates. One line per site, sorted by size: the
 * address range is 0 (macOS does not expose per-site kernel virtual addresses to
 * userspace), and the size + site name carry the real information. Empty without
 * a connected daemon. Chunked transfer like /proc/allocinfo.
 */
int
procfs_dovmallocinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_VMALLOCINFO, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/slabinfo - Linux's per-cache statistics for the SLAB/SLUB allocator.
 * macOS has no slab allocator, but its zone allocator (zalloc) is the direct
 * analog: every zone is a cache of fixed-size elements. The procfsd daemon
 * enumerates the zones via mach_zone_info (the data behind zprint) and maps them
 * onto the slabinfo columns (active/total objects, object size, objects and
 * pages per allocation chunk, slab counts); the SLUB tunables have no zone
 * equivalent and are 0. Empty without a connected daemon (or without root, since
 * mach_zone_info needs the host privilege port). Chunked transfer like allocinfo.
 */
int
procfs_doslabinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_SLABINFO, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/locks - Linux's table of the byte-range (advisory) file locks the kernel
 * currently holds. XNU stores these per-vnode (vp->v_lockf) with no global
 * registry, so procfs_kern.c enumerates every vnode with the public VFS iterators
 * (vfs_iterate over mounts, vnode_iterate over each mount's vnodes) and emits
 * each vnode's lock list - a fully in-kernel forward-port. macOS has no
 * mandatory locking, so every lock is reported ADVISORY. Empty is normal.
 */
int
procfs_dolocks(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    procfs_build_locks(&sb, ctx);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/kcore - on Linux this is an ELF core image of the running kernel's
 * virtual memory, used by debuggers (gdb/crash) to inspect the live kernel.
 * macOS deliberately does not expose kernel memory to userspace: SIP, KASLR and
 * PPL/KTRR protect it, there is no KPI to enumerate or safely read arbitrary
 * kernel VM, and doing so would defeat the kernel's security model. So we emit a
 * well-formed but empty core: a valid ELF64 ET_CORE header for the running
 * architecture with zero program headers, i.e. no PT_LOAD segments and no memory
 * exposed. Tools still recognise it (`file`/`readelf -h` report an ELF 64-bit
 * core file), matching a hardened/lockdown Linux where /proc/kcore is present
 * but restricted. This is the whole file - 64 bytes, the ELF header alone.
 */

/* ELF constants (avoid depending on <elf.h> in the kext build). */
#define PROCFS_ET_CORE       4
#define PROCFS_EV_CURRENT    1
#define PROCFS_ELFCLASS64    2
#define PROCFS_ELFDATA2LSB   1
#if defined(__arm64__) || defined(__aarch64__)
#define PROCFS_EM_NATIVE     183         /* EM_AARCH64 */
#elif defined(__x86_64__)
#define PROCFS_EM_NATIVE     62          /* EM_X86_64 */
#else
#define PROCFS_EM_NATIVE     0           /* EM_NONE */
#endif

struct procfs_elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

int
procfs_dokcore(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_elf64_ehdr eh;
    bzero(&eh, sizeof(eh));

    eh.e_ident[0] = 0x7f;               /* ELF magic */
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = PROCFS_ELFCLASS64;
    eh.e_ident[5] = PROCFS_ELFDATA2LSB;
    eh.e_ident[6] = PROCFS_EV_CURRENT;
    /* e_ident[7..15] (OS/ABI + padding) stay zero. */

    eh.e_type    = PROCFS_ET_CORE;
    eh.e_machine = PROCFS_EM_NATIVE;
    eh.e_version = PROCFS_EV_CURRENT;
    eh.e_ehsize  = (uint16_t)sizeof(eh);
    /* No program/section headers: no kernel memory is exposed. */

    return procfs_copy_data((const char *)&eh, sizeof(eh), uio);
}

/*
 * /proc/kmsg - Linux's kernel log buffer (the data behind dmesg). macOS keeps
 * the same classic kernel printf buffer, read with proc_kmsgbuf() - a root-only
 * libproc call - so the procfsd daemon (running as root) takes a snapshot and
 * streams it here. Unlike Linux's blocking/consuming interface, repeated reads
 * return the current buffer (like `dmesg`, not a drain). Empty without a
 * connected daemon. Chunked transfer like /proc/vmallocinfo.
 */
int
procfs_dokmsg(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_KMSG, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon -> empty node */
        return (error == ENOTCONN) ? 0 : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/last_kmsg - Linux/Android's kernel log from before the last reboot. macOS
 * has no per-reboot RAM console, but it does persist one cross-boot kernel log:
 * the kernel panic report. The procfsd daemon returns the newest panic report
 * (the world-readable panic-full-*.panic files under /Library/Logs/
 * DiagnosticReports) - exactly what last_kmsg is used for, diagnosing the prior
 * crash. Empty when there is no panic report (or without a connected daemon).
 * Chunked transfer like /proc/kmsg.
 */
int
procfs_dolast_kmsg(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_LAST_KMSG, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon -> empty node */
        return (error == ENOTCONN) ? 0 : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/ksyms - Linux's kernel symbol table ("address type name" per line; the
 * modern name is /proc/kallsyms). macOS does not expose the running kernel's
 * symbols at runtime (KASLR/SIP), but the procfsd daemon reads the symbol table
 * from the running kernel's on-disk image (the arch/SoC-specific Mach-O under
 * /System/Library/Kernels) and returns nm-style lines with the static (unslid)
 * link addresses. To mirror Linux's kptr_restrict we apply the reader's
 * privilege here: root sees the real addresses; every other reader gets the
 * address column zeroed (symbol names and types remain). Empty without a
 * connected daemon. Chunked transfer like /proc/vmallocinfo.
 */
/*
 * Shared body for /proc/ksyms and /proc/kallsyms: fetch the symbol-table blob
 * for req_type from the daemon and copy it out, applying kptr_restrict (only
 * root sees real kernel addresses; every other reader gets the 16-hex address
 * column zeroed - names and types remain).
 */
static int
procfs_do_symtab(uio_t uio, vfs_context_t ctx, int req_type)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(req_type, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon -> empty node */
        return (error == ENOTCONN) ? 0 : error;
    }

    sbuf_finish(&sb);

    kauth_cred_t cred = vfs_context_ucred(ctx);
    if (cred == NULL || kauth_cred_getuid(cred) != 0) {
        char *d = sbuf_data(&sb);
        int n = sbuf_len(&sb);
        boolean_t at_line_start = TRUE;

        for (int i = 0; i < n; i++) {
            if (at_line_start && d[i] != '\n') {
                for (int k = 0; k < 16 && (i + k) < n &&
                    d[i + k] != ' ' && d[i + k] != '\n'; k++) {
                    d[i + k] = '0';
                }
                at_line_start = FALSE;
            }
            if (d[i] == '\n') {
                at_line_start = TRUE;
            }
        }
    }

    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

int
procfs_doksyms(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    return procfs_do_symtab(uio, ctx, PROCFS_REQ_KSYMS);
}

/*
 * /proc/kallsyms - the modern, fuller symbol table. Same as /proc/ksyms (the
 * exported kernel symbols with real addresses) plus the non-exported symbol
 * names the daemon recovers from the XNU source; macOS ships the arm64 kernel
 * stripped of those locals, so they carry address 0. kptr_restrict applies as
 * for ksyms. Empty without a connected daemon.
 */
int
procfs_dokallsyms(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    return procfs_do_symtab(uio, ctx, PROCFS_REQ_KALLSYMS);
}

/*
 * /proc/ide/drivers - the registered IDE (ATA/PATA) driver modules. macOS has no
 * IDE subsystem: internal storage is NVMe, and other block devices (AHCI/SATA,
 * USB, Thunderbolt) are handled through IOKit and surfaced by /proc/partitions
 * and /proc/diskstats. So there are no IDE drivers and this file is empty - and
 * /proc/ide itself has no channel (ideN) or drive (hdX) subdirectories - exactly
 * as on a Linux host with the IDE core present but no IDE hardware attached.
 */
int
procfs_doide_drivers(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_copy_data("", 0, uio);
}

/*
 * /proc/misc - Linux's registry of miscellaneous character devices: the drivers
 * that share the misc major (10), one "<minor> <name>" line each (rtc, hpet,
 * fuse, kvm, watchdog, ...). macOS has no misc-device framework, but it has many
 * miscellaneous single-purpose character devices (autofs, bpf, dtrace, fsevents,
 * klog, oslog, pf, auditpipe, ...). XNU keeps no in-kernel named-device registry
 * - device names live in devfs - so, as for /proc/devices and /proc/tty/drivers,
 * the procfsd daemon enumerates them from /dev (one row per driver family,
 * excluding the tty/disk/mem families that belong to other majors on Linux).
 * Empty without a connected daemon. Chunked transfer like /proc/devices.
 */
int
procfs_domisc(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_MISC, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon -> empty node */
        return (error == ENOTCONN) ? 0 : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/isapnp - Linux's ISA Plug-and-Play device listing (detected ISA PnP
 * cards and their logical devices/resources). The ISA bus is long obsolete and
 * macOS has no ISA or ISA-PnP support on any platform, so there are no ISA PnP
 * cards and this file is empty, as on a modern Linux host with no ISA hardware.
 */
int
procfs_doisapnp(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_copy_data("", 0, uio);
}

/*
 * /proc/scsi/scsi - Linux's attached-SCSI-device list. macOS SCSI-protocol
 * peripherals (USB/external/Thunderbolt storage through the SCSI Architecture
 * Model) are enumerated from IOKit by the procfsd daemon and formatted in the
 * Linux layout (Host/Channel/Id/Lun + Vendor/Model/Rev + Type). Internal NVMe is
 * not SCSI and is excluded. Without a connected daemon the node is empty (the
 * daemon always emits at least the "Attached devices:" header when connected).
 */
int
procfs_doscsi(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_SCSI, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon -> empty node */
        return (error == ENOTCONN) ? 0 : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/sysvipc/{msg,sem,shm} - the System V IPC object tables. macOS implements
 * SysV IPC (msgget/semget/shmget); it lacks the Linux SHM_STAT/MSG_STAT/SEM_STAT
 * index extensions, but the procfsd daemon enumerates the live objects via the
 * same kern.sysv.ipcs.{shm,sem,msg} sysctl that ipcs(1) uses and formats them in
 * the Linux layout (header + one line per object). Without a connected daemon
 * the node falls back to the header line only - which is also exactly what Linux
 * shows when no objects of that type exist. Headers match Linux's ipc sources.
 */
static int
procfs_sysvipc_node(uio_t uio, uint32_t req_type, const char *hdr, int hdrlen)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(req_type, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon: still show the header (an empty table), as Linux does. */
        return (error == ENOTCONN) ? procfs_copy_data(hdr, hdrlen, uio) : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

int
procfs_dosysvipc_shm(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "       key      shmid perms       size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime        rss       swap\n";
    return procfs_sysvipc_node(uio, PROCFS_REQ_SYSVIPC_SHM, hdr, sizeof(hdr) - 1);
}

int
procfs_dosysvipc_sem(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n";
    return procfs_sysvipc_node(uio, PROCFS_REQ_SYSVIPC_SEM, hdr, sizeof(hdr) - 1);
}

int
procfs_dosysvipc_msg(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n";
    return procfs_sysvipc_node(uio, PROCFS_REQ_SYSVIPC_MSG, hdr, sizeof(hdr) - 1);
}

/*
 * /proc/net/{tcp,tcp6,udp,udp6} - the Linux TCP/UDP connection tables. XNU has
 * no in-kernel KPI to enumerate the inpcb tables, so - as netstat(1) does in
 * userspace - the procfsd daemon reads them via the net.inet.{tcp,udp}.pcblist_n
 * sysctls and reformats each connection into the Linux per-line layout (see the
 * daemon's build_net_pcb_blob). Chunked transfer like the other daemon nodes;
 * without a connected daemon the node falls back to just the Linux header line,
 * which is what Linux shows when there are no sockets of that type.
 */
static int
procfs_net_pcb_node(uio_t uio, uint32_t req_type, const char *hdr, int hdrlen)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(req_type, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        /* No daemon: still show the header (an empty table), as Linux does. */
        return (error == ENOTCONN) ? procfs_copy_data(hdr, hdrlen, uio) : error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

int
procfs_donettcp(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETTCP, hdr, sizeof(hdr) - 1);
}

int
procfs_donettcp6(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETTCP6, hdr, sizeof(hdr) - 1);
}

int
procfs_donetudp(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETUDP, hdr, sizeof(hdr) - 1);
}

int
procfs_donetudp6(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETUDP6, hdr, sizeof(hdr) - 1);
}

/*
 * /proc/net/unix - the Linux unix-domain socket table. Same daemon path as the
 * inet tables, sourced from net.local.{stream,dgram}.pcblist_n; see the daemon's
 * build_net_unix_blob. Without a daemon the node shows just the header.
 */
int
procfs_donetunix(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "Num       RefCount Protocol Flags    Type St Inode Path\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETUNIX, hdr, sizeof(hdr) - 1);
}

/*
 * /proc/net/route - the Linux IPv4 routing table, from the daemon's PF_ROUTE
 * NET_RT_DUMP walk (build_net_route_blob). Without a daemon: just the header.
 */
int
procfs_donetroute(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETROUTE, hdr, sizeof(hdr) - 1);
}

/*
 * /proc/net/arp - the Linux IPv4 ARP table, from the daemon's PF_ROUTE
 * NET_RT_FLAGS/RTF_LLINFO walk (build_net_arp_blob). Without a daemon: header.
 */
int
procfs_donetarp(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char hdr[] =
        "IP address       HW type     Flags       HW address            Mask     Device\n";

    return procfs_net_pcb_node(uio, PROCFS_REQ_NETARP, hdr, sizeof(hdr) - 1);
}

/*
 * /proc/net/netstat - Linux extended TCP/IP stats (TcpExt/IpExt), rendered by
 * the daemon from net.inet.{tcp,ip}.stats (build_net_netstat_blob). The two-line
 * SNMP-style sections have no single header, so without a daemon the node is
 * empty (the daemon is the sole data source).
 */
int
procfs_donetnetstat(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_net_pcb_node(uio, PROCFS_REQ_NETNETSTAT, "", 0);
}

/*
 * /proc/net/snmp - the standard RFC-1213 SNMP MIB counters (Ip/Icmp/Tcp/Udp),
 * rendered by the daemon from net.inet.{ip,icmp,tcp,udp}.stats
 * (build_net_snmp_blob). Empty without a daemon (the sole data source).
 */
int
procfs_donetsnmp(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_net_pcb_node(uio, PROCFS_REQ_NETSNMP, "", 0);
}

/*
 * /proc/fs/nfs/exports - the NFS export table. Linux shows the kernel NFS
 * server's active exports here; macOS keeps the export configuration in
 * /etc/exports (which nfsd registers with the kernel), so the procfsd daemon
 * reads that file and streams it. A machine with no NFS server configured has no
 * /etc/exports, so the node is empty. Chunked transfer like /proc/devices.
 */
int
procfs_donfsexports(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_NFSEXPORTS, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/allocinfo - Linux memory-allocation profiling. Linux keys this on code
 * tags (file:line func:name); macOS has no code-tag profiling, so the closest
 * faithful source is the zone allocator (mach_zone_info, what zprint reports):
 * one row per zone with its live allocated bytes, live element count, and the
 * zone name in place of the code tag. Gathered by procfsd (mach_zone_info needs
 * the privileged host port) and streamed in chunks like /proc/devices.
 */
int
procfs_doallocinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    error = procfs_ctl_request_blob(PROCFS_REQ_ALLOCINFO, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/apm - Linux advanced-power-management line. macOS has no APM, so the
 * power/battery state comes from IOKit power sources (via the daemon) and is
 * mapped onto the classic single-line APM format:
 *   driver_ver bios_ver bios_flags ac_status batt_status batt_flag pct% time units
 * The AC/battery status and flag bytes follow the Linux apm-emulation encoding.
 */
int
procfs_doapm(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    struct procfs_apm_info info = { -1, 0, 0, -1, -1 };
    struct procfs_apm_info tmp;

    uint32_t got = 0;

    error = procfs_ctl_request(PROCFS_REQ_APM, 0, 0, &tmp, sizeof(tmp), &got);
    if (error != 0) {
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    if (got == sizeof(tmp)) {
        info = tmp;
    }

    int ac = (info.ac_online == 1) ? 0x01 : (info.ac_online == 0) ? 0x00 : 0xff;
    int pct = info.percentage, tmin = info.time_minutes;
    int bstatus, bflag;

    if (!info.battery_present) {
        bstatus = 0xff; bflag = 0x80;               /* no battery */
        pct = -1; tmin = -1;
    } else if (info.charging) {
        bstatus = 0x03; bflag = 0x08;               /* charging */
    } else if (pct > 50) {
        bstatus = 0x00; bflag = 0x01;               /* high */
    } else if (pct > 5) {
        bstatus = 0x01; bflag = 0x02;               /* low */
    } else {
        bstatus = 0x02; bflag = 0x04;               /* critical */
    }

    const char *units = (tmin >= 0) ? "min" : "?";

    char buf[128];
    int len = snprintf(buf, sizeof(buf),
        "1.16 1.2 0x02 0x%02x 0x%02x 0x%02x %d%% %d %s\n",
        ac, bstatus, bflag, pct, tmin, units);

    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/net/dev - Linux per-interface network statistics. The interface list
 * and counters come from the public ifnet KPIs (ifnet_list_get / ifnet_stat),
 * so this runs entirely in-kernel. macOS keeps a single dropped counter and no
 * fifo/frame/compressed/carrier counters, so those Linux columns are reported
 * as 0 and the drop count is attributed to the receive side (as FreeBSD's
 * linprocfs does with if_iqdrops).
 */
int
procfs_donetdev(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    ifnet_t *iflist = NULL;
    uint32_t count = 0;

    errno_t kerr   = ifnet_list_get(IFNET_FAMILY_ANY, &iflist, &count);
    if (kerr != 0) {
        return (int)kerr;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        ifnet_list_free(iflist);
        return ENOMEM;
    }

    sbuf_printf(&sb, "Inter-|   Receive                                                |"
                     "  Transmit\n");
    sbuf_printf(&sb, " face |bytes    packets errs drop fifo frame compressed multicast|"
                     "bytes    packets errs drop fifo colls carrier compressed\n");

    for (uint32_t i = 0; i < count; i++) {
        ifnet_t ifp = iflist[i];
        struct ifnet_stats_param st;
        bzero(&st, sizeof(st));
        (void)ifnet_stat(ifp, &st);

        char name[32];
        snprintf(name, sizeof(name), "%s%u", ifnet_name(ifp), ifnet_unit(ifp));

        sbuf_printf(&sb,
            "%6s:%8llu %7llu %4llu %4llu %4llu %4llu %10llu %9llu "
            "%8llu %7llu %4llu %4llu %4llu %5llu %7llu %10llu\n",
            name,
            /* Receive: bytes packets errs drop fifo frame compressed multicast */
            (unsigned long long)st.bytes_in,
            (unsigned long long)st.packets_in,
            (unsigned long long)st.errors_in,
            (unsigned long long)st.dropped,
            0ULL, 0ULL, 0ULL,
            (unsigned long long)st.multicasts_in,
            /* Transmit: bytes packets errs drop fifo colls carrier compressed */
            (unsigned long long)st.bytes_out,
            (unsigned long long)st.packets_out,
            (unsigned long long)st.errors_out,
            0ULL, 0ULL,
            (unsigned long long)st.collisions,
            0ULL, 0ULL);
    }
    ifnet_list_free(iflist);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/cmdline - the kernel boot command line (Linux's root /proc/cmdline,
 * distinct from the per-process /proc/<pid>/cmdline). On macOS this is the
 * boot-args string (sysctl kern.bootargs). Read in-kernel where permitted,
 * falling back to the procfsd sysctl bridge (kern.bootargs is not always
 * CTLFLAG_KERN-readable from a kext, so it can be empty in-kernel without a
 * connected daemon).
 */
int
procfs_dokcmdline(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    char raw[1024];
    size_t rawlen = sizeof(raw);

    if (sysctlbyname("kern.bootargs", raw, &rawlen, NULL, 0) != 0 || rawlen == 0) {
        uint32_t dlen = 0;
        if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, "kern.bootargs",
            raw, sizeof(raw), &dlen) == 0) {
            rawlen = dlen;
        } else {
            rawlen = 0;
        }
    }

    if (rawlen > 0 && raw[rawlen - 1] == '\0') {
        /* Drop the trailing NUL */
        rawlen--;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "%.*s\n", (int)rawlen, raw);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * /proc/bootconfig - Linux's boot-configuration view. Linux fills this from a
 * bootconfig blob appended to the initrd, plus a "# Parameters from bootloader:"
 * note carrying the original bootloader command line. macOS has no such blob:
 * its only boot configuration is the boot-args the boot loader (iBoot / NVRAM)
 * passes to the kernel (sysctl kern.bootargs, the same source as /proc/cmdline).
 * So the boot-args are emitted as the boot config, and - since on macOS the
 * kernel parameters come from the boot loader - repeated in the Linux
 * "# Parameters from bootloader:" form when present.
 */
int
procfs_dobootconfig(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;
    char raw[1024];
    size_t rawlen = sizeof(raw);

    if (sysctlbyname("kern.bootargs", raw, &rawlen, NULL, 0) != 0 || rawlen == 0) {
        uint32_t dlen = 0;
        if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, "kern.bootargs",
            raw, sizeof(raw), &dlen) == 0) {
            rawlen = dlen;
        } else {
            rawlen = 0;
        }
    }

    if (rawlen > 0 && raw[rawlen - 1] == '\0') {
        /* Drop the trailing NUL */
        rawlen--;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    if (rawlen > 0) {
        /* Boot config (the kernel command line), then the bootloader-parameters
         * note - on macOS both are the boot loader's boot-args. */
        sbuf_printf(&sb, "%.*s\n", (int)rawlen, raw);
        sbuf_printf(&sb, "# Parameters from bootloader:\n# %.*s\n", (int)rawlen, raw);
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

#pragma mark -
#pragma mark Presentation-mode switch (native vs Linux)

/*
 * Presentation mode, toggled live from userspace via the `procfs.linux` sysctl:
 *   sysctl -w procfs.linux=1   # Linux-compatible rendering
 *   sysctl -w procfs.linux=0   # native BSD/XNU rendering (default)
 *
 * Nodes with both renderings (regs/fpregs/auxv) check this; other nodes keep
 * their single format. The oids are registered/unregistered by the kext's
 * start/stop (a kext must register its sysctl oids explicitly - they are not
 * wired into the kernel's linker set).
 */
int procfs_linux_mode = 0;

/*
 * The `procfs.linux` oids are built by hand rather than with the SYSCTL_NODE /
 * SYSCTL_INT macros. Under XNU_KERNEL_PRIVATE (which the kext compiles with)
 * those macros expand to an in-kernel STARTUP auto-registration that references
 * sysctl_register_oid_early() - an internal symbol NOT exported to kexts, so the
 * kext would fail to bind and never load. Constructing the sysctl_oid structs
 * directly and registering them through the KPI sysctl_register_oid() avoids
 * that symbol entirely; omitting CTLFLAG_PERMANENT lets us remove them at unload.
 */
static struct sysctl_oid_list procfs_sysctl_children;

static struct sysctl_oid procfs_sysctl_node = {
    .oid_parent  = &sysctl__children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_NODE | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &procfs_sysctl_children,
    .oid_arg2    = 0,
    .oid_name    = "procfs",
    .oid_handler = NULL,
    .oid_fmt     = "N",
    .oid_descr   = "procfs filesystem",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid procfs_sysctl_linux = {
    .oid_parent  = &procfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &procfs_linux_mode,
    .oid_arg2    = 0,
    .oid_name    = "linux",
    .oid_handler = sysctl_handle_int,
    .oid_fmt     = "I",
    .oid_descr   = "procfs presentation mode: 0 = native BSD/XNU, 1 = Linux-compatible",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid procfs_sysctl_linux_version = {
    .oid_parent  = &procfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &procfs_linux_version,
    .oid_arg2    = 0,
    .oid_name    = "linux_version",
    .oid_handler = sysctl_handle_int,
    .oid_fmt     = "I",
    .oid_descr   = "spoofed Linux kernel version: 0 = none (Darwin), 1..N = preset release",
    .oid_version = SYSCTL_OID_VERSION,
};

void
procfs_sysctl_register(void)
{
    sysctl_register_oid(&procfs_sysctl_node);   /* parent first */
    sysctl_register_oid(&procfs_sysctl_linux);
    sysctl_register_oid(&procfs_sysctl_linux_version);
}

void
procfs_sysctl_unregister(void)
{
    sysctl_unregister_oid(&procfs_sysctl_linux_version);
    sysctl_unregister_oid(&procfs_sysctl_linux);
    sysctl_unregister_oid(&procfs_sysctl_node);
}