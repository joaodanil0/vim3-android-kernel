// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 - Google LLC
 * Author: David Brazdil <dbrazdil@google.com>
 */

#include <linux/kvm_host.h>

#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_s2mpu.h>

#include <linux/arm-smccc.h>

#include <nvhe/iommu.h>
#include <nvhe/memory.h>
#include <nvhe/mm.h>
#include <nvhe/spinlock.h>
#include <nvhe/trap_handler.h>
#include <asm/io-mpt-s2mpu.h>

#define SMC_CMD_PREPARE_PD_ONOFF	0x82000410
#define SMC_MODE_POWER_UP		1

#define PA_MAX				((phys_addr_t)SZ_1G * NR_GIGABYTES)

#define SYNC_MAX_RETRIES		5
#define SYNC_TIMEOUT			5
#define SYNC_TIMEOUT_MULTIPLIER		3

#define CTX_CFG_ENTRY(ctxid, nr_ctx, vid) \
	(CONTEXT_CFG_VALID_VID_CTX_VID(ctxid, vid) \
	 | (((ctxid) < (nr_ctx)) ? CONTEXT_CFG_VALID_VID_CTX_VALID(ctxid) : 0))

#define for_each_child(child, dev) \
	list_for_each_entry((child), &(dev)->children, siblings)

/* HW version-specific operations. */
struct s2mpu_reg_ops {
	int (*init)(struct pkvm_iommu *dev);
	void (*set_control_regs)(struct pkvm_iommu *dev);
	u32 (*host_mmio_reg_access_mask)(size_t off, bool is_write);
};

struct s2mpu_drv_data {
	u32 version;
	u32 context_cfg_valid_vid;
};

static const struct s2mpu_mpt_ops *mpt_ops;
static const struct s2mpu_reg_ops *reg_ops;
static struct mpt host_mpt;

const struct pkvm_iommu_ops pkvm_s2mpu_ops;
const struct pkvm_iommu_ops pkvm_sysmmu_sync_ops;

static inline enum mpt_prot prot_to_mpt(enum kvm_pgtable_prot prot)
{
	return ((prot & KVM_PGTABLE_PROT_R) ? MPT_PROT_R : 0) |
	       ((prot & KVM_PGTABLE_PROT_W) ? MPT_PROT_W : 0);
}

static bool is_version(struct pkvm_iommu *dev, u32 version)
{
	struct s2mpu_drv_data *data = (struct s2mpu_drv_data *)dev->data;

	return (data->version & VERSION_CHECK_MASK) == version;
}

static u32 __context_cfg_valid_vid(struct pkvm_iommu *dev, u32 vid_bmap)
{
	struct s2mpu_drv_data *data = (struct s2mpu_drv_data *)dev->data;
	u8 ctx_vid[NR_CTX_IDS] = { 0 };
	unsigned int vid, ctx = 0;
	unsigned int num_ctx;
	u32 res;

	/* Only initialize once. */
	if (data->context_cfg_valid_vid)
		return data->context_cfg_valid_vid;

	num_ctx = readl_relaxed(dev->va + REG_NS_NUM_CONTEXT) & NUM_CONTEXT_MASK;
	while (vid_bmap) {
		/* Break if we cannot allocate more. */
		if (ctx >= num_ctx)
			break;

		vid = __ffs(vid_bmap);
		vid_bmap &= ~BIT(vid);
		ctx_vid[ctx++] = vid;
	}

	/* The following loop was unrolled so bitmasks are constant. */
	BUILD_BUG_ON(NR_CTX_IDS != 8);
	res = CTX_CFG_ENTRY(0, ctx, ctx_vid[0])
	    | CTX_CFG_ENTRY(1, ctx, ctx_vid[1])
	    | CTX_CFG_ENTRY(2, ctx, ctx_vid[2])
	    | CTX_CFG_ENTRY(3, ctx, ctx_vid[3])
	    | CTX_CFG_ENTRY(4, ctx, ctx_vid[4])
	    | CTX_CFG_ENTRY(5, ctx, ctx_vid[5])
	    | CTX_CFG_ENTRY(6, ctx, ctx_vid[6])
	    | CTX_CFG_ENTRY(7, ctx, ctx_vid[7]);

	data->context_cfg_valid_vid = res;
	return res;
}

