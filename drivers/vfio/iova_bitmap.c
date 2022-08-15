// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, Oracle and/or its affiliates.
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */
#include <linux/iova_bitmap.h>
#include <linux/highmem.h>

#define BITS_PER_PAGE (PAGE_SIZE * BITS_PER_BYTE)

static void iova_bitmap_iter_put(struct iova_bitmap_iter *iter);

/*
 * Converts a relative IOVA to a bitmap index.
 * The bitmap is viewed an array of u64, and each u64 represents
 * a range of IOVA, and the whole pinned pages to the range window.
 * Relative IOVA means relative to the iter::dirty base IOVA (stored
 * in dirty::iova). All computations in this file are done using
 * relative IOVAs and thus avoid an extra subtraction against
 * dirty::iova. The user API iova_bitmap_set() always uses a regular
 * absolute IOVAs.
 */
static unsigned long iova_bitmap_iova_to_index(struct iova_bitmap_iter *iter,
					       unsigned long iova)
{
	unsigned long pgsize = 1 << iter->dirty.pgshift;

	return iova / (BITS_PER_TYPE(*iter->data) * pgsize);
}

/*
 * Converts a bitmap index to a *relative* IOVA.
 */
static unsigned long iova_bitmap_index_to_iova(struct iova_bitmap_iter *iter,
					       unsigned long index)
{
	unsigned long pgshift = iter->dirty.pgshift;

	return (index * BITS_PER_TYPE(*iter->data)) << pgshift;
}

/*
 * Pins the bitmap user pages for the current range window.
 * This is internal to IOVA bitmap and called when advancing the
 * iterator.
 */
static int iova_bitmap_iter_get(struct iova_bitmap_iter *iter)
{
	struct iova_bitmap *dirty = &iter->dirty;
	unsigned long npages;
	u64 __user *addr;
	long ret;

	/*
	 * @offset is the cursor of the currently mapped u64 words
	 * that we have access. And it indexes u64 bitmap word that is
	 * mapped. Anything before @offset is not mapped. The range
	 * @offset .. @count is mapped but capped at a maximum number
	 * of pages.
	 */
	npages = DIV_ROUND_UP((iter->count - iter->offset) *
			      sizeof(*iter->data), PAGE_SIZE);

	/*
	 * We always cap at max number of 'struct page' a base page can fit.
	 * This is, for example, on x86 means 2M of bitmap data max.
	 */
	npages = min(npages,  PAGE_SIZE / sizeof(struct page *));
	addr = iter->data + iter->offset;
	ret = pin_user_pages_fast((unsigned long)addr, npages,
				  FOLL_WRITE, dirty->pages);
	if (ret <= 0)
		return -EFAULT;

	dirty->npages = (unsigned long)ret;
	/* Base IOVA where @pages point to i.e. bit 0 of the first page */
	dirty->iova = iova_bitmap_iova(iter);

	/*
	 * offset of the page where pinned pages bit 0 is located.
	 * This handles the case where the bitmap is not PAGE_SIZE
	 * aligned.
	 */
	dirty->start_offset = offset_in_page(addr);
	return 0;
}

/*
 * Unpins the bitmap user pages and clears @npages
 * (un)pinning is abstracted from API user and it's done
 * when advancing or freeing the iterator.
 */
static void iova_bitmap_iter_put(struct iova_bitmap_iter *iter)
{
	struct iova_bitmap *dirty = &iter->dirty;

	if (dirty->npages) {
		unpin_user_pages(dirty->pages, dirty->npages);
		dirty->npages = 0;
	}
}

int iova_bitmap_iter_init(struct iova_bitmap_iter *iter,
			  unsigned long iova, unsigned long length,
			  unsigned long page_size, u64 __user *data)
{
	struct iova_bitmap *dirty = &iter->dirty;

	memset(iter, 0, sizeof(*iter));
	dirty->pgshift = __ffs(page_size);
	iter->data = data;
	iter->count = iova_bitmap_iova_to_index(iter, length - 1) + 1;
	iter->iova = iova;
	iter->length = length;

	dirty->iova = iova;
	dirty->pages = (struct page **)__get_free_page(GFP_KERNEL);
	if (!dirty->pages)
		return -ENOMEM;

	return iova_bitmap_iter_get(iter);
}

void iova_bitmap_iter_free(struct iova_bitmap_iter *iter)
{
	struct iova_bitmap *dirty = &iter->dirty;

	iova_bitmap_iter_put(iter);

	if (dirty->pages) {
		free_page((unsigned long)dirty->pages);
		dirty->pages = NULL;
	}

	memset(iter, 0, sizeof(*iter));
}

unsigned long iova_bitmap_iova(struct iova_bitmap_iter *iter)
{
	unsigned long skip = iter->offset;

	return iter->iova + iova_bitmap_index_to_iova(iter, skip);
}

/*
 * Returns the remaining bitmap indexes count to process for the currently pinned
 * bitmap pages.
 */
static unsigned long iova_bitmap_iter_remaining(struct iova_bitmap_iter *iter)
{
	unsigned long remaining = iter->count - iter->offset;

	remaining = min_t(unsigned long, remaining,
		     (iter->dirty.npages << PAGE_SHIFT) / sizeof(*iter->data));

	return remaining;
}

unsigned long iova_bitmap_length(struct iova_bitmap_iter *iter)
{
	unsigned long max_iova = iter->iova + iter->length - 1;
	unsigned long iova = iova_bitmap_iova(iter);
	unsigned long remaining;

	/*
	 * iova_bitmap_iter_remaining() returns a number of indexes which
	 * when converted to IOVA gives us a max length that the bitmap
	 * pinned data can cover. Afterwards, that is capped to
	 * only cover the IOVA range in @iter::iova .. iter::length.
	 */
	remaining = iova_bitmap_index_to_iova(iter,
			iova_bitmap_iter_remaining(iter));

	if (iova + remaining - 1 > max_iova)
		remaining -= ((iova + remaining - 1) - max_iova);

	return remaining;
}

bool iova_bitmap_iter_done(struct iova_bitmap_iter *iter)
{
	return iter->offset >= iter->count;
}

int iova_bitmap_iter_advance(struct iova_bitmap_iter *iter)
{
	unsigned long iova = iova_bitmap_length(iter) - 1;
	unsigned long count = iova_bitmap_iova_to_index(iter, iova) + 1;

	iter->offset += count;

	iova_bitmap_iter_put(iter);
	if (iova_bitmap_iter_done(iter))
		return 0;

	/* When we advance the iterator we pin the next set of bitmap pages */
	return iova_bitmap_iter_get(iter);
}

unsigned long iova_bitmap_set(struct iova_bitmap *dirty,
			      unsigned long iova, unsigned long length)
{
	unsigned long nbits = max(1UL, length >> dirty->pgshift), set = nbits;
	unsigned long offset = (iova - dirty->iova) >> dirty->pgshift;
	unsigned long page_idx = offset / BITS_PER_PAGE;
	unsigned long page_offset = dirty->start_offset;
	void *kaddr;

	offset = offset % BITS_PER_PAGE;

	do {
		unsigned long size = min(BITS_PER_PAGE - offset, nbits);

		kaddr = kmap_local_page(dirty->pages[page_idx]);
		bitmap_set(kaddr + page_offset, offset, size);
		kunmap_local(kaddr);
		page_offset = offset = 0;
		nbits -= size;
		page_idx++;
	} while (nbits > 0);

	return set;
}
EXPORT_SYMBOL_GPL(iova_bitmap_set);
