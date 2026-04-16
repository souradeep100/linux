# KHO (Kexec Handover) Deep Analysis

## What is KHO?

KHO (Kexec Handover) is a Linux kernel mechanism that allows **preserving state and memory pages across a kexec reboot**. Normally, kexec loads a new kernel and starts fresh — all in-flight state is lost. KHO changes this by:

1. **Serializing device/driver state** into a Flat Device Tree (FDT) blob before kexec
2. **Marking specific physical memory pages as "preserved"** so the new kernel doesn't overwrite them
3. **Passing the preserved-page map and FDT** to the new kernel via crash MSR registers (on Hyper-V)

The new kernel boots, reads the KHO FDT, reclaims the preserved pages, and drivers can resume from where they left off — enabling **live update** of the hypervisor host kernel without disrupting guest VMs.

---

## KHO Core Concepts — Detailed Explanation with Code

### 1. KHO Device Tree (FDT)

The KHO FDT is a **Flat Device Tree blob** that carries serialized driver/subsystem state across a kexec reboot. Unlike standard device trees (which use big-endian), KHO uses **native endianness** for performance. Each subsystem that participates in KHO registers a named **subtree** in this FDT containing its metadata (physical addresses, counters, configuration).

**Key insight:** The FDT itself is a preserved page — it survives kexec because it's allocated via `kho_alloc_preserve()`.

#### Example: How a subsystem registers its FDT subtree

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_add_subtree(const char *name, void *fdt)
{
    phys_addr_t phys = virt_to_phys(fdt);
    void *root_fdt = kho_out.fdt;
    int off, fdt_err;

    guard(mutex)(&kho_out.lock);

    fdt_err = fdt_open_into(root_fdt, root_fdt, PAGE_SIZE);
    if (fdt_err < 0)
        return -ENOMEM;

    /* Create a named subnode in the root FDT */
    off = fdt_add_subnode(root_fdt, 0, name);
    if (off < 0) {
        if (off == -FDT_ERR_EXISTS)
            return -EEXIST;
        goto out_pack;
    }

    /* Store the physical address of the subsystem's own FDT */
    int err = fdt_setprop(root_fdt, off, KHO_FDT_SUB_TREE_PROP_NAME,
                          &phys, sizeof(phys));

out_pack:
    fdt_pack(root_fdt);
    return err;
}
```

**Usage by MSHV** (from `drivers/hv/mshv_page_preserve.c`):
```c
/* Register MSHV's FDT subtree right before kexec */
err = kho_add_subtree(FDT_SUBTREE_MSHV, fdt_page);
```

#### Example: How the new kernel retrieves the subtree after kexec

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_retrieve_subtree(const char *name, phys_addr_t *phys);

/* Usage in mshv_page_preserve.c - restore_tree() */
static int __init restore_tree(void)
{
    phys_addr_t fdt_pa, root_table_pa;

    /* Locate MSHV's subtree in the preserved FDT */
    int err = kho_retrieve_subtree(FDT_SUBTREE_MSHV, &fdt_pa);
    if (err)
        return err;

    void *fdt = phys_to_virt(fdt_pa);

    /* Verify compatibility string */
    int node = fdt_path_offset(fdt, "/");
    if (fdt_node_check_compatible(fdt, node, MSHV_KHO_COMPAT_STR))
        return -EINVAL;

    /* Extract radix tree root PA from FDT property */
    const phys_addr_t *root_table_fdt_ptr =
        fdt_getprop(fdt, node, "root_table", &len);

    /* Re-initialize the preserved page tree from restored root */
    int init_err = kho_radix_tree_init(&preserved_pages_tree,
                                        *root_table_fdt_ptr);
    ...
}
```

#### FDT Layout

```
KHO Root FDT (PAGE_SIZE, allocated via kho_alloc_preserve)
├── compatible = "kho-v1"
├── preserved-memory-map = <radix_tree_root_pa>   ← global page map
├── /mshv                                          ← MSHV subtree pointer
│   └── sub-tree = <mshv_fdt_pa>
│       MSHV FDT (separate page):
│       ├── compatible = "microsoft,mshv-kho"
│       ├── root_table = <preserved_pages_tree_root_pa>
│       └── partition_ids = [id0, id1, ...]
├── /liveupdate                                    ← LUO subtree pointer
│   └── sub-tree = <luo_fdt_pa>
│       LUO FDT (separate page):
│       ├── compatible = "luo,sessions-v1"
│       └── session_header = <session_header_pa>
└── ...
```

---

### 2. Scratch Regions

Scratch regions solve a chicken-and-egg problem: **where do you put the new kernel image during kexec if all memory might contain preserved pages?**

Scratch regions are **physically contiguous memory** allocated once (on the very first boot) and managed as CMA (Contiguous Memory Allocator). They serve as the **landing zone** for the kexec'd kernel image and initrd. Because they're CMA, they can be used for general allocations during normal operation but are guaranteed to be reclaimable when kexec needs contiguous space.

**Key insight:** Scratch regions are **reused across recursive KHO kexecs** — the new kernel inherits the same scratch regions and uses them for its eventual kexec.

#### Example: How scratch regions are initialized on fresh boot

```c
/* From kernel/liveupdate/kexec_handover.c - kho_init() */

static __init int kho_init(void)
{
    ...
    /* Fresh boot - convert scratch regions from reserved memblock to CMA */
    for (int i = 0; i < kho_scratch_cnt; i++) {
        unsigned long base_pfn = PHYS_PFN(kho_scratch[i].addr);
        unsigned long count = kho_scratch[i].size >> PAGE_SHIFT;
        kmemleak_ignore_phys(kho_scratch[i].addr);
        for (unsigned long pfn = base_pfn; pfn < base_pfn + count;
             pfn += pageblock_nr_pages)
            init_cma_reserved_pageblock(pfn_to_page(pfn));
    }
    ...
}
```