static int __initialize_v2(struct pkvm_iommu *dev)
{
	u32 ssmt_valid_vid_bmap, ctx_cfg;

	/* Assume all VIDs may be generated by the connected SSMTs for now. */
	ssmt_valid_vid_bmap = ALL_VIDS_BITMAP;
	ctx_cfg = __context_cfg_valid_vid(dev, ssmt_valid_vid_bmap);
	if (!ctx_cfg)
		return -EINVAL;

	/*
	 * Write CONTEXT_CFG_VALID_VID configuration before touching L1ENTRY*
	 * registers. Writes to those registers are ignored unless there is
	 * a context ID allocated to the corresponding VID (v2 only).
	 */
	writel_relaxed(ctx_cfg, dev->va + REG_NS_CONTEXT_CFG_VALID_VID);
	return 0;
}

static int __initialize(struct pkvm_iommu *dev)
{
	struct s2mpu_drv_data *data = (struct s2mpu_drv_data *)dev->data;

	if (!data->version)
		data->version = readl_relaxed(dev->va + REG_NS_VERSION);

	switch (data->version & VERSION_CHECK_MASK) {
	case S2MPU_VERSION_1:
		return 0;
	case S2MPU_VERSION_2:
		return __initialize_v2(dev);
	default:
		return -EINVAL;
	}
}

static void __set_control_regs(struct pkvm_iommu *dev)
{
	u32 ctrl0 = 0, irq_vids;

	/*
	 * Note: We set the values of CTRL0, CTRL1 and CFG registers here but we
	 * still rely on the correctness of their reset values. S2MPUs *must*
	 * reset to a state where all DMA traffic is blocked until the hypervisor
	 * writes its configuration to the S2MPU. A malicious EL1 could otherwise
	 * attempt to bypass the permission checks in the window between powering
	 * on the S2MPU and this function being called.
	 */

	/* Enable the S2MPU, otherwise all traffic would be allowed through. */
	ctrl0 |= CTRL0_ENABLE;

	/*
	 * Enable interrupts on fault for all VIDs. The IRQ must also be
	 * specified in DT to get unmasked in the GIC.
	 */
	ctrl0 |= CTRL0_INTERRUPT_ENABLE;
	irq_vids = ALL_VIDS_BITMAP;

	/* Return SLVERR/DECERR to device on permission fault. */
	ctrl0 |= is_version(dev, S2MPU_VERSION_2) ? CTRL0_FAULT_RESP_TYPE_DECERR
						  : CTRL0_FAULT_RESP_TYPE_SLVERR;

	writel_relaxed(irq_vids, dev->va + REG_NS_INTERRUPT_ENABLE_PER_VID_SET);
	writel_relaxed(0, dev->va + REG_NS_CFG);
	writel_relaxed(0, dev->va + REG_NS_CTRL1);
	writel_relaxed(ctrl0, dev->va + REG_NS_CTRL0);
}
static void __set_control_regs_v9(struct pkvm_iommu *dev)
{
	/* Return DECERR to device on permission fault. */
	writel_relaxed(ALL_VIDS_BITMAP,
		       dev->va + REG_NS_V9_CTRL_ERR_RESP_T_PER_VID_SET);
	/*
	 * Enable interrupts on fault for all VIDs. The IRQ must also be
	 * specified in DT to get unmasked in the GIC.
	 */
	writel_relaxed(ALL_VIDS_BITMAP,
		       dev->va + REG_NS_INTERRUPT_ENABLE_PER_VID_SET);
	writel_relaxed(0, dev->va + REG_NS_CTRL0);
	/* Enable the S2MPU, otherwise all traffic would be allowed through. */
	writel_relaxed(ALL_VIDS_BITMAP,
		       dev->va + REG_NS_V9_CTRL_PROT_EN_PER_VID_SET);
	writel_relaxed(0, dev->va + REG_NS_V9_CFG_MPTW_ATTRIBUTE);
}

