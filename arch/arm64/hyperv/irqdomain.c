// SPDX-License-Identifier: GPL-2.0
/*
 * Irqdomain for Linux to run as the root partition on Microsoft Hypervisor.
 *
 * Authors:
 *  Sunil Muthuswamy <sunilmut@microsoft.com>
 *  Wei Liu <wei.liu@kernel.org>
 */

#include <linux/of_irq.h>
#include <linux/hyperv.h>
#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/export.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/irqchip/irq-msi-lib.h>
#include <asm/mshyperv.h>
#include <linux/acpi.h>

static u64 hv_map_interrupt_hcall(u64 ptid, union hv_device_id hv_devid,
				  bool level, int cpu, int vector,
				  struct hv_interrupt_entry *ret_entry)
{
	struct hv_input_map_device_interrupt *input;
	struct hv_output_map_device_interrupt *output;
	struct hv_device_interrupt_descriptor *intr_desc;
	unsigned long flags;
	u64 status;
	int nr_bank, var_size;

	local_irq_save(flags);

	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = *this_cpu_ptr(hyperv_pcpu_output_arg);

	intr_desc = &input->interrupt_descriptor;
	memset(input, 0, sizeof(*input));

	input->partition_id = ptid;
	input->device_id = hv_devid.as_uint64;

	intr_desc->interrupt_type = HV_ARM64_INTERRUPT_TYPE_FIXED;
	intr_desc->vector_count = 1;
	intr_desc->target.vector = vector;

	if (level)
		intr_desc->trigger_mode = HV_INTERRUPT_TRIGGER_MODE_LEVEL;
	else
		intr_desc->trigger_mode = HV_INTERRUPT_TRIGGER_MODE_EDGE;

	intr_desc->target.vp_set.valid_bank_mask = 0;
	intr_desc->target.vp_set.format = HV_GENERIC_SET_SPARSE_4K;
	nr_bank = cpumask_to_vpset(&intr_desc->target.vp_set, cpumask_of(cpu));
	if (nr_bank < 0) {
		local_irq_restore(flags);
		pr_err("%s: unable to generate VP set\n", __func__);
		return -EINVAL;
	}
	intr_desc->target.flags = HV_DEVICE_INTERRUPT_TARGET_PROCESSOR_SET;

	/*
	 * var-sized hypercall, var-size starts after vp_mask (thus
	 * vp_set.format does not count, but vp_set.valid_bank_mask
	 * does).
	 */
	var_size = nr_bank + 1;

	status = hv_do_rep_hypercall(HVCALL_MAP_DEVICE_INTERRUPT, 0, var_size,
			input, output);
	*ret_entry = output->interrupt_entry;

	local_irq_restore(flags);

	return status;
}

static int hv_map_interrupt(u64 ptid, union hv_device_id device_id, bool level,
			    int cpu, int vector,
			    struct hv_interrupt_entry *ret_entry)
{
	u64 status;
	int rc, deposit_pgs = 16;		/* don't loop forever */

	while (deposit_pgs--) {
		status = hv_map_interrupt_hcall(ptid, device_id, level, cpu,
						vector, ret_entry);

		if (hv_result(status) != HV_STATUS_INSUFFICIENT_MEMORY)
			break;

		rc = hv_call_deposit_pages(NUMA_NO_NODE, ptid, 1);
		if (rc)
			break;
	}

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	return hv_result_to_errno(status);
}

static int hv_unmap_interrupt(u64 id, struct hv_interrupt_entry *irq_entry)
{
	unsigned long flags;
	struct hv_input_unmap_device_interrupt *input;
	u64 status;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);

	memset(input, 0, sizeof(*input));
	input->partition_id = hv_current_partition_id;
	input->device_id = id;
	input->interrupt_entry = *irq_entry;

	status = hv_do_hypercall(HVCALL_UNMAP_DEVICE_INTERRUPT, input, NULL);
	local_irq_restore(flags);

	if (!hv_result_success(status))
		hv_status_err(status, "\n");

	return hv_result_to_errno(status);
}