#### Example: How the new kernel discovers scratch regions

```c
/* From kernel/liveupdate/kexec_handover.c */

void __init kho_populate(phys_addr_t fdt_phys, u64 fdt_len,
                         phys_addr_t scratch_phys, u64 scratch_len)
{
    unsigned int scratch_cnt = scratch_len / sizeof(*kho_scratch);
    struct kho_scratch *scratch;

    /* Map and validate incoming FDT from old kernel */
    void *fdt = early_memremap(fdt_phys, fdt_len);
    int err = fdt_check_header(fdt);
    err = fdt_node_check_compatible(fdt, 0, KHO_FDT_COMPATIBLE);

    /* Map scratch region descriptors */
    scratch = early_memremap(scratch_phys, scratch_len);

    /* Mark scratch regions in memblock so they're available for alloc */
    for (int i = 0; i < scratch_cnt; i++) {
        struct kho_scratch *area = &scratch[i];
        memblock_add(area->addr, area->size);
        memblock_mark_kho_scratch(area->addr, area->size);
    }

    memblock_reserve(scratch_phys, scratch_len);
    memblock_set_kho_scratch_only();

    kho_in.fdt_phys = fdt_phys;
    kho_in.scratch_phys = scratch_phys;
    ...
}
```

#### Scratch Region Data Structure

```c
/* From include/linux/kexec_handover.h */

struct kho_scratch {
    phys_addr_t addr;   /* Physical start address */
    phys_addr_t size;   /* Region size in bytes */
};
```

#### Scratch Region Lifecycle

```
Fresh Boot:
  memblock reserves scratch regions → kho_init() converts to CMA pages
  ↓
Normal Operation:
  CMA allocator can use scratch pages for movable allocations
  ↓
Kexec Preparation:
  CMA reclaims scratch pages → new kernel/initrd loaded into scratch
  ↓
New Kernel Boot:
  kho_populate() re-discovers scratch → memblock marks them → reused
```

---

### 3. Radix Tree (Page Preservation Map)

The radix tree is the central data structure that tracks **which physical pages must survive kexec**. It replaces an earlier xarray-based approach and is designed so its **entire structure lives in physical memory** — the root physical address is all the new kernel needs to reconstruct the full map.

#### Architecture

```
Radix Tree Root (PA stored in crash MSR P2 & FDT)
│
├── Level 3: Internal Node (page-sized, 512 entries on 64-bit)
│   ├── [idx] → PA of Level 2 node
│   └── [idx] → PA of Level 2 node
│       │
│       ├── Level 2: Internal Node
│       │   └── [idx] → PA of Level 1 node
│       │       │
│       │       └── Level 1: Internal Node
│       │           └── [idx] → PA of Leaf node
│       │               │
│       │               └── Level 0: Leaf (bitmap)
│       │                   └── bitmap[0..PAGE_SIZE*8-1]
│       │                       Each bit = one preserved page
│       └── ...
└── ...
```

**Key design choices:**
- Internal nodes store **physical addresses** (not virtual), so the tree is position-independent across kexec
- Leaf nodes are **bitmaps** — one bit per preserved page, extremely memory-efficient
- A unique **key encoding** combines the physical address and page order into a single lookup key

#### Key Encoding/Decoding

The radix tree uses a clever key that encodes both the physical page frame number and its order (for compound/huge pages):

```c
/* From kernel/liveupdate/kexec_handover.c */

static unsigned long kho_radix_encode_key(phys_addr_t phys,
                                          unsigned int order)
{
    /* High bit encodes the order, low bits encode the PFN */
    unsigned long h = 1UL << (KHO_ORDER_0_LOG2 - order);
    unsigned long l = phys >> (PAGE_SHIFT + order);
    return h | l;
}

static phys_addr_t kho_radix_decode_key(unsigned long key,
                                        unsigned int *order)
{
    unsigned int order_bit = fls64(key);
    *order = KHO_ORDER_0_LOG2 - order_bit + 1;
    phys_addr_t phys = key << (PAGE_SHIFT + *order);
    return phys;
}
```

**Example:** For a 4K page at physical address `0x1000_0000`:
- `order = 0` (single page)
- `h = 1 << KHO_ORDER_0_LOG2` (high sentinel bit)
- `l = 0x1000_0000 >> 12 = 0x10000`
- `key = h | 0x10000`

#### Core Structures

```c
/* From include/linux/kho_radix_tree.h */

struct kho_radix_tree {
    struct kho_radix_node *root;
    struct mutex lock;      /* protects tree structure and root */
    bool frozen;            /* set true before kexec — no more modifications */
};

struct kho_radix_crash_tree {
    struct kho_radix_node *root;  /* VA pointers (remapped for crash kernel) */
};

/* Callback type for tree walking */
typedef int (*kho_radix_tree_walk_callback_t)(phys_addr_t phys,
                                              unsigned int order);
```