/*
 * Poll the given SFR until its value has all bits of a given mask set.
 * Returns true if successful, false if not successful after a given number of
 * attempts.
 */
static bool __wait_until(void __iomem *addr, u32 mask, size_t max_attempts)
{
	size_t i;

	for (i = 0; i < max_attempts; i++) {
		if ((readl_relaxed(addr) & mask) == mask)
			return true;
	}
	return false;
}

/* Poll the given SFR as long as its value has all bits of a given mask set. */
static void __wait_while(void __iomem *addr, u32 mask)
{
	while ((readl_relaxed(addr) & mask) == mask)
		continue;
}

static void __sync_cmd_start(struct pkvm_iommu *sync)
{
	writel_relaxed(SYNC_CMD_SYNC, sync->va + REG_NS_SYNC_CMD);
}

static void __invalidation_barrier_slow(struct pkvm_iommu *sync)
{
	size_t i, timeout;

	/*
	 * Wait for transactions to drain if SysMMU_SYNCs were registered.
	 * Assumes that they are in the same power domain as the S2MPU.
	 *
	 * The algorithm will try initiating the SYNC if the SYNC_COMP_COMPLETE
	 * bit has not been set after a given number of attempts, increasing the
	 * timeout exponentially each time. If this cycle fails a given number
	 * of times, the algorithm will give up completely to avoid deadlock.
	 */
	timeout = SYNC_TIMEOUT;
	for (i = 0; i < SYNC_MAX_RETRIES; i++) {
		__sync_cmd_start(sync);
		if (__wait_until(sync->va + REG_NS_SYNC_COMP, SYNC_COMP_COMPLETE, timeout))
			break;
		timeout *= SYNC_TIMEOUT_MULTIPLIER;
	}
}

/* Initiate invalidation barrier. */
static void __invalidation_barrier_init(struct pkvm_iommu *dev)
{
	struct pkvm_iommu *sync;

	for_each_child(sync, dev)
		__sync_cmd_start(sync);
}

/* Wait for invalidation to complete. */
static void __invalidation_barrier_complete(struct pkvm_iommu *dev)
{
	struct pkvm_iommu *sync;

	/*
	 * Check if the SYNC_COMP_COMPLETE bit has been set for individual
	 * devices. If not, fall back to non-parallel invalidation.
	 */
	for_each_child(sync, dev) {
		if (!(readl_relaxed(sync->va + REG_NS_SYNC_COMP) & SYNC_COMP_COMPLETE))
			__invalidation_barrier_slow(sync);
	}

	/* Must not access SFRs while S2MPU is busy invalidating */
	if (is_version(dev, S2MPU_VERSION_2) || is_version(dev, S2MPU_VERSION_9)) {
		__wait_while(dev->va + REG_NS_STATUS,
			     STATUS_BUSY | STATUS_ON_INVALIDATING);
	}
}

static void __all_invalidation(struct pkvm_iommu *dev)
{
	writel_relaxed(INVALIDATION_INVALIDATE, dev->va + REG_NS_ALL_INVALIDATION);
	__invalidation_barrier_init(dev);
	__invalidation_barrier_complete(dev);
}

static void __range_invalidation_init(struct pkvm_iommu *dev, phys_addr_t first_byte,
				      phys_addr_t last_byte)
{
	u32 start_ppn = first_byte >> RANGE_INVALIDATION_PPN_SHIFT;
	u32 end_ppn = last_byte >> RANGE_INVALIDATION_PPN_SHIFT;

	writel_relaxed(start_ppn, dev->va + REG_NS_RANGE_INVALIDATION_START_PPN);
	writel_relaxed(end_ppn, dev->va + REG_NS_RANGE_INVALIDATION_END_PPN);
	writel_relaxed(INVALIDATION_INVALIDATE, dev->va + REG_NS_RANGE_INVALIDATION);
	__invalidation_barrier_init(dev);
}

