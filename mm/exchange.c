/*
 * Exchange two in-use pages. Page flags and page->mapping are exchanged
 * as well. Only anonymous pages are supported.
 *
 * Copyright (C) 2016 NVIDIA, Zi Yan <ziy@nvidia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include <linux/syscalls.h>
#include <linux/migrate.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/mm_inline.h>
#include <linux/page_idle.h>
#include <linux/page-flags.h>
#include <linux/ksm.h>
#include <linux/memcontrol.h>
#include <linux/balloon_compaction.h>
#include <linux/buffer_head.h>


#include "internal.h"

/*
 * Move a list of individual pages
 */
struct pages_to_node {
	unsigned long from_addr;
	int from_status;

	unsigned long to_addr;
	int to_status;
};

struct exchange_page_info {
	struct page *from_page;
	struct page *to_page;

	struct anon_vma *from_anon_vma;
	struct anon_vma *to_anon_vma;

	struct list_head list;
};

struct page_flags {
	unsigned int page_error :1;
	unsigned int page_referenced:1;
	unsigned int page_uptodate:1;
	unsigned int page_active:1;
	unsigned int page_unevictable:1;
	unsigned int page_checked:1;
	unsigned int page_mappedtodisk:1;
	unsigned int page_dirty:1;
	unsigned int page_is_young:1;
	unsigned int page_is_idle:1;
	unsigned int page_swapcache:1;
	unsigned int page_writeback:1;
	unsigned int page_private:1;
	unsigned int __pad:3;
};


static void exchange_page(char *to, char *from)
{
	u64 tmp;
	int i;

	for (i = 0; i < PAGE_SIZE; i += sizeof(tmp)) {
		tmp = *((u64*)(from + i));
		*((u64*)(from + i)) = *((u64*)(to + i));
		*((u64*)(to + i)) = tmp;
	}
}

static inline void exchange_highpage(struct page *to, struct page *from)
{
	char *vfrom, *vto;

	vfrom = kmap_atomic(from);
	vto = kmap_atomic(to);
	exchange_page(vto, vfrom);
	kunmap_atomic(vto);
	kunmap_atomic(vfrom);
}

static void __exchange_gigantic_page(struct page *dst, struct page *src,
				int nr_pages)
{
	int i;
	struct page *dst_base = dst;
	struct page *src_base = src;

	for (i = 0; i < nr_pages; ) {
		cond_resched();
		exchange_highpage(dst, src);

		i++;
		dst = mem_map_next(dst, dst_base, i);
		src = mem_map_next(src, src_base, i);
	}
}

static void exchange_huge_page(struct page *dst, struct page *src)
{
	int i;
	int nr_pages;

	if (PageHuge(src)) {
		/* hugetlbfs page */
		struct hstate *h = page_hstate(src);
		nr_pages = pages_per_huge_page(h);

		if (unlikely(nr_pages > MAX_ORDER_NR_PAGES)) {
			__exchange_gigantic_page(dst, src, nr_pages);
			return;
		}
	} else {
		/* thp page */
		BUG_ON(!PageTransHuge(src));
		nr_pages = hpage_nr_pages(src);
	}

	for (i = 0; i < nr_pages; i++) {
		cond_resched();
		exchange_highpage(dst + i, src + i);
	}
}

/*
 * Copy the page to its new location without polluting cache
 */
