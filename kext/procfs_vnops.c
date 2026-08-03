/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_vnops.c
 *
 * Vnode operations for the ProcFS file system.
 */
#include <libkern/libkern.h>

#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/stat.h>
#include <sys/vm.h>
#include <sys/vnode.h>

#include <vfs/vfs_support.h>

#include <fs/procfs/procfs.h>

#pragma mark -
#pragma mark Local Definitions

/*
 * Read and execute permissions for all users.
 */
#define READ_EXECUTE_ALL (S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

/*
 * Read permission for all users.
 */
#define READ_ALL (S_IRUSR|S_IRGRP|S_IROTH)

/*
 * Read, write and execute permissions for everone.
 */
#define ALL_ACCESS_ALL (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_IROTH|S_IWOTH|S_IXOTH)

/*
 * Read/write for owner and group - the writable "note" node.
 */
#define RW_OWNER_GROUP (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)

/*
 * Vnode Operations Function Descriptor
 */
#define VOPFUNC int (*)(void *)

/*
 * Structure used to hold the values needed to create a new vnode
 * corresponding to a pfsnode_t.
 */
typedef struct {
    vnode_t vca_parentvp; // Parent vnode.
} procfs_vnode_create_args;

/*
 * Size of a buffer large enough to hold the string form of a pid_t
 * plus the command name for a process and some padding
 */
static const int PAD_SIZE = 8;
static const int PID_SIZE = 16;
static const int PROCESS_NAME_SIZE = MAXCOMLEN + PID_SIZE + PAD_SIZE;

#pragma mark -
#pragma mark Function Prototypes

STATIC int procfs_vnop_default(struct vnop_generic_args *arg);
STATIC int procfs_vnop_lookup(struct vnop_lookup_args *ap);
STATIC int procfs_vnop_getattr(struct vnop_getattr_args *ap);
STATIC int procfs_vnop_reclaim(struct vnop_reclaim_args *ap);
STATIC int procfs_vnop_readdir(struct vnop_readdir_args *ap);
STATIC int procfs_vnop_getattrlistbulk(struct vnop_getattrlistbulk_args *ap);
STATIC int procfs_vnop_readlink(struct vnop_readlink_args *ap);
STATIC int procfs_vnop_read(struct vnop_read_args *ap);
STATIC int procfs_vnop_write(struct vnop_write_args *ap);
STATIC int procfs_vnop_setattr(struct vnop_setattr_args *ap);
STATIC int procfs_vnop_open(struct vnop_open_args *ap);
STATIC int procfs_vnop_close(struct vnop_close_args *ap);
STATIC int procfs_vnop_access(struct vnop_access_args *ap);
STATIC int procfs_vnop_inactive(struct vnop_inactive_args *ap);

STATIC inline int procfs_calc_dirent_size(const char *name);
STATIC int procfs_copyout_dirent(int type, uint64_t file_id, const char *name, uio_t uio, int *sizep, off_t seekoff);
STATIC int procfs_sysctl_readdir(struct vnop_readdir_args *ap);
STATIC int procfs_create_vnode(procfs_vnode_create_args *cap, pfsnode_t *pnp, vnode_t *vpp);
STATIC void procfs_construct_process_dir_name(proc_t p, char *buffer);

#pragma mark -
#pragma mark Vnode Operations Structures

/*
 * Pointer to the constructed vnode operations vector. Set
 * when the file system is registered and used when creating
 * vnodes.
 */
int (**procfs_vnodeop_p)(void *);

/*
 * Entries for the vnode operations that this file system supports.
 * This table is converted to a fully-populated vnode operations
 * vector when procfs is registered as a file system and a pointer
 * to that vector is stored in procfs_vnodeop_p.
 */
struct vnodeopv_entry_desc procfs_vnodeop_entries[] = {
    { .opve_op = &vnop_default_desc,            .opve_impl = (VOPFUNC) procfs_vnop_default },           /* default */
    { .opve_op = &vnop_lookup_desc,             .opve_impl = (VOPFUNC) procfs_vnop_lookup },            /* lookup */
    { .opve_op = &vnop_open_desc,               .opve_impl = (VOPFUNC) procfs_vnop_open },              /* open */
    { .opve_op = &vnop_close_desc,              .opve_impl = (VOPFUNC) procfs_vnop_close },             /* close */
    { .opve_op = &vnop_access_desc,             .opve_impl = (VOPFUNC) procfs_vnop_access },            /* access */
    { .opve_op = &vnop_getattr_desc,            .opve_impl = (VOPFUNC) procfs_vnop_getattr },           /* getattr */
    { .opve_op = &vnop_setattr_desc,            .opve_impl = (VOPFUNC) procfs_vnop_setattr },           /* setattr (note O_TRUNC) */
    { .opve_op = &vnop_read_desc,               .opve_impl = (VOPFUNC) procfs_vnop_read },              /* read */
    { .opve_op = &vnop_write_desc,              .opve_impl = (VOPFUNC) procfs_vnop_write },             /* write (note node) */
    { .opve_op = &vnop_readdir_desc,            .opve_impl = (VOPFUNC) procfs_vnop_readdir },           /* readdir */
    { .opve_op = &vnop_readdirattr_desc,        .opve_impl = (VOPFUNC) err_readdirattr },               /* readdirattr -> ENOTSUP, forces fallback to getdirentries64 */
    { .opve_op = &vnop_getattrlistbulk_desc,    .opve_impl = (VOPFUNC) procfs_vnop_getattrlistbulk },   /* getattrlistbulk -> ENOTSUP, forces fallback to readdir+getattr */
    { .opve_op = &vnop_readlink_desc,           .opve_impl = (VOPFUNC) procfs_vnop_readlink },          /* readlink */
    { .opve_op = &vnop_inactive_desc,           .opve_impl = (VOPFUNC) procfs_vnop_inactive },          /* inactive */
    { .opve_op = &vnop_reclaim_desc,            .opve_impl = (VOPFUNC) procfs_vnop_reclaim },           /* reclaim */
    { .opve_op = (struct vnodeop_desc*)NULL,    .opve_impl = (int (*)(void *))NULL }
};

/*
 * Descriptor used to create the vnode operations vector for
 * procfs from procfs_vnodeop_entries. Entries for operations that
 * we do not support will get appropriate defaults.
 */
struct vnodeopv_desc procfs_vnodeop_opv_desc = {
    .opv_desc_vector_p  = &procfs_vnodeop_p,
    .opv_desc_ops       = procfs_vnodeop_entries
};

/*
 * List of descriptors used to build vnode operations
 * vectors. Since we only have one set of vnode operations,
 * there is only one descriptor.
 */
struct vnodeopv_desc *procfs_vnodeops_list[1] = {
    &procfs_vnodeop_opv_desc,
};