/*
 * Initialize S2MPU device and set all GB regions to 1G granularity with
 * given protection bits.
 */
static int initialize_with_prot(struct pkvm_iommu *dev, enum mpt_prot prot)
{
	int ret;

	ret = reg_ops->init(dev);
	if (ret)
		return ret;

	mpt_ops->init_with_prot(dev->va, prot);
	__all_invalidation(dev);

	/* Set control registers, enable the S2MPU. */
	reg_ops->set_control_regs(dev);
	return 0;
}

/*
 * Initialize S2MPU device, set L2 table addresses and configure L1TABLE_ATTR
 * registers according to the given MPT struct.
 */
static int initialize_with_mpt(struct pkvm_iommu *dev, struct mpt *mpt)
{
	int ret;

	ret = reg_ops->init(dev);
	if (ret)
		return ret;

	mpt_ops->init_with_mpt(dev->va, mpt);
	__all_invalidation(dev);

	/* Set control registers, enable the S2MPU. */
	reg_ops->set_control_regs(dev);
	return 0;
}

static bool to_valid_range(phys_addr_t *start, phys_addr_t *end)
{
	phys_addr_t new_start = *start;
	phys_addr_t new_end = *end;

	if (new_end > PA_MAX)
		new_end = PA_MAX;

	new_start = ALIGN_DOWN(new_start, SMPT_GRAN);
	new_end = ALIGN(new_end, SMPT_GRAN);

	if (new_start >= new_end)
		return false;

	*start = new_start;
	*end = new_end;
	return true;
}

static void __mpt_idmap_prepare(struct mpt *mpt, phys_addr_t first_byte,
				phys_addr_t last_byte, enum mpt_prot prot)
{
	mpt_ops->prepare_range(mpt, first_byte, last_byte, prot);
}

static void __mpt_idmap_apply(struct pkvm_iommu *dev, struct mpt *mpt,
			      phys_addr_t first_byte, phys_addr_t last_byte)
{
	unsigned int first_gb = first_byte / SZ_1G;
	unsigned int last_gb = last_byte / SZ_1G;

	mpt_ops->apply_range(dev->va, mpt, first_gb, last_gb);
	/* Initiate invalidation, completed in __mdt_idmap_complete. */
	__range_invalidation_init(dev, first_byte, last_byte);
}

static void __mpt_idmap_complete(struct pkvm_iommu *dev, struct mpt *mpt)
{
	__invalidation_barrier_complete(dev);
}

static void s2mpu_host_stage2_idmap_prepare(phys_addr_t start, phys_addr_t end,
					    enum kvm_pgtable_prot prot)
{
	if (!to_valid_range(&start, &end))
		return;

	__mpt_idmap_prepare(&host_mpt, start, end - 1, prot_to_mpt(prot));
}

static void s2mpu_host_stage2_idmap_apply(struct pkvm_iommu *dev,
					  phys_addr_t start, phys_addr_t end)
{
	if (!to_valid_range(&start, &end))
		return;

	__mpt_idmap_apply(dev, &host_mpt, start, end - 1);
}

static void s2mpu_host_stage2_idmap_complete(struct pkvm_iommu *dev)
{
	__mpt_idmap_complete(dev, &host_mpt);
}

static int s2mpu_resume(struct pkvm_iommu *dev)
{
	/*
	 * Initialize the S2MPU with the host stage-2 MPT. It is paramount
	 * that the S2MPU reset state is enabled and blocking all traffic,
	 * otherwise the host would not be forced to call the resume HVC
	 * before issuing DMA traffic.
	 */
	return initialize_with_mpt(dev, &host_mpt);
}

