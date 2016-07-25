/*
 * Parallel page copy routine.
 *
 * Zi Yan <ziy@nvidia.com>
 *
 */

#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>


const unsigned int limit_mt_num = 4;

/* ======================== multi-threaded copy page ======================== */

struct copy_page_info {
	struct work_struct copy_page_work;
	char *to;
	char *from;
	unsigned long chunk_size;
};

static void copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	memcpy(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *)work;

	copy_page_routine(my_work->to,
					  my_work->from,
					  my_work->chunk_size);
}

int copy_page_multithread(struct page *to, struct page *from, int nr_pages)
{
	unsigned int total_mt_num = limit_mt_num;
	int to_node = page_to_nid(to);
	int i;
	struct copy_page_info *work_items;
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));
	total_mt_num = (total_mt_num / 2) * 2;

	work_items = kcalloc(total_mt_num, sizeof(struct copy_page_info),
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE*nr_pages / total_mt_num;

	for (i = 0; i < total_mt_num; ++i) {
		INIT_WORK((struct work_struct *)&work_items[i],
				  copy_page_work_queue_thread);

		work_items[i].to = vto + i * chunk_size;
		work_items[i].from = vfrom + i * chunk_size;
		work_items[i].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
					  system_highpri_wq,
					  (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)&work_items[i]);

	kunmap(to);
	kunmap(from);

	kfree(work_items);

	return 0;
}