#pragma mark -
#pragma mark Vnode Operations

/*
 * Vnode operations that don't require us to do anything.
 */
STATIC int
procfs_vnop_open(__unused struct vnop_open_args *ap)
{
    return 0;
}


STATIC int
procfs_vnop_access(__unused struct vnop_access_args *ap)
{
    return 0;
}

STATIC
int procfs_vnop_close(__unused struct vnop_close_args *ap)
{
    return 0;
}

STATIC
int procfs_vnop_inactive(__unused struct vnop_inactive_args *ap)
{
    /*
     * We do everything in procfs_vnop_reclaim.
     */
    return 0;
}

STATIC
int procfs_vnop_default(__unused struct vnop_generic_args *arg)
{
    return 0;
}

/*
 * We do not implement bulk attribute enumeration. Returning ENOTSUP here
 * overrides XNU's default getattrlistbulk implementation (which mis-packs
 * our entries and fails with ERANGE), causing callers such as ls(1)/fts(3)
 * to fall back to plain VNOP_READDIR + VNOP_GETATTR, which work correctly.
 */
STATIC int
procfs_vnop_getattrlistbulk(__unused struct vnop_getattrlistbulk_args *ap)
{
    return ENOTSUP;
}

/*
 * Vnode lookup, called when resolving a path. Each invocation of this
 * function requires us to resolve one level of path name and return
 * either an error or the vnode that corresponds to it, with an
 * additional iocount that must eventually be released by the caller.
 *
 * When asked to resolve a path, we are given the vnode of the 
 * path's directory and the path segment. The vnode will map to a
 * pfsnode_t, which we can use to get its pfssnode_t.
 * Once we have the pfssnode_t, we know which level we 
 * are at in the file system and therefore which paths are valid.
 * In some cases, we can resolve the lookup by a simple comparison
 * of the path name with the name of a pfssnode_t. As
 * an exmaple, if the parent node is for a process, then the node
 * structure tells us that names like "ppid", "pgid" etc are valid.
 * In other cases, we have to do more work. In the root directory,
 * for example, most of the valid names are process ids, so we have
 * to check whether the name component is numeric and whether it
 * corresponds to an active process.
 * 
 * The end result of the name check will be a pfssnode_t.
 * From that, we can construct the node id of the node that the name
 * refers to and we can then use that to look up the vnode in the
 * vnode cache and create it if it's not there.
 */
