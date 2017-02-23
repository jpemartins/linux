/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "common.h"
#include <linux/hashtable.h>

static void *xenvif_map_table(struct xenvif *vif, u32 queue_id,
			      grant_ref_t gref,
			      struct gnttab_map_grant_ref *gop)
{
	struct xenvif_queue *queue;
	struct page *page;
	int err;

	if (queue_id >= vif->num_queues)
		return NULL;

	queue = &vif->queues[queue_id];
	page = virt_to_page(queue->grant.opaque);

	gnttab_set_map_op(gop,
			  (unsigned long)pfn_to_kaddr(page_to_pfn(page)),
			  GNTMAP_host_map, gref, vif->domid);

	err = gnttab_map_refs(gop, NULL, &page, 1);

	if (err || gop->status != GNTST_okay)
		return NULL;

	return queue->grant.opaque;
}

static void xenvif_unmap_table(struct xenvif *vif, u32 queue_id,
			       struct gnttab_map_grant_ref *mop)
{
	struct xenvif_queue *queue = &vif->queues[queue_id];
	struct gnttab_unmap_grant_ref gop;
	struct page *page;
	int err;

	page = virt_to_page(queue->grant.opaque);

	gnttab_set_unmap_op(&gop,
			    (unsigned long)pfn_to_kaddr(page_to_pfn(page)),
			    mop->flags, mop->handle);

	err = gnttab_unmap_refs(&gop, NULL, &page, 1);
	if (err)
		netdev_dbg(vif->dev, "Unmap table fail: ret %d\n", err);
}

static struct xenvif_grant *xenvif_new_grant(struct xenvif_queue *queue,
					     grant_ref_t ref, bool readonly)
{
	struct xenvif_grant *entry = NULL;
	struct gnttab_map_grant_ref gop;
	struct page *page = NULL;
	uint32_t flags;
	int err;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		goto err;

	err = gnttab_alloc_pages(1, &page);
	if (err)
		goto err;

	flags = GNTMAP_host_map;
	if (readonly)
		flags |= GNTMAP_readonly;

	gnttab_set_map_op(&gop,
			  (unsigned long)pfn_to_kaddr(page_to_pfn(page)),
			  flags, ref, queue->vif->domid);

	err = gnttab_map_refs(&gop, NULL, &page, 1);

	if (err || gop.status != GNTST_okay)
		goto err;

	entry->ref = gop.ref;
	entry->handle = gop.handle;
	entry->flags = flags;
	entry->page = page;
	atomic_set(&entry->refcount, 1);

	hash_add(queue->grant.entries, &entry->node, entry->ref);
	queue->grant.count++;
	return entry;

err:
	if (page)
		gnttab_free_pages(1, &page);
	kfree(entry);
	return NULL;
}

static struct xenvif_grant *xenvif_find_grant(struct xenvif_queue *queue,
					      grant_ref_t ref)
{
	struct xenvif_grant_mapping *table = &queue->grant;
	struct xenvif_grant *entry = NULL;

	hash_for_each_possible(table->entries, entry, node, ref) {
		if (entry->ref == ref)
			break;
	}

	return entry;
}

static int xenvif_remove_grant(struct xenvif_queue *queue,
			       struct xenvif_grant *entry)
{
	struct gnttab_unmap_grant_ref gop;
	unsigned long addr;
	int err;

	if (atomic_read(&entry->refcount) > 1)
		return -EBUSY;

	addr = (unsigned long)pfn_to_kaddr(page_to_pfn(entry->page)),
	gnttab_set_unmap_op(&gop, addr, entry->flags, entry->handle);

	err = gnttab_unmap_refs(&gop, NULL, &entry->page, 1);

	if (err || gop.status)
		return -EINVAL;

	hash_del(&entry->node);
	queue->grant.count--;

	gnttab_free_pages(1, &entry->page);
	kfree(entry);

	return 0;
}

struct xenvif_grant *xenvif_get_grant(struct xenvif_queue *queue,
				      grant_ref_t ref)
{
	struct xenvif_grant *grant = xenvif_find_grant(queue, ref);

	if (likely(grant))
		atomic_inc(&grant->refcount);

	return grant;
}

void xenvif_put_grant(struct xenvif_queue *queue, struct xenvif_grant *grant)
{
	if (atomic_dec_and_test(&grant->refcount))
		xenvif_remove_grant(queue, grant);
}

