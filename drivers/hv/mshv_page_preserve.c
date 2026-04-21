// SPDX-License-Identifier: GPL-2.0-only
/*
 * Preserve pages owned by Microsoft Hypervisor
 *
 * When handing pages to MSHV and kexec'ing, the next kernel needs to know which
 * pages not to touch. Handles this preservation here.
 *
 * Copyright (C) 2026 Microsoft Corporation, Jork Loeser <jloeser@microsoft.com>
 */

#define pr_fmt(fmt) "mshv: " fmt

#include <asm/mshyperv.h>
#include <linux/crash_dump.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/libfdt.h>
#include <linux/mmzone.h>		/* MAX_POSSIBLE_PHYSMEM_BITS */
#include <linux/reboot.h>
#include "mshv_root.h"
#include "mshv_page_preserve.h"

#define FDT_SUBTREE_MSHV "mshv_prsv_pt"
#define MSHV_KHO_COMPAT_STR "mshv_kho-v1"

static void *fdt_page;
static struct kho_radix_tree preserved_pages_tree;

/**
 * mshv_register_preserve_pages() - Register pages to be preserved by KHO
 * @pg: pointer to the first page to preserve
 * @order: order of the page range to preserve (0 for single page)
 *
 * Registers a contiguous range of pages to be preserved by KHO across kexec.
 *
 * Return: 0 on success, -errno on failure.
 */
int mshv_register_preserve_pages(struct page *pg, unsigned int order)
{
	unsigned long pfn = page_to_pfn(pg);

	return kho_radix_add_page(&preserved_pages_tree, pfn, order);
}

/**
 * mshv_unregister_preserve_pages() - Unregister pages from KHO preservation
 * @pg: pointer to the first page to unpreserve
 * @order: order of the page range to unpreserve (0 for single page)
 *
 * Unregisters a contiguous range of pages that were previously registered to be
 * preserved by KHO.
 *
 * Return: 0 on success, -errno on failure.
 */
int mshv_unregister_preserve_pages(struct page *pg, unsigned int order)
{
	unsigned long pfn = page_to_pfn(pg);

	return kho_radix_del_page(&preserved_pages_tree, pfn, order);
}

/**
 * mshv_iterate_preserved() - Walk all preserved page ranges
 * @cb_data: callback invoked for each preserved page range
 * @cb_meta: callback invoked for each radix-tree metadata page
 *
 * Return: 0 on success, -errno on failure.
 */
int mshv_iterate_preserved(kho_radix_tree_walk_callback_t cb_data,
			   kho_radix_tree_walk_callback_t cb_meta)
{
	return kho_radix_walk_tree(&preserved_pages_tree, cb_data, cb_meta);
}
EXPORT_SYMBOL_GPL(mshv_iterate_preserved);

/* register individual page-ranges with KHO */
static int preserve_page_cb(phys_addr_t phys, unsigned int order)
{
	return kho_preserve_pages(phys_to_page(phys), BIT(order));
}

static int create_fdt(u64 *partition_ids, unsigned int nr_partition_ids)
{
	int err;
	void *fdt;
	phys_addr_t root_table;

	if (!fdt_page)
		return -EINVAL;

	fdt = fdt_page;

	err = fdt_create(fdt, PAGE_SIZE);
	if (err)
		return err;
	err = fdt_finish_reservemap(fdt);
	if (err)
		return err;
	err = fdt_begin_node(fdt, "");
	if (err)
		return err;
	err = fdt_property(fdt, "compatible", MSHV_KHO_COMPAT_STR,
			   strlen(MSHV_KHO_COMPAT_STR) + 1);
	if (err)
		return err;
	root_table = virt_to_phys(preserved_pages_tree.root);
	err = fdt_property(fdt, "root_table", &root_table, sizeof(root_table));
	if (err)
		return err;
	if (nr_partition_ids) {
		phys_addr_t ids_pa = virt_to_phys(partition_ids);
		u32 count = nr_partition_ids;

		err = fdt_property(fdt, "partition_ids", &ids_pa,
				   sizeof(ids_pa));
		if (err)
			return err;
		err = fdt_property(fdt, "nr_partition_ids", &count,
				   sizeof(count));
		if (err)
			return err;
	}
	err = fdt_end_node(fdt);
	if (err)
		return err;
	err = fdt_finish(fdt);
	if (err)
		return err;

	return 0;
}

/**
 * preserve_tree() - Preserve pages owned by Microsoft Hypervisor
 *
 * This gets called prior to kexec and is our signal to finally preserve the
 * pages with KHO, and create & register the named FDT. We also need to freeze
 * the tree, since we cannot communicate any later changes.
 *
 * Return: 0 on success, -errno on error.
 */
