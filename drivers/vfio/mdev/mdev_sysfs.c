// SPDX-License-Identifier: GPL-2.0-only
/*
 * File attributes for Mediated devices
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *             Kirti Wankhede <kwankhede@nvidia.com>
 */

#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/mdev.h>

#include "mdev_private.h"

/* Static functions */

static ssize_t mdev_type_attr_show(struct kobject *kobj,
				     struct attribute *__attr, char *buf)
{
	struct mdev_type_attribute *attr = to_mdev_type_attr(__attr);
	struct mdev_type *type = to_mdev_type(kobj);
	ssize_t ret = -EIO;

	if (attr->show)
		ret = attr->show(type, attr, buf);
	return ret;
}

static ssize_t mdev_type_attr_store(struct kobject *kobj,
				      struct attribute *__attr,
				      const char *buf, size_t count)
{
	struct mdev_type_attribute *attr = to_mdev_type_attr(__attr);
	struct mdev_type *type = to_mdev_type(kobj);
	ssize_t ret = -EIO;

	if (attr->store)
		ret = attr->store(type, attr, buf, count);
	return ret;
}

static const struct sysfs_ops mdev_type_sysfs_ops = {
	.show = mdev_type_attr_show,
	.store = mdev_type_attr_store,
};

static ssize_t create_store(struct mdev_type *mtype,
			    struct mdev_type_attribute *attr, const char *buf,
			    size_t count)
{
	char *str;
	guid_t uuid;
	int ret;

	if ((count < UUID_STRING_LEN) || (count > UUID_STRING_LEN + 1))
		return -EINVAL;

	str = kstrndup(buf, count, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	ret = guid_parse(str, &uuid);
	kfree(str);
	if (ret)
		return ret;

	ret = mdev_device_create(mtype, &uuid);
	if (ret)
		return ret;

	return count;
}

static MDEV_TYPE_ATTR_WO(create);

static void mdev_type_release(struct kobject *kobj)
{
	struct mdev_type *type = to_mdev_type(kobj);

	pr_debug("Releasing group %s\n", kobj->name);
	kfree(type);
}

static struct kobj_type mdev_type_ktype = {
	.sysfs_ops = &mdev_type_sysfs_ops,
	.release = mdev_type_release,
};

int mdev_type_add(struct mdev_parent *parent, struct mdev_type *type)
{
	int ret;

	type->parent = parent;
	type->kobj.kset = parent->mdev_types_kset;
	ret = kobject_init_and_add(&type->kobj, &mdev_type_ktype, NULL, "%s-%s",
				   dev_driver_string(parent->dev),
				   type->sysfs_name);
	if (ret)
		return ret;

	ret = sysfs_create_file(&type->kobj, &mdev_type_attr_create.attr);
	if (ret)
		goto attr_create_failed;

	type->devices_kobj = kobject_create_and_add("devices", &type->kobj);
	if (!type->devices_kobj) {
		ret = -ENOMEM;
		goto attr_devices_failed;
	}

	ret = sysfs_create_files(&type->kobj, parent->mdev_driver->types_attrs);
	if (ret)
		goto attrs_failed;
	return 0;

attrs_failed:
	kobject_put(type->devices_kobj);
attr_devices_failed:
	sysfs_remove_file(&type->kobj, &mdev_type_attr_create.attr);
attr_create_failed:
	kobject_del(&type->kobj);
	kobject_put(&type->kobj);
	return ret;
}

void mdev_type_remove(struct mdev_type *type)
{
	sysfs_remove_files(&type->kobj, type->parent->mdev_driver->types_attrs);

	kobject_put(type->devices_kobj);
	sysfs_remove_file(&type->kobj, &mdev_type_attr_create.attr);
	kobject_del(&type->kobj);
	kobject_put(&type->kobj);
}

static ssize_t remove_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct mdev_device *mdev = to_mdev_device(dev);
	unsigned long val;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	if (val && device_remove_file_self(dev, attr)) {
		int ret;

		ret = mdev_device_remove(mdev);
		if (ret)
			return ret;
	}

	return count;
}

static DEVICE_ATTR_WO(remove);

static struct attribute *mdev_device_attrs[] = {
	&dev_attr_remove.attr,
	NULL,
};

static const struct attribute_group mdev_device_group = {
	.attrs = mdev_device_attrs,
};

const struct attribute_group *mdev_device_groups[] = {
	&mdev_device_group,
	NULL
};

int mdev_create_sysfs_files(struct mdev_device *mdev)
{
	struct mdev_type *type = mdev->type;
	struct kobject *kobj = &mdev->dev.kobj;
	int ret;

	ret = sysfs_create_link(type->devices_kobj, kobj, dev_name(&mdev->dev));
	if (ret)
		return ret;

	ret = sysfs_create_link(kobj, &type->kobj, "mdev_type");
	if (ret)
		goto type_link_failed;
	return ret;

type_link_failed:
	sysfs_remove_link(mdev->type->devices_kobj, dev_name(&mdev->dev));
	return ret;
}

void mdev_remove_sysfs_files(struct mdev_device *mdev)
{
	struct kobject *kobj = &mdev->dev.kobj;

	sysfs_remove_link(kobj, "mdev_type");
	sysfs_remove_link(mdev->type->devices_kobj, dev_name(&mdev->dev));
}
