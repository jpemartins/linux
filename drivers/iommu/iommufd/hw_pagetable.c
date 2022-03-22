// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
#include <linux/iommu.h>
#include <uapi/linux/iommufd.h>

#include "iommufd_private.h"

void iommufd_hw_pagetable_destroy(struct iommufd_object *obj)
{
	struct iommufd_hw_pagetable *hwpt =
		container_of(obj, struct iommufd_hw_pagetable, obj);

	WARN_ON(!list_empty(&hwpt->devices));

	iommu_domain_free(hwpt->domain);
	refcount_dec(&hwpt->ioas->obj.users);
	mutex_destroy(&hwpt->devices_lock);
}

/**
 * iommufd_hw_pagetable_alloc() - Get an iommu_domain for a device
 * @ictx: iommufd context
 * @ioas: IOAS to associate the domain with
 * @dev: Device to get an iommu_domain for
 *
 * Allocate a new iommu_domain and return it as a hw_pagetable.
 */
struct iommufd_hw_pagetable *
iommufd_hw_pagetable_alloc(struct iommufd_ctx *ictx, struct iommufd_ioas *ioas,
			   struct device *dev)
{
	struct iommufd_hw_pagetable *hwpt;
	int rc;

	hwpt = iommufd_object_alloc(ictx, hwpt, IOMMUFD_OBJ_HW_PAGETABLE);
	if (IS_ERR(hwpt))
		return hwpt;

	hwpt->domain = iommu_domain_alloc(dev->bus);
	if (!hwpt->domain) {
		rc = -ENOMEM;
		goto out_abort;
	}

	/*
	 * If the IOMMU can block non-coherent operations (ie PCIe TLPs with
	 * no-snoop set) then always turn it on. We currently don't have a uAPI
	 * to allow userspace to restore coherency if it wants to use no-snoop
	 * TLPs.
	 */
	if (hwpt->domain->ops->enforce_cache_coherency)
		hwpt->enforce_cache_coherency =
			hwpt->domain->ops->enforce_cache_coherency(
				hwpt->domain);

	INIT_LIST_HEAD(&hwpt->devices);
	INIT_LIST_HEAD(&hwpt->hwpt_item);
	mutex_init(&hwpt->devices_lock);
	/* Pairs with iommufd_hw_pagetable_destroy() */
	refcount_inc(&ioas->obj.users);
	hwpt->ioas = ioas;
	return hwpt;

out_abort:
	iommufd_object_abort(ictx, &hwpt->obj);
	return ERR_PTR(rc);
}

int iommufd_hwpt_get_dirty(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_get_dirty *cmd = ucmd->cmd;
	struct iommufd_hw_pagetable *hwpt;
	int rc;

	hwpt = iommufd_get_hwpt(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	rc = iopt_get_dirty_tracking(&hwpt->ioas->iopt, hwpt->domain);
	if (rc < 0)
		cmd->out_status = IOMMU_DIRTY_TRACKING_UNSUPPORTED;
	else if (!rc)
		cmd->out_status = IOMMU_DIRTY_TRACKING_SUPPORTED;
	else
		cmd->out_status = (IOMMU_DIRTY_TRACKING_SUPPORTED |
				   IOMMU_DIRTY_TRACKING_ENABLED);

	iommufd_put_object(&hwpt->obj);
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));
	return rc;
}

int iommufd_hwpt_set_dirty(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_set_dirty *cmd = ucmd->cmd;
	struct iommufd_hw_pagetable *hwpt;
	struct iommufd_ioas *ioas;
	int rc = -EOPNOTSUPP;
	bool enable;

	hwpt = iommufd_get_hwpt(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	ioas = hwpt->ioas;
	enable = cmd->flags & IOMMU_DIRTY_TRACKING_ENABLED;

	rc = iopt_set_dirty_tracking(&ioas->iopt, hwpt->domain, enable);

	iommufd_put_object(&hwpt->obj);
	return rc;
}

int iommufd_check_iova_range(struct iommufd_ioas *ioas,
			     struct iommufd_dirty_data *bitmap)
{
	unsigned long pgshift, npages;
	size_t iommu_pgsize;
	int rc = -EINVAL;

	pgshift = __ffs(bitmap->page_size);
	npages = bitmap->length >> pgshift;

	if (!npages || (npages > ULONG_MAX))
		return rc;

	iommu_pgsize = 1 << __ffs(ioas->iopt.iova_alignment);

	/* allow only smallest supported pgsize */
	if (bitmap->page_size != iommu_pgsize)
		return rc;

	if (bitmap->iova & (iommu_pgsize - 1))
		return rc;

	if (!bitmap->length || bitmap->length & (iommu_pgsize - 1))
		return rc;

	return 0;
}

int iommufd_hwpt_get_dirty_iova(struct iommufd_ucmd *ucmd)
{
	struct iommu_hwpt_get_dirty_iova *cmd = ucmd->cmd;
	struct iommufd_hw_pagetable *hwpt;
	struct iommufd_ioas *ioas;
	int rc = -EOPNOTSUPP;

	hwpt = iommufd_get_hwpt(ucmd, cmd->hwpt_id);
	if (IS_ERR(hwpt))
		return PTR_ERR(hwpt);

	ioas = hwpt->ioas;
	rc = iommufd_check_iova_range(ioas, &cmd->bitmap);
	if (rc)
		goto out_put;

	rc = iopt_read_and_clear_dirty_data(&ioas->iopt, hwpt->domain,
					    &cmd->bitmap);

out_put:
	iommufd_put_object(&hwpt->obj);
	return rc;
}