static int s2mpu_suspend(struct pkvm_iommu *dev)
{
	/*
	 * Stop updating the S2MPU when the host informs us about the intention
	 * to suspend it. Writes to powered-down MMIO registers would trigger
	 * SErrors in EL1 otherwise. However, hyp must put S2MPU back to
	 * blocking state first, in case the host does not actually power it
	 * down and continues issuing DMA traffic.
	 */
	return initialize_with_prot(dev, MPT_PROT_NONE);
}

static u32 host_mmio_reg_access_mask_v9(size_t off, bool is_write)
{
	const u32 no_access = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only = is_write ? no_access : read_write;
	const u32 write_only = is_write ? read_write : no_access;

	switch (off) {
	/* Allow reading control registers for debugging. */
	case REG_NS_CTRL0:
		return read_only & V9_CTRL0_MASK;
	case REG_NS_V9_CTRL_ERR_RESP_T_PER_VID_SET:
		return read_only & ALL_VIDS_BITMAP;
	case REG_NS_V9_CTRL_PROT_EN_PER_VID_SET:
		return read_only & ALL_VIDS_BITMAP;
	case REG_NS_V9_READ_STLB:
		return write_only & (V9_READ_STLB_MASK_TYPEA|V9_READ_STLB_MASK_TYPEB);
	case REG_NS_V9_READ_STLB_TPN:
		return read_only & V9_READ_STLB_TPN_MASK;
	case REG_NS_V9_READ_STLB_TAG_PPN:
		return read_only & V9_READ_STLB_TAG_PPN_MASK;
	case REG_NS_V9_READ_STLB_TAG_OTHERS:
		return read_only & V9_READ_STLB_TAG_OTHERS_MASK;
	case REG_NS_V9_READ_STLB_DATA:
		return read_only;
	case REG_NS_V9_MPTC_INFO:
		return read_only & V9_READ_MPTC_INFO_MASK;
	case REG_NS_V9_READ_MPTC:
		return write_only & V9_READ_MPTC_MASK;
	case REG_NS_V9_READ_MPTC_TAG_PPN:
		return read_only & V9_READ_MPTC_TAG_PPN_MASK;
	case REG_NS_V9_READ_MPTC_TAG_OTHERS:
		return read_only & V9_READ_MPTC_TAG_OTHERS_MASK;
	case REG_NS_V9_READ_MPTC_DATA:
		return read_only;
	case REG_NS_V9_PMMU_INFO:
		return read_only & V9_READ_PMMU_INFO_MASK;
	case REG_NS_V9_READ_PTLB:
		return write_only & V9_READ_PTLB_MASK;
	case REG_NS_V9_READ_PTLB_TAG:
		return read_only & V9_READ_PTLB_TAG_MASK;
	case REG_NS_V9_READ_PTLB_DATA_S1_EN_PPN_AP:
		return read_only & V9_READ_PTLB_DATA_S1_ENABLE_PPN_AP_MASK;
	case REG_NS_V9_READ_PTLB_DATA_S1_DIS_AP_LIST:
		return read_only;
	case REG_NS_V9_PMMU_INDICATOR:
		return read_only & V9_READ_PMMU_INDICATOR_MASK;
	case REG_NS_V9_SWALKER_INFO:
		return read_only&V9_SWALKER_INFO_MASK;
	};
	if (off >= REG_NS_V9_PMMU_PTLB_INFO(0) && off < REG_NS_V9_PMMU_PTLB_INFO(V9_MAX_PTLB_NUM))
		return read_only&V9_READ_PMMU_PTLB_INFO_MASK;
	if (off >= REG_NS_V9_STLB_INFO(0) && off < REG_NS_V9_STLB_INFO(V9_MAX_STLB_NUM))
		return read_only&V9_READ_SLTB_INFO_MASK;

	return no_access;
}

