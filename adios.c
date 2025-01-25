// SPDX-License-Identifier: GPL-2.0
/*
 * The Adaptive Deadline I/O Scheduler (ADIOS)
 * Based on mq-deadline and Kyber,
 * with learning-based adaptive latency control
 *
 * Copyright (C) 2025 Masahito Suzuki
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/timekeeping.h>

#include "include/elevator.h"
#include "include/blk.h"
#include "include/blk-mq.h"
#include "include/blk-mq-sched.h"

#define ADIOS_VERSION "0.12.0"

// Global variable to control the latency
static u64 global_latency_window = 16000000ULL;
// Ratio below which batch queues should be refilled
static int bq_refill_below_ratio = 15;

// Define operation types supported by ADIOS
enum adios_op_type {
	ADIOS_READ,
	ADIOS_WRITE,
	ADIOS_DISCARD,
	ADIOS_OTHER,
	ADIOS_OPTYPES,
};

// Latency targets for each operation type
static u64 default_latency_target[ADIOS_OPTYPES] = {
	[ADIOS_READ]    =    2ULL * NSEC_PER_MSEC,
	[ADIOS_WRITE]   =  750ULL * NSEC_PER_MSEC,
	[ADIOS_DISCARD] = 5000ULL * NSEC_PER_MSEC,
	[ADIOS_OTHER]   =    0ULL,
};

// Maximum batch size limits for each operation type
static u32 default_batch_limit[ADIOS_OPTYPES] = {
	[ADIOS_READ]    = 16,
	[ADIOS_WRITE]   =  8,
	[ADIOS_DISCARD] =  1,
	[ADIOS_OTHER]   =  1,
};

// Thresholds for latency model control
#define LM_BLOCK_SIZE_THRESHOLD 4096
#define LM_SAMPLES_THRESHOLD    1024
#define LM_INTERVAL_THRESHOLD   1500
#define LM_OUTLIER_PERCENTILE     99
#define LM_LAT_BUCKET_COUNT       64
#define LM_SHRINK_AT_MREQ         10
#define LM_SHRINK_AT_GBYTES      100
#define LM_SHRINK_RESIST           2

// Structure to hold latency bucket data
struct latency_bucket {
	u64 count;
	u64 sum_latency;
	u64 sum_block_size;
};

// Structure to hold the latency model context data
struct latency_model {
	u64 base;
	u64 slope;
	u64 small_sum_delay;
	u64 small_count;
	u64 large_sum_delay;
	u64 large_sum_bsize;
	u64 last_update_jiffies;

	spinlock_t lock;
	struct latency_bucket small_bucket[LM_LAT_BUCKET_COUNT];
	struct latency_bucket large_bucket[LM_LAT_BUCKET_COUNT];
	spinlock_t buckets_lock;
};

#define ADIOS_BQ_PAGES 2

// Adios scheduler data
struct adios_data {
	struct list_head prio_queue;
	spinlock_t pq_lock;

	struct rb_root_cached dl_tree;
	spinlock_t lock;

	int bq_page;
	bool more_bq_ready;
	struct list_head batch_queue[ADIOS_BQ_PAGES][ADIOS_OPTYPES];
	unsigned int batch_count[ADIOS_BQ_PAGES][ADIOS_OPTYPES];
	spinlock_t bq_lock;

	atomic64_t total_pred_lat;

	struct latency_model latency_model[ADIOS_OPTYPES];
	struct timer_list update_timer;

	uint32_t batch_actual_max_size[ADIOS_OPTYPES];
	uint32_t batch_actual_max_total;
	u32 async_depth;

	struct kmem_cache *rq_data_pool;
	struct kmem_cache *dl_group_pool;

	u64 latency_target[ADIOS_OPTYPES];
	u32 batch_limit[ADIOS_OPTYPES];
};

// List of requests with the same deadline in the deadline-sorted red-black tree
struct dl_group {
	struct rb_node node;
	u64 deadline;
	struct list_head rqs;
};

// Structure to hold scheduler-specific data for each request
struct adios_rq_data {
	struct request *rq;
	u64 deadline;
	u64 pred_lat;
	u64 block_size;

	struct list_head *dl_group;
	struct list_head dl_node;
};

// Count the number of entries in small buckets
static u32 lm_count_small_entries(struct latency_model *model) {
	u32 total_count = 0;
	for (int i = 0; i < LM_LAT_BUCKET_COUNT; i++)
		total_count += model->small_bucket[i].count;
	return total_count;
}

// Update the small buckets in the latency model
static bool lm_update_small_buckets(struct latency_model *model,
		unsigned long flags,u32 total_count, bool count_all) {
	u32 threshold_count = 0;
	u32 cumulative_count = 0;
	u32 outlier_threshold_bucket = 0;
	u64 sum_latency = 0, sum_count = 0;
	u32 outlier_percentile = LM_OUTLIER_PERCENTILE;
	u64 reduction;

	if (count_all)
		outlier_percentile = 100;

	// Calculate the threshold count for outlier detection
	threshold_count = (total_count * outlier_percentile) / 100;

	// Identify the bucket that corresponds to the outlier threshold
	for (int i = 0; i < LM_LAT_BUCKET_COUNT; i++) {
		cumulative_count += model->small_bucket[i].count;
		if (cumulative_count >= threshold_count) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	// Calculate the average latency, excluding outliers
	for (int i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket *bucket = &model->small_bucket[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->sum_latency;
			sum_count += bucket->count;
		} else {
			// For the threshold bucket, calculate the contribution proportionally
			u64 remaining_count =
				threshold_count - (cumulative_count - bucket->count);
			if (bucket->count > 0) {
				sum_latency +=
					(bucket->sum_latency * remaining_count) / bucket->count;
				sum_count += remaining_count;
			}
		}
	}

	// Shrink the model if it reaches at the readjustment threshold
	if (model->small_count >= 1000000ULL * LM_SHRINK_AT_MREQ) {
		reduction = LM_SHRINK_RESIST;
		if (model->small_count >> reduction) {
			model->small_sum_delay -= model->small_sum_delay >> reduction;
			model->small_count     -= model->small_count     >> reduction;
		}
	}

	// Accumulate the average latency into the statistics
	model->small_sum_delay += sum_latency;
	model->small_count += sum_count;

	// Reset small bucket information
	memset(model->small_bucket, 0,
		sizeof(model->small_bucket[0]) * LM_LAT_BUCKET_COUNT);

	return true;
}

// Count the number of entries in large buckets
static u32 lm_count_large_entries(struct latency_model *model) {
	u32 total_count = 0;
	for (int i = 0; i < LM_LAT_BUCKET_COUNT; i++)
		total_count += model->large_bucket[i].count;
	return total_count;
}

// Update the large buckets in the latency model
static bool lm_update_large_buckets(
		struct latency_model *model, unsigned long flags,
		u32 total_count, bool count_all) {
	unsigned int threshold_count = 0;
	unsigned int cumulative_count = 0;
	unsigned int outlier_threshold_bucket = 0;
	s64 sum_latency = 0;
	u64 sum_block_size = 0, intercept;
	u32 outlier_percentile = LM_OUTLIER_PERCENTILE;
	u64 reduction;

	if (count_all)
		outlier_percentile = 100;

	// Calculate the threshold count for outlier detection
	threshold_count = (total_count * outlier_percentile) / 100;

	// Identify the bucket that corresponds to the outlier threshold
	for (int i = 0; i < LM_LAT_BUCKET_COUNT; i++) {
		cumulative_count += model->large_bucket[i].count;
		if (cumulative_count >= threshold_count) {
			outlier_threshold_bucket = i;
			break;
		}
	}

	// Calculate the average latency and block size, excluding outliers
	for (int i = 0; i <= outlier_threshold_bucket; i++) {
		struct latency_bucket *bucket = &model->large_bucket[i];
		if (i < outlier_threshold_bucket) {
			sum_latency += bucket->sum_latency;
			sum_block_size += bucket->sum_block_size;
		} else {
			// For the threshold bucket, calculate the contribution proportionally
			u64 remaining_count =
				threshold_count - (cumulative_count - bucket->count);
			if (bucket->count > 0) {
				sum_latency +=
					(bucket->sum_latency * remaining_count) / bucket->count;
				sum_block_size +=
					(bucket->sum_block_size * remaining_count) / bucket->count;
			}
		}
	}

	// Shrink the model if it reaches at the readjustment threshold
	if (model->large_sum_bsize >= 0x40000000ULL * LM_SHRINK_AT_GBYTES) {
		reduction = LM_SHRINK_RESIST;
		if (model->large_sum_bsize >> reduction) {
			model->large_sum_delay -= model->large_sum_delay >> reduction;
			model->large_sum_bsize -= model->large_sum_bsize >> reduction;
		}
	}

	// Accumulate the average delay into the statistics
	intercept = model->base * threshold_count;
	if (sum_latency > intercept)
		sum_latency -= intercept;

	model->large_sum_delay += sum_latency;
	model->large_sum_bsize += sum_block_size;

	// Reset large bucket information
	memset(model->large_bucket, 0,
		sizeof(model->large_bucket[0]) * LM_LAT_BUCKET_COUNT);

	return true;
}

// Update the latency model parameters and statistics
static void latency_model_update(struct latency_model *model) {
	unsigned long flags;
	u64 now;
	u32 small_count, large_count;
	bool time_elapsed;
	bool small_processed = false, large_processed = false;

	guard(spinlock_irqsave)(&model->lock);

	spin_lock_irqsave(&model->buckets_lock, flags);

	// Whether enough time has elapsed since the last update
	now = jiffies;
	time_elapsed = unlikely(!model->base) || model->last_update_jiffies +
		msecs_to_jiffies(LM_INTERVAL_THRESHOLD) <= now;

	// Count the number of entries in buckets
	small_count = lm_count_small_entries(model);
	large_count = lm_count_large_entries(model);

	// Update small buckets
	if (small_count && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= small_count || !model->base))
		small_processed = lm_update_small_buckets(
			model, flags, small_count, !model->base);
	// Update large buckets
	if (large_count && (time_elapsed ||
			LM_SAMPLES_THRESHOLD <= large_count || !model->slope))
		large_processed = lm_update_large_buckets(
			model, flags, large_count, !model->slope);

	spin_unlock_irqrestore(&model->buckets_lock, flags);

	// Update the base parameter if small bucket was processed
	if (small_processed && likely(model->small_count))
		model->base = div_u64(model->small_sum_delay, model->small_count);

	// Update the slope parameter if large bucket was processed
	if (large_processed && likely(model->large_sum_bsize))
		model->slope = div_u64(model->large_sum_delay,
			DIV_ROUND_UP_ULL(model->large_sum_bsize, 1024));

	// Reset statistics and update last updated jiffies if time has elapsed
	if (time_elapsed)
		model->last_update_jiffies = now;
}

// Determine the bucket index for a given measured latency and predicted latency
static unsigned int lm_input_bucket_index(
		struct latency_model *model, u64 measured, u64 predicted) {
	unsigned int bucket_index;

	if (measured < predicted * 2)
		bucket_index = (measured * 20) / predicted;
	else if (measured < predicted * 5)
		bucket_index = (measured * 10) / predicted + 20;
	else
		bucket_index = (measured * 3) / predicted + 40;

	return bucket_index;
}

// Input latency data into the latency model
static void latency_model_input(struct latency_model *model,
		u64 block_size, u64 latency, u64 pred_lat) {
	unsigned long flags;
	unsigned int bucket_index;

	spin_lock_irqsave(&model->buckets_lock, flags);

	if (block_size <= LM_BLOCK_SIZE_THRESHOLD) {
		// Handle small requests

		bucket_index =
			lm_input_bucket_index(model, latency, (model->base ?: 1));

		if (bucket_index >= LM_LAT_BUCKET_COUNT)
			bucket_index = LM_LAT_BUCKET_COUNT - 1;

		model->small_bucket[bucket_index].count++;
		model->small_bucket[bucket_index].sum_latency += latency;

		if (unlikely(!model->base)) {
			spin_unlock_irqrestore(&model->buckets_lock, flags);
			latency_model_update(model);
			return;
		}
	} else {
		// Handle large requests
		if (!model->base || !pred_lat) {
			spin_unlock_irqrestore(&model->buckets_lock, flags);
			return;
		}

		bucket_index =
			lm_input_bucket_index(model, latency, pred_lat);

		if (bucket_index >= LM_LAT_BUCKET_COUNT)
			bucket_index = LM_LAT_BUCKET_COUNT - 1;

		model->large_bucket[bucket_index].count++;
		model->large_bucket[bucket_index].sum_latency += latency;
		model->large_bucket[bucket_index].sum_block_size += block_size;
	}

	spin_unlock_irqrestore(&model->buckets_lock, flags);
}

// Predict the latency for a given block size using the latency model
static u64 latency_model_predict(struct latency_model *model, u64 block_size) {
	u64 result;

	guard(spinlock_irqsave)(&model->lock);
	// Predict latency based on the model
	result = model->base;
	if (block_size > LM_BLOCK_SIZE_THRESHOLD)
		result += model->slope *
			DIV_ROUND_UP_ULL(block_size - LM_BLOCK_SIZE_THRESHOLD, 1024);

	return result;
}

// Determine the type of operation based on request flags
static unsigned int adios_optype(struct request *rq) {
	blk_opf_t opf = rq->cmd_flags;
	switch (opf & REQ_OP_MASK) {
	case REQ_OP_READ:
		return ADIOS_READ;
	case REQ_OP_WRITE:
		return ADIOS_WRITE;
	case REQ_OP_DISCARD:
		return ADIOS_DISCARD;
	default:
		return ADIOS_OTHER;
	}
}

// Helper function to retrieve adios_rq_data from a request
static inline struct adios_rq_data *get_rq_data(struct request *rq) {
	return (struct adios_rq_data *)rq->elv.priv[0];
}

// Add a request to the deadline-sorted red-black tree
static void add_to_dl_tree(struct adios_data *ad, struct request *rq) {
	struct rb_root_cached *root = &ad->dl_tree;
	struct rb_node **link = &(root->rb_root.rb_node), *parent = NULL;
	bool leftmost = true;
	struct adios_rq_data *rd = get_rq_data(rq);
	struct dl_group *dl_group;

	rd->block_size = blk_rq_bytes(rq);
	unsigned int optype = adios_optype(rq);
	rd->pred_lat =
		latency_model_predict(&ad->latency_model[optype], rd->block_size);
	rd->deadline =
		rq->start_time_ns + ad->latency_target[optype] + rd->pred_lat;

	while (*link) {
		dl_group = rb_entry(*link, struct dl_group, node);
		s64 diff = rd->deadline - dl_group->deadline;

		parent = *link;
		if (diff < 0) {
			link = &((*link)->rb_left);
		} else if (diff > 0) {
			link = &((*link)->rb_right);
			leftmost = false;
		} else { // diff == 0
			goto found;
		}
	}

	dl_group = rb_entry_safe(parent, struct dl_group, node);
	if (!dl_group || dl_group->deadline != rd->deadline) {
		dl_group = kmem_cache_zalloc(ad->dl_group_pool, GFP_ATOMIC);
		if (!dl_group)
			return;
		dl_group->deadline = rd->deadline;
		INIT_LIST_HEAD(&dl_group->rqs);
		rb_link_node(&dl_group->node, parent, link);
		rb_insert_color_cached(&dl_group->node, root, leftmost);
	}
found:
	list_add_tail(&rd->dl_node, &dl_group->rqs);
	rd->dl_group = &dl_group->rqs;
}

// Remove a request from the deadline-sorted red-black tree
static void del_from_dl_tree(struct adios_data *ad, struct request *rq) {
	struct rb_root_cached *root = &ad->dl_tree;
	struct adios_rq_data *rd = get_rq_data(rq);
	struct dl_group *dl_group = container_of(rd->dl_group, struct dl_group, rqs);

	list_del_init(&rd->dl_node);
	if (list_empty(&dl_group->rqs)) {
		rb_erase_cached(&dl_group->node, root);
		kmem_cache_free(ad->dl_group_pool, dl_group);
	}
	rd->dl_group = NULL;
}

// Remove a request from the scheduler
static void remove_request(struct adios_data *ad, struct request *rq) {
	struct request_queue *q = rq->q;
	struct adios_rq_data *rd = get_rq_data(rq);

	list_del_init(&rq->queuelist);

	// We might not be on the rbtree, if we are doing an insert merge
	if (rd->dl_group)
		del_from_dl_tree(ad, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

// Convert a queue depth to the corresponding word depth for shallow allocation
static int to_word_depth(struct blk_mq_hw_ctx *hctx, unsigned int qdepth) {
	struct sbitmap_queue *bt = &hctx->sched_tags->bitmap_tags;
	const unsigned int nrr = hctx->queue->nr_requests;

	return ((qdepth << bt->sb.shift) + nrr - 1) / nrr;
}

// Limit the depth of request allocation for asynchronous and write requests
static void adios_limit_depth(blk_opf_t opf, struct blk_mq_alloc_data *data) {
	struct adios_data *ad = data->q->elevator->elevator_data;

	// Do not throttle synchronous reads
	if (op_is_sync(opf) && !op_is_write(opf))
		return;

	data->shallow_depth = to_word_depth(data->hctx, ad->async_depth);
}

// Update the async_depth parameter when the number of requests in the queue changes
static void adios_depth_updated(struct blk_mq_hw_ctx *hctx) {
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	struct blk_mq_tags *tags = hctx->sched_tags;

	ad->async_depth = q->nr_requests;

	sbitmap_queue_min_shallow_depth(&tags->bitmap_tags, 1);
}

// Handle request merging after a merge operation
static void adios_request_merged(struct request_queue *q, struct request *req,
				  enum elv_merge type) {
	struct adios_data *ad = q->elevator->elevator_data;

	// if the merge was a front merge, we need to reposition request
	if (type == ELEVATOR_FRONT_MERGE) {
		del_from_dl_tree(ad, req);
		add_to_dl_tree(ad, req);
	}
}

// Handle merging of requests after one has been merged into another
static void adios_merged_requests(struct request_queue *q, struct request *req,
				   struct request *next) {
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	// kill knowledge of next, this one is a goner
	remove_request(ad, next);
}

// Attempt to merge a bio into an existing request before associating it with a request
static bool adios_bio_merge(struct request_queue *q, struct bio *bio,
		unsigned int nr_segs) {
	unsigned long flags;
	struct adios_data *ad = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock_irqsave(&ad->lock, flags);
	ret = blk_mq_sched_try_merge(q, bio, nr_segs, &free);
	spin_unlock_irqrestore(&ad->lock, flags);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

// Insert a request into the scheduler
static void insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
				  blk_insert_t insert_flags, struct list_head *free) {
	unsigned long flags;
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;

	lockdep_assert_held(&ad->lock);

	if (insert_flags & BLK_MQ_INSERT_AT_HEAD) {
		spin_lock_irqsave(&ad->pq_lock, flags);
		list_add(&rq->queuelist, &ad->prio_queue);
		spin_unlock_irqrestore(&ad->pq_lock, flags);
		return;
	}

	if (blk_mq_sched_try_insert_merge(q, rq, free))
		return;

	add_to_dl_tree(ad, rq);

	if (rq_mergeable(rq)) {
		elv_rqhash_add(q, rq);
		if (!q->last_merge)
			q->last_merge = rq;
	}
}

// Insert multiple requests into the scheduler
static void adios_insert_requests(struct blk_mq_hw_ctx *hctx,
				   struct list_head *list,
				   blk_insert_t insert_flags) {
	unsigned long flags;
	struct request_queue *q = hctx->queue;
	struct adios_data *ad = q->elevator->elevator_data;
	LIST_HEAD(free);

	spin_lock_irqsave(&ad->lock, flags);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
		insert_request(hctx, rq, insert_flags, &free);
	}
	spin_unlock_irqrestore(&ad->lock, flags);

	blk_mq_free_requests(&free);
}

// Prepare a request before it is inserted into the scheduler
static void adios_prepare_request(struct request *rq) {
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd;

	rq->elv.priv[0] = NULL;

	/* Allocate adios_rq_data from the memory pool */
	rd = kmem_cache_zalloc(ad->rq_data_pool, GFP_ATOMIC);
	if (WARN(!rd, "adios_prepare_request: Failed to allocate memory from rq_data_pool. rd is NULL\n"))
		return;

	rd->rq = rq;
	rq->elv.priv[0] = rd;
}