#ifdef CONFIG_PCI_MSI
struct rid_data {
	struct pci_dev *bridge;
	u32 rid;
};

static int get_rid_cb(struct pci_dev *pdev, u16 alias, void *data)
{
	struct rid_data *rd = data;
	u8 bus = PCI_BUS_NUM(rd->rid);

	if (pdev->bus->number != bus || PCI_BUS_NUM(alias) != bus) {
		rd->bridge = pdev;
		rd->rid = alias;
	}

	return 0;
}

u64 hv_build_devid_type_pci(struct pci_dev *pdev)
{
	union hv_device_id hv_devid;
	struct rid_data data = {
		.bridge = NULL,
		.rid = PCI_DEVID(pdev->bus->number, pdev->devfn)
	};

	pci_for_each_dma_alias(pdev, get_rid_cb, &data);

	hv_devid.as_uint64 = 0;
	hv_devid.device_type = HV_DEVICE_TYPE_PCI;
	hv_devid.pci.segment = pci_domain_nr(pdev->bus);

	hv_devid.pci.bdf.bus = PCI_BUS_NUM(data.rid);
	hv_devid.pci.bdf.device = PCI_SLOT(data.rid);
	hv_devid.pci.bdf.function = PCI_FUNC(data.rid);
	hv_devid.pci.source_shadow = HV_SOURCE_SHADOW_NONE;

	return hv_devid.as_uint64;
}

EXPORT_SYMBOL_GPL(hv_build_devid_type_pci);

/*
 * Map the MSI doorbell page into the default S2 device domain so the device's
 * MSI write does not fault in the SMMU. For pthru devices the iommu driver
 * manages its own S2 domain; this path is only taken for the default domain.
 */
static u64 hv_iommu_map_msi_doorbell(phys_addr_t paddr, unsigned long npages,
				     u32 map_flags)
{
	struct hv_input_map_device_gpa_pages *input;
	unsigned long flags, pfn;
	u64 status;
	int i;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));

	input->device_domain.partition_id = HV_PARTITION_ID_SELF;
	input->device_domain.domain_id.type = HV_DEVICE_DOMAIN_TYPE_S2;
	input->device_domain.domain_id.id = HV_DEVICE_DOMAIN_ID_S2_DEFAULT;
	input->map_flags = map_flags;
	input->target_device_va_base = paddr;

	pfn = paddr >> HV_HYP_PAGE_SHIFT;
	for (i = 0; i < npages; i++, pfn++)
		input->gpa_page_list[i] = pfn;

	status = hv_do_rep_hypercall(HVCALL_MAP_DEVICE_GPA_PAGES, npages, 0,
				     input, NULL);

	local_irq_restore(flags);
	return status;
}

static unsigned int hv_msi_get_int_vector(struct irq_data *irqd)
{
	return irqd->parent_data->hwirq;
}
/*
 * hv_map_msi_interrupt() - Map the MSI IRQ in the hypervisor.
 * @data:      Describes the IRQ
 * @out_entry: Hypervisor (MSI) interrupt entry (can be NULL)
 *
 * Map the IRQ in the hypervisor by issuing a MAP_DEVICE_INTERRUPT hypercall.
 *
 * Return: 0 on success, -errno on failure
 */
int hv_map_msi_interrupt(struct irq_data *data,
			 struct hv_interrupt_entry *out_entry)
{
	int vector = hv_msi_get_int_vector(data);
	struct hv_interrupt_entry dummy;
	union hv_device_id hv_devid;
	struct msi_desc *msidesc;
	struct pci_dev *pdev;
	int cpu;

	msidesc = irq_data_get_msi_desc(data);
	pdev = msi_desc_to_pci_dev(msidesc);
	hv_devid.as_uint64 = hv_devid_from_pdev(pdev);
	cpu = cpumask_first(irq_data_get_effective_affinity_mask(data));

