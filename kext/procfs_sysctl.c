/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_sysctl.c
 *
 * Dynamic /proc/sys tree: a live mirror of the kernel's sysctl MIB, the macOS
 * counterpart to Linux's /proc/sys. Rather than static structure nodes, every
 * /proc/sys vnode is an instance of the single PFSsysctl structure node,
 * distinguished by its node id's objectid, which holds the kernel address of the
 * corresponding `struct sysctl_oid` (objectid 0 == the tree root,
 * `sysctl__children`). A NODE oid is a directory; a leaf is a file whose read
 * builds the dotted MIB name and fetches the value with sysctlbyname().
 *
 * This file owns all knowledge of the sysctl_oid internals; the vnops layer
 * drives it through the small interface declared in procfs.h.
 */
#include <stdint.h>
#include <string.h>

#include <libkern/libkern.h>

#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <uuid/uuid.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/*
 * In-kernel sysctl-by-name (not prototyped in the kext's sysctl.h view).
 */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);

/*
 * ----- Synthetic /proc/sys nodes -----
 *
 * A few Linux /proc/sys paths have no macOS sysctl MIB behind them; we
 * synthesize those here. A synthetic node is identified by a small sentinel
 * objectid rather than a sysctl_oid address: real objectids are sysctl_oid
 * kernel pointers (always high addresses) and 0 is the tree root, so the low
 * sentinels below never collide. Every procfs_sysctl_* entry point checks for
 * these before treating an objectid as an oid pointer.
 *
 * Today this is only /proc/sys/kernel/random and its leaves. On Linux those are
 * files served by the random driver, not real sysctls; macOS has no equivalent
 * MIB node, so we render Linux-compatible values from the platform's own
 * facilities (kern.bootsessionuuid, uuid_generate_random).
 */
enum {
    PROCFS_SYN_RANDOM = 1,          /* kernel/random/              (directory) */
    PROCFS_SYN_RANDOM_BOOT_ID,      /* kernel/random/boot_id       (file) */
    PROCFS_SYN_RANDOM_ENTROPY,      /* kernel/random/entropy_avail (file) */
    PROCFS_SYN_RANDOM_POOLSIZE,     /* kernel/random/poolsize      (file) */
    PROCFS_SYN_RANDOM_UUID,         /* kernel/random/uuid          (file) */
    PROCFS_SYN_MAX
};

#define PROCFS_SYN_IS(id)   ((id) >= PROCFS_SYN_RANDOM && (id) < PROCFS_SYN_MAX)

/*
 * Children of /proc/sys/kernel/random, in readdir order (Linux lists them
 * alphabetically).
 */
static const struct {
    const char *name;
    uint64_t    objectid;
} procfs_random_leaves[] = {
    { "boot_id",       PROCFS_SYN_RANDOM_BOOT_ID },
    { "entropy_avail", PROCFS_SYN_RANDOM_ENTROPY },
    { "poolsize",      PROCFS_SYN_RANDOM_POOLSIZE },
    { "uuid",          PROCFS_SYN_RANDOM_UUID },
};

#define PROCFS_RANDOM_LEAF_COUNT \
    (sizeof(procfs_random_leaves) / sizeof(procfs_random_leaves[0]))

/*
 * Linux reports the entropy pool as 256 bits and, on modern kernels, always
 * full; we report the same fixed values for poolsize and entropy_avail.
 */
#define PROCFS_RANDOM_POOLSIZE_BITS 256

/*
 * Objectid of the top-level "kern" oid (Linux's "kernel"), under which the
 * synthetic random directory hangs. Resolved by walking the root child list -
 * cheap, and only reached on the cold lookup/readdir paths.
 */
static uint64_t
procfs_sysctl_kern_objectid(void)
{
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, &sysctl__children, oid_link) {
        if (oid->oid_name != NULL && strcmp(oid->oid_name, "kern") == 0) {
            return (uint64_t)oid;
        }
    }
    return 0;
}