#### Example: Adding a page to the preservation tree

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_radix_add_page(struct kho_radix_tree *tree,
                       unsigned long pfn, unsigned int order)
{
    unsigned long key = kho_radix_encode_key(PFN_PHYS(pfn), order);
    struct kho_radix_node *node = tree->root;
    struct kho_radix_leaf *leaf;
    unsigned int i, idx;

    guard(mutex)(&tree->lock);

    if (tree->frozen)
        return -EBUSY;   /* Tree locked for kexec — no changes */

    /* Walk internal levels, allocating nodes as needed */
    for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
        idx = kho_radix_get_table_index(key, i);
        if (!node->table[idx]) {
            /* Allocate a new page-sized node */
            new_node = (struct kho_radix_node *)get_zeroed_page(GFP_KERNEL);
            if (!new_node)
                return -ENOMEM;
            /* Store physical address for cross-kexec portability */
            node->table[idx] = virt_to_phys(new_node);
        }
        node = phys_to_virt(node->table[idx]);
    }

    /* Set the bit in the leaf bitmap */
    idx = kho_radix_get_bitmap_index(key);
    leaf = (struct kho_radix_leaf *)node;
    __set_bit(idx, leaf->bitmap);

    return 0;
}
```

#### Example: Walking the tree (used to preserve tree metadata pages themselves)

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_radix_walk_tree(struct kho_radix_tree *tree,
                        kho_radix_tree_walk_callback_t cb_data,
                        kho_radix_tree_walk_callback_t cb_meta)
{
    guard(mutex)(&tree->lock);

    /* Report root metadata page */
    int err = cb_meta(virt_to_phys(tree->root), 0);
    if (err)
        return err;

    /* Recursively walk all levels */
    return __kho_radix_walk_tree(tree->root, KHO_TREE_MAX_DEPTH - 1,
                                 0, cb_data, cb_meta);
}

/* Leaf walker — iterates over set bits in the bitmap */
static int kho_radix_walk_leaf(struct kho_radix_leaf *leaf,
                               unsigned long key,
                               kho_radix_tree_walk_callback_t cb)
{
    unsigned long *bitmap = (unsigned long *)leaf;

    for_each_set_bit(i, bitmap, PAGE_SIZE * BITS_PER_BYTE) {
        unsigned int order;
        phys_addr_t phys = kho_radix_decode_key(key | i, &order);
        int err = cb(phys, order);
        if (err)
            return err;
    }
    return 0;
}
```

---

### 4. Page Preservation API

KHO provides multiple levels of page preservation APIs, from low-level page marking to high-level allocate-and-preserve helpers.

#### API Hierarchy

```
High-level (auto-allocate + preserve):
  kho_alloc_preserve(size)     → allocates zeroed pages + marks preserved
  kho_unpreserve_free(ptr)     → unmarks + frees

Mid-level (preserve existing):
  kho_preserve_folio(folio)    → marks a folio as preserved
  kho_preserve_pages(page, n)  → marks N contiguous pages as preserved
  kho_unpreserve_folio(folio)  → unmarks a folio
  kho_unpreserve_pages(page,n) → unmarks N pages

Low-level (direct radix tree):
  kho_radix_add_page(tree, pfn, order)  → sets bit in radix tree
  kho_radix_del_page(tree, pfn, order)  → clears bit in radix tree

Restoration (new kernel):
  kho_restore_folio(phys)      → reclaims a preserved folio
  kho_restore_pages(phys, n)   → reclaims N preserved pages
  kho_restore_free(ptr)        → reclaims + frees preserved allocation
```

#### Example: Allocating memory that survives kexec

```c
/* From kernel/liveupdate/kexec_handover.c */

void *kho_alloc_preserve(size_t size)
{
    struct folio *folio;
    int order, ret;

    if (!size)
        return ERR_PTR(-EINVAL);

    order = get_order(size);
    if (order > MAX_PAGE_ORDER)
        return ERR_PTR(-E2BIG);

    /* Allocate zeroed, physically contiguous pages */
    folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, order);
    if (!folio)
        return ERR_PTR(-ENOMEM);

    /* Mark the folio as preserved in the radix tree */
    ret = kho_preserve_folio(folio);
    if (ret) {
        folio_put(folio);
        return ERR_PTR(ret);
    }

    return folio_address(folio);
}
```

**Usage example** (from `drivers/hv/mshv_page_preserve.c`):
```c
/* Allocate preserved memory for partition ID array */
u64 *ids = kho_alloc_preserve(nr_alloc * sizeof(*ids));
if (IS_ERR(ids))
    return PTR_ERR(ids);

/* This memory will survive kexec — new kernel can read partition IDs */
for (i = 0; i < nr_partitions; i++)
    ids[i] = partition->pt_id;
```

#### Example: Preserving pages for a range (e.g. Hyper-V donated pages)

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_preserve_pages(struct page *page, unsigned long nr_pages)
{
    struct kho_radix_tree *tree = &kho_out.radix_tree;
    const unsigned long start_pfn = page_to_pfn(page);
    const unsigned long end_pfn = start_pfn + nr_pages;
    unsigned long pfn = start_pfn;
    int err = 0;

    /* Safety: ensure no overlap with scratch regions */
    if (WARN_ON(kho_scratch_overlap(start_pfn << PAGE_SHIFT,
                                    nr_pages << PAGE_SHIFT)))
        return -EINVAL;

    /* Process pages in maximal power-of-2 chunks */
    while (pfn < end_pfn) {
        const unsigned int order =
            min(count_trailing_zeros(pfn), ilog2(end_pfn - pfn));

        err = kho_radix_add_page(tree, pfn, order);
        if (err) {
            /* Rollback: unpreserve all pages added so far */
            __kho_unpreserve(tree, start_pfn, pfn);
            break;
        }
        pfn += 1 << order;
    }

    return err;
}
```

**Usage in MSHV** (from `drivers/hv/mshv_page_preserve.c`):
```c
/* When MSHV donates a page to the hypervisor for guest memory */
int mshv_register_preserve_pages(struct page *pg, unsigned int order)
{
    unsigned long pfn = page_to_pfn(pg);
    return kho_radix_add_page(&preserved_pages_tree, pfn, order);
}
```

#### Example: Restoring a preserved page in the new kernel

```c
/* From kernel/liveupdate/kexec_handover.c */