// Select the next request to dispatch from the deadline-sorted red-black tree
static struct request *get_ealiest_request(struct adios_data *ad) {
	struct rb_root_cached *root = &ad->dl_tree;
	struct rb_node *first = rb_first_cached(root);

	if (!first)
		return NULL;

	struct dl_group *dl_group = rb_entry(first, struct dl_group, node);
	struct adios_rq_data *rd =
		list_first_entry(&dl_group->rqs, struct adios_rq_data, dl_node);

	return rd->rq;
}

// Reset the batch queue counts for a given page
static void reset_batch_counts(struct adios_data *ad, int page) {
	memset(&ad->batch_count[page], 0, sizeof(ad->batch_count[page]));
}

// Initialize all batch queues
static void init_batch_queues(struct adios_data *ad) {
	for (int page = 0; page < ADIOS_BQ_PAGES; page++) {
		reset_batch_counts(ad, page);

		for (int optype = 0; optype < ADIOS_OPTYPES; optype++)
			INIT_LIST_HEAD(&ad->batch_queue[page][optype]);
	}
}

// Fill the batch queues with requests from the deadline-sorted red-black tree
static bool fill_batch_queues(struct adios_data *ad, u64 current_lat) {
	unsigned long flags;
	unsigned int count = 0;
	unsigned int optype_count[ADIOS_OPTYPES];
	memset(optype_count, 0, sizeof(optype_count));
	int page = (ad->bq_page + 1) % ADIOS_BQ_PAGES;

	reset_batch_counts(ad, page);

	spin_lock_irqsave(&ad->lock, flags);
	while (true) {
		struct request *rq = get_ealiest_request(ad);
		if (!rq)
			break;

		struct adios_rq_data *rd = get_rq_data(rq);
		unsigned int optype = adios_optype(rq);
		current_lat += rd->pred_lat;

		// Check batch size and total predicted latency
		if (count && (!ad->latency_model[optype].base || 
			ad->batch_count[page][optype] >= ad->batch_limit[optype] ||
			current_lat > global_latency_window)) {
			break;
		}

		remove_request(ad, rq);

		// Add request to the corresponding batch queue
		list_add_tail(&rq->queuelist, &ad->batch_queue[page][optype]);
		ad->batch_count[page][optype]++;
		atomic64_add(rd->pred_lat, &ad->total_pred_lat);
		optype_count[optype]++;
		count++;
	}
	spin_unlock_irqrestore(&ad->lock, flags);

	if (count) {
		ad->more_bq_ready = true;
		for (int optype = 0; optype < ADIOS_OPTYPES; optype++) {
			if (ad->batch_actual_max_size[optype] < optype_count[optype])
				ad->batch_actual_max_size[optype] = optype_count[optype];
		}
		if (ad->batch_actual_max_total < count)
			ad->batch_actual_max_total = count;
	}
	return count;
}

