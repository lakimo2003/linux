// SPDX-License-Identifier: GPL-2.0

#include <linux/sched.h>
#include <linux/sched/cache_guard.h>
#include <linux/cpumask.h>
#include <linux/types.h>
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>


static struct cache_tile *cache_tiles;
static int num_cache_tiles;
static DEFINE_MUTEX(cache_tiles_lock);

struct cache_tile {
	int id;
	cpumask_t cpus;
	bool reserved_for_secure;
};

static int migrate_task_to_tile(struct task_struct *p, int tile_id);

static int find_tile_in_array(struct cache_tile *tiles, int nr,
			      const struct cpumask *mask)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (cpumask_equal(&tiles[i].cpus, mask))
			return i;
	}

	return -1;
}

static int cache_build_L2_tiles(void)
{
	struct cache_tile *tiles;
	struct cacheinfo *cache_info;
	cpumask_t mask;
	int cpu;
	int num = 0;

	tiles = kcalloc(num_possible_cpus(), sizeof(*tiles), GFP_KERNEL);
	if (!tiles)
		return -ENOMEM;

	cpus_read_lock();

	for_each_online_cpu(cpu) {
		cache_info = get_cpu_cacheinfo_level(cpu, 2);
		if (!cache_info)
			continue;

		cpumask_and(&mask, &cache_info->shared_cpu_map, cpu_online_mask);
		if (cpumask_empty(&mask))
			continue;

		if (find_tile_in_array(tiles, num, &mask) >= 0)
			continue;

		tiles[num].id = num;
		cpumask_copy(&tiles[num].cpus, &mask);
		tiles[num].reserved_for_secure = false;
		num++;
	}

	cpus_read_unlock();

	mutex_lock(&cache_tiles_lock);
	kfree(cache_tiles);
	cache_tiles = tiles;
	num_cache_tiles = num;
	mutex_unlock(&cache_tiles_lock);
	return 0;
}

static unsigned int cache_count_tasks_on_tile(const struct cache_tile *tile,
					      const struct task_struct *skip)
{
	struct task_struct *g;
	struct task_struct *t;
	unsigned int count = 0;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (t == skip)
			continue;

		if (cpumask_test_cpu(task_cpu(t), &tile->cpus))
			count++;
	}
	rcu_read_unlock();

	return count;
}

static int cache_ensure_tiles(void)
{
	int ret = 0;

	mutex_lock(&cache_tiles_lock);

	if (num_cache_tiles > 0)
		goto out;

	mutex_unlock(&cache_tiles_lock);
	return cache_build_L2_tiles();

out:
	mutex_unlock(&cache_tiles_lock);
	return ret;
}

static int cache_move_to_secure_tile(struct task_struct *p)
{
	int i;
	int best_tile = -1;
	int ret;
	unsigned int task_count;
	unsigned int best_task_count = ~0U;

	if (!p)
		return -EINVAL;

	ret = cache_ensure_tiles();
	if (ret)
		return ret;

	mutex_lock(&cache_tiles_lock);

	for (i = 0; i < num_cache_tiles; i++) {
		if (cpumask_weight_and(&cache_tiles[i].cpus, cpu_active_mask) != 1)
			continue;

		task_count = cache_count_tasks_on_tile(&cache_tiles[i], p);
		if (task_count < best_task_count) {
			best_task_count = task_count;
			best_tile = i;

			if (task_count == 0)
				break;
		}
	}

	if (best_tile >= 0)
		cache_tiles[best_tile].reserved_for_secure = true;

	mutex_unlock(&cache_tiles_lock);

	if (best_tile < 0)
		return -ENODEV;

	return migrate_task_to_tile(p, best_tile);
}


int cache_guard_mark_secure(struct task_struct *p)
{
	int ret;

	if (!p)
		return -EINVAL;

	ret = cache_ensure_tiles();
	if (ret)
		return ret;

	ret = cache_move_to_secure_tile(p);
	if (!ret)
		p->cache_guard.secure = true;

	return ret;
}


static int migrate_task_to_tile(struct task_struct *p, int tile_id)
{
	cpumask_var_t target_mask;
	int ret;

	if (!p)
		return -EINVAL;

	if (!alloc_cpumask_var(&target_mask, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&cache_tiles_lock);

	if (tile_id < 0 || tile_id >= num_cache_tiles) {
		mutex_unlock(&cache_tiles_lock);
		free_cpumask_var(target_mask);
		return -EINVAL;
	}

	cpumask_and(target_mask, &cache_tiles[tile_id].cpus, cpu_active_mask);

	mutex_unlock(&cache_tiles_lock);

	if (cpumask_empty(target_mask)) {
		free_cpumask_var(target_mask);
		return -ENODEV;
	}

	ret = set_cpus_allowed_ptr(p, target_mask);

	free_cpumask_var(target_mask);
	return ret;
}

void cache_guard_periodic_check(void)
{
	/* later: scan secure tasks and call migration helper */
}

int cache_guard_migrate_secure_task_or_force_isolation(struct task_struct *p)
{
	if (!p)
		return -EINVAL;

	/* later: find tile, migrate, or force isolation */

	return 0;
}