static struct page *kho_restore_page(phys_addr_t phys, bool is_folio)
{
    struct page *page = pfn_to_online_page(PHYS_PFN(phys));
    union kho_page_info info;

    if (!page)
        return NULL;

    /* Read the magic marker left by the old kernel */
    info.page_private = page->private;
    if (WARN_ON_ONCE(info.magic != KHO_PAGE_MAGIC ||
                     info.order > MAX_PAGE_ORDER))
        return NULL;

    page->private = 0;  /* Clear magic after retrieval */

    if (is_folio)
        kho_init_folio(page, info.order);
    else
        kho_init_pages(page, 1 << info.order);

    /* Return pages to the managed page count */
    adjust_managed_page_count(page, (1 << info.order));
    return page;
}

struct folio *kho_restore_folio(phys_addr_t phys)
{
    struct page *page = kho_restore_page(phys, true);
    return page ? page_folio(page) : NULL;
}
```

---

### 5. Vmalloc Preservation

KHO can preserve **vmalloc regions** — virtually contiguous but physically scattered memory. This is important for preserving large kernel data structures that were allocated via `vmalloc()`.

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_preserve_vmalloc(void *ptr, struct kho_vmalloc *preservation)
{
    struct vm_struct *vm = find_vm_area(ptr);
    struct kho_vmalloc_chunk *chunk;
    unsigned int order, nr_contig_pages;

    if (!vm || (vm->flags & ~KHO_VMALLOC_SUPPORTED_FLAGS))
        return -EINVAL;

    order = get_vm_area_page_order(vm);
    chunk = new_vmalloc_chunk(NULL);
    KHOSER_STORE_PTR(preservation->first, chunk);
    nr_contig_pages = (1 << order);

    /* Walk all physical pages backing the vmalloc region */
    for (int i = 0; i < vm->nr_pages; i += nr_contig_pages) {
        phys_addr_t phys = page_to_phys(vm->pages[i]);

        /* Preserve each physical page chunk */
        int err = kho_preserve_pages(vm->pages[i], nr_contig_pages);
        if (err)
            return err;

        /* Record physical addresses in a linked-list of chunks */
        chunk->phys[idx++] = phys;
        if (idx == ARRAY_SIZE(chunk->phys)) {
            chunk = new_vmalloc_chunk(chunk);
            idx = 0;
        }
    }

    preservation->total_pages = vm->nr_pages;
    preservation->flags = vmalloc_flags_to_kho(vm->flags);
    preservation->order = order;

    return 0;
}
```

**Restoration** creates a new vmalloc mapping backed by the preserved physical pages:
```c
void *kho_restore_vmalloc(const struct kho_vmalloc *preservation);
```

---

### 6. KHO Active Phase & Freezing

The KHO lifecycle has a critical state transition: before kexec, the radix tree is **frozen** to prevent concurrent modifications while the tree is being serialized and its metadata pages are themselves being preserved.

```
Normal Operation ──► Freeze (pre-kexec) ──► Kexec ──► New Kernel
  (add/del pages)     (no modifications)               (restore)
```

```c
/* From kernel/liveupdate/kexec_handover.c */

int kho_radix_tree_freeze(struct kho_radix_tree *tree)
{
    guard(mutex)(&tree->lock);

    if (tree->frozen)
        return -EBUSY;

    tree->frozen = true;
    return 0;
}
```

After freezing, any attempt to add/delete pages returns `-EBUSY`:
```c
int kho_radix_add_page(struct kho_radix_tree *tree, ...)
{
    guard(mutex)(&tree->lock);
    if (tree->frozen)
        return -EBUSY;   /* Cannot modify frozen tree */
    ...
}
```

---

### 7. Live Update Orchestrator (LUO)

LUO sits on top of KHO and provides **session-based state preservation** for userspace services. It allows processes to preserve file descriptors and metadata across kexec via the `/dev/liveupdate` device.

#### LUO State Structure

```c
/* From kernel/liveupdate/luo_core.c */

static struct {
    bool enabled;
    void *fdt_out;       /* Outgoing FDT for this kernel */
    void *fdt_in;        /* Incoming FDT from previous kernel */
    u64 liveupdate_num;  /* Counter: how many live updates have occurred */
} luo_global;

/* From kernel/liveupdate/luo_session.c */

struct luo_session {
    char name[LIVEUPDATE_SESSION_NAME_LENGTH];
    struct luo_session_ser *ser;
    struct list_head list;
    bool retrieved;
    struct luo_file_set file_set;  /* Preserved file descriptors */
    struct mutex mutex;
};
```

#### LUO Serialization Flow (pre-kexec)

```c
/* Called from the kexec path */
int liveupdate_reboot(void)
{
    if (!liveupdate_enabled())
        return 0;

    /* Serialize all sessions to preserved memory */
    int err = luo_session_serialize();
    if (err)
        return err;

    luo_flb_serialize();
    return 0;
}

/* Serialize sessions — writes metadata to kho_alloc_preserve'd memory */
int luo_session_serialize(void)
{
    struct luo_session_header *sh = &luo_session_global.outgoing;
    struct luo_session *session;
    int i = 0;

    guard(rwsem_write)(&sh->rwsem);

    list_for_each_entry(session, &sh->list, list) {
        int err = luo_session_freeze_one(session, &sh->ser[i]);
        strscpy(sh->ser[i].name, session->name,
                sizeof(sh->ser[i].name));
        i++;
    }

    sh->header_ser->count = sh->count;
    return 0;
}
```

#### LUO Deserialization Flow (post-kexec)

