// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Intel Corporation.
 */
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include "vfio.h"

static dev_t device_devt;

void vfio_init_device_cdev(struct vfio_device *device)
{
	device->device.devt = MKDEV(MAJOR(device_devt), device->index);
	cdev_init(&device->cdev, &vfio_device_fops);
	device->cdev.owner = THIS_MODULE;
}

/*
 * device access via the fd opened by this function is blocked until
 * .open_device() is called successfully during BIND_IOMMUFD.
 */
int vfio_device_fops_cdev_open(struct inode *inode, struct file *filep)
{
	struct vfio_device *device = container_of(inode->i_cdev,
						  struct vfio_device, cdev);
	struct vfio_device_file *df;
	int ret;

	/* Paired with the put in vfio_device_fops_release() */
	if (!vfio_device_try_get_registration(device))
		return -ENODEV;

	df = vfio_allocate_device_file(device);
	if (IS_ERR(df)) {
		ret = PTR_ERR(df);
		goto err_put_registration;
	}

	filep->private_data = df;

	/*
	 * Use the pseudo fs inode on the device to link all mmaps
	 * to the same address space, allowing us to unmap all vmas
	 * associated to this device using unmap_mapping_range().
	 */
	filep->f_mapping = device->inode->i_mapping;

	return 0;

err_put_registration:
	vfio_device_put_registration(device);
	return ret;
}

static void vfio_df_get_kvm_safe(struct vfio_device_file *df)
{
	spin_lock(&df->kvm_ref_lock);
	vfio_device_get_kvm_safe(df->device, df->kvm);
	spin_unlock(&df->kvm_ref_lock);
}

long vfio_df_ioctl_bind_iommufd(struct vfio_device_file *df,
				struct vfio_device_bind_iommufd __user *arg)
{
	struct vfio_device *device = df->device;
	struct vfio_device_bind_iommufd bind;
	unsigned long minsz;
	int ret;

	static_assert(__same_type(arg->out_devid, df->devid));

	minsz = offsetofend(struct vfio_device_bind_iommufd, out_devid);

	if (copy_from_user(&bind, arg, minsz))
		return -EFAULT;

	if (bind.argsz < minsz || bind.flags || bind.iommufd < 0)
		return -EINVAL;

	/* BIND_IOMMUFD only allowed for cdev fds */
	if (df->group)
		return -EINVAL;

	ret = vfio_device_block_group(device);
	if (ret)
		return ret;

	mutex_lock(&device->dev_set->lock);
	/* one device cannot be bound twice */
	if (df->access_granted) {
		ret = -EINVAL;
		goto out_unlock;
	}

	df->iommufd = iommufd_ctx_from_fd(bind.iommufd);
	if (IS_ERR(df->iommufd)) {
		ret = PTR_ERR(df->iommufd);
		df->iommufd = NULL;
		goto out_unlock;
	}

	/*
	 * Before the device open, get the KVM pointer currently
	 * associated with the device file (if there is) and obtain
	 * a reference.  This reference is held until device closed.
	 * Save the pointer in the device for use by drivers.
	 */
	vfio_df_get_kvm_safe(df);

	ret = vfio_df_open(df);
	if (ret)
		goto out_put_kvm;

	ret = copy_to_user(&arg->out_devid, &df->devid,
			   sizeof(df->devid)) ? -EFAULT : 0;
	if (ret)
		goto out_close_device;

	device->cdev_opened = true;
	/*
	 * Paired with smp_load_acquire() in vfio_device_fops::ioctl/
	 * read/write/mmap
	 */
	smp_store_release(&df->access_granted, true);
	mutex_unlock(&device->dev_set->lock);
	return 0;

out_close_device:
	vfio_df_close(df);
out_put_kvm:
	vfio_device_put_kvm(device);
	iommufd_ctx_put(df->iommufd);
	df->iommufd = NULL;
out_unlock:
	mutex_unlock(&device->dev_set->lock);
	vfio_device_unblock_group(device);
	return ret;
}

void vfio_df_unbind_iommufd(struct vfio_device_file *df)
{
	struct vfio_device *device = df->device;

	/*
	 * In the time of close, there is no contention with another one
	 * changing this flag.  So read df->access_granted without lock
	 * and no smp_load_acquire() is ok.
	 */
	if (!df->access_granted)
		return;

	mutex_lock(&device->dev_set->lock);
	vfio_df_close(df);
	vfio_device_put_kvm(device);
	iommufd_ctx_put(df->iommufd);
	device->cdev_opened = false;
	mutex_unlock(&device->dev_set->lock);
	vfio_device_unblock_group(device);
}