/*
 * Render /proc/sys/kernel/random/boot_id: this boot's UUID, taken from
 * kern.bootsessionuuid and lowercased to match Linux's format. If that sysctl
 * cannot be read (e.g. no daemon and it is not a KERN oid), fall back to a
 * freshly generated UUID so the node still yields a well-formed value.
 * Returns the formatted length.
 */
static int
procfs_random_boot_id(char *out, size_t outsz)
{
    char     raw[64];
    size_t   rawlen = 0;
    uint32_t dlen   = 0;

    if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, "kern.bootsessionuuid",
            (uint8_t *)raw, sizeof(raw), &dlen) == 0 && dlen > 0) {
        rawlen = dlen;
    } else {
        rawlen = sizeof(raw);
        if (sysctlbyname("kern.bootsessionuuid", raw, &rawlen, NULL, 0) != 0) {
            rawlen = 0;
        }
    }

    /* Drop any trailing NUL/newline the sysctl included. */
    while (rawlen > 0 && (raw[rawlen - 1] == '\0' || raw[rawlen - 1] == '\n')) {
        rawlen--;
    }

    if (rawlen == 0) {
        uuid_t        u;
        uuid_string_t s;
        uuid_generate_random(u);
        uuid_unparse_lower(u, s);
        return snprintf(out, outsz, "%s\n", s);
    }

    /* macOS reports the UUID uppercase; Linux boot_id is lowercase. */
    for (size_t i = 0; i < rawlen; i++) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') {
            raw[i] = (char)(c - 'A' + 'a');
        }
    }
    return snprintf(out, outsz, "%.*s\n", (int)rawlen, raw);
}

/*
 * Read a synthetic leaf as Linux-style text. The random directory itself is
 * EISDIR; unknown ids are ENOENT.
 */
static int
procfs_sysctl_read_synthetic(uint64_t objectid, uio_t uio)
{
    char out[64];
    int  len = 0;

    switch (objectid) {
    case PROCFS_SYN_RANDOM:
        return EISDIR;

    case PROCFS_SYN_RANDOM_BOOT_ID:
        len = procfs_random_boot_id(out, sizeof(out));
        break;

    case PROCFS_SYN_RANDOM_UUID: {
        /* A new random UUID on every read, as on Linux. */
        uuid_t        u;
        uuid_string_t s;
        uuid_generate_random(u);
        uuid_unparse_lower(u, s);
        len = snprintf(out, sizeof(out), "%s\n", s);
        break;
    }

    case PROCFS_SYN_RANDOM_ENTROPY:     /* FALLTHROUGH - both are the pool size */
    case PROCFS_SYN_RANDOM_POOLSIZE:
        len = snprintf(out, sizeof(out), "%d\n", PROCFS_RANDOM_POOLSIZE_BITS);
        break;

    default:
        return ENOENT;
    }

    return procfs_copy_data(out, len, uio);
}

/*
 * The children list for a directory objectid: the tree root (objectid 0) maps to
 * sysctl__children; a NODE oid maps to its child list (oid_arg1); anything else
 * (a leaf) has no children.
 */
static struct sysctl_oid_list *
procfs_sysctl_children(uint64_t objectid)
{
    if (objectid == 0) {
        return &sysctl__children;
    }

    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;
    if ((oid->oid_kind & CTLTYPE) != CTLTYPE_NODE) {
        return NULL;
    }

    return (struct sysctl_oid_list *)oid->oid_arg1;
}

/*
 * Internal oids not shown in the listing: nameless entries, deprecated/masked
 * variables, and the mutable-anchor markers (__anchor__(...)).
 */
static boolean_t
procfs_sysctl_hidden(struct sysctl_oid *oid)
{
    return oid->oid_name == NULL
        || (oid->oid_kind & CTLFLAG_MASKED) != 0
        || oid->oid_number == OID_MUTABLE_ANCHOR;
}