static u32 host_mmio_reg_access_mask_v1_v2(size_t off, bool is_write)
{
	const u32 no_access = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only = is_write ? no_access : read_write;
	const u32 write_only = is_write ? read_write : no_access;

	switch (off) {
	/* Allow reading control registers for debugging. */
	case REG_NS_CTRL0:
		return read_only & CTRL0_MASK;
	case REG_NS_CTRL1:
		return read_only & CTRL1_MASK;
	/* Allow reading MPTC entries for debugging. That involves:
	 *   - writing (set,way) to READ_MPTC
	 *   - reading READ_MPTC_*
	 */
	case REG_NS_READ_MPTC:
		return write_only & READ_MPTC_MASK;
	case REG_NS_READ_MPTC_TAG_PPN:
		return read_only & READ_MPTC_TAG_PPN_MASK;
	case REG_NS_READ_MPTC_TAG_OTHERS:
		return read_only & READ_MPTC_TAG_OTHERS_MASK;
	case REG_NS_READ_MPTC_DATA:
		return read_only;
	};
	return no_access;
}

static u32 host_mmio_reg_access_mask(size_t off, bool is_write)
{
	const u32 no_access  = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only  = is_write ? no_access  : read_write;
	const u32 write_only = is_write ? read_write : no_access;
	u32 masked_off;

	switch (off) {
	case REG_NS_CFG:
		return read_only & CFG_MASK;
	/* Allow EL1 IRQ handler to clear interrupts. */
	case REG_NS_INTERRUPT_CLEAR:
		return write_only & ALL_VIDS_BITMAP;
	/* Allow reading number of sets used by MPTC. */
	case REG_NS_INFO:
		return read_only & INFO_NUM_SET_MASK;
	/* Allow EL1 IRQ handler to read bitmap of pending interrupts. */
	case REG_NS_FAULT_STATUS:
		return read_only & ALL_VIDS_BITMAP;
	}

	/* Allow reading L1ENTRY registers for debugging. */
	if (off >= REG_NS_L1ENTRY_L2TABLE_ADDR(0, 0) &&
	    off < REG_NS_L1ENTRY_ATTR(NR_VIDS, 0))
		return read_only;

	/* Allow EL1 IRQ handler to read fault information. */
	masked_off = off & ~REG_NS_FAULT_VID_MASK;
	if ((masked_off == REG_NS_FAULT_PA_LOW(0)) ||
	    (masked_off == REG_NS_FAULT_PA_HIGH(0)) ||
	    (masked_off == REG_NS_FAULT_INFO(0)))
		return read_only;

	/* Check version-specific registers. */
	return reg_ops->host_mmio_reg_access_mask(off, is_write);
}

static bool s2mpu_host_dabt_handler(struct pkvm_iommu *dev,
				    struct kvm_cpu_context *host_ctxt,
				    u32 esr, size_t off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	u32 mask;

	/* Only handle MMIO access with u32 size and alignment. */
	if ((len != sizeof(u32)) || (off & (sizeof(u32) - 1)))
		return false;

	mask = host_mmio_reg_access_mask(off, is_write);
	if (!mask)
		return false;

	if (is_write)
		writel_relaxed(cpu_reg(host_ctxt, rd) & mask, dev->va + off);
	else
		cpu_reg(host_ctxt, rd) = readl_relaxed(dev->va + off) & mask;
	return true;
}
/*
 * Operations that differ between versions. We need to maintain
 * old behaviour were v1 and v2 can be used together.
 */
const struct s2mpu_reg_ops ops_v1_v2 = {
	.init = __initialize,
	.host_mmio_reg_access_mask = host_mmio_reg_access_mask_v1_v2,
	.set_control_regs = __set_control_regs,
};
const struct s2mpu_reg_ops ops_v9 = {
	.init = __initialize_v2,
	.host_mmio_reg_access_mask = host_mmio_reg_access_mask_v9,
	.set_control_regs = __set_control_regs_v9,
};

