/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief       Primitive list scheduling with different node selectors.
 * @author      Sebastian Hack
 * @date        20.10.2004
 * @version     $Id$
 */
#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>

#include "benode.h"
#include "be_t.h"

#include "obst.h"
#include "list.h"
#include "iterator.h"

#include "iredges_t.h"
#include "irgwalk.h"
#include "irnode_t.h"
#include "irmode_t.h"
#include "irdump.h"
#include "irprintf_t.h"
#include "array.h"
#include "debug.h"
#include "irtools.h"

#include "bemodule.h"
#include "besched.h"
#include "beutil.h"
#include "belive_t.h"
#include "belistsched.h"
#include "bearch.h"
#include "bestat.h"
#include "beirg.h"

#include "lc_opts.h"
#include "lc_opts_enum.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL);

/**
 * All scheduling info needed per node.
 */
typedef struct sched_irn_t {
	unsigned num_not_sched_user; /**< The number of not yet scheduled users of this node */
	unsigned already_sched : 1;  /**< Set if this node is already scheduled */
} sched_irn_t;

/**
 * Scheduling environment for the whole graph.
 */
typedef struct sched_env_t {
	sched_irn_t *sched_info;                    /**< scheduling info per node */
	const list_sched_selector_t *selector;      /**< The node selector. */
	void *selector_env;                         /**< A pointer to give to the selector. */
} sched_env_t;

/**
 * Environment for a block scheduler.
 */
typedef struct block_sched_env_t {
	sched_irn_t *sched_info;                    /**< scheduling info per node, copied from the global scheduler object */
	ir_nodeset_t cands;                         /**< the set of candidates */
	ir_node *block;                             /**< the current block */
	sched_env_t *sched_env;                     /**< the scheduler environment */
	ir_nodeset_t live;                          /**< simple liveness during scheduling */
	const list_sched_selector_t *selector;
	void *selector_block_env;
} block_sched_env_t;

/**
 * Returns non-zero if the node is already scheduled
 */
static inline int is_already_scheduled(block_sched_env_t *env, ir_node *n)
{
	unsigned const idx = get_irn_idx(n);

	assert(idx < ARR_LEN(env->sched_info));
	return env->sched_info[idx].already_sched;
}

/**
 * Mark a node as already scheduled
 */
static inline void set_already_scheduled(block_sched_env_t *env, ir_node *n)
{
	unsigned const idx = get_irn_idx(n);

	assert(idx < ARR_LEN(env->sched_info));
	env->sched_info[idx].already_sched = 1;
}

static void add_to_sched(block_sched_env_t *env, ir_node *irn);

/**
 * Try to put a node in the ready set.
 * @param env   The block scheduler environment.
 * @param pred  The previous scheduled node.
 * @param irn   The node to make ready.
 * @return 1, if the node could be made ready, 0 else.
 */
static inline int make_ready(block_sched_env_t *env, ir_node *pred, ir_node *irn)
{
	int i, n;

	/* Blocks cannot be scheduled. */
	if (is_Block(irn) || get_irn_n_edges(irn) == 0)
		return 0;

	/*
	 * Check, if the given ir node is in a different block as the
	 * currently scheduled one. If that is so, don't make the node ready.
	 */
	if (env->block != get_nodes_block(irn))
		return 0;

	for (i = 0, n = get_irn_ins_or_deps(irn); i < n; ++i) {
		ir_node *op = get_irn_in_or_dep(irn, i);

		/* if irn is an End we have keep-alives and op might be a block, skip that */
		if (is_Block(op)) {
			assert(is_End(irn));
			continue;
		}

		/* If the operand is local to the scheduled block and not yet
		 * scheduled, this nodes cannot be made ready, so exit. */
		if (! is_already_scheduled(env, op) && get_nodes_block(op) == env->block)
			return 0;
	}

	if (! to_appear_in_schedule(irn)) {
		add_to_sched(env, irn);
		DB((dbg, LEVEL_3, "\tmaking immediately available: %+F\n", irn));
	} else {
		ir_nodeset_insert(&env->cands, irn);

		/* Notify selector about the ready node. */
		if (env->selector->node_ready)
			env->selector->node_ready(env->selector_block_env, irn, pred);

		DB((dbg, LEVEL_2, "\tmaking ready: %+F\n", irn));
	}

    return 1;
}

/**
 * Try, to make all users of a node ready.
 * In fact, a usage node can only be made ready, if all its operands
 * have already been scheduled yet. This is checked by make_ready().
 * @param env The block schedule environment.
 * @param irn The node, which usages (successors) are to be made ready.
 */
