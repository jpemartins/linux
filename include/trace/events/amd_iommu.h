/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2021 Oracle Corporation */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM amd_iommu

#if !defined(_TRACE_AMD_IOMMU_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_AMD_IOMMU_H

#include <linux/tracepoint.h>

TRACE_EVENT(iommu_v1_sync_dirty_log,
	TP_PROTO(unsigned long iova, unsigned long size, unsigned long npages),

	TP_ARGS(iova, size, npages),

	TP_STRUCT__entry(
		__field(unsigned long, iova)
		__field(unsigned long, size)
		__field(unsigned long, npages)
	),

	TP_fast_assign(
		__entry->iova = iova;
		__entry->size = size;
		__entry->npages = npages;
	),

	TP_printk("0x%lx 0x%lx 0x%lu",
		__entry->iova, __entry->size, __entry->npages
	)
);

#endif /* _TRACE_AMD_IOMMU_H */

#include <trace/define_trace.h>