```c
int luo_session_deserialize(void)
{
    struct luo_session_header *sh = &luo_session_global.incoming;

    for (int i = 0; i < sh->header_ser->count; i++) {
        /* Re-create session object */
        struct luo_session *session = luo_session_alloc(sh->ser[i].name);

        luo_session_insert(sh, session);

        /* Restore file descriptors from preserved memory */
        scoped_guard(mutex, &session->mutex) {
            luo_file_deserialize(&session->file_set,
                                 &sh->ser[i].file_set_ser);
        }
    }

    kho_restore_free(sh->header_ser);
    return 0;
}
```

---

### 8. Crash Dump Exclusion

In a crash kernel scenario, preserved pages (containing guest VM memory) must be **excluded from crash dumps** to reduce dump size and avoid leaking guest data. The crash kernel uses a separate read-only tree structure.

```c
/* From drivers/hv/mshv_page_preserve.c */

#ifdef CONFIG_CRASH_DUMP
static struct kho_radix_crash_tree crash_preserved_pages_tree;

static int __init restore_crash_tree(void)
{
    /* Root PA was stashed in crash MSR P2 by the panicked kernel */
    phys_addr_t root_pa = hv_get_msr(HV_MSR_CRASH_P2);
    if (!root_pa)
        return -ENOENT;

    /* Use memremap (not phys_to_virt) — crash kernel has limited mappings */
    return kho_radix_crash_init(&crash_preserved_pages_tree, root_pa);
}

/* Callback registered with vmcore: "is this PFN real RAM for dumping?" */
static bool mshv_vmcore_pfn_is_ram(struct vmcore_cb *cb, unsigned long pfn)
{
    /* Exclude MSHV preserved pages from crash dump */
    return !kho_radix_crash_contains_page(&crash_preserved_pages_tree,
                                           pfn, 0);
}

static struct vmcore_cb mshv_vmcore_cb = {
    .pfn_is_ram = mshv_vmcore_pfn_is_ram,
};
#endif
```

**Crash tree lookup** — uses `memremap`'d VA pointers instead of `phys_to_virt`:

```c
bool kho_radix_crash_contains_page(struct kho_radix_crash_tree *tree,
                                   unsigned long pfn, unsigned int order)
{
    unsigned long key = kho_radix_encode_key(PFN_PHYS(pfn), order);
    struct kho_radix_node *node = tree->root;

    if (!tree->root)
        return false;

    /* Traverse using VA pointers (from memremap) */
    for (int i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
        unsigned int idx = kho_radix_get_table_index(key, i);
        if (!node->table[idx])
            return false;
        node = (struct kho_radix_node *)(uintptr_t)node->table[idx];
    }

    struct kho_radix_leaf *leaf = (struct kho_radix_leaf *)node;
    unsigned int idx = kho_radix_get_bitmap_index(key);
    return test_bit(idx, leaf->bitmap);
}
```

---

## How KHO Works End-to-End

### Lifecycle Diagram

```
┌─────────────────────── PHASE 1: FRESH BOOT ───────────────────────┐
│                                                                    │
│  memblock reserves scratch regions                                 │
│  kho_init() → alloc radix tree root + FDT, convert scratch to CMA │
│  Drivers register with KHO (e.g., MSHV registers reboot notifier) │
│  Normal operation: pages donated to hypervisor, tracked in tree    │
│                                                                    │
└────────────────────────────┬───────────────────────────────────────┘
                             │
┌────────────────────── PHASE 2: PRE-KEXEC ─────────────────────────┐
│                                                                    │
│  1. LUO serializes userspace sessions → preserved memory           │
│  2. MSHV freezes all guest partitions (stop VP execution)          │
│  3. MSHV vacuums partitions (reclaim lazy hypervisor resources)    │
│  4. Radix tree frozen (no more add/del)                            │
│  5. Tree walked → all data + metadata pages preserved via KHO     │
│  6. FDT subtree added with partition IDs + tree root PA           │
│  7. Root PA → crash MSR P2 (backup for crash kernel)              │
│  8. kho_fill_kimage() packages FDT + scratch into kexec segments  │
│                                                                    │
└────────────────────────────┬───────────────────────────────────────┘
                             │ kexec
                             ▼
┌────────────────────── PHASE 3: NEW KERNEL BOOT ───────────────────┐
│                                                                    │
│  1. kho_populate() — early boot: discover FDT + scratch regions    │
│  2. memblock marks preserved pages as reserved (don't overwrite!) │
│  3. kho_init() — restore radix tree from FDT root PA              │
│  4. mshv_preserve_init() — restore MSHV page tree + partition IDs │
│  5. luo_session_deserialize() — restore userspace sessions        │
│  6. MSHV driver reclaims preserved pages from tracker             │
│  7. Guest partitions unfrozen → VPs resume execution              │
│  8. Normal operation — guests never knew the host rebooted        │
│                                                                    │
└───────────────────────────────────────────────────────────────────-┘
```

### Data Flow Diagram

```
                    OLD KERNEL                          NEW KERNEL
                    ─────────                           ──────────
  Driver donates page to HV
       │
       ▼
  mshv_register_preserve_pages()
       │
       ▼
  kho_radix_add_page(tree, pfn, order)
       │                                           kho_populate()
       ▼                                                │
  [Radix Tree: bit set for PFN]                         ▼
       │                                           memblock reserves
       ▼                                           preserved pages
  reboot_cb() on kexec:                                 │
       │                                                ▼
       ├── freeze partitions                       kho_init()
       ├── kho_radix_tree_freeze()                      │
       ├── kho_radix_walk_tree()                        ▼
       │      └── preserve each page              kho_radix_tree_init()
       ├── kho_add_subtree("mshv", fdt)           from restored root PA
       └── crash MSR P2 = tree root PA                  │
                                                        ▼
                                                   restore_tree()
                                                        │
                                                        ▼
                                                   kho_restore_pages()
                                                   for each preserved PFN
                                                        │
                                                        ▼
                                                   Guest VMs resume
```