STATIC int
procfs_vnop_lookup(struct vnop_lookup_args *ap)
{
    char name[NAME_MAX + 1];
    int error = 0;
    struct componentname *cnp = ap->a_cnp;
    vnode_t dvp = ap->a_dvp; // Parent of the name to be looked up

    /*
     * The parent directory must not be NULL and the name
     * length must be at least 1.
     */
    if (dvp == NULLVP || vnode_vtype(dvp) != VDIR || cnp->cn_namelen < 1) {
        error = EINVAL;
        goto out;
    }

    /*
     * Get the pfsnode_t for the directory. This must
     * not be NULL.
     */
    pfsnode_t *dir_pnp = VTOPFS(dvp);
    if (dir_pnp == NULL) {
        error = EINVAL;
        goto out;
    }

    /*
     * Preparation: get the component that we are looking up, clear
     * the returned vnode and ensure that nothing is added to the
     * name cache.
     */
    strlcpy(name, cnp->cn_nameptr, min(sizeof(name), cnp->cn_namelen + 1));
    cnp->cn_flags &= ~MAKEENTRY;
    *ap->a_vpp = NULLVP;

    /*
     * ProcFS file system mount.
     */
    pfsmount_t *mp = vfs_mp_to_procfs_mp(vnode_mount(dvp));
    
    if (cnp->cn_flags & ISDOTDOT) {
        /*
         * We need the parent of the directory in "dvp". Get that by figuring out what its
         * node id would be.
         */
        pfsid_t parent_node_id;
        pfssnode_t *parent_snode = dir_pnp->node_structure_node->psn_parent;

        if (dir_pnp->node_structure_node->psn_node_type == PFSsysctl &&
            dir_pnp->node_id.nodeid_objectid != 0) {
            /*
             * A nested /proc/sys directory: its parent is the enclosing sysctl
             * oid (or the /proc/sys root, objectid 0), backed by the same shared
             * PFSsysctl structure node - not that node's static parent (root).
             */
            uint64_t parent_oid = 0;
            (void)procfs_sysctl_parent(dir_pnp->node_id.nodeid_objectid, &parent_oid);
            parent_node_id.nodeid_base_id  = dir_pnp->node_structure_node->psn_base_node_id;
            parent_node_id.nodeid_pid      = PRNODE_NO_PID;
            parent_node_id.nodeid_objectid = parent_oid;
            parent_snode = dir_pnp->node_structure_node;
        } else {
            procfs_get_parent_node_id(dir_pnp, &parent_node_id);
        }

        pfsnode_t *target_pfsnode;
        vnode_t target_vnode;
        procfs_vnode_create_args create_args;
        create_args.vca_parentvp = NULLVP;

        error = procfsnode_find(mp, parent_node_id,
                                parent_snode,
                                &target_pfsnode,
                                &target_vnode,
                                (create_vnode_func)&procfs_create_vnode,
                                &create_args);
        if (error == 0) {
            *ap->a_vpp = target_vnode;
        }
    } else if (cnp->cn_namelen == 1 && name[0] == '.') {
        /*
         * Looking for the current directory, so return
         * "dvp" with an extra iocount reference.
         */
        error = vnode_get(dvp);
        *ap->a_vpp = dvp;
    } else {
        /*
         * For all other cases, we try to match the name component
         * against the child nodes of the directory's structure node.
         * If we find a process or thread  structure node, we try to
         * convert the name to an integer and match if successful.
         */
        pfssnode_t *dir_snode = dir_pnp->node_structure_node;
        pfssnode_t *match_node = NULL;
        pfsid_t match_node_id;
        proc_t target_proc = PROC_NULL;

        /*
         * /proc/sys is a dynamic mirror of the sysctl tree: its children are the
         * live sysctl oids, not static structure children. Match by name against
         * the oid's children and reuse the single PFSsysctl structure node,
         * distinguished by the matched oid's address in the objectid.
         */
        if (dir_snode->psn_node_type == PFSsysctl) {
            uint64_t child_objectid = 0;
            if (procfs_sysctl_find(dir_pnp->node_id.nodeid_objectid, name, &child_objectid)) {
                match_node = dir_snode;
                match_node_id.nodeid_base_id  = dir_snode->psn_base_node_id;
                match_node_id.nodeid_pid      = PRNODE_NO_PID;
                match_node_id.nodeid_objectid = child_objectid;
            }
            goto sysctl_matched;
        }

        TAILQ_FOREACH(match_node, &dir_snode->psn_children, psn_next) {
            assert(error == 0);
            pfstype node_type = match_node->psn_node_type;
            /*
             * A version-spoofed release hides ksyms or kallsyms; a hidden node
             * must not resolve (looks up as ENOENT), matching its absence in
             * readdir.
             */
            if (procfs_node_version_hidden(match_node->psn_name)) {
                continue;
            }
            if (strcmp(name, match_node->psn_name) == 0) {
                /*
                 * Name matched. This is the droid we are looking for. Construct the
                 * node_id from the matched node and the pid and object id of the
                 * parent directory.
                 */
                match_node_id.nodeid_base_id = match_node->psn_base_node_id;
                match_node_id.nodeid_pid = dir_pnp->node_id.nodeid_pid;
                match_node_id.nodeid_objectid = dir_pnp->node_id.nodeid_objectid;
                break;
            } else if (node_type == PFSfd) {
                /*
                 * Entries in this directory must be numeric and must correspond to
                 * an open file descriptor in the process.
                 */
                const char *endp;
                int id = procfs_atoi(name, &endp);
                boolean_t valid = FALSE;
                if (id != -1) {
                    /*
                     * Check whether it is a valid file descriptor by looking it
                     * up in the process's fd list.
                     */
                    target_proc = proc_find(dir_pnp->node_id.nodeid_pid);
                    if (target_proc != PROC_NULL) { // target_proc is released at loop end.
                        struct proc_fdinfo *fdlist = NULL;
                        size_t fd_count = 0;
                        if (procfs_get_fd_list(target_proc, &fdlist, &fd_count) == 0) {
                            for (size_t k = 0; k < fd_count; k++) {
                                if (fdlist[k].proc_fd == id) {
                                    valid = TRUE;
                                    break;
                                }
                            }
                        }
                        procfs_release_fd_list(fdlist);
                    }
                }

                if (valid) {
                    /*
                     * Construct the node id from the process id and file number.
                     */
                    match_node_id.nodeid_base_id = match_node->psn_base_node_id;
                    match_node_id.nodeid_pid = dir_pnp->node_id.nodeid_pid;
                    match_node_id.nodeid_objectid = id;
                } else {
                    error = ENOENT;
                }
                break;
            } else if (node_type == PFSproc || node_type == PFSprocnamedir
                       || node_type == PFSthread) {
                /*
                 * Process or thread directory entry marker. For PFSproc and
                 * PFSthread, this can match only if "name" is a valid integer.
                 * For PFSthread, it has to be something like "1: launchd".
                 */
                const char *endp;
                int id = procfs_atoi(name, &endp);
                if (node_type != PFSprocnamedir && *endp != (char)0) {
                    /*
                     * Non-numeric before the end of the name -- this is invalid
                     * so skip this entry.
                     */
                    continue;
                }
                if (id != -1) {
                    /*
                     * An integer, so this node is a potential match. Construct the node id
                     * from the base node id of the matched node and the parent directory
                     * node's pid and object id, replacing either the pid or the object id
                     * with the value constructed from the name being looked up.
                     */
                    match_node_id.nodeid_base_id = match_node->psn_base_node_id;
                    match_node_id.nodeid_pid = node_type ==
                            PFSproc || node_type == PFSprocnamedir ? id : dir_pnp->node_id.nodeid_pid;
                    match_node_id.nodeid_objectid = node_type == PFSthread ? id : dir_pnp->node_id.nodeid_objectid;

                    /*
                     * The pid must match an existing process.
                     */
                    target_proc = proc_find(match_node_id.nodeid_pid);
                    if (target_proc == PROC_NULL) {
                        /*
                         * No matching process.
                         */
                        error = ENOENT;
                        break;
                    }

                    /*
                     * For the case of PFSprocnamedir, the name must be a complete
                     * and literal match to the full name that corresponds to the process
                     * id from the first part of the name.
                     */
                    if (node_type == PFSprocnamedir) {
                        char name_buffer[PROCESS_NAME_SIZE];
                        procfs_construct_process_dir_name(target_proc, name_buffer);
                        if (strcmp(name, name_buffer) != 0) {
                            /*
                             * Mismatched.
                             */
                            error = ENOENT;
                            break;
                        }
                    }

                    /*
                     * Determine whether an access check is required for access to
                     * the target process directory and its subdirectories. Do not
                     * check if root or if the file system is mounted with
                     * the "noprocperms" option.
                     */
                    boolean_t suser = vfs_context_suser(ap->a_context) == 0;
                    pfsmount_t *pmp = vfs_mp_to_procfs_mp(vnode_mount(dvp));
                    boolean_t check_access = !suser && procfs_should_access_check(pmp);
                    kauth_cred_t creds = vfs_context_ucred(ap->a_context);
                    if (check_access && procfs_check_can_access_process(creds, target_proc) != 0) {
                        /*
                         * Access not permitted - claim that the path does not exist.
                         */
                        error = ENOENT;
                        break;
                    } else {
                        /*
                         * If we have a thread id, it must match a thread of the process.
                         */
                        if (node_type == PFSthread) {
                            uint64_t *thread_ids;
                            int thread_count;
                            
                            int result =  procfs_get_thread_ids_for_task(target_proc, &thread_ids, &thread_count);
                            if (result == KERN_SUCCESS) {
                                boolean_t found = FALSE;
                                for (int i = 0; i < thread_count; i++) {
                                    if (thread_ids[i] == match_node_id.nodeid_objectid) {
                                        found = TRUE;
                                        break;
                                    }
                                }
                                procfs_release_thread_ids(thread_ids, thread_count);

                                if (thread_ids != NULL) {
                                    thread_ids = NULL;
                                }
                                
                                if (found == FALSE) {
                                    error = ENOENT;
                                    break;
                                }
                            } else {
                                error = ENOENT;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }

sysctl_matched:
        if (target_proc != PROC_NULL) {
            proc_rele(target_proc);
        }

        /*
         * We have a match if match_node is not NULL.
         */
        if (match_node != NULL && error == 0) {
            /*
             * We matched and match_node_id has been set to the node id of the
             * required node. Look for it in the cache, or create it if it is
             * not there. This also creates the vnode and increments its iocount.
             */
            pfsnode_t *target_pfsnode;
            vnode_t target_vnode;
            procfs_vnode_create_args create_args;
            create_args.vca_parentvp = dvp;
            error = procfsnode_find(mp, match_node_id,
                                    match_node,
                                    &target_pfsnode,
                                    &target_vnode,
                                    (create_vnode_func)&procfs_create_vnode,
                                    &create_args);
            if (error == 0) {
                *ap->a_vpp = target_vnode;
            }
        } else if (error == 0) {
            // No match
            error = ENOENT;
        }
    }

out:
    return error;
}

/*
 * Implementation of the VNOP_READDIR operation. Given a directory vnode,
 * returns as many directory entries as will fit in the area described by
 * a uio structure.
 * 
 * The content of a directory depends on its type, which is obtained from
 * its pfssnode_t object. In the simplest case, the directory
 * entries are simply the children of the structure node. This is the case
 * for process directories (e.g. /proc/1), for example, where the directory
 * entries are fixed vy the node structure. In the case of the root directory
 * (and several others) the content has to be determined dynamically based on
 * the running processes that are visible to the user. 
 *
 * Each directory entry is made as small as possible by only including the
 * non-null part of the file name. That means that the entries are of variable
 * size. To read a whole directory, the caller may need to invoke this operation
 * multiple times, each time with a different uio_offset value. Since it's not
 * possible to track which directory entry a given offset corrsponds to between
 * calls (especially since the logical content of the directory may change as 
 * a result of processes starting or terminating), the process always starts
 * from the first entry. However, entries are only copied out to the caller
 * once the uio_offset value has been reached.
 */
STATIC int
procfs_vnop_readdir(struct vnop_readdir_args *ap)
{
    vnode_t vp = ap->a_vp;
    if (vnode_vtype(vp) != VDIR) {
        return ENOTDIR;
    }

    pfsnode_t *dir_pnp = VTOPFS(vp);
    pfssnode_t *dir_snode = dir_pnp->node_structure_node;

    /*
     * /proc/sys directories enumerate the live sysctl tree, not static children.
     */
    if (dir_snode->psn_node_type == PFSsysctl) {
        return procfs_sysctl_readdir(ap);
    }

    int numentries = 0;
    int error = 0;
    uio_t uio = ap->a_uio;
    off_t nextpos = 0;
    off_t startpos = uio_offset(uio);

    /*
     * Determine whether access checks are required for process-related
     * nodes. Do not check if root or if the file system is mounted with
     * the "noprocperms" option.
     */
    boolean_t suser = vfs_context_suser(ap->a_context) == 0;
    pfsmount_t *pmp = vfs_mp_to_procfs_mp(vnode_mount(vp));
    boolean_t check_access = !suser && procfs_should_access_check(pmp);
    kauth_cred_t creds = vfs_context_ucred(ap->a_context);

    pfssnode_t *snode = TAILQ_FIRST(&dir_snode->psn_children);
    while (snode != NULL && uio_resid(uio) > 0) {
        /*
         * A version-spoofed release hides ksyms or kallsyms. Skip a hidden node
         * entirely (it contributes nothing to the directory offset), so it is
         * consistently absent across resumed readdir calls.
         */
        if (procfs_node_version_hidden(snode->psn_name)) {
            snode = TAILQ_NEXT(snode, psn_next);
            continue;
        }

        /*
         * We inherit the parent directory's pid and thread id for
         * most cases. This is overridden only for entries of type
         * PROCFS_PROCDIR and PROCFS_THREADDIR.
         */
        pid_t pid = dir_pnp->node_id.nodeid_pid;
        uint64_t objectid = dir_pnp->node_id.nodeid_objectid;
        pfsbaseid_t base_node_id = snode->psn_base_node_id;
        const char *name = snode->psn_name;

        /*
         * If there is a process id associated with this node, perform
         * an access check if required. Skip the entry if the user
         * does not have permission to see it.
         */
        if (pid == PRNODE_NO_PID || !check_access || procfs_check_can_access_proc_pid(creds, pid) == 0) {
            boolean_t procdir = FALSE;
            boolean_t procnamedir = FALSE;
            boolean_t threaddir = FALSE;
            boolean_t fddir = FALSE;
            int type = VREG;
            switch (snode->psn_node_type) {
            case PFSroot: /* Indicates structure error - skip it. */
                printf("procfs_vnop_readdir: ERROR: found PROCFS_ROOT\n");
                continue;

            case PFSdir:
                type = DT_DIR;
                break;

            case PFSfile:
                type = DT_REG;
                break;

            case PFScpuinfo:
                type = DT_REG;
                break;

            case PFSloadavg:
                type = DT_REG;
                break;

            case PFSmeminfo:
                type = DT_REG;
                break;

            case PFSmtab:
                type = DT_REG;
                break;

            case PFSstat:
                type = DT_REG;
                break;

            case PFSvmstat:
                type = DT_REG;
                break;

            case PFSpartitions:
                type = DT_REG;
                break;

            case PFSversion:
                type = DT_REG;
                break;

            case PFSuptime:         /* FALLTHROUGH */
            case PFSswaps:          /* FALLTHROUGH */
            case PFSfilesystems:    /* FALLTHROUGH */
            case PFSextensions:     /* FALLTHROUGH */
            case PFSmodules:        /* FALLTHROUGH */
            case PFSdiskstats:      /* FALLTHROUGH */
            case PFSdevices:        /* FALLTHROUGH */
            case PFSnetdev:         /* FALLTHROUGH */
            case PFSallocinfo:      /* FALLTHROUGH */
            case PFSapm:            /* FALLTHROUGH */
            case PFSbootconfig:     /* FALLTHROUGH */
            case PFSbuddyinfo:      /* FALLTHROUGH */
            case PFSpcidevices:     /* FALLTHROUGH */
            case PFSdma:            /* FALLTHROUGH */
            case PFSrtc:            /* FALLTHROUGH */
            case PFSexecdomains:    /* FALLTHROUGH */
            case PFSfb:             /* FALLTHROUGH */
            case PFSnfsexports:     /* FALLTHROUGH */
            case PFSinterrupts:     /* FALLTHROUGH */
            case PFSirq:            /* FALLTHROUGH */
            case PFStty:            /* FALLTHROUGH */
            case PFSioports:        /* FALLTHROUGH */
            case PFSiomem:          /* FALLTHROUGH */
            case PFSsoftirqs:       /* FALLTHROUGH */
            case PFSkcmdline:       /* FALLTHROUGH */
            case PFSide:            /* FALLTHROUGH */
            case PFSvmallocinfo:    /* FALLTHROUGH */
            case PFSisapnp:         /* FALLTHROUGH */
            case PFSscsi:           /* FALLTHROUGH */
            case PFSsysvipc:        /* FALLTHROUGH */
            case PFSpagetypeinfo:   /* FALLTHROUGH */
            case PFSslabinfo:       /* FALLTHROUGH */
            case PFSlocks:          /* FALLTHROUGH */
            case PFSkcore:          /* FALLTHROUGH */
            case PFSkmsg:           /* FALLTHROUGH */
            case PFSlastkmsg:       /* FALLTHROUGH */
            case PFSksyms:          /* FALLTHROUGH */
            case PFSmisc:
                type = DT_REG;
                break;

            case PFSsysctl:         /* /proc/sys itself is a directory */
                type = DT_DIR;
                break;

            case PFSdirthis:
                type = DT_DIR;

                /*
                 * We need to use the node id of the directory node for this case.
                 */
                pid = dir_pnp->node_id.nodeid_pid;
                objectid = dir_pnp->node_id.nodeid_objectid;
                base_node_id = dir_pnp->node_id.nodeid_base_id;
                break;

            case PFSdirparent:
                type = DT_DIR;

                /*
                 * We need to use the node id of the directory's parent node for this case.
                 */
                pfsid_t parent_node_id;
                procfs_get_parent_node_id(dir_pnp, &parent_node_id);
                pid = parent_node_id.nodeid_pid;
                objectid = parent_node_id.nodeid_objectid;
                base_node_id = parent_node_id.nodeid_base_id;
                break;

            case PFScurproc:        /* FALLTHROUGH */
            case PFSproclink:
                type = DT_LNK;
                break;

            /*
             * We handle these cases separately.
             */
            case PFSproc:
                procdir = TRUE;
                break;

            case PFSprocnamedir:
                procnamedir = TRUE;
                break;

            case PFSthread:
                threaddir = TRUE;
                break;

            case PFSfd:
                fddir = TRUE;
                break;
            }

            if (procdir || procnamedir) {
                /*
                 * An entry that represents the list of all processes.
                 * Iterate over all active processes and write entries for those that
                 * should appear after the start position, until we fill up the
                 * space or run out of processes. We don't include any processes that the
                 * caller does not have permission to access, unless the file system
                 * is mounted with the noprocperms option or the user is root.
                 */
                char name_buffer[PROCESS_NAME_SIZE];
                int pid_count;
                uint32_t pid_list_size;
                pid_t *pid_list;
                procfs_get_pids(&pid_list, &pid_count, &pid_list_size, check_access ? creds : NULL);

                /*
                 * Process each process in turn. We only get back process ids for the
                 * processes that the caller has permission to access.
                 */
                boolean_t pids_exhausted = TRUE;
                for (int i = 0; i < pid_count; i++) {
                    pid_t this_pid = pid_list[i];
                    if (procdir) {
                        snprintf(name_buffer, PROCESS_NAME_SIZE, "%d", this_pid);
                    } else {
                        proc_t p = proc_find(this_pid);
                        if (p == PROC_NULL) {
                            continue;
                        }
                        procfs_construct_process_dir_name(p, name_buffer);
                        proc_rele(p);
                    }
                    int size = procfs_calc_dirent_size(name_buffer);
                    if (nextpos >= startpos) {
                        error = procfs_copyout_dirent(VDIR, procfs_get_fileid(this_pid,
                                            PRNODE_NO_OBJECTID, base_node_id), name_buffer, uio, &size, nextpos + size);
                        if (error != 0 || size == 0) {
                            pids_exhausted = FALSE;
                            break;
                        }
                        numentries++;
                    }
                    nextpos += size;
                }

                procfs_release_pids(pid_list, pid_list_size);
                pid_list = NULL;
                /*
                 * Advance snode only when every PID was emitted so that eofflag
                 * is set correctly.  If the buffer filled mid-way, snode stays
                 * on this node and the caller will resume from nextpos.
                 */
                if (pids_exhausted) {
                    snode = TAILQ_NEXT(snode, psn_next);
                }
                /*
                 * Exit from the outer loop.
                 */
                break;
            } else if (threaddir) {
                /*
                 * Iterate over all of the threads for the current process and write
                 * entries for those that should appear after the start position,
                 * until we fill up the space or run out of threads.
                 */
                proc_t p = proc_find(pid);
                if (p != PROC_NULL) {
                    int thread_count;
                    uint64_t *thread_ids;
                    error = procfs_get_thread_ids_for_task(p, &thread_ids, &thread_count);
                    if (error == 0) {
                        char thread_buffer[PROCESS_NAME_SIZE];
                        for (int i = 0; i < thread_count; i++) {
                            uint64_t next_thread_id = thread_ids[i];
                            snprintf(thread_buffer, sizeof(thread_buffer), "%lld", next_thread_id);
                            int size = procfs_calc_dirent_size(thread_buffer);
                            // Copy out only if we are past the start offset.
                            if (nextpos >= startpos) {
                                error = procfs_copyout_dirent(VDIR, procfs_get_fileid(pid, next_thread_id, base_node_id), thread_buffer, uio, &size, nextpos + size);
                                if (error != 0 || size == 0) {
                                    break;
                                }
                                numentries++;
                            }
                            nextpos += size;
                        }
                        procfs_release_thread_ids(thread_ids, thread_count);
                        if (thread_ids != NULL) {
                            thread_ids = NULL;
                        }
                    }
                    proc_rele(p);
                    /*
                     * Exit from the outer loop.
                     */
                    break;
                } else {
                    /*
                     * No process for the current pid.
                     */
                    error = ENOENT;
                }
                if (error != 0) {
                    break;
                }
            } else if (fddir) {
                /*
                 * Iterate over the open file descriptors for the current process
                 * and write entries for those that should appear after the start
                 * position, until we fill up the space or run out of descriptors.
                 */
                proc_t p = proc_find(pid);
                if (p != PROC_NULL) {
                    struct proc_fdinfo *fdlist = NULL;
                    size_t fd_count = 0;
                    procfs_get_fd_list(p, &fdlist, &fd_count);

                    char fd_buffer[PROCESS_NAME_SIZE];
                    for (size_t k = 0; k < fd_count; k++) {
                        int fd = fdlist[k].proc_fd;
                        snprintf(fd_buffer, sizeof(fd_buffer), "%d", fd);
                        int size = procfs_calc_dirent_size(fd_buffer);

                        /*
                         * Copy out only if we are past the start offset.
                         */
                        if (nextpos >= startpos) {
                            error = procfs_copyout_dirent(VDIR,
                                procfs_get_fileid(pid, fd, base_node_id),
                                fd_buffer, uio, &size, nextpos + size);

                            if (error != 0 || size == 0) {
                                break;
                            }
                            numentries++;
                        }
                        nextpos += size;
                    }

                    procfs_release_fd_list(fdlist);
                    proc_rele(p);
                    /*
                     * Exit from the outer loop.
                     */
                    break;
                } else {
                    /*
                     * No process for the current pid.
                     */
                    error = ENOENT;
                }
                if (error != 0) {
                    break;
                }
            } else {
                /*
                 * Always compute size so nextpos advances correctly even when
                 * this entry falls before startpos (i.e. is being skipped on a
                 * resumed readdir call).
                 */
                int size = procfs_calc_dirent_size(name);
                if (nextpos >= startpos) {
                    error = procfs_copyout_dirent(type, procfs_get_fileid(pid, objectid, base_node_id), name, uio, &size, nextpos + size);
                    if (size == 0 || error != 0) {
                        break;
                    }
                    numentries++;
                }
                nextpos += size;
            }
        }
        
        /*
         * Continue with the next node.
         */
        snode = TAILQ_NEXT(snode, psn_next);
    }

    /*
     * Set output values for the next pass.
     */
    uio_setoffset(uio, nextpos);
    *ap->a_eofflag = snode == NULL; // EOF if we handled the last entry
    *ap->a_numdirent = numentries;

    return error;
}

/*
 * Readdir for a /proc/sys directory: emits "." and "..", then one entry per
 * live child sysctl oid (directory for a NODE, regular file for a leaf). Mirrors
 * the offset/EOF accounting of the main readdir (start from the first entry each
 * call; only copy entries at/after the caller's offset).
 */
STATIC int
procfs_sysctl_readdir(struct vnop_readdir_args *ap)
{
    pfsnode_t  *dir_pnp = VTOPFS(ap->a_vp);
    uio_t       uio      = ap->a_uio;
    off_t       startpos = uio_offset(uio);
    off_t       nextpos  = 0;
    int         numentries = 0;
    int         error    = 0;

    uint64_t    dir_objectid = dir_pnp->node_id.nodeid_objectid;
    pfsbaseid_t base         = dir_pnp->node_structure_node->psn_base_node_id;
    uint64_t    self_fileid  = procfs_get_node_fileid(dir_pnp);
    boolean_t   more         = TRUE;

    const char *dots[2] = { ".", ".." };
    for (int d = 0; d < 2; d++) {
        int size = procfs_calc_dirent_size(dots[d]);
        if (nextpos >= startpos) {
            error = procfs_copyout_dirent(DT_DIR, self_fileid, dots[d], uio, &size, nextpos + size);
            if (size == 0 || error != 0) {
                goto done;
            }
            numentries++;
        }
        nextpos += size;
    }

    for (int index = 0; error == 0 && uio_resid(uio) > 0; index++) {
        const char *name;
        boolean_t   is_node;
        uint64_t    objid;
        if (!procfs_sysctl_child_at(dir_objectid, index, &name, &is_node, &objid)) {
            /*
             * Reached the end of the list.
             */
            more = FALSE;
            break;
        }
        int size = procfs_calc_dirent_size(name);
        if (nextpos >= startpos) {
            error = procfs_copyout_dirent(is_node ? DT_DIR : DT_REG,
                        procfs_get_fileid(PRNODE_NO_PID, objid, base),
                        name, uio, &size, nextpos + size);
            if (size == 0 || error != 0) {
                break;
            }
            numentries++;
        }
        nextpos += size;
    }

done:
    uio_setoffset(uio, nextpos);
    *ap->a_eofflag   = (error == 0 && !more) ? 1 : 0;
    *ap->a_numdirent = numentries;
    return error;
}

/*
 * Calculates the packed size for a directory entry for a given
 * file name. The size is the sum of the fixed part of the dirent
 * structure plus the space required for the null-terminated name,
 * rounded up to a multiple of 8 bytes (required by XNU's direntry validation).
 */
STATIC int
procfs_calc_dirent_size(const char *name)
{
    struct direntry entry;
    return (int)(sizeof(struct direntry) - sizeof(entry.d_name) + ((strlen(name) + 1 + 7) & ~7));
}


/*
 * Copies a directory entry out to the area described by a uio structure and updates
 * that structure. No copy is performed if there is not enough space to copy the entire
 * structure.
 */
STATIC int
procfs_copyout_dirent(int type, uint64_t file_id, const char *name, uio_t uio, int *sizep, off_t seekoff)
{
    struct direntry entry;
    bzero(&entry, sizeof(entry));

    entry.d_type   = (uint8_t)type;
    entry.d_ino    = file_id;
    entry.d_seekoff = (uint64_t)seekoff;
    entry.d_namlen = (uint16_t)strlen(name);
    strlcpy(entry.d_name, name, entry.d_namlen + 1);

    int size = *sizep;
    entry.d_reclen = (uint16_t)size;

    int error = 0;
    if (size <= uio_resid(uio)) {
        error = uiomove((const char *)&entry, size, uio);
        *sizep = size;
    } else {
        *sizep = 0;
    }
    return error;
}

/*
 * Gets the attributes for a node, as seen by the stat(2) system
 * call. Many attributes don't make sense for procfs nodes, so are
 * not set. In other cases, the values are fixed. 
 *
 * The node permissions depend on whether the file system was mounted
 * with the "noprocperms" option. If it was, full access to all users
 * is granted. Otherwise, only owner and group have access, except for
 * symbolic links which always have mode 0777, allowing the access
 * decision to be made when resolving the target.
 */
STATIC int
procfs_vnop_getattr(struct vnop_getattr_args *ap)
{
    vnode_t vp = ap->a_vp;
    pfsnode_t *procfs_node = VTOPFS(vp);
    pfssnode_t *snode = procfs_node->node_structure_node;
    pfstype node_type = snode->psn_node_type;

    pid_t pid;  /* pid of the process for this node. */
    proc_t p = NULL;   /* proc_t for the process - NULL for the root node. */

    if (node_type != PFScpuinfo && node_type != PFSloadavg
     && node_type != PFSpartitions && node_type != PFSversion
     && node_type != PFSmeminfo && node_type != PFSmtab
     && node_type != PFSstat && node_type != PFSvmstat
     && node_type != PFSuptime && node_type != PFSswaps
     && node_type != PFSfilesystems && node_type != PFSsysctl
     && node_type != PFSextensions && node_type != PFSmodules
     && node_type != PFSdiskstats && node_type != PFSdevices
     && node_type != PFSnetdev && node_type != PFSallocinfo
     && node_type != PFSapm && node_type != PFSbootconfig
     && node_type != PFSbuddyinfo && node_type != PFSpcidevices
     && node_type != PFSdma && node_type != PFSrtc
     && node_type != PFSexecdomains && node_type != PFSfb
     && node_type != PFSnfsexports && node_type != PFSinterrupts
     && node_type != PFSirq && node_type != PFStty
     && node_type != PFSioports && node_type != PFSiomem
     && node_type != PFSsoftirqs && node_type != PFSkcmdline
     && node_type != PFSide && node_type != PFSvmallocinfo
     && node_type != PFSisapnp && node_type != PFSscsi
     && node_type != PFSsysvipc && node_type != PFSpagetypeinfo
     && node_type != PFSslabinfo && node_type != PFSlocks
     && node_type != PFSkcore && node_type != PFSkmsg
     && node_type != PFSlastkmsg && node_type != PFSksyms
     && node_type != PFSmisc) {
        /*
         * Get the process pid and proc_t for the target vnode.
         * Returns ENOENT if the process does not exist. For the
         * root vnode, p is zero and pid is PRNODE_NO_PID, but the
         * return value is zero.
         */
        int error = procfs_get_process_info(vp, &pid, &p);
        if (error != 0) {
            return error;
        }
    }

    /*
     * Report Linux-style fixed modes: directories 0555 (r-xr-xr-x), regular
     * files 0444 (r--r--r--), symlinks 0777. On Linux every /proc node is
     * world-readable, so these bits are not owner-restricted (the historical
     * "noprocperms" modemask is gone). Access to another user's process data is
     * still gated at lookup/readdir by procfs_should_access_check (the
     * "noprocperms" mount option), not by these mode bits; natively
     * procfs_vnop_access grants and the guest/consumer enforces on the mode.
     * Writable pseudo-files (note, clear_refs) get their write bits below.
     */
    pfsmount_t *pmp = vfs_mp_to_procfs_mp(vnode_mount(vp));

    struct vnode_attr *vap = ap->a_vap;
    switch (node_type) {
    case PFScurproc:        /* Symbolic link to the calling process (FALLTHRU) */
    case PFSprocnamedir:    /* Symbolic link to a process directory (FALLTHRU) */
    case PFSproclink:       /* Per-process exe/cwd/root symlink */
        VATTR_RETURN(vap, va_mode, ALL_ACCESS_ALL);   /* All access - target will determine actual access. */
        break;

    case PFSsysctl:         /* /proc/sys node: directory or leaf per oid */
        VATTR_RETURN(vap, va_mode,
            procfs_sysctl_is_node(procfs_node->node_id.nodeid_objectid)
                ? READ_EXECUTE_ALL : READ_ALL);
        break;

    default:
        /*
         * Everything else classifies via the shared directory predicate:
         * directories are 0555, regular files 0444.
         */
        VATTR_RETURN(vap, va_mode,
            procfs_is_directory_type(node_type) ? READ_EXECUTE_ALL : READ_ALL);
        break;
    }

    /*
     * The "note" node is an ordinary PFSfile structurally but is writable
     * (Plan9-style signal notes); grant it a read/write mode, identified by its
     * data function. open(O_WRONLY) would otherwise be rejected before write.
     */
    if (snode->psn_read_data_fn == procfs_donote) {
        VATTR_RETURN(vap, va_mode, RW_OWNER_GROUP);
    }
    /*
     * clear_refs is write-only (Linux mode 0200): a working-set knob written to,
     * never read. Grant owner/group write so open(O_WRONLY) is permitted.
     */
    if (snode->psn_read_data_fn == procfs_doclear_refs) {
        VATTR_RETURN(vap, va_mode, S_IWUSR | S_IWGRP);
    }

    /*
     *  ----- Generic attributes.
     */

    /*
     *  /proc/sys nodes are dir or file per the specific sysctl oid (objectid).
     */
    VATTR_RETURN(vap, va_type,
        node_type == PFSsysctl
            ? (procfs_sysctl_is_node(procfs_node->node_id.nodeid_objectid) ? VDIR : VREG)
            : procfs_allocvp(node_type));                                                                       /* File type */
    VATTR_RETURN(vap, va_fsid, pmp->pmnt_id);                                                                   /* File system id. */
    VATTR_RETURN(vap, va_fileid, procfs_get_node_fileid(procfs_node));                                          /* Unique file id. */
    VATTR_RETURN(vap, va_data_size, procfs_get_node_size_attr(procfs_node, vfs_context_ucred(ap->a_context)));  /* File size. */

    /*
     * For a process node, use the process's real start time (stable and
     * informative). For synthetic system nodes (root files, /proc/sys, ...)
     * there is no process, so report the current time as Linux's /proc does -
     * these files are generated on read, so a live timestamp is more meaningful
     * than the frozen mount time we used before.
     */
    struct timespec create_time;
    if (p != NULL) {
        create_time.tv_sec = p->p_start.tv_sec;
        create_time.tv_nsec = p->p_start.tv_usec * 1000;
    } else {
        nanotime(&create_time);
    }
    VATTR_RETURN(vap, va_access_time, create_time);
    VATTR_RETURN(vap, va_change_time, create_time);
    VATTR_RETURN(vap, va_create_time, create_time);
    VATTR_RETURN(vap, va_modify_time, create_time);

    /*
     * Set the UID/GID from the credentials of the process that
     * corresponds to the pfsnode_t, if there is one. There
     * is no process for the root node. For other nodes. the uid
     * and gid are the real ids for the current process.
     */
    proc_t current = current_proc();
    kauth_cred_t proc_cred = kauth_cred_proc_ref(current);
    uid_t uid = current == NULL ? (uid_t)0 : kauth_cred_getruid(proc_cred);
    gid_t gid = current == NULL ? (gid_t)0 : kauth_cred_getgid(proc_cred);
    if (p != NULL) {
        /*
         * Get the effective uid and gid from the process.
         */
        uid = kauth_cred_getuid(proc_cred);
        gid = kauth_cred_getgid(proc_cred);

        proc_rele(p);
    }
    VATTR_RETURN(vap, va_uid, uid);
    VATTR_RETURN(vap, va_gid, gid);
    kauth_cred_unref(&proc_cred);

    return 0;
}

/*
 * Reads the content of a symbolic link. Only the "curproc" entry
 * and nodes in the "byname" directory are symbolic links. The
 * specific data for each case is generated and returned here.
 */
STATIC int
procfs_vnop_readlink(struct vnop_readlink_args *ap)
{
    int error = 0;
    vnode_t vp = ap->a_vp;
    pfsnode_t *pnp = VTOPFS(vp);
    pfssnode_t *snode = pnp->node_structure_node;
    if (snode->psn_node_type == PFScurproc) {
        /*
         * The link is "curproc". Get the pid of the current process
         * and copy it out to the caller's buffer.
         */
        char pid_buffer[PROCESS_NAME_SIZE];
        proc_t p = current_proc();
        int pid = proc_pid(p);
        snprintf(pid_buffer, PROCESS_NAME_SIZE, "%d", pid);
        error = uiomove(pid_buffer, (int)strlen(pid_buffer), ap->a_uio);
    } else if (snode->psn_node_type == PFSprocnamedir) {
        /*
         * A link from the process name to the process id. Create a target
         * of the form "../123" and copy it out to the caller's buffer.
         */
        int pid = pnp->node_id.nodeid_pid;
        char pid_buffer[PROCESS_NAME_SIZE];
        snprintf(pid_buffer, PROCESS_NAME_SIZE, "../%d", pid);
        error = uiomove(pid_buffer, (int)strlen(pid_buffer), ap->a_uio);
    } else if (snode->psn_node_type == PFSproclink) {
        /*
         * A per-process symlink (exe/cwd/root): resolve to the target path.
         */
        char path[MAXPATHLEN];
        error = procfs_proclink_path(pnp->node_id.nodeid_pid, snode->psn_name,
                                     path, (int)sizeof(path));
        if (error == 0) {
            error = uiomove(path, (int)strlen(path), ap->a_uio);
        }
    } else {
        /*
         * Not valid for other node types.
         */
        error = EINVAL;
    }
    return error;
}

/*
 * Reads a node's data. The read operation is delegated to a function
 * that's held in the node's pfssnode_t. For nodes that
 * can't be read, the funtion is NULL and EINVAL will be returned,
 * except in the case of a directory, for which the error is EISDIR.
 */
STATIC int
procfs_vnop_read(struct vnop_read_args *ap)
{
    vnode_t vp = ap->a_vp;
    pfsnode_t *pnp = VTOPFS(vp);
    pfssnode_t *snode = pnp->node_structure_node;
    procfs_read_data_fn read_data_fn = snode->psn_read_data_fn;

    /*
     * /proc/sys nodes have no static read fn; read the sysctl value (or EISDIR
     * for a sysctl directory) from the oid carried in the node id.
     */
    if (snode->psn_node_type == PFSsysctl) {
        return procfs_sysctl_read(pnp->node_id.nodeid_objectid, ap->a_uio);
    }

    int error = EINVAL;
    if (procfs_is_directory_type(snode->psn_node_type)) {
        error = EISDIR;
    } else if (read_data_fn != NULL) {
        error = read_data_fn(pnp, ap->a_uio, ap->a_context);
    }
    return error;
}

/*
 * Writes to a node. Only the per-process "note" node is writable; its write
 * posts the note to the target process as a signal (see procfs_donote). Every
 * other node rejects writes. The note node is identified by its data function,
 * since it is an ordinary PFSfile structurally.
 */
STATIC int
procfs_vnop_write(struct vnop_write_args *ap)
{
    pfsnode_t *pnp = VTOPFS(ap->a_vp);
    pfssnode_t *snode = pnp->node_structure_node;

    if (procfs_is_directory_type(snode->psn_node_type)) {
        return EISDIR;
    }
    if (snode->psn_read_data_fn == procfs_donote) {
        return procfs_donote(pnp, ap->a_uio, ap->a_context);
    }
    if (snode->psn_read_data_fn == procfs_doclear_refs) {
        return procfs_doclear_refs(pnp, ap->a_uio, ap->a_context);
    }
    return EROFS;
}

/*
 * Setattr. procfs data is synthetic, so there is nothing to change. The one case
 * that matters is O_TRUNC on the writable "note" node (e.g. shell `>`): the open
 * path issues a va_data_size=0 setattr, which must succeed or the open fails.
 * Accept that as a no-op for the note node; reject everything else.
 */
STATIC int
procfs_vnop_setattr(struct vnop_setattr_args *ap)
{
    pfsnode_t *pnp = VTOPFS(ap->a_vp);
    pfssnode_t *snode = pnp->node_structure_node;

    if (snode->psn_read_data_fn == procfs_donote ||
        snode->psn_read_data_fn == procfs_doclear_refs) {
        if (VATTR_IS_ACTIVE(ap->a_vap, va_data_size)) {
            VATTR_SET_SUPPORTED(ap->a_vap, va_data_size);
        }
        return 0;
    }
    return ENOTSUP;
}

/*
 * Reclaims a vnode and its associated pfsnode_t when it's
 * no longer needed by the kernel file system code.
 * Removes the pfsnode_t from the hash table, removes the
 * file system reference and breaks the link between the
 * vnode and the pfsnode_t.
 */
STATIC int
procfs_vnop_reclaim(struct vnop_reclaim_args *ap)
{
    pfsnode_t *pnp = VTOPFS(ap->a_vp);

    if (pnp != NULL) {
        /*
         * Lock to manipulate the hash table.
         */
        lck_mtx_lock(pfsnode_hash_mutex);

        /*
         * Remove the node from the hash table and free it.
         */
        procfsnode_free_node(pnp);

        /*
         * CAUTION: pnp is now invalid. Null it out to cause a panic
         * if it gets referenced beyond this point.
         */
        if (pnp != NULL) {
            pnp = NULL;
        }
        lck_mtx_unlock(pfsnode_hash_mutex);
    }

    /*
     * Remove the file system reference that we added when
     * we created the vnode.
     */
    vnode_removefsref(ap->a_vp);

    /*
     * Clear the link to the pfsnode_t since the
     * vnode will no longer be linked to it.
     */
    vnode_clearfsnode(ap->a_vp);

    return 0;
}

#pragma mark -
#pragma mark Helper Functions

/*
 * Creates a vnode with given properties, which depend on the vnode type.
 */
STATIC int
procfs_create_vnode(procfs_vnode_create_args *cap, pfsnode_t *pnp, vnode_t *vpp)
{
    pfssnode_t *snode = pnp->node_structure_node;
    struct vnode_fsparam vnode_create_params;

    memset(&vnode_create_params, 0, sizeof(vnode_create_params));
    vnode_create_params.vnfs_mp = vnode_mount(cap->vca_parentvp);
    /*
     * /proc/sys nodes are directories or files depending on the specific sysctl
     * oid carried in the node id (objectid), not on the pfstype alone.
     */
    vnode_create_params.vnfs_vtype = (snode->psn_node_type == PFSsysctl)
        ? (procfs_sysctl_is_node(pnp->node_id.nodeid_objectid) ? VDIR : VREG)
        : procfs_allocvp(snode->psn_node_type);
    vnode_create_params.vnfs_str = "procfs vnode";
    vnode_create_params.vnfs_dvp = cap->vca_parentvp;
    vnode_create_params.vnfs_fsnode = pnp;
    vnode_create_params.vnfs_vops = procfs_vnodeop_p;
    vnode_create_params.vnfs_markroot = 0;
    vnode_create_params.vnfs_flags = VNFS_CANTCACHE;

    /*
     * Create the vnode, if possible.
     */
    vnode_t new_vnode;
    int error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vnode_create_params, &new_vnode);

    /*
     * Return the root vnode pointer to the caller, if it was created.
     */
    *vpp = error == 0 ? new_vnode : NULLVP;

    return error;
}

/*
 * Constructs the name of the directory for a given process. This
 * is simply a matter of converting its process id to a decimal
 * string.
 */
STATIC void
procfs_construct_process_dir_name(proc_t p, char *buffer)
{
    int pid = proc_pid(p);
    int len = snprintf(buffer, PROCESS_NAME_SIZE, "%d ", pid);
    proc_name(pid, buffer + len, MAXCOMLEN + 1);
}
