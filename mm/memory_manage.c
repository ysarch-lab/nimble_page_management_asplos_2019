/*
 * A syscall used to move pages between two nodes.
 */

#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/nodemask.h>
#include <linux/security.h>
#include <linux/syscalls.h>

#include "internal.h"

enum isolate_action {
	ISOLATE_COLD_PAGES = 1,
	ISOLATE_HOT_PAGES,
	ISOLATE_HOT_AND_COLD_PAGES,
};

static inline unsigned long memcg_size_node(struct mem_cgroup *memcg, int nid)
{
	unsigned long val = 0;
	int i;

	for (i = 0; i < NR_LRU_LISTS; i++)
		val += mem_cgroup_node_nr_lru_pages(memcg, nid, BIT(i));

	return val;
}

static inline unsigned long memcg_max_size_node(struct mem_cgroup *memcg, int nid)
{
	return memcg->nodeinfo[nid]->max_nr_base_pages;
}

static unsigned long isolate_lru_pages(unsigned long nr_to_scan,
		struct lruvec *lruvec,
		struct list_head *dst_base_page,
		struct list_head *dst_huge_page,
		unsigned long *nr_scanned,
		unsigned long *nr_taken_base_page,
		unsigned long *nr_taken_huge_page,
		isolate_mode_t mode, enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long scan, total_scan, nr_pages;
	LIST_HEAD(busy_list);

	scan = 0;
	for (total_scan = 0;
	     scan < nr_to_scan && nr_taken < nr_to_scan && !list_empty(src);
	     total_scan++) {
		struct page *page;

		page = lru_to_page(src);
		/*prefetchw_prev_lru_page(page, src, flags);*/

		VM_BUG_ON_PAGE(!PageLRU(page), page);

		/*
		 * Do not count skipped pages because that makes the function
		 * return with no isolated pages if the LRU mostly contains
		 * ineligible pages.  This causes the VM to not reclaim any
		 * pages, triggering a premature OOM.
		 */
		scan++;
		switch (__isolate_lru_page(page, mode)) {
		case 0:
			nr_pages = hpage_nr_pages(page);
			nr_taken += nr_pages;
			nr_zone_taken[page_zonenum(page)] += nr_pages;
			if (nr_pages == 1) {
				list_move(&page->lru, dst_base_page);
				*nr_taken_base_page += nr_pages;
			} else {
				list_move(&page->lru, dst_huge_page);
				*nr_taken_huge_page += nr_pages;
			}
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, &busy_list);
			continue;

		default:
			BUG();
		}
	}
	if (!list_empty(&busy_list))
		list_splice(&busy_list, src);

	*nr_scanned = total_scan;
	update_lru_sizes(lruvec, lru, nr_zone_taken);
	return nr_taken;
}

static unsigned long isolate_pages_from_lru_list(pg_data_t *pgdat,
		struct mem_cgroup *memcg, unsigned long nr_pages,
		struct list_head *base_page_list,
		struct list_head *huge_page_list,
		unsigned long *nr_taken_base_page,
		unsigned long *nr_taken_huge_page,
		enum isolate_action action)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	enum lru_list lru;
	unsigned long nr_all_taken = 0;

	lru_add_drain_all();

	if (nr_pages == ULONG_MAX)
		nr_pages = memcg_size_node(memcg, pgdat->node_id);

	for_each_evictable_lru(lru) {
		unsigned long nr_scanned, nr_taken;
		int file = is_file_lru(lru);

		if (action == ISOLATE_COLD_PAGES && is_active_lru(lru))
			continue;
		if (action == ISOLATE_HOT_PAGES && !is_active_lru(lru))
			continue;

		spin_lock_irq(&pgdat->lru_lock);

		nr_taken = isolate_lru_pages(nr_pages, lruvec, base_page_list,
					huge_page_list, &nr_scanned,
					nr_taken_base_page,
					nr_taken_huge_page,
					0, lru);

		__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

		spin_unlock_irq(&pgdat->lru_lock);

		nr_all_taken += nr_taken;

		if (nr_all_taken > nr_pages)
			break;
	}

	return nr_all_taken;
}

static int migrate_to_node(struct list_head *page_list, int nid,
		enum migrate_mode mode)
{
	bool migrate_concur = mode & MIGRATE_CONCUR;
	int num = 0;
	int from_nid;
	int err;

	if (list_empty(page_list))
		return num;

	from_nid = page_to_nid(list_first_entry(page_list, struct page, lru));

	if (migrate_concur)
		err = migrate_pages_concur(page_list, alloc_new_node_page,
			NULL, nid, mode, MR_SYSCALL);
	else
		err = migrate_pages(page_list, alloc_new_node_page,
			NULL, nid, mode, MR_SYSCALL);