---

## Commit Analysis: What Each Change Does

### Group 1: KHO Infrastructure (`kernel/liveupdate/`)

These commits rework the core KHO subsystem:

| Commit | Title | What It Does |
|--------|-------|--------------|
| `d21ccd52d074` | Adopt radix tree for preserved memory tracking | Replaces xarray with a radix tree where leaf nodes are bitmaps of preserved pages. More memory-efficient; root PA can be passed to the new kernel. |
| `887744d84a8c` | Remove finalize state and clients | Simplifies the KHO lifecycle by removing the "finalize" phase and the client abstraction. Streamlines the API surface. |
| `ce6252935ab1` | Add radix tree initializer and metadata walk callback | Adds helper functions so drivers can initialize the radix tree and iterate over preserved pages via a callback. |
| `e22ec81e7215` | Add crash-kernel-safe radix tree presence check | Adds `kho_radix_tree_is_present()` that works even from a crash kernel context. Important for deciding whether to exclude pages from crashdump. |

**Key data structure:**
```
Radix Tree root → see "Radix Tree (Page Preservation Map)" section
above for detailed architecture and code.
```

### Group 2: MSHV Driver Kexec Resilience (`drivers/hv/`)

These make the MSHV (Microsoft Hypervisor) root partition driver survive kexec cleanly:

| Commit | Title | What It Does |
|--------|-------|--------------|
| `21abba29df09` | Limit SynIC management to MSHV-owned resources | SynIC (Synthetic Interrupt Controller) has multiple message/event slots. MSHV and VMBus each own different slots. This commit ensures MSHV only saves/restores its own slots during kexec, preventing corruption of VMBus-owned SynIC state. |
| `a4ea6f0b2235` | Clean up SynIC state on kexec for L1VH | Registers a **reboot notifier** that properly tears down SynIC interrupt state before kexec. Without this, the new kernel inherits stale interrupt vectors and crashes on the first synthetic interrupt. |
| `800d9d397787` | Unmap debugfs stats pages on kexec | The hypervisor maps statistics pages into the kernel. This unmaps them before kexec so the new kernel doesn't have stale virtual→physical mappings. |
| `d50c6a3b7fa6` | Use page tracker to manage MSHV-owned pages | **Core change.** Creates `mshv_page_preserve.c` — a new subsystem that tracks every page the hypervisor has "donated" (memory given to Hyper-V for guest VM backing). These pages are registered with KHO so they survive kexec. The new kernel knows not to touch them, and MSHV can reclaim them. |
| `21b3c95fd70c` | Add debugfs interface to page tracker | Exposes page preservation stats (total tracked, preserved count) via `/sys/kernel/debug/mshv/page_tracker`. |
| `8eb272b130d8` | Reserve crash MSR P2 for page preservation root PA | Repurposes Hyper-V crash register P2 to carry the radix tree root physical address. The crash MSR P2 previously stored `regs->pc`; now PC moves to P3 and SP to P4. This is how the new kernel discovers the preserved-page map. |
| `63f01cbba335` | Exclude Hyper-V donated pages from crash dump | Donated pages contain **guest memory**, not host debug info. Excluding them from crashdumps reduces dump size and avoids exposing guest data. |
| `b3bbbe72b141` | Freeze and vacuum partitions across kexec | Before kexec: (1) **freezes** all guest partitions — stops guest VP execution, (2) **vacuums** them — forces Hyper-V to reclaim lazily-held resources, ensuring a clean handoff to the new kernel. |

#### MSHV Page Preservation — Detailed Code Flow

**Step 1: During normal operation — track donated pages:**
```c
/* From drivers/hv/mshv_page_preserve.c */

static struct kho_radix_tree preserved_pages_tree;

/* Called whenever MSHV donates a page to the hypervisor */
int mshv_register_preserve_pages(struct page *pg, unsigned int order)
{
    unsigned long pfn = page_to_pfn(pg);
    return kho_radix_add_page(&preserved_pages_tree, pfn, order);
}

/* Called when hypervisor returns a page */
int mshv_unregister_preserve_pages(struct page *pg, unsigned int order)
{
    unsigned long pfn = page_to_pfn(pg);
    return kho_radix_del_page(&preserved_pages_tree, pfn, order);
}
```

**Step 2: On kexec — freeze partitions, preserve tree, register with KHO:**
```c
/* Reboot notifier registered during mshv_preserve_init() */
static int reboot_cb(struct notifier_block *nb, unsigned long action,
                     void *data)
{
    if (kexec_in_progress) {
        u64 *partition_ids;
        unsigned int nr_partition_ids;

        /* Freeze all guest partitions - stop VP execution */
        int err = mshv_freeze_and_get_partition_ids(&partition_ids,
                                                     &nr_partition_ids);
        if (err)
            panic("mshv_freeze_and_get_partition_ids() failed\n");

        /* Serialize and preserve the page tree */
        err = preserve_tree(partition_ids, nr_partition_ids);
        if (err)
            panic("preserve_tree() failed\n");
    }
    return NOTIFY_OK;
}

static int preserve_tree(u64 *partition_ids, unsigned int nr_partition_ids)
{
    /* 1. Freeze tree - no more additions allowed */
    int err = kho_radix_tree_freeze(&preserved_pages_tree);

    /* 2. Create FDT describing preserved pages */
    err = create_fdt(partition_ids, nr_partition_ids);

    /* 3. Walk radix tree: preserve all data & metadata pages via KHO */
    err = kho_radix_walk_tree(&preserved_pages_tree,
                               preserve_page_cb, preserve_page_cb);

    /* 4. Preserve the FDT page itself */
    err = kho_preserve_pages(virt_to_page(fdt_page), 1);

    /* 5. Register FDT as KHO subtree */
    err = kho_add_subtree(FDT_SUBTREE_MSHV, fdt_page);

    return 0;
}
```