static void exchange_page_flags(struct page *to_page, struct page *from_page)
{
	int from_cpupid, to_cpupid;
	struct page_flags from_page_flags, to_page_flags;
	struct mem_cgroup *to_memcg = page_memcg(to_page),
					  *from_memcg = page_memcg(from_page);

	from_cpupid = page_cpupid_xchg_last(from_page, -1);

	from_page_flags.page_error = TestClearPageError(from_page);
	from_page_flags.page_referenced = TestClearPageReferenced(from_page);
	from_page_flags.page_uptodate = PageUptodate(from_page);
	ClearPageUptodate(from_page);
	from_page_flags.page_active = TestClearPageActive(from_page);
	from_page_flags.page_unevictable = TestClearPageUnevictable(from_page);
	from_page_flags.page_checked = PageChecked(from_page);
	ClearPageChecked(from_page);
	from_page_flags.page_mappedtodisk = PageMappedToDisk(from_page);
	ClearPageMappedToDisk(from_page);
	from_page_flags.page_dirty = PageDirty(from_page);
	ClearPageDirty(from_page);
	from_page_flags.page_is_young = test_and_clear_page_young(from_page);
	from_page_flags.page_is_idle = page_is_idle(from_page);
	clear_page_idle(from_page);
	from_page_flags.page_swapcache = PageSwapCache(from_page);
	from_page_flags.page_private = PagePrivate(from_page);
	ClearPagePrivate(from_page);
	from_page_flags.page_writeback = test_clear_page_writeback(from_page);


	to_cpupid = page_cpupid_xchg_last(to_page, -1);

	to_page_flags.page_error = TestClearPageError(to_page);
	to_page_flags.page_referenced = TestClearPageReferenced(to_page);
	to_page_flags.page_uptodate = PageUptodate(to_page);
	ClearPageUptodate(to_page);
	to_page_flags.page_active = TestClearPageActive(to_page);
	to_page_flags.page_unevictable = TestClearPageUnevictable(to_page);
	to_page_flags.page_checked = PageChecked(to_page);
	ClearPageChecked(to_page);
	to_page_flags.page_mappedtodisk = PageMappedToDisk(to_page);
	ClearPageMappedToDisk(to_page);
	to_page_flags.page_dirty = PageDirty(to_page);
	ClearPageDirty(to_page);
	to_page_flags.page_is_young = test_and_clear_page_young(to_page);
	to_page_flags.page_is_idle = page_is_idle(to_page);
	clear_page_idle(to_page);
	to_page_flags.page_swapcache = PageSwapCache(to_page);
	to_page_flags.page_private = PagePrivate(to_page);
	ClearPagePrivate(to_page);
	to_page_flags.page_writeback = test_clear_page_writeback(to_page);

	/* set to_page */
	if (from_page_flags.page_error)
		SetPageError(to_page);
	if (from_page_flags.page_referenced)
		SetPageReferenced(to_page);
	if (from_page_flags.page_uptodate)
		SetPageUptodate(to_page);
	if (from_page_flags.page_active) {
		VM_BUG_ON_PAGE(from_page_flags.page_unevictable, from_page);
		SetPageActive(to_page);
	} else if (from_page_flags.page_unevictable)
		SetPageUnevictable(to_page);
	if (from_page_flags.page_checked)
		SetPageChecked(to_page);
	if (from_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(to_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (from_page_flags.page_dirty)
		SetPageDirty(to_page);

	if (from_page_flags.page_is_young)
		set_page_young(to_page);
	if (from_page_flags.page_is_idle)
		set_page_idle(to_page);

	/* set from_page */
	if (to_page_flags.page_error)
		SetPageError(from_page);
	if (to_page_flags.page_referenced)
		SetPageReferenced(from_page);
	if (to_page_flags.page_uptodate)
		SetPageUptodate(from_page);
	if (to_page_flags.page_active) {
		VM_BUG_ON_PAGE(to_page_flags.page_unevictable, from_page);
		SetPageActive(from_page);
	} else if (to_page_flags.page_unevictable)
		SetPageUnevictable(from_page);
	if (to_page_flags.page_checked)
		SetPageChecked(from_page);
	if (to_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(from_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (to_page_flags.page_dirty)
		SetPageDirty(from_page);

	if (to_page_flags.page_is_young)
		set_page_young(from_page);
	if (to_page_flags.page_is_idle)
		set_page_idle(from_page);

	/*
	 * Copy NUMA information to the new page, to prevent over-eager
	 * future migrations of this same page.
	 */
	page_cpupid_xchg_last(to_page, from_cpupid);
	page_cpupid_xchg_last(from_page, to_cpupid);

	ksm_exchange_page(to_page, from_page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(to_page);
	ClearPageSwapCache(from_page);
	if (from_page_flags.page_swapcache)
		SetPageSwapCache(to_page);
	if (to_page_flags.page_swapcache)
		SetPageSwapCache(from_page);


#ifdef CONFIG_PAGE_OWNER
	/* exchange page owner  */
	BUG();
#endif
	/* exchange mem cgroup  */
	to_page->mem_cgroup = from_memcg;
	from_page->mem_cgroup = to_memcg;

}

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */

static int exchange_page_move_mapping(struct address_space *to_mapping,
			struct address_space *from_mapping,
			struct page *to_page, struct page *from_page,
			enum migrate_mode mode,
			int to_extra_count, int from_extra_count)
{
	int to_expected_count = 1 + to_extra_count,
		from_expected_count = 1 + from_extra_count;
	unsigned long from_page_index = page_index(from_page),
				  to_page_index = page_index(to_page);
	int to_swapbacked = PageSwapBacked(to_page),
		from_swapbacked = PageSwapBacked(from_page);
	struct address_space *to_mapping_value = to_page->mapping,
						 *from_mapping_value = from_page->mapping;


	if (!to_mapping) {
		/* Anonymous page without mapping */
		if (page_count(to_page) != to_expected_count)
			return -EAGAIN;
	}

	if (!from_mapping) {
		/* Anonymous page without mapping */
		if (page_count(from_page) != from_expected_count)
			return -EAGAIN;
	}

	/*
	 * Now we know that no one else is looking at the page:
	 * no turning back from here.
	 */
	/* from_page  */
	from_page->index = to_page_index;
	from_page->mapping = to_mapping_value;

	ClearPageSwapBacked(from_page);
	if (to_swapbacked)
		SetPageSwapBacked(from_page);


	/* to_page  */
	to_page->index = from_page_index;
	to_page->mapping = from_mapping_value;

	ClearPageSwapBacked(to_page);
	if (from_swapbacked)
		SetPageSwapBacked(to_page);

	return MIGRATEPAGE_SUCCESS;
}

static int exchange_from_to_pages(struct page *to_page, struct page *from_page,
				enum migrate_mode mode)
{
	int rc = -EBUSY;
	struct address_space *to_page_mapping, *from_page_mapping;

	VM_BUG_ON_PAGE(!PageLocked(from_page), from_page);
	VM_BUG_ON_PAGE(!PageLocked(to_page), to_page);

	/* copy page->mapping not use page_mapping()  */
	to_page_mapping = page_mapping(to_page);
	from_page_mapping = page_mapping(from_page);

	BUG_ON(from_page_mapping);
	BUG_ON(to_page_mapping);

	BUG_ON(PageWriteback(from_page));
	BUG_ON(PageWriteback(to_page));

	/* actual page mapping exchange */
	rc = exchange_page_move_mapping(to_page_mapping, from_page_mapping,
						to_page, from_page, mode, 0, 0);
	/* actual page data exchange  */
	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	rc = -EFAULT;

	if (mode & MIGRATE_MT)
		rc = exchange_page_mthread(to_page, from_page,
				hpage_nr_pages(from_page));
	if (rc) {
		if (PageHuge(from_page) || PageTransHuge(from_page))
			exchange_huge_page(to_page, from_page);
		else
			exchange_highpage(to_page, from_page);
		rc = 0;
	}

	exchange_page_flags(to_page, from_page);

	return rc;
}

static int unmap_and_exchange_anon(struct page *from_page, struct page *to_page,
				enum migrate_mode mode)
{
	int rc = -EAGAIN;
	int from_page_was_mapped = 0, to_page_was_mapped = 0;
	struct anon_vma *anon_vma_from_page = NULL, *anon_vma_to_page = NULL;

	/* from_page lock down  */
	if (!trylock_page(from_page)) {
		if (mode & MIGRATE_ASYNC)
			goto out;

		lock_page(from_page);
	}

	BUG_ON(PageWriteback(from_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(from_page) && !PageKsm(from_page))
		anon_vma_from_page = page_get_anon_vma(from_page);

	/* to_page lock down  */
	if (!trylock_page(to_page)) {
		if (mode & MIGRATE_ASYNC)
			goto out_unlock;

		lock_page(to_page);
	}

	BUG_ON(PageWriteback(to_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(to_page) && !PageKsm(to_page))
		anon_vma_to_page = page_get_anon_vma(to_page);

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!from_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(from_page), from_page);
		if (page_has_private(from_page)) {
			try_to_free_buffers(from_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(from_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(from_page) && !PageKsm(from_page) &&
					   !anon_vma_from_page, from_page);
		try_to_unmap(from_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		from_page_was_mapped = 1;
	}

	if (!to_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(to_page), to_page);
		if (page_has_private(to_page)) {
			try_to_free_buffers(to_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(to_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(to_page) && !PageKsm(to_page) &&
					   !anon_vma_to_page, to_page);
		try_to_unmap(to_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		to_page_was_mapped = 1;
	}

	if (!page_mapped(from_page) && !page_mapped(to_page))
		rc = exchange_from_to_pages(to_page, from_page, mode);

	if (from_page_was_mapped)
		remove_migration_ptes(from_page,
			rc == MIGRATEPAGE_SUCCESS ? to_page : from_page, false);

	if (to_page_was_mapped)
		remove_migration_ptes(to_page,
			rc == MIGRATEPAGE_SUCCESS ? from_page : to_page, false);


out_unlock_both:
	if (anon_vma_to_page)
		put_anon_vma(anon_vma_to_page);
	unlock_page(to_page);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma_from_page)
		put_anon_vma(anon_vma_from_page);
	unlock_page(from_page);
out:

	return rc;
}

/*
 * Exchange pages in the exchange_list
 *
 * Caller should release the exchange_list resource.
 *
 * */
static int exchange_pages(struct list_head *exchange_list,
			enum migrate_mode mode,
			int reason)
{
	struct exchange_page_info *one_pair, *one_pair2;
	int failed = 0;

	list_for_each_entry_safe(one_pair, one_pair2, exchange_list, list) {
		struct page *from_page = one_pair->from_page;
		struct page *to_page = one_pair->to_page;
		int rc;

		if ((page_mapping(from_page) != NULL) ||
			(page_mapping(to_page) != NULL)) {
			++failed;
			goto putback;
		}

		
		rc = unmap_and_exchange_anon(from_page, to_page, mode);

		if (rc != MIGRATEPAGE_SUCCESS)
			++failed;

putback:
		putback_lru_page(from_page);
		putback_lru_page(to_page);

	}
	return failed;
}


static int unmap_pair_pages_concur(struct exchange_page_info *one_pair,
				int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	struct anon_vma *anon_vma_from_page = NULL, *anon_vma_to_page = NULL;
	struct page *from_page = one_pair->from_page;
	struct page *to_page = one_pair->to_page;

	/* from_page lock down  */
	if (!trylock_page(from_page)) {
		if (!force || (mode & MIGRATE_ASYNC))
			goto out;

		lock_page(from_page);
	}

	BUG_ON(PageWriteback(from_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(from_page) && !PageKsm(from_page))
		one_pair->from_anon_vma = anon_vma_from_page
					= page_get_anon_vma(from_page);

	/* to_page lock down  */
	if (!trylock_page(to_page)) {
		if (!force || (mode & MIGRATE_ASYNC))
			goto out_unlock;

		lock_page(to_page);
	}

	BUG_ON(PageWriteback(to_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(to_page) && !PageKsm(to_page))
		one_pair->to_anon_vma = anon_vma_to_page = page_get_anon_vma(to_page);

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!from_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(from_page), from_page);
		if (page_has_private(from_page)) {
			try_to_free_buffers(from_page);
			goto out_unlock_both;
		}
	} else {
		VM_BUG_ON_PAGE(!page_mapped(from_page), from_page);
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(from_page) && !PageKsm(from_page) &&
					   !anon_vma_from_page, from_page);
		rc = try_to_unmap(from_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
	}

	if (!to_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(to_page), to_page);
		if (page_has_private(to_page)) {
			try_to_free_buffers(to_page);
			goto out_unlock_both;
		}
	} else {
		VM_BUG_ON_PAGE(!page_mapped(to_page), to_page);
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(to_page) && !PageKsm(to_page) &&
					   !anon_vma_to_page, to_page);
		rc = try_to_unmap(to_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
	}

	return rc;

out_unlock_both:
	if (anon_vma_to_page)
		put_anon_vma(anon_vma_to_page);
	unlock_page(to_page);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma_from_page)
		put_anon_vma(anon_vma_from_page);
	unlock_page(from_page);
out:

	return rc;
}

static int exchange_page_mapping_concur(struct list_head *unmapped_list_ptr,
					   struct list_head *exchange_list_ptr,
						enum migrate_mode mode)
{
	int rc = -EBUSY;
	int nr_failed = 0;
	struct address_space *to_page_mapping, *from_page_mapping;
	struct exchange_page_info *one_pair, *one_pair2;

	list_for_each_entry_safe(one_pair, one_pair2, unmapped_list_ptr, list) {
		struct page *from_page = one_pair->from_page;
		struct page *to_page = one_pair->to_page;

		VM_BUG_ON_PAGE(!PageLocked(from_page), from_page);
		VM_BUG_ON_PAGE(!PageLocked(to_page), to_page);

		/* copy page->mapping not use page_mapping()  */
		to_page_mapping = page_mapping(to_page);
		from_page_mapping = page_mapping(from_page);

		BUG_ON(from_page_mapping);
		BUG_ON(to_page_mapping);

		BUG_ON(PageWriteback(from_page));
		BUG_ON(PageWriteback(to_page));

		/* actual page mapping exchange */
		rc = exchange_page_move_mapping(to_page_mapping, from_page_mapping,
							to_page, from_page, mode, 0, 0);

		if (rc) {
			list_move(&one_pair->list, exchange_list_ptr);
			++nr_failed;
		}
	}

	return nr_failed;
}

static int exchange_page_data_concur(struct list_head *unmapped_list_ptr,
									enum migrate_mode mode)
{
	struct exchange_page_info *one_pair;
	int num_pages = 0, idx = 0;
	struct page **src_page_list = NULL, **dst_page_list = NULL;
	unsigned long size = 0;
	int rc = -EFAULT;

	/* form page list  */
	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		++num_pages;
		size += PAGE_SIZE * hpage_nr_pages(one_pair->from_page);
	}

	src_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!src_page_list)
		return -ENOMEM;
	dst_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!dst_page_list)
		return -ENOMEM;

	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		src_page_list[idx] = one_pair->from_page;
		dst_page_list[idx] = one_pair->to_page;
		++idx;
	}

	BUG_ON(idx != num_pages);


	if (mode & MIGRATE_MT)
		rc = exchange_page_lists_mthread(dst_page_list, src_page_list,
				num_pages);

	if (rc) {
		list_for_each_entry(one_pair, unmapped_list_ptr, list) {
			if (PageHuge(one_pair->from_page) ||
				PageTransHuge(one_pair->from_page)) {
				exchange_huge_page(one_pair->to_page, one_pair->from_page);
			} else {
				exchange_highpage(one_pair->to_page, one_pair->from_page);
			}
		}
	}

	kfree(src_page_list);
	kfree(dst_page_list);

	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		exchange_page_flags(one_pair->to_page, one_pair->from_page);
	}
	
	return rc;
}

static int remove_migration_ptes_concur(struct list_head *unmapped_list_ptr)
{
	struct exchange_page_info *iterator;

	list_for_each_entry(iterator, unmapped_list_ptr, list) {
		remove_migration_ptes(iterator->from_page, iterator->to_page, false);
		remove_migration_ptes(iterator->to_page, iterator->from_page, false);

		unlock_page(iterator->from_page);

		if (iterator->from_anon_vma)
			put_anon_vma(iterator->from_anon_vma);

		unlock_page(iterator->to_page);

		if (iterator->to_anon_vma)
			put_anon_vma(iterator->to_anon_vma);


		putback_lru_page(iterator->from_page);
		iterator->from_page = NULL;

		putback_lru_page(iterator->to_page);
		iterator->to_page = NULL;
	}

	return 0;
}

static int exchange_pages_concur(struct list_head *exchange_list,
		enum migrate_mode mode, int reason)
{
	struct exchange_page_info *one_pair, *one_pair2;
	int pass = 0;
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int rc = 0;
	LIST_HEAD(serialized_list);
	LIST_HEAD(unmapped_list);

	for(pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		/* unmap and get new page for page_mapping(page) == NULL */
		list_for_each_entry_safe(one_pair, one_pair2, exchange_list, list) {
			cond_resched();

		/* We do not exchange huge pages and file-backed pages concurrently */
			if (PageHuge(one_pair->from_page) || PageHuge(one_pair->to_page)) {
				rc = -ENODEV;
			}
			else if ((page_mapping(one_pair->from_page) != NULL) ||
					 (page_mapping(one_pair->from_page) != NULL)) {
				rc = -ENODEV;
			}
			else
				rc = unmap_pair_pages_concur(one_pair,
											pass > 2,
											mode);

			switch(rc) {
			case -ENODEV:
				list_move(&one_pair->list, &serialized_list);
				break;
			case -ENOMEM:
				goto out;
			case -EAGAIN:
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				list_move(&one_pair->list, &unmapped_list);
				nr_succeeded++;
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, -ENOSYS, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				list_move(&one_pair->list, &serialized_list);
				nr_failed++;
				break;
			}
		}

		/* move page->mapping to new page, only -EAGAIN could happen  */
		exchange_page_mapping_concur(&unmapped_list, exchange_list, mode);


		/* copy pages in unmapped_list */
		exchange_page_data_concur(&unmapped_list, mode);


		/* remove migration pte, if old_page is NULL?, unlock old and new
		 * pages, put anon_vma, put old and new pages */
		remove_migration_ptes_concur(&unmapped_list);
	}

	nr_failed += retry;
	rc = nr_failed;

	list_for_each_entry_safe(one_pair, one_pair2, &serialized_list, list) {
		struct page *from_page = one_pair->from_page;
		struct page *to_page = one_pair->to_page;
		int rc;

		if ((page_mapping(from_page) != NULL) ||
			(page_mapping(to_page) != NULL)) {
			++nr_failed;
			goto putback;
		}

		
		rc = unmap_and_exchange_anon(from_page, to_page, mode);

		if (rc != MIGRATEPAGE_SUCCESS)
			++nr_failed;

putback:

		putback_lru_page(from_page);
		putback_lru_page(to_page);

	}
out:
	list_splice(&unmapped_list, exchange_list);
	list_splice(&serialized_list, exchange_list);

	return nr_failed?-EFAULT:0;
}
