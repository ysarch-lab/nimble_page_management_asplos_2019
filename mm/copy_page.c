/*
 * Parallel page copy routine.
 * Use DMA engine to copy page data
 *
 * Zi Yan <zi.yan@cs.rutgers.edu>
 *
 */

#include <linux/sysctl.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>


unsigned int limit_mt_num = 4;

/* ======================== multi-threaded copy page ======================== */

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	struct work_struct copy_page_work;
	unsigned long num_items;
	struct copy_item item_list[0];
};

static void copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	memcpy(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *)work;
	int i;

	for (i = 0; i < my_work->num_items; ++i)
		copy_page_routine(my_work->item_list[i].to,
						  my_work->item_list[i].from,
						  my_work->item_list[i].chunk_size);
}

int copy_page_multithread(struct page *to, struct page *from, int nr_pages)
{
	unsigned int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items[32] = {0};
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int err = 0;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));
	if (total_mt_num > 1)
		total_mt_num = (total_mt_num / 2) * 2;

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info)
						+ sizeof(struct copy_item), GFP_KERNEL);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

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
		INIT_WORK((struct work_struct *)work_items[i],
				  copy_page_work_queue_thread);

		work_items[i]->num_items = 1;
		work_items[i]->item_list[0].to = vto + i * chunk_size;
		work_items[i]->item_list[0].from = vfrom + i * chunk_size;
		work_items[i]->item_list[0].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
					  system_highpri_wq,
					  (struct work_struct *)work_items[i]);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)work_items[i]);

	kunmap(to);
	kunmap(from);

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		if (work_items[cpu])
			kfree(work_items[cpu]);

	return err;
}

int copy_page_lists_mt(struct page **to, struct page **from, int nr_items)
{
	int err = 0;
	unsigned int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(*to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items[32] = {0};
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int max_items_per_thread;
	int item_idx;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));


	if (total_mt_num > 32)
		return -ENODEV;

	/* Each threads get part of each page, if nr_items < totla_mt_num */
	if (nr_items < total_mt_num)
		max_items_per_thread = nr_items;
	else
		max_items_per_thread = (nr_items / total_mt_num) +
				((nr_items % total_mt_num)?1:0);


	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info) +
					sizeof(struct copy_item)*max_items_per_thread, GFP_KERNEL);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);
			work_items[cpu]->num_items = max_items_per_thread;
		}

		for (item_idx = 0; item_idx < nr_items; ++item_idx) {
			unsigned long chunk_size = PAGE_SIZE * hpage_nr_pages(from[item_idx]) / total_mt_num;
			char *vfrom = kmap(from[item_idx]);
			char *vto = kmap(to[item_idx]);
			VM_BUG_ON(PAGE_SIZE * hpage_nr_pages(from[item_idx]) % total_mt_num);
			BUG_ON(hpage_nr_pages(to[item_idx]) !=
				   hpage_nr_pages(from[item_idx]));

			for (cpu = 0; cpu < total_mt_num; ++cpu) {
				work_items[cpu]->item_list[item_idx].to = vto + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].from = vfrom + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].chunk_size =
					chunk_size;
			}
		}

		for (cpu = 0; cpu < total_mt_num; ++cpu)
			queue_work_on(cpu_id_list[cpu],
						  system_highpri_wq,
						  (struct work_struct *)work_items[cpu]);
	} else {
		item_idx = 0;
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			int num_xfer_per_thread = nr_items / total_mt_num;
			int per_cpu_item_idx;

			if (cpu < (nr_items % total_mt_num))
				num_xfer_per_thread += 1;

			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);

			work_items[cpu]->num_items = num_xfer_per_thread;
			for (per_cpu_item_idx = 0; per_cpu_item_idx < work_items[cpu]->num_items;
				 ++per_cpu_item_idx, ++item_idx) {
				work_items[cpu]->item_list[per_cpu_item_idx].to = kmap(to[item_idx]);
				work_items[cpu]->item_list[per_cpu_item_idx].from =
					kmap(from[item_idx]);
				work_items[cpu]->item_list[per_cpu_item_idx].chunk_size =
					PAGE_SIZE * hpage_nr_pages(from[item_idx]);

				BUG_ON(hpage_nr_pages(to[item_idx]) !=
					   hpage_nr_pages(from[item_idx]));
			}

			queue_work_on(cpu_id_list[cpu],
						  system_highpri_wq,
						  (struct work_struct *)work_items[cpu]);
		}
		if (item_idx != nr_items)
			pr_err("%s: only %d out of %d pages are transferred\n", __func__,
				item_idx - 1, nr_items);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)work_items[i]);

	for (i = 0; i < nr_items; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		if (work_items[cpu])
			kfree(work_items[cpu]);

	return err;
}
/* ======================== DMA copy page ======================== */
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#define NUM_AVAIL_DMA_CHAN 16