static int preserve_tree(u64 *partition_ids, unsigned int nr_partition_ids)
{
	int err;

	err = kho_radix_tree_freeze(&preserved_pages_tree);
	if (err) {
		pr_warn("%s() - kho_radix_tree_freeze() failed: %d\n",
			__func__, err);
		return err;
	}

	/* Populate the pre-allocated FDT page with current tree state */
	err = create_fdt(partition_ids, nr_partition_ids);
	if (err) {
		pr_warn("%s() - create_fdt() failed: %d\n", __func__, err);
		return err;
	}

	/*
	 * Preserve both data- and meta-pages. Intentional same callback for
	 * both.
	 */
	err = kho_radix_walk_tree(&preserved_pages_tree, preserve_page_cb,
				  preserve_page_cb);
	if (err) {
		/* We could not preserve all pages and cannot kexec. */
		pr_warn("%s() - kho_radix_walk_tree() failed: %d\n", __func__,
			err);
		return err;
	}

	err = kho_preserve_pages(virt_to_page(fdt_page), 1);
	if (err) {
		pr_warn("%s() - kho_preserve_pages(fdt) failed: %d\n", __func__,
			err);
		return err;
	}

	err = kho_add_subtree(FDT_SUBTREE_MSHV, fdt_page, fdt_totalsize(fdt_page));
	if (err) {
		/* KHO will abort and undo all preservations. We cannot kexec. */
		pr_warn("%s() - kho_add_subtree() failed: %d\n", __func__, err);
		return err;
	}

	pr_debug("%s() - success\n", __func__);
	return 0;
}

/*
 * Reboot-callback triggering page preservation prior to kexec. Other reboots
 * need no KHO preservation.
 */
static int reboot_cb(struct notifier_block *nb, unsigned long action,
		     void *data)
{
	/* codes such as SYS_RESTART, SYS_HALT do not convey kexec specifically */
	if (kexec_in_progress) {
		u64 *partition_ids;
		unsigned int nr_partition_ids;
		int err;

		/*
		 * Stop all VPs so no guest can modify memory that Linux will
		 * re-use after kexec, then preserve the page tree.
		 */
		err = mshv_freeze_and_get_partition_ids(&partition_ids,
						        &nr_partition_ids);
		if (err)
			panic("mshv_freeze_and_get_partition_ids() failed - must not kexec: %d\n",
			      err);

		pr_debug("%s() - KHO-preserving page tree\n", __func__);
		err = preserve_tree(partition_ids, nr_partition_ids);
		if (err)
			panic("preserve_tree() failed - must not kexec: %d\n",
			      err);
	}
	return NOTIFY_OK;
}

/**
 * restore_tree() - Restore the page-tree state from KHO.
 *
 * Return: 0 on success, -ENOENT if no KHO subtree was found (i.e. this is
 *         not a KHO boot), -EINVAL if the preserved FDT is malformed or
 *         incompatible.
 */
static int __init restore_tree(void)
{
	void *fdt;
	phys_addr_t fdt_pa;
	size_t fdt_size;
	int len;
	int node;
	const phys_addr_t *root_table_fdt_ptr;
	int err;

	err = kho_retrieve_subtree(FDT_SUBTREE_MSHV, &fdt_pa, &fdt_size);
	if (err)
		return err;

	fdt = phys_to_virt(fdt_pa);
	node = fdt_path_offset(fdt, "/");
	if (node < 0) {
		pr_err("Could not find root node in KHO-preserved FDT.\n");
		return -EINVAL;
	}

	if (fdt_node_check_compatible(fdt, node, MSHV_KHO_COMPAT_STR)) {
		/*
		 * This is unfortunate. We kexec'd into a kernel that isn't
		 * compatible with prior preservations. Pages this kernel
		 * considers available might actually be held by MSHV. The only
		 * recourse is to reboot.
		 */
		const char *s = fdt_getprop(fdt, node, "compatible", &len);

		if (s && len >= 0)
			pr_err("Incompatible kernel: Current is %s, preserved is %.*s\n",
			       MSHV_KHO_COMPAT_STR, len, s);
		else
			pr_err("Incompatible kernel: preserved misses 'compatible' mark.\n");
		return -EINVAL;
	}

	root_table_fdt_ptr = fdt_getprop(fdt, node, "root_table", &len);
	if (!root_table_fdt_ptr || len != sizeof(*root_table_fdt_ptr)) {
		pr_err("Could not obtain root_table property from KHO-preserved FDT.\n");
		return -EINVAL;
	}

	/* Restore struct page so it could be freed if needed */
	if (!kho_restore_pages(fdt_pa, 1))
		return -EINVAL;

	fdt_page = phys_to_virt(fdt_pa);

	err = kho_radix_tree_init(&preserved_pages_tree, *root_table_fdt_ptr);
	if (err)
		return -EINVAL;

	pr_debug("Restored tracking from KHO.\n");
	return 0;
}