// Flip to the next batch queue page
static void flip_bq_page(struct adios_data *ad) {
	ad->more_bq_ready = false;
	ad->bq_page = (ad->bq_page + 1) % ADIOS_BQ_PAGES;
}

// Dispatch a request from the batch queues
static struct request *dispatch_from_bq(struct adios_data *ad) {
	struct request *rq = NULL;
	u64 tpl;

	guard(spinlock_irqsave)(&ad->bq_lock);

	tpl = atomic64_read(&ad->total_pred_lat);

	if (!ad->more_bq_ready && (!tpl ||
			tpl < global_latency_window * bq_refill_below_ratio / 100))
		fill_batch_queues(ad, tpl);

again:
	// Check if there are any requests in the batch queues
	for (int i = 0; i < ADIOS_OPTYPES; i++) {
		if (!list_empty(&ad->batch_queue[ad->bq_page][i])) {
			rq = list_first_entry(&ad->batch_queue[ad->bq_page][i],
									struct request, queuelist);
			list_del_init(&rq->queuelist);
			return rq;
		}
	}

	// If there's more batch queue page available, flip to it and retry
	if (ad->more_bq_ready) {
		flip_bq_page(ad);
		goto again;
	}

	return NULL;
}

// Dispatch a request from the priority queue
static struct request *dispatch_from_pq(struct adios_data *ad) {
	struct request *rq = NULL;