	return hv_map_interrupt(hv_current_partition_id, hv_devid, false, cpu,
				vector, out_entry ? out_entry : &dummy);
}
EXPORT_SYMBOL_GPL(hv_map_msi_interrupt);

static void entry_to_msi_msg(struct hv_interrupt_entry *entry,
			     struct msi_msg *msg)
{
	/* High address is always 0 */
    msg->address_hi = upper_32_bits(entry->msi_entry.address);
    msg->address_lo = lower_32_bits(entry->msi_entry.address);
    msg->data = entry->msi_entry.data;
}

static int hv_unmap_msi_interrupt(struct pci_dev *pdev,
				  struct hv_interrupt_entry *irq_entry);

static void hv_irq_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct hv_interrupt_entry *stored_entry;
    struct irq_data *parent_data = data->parent_data;
	struct msi_desc *msidesc;
	struct pci_dev *pdev;
	int ret;

	msidesc = irq_data_get_msi_desc(data);
	pdev = msi_desc_to_pci_dev(msidesc);

	if (!parent_data) {
		pr_debug("%s: parent_data is NULL", __func__);
		return;
	}

	/*
	 * For direct attached devices, we cannot map interrupts in the
	 * hypervisor because it will not allow it until we have guest target
	 * vcpu and vector. So defer it until irqbypass. Also, do the same
	 * for domain attached devices for simplicity.
	 */
	if (hv_pcidev_is_pthru_dev(pdev)) {
		if (data->chip_data)
			entry_to_msi_msg(data->chip_data, msg);
		else
			memset(msg, 0, sizeof(struct msi_msg));
		return;
	}

	if (data->chip_data) {
		/*
		 * This interrupt is already mapped. Let's unmap first.
		 *
		 * We don't use retarget interrupt hypercalls here because
		 * Microsoft Hypervisor doesn't allow root to change the vector
		 * or specify VPs outside of the set that is initially used
		 * during mapping.
		 */
		stored_entry = data->chip_data;
		data->chip_data = NULL;

		ret = hv_unmap_msi_interrupt(pdev, stored_entry);

		kfree(stored_entry);

		if (ret)
			return;
	}

	stored_entry = kzalloc_obj(*stored_entry, GFP_ATOMIC);
	if (!stored_entry)
		return;

	ret = hv_map_msi_interrupt(data, stored_entry);
	if (ret) {
		kfree(stored_entry);
		return;
	}

	data->chip_data = stored_entry;
	entry_to_msi_msg(data->chip_data, msg);

	/*
	 * Map the MSI doorbell page into the default S2 device domain so the
	 * device's MSI write reaches the GIC ITS through the SMMU.
	 */
	{
		u64 status;

		status = hv_iommu_map_msi_doorbell(stored_entry->msi_entry.address & HV_HYP_PAGE_MASK,
						   1, HV_MAP_GPA_READABLE | HV_MAP_GPA_WRITABLE);
		if (!hv_result_success(status))
			pr_err("%s: failed to map MSI doorbell, status 0x%llx\n",
			       __func__, status);
	}
}

static int hv_unmap_msi_interrupt(struct pci_dev *pdev,
				  struct hv_interrupt_entry *irq_entry)
{
	union hv_device_id hv_devid;

	hv_devid.as_uint64 = hv_devid_from_pdev(pdev);
	return hv_unmap_interrupt(hv_devid.as_uint64, irq_entry);
}

/* NB: during map, hv_interrupt_entry is saved via data->chip_data */
static void hv_teardown_msi_irq(struct pci_dev *pdev, struct irq_data *irqd)
{
	struct hv_interrupt_entry irq_entry;
	struct msi_msg msg;

	if (!irqd->chip_data) {
		pr_debug("%s: no chip data\n!", __func__);
		return;
	}

	irq_entry = *(struct hv_interrupt_entry *)irqd->chip_data;
	entry_to_msi_msg(&irq_entry, &msg);

	kfree(irqd->chip_data);
	irqd->chip_data = NULL;

	(void)hv_unmap_msi_interrupt(pdev, &irq_entry);
}