/**
 * vfio_copy_from_user - Copy the user struct that may have extended fields
 *
 * @buffer: The local buffer to store the data copied from user
 * @arg: The user buffer pointer
 * @minsz: The minimum size of the user struct, it should never bump up.
 * @xend: The most recent size of the user struct
 * @flags_mask: The combination of all the falgs defined
 * @xflags_mask: The combination of all the flags that mark a new field.
 *
 * This helper requires the user struct put the argsz and flags fields in
 * the first 8 bytes.
 *
 * Return 0 for success, otherwise -errno
 */
static int vfio_copy_from_user(void *buffer, void __user *arg,
			       unsigned long minsz, unsigned long xend,
			       u32 flags_mask, u32 xflags_mask)
{
	struct user_header {
		u32 argsz;
		u32 flags;
	} *header;


	if (copy_from_user(buffer, arg, minsz))
		return -EFAULT;

	header = (struct user_header *)buffer;
	if (header->argsz < minsz)
		return -EINVAL;

	if (header->flags & ~flags_mask)
		return -EINVAL;

	if (header->flags & xflags_mask && xend) {
		if (header->argsz < xend)
			return -EINVAL;

		if (copy_from_user(buffer + minsz,
				   arg + minsz, xend - minsz))
			return -EFAULT;
	}

	return 0;
}

#define VFIO_COPY_USER_DATA(_arg, _local_buffer, _struct, _init_last,         \
			    _recent_last, _flags_mask, _ext_flag_mask)        \
	vfio_copy_from_user(_local_buffer, _arg,                              \
			    offsetofend(_struct, _init_last) +                \
			    BUILD_BUG_ON_ZERO(offsetof(_struct, argsz) != 0), \
			    offsetofend(_struct, _recent_last) +              \
			    BUILD_BUG_ON_ZERO(offsetof(_struct, flags) !=     \
					      sizeof(u32)),                   \
			    _flags_mask, _ext_flag_mask)

int vfio_df_ioctl_attach_pt(struct vfio_device_file *df,
			    struct vfio_device_attach_iommufd_pt __user *arg)
{
	struct vfio_device_attach_iommufd_pt attach;
	struct vfio_device *device = df->device;
	int ret;

	ret = VFIO_COPY_USER_DATA((void __user *)arg, &attach,
				  struct vfio_device_attach_iommufd_pt,
				  pt_id, pasid, VFIO_DEVICE_ATTACH_PASID,
				  VFIO_DEVICE_ATTACH_PASID);
	if (ret)
		return ret;

	if ((attach.flags & VFIO_DEVICE_ATTACH_PASID) &&
	    !device->ops->pasid_attach_ioas)
		return -EOPNOTSUPP;

	mutex_lock(&device->dev_set->lock);
	if (attach.flags & VFIO_DEVICE_ATTACH_PASID)
		ret = device->ops->pasid_attach_ioas(device, attach.pasid,
						     &attach.pt_id);
	else
		ret = device->ops->attach_ioas(device, &attach.pt_id);
	if (ret)
		goto out_unlock;

	if (copy_to_user(&arg->pt_id, &attach.pt_id, sizeof(attach.pt_id))) {
		ret = -EFAULT;
		goto out_detach;
	}
	mutex_unlock(&device->dev_set->lock);
printk("%s succ pasid: %u\n", __func__, attach.pasid);
	return 0;

out_detach:
	device->ops->detach_ioas(device);
out_unlock:
	mutex_unlock(&device->dev_set->lock);
printk("%s err pasid: %u\n", __func__, attach.pasid);
	return ret;
}

int vfio_df_ioctl_detach_pt(struct vfio_device_file *df,
			    struct vfio_device_detach_iommufd_pt __user *arg)
{
	struct vfio_device_detach_iommufd_pt detach;
	struct vfio_device *device = df->device;
	int ret;

	ret = VFIO_COPY_USER_DATA((void __user *)arg, &detach,
				  struct vfio_device_detach_iommufd_pt,
				  flags, pasid, VFIO_DEVICE_DETACH_PASID,
				  VFIO_DEVICE_DETACH_PASID);
	if (ret)
		return ret;

	if ((detach.flags & VFIO_DEVICE_DETACH_PASID) &&
	    !device->ops->pasid_detach_ioas)
		return -EOPNOTSUPP;

	mutex_lock(&device->dev_set->lock);
	if (detach.flags & VFIO_DEVICE_DETACH_PASID)
		device->ops->pasid_detach_ioas(device, detach.pasid);
	else
		device->ops->detach_ioas(device);
	mutex_unlock(&device->dev_set->lock);

printk("%s succ pasid: %u\n", __func__, detach.pasid);
	return 0;
}

static char *vfio_device_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "vfio/devices/%s", dev_name(dev));
}

int vfio_cdev_init(struct class *device_class)
{
	device_class->devnode = vfio_device_devnode;
	return alloc_chrdev_region(&device_devt, 0,
				   MINORMASK + 1, "vfio-dev");
}

void vfio_cdev_cleanup(void)
{
	unregister_chrdev_region(device_devt, MINORMASK + 1);
}