static void make_users_ready(block_sched_env_t *env, ir_node *irn)
{
	const ir_edge_t *edge;

	/* make all data users ready */
	foreach_out_edge(irn, edge) {
		ir_node *user = get_edge_src_irn(edge);

		if (! is_Phi(user))
			make_ready(env, irn, user);
	}

	/* and the dependent nodes as well */
	foreach_out_edge_kind(irn, edge, EDGE_KIND_DEP) {
		ir_node *user = get_edge_src_irn(edge);

		if (! is_Phi(user))
			make_ready(env, irn, user);
	}
}

/**
 * Returns the number of not yet schedules users.
 */
static inline int get_irn_not_sched_user(block_sched_env_t *env, ir_node *n)
{
	unsigned const idx = get_irn_idx(n);

	assert(idx < ARR_LEN(env->sched_info));
	return env->sched_info[idx].num_not_sched_user;
}

/**
 * Sets the number of not yet schedules users.
 */
static inline void set_irn_not_sched_user(block_sched_env_t *env, ir_node *n, int num)
{
	unsigned const idx = get_irn_idx(n);

	assert(idx < ARR_LEN(env->sched_info));
	env->sched_info[idx].num_not_sched_user = num;
}

/**
 * Add @p num to the number of not yet schedules users and returns the result.
 */
static inline int add_irn_not_sched_user(block_sched_env_t *env, ir_node *n, int num)
{
	unsigned const idx = get_irn_idx(n);

	assert(idx < ARR_LEN(env->sched_info));
	env->sched_info[idx].num_not_sched_user += num;
	return env->sched_info[idx].num_not_sched_user;
}

/**
 * Returns the number of users of a node having mode datab.
 */
static int get_num_successors(ir_node *irn)
{
	int             sum = 0;
	const ir_edge_t *edge;

	if (get_irn_mode(irn) == mode_T) {
		/* for mode_T nodes: count the users of all Projs */
		foreach_out_edge(irn, edge) {
			ir_node *proj = get_edge_src_irn(edge);
			ir_mode *mode = get_irn_mode(proj);

			if (mode == mode_T) {
				sum += get_num_successors(proj);
			} else if (mode_is_datab(mode)) {
				sum += get_irn_n_edges(proj);
			}
		}
	}
	else {
		/* do not count keep-alive edges */
		foreach_out_edge(irn, edge) {
			if (get_irn_opcode(get_edge_src_irn(edge)) != iro_End)
				sum++;
		}
	}

	return sum;
}

/**
 * Adds irn to @p live, updates all inputs that this user is scheduled
 * and counts all of its non scheduled users.
 */
static void update_sched_liveness(block_sched_env_t *env, ir_node *irn)
{
	int i;

	/* ignore Projs */
	if (is_Proj(irn))
		return;

	for (i = get_irn_ins_or_deps(irn) - 1; i >= 0; --i) {
		ir_node *in = get_irn_in_or_dep(irn, i);

		/* if in is a proj: update predecessor */
		in = skip_Proj(in);

		/* if in is still in the live set: reduce number of users by one */
		if (ir_nodeset_contains(&env->live, in)) {
			if (add_irn_not_sched_user(env, in, -1) <= 0)
				ir_nodeset_remove(&env->live, in);
		}
	}

	/*
		get_num_successors returns the number of all users. This includes
		users in different blocks as well. As the each block is scheduled separately
		the liveness info of those users will not be updated and so these
		users will keep up the register pressure as it is desired.
	*/
	i = get_num_successors(irn);
	if (i > 0) {
		set_irn_not_sched_user(env, irn, i);
		ir_nodeset_insert(&env->live, irn);
	}
}

/**
 * Append an instruction to a schedule.
 * @param env The block scheduling environment.
 * @param irn The node to add to the schedule.
 * @return    The given node.
 */
static void add_to_sched(block_sched_env_t *env, ir_node *irn)
{
    /* If the node consumes/produces data, it is appended to the schedule
     * list, otherwise, it is not put into the list */
    if (to_appear_in_schedule(irn)) {
		update_sched_liveness(env, irn);
		sched_add_before(env->block, irn);

		DBG((dbg, LEVEL_2, "\tadding %+F\n", irn));

		/* Remove the node from the ready set */
		ir_nodeset_remove(&env->cands, irn);
    }

	/* notify the selector about the finally selected node. */
	if (env->selector->node_selected)
		env->selector->node_selected(env->selector_block_env, irn);

    /* Insert the node in the set of all available scheduled nodes. */
    set_already_scheduled(env, irn);

	make_users_ready(env, irn);
}

/**
 * Perform list scheduling on a block.
 *
 * Note, that the caller must compute a linked list of nodes in the block
 * using the link field before calling this function.
 *
 * Also the outs must have been computed.
 *
 * @param block The block node.
 * @param env Scheduling environment.
 */