int use_all_dma_chans = 0;
int limit_dma_chans = NUM_AVAIL_DMA_CHAN;


struct dma_chan *copy_chan[NUM_AVAIL_DMA_CHAN] = {0};
struct dma_device *copy_dev[NUM_AVAIL_DMA_CHAN] = {0};



#ifdef CONFIG_PROC_SYSCTL
int proc_dointvec_minmax(struct ctl_table *table, int write,
		    void __user *buffer, size_t *lenp, loff_t *ppos);
int sysctl_dma_page_migration(struct ctl_table *table, int write,
				 void __user *buffer, size_t *lenp,
				 loff_t *ppos)
{
	int err = 0;
	int use_all_dma_chans_prior_val = use_all_dma_chans;
	dma_cap_mask_t copy_mask;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	err = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (err < 0)
		return err;
	if (write) {
		/* Grab all DMA channels  */
		if (use_all_dma_chans_prior_val == 0 && use_all_dma_chans == 1) {
			int i;

			dma_cap_zero(copy_mask);
			dma_cap_set(DMA_MEMCPY, copy_mask);

			dmaengine_get();
			for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
				if (!copy_chan[i]) {
					copy_chan[i] = dma_request_channel(copy_mask, NULL, NULL);
				}
				if (!copy_chan[i]) {
					pr_err("%s: cannot grab channel: %d\n", __func__, i);
					continue;
				}

				copy_dev[i] = copy_chan[i]->device;

				if (!copy_dev[i]) {
					pr_err("%s: no device: %d\n", __func__, i);
					continue;
				}
			}

		}
		/* Release all DMA channels  */
		else if (use_all_dma_chans_prior_val == 1 && use_all_dma_chans == 0) {
			int i;

			for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
				if (copy_chan[i]) {
					dma_release_channel(copy_chan[i]);
					copy_chan[i] = NULL;
					copy_dev[i] = NULL;
				}
			}

			dmaengine_put();
		}

		if (err)
			use_all_dma_chans = use_all_dma_chans_prior_val;
	}
	return err;
}

#endif

static int copy_page_dma_once(struct page *to, struct page *from, int nr_pages)
{
	static struct dma_chan *copy_chan = NULL;
	struct dma_device *device = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags = 0;
	struct dmaengine_unmap_data *unmap = NULL;
	dma_cap_mask_t mask;
	int ret_val = 0;


	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	dmaengine_get();

	copy_chan = dma_request_channel(mask, NULL, NULL);

	if (!copy_chan) {
		pr_err("%s: cannot get a channel\n", __func__);
		ret_val = -1;
		goto no_chan;
	}

	device = copy_chan->device;

	if (!device) {
		pr_err("%s: cannot get a device\n", __func__);
		ret_val = -2;
		goto release;
	}

	unmap = dmaengine_get_unmap_data(device->dev, 2, GFP_NOWAIT);

	if (!unmap) {
		pr_err("%s: cannot get unmap data\n", __func__);
		ret_val = -3;
		goto release;
	}

	unmap->to_cnt = 1;
	unmap->addr[0] = dma_map_page(device->dev, from, 0, PAGE_SIZE*nr_pages,
					  DMA_TO_DEVICE);
	unmap->from_cnt = 1;
	unmap->addr[1] = dma_map_page(device->dev, to, 0, PAGE_SIZE*nr_pages,
					  DMA_FROM_DEVICE);
	unmap->len = PAGE_SIZE*nr_pages;

	tx = device->device_prep_dma_memcpy(copy_chan,
						unmap->addr[1],
						unmap->addr[0], unmap->len,
						flags);

	if (!tx) {
		pr_err("%s: null tx descriptor\n", __func__);
		ret_val = -4;
		goto unmap_dma;
	}

	cookie = tx->tx_submit(tx);

	if (dma_submit_error(cookie)) {
		pr_err("%s: submission error\n", __func__);
		ret_val = -5;
		goto unmap_dma;
	}

	if (dma_sync_wait(copy_chan, cookie) != DMA_COMPLETE) {
		pr_err("%s: dma does not complete properly\n", __func__);
		ret_val = -6;
	}

unmap_dma:
	dmaengine_unmap_put(unmap);
release:
	if (copy_chan) {
		dma_release_channel(copy_chan);
	}
no_chan:
	dmaengine_put();

	return ret_val;
}