	guard(spinlock_irqsave)(&ad->pq_lock);

	if (!list_empty(&ad->prio_queue)) {
		rq = list_first_entry(&ad->prio_queue, struct request, queuelist);
		list_del_init(&rq->queuelist);
	}
	return rq;
}

// Dispatch a request to the hardware queue
static struct request *adios_dispatch_request(struct blk_mq_hw_ctx *hctx) {
	struct adios_data *ad = hctx->queue->elevator->elevator_data;
	struct request *rq;

	rq = dispatch_from_pq(ad);
	if (rq) goto found;
	rq = dispatch_from_bq(ad);
	if (!rq) return NULL;
found:
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

// Timer callback function to periodically update latency models
static void update_timer_callback(struct timer_list *t) {
	struct adios_data *ad = from_timer(ad, t, update_timer);
	unsigned int optype;

	for (optype = 0; optype < ADIOS_OPTYPES; optype++)
		latency_model_update(&ad->latency_model[optype]);
}

// Handle the completion of a request
static void adios_completed_request(struct request *rq, u64 now) {
	struct adios_data *ad = rq->q->elevator->elevator_data;
	struct adios_rq_data *rd = get_rq_data(rq);

	atomic64_sub(rd->pred_lat, &ad->total_pred_lat);

	if (!rq->io_start_time_ns || !rd->block_size)
		return;
	u64 latency = now - rq->io_start_time_ns;
	unsigned int optype = adios_optype(rq);
	latency_model_input(&ad->latency_model[optype],
		rd->block_size, latency, rd->pred_lat);
	timer_reduce(&ad->update_timer, jiffies + msecs_to_jiffies(100));
}

// Clean up after a request is finished
static void adios_finish_request(struct request *rq) {
	struct adios_data *ad = rq->q->elevator->elevator_data;

	if (rq->elv.priv[0]) {
		// Free adios_rq_data back to the memory pool
		kmem_cache_free(ad->rq_data_pool, get_rq_data(rq));
		rq->elv.priv[0] = NULL;
	}
}

static inline bool pq_has_work(struct adios_data *ad) {
	guard(spinlock_irqsave)(&ad->pq_lock);
	return !list_empty(&ad->prio_queue);
}

static inline bool bq_has_work(struct adios_data *ad) {
	guard(spinlock_irqsave)(&ad->bq_lock);

	for (int i = 0; i < ADIOS_OPTYPES; i++)
		if (!list_empty(&ad->batch_queue[ad->bq_page][i]))
			return true;

	return ad->more_bq_ready;
}

static inline bool dl_tree_has_work(struct adios_data *ad) {
	guard(spinlock_irqsave)(&ad->lock);
	return !RB_EMPTY_ROOT(&ad->dl_tree.rb_root);
}

// Check if there are any requests available for dispatch
static bool adios_has_work(struct blk_mq_hw_ctx *hctx) {
	struct adios_data *ad = hctx->queue->elevator->elevator_data;

	return pq_has_work(ad) || bq_has_work(ad) || dl_tree_has_work(ad);
}

// Initialize the scheduler-specific data for a hardware queue
static int adios_init_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx) {
	adios_depth_updated(hctx);
	return 0;
}