**Step 3: In the new kernel — restore the tree and reclaim pages:**
```c
int __init mshv_preserve_init(void)
{
    if (is_kdump_kernel()) {
        /* Crash kernel: only setup dump exclusion */
        restore_crash_tree();
        register_vmcore_cb(&mshv_vmcore_cb);
        return 0;
    }

    if (!kho_is_enabled()) {
        pr_err("KHO is disabled\n");
        return 0;
    }

    int err = restore_tree();
    if (!err) {
        /* KHO boot: restore page structs so pages can be freed later */
        if (restore_page_structs())
            panic("Failed to restore page structs\n");
    } else if (err == -ENOENT) {
        /* Fresh boot: allocate a new empty tree */
        if (alloc_tree())
            return 0;
    } else {
        panic("Could not restore page tree from KHO\n");
    }

    /* Register reboot notifier for the next kexec */
    register_reboot_notifier(&reboot_notifier);

    /* Stash tree root PA in crash MSR for dump exclusion */
    hv_set_msr(HV_MSR_CRASH_P2,
               virt_to_phys(preserved_pages_tree.root));

    return 0;
}
```

#### Partition Freezing — Detailed Code Flow

```c
/* From drivers/hv/mshv_root_main.c */

int mshv_freeze_and_get_partition_ids(u64 **partition_ids,
                                       unsigned int *nr_ids)
{
    struct mshv_partition *partition;
    struct mshv_vp *vp;
    u64 *ids;

    /* Set global frozen flag — blocks new partition creation */
    scoped_guard(spinlock, &mshv_root.pt_ht_lock)
        mshv_root.frozen = true;

    /* Count and allocate preserved memory for partition IDs */
    ids = kho_alloc_preserve(nr_alloc * sizeof(*ids));

    /* For each live partition: */
    rcu_read_lock();
    hash_for_each_rcu(mshv_root.pt_htable, bkt, partition, pt_hnode) {
        ids[nr_ref++] = partition->pt_id;
    }
    rcu_read_unlock();

    /* Stop all VPs in each partition */
    for (i = 0; i < nr_ref; i++) {
        partition = mshv_partition_find(ids[i]);
        for (bkt = 0; bkt < MSHV_MAX_VPS; bkt++) {
            vp = partition->pt_vp_array[bkt];
            if (!vp) continue;

            /* Disable VP dispatch (root scheduler) */
            if (hv_scheduler_type == HV_SCHEDULER_TYPE_ROOT)
                disable_vp_dispatch(vp);

            /* Wake up VPs so they exit the run loop */
            wake_up_all(&vp->run.vp_suspend_queue);
        }

        /* Drain: wait for VPs to complete current ioctl */
        for (bkt = 0; bkt < MSHV_MAX_VPS; bkt++) {
            vp = partition->pt_vp_array[bkt];
            if (!vp) continue;
            scoped_guard(mutex, &vp->vp_mutex) {
                /* Suspend VP via hypercall if needed */
                if (hv_scheduler_type != HV_SCHEDULER_TYPE_ROOT)
                    mshv_suspend_vp(vp, &mif);
            }
        }
    }

    *partition_ids = ids;
    *nr_ids = nr_ref + nr_noref;
    return 0;
}
```

**Data flow for page preservation:**
```
MSHV deposits page to hypervisor
    → mshv_register_preserve_pages(page, order)
    → kho_radix_add_page(tree, pfn, order) — bit set in radix tree
    → kexec triggered
    → reboot_cb() → freeze partitions → preserve_tree()
        → kho_radix_tree_freeze() — lock tree
        → kho_radix_walk_tree() — preserve all tree pages
        → kho_add_subtree("mshv", fdt) — register with KHO
    → new kernel boots
    → mshv_preserve_init() → restore_tree()
        → kho_retrieve_subtree("mshv") — find FDT
        → kho_radix_tree_init(tree, root_pa) — restore tree
        → restore_page_structs() — reclaim page structs
    → guest VMs continue using their memory
```

### Group 3: x86-Specific Kexec Fixes (`arch/x86/`)

| Commit | Title | What It Does |
|--------|-------|--------------|
| `7099133ba192` | Move stimer cleanup to `hv_machine_shutdown()` | Ensures synthetic timers (stimer) are stopped **before** kexec. Without this, the new kernel gets unexpected timer interrupts from timers the old kernel set up. Calls `hv_stimer_global_cleanup()` across all CPUs. |
| `4f6dd20e704f` | Skip LP/VP creation on kexec | On x86, the root partition creates Logical Processors (LPs) and Virtual Processors (VPs) at boot via hypercalls. After kexec, these **already exist** in the hypervisor — re-creating them would `BUG_ON`. This commit detects kexec via `hv_lp_exists(1)` and skips creation. |
| `2423a7132136` | DONOTMERGE: trace SynIC MSR state | Temporary debug traces (`SYNIC-TRACE` pr_info) throughout SynIC init/shutdown. Not for production. |

---

## The Big Picture: Live Kernel Update for Hyper-V L1VH

### Goal
Update the host kernel on a Hyper-V root partition (L1) **without shutting down guest VMs**.

### Complete Flow with Code Entry Points