static void list_sched_block(ir_node *block, void *env_ptr)
{
	sched_env_t *env                      = (sched_env_t*)env_ptr;
	const list_sched_selector_t *selector = env->selector;

	block_sched_env_t be;
	const ir_edge_t *edge;
	ir_node *irn;
	int j, m;

	/* Initialize the block's list head that will hold the schedule. */
	sched_init_block(block);

	/* Initialize the block scheduling environment */
	be.sched_info = env->sched_info;
	be.block      = block;
	ir_nodeset_init_size(&be.cands, get_irn_n_edges(block));
	ir_nodeset_init_size(&be.live, get_irn_n_edges(block));
	be.selector   = selector;
	be.sched_env  = env;

	DBG((dbg, LEVEL_1, "scheduling %+F\n", block));

	if (selector->init_block)
		be.selector_block_env = selector->init_block(env->selector_env, block);

	/* Then one can add all nodes are ready to the set. */
	foreach_out_edge(block, edge) {
		ir_node   *irn  = get_edge_src_irn(edge);
		unsigned   code = get_irn_opcode(irn);
		int users;

		if (code == iro_End) {
			/* Skip the end node because of keep-alive edges. */
			continue;
		}

		users = get_irn_n_edges(irn);
		if (users == 0)
			continue;
		else if (users == 1) { /* ignore nodes that are only hold by the anchor */
			const ir_edge_t *edge = get_irn_out_edge_first_kind(irn, EDGE_KIND_NORMAL);
			ir_node *user = get_edge_src_irn(edge);
			if (is_Anchor(user))
				continue;
		}

		if (is_Phi(irn)) {
			/*
			   Phi functions are scheduled immediately, since they only
			   transfer data flow from the predecessors to this block.
			*/
			add_to_sched(&be, irn);
		} else if (be_is_Start(irn)) {
			/* The start block will be scheduled as the first node */
			add_to_sched(&be, irn);
		} else {
			/* Other nodes must have all operands in other blocks to be made
			 * ready */
			int ready = 1;

			/* Check, if the operands of a node are not local to this block */
			for (j = 0, m = get_irn_ins_or_deps(irn); j < m; ++j) {
				ir_node *operand = get_irn_in_or_dep(irn, j);

				if (get_nodes_block(operand) == block) {
					ready = 0;
					break;
				} else {
					/* live in values increase register pressure */
					ir_nodeset_insert(&be.live, operand);
				}
			}

			/* Make the node ready, if all operands live in a foreign block */
			if (ready) {
				DBG((dbg, LEVEL_2, "\timmediately ready: %+F\n", irn));
				make_ready(&be, NULL, irn);
			}
		}
	}

	/* Iterate over all remaining nodes */
	while (ir_nodeset_size(&be.cands) > 0) {
		ir_nodeset_iterator_t iter;

		/* Keeps must be scheduled immediately */
		foreach_ir_nodeset(&be.cands, irn, iter) {
			if (be_is_Keep(irn) || be_is_CopyKeep(irn) || is_Sync(irn)) {
				break;
			}
		}

		if (! irn) {
			/* Keeps must be immediately scheduled */
			irn = be.selector->select(be.selector_block_env, &be.cands, &be.live);
		}

		DB((dbg, LEVEL_2, "\tpicked node %+F\n", irn));

		/* Add the node to the schedule. */
		add_to_sched(&be, irn);

		/* remove the scheduled node from the ready list. */
		ir_nodeset_remove(&be.cands, irn);
	}

	if (selector->finish_block)
		selector->finish_block(be.selector_block_env);

	ir_nodeset_destroy(&be.cands);
	ir_nodeset_destroy(&be.live);
}

/* List schedule a graph. */
void be_list_sched_graph(ir_graph *irg, const list_sched_selector_t *selector)
{
	int num_nodes;
	sched_env_t env;

#if 1
	/* Matze: This is very slow, we should avoid it to improve backend speed,
	 * we just have to make sure that we have no dangling out-edges at this
	 * point...
	 */

	/* Assure, that we have no dangling out-edges to deleted stuff */
	edges_deactivate(irg);
	edges_activate(irg);
#endif

	num_nodes = get_irg_last_idx(irg);

	/* initialize environment for list scheduler */
	memset(&env, 0, sizeof(env));
	env.selector   = selector;
	env.sched_info = NEW_ARR_F(sched_irn_t, num_nodes);

	memset(env.sched_info, 0, num_nodes * sizeof(env.sched_info[0]));

	if (selector->init_graph != NULL)
		env.selector_env = selector->init_graph(irg);

	/* Schedule each single block. */
	irg_block_walk_graph(irg, list_sched_block, NULL, &env);

	if (selector->finish_graph != NULL)
		selector->finish_graph(env.selector_env);

	DEL_ARR_F(env.sched_info);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_listsched);
void be_init_listsched(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.sched");
}