// Initialize the scheduler-specific data when initializing the request queue
static int adios_init_sched(struct request_queue *q, struct elevator_type *e) {
	struct adios_data *ad;
	struct elevator_queue *eq;
	int ret = -ENOMEM;

	eq = elevator_alloc(q, e);
	if (!eq)
		return ret;

	ad = kzalloc_node(sizeof(*ad), GFP_KERNEL, q->node);
	if (!ad)
		goto put_eq;

	// Create a memory pool for adios_rq_data
	ad->rq_data_pool = kmem_cache_create("rq_data_pool",
						sizeof(struct adios_rq_data),
						0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ad->rq_data_pool) {
		pr_err("adios: Failed to create rq_data_pool\n");
		goto free_ad;
	}

	/* Create a memory pool for dl_group */
	ad->dl_group_pool = kmem_cache_create("dl_group_pool",
						sizeof(struct dl_group),
						0, SLAB_HWCACHE_ALIGN, NULL);
	if (!ad->dl_group_pool) {
		pr_err("adios: Failed to create dl_group_pool\n");
		goto destroy_rq_data_pool;
	}

	eq->elevator_data = ad;

	INIT_LIST_HEAD(&ad->prio_queue);
	ad->dl_tree = RB_ROOT_CACHED;

	for (int i = 0; i < ADIOS_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		spin_lock_init(&model->lock);
		spin_lock_init(&model->buckets_lock);
		memset(model->small_bucket, 0,
			sizeof(model->small_bucket[0]) * LM_LAT_BUCKET_COUNT);
		memset(model->large_bucket, 0,
			sizeof(model->large_bucket[0]) * LM_LAT_BUCKET_COUNT);
		model->last_update_jiffies = jiffies;

		// Initialize latency_target and batch_limit per adios_data
		ad->latency_target[i] = default_latency_target[i];
		ad->batch_limit[i] = default_batch_limit[i];
	}
	timer_setup(&ad->update_timer, update_timer_callback, 0);
	init_batch_queues(ad);

	spin_lock_init(&ad->lock);
	spin_lock_init(&ad->pq_lock);
	spin_lock_init(&ad->bq_lock);

	/* We dispatch from request queue wide instead of hw queue */
	blk_queue_flag_set(QUEUE_FLAG_SQ_SCHED, q);

	q->elevator = eq;
	return 0;

destroy_rq_data_pool:
	kmem_cache_destroy(ad->rq_data_pool);
free_ad:
	kfree(ad);
put_eq:
	kobject_put(&eq->kobj);
	return ret;
}