```
1. Host running guests via MSHV driver
   ├── Pages donated via mshv_register_preserve_pages() → tracked in radix tree
   ├── LUO sessions created via ioctl(/dev/liveupdate)
   └── Guests have memory backed by "donated" pages

2. Operator triggers live update (kexec)
   ├── liveupdate_reboot()                      [kernel/liveupdate/luo_core.c]
   │   ├── luo_session_serialize()              → preserve session metadata
   │   └── luo_flb_serialize()                  → preserve file descriptors
   ├── reboot_cb()                              [drivers/hv/mshv_page_preserve.c]
   │   ├── mshv_freeze_and_get_partition_ids()  → stop all VPs
   │   └── preserve_tree()
   │       ├── kho_radix_tree_freeze()          → lock tree
   │       ├── kho_radix_walk_tree()            → preserve all pages
   │       ├── kho_add_subtree("mshv", fdt)     → register with KHO
   │       └── hv_set_msr(CRASH_P2, root_pa)   → backup for crash kernel
   └── kho_fill_kimage()                        → package FDT + scratch

3. kexec loads new kernel into scratch regions
   └── New kernel starts from scratch region

4. New kernel running
   ├── kho_populate()                           [kernel/liveupdate/kexec_handover.c]
   │   └── memblock marks preserved pages as reserved
   ├── kho_init()                               → restore radix tree + FDT
   ├── mshv_preserve_init()                     [drivers/hv/mshv_page_preserve.c]
   │   ├── restore_tree()                       → restore MSHV page tree
   │   └── restore_page_structs()               → reclaim page structs
   ├── luo_session_deserialize()                → restore userspace sessions
   └── Guest partitions unfrozen
       └── Guests resume — never knew the host rebooted
```

---

## ARM64 Status and Gaps

### Already Done
| Item | Status |
|------|--------|
| `ARCH_SUPPORTS_KEXEC_HANDOVER` in arm64 Kconfig | ✅ Present (`def_bool y`) |
| Crash MSR P2 reservation for radix tree root PA | ✅ Done (`arch/arm64/hyperv/hv_core.c`) |
| KHO enabled in `mshv_defconfig` | ✅ Done (added `CONFIG_KEXEC_HANDOVER=y` + debugfs + enable_default) |
| All `drivers/hv/` changes compile on arm64 | ✅ Build passes |
| All `kernel/liveupdate/` changes compile on arm64 | ✅ Build passes |

### Gaps Requiring ARM64 Work

#### 1. Kexec Shutdown Hook (HIGH priority)
**Problem:** ARM64 has no `hv_machine_shutdown()` to call `hv_stimer_global_cleanup()`, `cpuhp_remove_state()`, and `hyperv_cleanup()` before kexec.

**x86 does this in `hv_machine_shutdown()`** via `machine_ops` — ARM64 has no equivalent hook.

**Options:**
- (a) Register a **reboot notifier** from `arch/arm64/hyperv/mshyperv.c`
- (b) Hook into `machine_shutdown()` directly (arm64 doesn't use `machine_ops`)
- (c) Rely on cpuhp teardown ordering

**Risk if not done:** Stale stimer interrupts crash the new kernel.

#### 2. LP/VP Creation on Kexec (HIGH priority)
**Problem:** x86 creates Logical Processors and Virtual Processors at boot. After kexec, they already exist — must be skipped.

**ARM64 currently does NOT call `hv_call_add_logical_proc()` or `hv_call_create_vp()`** — needs investigation whether ARM64 L1VH does this via a different path or relies on the hypervisor.

**Risk if not done:** If ARM64 does LP/VP creation, re-creating them after kexec will `BUG_ON`.

#### 3. SynIC Debug Traces (LOW priority)
**Problem:** No arm64 equivalent of the `DONOTMERGE` SynIC traces.

**Action:** Optional — add when debugging arm64 kexec issues. Do not merge.

---

## Architecture Differences: x86 vs ARM64

| Aspect | x86 | ARM64 |
|--------|-----|-------|
| Machine shutdown override | `hv_machine_shutdown()` via `machine_ops` | No override mechanism |
| Hypervisor cleanup | Disables hypercall page, TSC ref | Zeros Guest OS ID in `hyperv_cleanup()` |
| SMP prepare hook | `hv_smp_prepare_cpus()` | No SMP prepare override |
| LP/VP creation | Done in `hv_smp_prepare_cpus()` | **Not done** — needs investigation |
| MSR access | `wrmsrq()`/`rdmsrq()` | `hv_set_vpreg()`/`hv_get_vpreg()` |
| KHO Kconfig | `ARCH_SUPPORTS_KEXEC_HANDOVER` = X86_64 | `ARCH_SUPPORTS_KEXEC_HANDOVER` = y |

---

## Key Files Reference

| File | Role |
|------|------|
| `kernel/liveupdate/kexec_handover.c` | Core KHO subsystem — FDT creation, page preservation, scratch regions |
| `include/linux/kho_radix_tree.h` | Radix tree API for page preservation tracking |
| `drivers/hv/mshv_page_preserve.c` | MSHV page tracker — tracks donated pages, registers with KHO |
| `drivers/hv/mshv_root_main.c` | MSHV root driver — partition freeze/vacuum, reboot notifier |
| `drivers/hv/mshv_synic.c` | SynIC management — interrupt setup/teardown scoped to MSHV |
| `drivers/hv/mshv_debugfs.c` | Debugfs interfaces for stats pages and page tracker |
| `arch/arm64/hyperv/hv_core.c` | ARM64 crash MSR P2 reservation for radix tree root |
| `arch/arm64/hyperv/mshyperv.c` | ARM64 Hyper-V platform init (needs kexec shutdown hook) |
| `arch/arm64/configs/mshv_defconfig` | ARM64 MSHV kernel config (KHO now enabled) |