static int s2mpu_init(void *data, size_t size)
{
	struct mpt in_mpt;
	u32 *smpt;
	phys_addr_t pa;
	unsigned int gb;
	int ret = 0;
	int smpt_nr_pages, smpt_size;
	struct s2mpu_mpt_cfg cfg;

	if (size != sizeof(in_mpt))
		return -EINVAL;

	/* The host can concurrently modify 'data'. Copy it to avoid TOCTOU. */
	memcpy(&in_mpt, data, sizeof(in_mpt));

	cfg.version = in_mpt.version;
	/* Make sure the version sent is supported by the driver. */
	if ((cfg.version == S2MPU_VERSION_1) || (cfg.version == S2MPU_VERSION_2))
		reg_ops = &ops_v1_v2;
	else if (cfg.version == S2MPU_VERSION_9)
		reg_ops = &ops_v9;
	else
		return -ENODEV;

	/* Get page table operations for this version. */
	mpt_ops = s2mpu_get_mpt_ops(cfg);
	/* If version is wrong return. */
	if (!mpt_ops)
		return -EINVAL;

	smpt_size = mpt_ops->smpt_size();
	smpt_nr_pages = smpt_size / PAGE_SIZE;

	/* Take ownership of all SMPT buffers. This will also map them in. */
	for_each_gb(gb) {
		smpt = kern_hyp_va(in_mpt.fmpt[gb].smpt);
		pa = __hyp_pa(smpt);

		if (!IS_ALIGNED(pa, smpt_size)) {
			ret = -EINVAL;
			break;
		}

		ret = __pkvm_host_donate_hyp(pa >> PAGE_SHIFT, smpt_nr_pages);
		if (ret)
			break;

		host_mpt.fmpt[gb] = (struct fmpt){
			.smpt = smpt,
			.gran_1g = true,
			.prot = MPT_PROT_RW,
		};
	}

	/* Try to return memory back if there was an error. */
	if (ret) {
		for_each_gb(gb) {
			smpt = host_mpt.fmpt[gb].smpt;
			if (!smpt)
				break;

			WARN_ON(__pkvm_hyp_donate_host(__hyp_pa(smpt) >> PAGE_SHIFT,
						       smpt_nr_pages));
		}
		memset(&host_mpt, 0, sizeof(host_mpt));
	}

	return ret;
}

static int s2mpu_validate(struct pkvm_iommu *dev)
{
	if (dev->size != S2MPU_MMIO_SIZE)
		return -EINVAL;

	return 0;
}

static int s2mpu_validate_child(struct pkvm_iommu *dev, struct pkvm_iommu *child)
{
	if (child->ops != &pkvm_sysmmu_sync_ops)
		return -EINVAL;

	return 0;
}

static int sysmmu_sync_validate(struct pkvm_iommu *dev)
{
	if (dev->size != SYSMMU_SYNC_S2_MMIO_SIZE)
		return -EINVAL;

	if (!dev->parent || dev->parent->ops != &pkvm_s2mpu_ops)
		return -EINVAL;

	return 0;
}

const struct pkvm_iommu_ops pkvm_s2mpu_ops = (struct pkvm_iommu_ops){
	.init = s2mpu_init,
	.validate = s2mpu_validate,
	.validate_child = s2mpu_validate_child,
	.resume = s2mpu_resume,
	.suspend = s2mpu_suspend,
	.host_stage2_idmap_prepare = s2mpu_host_stage2_idmap_prepare,
	.host_stage2_idmap_apply = s2mpu_host_stage2_idmap_apply,
	.host_stage2_idmap_complete = s2mpu_host_stage2_idmap_complete,
	.host_dabt_handler = s2mpu_host_dabt_handler,
	.data_size = sizeof(struct s2mpu_drv_data),
};

const struct pkvm_iommu_ops pkvm_sysmmu_sync_ops = (struct pkvm_iommu_ops){
	.validate = sysmmu_sync_validate,
};
struct pkvm_iommu_driver pkvm_s2mpu_driver = (struct pkvm_iommu_driver){
	.ops = &pkvm_s2mpu_ops,
};
struct pkvm_iommu_driver pkvm_sysmmu_sync_driver = (struct pkvm_iommu_driver){
	.ops = &pkvm_sysmmu_sync_ops,
};
