/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef _UAPI_IOMMUFD_TEST_H
#define _UAPI_IOMMUFD_TEST_H

#include <linux/types.h>
#include <linux/iommufd.h>

enum {
	IOMMU_TEST_OP_ADD_RESERVED = 1,
	IOMMU_TEST_OP_MOCK_DOMAIN,
	IOMMU_TEST_OP_MD_CHECK_MAP,
	IOMMU_TEST_OP_MD_CHECK_REFS,
	IOMMU_TEST_OP_CREATE_ACCESS,
	IOMMU_TEST_OP_DESTROY_ACCESS_PAGES,
	IOMMU_TEST_OP_ACCESS_PAGES,
	IOMMU_TEST_OP_ACCESS_RW,
	IOMMU_TEST_OP_SET_TEMP_MEMORY_LIMIT,
	IOMMU_TEST_OP_MOCK_DOMAIN_REPLACE,
	IOMMU_TEST_OP_ACCESS_REPLACE_IOAS,
	IOMMU_TEST_OP_MD_CHECK_IOTLB,
	IOMMU_TEST_OP_DEV_CHECK_DATA,
	IOMMU_TEST_OP_PASID_ATTACH,
	IOMMU_TEST_OP_PASID_REPLACE,
	IOMMU_TEST_OP_PASID_DETACH,
	IOMMU_TEST_OP_PASID_CHECK_DOMAIN,
};

enum {
	MOCK_APERTURE_START = 1UL << 24,
	MOCK_APERTURE_LAST = (1UL << 31) - 1,
};

enum {
	MOCK_FLAGS_ACCESS_WRITE = 1 << 0,
	MOCK_FLAGS_ACCESS_SYZ = 1 << 16,
};

enum {
	MOCK_ACCESS_RW_WRITE = 1 << 0,
	MOCK_ACCESS_RW_SLOW_PATH = 1 << 2,
};

enum {
	MOCK_FLAGS_ACCESS_CREATE_NEEDS_PIN_PAGES = 1 << 0,
};

enum {
	MOCK_NESTED_DOMAIN_IOTLB_ID_MAX = 3,
	MOCK_NESTED_DOMAIN_IOTLB_NUM = 4,
};

struct iommu_test_cmd {
	__u32 size;
	__u32 op;
	__u32 id;
	__u32 __reserved;
	union {
		struct {
			__aligned_u64 start;
			__aligned_u64 length;
		} add_reserved;
		struct {
			/* #0 is invalid, any non-zero is meaningful */
			__u32 default_pasid;
			__u32 out_stdev_id;
			__u32 out_hwpt_id;
			/* out_idev_id is the standard iommufd_bind object */
			__u32 out_idev_id;
		} mock_domain;
		struct {
			__u32 pt_id;
		} mock_domain_replace;
		struct {
			__aligned_u64 iova;
			__aligned_u64 length;
			__aligned_u64 uptr;
		} check_map;
		struct {
			__aligned_u64 length;
			__aligned_u64 uptr;
			__u32 refs;
		} check_refs;
		struct {
			__u32 out_access_fd;
			__u32 flags;
		} create_access;
		struct {
			__u32 access_pages_id;
		} destroy_access_pages;
		struct {
			__u32 flags;
			__u32 out_access_pages_id;
			__aligned_u64 iova;
			__aligned_u64 length;
			__aligned_u64 uptr;
		} access_pages;
		struct {
			__aligned_u64 iova;
			__aligned_u64 length;
			__aligned_u64 uptr;
			__u32 flags;
		} access_rw;
		struct {
			__u32 limit;
		} memory_limit;
		struct {
			__u32 ioas_id;
		} access_replace_ioas;
		struct {
			__u32 id;
			__u32 iotlb;
		} check_iotlb;
		struct {
			__u32 val;
		} check_dev_data;
		struct {
			__u32 pasid;
			__u32 pt_id;
			/* @id is stdev_id for IOMMU_TEST_OP_PASID_ATTACH */
		} pasid_attach;
		struct {
			__u32 pasid;
			__u32 pt_id;
			/* @id is stdev_id for IOMMU_TEST_OP_PASID_ATTACH */
		} pasid_replace;
		struct {
			__u32 pasid;
			/* @id is stdev_id for IOMMU_TEST_OP_PASID_DETACH */
		} pasid_detach;
		struct {
			__u32 pasid;
			__u32 hwpt_id;
			__u64 out_result_ptr;
			/* @id is stdev_id for IOMMU_TEST_OP_HWPT_GET_DOMAIN */
		} pasid_check;
	};
	__u32 last;
};
#define IOMMU_TEST_CMD _IO(IOMMUFD_TYPE, IOMMUFD_CMD_BASE + 32)

/* Mock structs for IOMMU_DEVICE_GET_HW_INFO ioctl */
#define IOMMU_HW_INFO_TYPE_SELFTEST	0xfeedbeef
#define IOMMU_HW_INFO_SELFTEST_REGVAL	0xdeadbeef

struct iommu_test_hw_info {
	__u32 flags;
	__u32 test_reg;
};

#define IOMMU_DEVICE_DATA_SELFTEST	0xdadbeef

/**
 * struct iommu_test_device_data
 *
 * @val: Should be set to IOMMU_DEVICE_DATA_SELFTEST or unset to 0x0
 */
struct iommu_test_device_data {
	__u32 val;
};

/* Should not be equal to any defined value in enum iommu_hwpt_data_type */
#define IOMMU_HWPT_ALLOC_DATA_SELFTEST		0xdead

/**
 * struct iommu_hwpt_selftest
 *
 * @iotlb: default mock iotlb value, IOMMU_TEST_IOTLB_DEFAULT
 */
struct iommu_hwpt_selftest {
#define IOMMU_TEST_IOTLB_DEFAULT	0xbadbeef
	__u32 iotlb;
};

/**
 * struct iommu_hwpt_invalidate_selftest
 *
 * @flags: invalidate flags
 * @iotlb_id: invalidate iotlb entry index
 *
 * If IOMMU_TEST_INVALIDATE_ALL is set in @flags, @iotlb_id will be ignored
 */
struct iommu_hwpt_invalidate_selftest {
#define IOMMU_TEST_INVALIDATE_ALL	(1ULL << 0)
	__u32 flags;
	__u32 iotlb_id;
};

#define IOMMU_TEST_INVALIDATE_ERR_FETCH	0xdeadbeee
#define IOMMU_TEST_INVALIDATE_ERR_REQ	0xdeadbeef

#endif
