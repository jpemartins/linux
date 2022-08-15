/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022, Oracle and/or its affiliates.
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */
#ifndef _IOVA_BITMAP_H_
#define _IOVA_BITMAP_H_

#include <linux/mm.h>

/**
 * struct iova_bitmap - A bitmap representing a portion IOVA space
 *
 * Main data structure for tracking dirty IOVAs.
 *
 * For example something recording dirty IOVAs, will be provided of a
 * struct iova_bitmap structure. This structure only represents a
 * subset of the total IOVA space pinned by its parent counterpart
 * iterator object.
 *
 * The user does not need to exact location of the bits in the bitmap.
 * From user perspective the bitmap the only API available to the dirty
 * tracker is iova_bitmap_set() which records the dirty IOVA *range*
 * in the bitmap data.
 *
 * The bitmap is an array of u64 whereas each bit represents an IOVA
 * of range of (1 << pgshift). Thus formula for the bitmap data to be
 * set is:
 *
 *   data[(iova / page_size) / 64] & (1ULL << (iova % 64))
 */
struct iova_bitmap {
	/* base IOVA representing bit 0 of the first page */
	unsigned long iova;

	/* page size order that each bit granules to */
	unsigned long pgshift;

	/* offset of the first user page pinned */
	unsigned long start_offset;

	/* number of pages pinned */
	unsigned long npages;

	/* pinned pages representing the bitmap data */
	struct page **pages;
};

/**
 * struct iova_bitmap_iter - Iterator object of the IOVA bitmap
 *
 * Main data structure for walking the bitmap data.
 *
 * Abstracts the pinning work to iterate an IOVA ranges.
 * It uses a windowing scheme and pins the bitmap in relatively
 * big ranges e.g.
 *
 * The iterator uses one base page to store all the pinned pages
 * pointers related to the bitmap. For sizeof(struct page) == 64 it
 * stores 512 struct pages which, if base page size is 4096 it means 2M
 * of bitmap data is pinned at a time. If the iova_bitmap page size is
 * also base page size then the range window to iterate is 64G.
 *
 * For example iterating on a total IOVA range of 4G..128G, it will
 * walk through this set of ranges:
 *
 *  - 4G  -  68G-1 (64G)
 *  - 68G - 128G-1 (64G)
 *
 * An example of the APIs on how to iterate the IOVA bitmap:
 *
 *   ret = iova_bitmap_iter_init(&iter, iova, PAGE_SIZE, length, data);
 *   if (ret)
 *       return -ENOMEM;
 *
 *   for (; !iova_bitmap_iter_done(&iter) && !ret;
 *        ret = iova_bitmap_iter_advance(&iter)) {
 *
 *        dirty_reporter_ops(&iter.dirty, iova_bitmap_iova(&iter),
 *                           iova_bitmap_length(&iter));
 *   }
 *
 * An implementation of the lower end (referred to above as
 * dirty_reporter_ops) that is tracking dirty bits would:
 *
 *        if (iova_dirty)
 *            iova_bitmap_set(&iter.dirty, iova, PAGE_SIZE);
 *
 * The internals of the object use a cursor @offset that indexes
 * which part u64 word of the bitmap is mapped, up to @count.
 * Those keep being incremented until @count reaches while mapping
 * up to PAGE_SIZE / sizeof(struct page*) maximum of pages.
 *
 * The iterator is usually located on what tracks DMA mapped ranges
 * or some form of IOVA range tracking that co-relates to the user
 * passed bitmap.
 */
struct iova_bitmap_iter {
	/* IOVA range representing the currently pinned bitmap data */
	struct iova_bitmap dirty;

	/* userspace address of the bitmap */
	u64 __user *data;

	/* u64 index that @dirty points to */
	size_t offset;

	/* how many u64 can we walk in total */
	size_t count;

	/* base IOVA of the whole bitmap */
	unsigned long iova;

	/* length of the IOVA range for the whole bitmap */
	unsigned long length;
};

/**
 * iova_bitmap_iter_init() - Initializes an IOVA bitmap iterator object.
 * @iter: IOVA bitmap iterator to initialize
 * @iova: Start address of the IOVA range
 * @length: Length of the IOVA range
 * @page_size: Page size of the IOVA bitmap. It defines what each bit
 *             granularity represents
 * @data: Userspace address of the bitmap
 *
 * Initializes all the fields in the IOVA iterator including the first
 * user pages of @data. Returns 0 on success or otherwise errno on error.
 */
int iova_bitmap_iter_init(struct iova_bitmap_iter *iter, unsigned long iova,
			  unsigned long length, unsigned long page_size,
			  u64 __user *data);

/**
 * iova_bitmap_iter_free() - Frees an IOVA bitmap iterator object
 * @iter: IOVA bitmap iterator to free
 *
 * It unpins and releases pages array memory and clears any leftover
 * state.
 */
void iova_bitmap_iter_free(struct iova_bitmap_iter *iter);

/**
 * iova_bitmap_iter_done: Checks if the IOVA bitmap has data to iterate
 * @iter: IOVA bitmap iterator to free
 *
 * Returns true if there's more data to iterate.
 */
bool iova_bitmap_iter_done(struct iova_bitmap_iter *iter);

/**
 * iova_bitmap_iter_advance: Advances the IOVA bitmap iterator
 * @iter: IOVA bitmap iterator to advance
 *
 * Advances to the next range, releases the current pinned
 * pages and pins the next set of bitmap pages.
 * Returns 0 on success or otherwise errno.
 */
int iova_bitmap_iter_advance(struct iova_bitmap_iter *iter);

/**
 * iova_bitmap_iova: Base IOVA of the current range
 * @iter: IOVA bitmap iterator
 *
 * Returns the base IOVA of the current range.
 */
unsigned long iova_bitmap_iova(struct iova_bitmap_iter *iter);

/**
 * iova_bitmap_length: IOVA length of the current range
 * @iter: IOVA bitmap iterator
 *
 * Returns the length of the current IOVA range.
 */
unsigned long iova_bitmap_length(struct iova_bitmap_iter *iter);

/**
 * iova_bitmap_set: Marks an IOVA range as dirty
 * @dirty: IOVA bitmap
 * @iova: IOVA to mark as dirty
 * @length: IOVA range length
 *
 * Marks the range [iova .. iova+length-1] as dirty in the bitmap.
 * Returns the number of bits set.
 */
unsigned long iova_bitmap_set(struct iova_bitmap *dirty,
			      unsigned long iova, unsigned long length);

#endif