/**
 * mshv_retrieve_frozen_partition_ids() - Retrieve frozen partition IDs
 * @partition_ids: receives pointer to the preserved ID array, or NULL
 * @nr_ids: receives the number of entries, or 0
 *
 * Counterpart to mshv_freeze_and_get_partition_ids(). Reads the partition
 * ID list from the KHO-preserved FDT. The returned pointer (if non-NULL)
 * refers to kho_alloc_preserve()'d memory from the previous kernel.
 *
 * Return: 0 on success (including when no IDs are found), negative errno on
 *  error.
 */
int __init mshv_retrieve_frozen_partition_ids(u64 **partition_ids,
					      unsigned int *nr_ids)
{
	int node, len;
	const phys_addr_t *ids_pa;
	const u32 *count_prop;

	*partition_ids = NULL;
	*nr_ids = 0;

	if (!fdt_page) {
		pr_info("KHO-TRACE: No KHO FDT page - not a kexec boot\n");
		return 0;
	}

	node = fdt_path_offset(fdt_page, "/");
	if (node < 0)
		return 0;

	ids_pa = fdt_getprop(fdt_page, node, "partition_ids", &len);
	if (!ids_pa)
		return 0;

	if (len != sizeof(*ids_pa)) {
		pr_err("Malformed preserved FDT: invalid partition_ids property.\n");
		return -EINVAL;
	}

	count_prop = fdt_getprop(fdt_page, node, "nr_partition_ids", &len);
	if (!count_prop || len != sizeof(*count_prop)) {
		pr_err("Malformed preserved FDT: invalid nr_partition_ids property.\n");
		return -EINVAL;
	}

	*nr_ids = *count_prop;

	/* Validate the partition_ids array has enough backing memory */
	if (*nr_ids > (PAGE_SIZE / sizeof(u64))) {
		pr_err("Malformed preserved FDT: nr_partition_ids=%u exceeds page\n",
		       *nr_ids);
		*nr_ids = 0;
		return -EINVAL;
	}

	*partition_ids = phys_to_virt(*ids_pa);

	pr_info("KHO-TRACE: Retrieved %u frozen partition ID(s) from KHO (ids_pa=%pa)\n", *nr_ids, ids_pa);
	{
		unsigned int j;
		for (j = 0; j < *nr_ids; j++)
			pr_info("KHO-TRACE:   partition_id[%u] = %llu\n", j, (*partition_ids)[j]);
	}
	return 0;
}

/*
 * Restore individual pages using KHO's helper during boot.
 *
 * Pages must be restored one at a time because they were deposited to
 * the hypervisor individually and will be withdrawn individually later.
 * Restoring them as a higher-order group would create compound pages
 * that cannot be freed with __free_page().
 */
static int __init restore_page_cb(phys_addr_t phys, unsigned int order)
{
	unsigned int i;

	for (i = 0; i < BIT(order); i++) {
		if (!kho_restore_pages(phys + i * PAGE_SIZE, 1))
			return -EINVAL;
	}
	return 0;
}