/*
 * True if the node is a directory (the tree root, or a CTLTYPE_NODE oid).
 */
boolean_t
procfs_sysctl_is_node(uint64_t objectid)
{
    if (objectid == 0) {
        return TRUE;
    }
    if (PROCFS_SYN_IS(objectid)) {
        return objectid == PROCFS_SYN_RANDOM;   /* only the random dir is a node */
    }

    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;
    return (oid->oid_kind & CTLTYPE) == CTLTYPE_NODE;
}

/*
 * Find a named child of a directory objectid; sets *out_objectid on success.
 */
boolean_t
procfs_sysctl_find(uint64_t dir_objectid, const char *name, uint64_t *out_objectid)
{
    /*
     * Linux-compatibility alias: Linux groups these sysctls under /proc/sys/kernel
     * while macOS names the top MIB node "kern". At the /proc/sys root (objectid 0)
     * resolve "kernel" to the "kern" node so Linux paths like
     * /proc/sys/kernel/ostype work. macOS has no real "kernel" top-level node, so
     * this shadows nothing; it resolves to the same oid as "kern", so reads (and
     * the version-spoof interception, which derives the MIB name from the oid)
     * behave identically to the /proc/sys/kern path.
     */
    if (dir_objectid == 0 && strcmp(name, "kernel") == 0) {
        name = "kern";
    }

    /*
     * Synthetic directory: resolve /proc/sys/kernel/random's leaves by name.
     * Handled before procfs_sysctl_children, which would misread the sentinel
     * objectid as an oid pointer.
     */
    if (dir_objectid == PROCFS_SYN_RANDOM) {
        for (size_t i = 0; i < PROCFS_RANDOM_LEAF_COUNT; i++) {
            if (strcmp(name, procfs_random_leaves[i].name) == 0) {
                *out_objectid = procfs_random_leaves[i].objectid;
                return TRUE;
            }
        }
        return FALSE;
    }

    struct sysctl_oid_list *list = procfs_sysctl_children(dir_objectid);
    if (list == NULL) {
        return FALSE;
    }

    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (!procfs_sysctl_hidden(oid) && strcmp(oid->oid_name, name) == 0) {
            *out_objectid = (uint64_t)oid;
            return TRUE;
        }
    }

    /*
     * Graft the synthetic random directory in as a child of kern (Linux
     * "kernel"). Ordered so the kern-oid lookup only runs for the "random" name.
     */
    if (strcmp(name, "random") == 0
        && dir_objectid == procfs_sysctl_kern_objectid()) {
        *out_objectid = PROCFS_SYN_RANDOM;
        return TRUE;
    }

    return FALSE;
}

/*
 * Enumerate a directory's children by index (for readdir): fills *name,
 * *is_node and *objectid for the child at `index`. Returns 1 if present, 0 if
 * the index is past the end. The SLIST order is stable except across kext
 * load/unload, matching readdir's "content may change between calls" contract.
 */
int
procfs_sysctl_child_at(uint64_t dir_objectid, int index,
    const char **name, boolean_t *is_node, uint64_t *objectid)
{
    /*
     * Synthetic directory: enumerate /proc/sys/kernel/random's leaves. Handled
     * before procfs_sysctl_children, which expects an oid-pointer objectid.
     */
    if (dir_objectid == PROCFS_SYN_RANDOM) {
        if (index < 0 || (size_t)index >= PROCFS_RANDOM_LEAF_COUNT) {
            return 0;
        }
        *name     = procfs_random_leaves[index].name;
        *is_node  = FALSE;
        *objectid = procfs_random_leaves[index].objectid;
        return 1;
    }

    struct sysctl_oid_list *list = procfs_sysctl_children(dir_objectid);
    if (list == NULL) {
        return 0;
    }
    int i = 0;
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (procfs_sysctl_hidden(oid)) {
            continue;
        }
        if (i == index) {
            *name     = oid->oid_name;
            *is_node  = (oid->oid_kind & CTLTYPE) == CTLTYPE_NODE;
            *objectid = (uint64_t)oid;
            return 1;
        }
        i++;
    }

    /*
     * Append the synthetic random directory as kern's last child, at the index
     * just past its real children.
     */
    if (index == i && dir_objectid == procfs_sysctl_kern_objectid()) {
        *name     = "random";
        *is_node  = TRUE;
        *objectid = PROCFS_SYN_RANDOM;
        return 1;
    }
    return 0;
}