static int copy_page_dma_always(struct page *to, struct page *from, int nr_pages)
{
	struct dma_async_tx_descriptor *tx[NUM_AVAIL_DMA_CHAN] = {0};
	dma_cookie_t cookie[NUM_AVAIL_DMA_CHAN];
	enum dma_ctrl_flags flags[NUM_AVAIL_DMA_CHAN] = {0};
	struct dmaengine_unmap_data *unmap[NUM_AVAIL_DMA_CHAN] = {0};
	int ret_val = 0;
	int total_available_chans = NUM_AVAIL_DMA_CHAN;
	int i;
	size_t page_offset;

	for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
		if (!copy_chan[i]) {
			total_available_chans = i;
		}
	}
	if (total_available_chans != NUM_AVAIL_DMA_CHAN) {
		pr_err("%d channels are missing", NUM_AVAIL_DMA_CHAN - total_available_chans);
	}

	total_available_chans = min_t(int, total_available_chans, limit_dma_chans);

	/* round down to closest 2^x value  */
	total_available_chans = 1<<ilog2(total_available_chans);

	if ((nr_pages != 1) && (nr_pages % total_available_chans != 0))
		return -5;

	for (i = 0; i < total_available_chans; ++i) {
		unmap[i] = dmaengine_get_unmap_data(copy_dev[i]->dev, 2, GFP_NOWAIT);
		if (!unmap[i]) {
			pr_err("%s: no unmap data at chan %d\n", __func__, i);
			ret_val = -3;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		if (nr_pages == 1) {
			page_offset = PAGE_SIZE / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev, from, page_offset*i,
							  page_offset,
							  DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev, to, page_offset*i,
							  page_offset,
							  DMA_FROM_DEVICE);
			unmap[i]->len = page_offset;
		} else {
			page_offset = nr_pages / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev,
								from + page_offset*i,
								0,
								PAGE_SIZE*page_offset,
								DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev,
								to + page_offset*i,
								0,
								PAGE_SIZE*page_offset,
								DMA_FROM_DEVICE);
			unmap[i]->len = PAGE_SIZE*page_offset;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		tx[i] = copy_dev[i]->device_prep_dma_memcpy(copy_chan[i],
							unmap[i]->addr[1],
							unmap[i]->addr[0],
							unmap[i]->len,
							flags[i]);
		if (!tx[i]) {
			pr_err("%s: no tx descriptor at chan %d\n", __func__, i);
			ret_val = -4;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		cookie[i] = tx[i]->tx_submit(tx[i]);

		if (dma_submit_error(cookie[i])) {
			pr_err("%s: submission error at chan %d\n", __func__, i);
			ret_val = -5;
			goto unmap_dma;
		}

		dma_async_issue_pending(copy_chan[i]);
	}

	for (i = 0; i < total_available_chans; ++i) {
		if (dma_sync_wait(copy_chan[i], cookie[i]) != DMA_COMPLETE) {
			ret_val = -6;
			pr_err("%s: dma does not complete at chan %d\n", __func__, i);
		}
	}

unmap_dma:

	for (i = 0; i < total_available_chans; ++i) {
		if (unmap[i])
			dmaengine_unmap_put(unmap[i]);
	}

	return ret_val;
}

int copy_page_dma(struct page *to, struct page *from, int nr_pages)
{
	BUG_ON(hpage_nr_pages(from) != nr_pages);
	BUG_ON(hpage_nr_pages(to) != nr_pages);

	if (!use_all_dma_chans) {
		return copy_page_dma_once(to, from, nr_pages);
	}

	return copy_page_dma_always(to, from, nr_pages);
}

/*
 * Use DMA copy a list of pages to a new location
 *
 * Just put each page into individual DMA channel.
 *
 * */
