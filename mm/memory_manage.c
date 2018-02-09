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
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned,
		isolate_mode_t mode, enum lru_list lru)
{
	struct list_head *src = &lruvec->lists[lru];
	unsigned long nr_taken = 0;
	unsigned long nr_zone_taken[MAX_NR_ZONES] = { 0 };
	unsigned long scan, total_scan, nr_pages;

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
			list_move(&page->lru, dst);
			break;

		case -EBUSY:
			/* else it is being freed elsewhere */
			list_move(&page->lru, src);
			continue;

		default:
			BUG();
		}
	}

	*nr_scanned = total_scan;
	update_lru_sizes(lruvec, lru, nr_zone_taken);
	return nr_taken;
}

static unsigned long isolate_pages_from_lru_list(pg_data_t *pgdat,
		struct mem_cgroup *memcg, unsigned long nr_pages,
		struct list_head *page_list, enum isolate_action action)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	enum lru_list lru;
	unsigned long nr_all_taken = 0;

	if (nr_pages == ULONG_MAX)
		nr_pages = memcg_size_node(memcg, pgdat->node_id);

	lru_add_drain_all();

	for_each_evictable_lru(lru) {
		unsigned long nr_scanned, nr_taken;
		int file = is_file_lru(lru);

		if (action == ISOLATE_COLD_PAGES && is_active_lru(lru))
			continue;
		if (action == ISOLATE_HOT_PAGES && !is_active_lru(lru))
			continue;

		spin_lock_irq(&pgdat->lru_lock);

		nr_taken = isolate_lru_pages(nr_pages, lruvec, page_list,
						 &nr_scanned, 0, lru);

		__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

		spin_unlock_irq(&pgdat->lru_lock);

		nr_all_taken += nr_taken;

		if (nr_all_taken > nr_pages)
			break;
	}

	return nr_all_taken;
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
	unsigned long max_nr_pages_to_node, nr_pages_to_node;
	int from_nid, to_nid;
	LIST_HEAD(from_page_list);

	VM_BUG_ON(!memcg);
	/* Let's handle simplest situation first */
	VM_BUG_ON(!(nodes_weight(*from) == 1 && nodes_weight(*to) == 1));

	if (memcg == root_mem_cgroup)
		return 0;

	from_nid = first_node(*from);
	to_nid = first_node(*to);

	max_nr_pages_to_node = memcg_max_size_node(memcg, to_nid);
	nr_pages_to_node = memcg_size_node(memcg, to_nid);


	nr_isolated_from_pages = isolate_pages_from_lru_list(NODE_DATA(from_nid),
			memcg, nr_pages, &from_page_list,
			move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_HOT_PAGES);

	if (max_nr_pages_to_node != ULONG_MAX &&
		max_nr_pages_to_node < (nr_pages_to_node + nr_isolated_from_pages)) {
		unsigned long nr_isolated_to_pages;
		LIST_HEAD(to_page_list);
		/* isolate pages on to node as well  */
		nr_isolated_to_pages = isolate_pages_from_lru_list(NODE_DATA(to_nid),
				memcg,
				nr_isolated_from_pages + nr_pages_to_node - max_nr_pages_to_node,
				&to_page_list,
				move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_COLD_PAGES);

		if (!list_empty(&to_page_list)) {
			int err;
			if (migrate_concur)
				err = migrate_pages_concur(&to_page_list, alloc_new_node_page,
					NULL, from_nid,
					MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
					(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
					MR_SYSCALL);
			else
				err = migrate_pages(&to_page_list, alloc_new_node_page,
					NULL, from_nid,
					MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
					(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
					MR_SYSCALL);
		}
	}


	if (!list_empty(&from_page_list)) {
		int err;
		if (migrate_concur)
			err = migrate_pages_concur(&from_page_list, alloc_new_node_page,
				NULL, to_nid,
				MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
				(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
				MR_SYSCALL);
		else
			err = migrate_pages(&from_page_list, alloc_new_node_page,
				NULL, to_nid,
				MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
				(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
				MR_SYSCALL);
	}


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
	if (flags & ~(MPOL_MF_MOVE_MT|
				  MPOL_MF_MOVE_DMA|
				  MPOL_MF_MOVE_CONCUR|
				  MPOL_MF_EXCHANGE|
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

	err = do_mm_manage(task, mm, old, new, nr_pages, flags);

	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	put_task_struct(task);
	goto out;

}