/*
 * Recover the full dotted MIB name of `target` by DFS from the tree root, since
 * a sysctl_oid has no back-pointer to its parent oid. The tree is shallow, so
 * this is cheap enough for the (cold) read path.
 */
static boolean_t
procfs_sysctl_build_name(struct sysctl_oid_list *list, const char *prefix,
    uint64_t target, char *out, size_t outsz)
{
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (oid->oid_name == NULL) {
            continue;
        }

        char path[256];
        if (prefix[0] != '\0') {
            snprintf(path, sizeof(path), "%s.%s", prefix, oid->oid_name);
        } else {
            strlcpy(path, oid->oid_name, sizeof(path));
        }

        if ((uint64_t)oid == target) {
            strlcpy(out, path, outsz);
            return TRUE;
        }

        if ((oid->oid_kind & CTLTYPE) == CTLTYPE_NODE && oid->oid_arg1 != NULL) {
            if (procfs_sysctl_build_name((struct sysctl_oid_list *)oid->oid_arg1,
                    path, target, out, outsz)) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*
 * DFS helper: find `target`'s parent oid, carrying the current parent id down
 * the recursion (0 for the tree root == /proc/sys).
 */
static boolean_t
procfs_sysctl_find_parent(struct sysctl_oid_list *list, uint64_t parent_id,
    uint64_t target, uint64_t *out_parent)
{
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if ((uint64_t)oid == target) {
            *out_parent = parent_id;
            return TRUE;
        }

        if ((oid->oid_kind & CTLTYPE) == CTLTYPE_NODE && oid->oid_arg1 != NULL) {
            if (procfs_sysctl_find_parent((struct sysctl_oid_list *)oid->oid_arg1,
                    (uint64_t)oid, target, out_parent)) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*
 * Resolve the parent directory objectid of `objectid` for ".." traversal. Sets
 * *parent_objectid to the enclosing oid, or 0 when the parent is the /proc/sys
 * root (a top-level oid). Returns FALSE for the root itself (no sysctl parent),
 * leaving ".." to fall through to the /proc root.
 */
boolean_t
procfs_sysctl_parent(uint64_t objectid, uint64_t *parent_objectid)
{
    if (objectid == 0) {
        *parent_objectid = 0;
        return FALSE;
    }

    /*
     * Synthetic nodes: the random directory's parent is kern, and each random
     * leaf's parent is the random directory. procfs_sysctl_find_parent walks
     * only real oids and would never find these.
     */
    if (objectid == PROCFS_SYN_RANDOM) {
        *parent_objectid = procfs_sysctl_kern_objectid();
        return TRUE;
    }
    if (PROCFS_SYN_IS(objectid)) {
        *parent_objectid = PROCFS_SYN_RANDOM;
        return TRUE;
    }

    return procfs_sysctl_find_parent(&sysctl__children, 0, objectid, parent_objectid);
}

/*
 * Read a leaf's value as Linux-style text. Builds the MIB name, fetches the raw
 * value, and formats it by the oid's declared type (int / quad / string).
 * Opaque/struct sysctls have no text rendering and read empty. A directory
 * objectid returns EISDIR.
 *
 * The raw value comes from the procfsd bridge (userspace sysctlbyname, which
 * serves every oid), falling back to in-kernel sysctlbyname() when no daemon is
 * connected. The kernel path only serves oids marked CTLFLAG_KERN; others (e.g.
 * kern.hostname, kern.maxproc) read empty without the daemon. Invoking the oid
 * handler in-kernel to bypass that gate is unsafe — the custom handlers assume
 * the sysctl lock is held and panic — so the daemon is the path to full coverage.
 */
int
procfs_sysctl_read(uint64_t objectid, uio_t uio)
{
    if (PROCFS_SYN_IS(objectid)) {
        return procfs_sysctl_read_synthetic(objectid, uio);
    }
    if (procfs_sysctl_is_node(objectid)) {
        return EISDIR;
    }
    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;

    char name[PROCFS_CTL_NAMEMAX];
    name[0] = '\0';
    if (!procfs_sysctl_build_name(&sysctl__children, "", objectid, name, sizeof(name))) {
        return ENOENT;
    }

    /*
     * When a Linux kernel version is being spoofed, the OS-identity sysctls
     * report a Linux identity so /proc/sys/kernel/{ostype,osrelease,version}
     * agree with /proc/version.
     */
    const char *rel = procfs_spoofed_release();
    if (rel != NULL) {
        char sbuf[512];
        if (strcmp(name, "kern.ostype") == 0) {
            return procfs_copy_data("Linux\n", 6, uio);
        }
        if (strcmp(name, "kern.osrelease") == 0) {
            int n = snprintf(sbuf, sizeof(sbuf), "%s\n", rel);
            return procfs_copy_data(sbuf, n, uio);
        }
        if (strcmp(name, "kern.version") == 0) {
            int n = procfs_build_linux_version(sbuf, sizeof(sbuf));
            return procfs_copy_data(sbuf, n, uio);
        }
    }

    uint8_t raw[1024];
    size_t  rawlen = 0;

    /*
     * Prefer the daemon (covers all oids); fall back to the in-kernel path.
     */
    uint32_t dlen = 0;
    if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, name,
        raw, sizeof(raw), &dlen) == 0) {
        rawlen = dlen;
    } else {
        rawlen = sizeof(raw);
        if (sysctlbyname(name, raw, &rawlen, NULL, 0) != 0) {
            rawlen = 0;
        }
    }

    if (rawlen == 0) {
        /*
         * unreadable/non-KERN/empty
         */
        return procfs_copy_data("", 0, uio);
    }

    char out[1100];
    int  len  = 0;
    int  type = oid->oid_kind & CTLTYPE;
    const char *fmt = (oid->oid_fmt != NULL) ? oid->oid_fmt : "";

    switch (type) {
    case CTLTYPE_STRING: {
        if (raw[rawlen - 1] == '\0') {
            rawlen--;                              /* drop the trailing NUL */
        }
        len = snprintf(out, sizeof(out), "%.*s\n", (int)rawlen, (char *)raw);
        break;
    }
    case CTLTYPE_INT: {
        if (rawlen >= sizeof(int32_t)) {
            int32_t v;
            memcpy(&v, raw, sizeof(v));
            len = (fmt[0] == 'I' && fmt[1] == 'U')
                ? snprintf(out, sizeof(out), "%u\n", (uint32_t)v)
                : snprintf(out, sizeof(out), "%d\n", v);
        }
        break;
    }
    case CTLTYPE_QUAD: {
        if (rawlen >= sizeof(int64_t)) {
            int64_t v;
            memcpy(&v, raw, sizeof(v));
            len = (fmt[0] == 'Q' && fmt[1] == 'U')
                ? snprintf(out, sizeof(out), "%llu\n", (unsigned long long)v)
                : snprintf(out, sizeof(out), "%lld\n", (long long)v);
        }
        break;
    }
    default:
        return procfs_copy_data("", 0, uio);        /* opaque/struct: no text */
    }

    return procfs_copy_data(out, len, uio);
}