/*
 * Hyper-V root: enable an LPI by writing its byte in the redistributor LPI
 * configuration table (ENABLED | GROUP1) and invalidating the cached entry
 * via GICR_INVALLR on the target redistributor.  We cannot delegate to the
 * parent's irq_unmask because dom0 does not enumerate an ITS and the
 * standard GIC-v3 LPI chip ops are not on this hierarchy.
 *
 * The cache-flushing flag is private to drivers/irqchip/irq-gic-common.h
 * (RDIST_FLAGS_PROPBASE_NEEDS_FLUSHING = 1<<0); we cannot include that
 * driver-internal header from arch/arm64/hyperv/, so redefine locally.
 * Keep the value in sync with irq-gic-common.h.
 */
#define HV_RDIST_FLAGS_PROPBASE_NEEDS_FLUSHING	(1U << 0)

static void hv_irq_unmask(struct irq_data *irqd)
{
	struct rdists *rdists = gic_get_rdists();
	void *prop_va = rdists->prop_table_va;
	const struct cpumask *aff;
	irq_hw_number_t hwirq;
	int cpu;
	u8 *cfg;

	if (!irqd->parent_data) {
		pr_debug("%s: parent_data NULL\n", __func__);
		return;
	}

	hwirq = irqd->parent_data->hwirq;
	aff = irq_data_get_effective_affinity_mask(irqd);
	cpu = cpumask_first(aff);

	cfg = (u8 *)prop_va + hwirq - 8192;
	*cfg |= LPI_PROP_ENABLED | LPI_PROP_GROUP1;

	if (rdists->flags & HV_RDIST_FLAGS_PROPBASE_NEEDS_FLUSHING)
		gic_flush_dcache_to_poc(cfg, sizeof(*cfg));
	else
		dsb(ishst);

	gic_write_lpir(0, per_cpu_ptr(rdists->rdist, cpu)->rd_base + GICR_INVALLR);

	pci_msi_unmask_irq(irqd);
}

/*
 * Microsoft Hypervisor doesn't allow root to change the vector or specify
 * VPs outside the set used during initial map.  Retarget is achieved by
 * unmap+remap in compose_msi_msg on the next delivery.  Just record the
 * new effective affinity here.
 */
static int hv_irq_set_affinity(struct irq_data *irqd,
			       const struct cpumask *mask, bool force)
{
	int cpu = cpumask_first(mask);

	irq_data_update_effective_affinity(irqd, cpumask_of(cpu));
	return IRQ_SET_MASK_OK_DONE;
}

/*
 * IRQ Chip for MSI PCI/PCI-X/PCI-Express Devices,
 * which implement the MSI or MSI-X Capability Structure.
 */
static struct irq_chip hv_pci_msi_controller = {
	.name			= "HV-PCI-MSI",
	.irq_compose_msi_msg	= hv_irq_compose_msi_msg,
	.irq_set_affinity	= hv_irq_set_affinity,
	.irq_ack		= irq_chip_ack_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_mask		= pci_msi_mask_irq,
	.irq_unmask		= hv_irq_unmask,
};

static bool hv_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				 struct irq_domain *real_parent,
				 struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;

	if (!msi_lib_init_dev_msi_info(dev, domain, real_parent, info))
		return false;

	chip->flags |= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MOVE_DEFERRED;

	return true;
}

#define HV_MSI_FLAGS_SUPPORTED	(MSI_GENERIC_FLAGS_MASK | MSI_FLAG_PCI_MSIX)
#define HV_MSI_FLAGS_REQUIRED	(MSI_FLAG_USE_DEF_DOM_OPS |	\
				 MSI_FLAG_USE_DEF_CHIP_OPS)