int copy_page_lists_dma_always(struct page **to, struct page **from, int nr_items)
{
	struct dma_async_tx_descriptor **tx = NULL;
	dma_cookie_t *cookie = NULL;
	enum dma_ctrl_flags flags[NUM_AVAIL_DMA_CHAN] = {0};
	struct dmaengine_unmap_data *unmap[NUM_AVAIL_DMA_CHAN] = {0};
	int ret_val = 0;
	int total_available_chans = NUM_AVAIL_DMA_CHAN;
	int i;
	int page_idx;

	for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
		if (!copy_chan[i]) {
			total_available_chans = i;
		}
	}
	if (total_available_chans != NUM_AVAIL_DMA_CHAN) {
		pr_err("%d channels are missing\n", NUM_AVAIL_DMA_CHAN - total_available_chans);
	}
	if (limit_dma_chans < total_available_chans)
		total_available_chans = limit_dma_chans;

	/* round down to closest 2^x value  */
	total_available_chans = 1<<ilog2(total_available_chans);

	total_available_chans = min_t(int, total_available_chans, nr_items);


	tx = kzalloc(sizeof(struct dma_async_tx_descriptor*)*nr_items, GFP_KERNEL);
	if (!tx) {
		ret_val = -ENOMEM;
		goto out;
	}
	cookie = kzalloc(sizeof(dma_cookie_t)*nr_items, GFP_KERNEL);
	if (!cookie) {
		ret_val = -ENOMEM;
		goto out_free_tx;
	}

	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_items / total_available_chans;

		if (i < (nr_items % total_available_chans))
			num_xfer_per_dev += 1;

		if (num_xfer_per_dev > 128) {
			ret_val = -ENOMEM;
			pr_err("%s: too many pages to be transferred\n", __func__);
			goto out_free_both;
		}

		unmap[i] = dmaengine_get_unmap_data(copy_dev[i]->dev,
						2 * num_xfer_per_dev, GFP_NOWAIT);
		if (!unmap[i]) {
			pr_err("%s: no unmap data at chan %d\n", __func__, i);
			ret_val = -ENODEV;
			goto unmap_dma;
		}
	}

	page_idx = 0;
	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_items / total_available_chans;
		int xfer_idx;

		if (i < (nr_items % total_available_chans))
			num_xfer_per_dev += 1;

		unmap[i]->to_cnt = num_xfer_per_dev;
		unmap[i]->from_cnt = num_xfer_per_dev;
		unmap[i]->len = hpage_nr_pages(from[i]) * PAGE_SIZE;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx, ++page_idx) {
			size_t page_len = hpage_nr_pages(from[page_idx]) * PAGE_SIZE;

			BUG_ON(page_len != hpage_nr_pages(to[page_idx]) * PAGE_SIZE);
			BUG_ON(unmap[i]->len != page_len);

			unmap[i]->addr[xfer_idx] =
				 dma_map_page(copy_dev[i]->dev, from[page_idx],
							  0,
							  page_len,
							  DMA_TO_DEVICE);

			unmap[i]->addr[xfer_idx+num_xfer_per_dev] =
				 dma_map_page(copy_dev[i]->dev, to[page_idx],
							  0,
							  page_len,
							  DMA_FROM_DEVICE);
		}
	}

	page_idx = 0;
	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_items / total_available_chans;
		int xfer_idx;

		if (i < (nr_items % total_available_chans))
			num_xfer_per_dev += 1;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx, ++page_idx) {

			tx[page_idx] = copy_dev[i]->device_prep_dma_memcpy(copy_chan[i],
								unmap[i]->addr[xfer_idx + num_xfer_per_dev],
								unmap[i]->addr[xfer_idx],
								unmap[i]->len,
								flags[i]);
			if (!tx[page_idx]) {
				pr_err("%s: no tx descriptor at chan %d xfer %d\n",
					   __func__, i, xfer_idx);
				ret_val = -ENODEV;
				goto unmap_dma;
			}

			cookie[page_idx] = tx[page_idx]->tx_submit(tx[page_idx]);

			if (dma_submit_error(cookie[page_idx])) {
				pr_err("%s: submission error at chan %d xfer %d\n",
					   __func__, i, xfer_idx);
				ret_val = -ENODEV;
				goto unmap_dma;
			}
		}

		dma_async_issue_pending(copy_chan[i]);
	}

	page_idx = 0;
	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_items / total_available_chans;
		int xfer_idx;

		if (i < (nr_items % total_available_chans))
			num_xfer_per_dev += 1;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx, ++page_idx) {

			if (dma_sync_wait(copy_chan[i], cookie[page_idx]) != DMA_COMPLETE) {
				ret_val = -6;
				pr_err("%s: dma does not complete at chan %d, xfer %d\n",
					   __func__, i, xfer_idx);
			}
		}
	}

unmap_dma:
	for (i = 0; i < total_available_chans; ++i) {
		if (unmap[i])
			dmaengine_unmap_put(unmap[i]);
	}

out_free_both:
	kfree(cookie);
out_free_tx:
	kfree(tx);
out:

	return ret_val;
}