// Clean up and free resources when exiting the scheduler
static void adios_exit_sched(struct elevator_queue *e) {
	struct adios_data *ad = e->elevator_data;

	timer_shutdown_sync(&ad->update_timer);

	WARN_ON_ONCE(!list_empty(&ad->prio_queue));

	if (ad->rq_data_pool)
		kmem_cache_destroy(ad->rq_data_pool);

	if (ad->dl_group_pool)
		kmem_cache_destroy(ad->dl_group_pool);

	kfree(ad);
}

// Define sysfs attributes for read operation latency model
#define SYSFS_OPTYPE_DECL(name, optype)					\
static ssize_t adios_lat_model_##name##_show(struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data;				\
	struct latency_model *model = &ad->latency_model[optype];		\
	ssize_t len = 0;						\
	guard(spinlock_irqsave)(&model->lock); \
	len += sprintf(page,       "base : %llu ns\n", model->base);	\
	len += sprintf(page + len, "slope: %llu ns/KiB\n", model->slope);	\
	return len;							\
} \
static ssize_t adios_lat_target_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	struct adios_data *ad = e->elevator_data;						\
	unsigned long nsec;								\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &nsec);							\
	if (ret)									\
		return ret;									\
											\
	ad->latency_model[optype].base = 0ULL;					\
	ad->latency_target[optype] = nsec;						\
											\
	return count;									\
}										\
static ssize_t adios_lat_target_##name##_show( \
		struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data;						\
	return sprintf(page, "%llu\n", ad->latency_target[optype]);			\
} \
static ssize_t adios_batch_limit_##name##_store( \
		struct elevator_queue *e, const char *page, size_t count) { \
	unsigned long max_batch;							\
	int ret;									\
											\
	ret = kstrtoul(page, 10, &max_batch);						\
	if (ret || max_batch == 0)							\
		return -EINVAL;								\
											\
	struct adios_data *ad = e->elevator_data;					\
	ad->batch_limit[optype] = max_batch;					\
											\
	return count;									\
}										\
static ssize_t adios_batch_limit_##name##_show( \
		struct elevator_queue *e, char *page) { \
	struct adios_data *ad = e->elevator_data;						\
	return sprintf(page, "%u\n", ad->batch_limit[optype]);				\
}