/**
 * restore_page_structs() - Restore page-structs so they can be __free_page()'d
 *
 * This is necessary because KHO-preserved pages are in a "weird" state
 * post-kexec. While doing so here in bulk adds to boot time, there is no vetted
 * alternative that would allow doing this later, when we cannot say which pages
 * had been freshly added, and which came into the tree through KHO.
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init restore_page_structs(void)
{
	return kho_radix_walk_tree(&preserved_pages_tree, restore_page_cb,
				  restore_page_cb);
}

/**
 * alloc_tree() - Allocate a fresh page tree and FDT page.
 *
 * Called on fresh boot (no KHO data). Allocates an empty radix tree and
 * the FDT page used to serialize state before kexec.
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init alloc_tree(void)
{
	int err;

	fdt_page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!fdt_page)
		return -ENOMEM;

	err = kho_radix_tree_init(&preserved_pages_tree, 0);
	if (err) {
		free_page((unsigned long)fdt_page);
		fdt_page = NULL;
		return err;
	}

	return 0;
}

#ifdef CONFIG_CRASH_DUMP
static struct kho_radix_crash_tree crash_preserved_pages_tree;

/**
 * restore_crash_tree() - Set up the crash tree for dump-time page exclusion.
 *
 * In the crash kernel, the old kernel's memory is not in the direct map.
 * The old kernel stashes the radix tree root PA in Hyper-V crash MSR P2
 * so we can retrieve it without touching the old kernel's FDT.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int __init restore_crash_tree(void)
{
	phys_addr_t root_pa;

	root_pa = hv_get_msr(HV_MSR_CRASH_P2);
	if (!root_pa)
		return -ENOENT;

	/*
	 * The MSR may contain stale data from a previous
	 * hyperv_report_panic().  Sanity-check that it looks like a
	 * page-aligned physical address within the architectural limit.
	 */
	if (!PAGE_ALIGNED(root_pa) || root_pa >> MAX_POSSIBLE_PHYSMEM_BITS) {
		pr_warn("Invalid crash tree root PA: 0x%llx\n",
			(unsigned long long)root_pa);
		return -EINVAL;
	}

	return kho_radix_crash_init(&crash_preserved_pages_tree, root_pa);
}

static bool mshv_vmcore_pfn_is_ram(struct vmcore_cb *cb, unsigned long pfn)
{
	/*
	 * MSHV-owned pages must not be read during crash dump collection.
	 * Currently all pages are registered at order 0. If higher-order
	 * registrations are added, this lookup will need to handle them
	 * (e.g. by querying multiple orders or using a range-based API).
	 */
	return !kho_radix_crash_contains_page(&crash_preserved_pages_tree,
					      pfn, 0);
}

static struct vmcore_cb mshv_vmcore_cb = {
	.pfn_is_ram = mshv_vmcore_pfn_is_ram,
};
#endif

static struct notifier_block reboot_notifier = {
	.notifier_call = reboot_cb,
	.priority = 0,
};

/**
 * mshv_preserve_init() - Initialize the page preservation
 *
 * Upon return:
 * - the tracker will be ready for use (restored post-kexec, or empty
 *   post-reboot),
 * - restored pages will be in a state that can be __free_page()'d,
 * - KHO notification for preservation will be registered.
 *
 * Return: 0 on success, -errno on error.
 */
int __init mshv_preserve_init(void)
{
	int err;

#ifdef CONFIG_CRASH_DUMP
	if (is_kdump_kernel()) {
		/*
		 * Crash kernel only needs the pfn_is_ram callback to exclude
		 * MSHV-owned pages from the dump.  No page restoration, no
		 * reboot notifier — the crash kernel reboots after collection.
		 */
		err = restore_crash_tree();
		if (err) {
			pr_err("Could not set up crash page tree: %d; MSHV pages may appear in dump\n", err);
			return 0;
		}
		register_vmcore_cb(&mshv_vmcore_cb);
		return 0;
	}
#endif

	if (!kho_is_enabled()) {
		pr_err("KHO is disabled; page deposits will fail.\n");
		return 0;
	}

	err = restore_tree();
	if (!err) {
		/* Restore struct pages so they can be __free_page()'d */
		if (restore_page_structs())
			/*
			 * Unrestored struct pages would BUG when freed
			 * at withdraw time.
			 */
			panic("Failed to restore MSHV page structs\n");
	} else if (err == -ENOENT) {
		pr_debug("Nothing to restore from KHO.\n");
		if (alloc_tree()) {
			pr_err("Could not allocate page tree; page deposits will fail.\n");
			return 0;
		}
	} else {
		/*
		 * Pages from the prior kernel are held by MSHV but we
		 * lost track of them -- memory corruption is inevitable.
		 */
		panic("Could not restore page tree from KHO: %d\n", err);
	}

	err = register_reboot_notifier(&reboot_notifier);
	if (err)
		/*
		 * Deposits would succeed but pages would not be preserved
		 * across kexec, causing memory corruption post-kexec.
		 */
		panic("Could not register reboot notification: %d\n", err);

	/*
	 * Stash the radix tree root PA in crash MSR P2 so the crash
	 * kernel can retrieve it without touching the old kernel's FDT
	 * (which is not in the crash kernel's direct map).  The root
	 * pointer is stable once the tree is initialized — pages are
	 * added/removed within the existing tree structure.
	 */
	hv_set_msr(HV_MSR_CRASH_P2,
		   virt_to_phys(preserved_pages_tree.root));

	return 0;
}