static struct msi_parent_ops hv_msi_parent_ops = {
	.supported_flags	= HV_MSI_FLAGS_SUPPORTED,
	.required_flags		= HV_MSI_FLAGS_REQUIRED,
	.bus_select_token	= DOMAIN_BUS_NEXUS,
	.bus_select_mask	= MATCH_PCI_MSI,
	.chip_flags		= MSI_CHIP_FLAG_SET_EOI,
	.prefix			= "HV-",
	.init_dev_msi_info	= hv_init_dev_msi_info,
};
/*
 * Hyper-V root PCI MSI is delivered as GIC LPIs (Hyper-V owns the physical
 * ITS; dom0 sees no ITS but still consumes LPI INTIDs at the redistributor).
 * LPI INTIDs start at 8192; we carve a window beginning at 0x2000.
 */
#define HV_PCI_MSI_LPI_START	0x2000
#define HV_PCI_MSI_LPI_NR	(1U << 16)

struct hv_pci_chip_data {
	DECLARE_BITMAP(lpi_map, HV_PCI_MSI_LPI_NR);
	struct mutex	map_lock;
};

static int hv_pci_vec_alloc_device_irq(struct irq_domain *domain,
				       unsigned int nr_irqs,
				       irq_hw_number_t *hwirq)
{
	struct hv_pci_chip_data *chip_data = domain->host_data;
	int index;

	mutex_lock(&chip_data->map_lock);
	index = bitmap_find_free_region(chip_data->lpi_map, HV_PCI_MSI_LPI_NR,
					get_count_order(nr_irqs));
	mutex_unlock(&chip_data->map_lock);
	if (index < 0)
		return -ENOSPC;
	*hwirq = index + HV_PCI_MSI_LPI_START;
	return 0;
}

static int hv_pci_vec_irq_gic_domain_alloc(struct irq_domain *domain,
					   unsigned int virq,
					   irq_hw_number_t hwirq)
{
	struct irq_fwspec fwspec;

	/*
	 * LPI INTIDs have no OF GIC binding (LPIs are dynamically assigned,
	 * with no DT representation).  On Hyper-V arm64 the root partition
	 * boots via ACPI; the parent fwnode is the ACPI GSI dispatcher and
	 * the 2-parameter (hwirq, type) form is the only valid encoding.
	 */
	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 2;
	fwspec.param[0] = hwirq;
	fwspec.param[1] = IRQ_TYPE_EDGE_RISING;

	return irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
}

static void hv_pci_vec_irq_free(struct irq_domain *domain,
				unsigned int virq,
				unsigned int nr_bm_irqs,
				unsigned int nr_dom_irqs)
{
	struct hv_pci_chip_data *chip_data = domain->host_data;
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	int first = d->hwirq - HV_PCI_MSI_LPI_START;
	int i;

	mutex_lock(&chip_data->map_lock);
	bitmap_release_region(chip_data->lpi_map,
			      first,
			      get_count_order(nr_bm_irqs));
	mutex_unlock(&chip_data->map_lock);
	for (i = 0; i < nr_dom_irqs; i++) {
		if (i)
			d = irq_domain_get_irq_data(domain, virq + i);
		irq_domain_reset_irq_data(d);
	}

	irq_domain_free_irqs_parent(domain, virq, nr_dom_irqs);
}
/* Allocate nr_irqs IRQs for the given irq domain */
static int hv_msi_domain_alloc(struct irq_domain *d, unsigned int virq,
			       unsigned int nr_irqs, void *arg)
{
    irq_hw_number_t hwirq;
    int ret, i;

    ret = hv_pci_vec_alloc_device_irq(d, nr_irqs, &hwirq);
    if (ret)
        return ret;

    for (i = 0; i < nr_irqs; i++) {
        ret = hv_pci_vec_irq_gic_domain_alloc(d, virq + i, hwirq + i);
        if (ret) {
            hv_pci_vec_irq_free(d, virq, nr_irqs, i);
            return ret;
        }
        irq_domain_set_hwirq_and_chip(d, virq + i, hwirq + i,
                                        &hv_pci_msi_controller,
                                        d->host_data);
    }
    return 0;
}