SYSFS_OPTYPE_DECL(read, ADIOS_READ);
SYSFS_OPTYPE_DECL(write, ADIOS_WRITE);
SYSFS_OPTYPE_DECL(discard, ADIOS_DISCARD);

// Show the maximum batch size actually achieved for each operation type
static ssize_t adios_batch_actual_max_show(
		struct elevator_queue *e, char *page) {
	struct adios_data *ad = e->elevator_data;
	unsigned int total_count, read_count, write_count, discard_count;

	total_count = ad->batch_actual_max_total;
	read_count = ad->batch_actual_max_size[ADIOS_READ];
	write_count = ad->batch_actual_max_size[ADIOS_WRITE];
	discard_count = ad->batch_actual_max_size[ADIOS_DISCARD];

	return sprintf(page,
		"Total  : %u\nDiscard: %u\nRead   : %u\nWrite  : %u\n",
		total_count, discard_count, read_count, write_count);
}

// Set the global latency window
static ssize_t adios_global_latency_window_store(
		struct elevator_queue *e, const char *page, size_t count) {
	unsigned long nsec;
	int ret;

	ret = kstrtoul(page, 10, &nsec);
	if (ret)
		return ret;

	global_latency_window = nsec;

	return count;
}

// Show the global latency window
static ssize_t adios_global_latency_window_show(
		struct elevator_queue *e, char *page) {
	return sprintf(page, "%llu\n", global_latency_window);
}

