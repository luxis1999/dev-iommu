// SPDX-License-Identifier: GPL-2.0
/*
 * nested.c - nested mode translation support
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *         Jacob Pan <jacob.jun.pan@linux.intel.com>
 *         Yi Liu <yi.l.liu@intel.com>
 */

#define pr_fmt(fmt)	"DMAR: " fmt

#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>

#include "iommu.h"
#include "pasid.h"

static int intel_nested_attach_dev(struct iommu_domain *domain,
				   struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct intel_iommu *iommu = info->iommu;
	unsigned long flags;
	int ret = 0;

	if (info->domain)
		device_block_translation(dev);

	if (iommu->agaw < dmar_domain->s2_domain->agaw) {
		dev_err_ratelimited(dev, "Adjusted guest address width not compatible\n");
		return -ENODEV;
	}

	/*
	 * Stage-1 domain cannot work alone, it is nested on a s2_domain.
	 * The s2_domain will be used in nested translation, hence needs
	 * to ensure the s2_domain is compatible with this IOMMU.
	 */
	ret = prepare_domain_attach_device(&dmar_domain->s2_domain->domain, dev);
	if (ret) {
		dev_err_ratelimited(dev, "s2 domain is not compatible\n");
		return ret;
	}

	ret = domain_attach_iommu(dmar_domain, iommu);
	if (ret) {
		dev_err_ratelimited(dev, "Failed to attach domain to iommu\n");
		return ret;
	}

	ret = intel_pasid_setup_nested(iommu, dev,
				       IOMMU_NO_PASID, dmar_domain);
	if (ret) {
		domain_detach_iommu(dmar_domain, iommu);
		dev_err_ratelimited(dev, "Failed to setup pasid entry\n");
		return ret;
	}

	info->domain = dmar_domain;
	spin_lock_irqsave(&dmar_domain->lock, flags);
	list_add(&info->link, &dmar_domain->devices);
	spin_unlock_irqrestore(&dmar_domain->lock, flags);

	return 0;
}

static int intel_nested_set_dev_pasid(struct iommu_domain *domain,
				      struct device *dev, ioasid_t pasid)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct intel_iommu *iommu = info->iommu;
	struct dev_pasid_info *dev_pasid;
	unsigned long flags;
	int ret = 0;

	if (!pasid_supported(iommu))
		return -EOPNOTSUPP;

	if (iommu->agaw < dmar_domain->s2_domain->agaw)
		return -EINVAL;

	ret = prepare_domain_attach_device(&dmar_domain->s2_domain->domain, dev);
	if (ret)
		return ret;

	dev_pasid = kzalloc(sizeof(*dev_pasid), GFP_KERNEL);
	if (!dev_pasid)
		return -ENOMEM;

	ret = domain_attach_iommu(dmar_domain, iommu);
	if (ret)
		goto err_free;

	ret = intel_pasid_setup_nested(iommu, dev, pasid, dmar_domain);
	if (ret)
		goto err_detach_iommu;

	dev_pasid->dev = dev;
	dev_pasid->pasid = pasid;
	spin_lock_irqsave(&dmar_domain->lock, flags);
	list_add(&dev_pasid->link_domain, &dmar_domain->dev_pasids);
	spin_unlock_irqrestore(&dmar_domain->lock, flags);

	return 0;
err_detach_iommu:
	domain_detach_iommu(dmar_domain, iommu);
err_free:
	kfree(dev_pasid);
	return ret;
}

static void intel_nested_domain_free(struct iommu_domain *domain)
{
	kfree(to_dmar_domain(domain));
}

static void domain_flush_iotlb_psi(struct dmar_domain *domain,
				   u64 addr, unsigned long npages)
{
	struct iommu_domain_info *info;
	unsigned long i;

	xa_for_each(&domain->iommu_array, i, info)
		iommu_flush_iotlb_psi(info->iommu, domain,
				      addr >> VTD_PAGE_SHIFT, npages, 1, 0);
}

static int intel_nested_cache_invalidate_user(struct iommu_domain *domain,
					      struct iommu_user_data_array *array,
					      u32 *cerror_idx)
{
	struct dmar_domain *dmar_domain = to_dmar_domain(domain);
	struct iommu_hwpt_vtd_s1_invalidate inv_info;
	u32 index;
	int ret;

	/* REVISIT:
	 * VT-d has defined ITE, ICE, IQE for invalidation failure per hardware,
	 * but no error code yet, so just set the error code to be 0.
	 */
	*cerror_idx = 0;

	for (index = 0; index < array->entry_num; index++) {
		ret = iommu_copy_struct_from_user_array(&inv_info, array, index,
						        __reserved);
		if (ret) {
			pr_err_ratelimited("Failed to fetch invalidation request\n");
			break;
		}

		if (inv_info.__reserved || (inv_info.flags & ~IOMMU_VTD_INV_FLAGS_LEAF) ||
		    !IS_ALIGNED(inv_info.addr, VTD_PAGE_SIZE)) {
			ret = -EINVAL;
			break;
		}

		if (inv_info.addr == 0 && inv_info.npages == -1)
			intel_flush_iotlb_all(domain);
		else
			domain_flush_iotlb_psi(dmar_domain,
					       inv_info.addr, inv_info.npages);
	}

	array->entry_num = index;

	return ret;
}

static const struct iommu_domain_ops intel_nested_domain_ops = {
	.attach_dev		= intel_nested_attach_dev,
	.set_dev_pasid		= intel_nested_set_dev_pasid,
	.free			= intel_nested_domain_free,
	.cache_invalidate_user	= intel_nested_cache_invalidate_user,
};

struct iommu_domain *intel_nested_domain_alloc(struct iommu_domain *s2_domain,
					       const struct iommu_user_data *user_data)
{
	struct iommu_hwpt_vtd_s1 vtd;
	struct dmar_domain *domain;
	int ret;

	ret = iommu_copy_struct_from_user(&vtd, user_data, __reserved);
	if (ret)
		return ERR_PTR(ret);

	domain = kzalloc(sizeof(*domain), GFP_KERNEL_ACCOUNT);
	if (!domain)
		return NULL;

	domain->use_first_level = true;
	domain->s2_domain = to_dmar_domain(s2_domain);
	domain->s1_pgtbl = vtd.pgtbl_addr;
	domain->s1_cfg = vtd;
	domain->domain.ops = &intel_nested_domain_ops;
	domain->domain.type = IOMMU_DOMAIN_NESTED;
	INIT_LIST_HEAD(&domain->devices);
	INIT_LIST_HEAD(&domain->dev_pasids);
	spin_lock_init(&domain->lock);
	xa_init(&domain->iommu_array);

	return &domain->domain;
}