static void hv_msi_domain_free(struct irq_domain *d, unsigned int virq,
			       unsigned int nr_irqs)
{
	for (int i = 0; i < nr_irqs; ++i) {
		struct irq_data *irqd = irq_domain_get_irq_data(d, virq + i);
		struct msi_desc *desc = irq_data_get_msi_desc(irqd);

		if (!desc || !desc->irq || WARN_ON_ONCE(!dev_is_pci(desc->dev)))
			continue;
		hv_teardown_msi_irq(to_pci_dev(desc->dev), irqd);
	}
	hv_pci_vec_irq_free(d, virq, nr_irqs, nr_irqs);
}

static int hv_msi_domain_activate(struct irq_domain *d, struct irq_data *irqd,
				  bool reserve)
{
	int cpu = cpumask_first(cpu_present_mask);
	irq_data_update_effective_affinity(irqd, cpumask_of(cpu));
	return 0;
}

static const struct irq_domain_ops hv_msi_domain_ops = {
	.select	= msi_lib_irq_domain_select,
	.alloc	= hv_msi_domain_alloc,
	.free	= hv_msi_domain_free,
    .activate = hv_msi_domain_activate,
};



#ifdef CONFIG_ACPI

static struct irq_domain *hv_pci_acpi_irq_domain_parent(void)
{
	acpi_gsi_domain_disp_fn gsi_domain_disp_fn;

	gsi_domain_disp_fn = acpi_get_gsi_dispatcher();
	if (!gsi_domain_disp_fn)
		return NULL;
	return irq_find_matching_fwnode(gsi_domain_disp_fn(0),
				     DOMAIN_BUS_ANY);
}

#endif
#ifdef CONFIG_OF

static struct irq_domain *hv_pci_of_irq_domain_parent(void)
{
	struct device_node *parent;
	struct irq_domain *domain;

	parent = of_irq_find_parent(hv_get_vmbus_root_device()->of_node);
	if (!parent)
		return NULL;
	domain = irq_find_host(parent);
	of_node_put(parent);

	return domain;
}

#endif
struct irq_domain * __init hv_create_pci_msi_domain(void)
{
	static struct hv_pci_chip_data *chip_data;
	struct fwnode_handle *fn = NULL;
	struct irq_domain *irq_domain_parent = NULL;
    struct irq_domain *hv_msi_gic_irq_domain;

	chip_data = kzalloc_obj(*chip_data);
	if (!chip_data)
		return NULL;

	mutex_init(&chip_data->map_lock);
	fn = irq_domain_alloc_named_fwnode("hv_vpci_arm64");
	if (!fn)
		goto free_chip;

	/*
	 * IRQ domain once enabled, should not be removed since there is no
	 * way to ensure that all the corresponding devices are also gone and
	 * no interrupts will be generated.
	 */
#ifdef CONFIG_ACPI
	if (!acpi_disabled)
		irq_domain_parent = hv_pci_acpi_irq_domain_parent();
#endif
#ifdef CONFIG_OF
	if (!irq_domain_parent)
		irq_domain_parent = hv_pci_of_irq_domain_parent();
#endif
	if (!irq_domain_parent) {
		WARN_ONCE(
			1,
			"Invalid firmware configuration for VMBus interrupts\n");
		goto free_chip;
	}

	hv_msi_gic_irq_domain = ({
		struct irq_domain_info info = {
			.fwnode = fn,
			.ops = &hv_msi_domain_ops,
			.parent = irq_domain_parent, /* GIC distributor */
			.host_data = chip_data,
			.size = HV_PCI_MSI_LPI_NR,
		};
		msi_create_parent_irq_domain(&info, &hv_msi_parent_ops);
	});

	if (!hv_msi_gic_irq_domain) {
		pr_err("Failed to create Hyper-V arm64 vPCI MSI IRQ domain\n");
		goto free_chip;
	}

	return hv_msi_gic_irq_domain;

free_chip:
	kfree(chip_data);
	if (fn)
		irq_domain_free_fwnode(fn);

	return NULL;
}

#endif /* CONFIG_PCI_MSI */