	if (err) {
		struct page *page;

		list_for_each_entry(page, page_list, lru)
			num += hpage_nr_pages(page);
		pr_debug("%d pages failed to migrate from %d to %d\n",
			num, from_nid, nid);

		putback_movable_pages(page_list);
	}
	return num;
}

static inline int _putback_overflow_pages(unsigned long max_nr_pages,
		struct list_head *page_list)
{
	struct page *page;
	LIST_HEAD(putback_list);

	list_for_each_entry(page, page_list, lru) {
		int nr_pages = hpage_nr_pages(page);
		if (max_nr_pages <= nr_pages) {
			max_nr_pages = 0;
			break;
		}
		max_nr_pages -= nr_pages;
	}

	if (&page->lru != page_list) {
		list_cut_position(&putback_list, page_list, &page->lru);
		putback_movable_pages(page_list);
		list_splice(&putback_list, page_list);
	}

	return max_nr_pages;
}

static int putback_overflow_pages(unsigned long max_nr_base_pages,
		unsigned long max_nr_huge_pages,
		struct list_head *base_page_list,
		struct list_head *huge_page_list)
{
	/*
	 * counting pages in page lists and substract the number from max_nr_*
	 * when max_nr_* go to zero, drop the remaining pages
	 */
	max_nr_huge_pages += _putback_overflow_pages(max_nr_base_pages,
			base_page_list);
	_putback_overflow_pages(max_nr_huge_pages, huge_page_list);
	return 0;
}

