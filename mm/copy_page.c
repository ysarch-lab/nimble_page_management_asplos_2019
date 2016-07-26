/*
 * Parallel page copy routine.
 * Use DMA engine to copy page data
 *
 * Zi Yan <zi.yan@cs.rutgers.edu>
 *
 */

#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>


unsigned int limit_mt_num = 4;

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

int copy_page_lists_mt(struct page **to, struct page **from, int nr_pages)
{
	int err = 0;
	int total_mt_num = limit_mt_num;
	int to_node = page_to_nid(*to);
	int i;
	struct copy_page_info *work_items;
	int nr_pages_per_page = hpage_nr_pages(*from);
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;

	work_items = kzalloc(sizeof(struct copy_page_info)*nr_pages,
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	total_mt_num = min_t(int, nr_pages, total_mt_num);

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	for (i = 0; i < nr_pages; ++i) {
		int thread_idx = i % total_mt_num;

		INIT_WORK((struct work_struct *)&work_items[i],
				  copy_page_work_queue_thread);

		work_items[i].to = kmap(to[i]);
		work_items[i].from = kmap(from[i]);
		work_items[i].chunk_size = PAGE_SIZE * hpage_nr_pages(from[i]);

		BUG_ON(nr_pages_per_page != hpage_nr_pages(from[i]));
		BUG_ON(nr_pages_per_page != hpage_nr_pages(to[i]));


		queue_work_on(cpu_id_list[thread_idx],
					  system_highpri_wq,
					  (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	flush_workqueue(system_highpri_wq);

	for (i = 0; i < nr_pages; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

	kfree(work_items);

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
int copy_page_lists_dma_always(struct page **to, struct page **from, int nr_pages)
{
	struct dma_async_tx_descriptor **tx = NULL;
	dma_cookie_t *cookie = NULL;
	enum dma_ctrl_flags flags[NUM_AVAIL_DMA_CHAN] = {0};
	struct dmaengine_unmap_data *unmap[NUM_AVAIL_DMA_CHAN] = {0};
	int ret_val = 0;
	int total_available_chans = NUM_AVAIL_DMA_CHAN;
	int i;

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

	total_available_chans = min_t(int, total_available_chans, nr_pages);


	tx = kzalloc(sizeof(struct dma_async_tx_descriptor*)*nr_pages, GFP_KERNEL);
	if (!tx) {
		ret_val = -ENOMEM;
		goto out;
	}
	cookie = kzalloc(sizeof(dma_cookie_t)*nr_pages, GFP_KERNEL);
	if (!cookie) {
		ret_val = -ENOMEM;
		goto out_free_tx;
	}

	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_pages / total_available_chans;

		if (i < (nr_pages % total_available_chans))
			num_xfer_per_dev += 1;

		unmap[i] = dmaengine_get_unmap_data(copy_dev[i]->dev,
						2*num_xfer_per_dev, GFP_NOWAIT);
		if (!unmap[i]) {
			pr_err("%s: no unmap data at chan %d\n", __func__, i);
			ret_val = -ENODEV;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_pages / total_available_chans;
		int xfer_idx;

		if (i < (nr_pages % total_available_chans))
			num_xfer_per_dev += 1;

		unmap[i]->to_cnt = num_xfer_per_dev;
		unmap[i]->from_cnt = num_xfer_per_dev;
		unmap[i]->len = hpage_nr_pages(from[i]) * PAGE_SIZE;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx) {
			int page_idx = i + xfer_idx * total_available_chans;
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

	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_pages / total_available_chans;
		int xfer_idx;

		if (i < (nr_pages % total_available_chans))
			num_xfer_per_dev += 1;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx) {
			int page_idx = i + xfer_idx * total_available_chans;

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

	for (i = 0; i < total_available_chans; ++i) {
		int num_xfer_per_dev = nr_pages / total_available_chans;
		int xfer_idx;

		if (i < (nr_pages % total_available_chans))
			num_xfer_per_dev += 1;

		for (xfer_idx = 0; xfer_idx < num_xfer_per_dev; ++xfer_idx) {
			int page_idx = i + xfer_idx * total_available_chans;

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

	kfree(cookie);
out_free_tx:
	kfree(tx);
out:

	return ret_val;
}