static inline int xenvif_map_grefs(struct xenvif *vif, u32 queue_id,
				   struct xen_netif_gref *entries,
				   u32 count)
{
	struct xenvif_queue *queue = &vif->queues[queue_id];
	struct xenvif_grant *entry = NULL;
	bool readonly;
	int i;

	for (i = 0; i < count; i++) {
		if (queue->grant.count >= xenvif_gref_mapping_size)
			break;

		readonly = (entries[i].flags & XEN_NETIF_CTRLF_GREF_readonly);
		entry = xenvif_new_grant(queue, entries[i].ref, readonly);
		if (!entry)
			entries[i].status =
				XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;
	}

	return i;
}

static inline int xenvif_unmap_grefs(struct xenvif *vif, u32 queue_id,
				     struct xen_netif_gref *entries,
				     u32 count)
{
	struct xenvif_queue *queue = &vif->queues[queue_id];
	struct xenvif_grant *entry;
	int i;

	for (i = 0; i < count; i++) {
		entry = xenvif_find_grant(queue, entries[i].ref);
		if (!entry || xenvif_remove_grant(queue, entry)) {
			entries[i].status =
				XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;
			continue;
		}
	}

	return i;
}

static inline void xenvif_unmap_all_grefs(struct xenvif_queue *queue)
{
	struct xenvif_grant_mapping *table = &queue->grant;
	struct xenvif_grant *entry = NULL;
	struct hlist_node *tmp;
	unsigned int bkt;

	hash_for_each_safe(table->entries, bkt, tmp, entry, node)
		xenvif_put_grant(queue, entry);
}

u32 xenvif_get_gref_mapping_size(struct xenvif *vif, u32 queue_id, u32 *num)
{
	if (queue_id < 0 || queue_id >= vif->num_queues)
		return XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;

	*num = xenvif_gref_mapping_size - vif->queues[queue_id].grant.count;

	return XEN_NETIF_CTRL_STATUS_SUCCESS;
}

u32 xenvif_add_gref_mapping(struct xenvif *vif, u32 queue_id, grant_ref_t gref,
			    u32 size, u32 *num)
{
	struct xen_netif_gref *entries = NULL;
	struct gnttab_map_grant_ref mop;

	entries = (struct xen_netif_gref *)
		xenvif_map_table(vif, queue_id, gref, &mop);
	if (!entries)
		return XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;

	*num = xenvif_map_grefs(vif, queue_id, entries, size);

	xenvif_unmap_table(vif, queue_id, &mop);

	return *num != 0 ? XEN_NETIF_CTRL_STATUS_SUCCESS :
		XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;
}

u32 xenvif_del_gref_mapping(struct xenvif *vif, u32 queue_id, grant_ref_t gref,
			    u32 size, u32 *num)
{
	struct xen_netif_gref *entries = NULL;
	struct gnttab_map_grant_ref mop;

	entries = (struct xen_netif_gref *)
		xenvif_map_table(vif, queue_id, gref, &mop);
	if (!entries)
		return XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;

	*num = xenvif_unmap_grefs(vif, queue_id, entries, size);

	xenvif_unmap_table(vif, queue_id, &mop);

	return *num != 0 ? XEN_NETIF_CTRL_STATUS_SUCCESS :
		XEN_NETIF_CTRL_STATUS_INVALID_PARAMETER;
}

void xenvif_init_grant(struct xenvif_queue *queue)
{
	struct page *page;
	int err;

	err = gnttab_alloc_pages(1, &page);
	if (err)
		return;

	hash_init(queue->grant.entries);
	queue->grant.opaque = (void *)page_to_virt(page);
}

void xenvif_deinit_grant(struct xenvif_queue *queue)
{
	struct page *page;

	if (!queue->grant.opaque)
		return;

	page = virt_to_page(queue->grant.opaque);
	xenvif_unmap_all_grefs(queue);
	gnttab_free_pages(1, &page);
}

#ifdef CONFIG_DEBUG_FS
void xenvif_dump_grant_info(struct xenvif_queue *queue, struct seq_file *m)
{
	struct xenvif_grant_mapping *table = &queue->grant;
	struct xenvif_grant *entry = NULL;
	unsigned int bkt, i = 0;
	struct hlist_node *tmp;

	seq_printf(m, "\nMapped grants: (count %u)\n", table->count);

	hash_for_each_safe(table->entries, bkt, tmp, entry, node) {
		seq_printf(m, "%u:%s(%x) ",
			   entry->ref,
			   entry->flags & GNTMAP_readonly ? "r" : "rw",
			   entry->flags);

		/* Occasionally print a newline for each 10 grants printed */
		if (!(++i % 10))
			seq_puts(m, "\n");
	}

	seq_puts(m, "\n");
}
#endif /* CONFIG_DEBUG_FS */