static int do_mm_manage(struct task_struct *p, struct mm_struct *mm,
		const nodemask_t *from, const nodemask_t *to,
		unsigned long nr_pages, int flags)
{
	bool migrate_mt = flags & MPOL_MF_MOVE_MT;
	bool migrate_concur = flags & MPOL_MF_MOVE_CONCUR;
	bool migrate_dma = flags & MPOL_MF_MOVE_DMA;
	bool move_hot_and_cold_pages = flags & MPOL_MF_MOVE_ALL;
	/*bool exchange_pages = flags & MPOL_MF_EXCHANGE;*/
	/*bool migrate_pages_out = false;*/
	struct mem_cgroup *memcg = mem_cgroup_from_task(p);
	int err = 0;
	unsigned long nr_isolated_from_pages;
	unsigned long nr_isolated_from_base_pages = 0, nr_isolated_from_huge_pages = 0;
	unsigned long nr_isolated_to_pages;
	/* in case no migration from to node, we migrate all isolated pages from
	 * from node  */
	unsigned long nr_isolated_to_base_pages = ULONG_MAX,
				  nr_isolated_to_huge_pages = ULONG_MAX;
	unsigned long max_nr_pages_to_node, nr_pages_to_node;
	int from_nid, to_nid;
	enum migrate_mode mode = MIGRATE_SYNC |
		(migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
		(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD) |
		(migrate_concur ? MIGRATE_CONCUR : MIGRATE_SINGLETHREAD);
	enum isolate_action from_action =
		move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_HOT_PAGES;
	LIST_HEAD(from_base_page_list);
	LIST_HEAD(from_huge_page_list);

	VM_BUG_ON(!memcg);
	/* Let's handle simplest situation first */
	VM_BUG_ON(!(nodes_weight(*from) == 1 && nodes_weight(*to) == 1));

	if (memcg == root_mem_cgroup)
		return 0;

	from_nid = first_node(*from);
	to_nid = first_node(*to);

	max_nr_pages_to_node = memcg_max_size_node(memcg, to_nid);
	nr_pages_to_node = memcg_size_node(memcg, to_nid);

	/* do not migrate in more pages than to node can hold */
	nr_pages = min_t(unsigned long, max_nr_pages_to_node, nr_pages);

	/* if to node has enough space, migrate all possible pages in from node */
	if (nr_pages != ULONG_MAX &&
		nr_pages + nr_pages_to_node < max_nr_pages_to_node)
		from_action = ISOLATE_HOT_AND_COLD_PAGES;

	nr_isolated_from_pages = isolate_pages_from_lru_list(NODE_DATA(from_nid),
			memcg, nr_pages, &from_base_page_list, &from_huge_page_list,
			&nr_isolated_from_base_pages, &nr_isolated_from_huge_pages,
			from_action);

	pr_debug("%ld pages isolated at from node: %d\n", nr_isolated_from_pages, from_nid);

	if (max_nr_pages_to_node != ULONG_MAX &&
		max_nr_pages_to_node < (nr_pages_to_node + nr_isolated_from_pages)) {
		LIST_HEAD(to_base_page_list);
		LIST_HEAD(to_huge_page_list);

		nr_isolated_to_base_pages = 0;
		nr_isolated_to_huge_pages = 0;
		/* isolate pages on to node as well  */
		nr_isolated_to_pages = isolate_pages_from_lru_list(NODE_DATA(to_nid),
				memcg,
				nr_isolated_from_pages + nr_pages_to_node - max_nr_pages_to_node,
				&to_base_page_list, &to_huge_page_list,
				&nr_isolated_to_base_pages, &nr_isolated_to_huge_pages,
				move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_COLD_PAGES);
		pr_debug("%lu pages isolated at to node: %d\n", nr_isolated_to_pages, to_nid);
		if (migrate_mt || migrate_concur) {
			nr_isolated_to_base_pages -=
				migrate_to_node(&to_base_page_list, from_nid, mode & ~MIGRATE_MT);
			nr_isolated_to_huge_pages -=
				migrate_to_node(&to_huge_page_list, from_nid, mode);
		} else {
			nr_isolated_to_base_pages -=
				migrate_to_node(&to_base_page_list, from_nid, mode);
			nr_isolated_to_huge_pages -=
				migrate_to_node(&to_huge_page_list, from_nid, mode);
#if 0
			/* migrate base pages and THPs together if no opt is used */
			if (!list_empty(&to_huge_page_list)) {
				list_splice(&to_base_page_list, &to_huge_page_list);
				migrate_to_node(&to_huge_page_list, from_nid, mode);
			} else
				migrate_to_node(&to_base_page_list, from_nid, mode);
#endif
		}

		p->page_migration_stats.f2s.nr_migrations += 1;
		p->page_migration_stats.f2s.nr_base_pages += nr_isolated_to_base_pages;
		p->page_migration_stats.f2s.nr_huge_pages += nr_isolated_to_huge_pages;
	}

	putback_overflow_pages(nr_isolated_to_base_pages, nr_isolated_to_huge_pages,
			&from_base_page_list, &from_huge_page_list);

	if (migrate_mt || migrate_concur) {
		nr_isolated_from_base_pages -=
			migrate_to_node(&from_base_page_list, to_nid, mode & ~MIGRATE_MT);
		nr_isolated_from_huge_pages -=
			migrate_to_node(&from_huge_page_list, to_nid, mode);
	} else {
		nr_isolated_from_base_pages -=
			migrate_to_node(&from_base_page_list, to_nid, mode);
		nr_isolated_from_huge_pages -=
			migrate_to_node(&from_huge_page_list, to_nid, mode);
#if 0
		/* migrate base pages and THPs together if no opt is used */
		if (!list_empty(&from_huge_page_list)) {
			list_splice(&from_base_page_list, &from_huge_page_list);
			migrate_to_node(&from_huge_page_list, to_nid, mode);
		} else
			migrate_to_node(&from_base_page_list, to_nid, mode);
#endif
	}

	p->page_migration_stats.s2f.nr_migrations += 1;
	p->page_migration_stats.s2f.nr_base_pages += nr_isolated_from_base_pages;
	p->page_migration_stats.s2f.nr_huge_pages += nr_isolated_from_huge_pages;

	return err;
}

static int shrink_lists(struct task_struct *p, struct mm_struct *mm)
{
	struct mem_cgroup *memcg = mem_cgroup_from_task(p);
	int err = 0;

	return err;
}

SYSCALL_DEFINE6(mm_manage, pid_t, pid, unsigned long, nr_pages,
		unsigned long, maxnode,
		const unsigned long __user *, old_nodes,
		const unsigned long __user *, new_nodes,
		int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	int err;
	nodemask_t task_nodes;
	nodemask_t *old;
	nodemask_t *new;
	NODEMASK_SCRATCH(scratch);

	if (!scratch)
		return -ENOMEM;

	old = &scratch->mask1;
	new = &scratch->mask2;

	err = get_nodes(old, old_nodes, maxnode);
	if (err)
		goto out;

	err = get_nodes(new, new_nodes, maxnode);
	if (err)
		goto out;

	/* Check flags */
	if (flags & ~(
				  MPOL_MF_MOVE|
				  MPOL_MF_MOVE_MT|
				  MPOL_MF_MOVE_DMA|
				  MPOL_MF_MOVE_CONCUR|
				  MPOL_MF_EXCHANGE|
				  MPOL_MF_SHRINK_LISTS|
				  MPOL_MF_MOVE_ALL))
		return -EINVAL;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		err = -ESRCH;
		goto out;
	}
	get_task_struct(task);

	err = -EINVAL;
	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) && !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid,  tcred->suid) && !uid_eq(cred->uid,  tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out_put;
	}
	rcu_read_unlock();

	err = security_task_movememory(task);
	if (err)
		goto out_put;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm) {
		err = -EINVAL;
		goto out;
	}

	if (flags & MPOL_MF_SHRINK_LISTS)
		shrink_lists(task, mm);

	if (flags & MPOL_MF_MOVE)
		err = do_mm_manage(task, mm, old, new, nr_pages, flags);

	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	put_task_struct(task);
	goto out;

}