// Show the bq_refill_below_ratio
static ssize_t adios_bq_refill_below_ratio_show(
		struct elevator_queue *e, char *page) {
	return sprintf(page, "%d\n", bq_refill_below_ratio);
}

// Set the bq_refill_below_ratio
static ssize_t adios_bq_refill_below_ratio_store(
		struct elevator_queue *e, const char *page, size_t count) {
	int ratio;
	int ret;

	ret = kstrtoint(page, 10, &ratio);
	if (ret || ratio < 0 || ratio > 100)
		return -EINVAL;

	bq_refill_below_ratio = ratio;

	return count;
}

// Reset batch queue statistics
static ssize_t adios_reset_bq_stats_store(
		struct elevator_queue *e, const char *page, size_t count) {
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	for (int i = 0; i < ADIOS_OPTYPES; i++)
		ad->batch_actual_max_size[i] = 0;

	ad->batch_actual_max_total = 0;

	return count;
}

// Reset the latency model parameters
static ssize_t adios_reset_lat_model_store(
		struct elevator_queue *e, const char *page, size_t count) {
	struct adios_data *ad = e->elevator_data;
	unsigned long val;
	int ret;

	ret = kstrtoul(page, 10, &val);
	if (ret || val != 1)
		return -EINVAL;

	for (int i = 0; i < ADIOS_OPTYPES; i++) {
		struct latency_model *model = &ad->latency_model[i];
		unsigned long flags;
		spin_lock_irqsave(&model->lock, flags);
		model->base = 0ULL;
		model->slope = 0ULL;
		model->small_sum_delay = 0ULL;
		model->small_count = 0ULL;
		model->large_sum_delay = 0ULL;
		model->large_sum_bsize = 0ULL;
		spin_unlock_irqrestore(&model->lock, flags);
	}

	return count;
}

// Show the ADIOS version
static ssize_t adios_version_show(struct elevator_queue *e, char *page) {
	return sprintf(page, "%s\n", ADIOS_VERSION);
}

// Define sysfs attributes
#define AD_ATTR(name, show_func, store_func) \
	__ATTR(name, 0644, show_func, store_func)
#define AD_ATTR_RW(name) \
	__ATTR(name, 0644, adios_##name##_show, adios_##name##_store)
#define AD_ATTR_RO(name) \
	__ATTR(name, 0644, adios_##name##_show, NULL)
#define AD_ATTR_WO(name) \
	__ATTR(name, 0644, NULL, adios_##name##_store)

// Define sysfs attributes for ADIOS scheduler
static struct elv_fs_entry adios_sched_attrs[] = {
	AD_ATTR_RO(batch_actual_max),
	AD_ATTR_RW(bq_refill_below_ratio),
	AD_ATTR_RW(global_latency_window),

	AD_ATTR_RW(batch_limit_read),
	AD_ATTR_RW(batch_limit_write),
	AD_ATTR_RW(batch_limit_discard),

	AD_ATTR_RO(lat_model_read),
	AD_ATTR_RO(lat_model_write),
	AD_ATTR_RO(lat_model_discard),

	AD_ATTR_RW(lat_target_read),
	AD_ATTR_RW(lat_target_write),
	AD_ATTR_RW(lat_target_discard),

	AD_ATTR_WO(reset_bq_stats),
	AD_ATTR_WO(reset_lat_model),
	AD_ATTR(adios_version, adios_version_show, NULL),

	__ATTR_NULL
};

// Define the ADIOS scheduler type
static struct elevator_type mq_adios = {
	.ops = {
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.limit_depth		= adios_limit_depth,
		.depth_updated		= adios_depth_updated,
		.request_merged		= adios_request_merged,
		.requests_merged	= adios_merged_requests,
		.bio_merge			= adios_bio_merge,
		.insert_requests	= adios_insert_requests,
		.prepare_request	= adios_prepare_request,
		.dispatch_request	= adios_dispatch_request,
		.completed_request	= adios_completed_request,
		.finish_request		= adios_finish_request,
		.has_work			= adios_has_work,
		.init_hctx			= adios_init_hctx,
		.init_sched			= adios_init_sched,
		.exit_sched			= adios_exit_sched,
	},
#ifdef CONFIG_BLK_DEBUG_FS
#endif
	.elevator_attrs = adios_sched_attrs,
	.elevator_name = "adios",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-adios-iosched");

#define ADIOS_PROGNAME "Adaptive Deadline I/O Scheduler"
#define ADIOS_AUTHOR   "Masahito Suzuki"

// Initialize the ADIOS scheduler module
static int __init adios_init(void) {
	printk(KERN_INFO "%s %s by %s\n",
		ADIOS_PROGNAME, ADIOS_VERSION, ADIOS_AUTHOR);
	return elv_register(&mq_adios);
}

// Exit the ADIOS scheduler module
static void __exit adios_exit(void) {
	elv_unregister(&mq_adios);
}

module_init(adios_init);
module_exit(adios_exit);

MODULE_AUTHOR(ADIOS_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(ADIOS_PROGNAME);
