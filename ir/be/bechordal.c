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
 * @brief       Chordal register allocation.
 * @author      Sebastian Hack
 * @date        08.12.2004
 */
#include "config.h"

#include "bechordal_common.h"
#include "bechordal_draw.h"
#include "bechordal_t.h"
#include "beinsn_t.h"
#include "beintlive_t.h"
#include "beirg.h"
#include "bemodule.h"
#include "debug.h"
#include "irdump.h"

#define USE_HUNGARIAN 0

#if USE_HUNGARIAN
#include "hungarian.h"
#else
#include "bipartite.h"
#endif

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

typedef struct be_chordal_alloc_env_t {
	be_chordal_env_t *chordal_env;
	bitset_t         *live;        /**< A liveness bitset. */
	bitset_t         *tmp_colors;  /**< An auxiliary bitset which is as long as the number of colors in the class. */
	bitset_t         *colors;      /**< The color mask. */
} be_chordal_alloc_env_t;

static int get_next_free_reg(be_chordal_alloc_env_t const *const alloc_env, bitset_t *const colors)
{
	bitset_t *tmp = alloc_env->tmp_colors;
	bitset_copy(tmp, colors);
	bitset_flip_all(tmp);
	bitset_and(tmp, alloc_env->chordal_env->allocatable_regs);
	return bitset_next_set(tmp, 0);
}

static bitset_t const *get_decisive_partner_regs(be_operand_t const *const o1)
{
	be_operand_t const *const o2 = o1->partner;
	assert(!o2 || o1->req->cls == o2->req->cls);

	if (!o2 || bitset_contains(o1->regs, o2->regs)) {
		return o1->regs;
	} else if (bitset_contains(o2->regs, o1->regs)) {
		return o2->regs;
	} else {
		return NULL;
	}
}

static void pair_up_operands(be_chordal_env_t const *const env, be_insn_t *const insn)
{
	/* For each out operand, try to find an in operand which can be assigned the
	 * same register as the out operand. */
	int       const n_regs = env->cls->n_regs;
	bitset_t *const bs     = bitset_alloca(n_regs);
	be_lv_t  *const lv     = be_get_irg_liveness(env->irg);
	for (int j = 0; j < insn->use_start; ++j) {
		/* Try to find an in operand which has ... */
		be_operand_t       *smallest        = NULL;
		int                 smallest_n_regs = n_regs + 1;
		be_operand_t *const out_op          = &insn->ops[j];
		for (int i = insn->use_start; i < insn->n_ops; ++i) {
			be_operand_t *const op = &insn->ops[i];
			if (op->partner || be_values_interfere(lv, op->irn, op->carrier))
				continue;

			bitset_copy(bs, op->regs);
			bitset_and(bs, out_op->regs);
			int const n_total = bitset_popcount(op->regs);
			if (!bitset_is_empty(bs) && n_total < smallest_n_regs) {
				smallest        = op;
				smallest_n_regs = n_total;
			}
		}

		if (smallest != NULL) {
			for (int i = insn->use_start; i < insn->n_ops; ++i) {
				if (insn->ops[i].carrier == smallest->carrier)
					insn->ops[i].partner = out_op;
			}

			out_op->partner   = smallest;
			smallest->partner = out_op;
		}
	}
}

static bool list_contains_irn(ir_node *const *const list, size_t const n, ir_node *const irn)
{
	for (ir_node *const *i = list; i != list + n; ++i) {
		if (*i == irn)
			return true;
	}
	return false;
}

static void handle_constraints(be_chordal_alloc_env_t *const alloc_env, ir_node *const irn)
{
	be_chordal_env_t *const env  = alloc_env->chordal_env;
	void             *const base = obstack_base(env->obst);
	be_insn_t              *insn = be_scan_insn(env, irn);

	/* Perms inserted before the constraint handling phase are considered to be
	 * correctly precolored. These Perms arise during the ABI handling phase. */
	if (!insn->has_constraints || is_Phi(irn))
		goto end;

	/* Prepare the constraint handling of this node.
	 * Perms are constructed and Copies are created for constrained values
	 * interfering with the instruction. */
	ir_node *const perm = pre_process_constraints(env, &insn);

	/* find suitable in operands to the out operands of the node. */
	pair_up_operands(env, insn);

	/* Look at the in/out operands and add each operand (and its possible partner)
	 * to a bipartite graph (left: nodes with partners, right: admissible colors). */
	int                        n_alloc     = 0;
	int                  const n_regs      = env->cls->n_regs;
	ir_node            **const alloc_nodes = ALLOCAN(ir_node*, n_regs);
	pmap                *const partners    = pmap_create();
#if USE_HUNGARIAN
	hungarian_problem_t *const bp          = hungarian_new(n_regs, n_regs, HUNGARIAN_MATCH_PERFECT);
#else
	bipartite_t         *const bp          = bipartite_new(n_regs, n_regs);
#endif
	for (int i = 0; i < insn->n_ops; ++i) {
		/* If the operand has no partner or the partner has not been marked
		 * for allocation, determine the admissible registers and mark it
		 * for allocation by associating the node and its partner with the
		 * set of admissible registers via a bipartite graph. */
		be_operand_t *const op = &insn->ops[i];
		if (op->partner && pmap_contains(partners, op->partner->carrier))
			continue;

		ir_node *const partner = op->partner ? op->partner->carrier : NULL;
		pmap_insert(partners, op->carrier, partner);
		if (partner != NULL)
			pmap_insert(partners, partner, op->carrier);

		/* Don't insert a node twice. */
		if (list_contains_irn(alloc_nodes, n_alloc, op->carrier))
			continue;

		alloc_nodes[n_alloc] = op->carrier;

		DBG((dbg, LEVEL_2, "\tassociating %+F and %+F\n", op->carrier, partner));

		bitset_t const *const bs = get_decisive_partner_regs(op);
		if (bs) {
			DBG((dbg, LEVEL_2, "\tallowed registers for %+F: %B\n", op->carrier, bs));

			bitset_foreach(bs, col) {
#if USE_HUNGARIAN
				hungarian_add(bp, n_alloc, col, 1);
#else
				bipartite_add(bp, n_alloc, col);
#endif
			}
		} else {
			DBG((dbg, LEVEL_2, "\tallowed registers for %+F: none\n", op->carrier));
		}

		n_alloc++;
	}

	/* Put all nodes which live through the constrained instruction also to the
	 * allocation bipartite graph. They are considered unconstrained. */
	if (perm != NULL) {
		be_lv_t *const lv = be_get_irg_liveness(env->irg);
		foreach_out_edge(perm, edge) {
			ir_node *const proj = get_edge_src_irn(edge);
			assert(is_Proj(proj));

			if (!be_values_interfere(lv, proj, irn) || pmap_contains(partners, proj))
				continue;

			/* Don't insert a node twice. */
			if (list_contains_irn(alloc_nodes, n_alloc, proj))
				continue;

			assert(n_alloc < n_regs);

			alloc_nodes[n_alloc] = proj;
			pmap_insert(partners, proj, NULL);

			bitset_foreach(env->allocatable_regs, col) {
#if USE_HUNGARIAN
				hungarian_add(bp, n_alloc, col, 1);
#else
				bipartite_add(bp, n_alloc, col);
#endif
			}

			n_alloc++;
		}
	}

	/* Compute a valid register allocation. */
	int *const assignment = ALLOCAN(int, n_regs);
#if USE_HUNGARIAN
	hungarian_prepare_cost_matrix(bp, HUNGARIAN_MODE_MAXIMIZE_UTIL);
	int const match_res = hungarian_solve(bp, assignment, NULL, 1);
	assert(match_res == 0 && "matching failed");
#else
	bipartite_matching(bp, assignment);
#endif

	/* Assign colors obtained from the matching. */
	for (int i = 0; i < n_alloc; ++i) {
		assert(assignment[i] >= 0 && "there must have been a register assigned (node not register pressure faithful?)");
		arch_register_t const *const reg = arch_register_for_index(env->cls, assignment[i]);

		ir_node *const irn = alloc_nodes[i];
		if (irn != NULL) {
			arch_set_irn_register(irn, reg);
			DBG((dbg, LEVEL_2, "\tsetting %+F to register %s\n", irn, reg->name));
		}

		ir_node *const partner = pmap_get(ir_node, partners, alloc_nodes[i]);
		if (partner != NULL) {
			arch_set_irn_register(partner, reg);
			DBG((dbg, LEVEL_2, "\tsetting %+F to register %s\n", partner, reg->name));
		}
	}

	/* Allocate the non-constrained Projs of the Perm. */
	if (perm != NULL) {
		bitset_t *const bs = bitset_alloca(n_regs);

		/* Put the colors of all Projs in a bitset. */
		foreach_out_edge(perm, edge) {
			ir_node               *const proj = get_edge_src_irn(edge);
			arch_register_t const *const reg  = arch_get_irn_register(proj);
			if (reg != NULL)
				bitset_set(bs, reg->index);
		}

		/* Assign the not yet assigned Projs of the Perm a suitable color. */
		foreach_out_edge(perm, edge) {
			ir_node               *const proj = get_edge_src_irn(edge);
			arch_register_t const *const reg  = arch_get_irn_register(proj);

			DBG((dbg, LEVEL_2, "\tchecking reg of %+F: %s\n", proj, reg ? reg->name : "<none>"));

			if (reg == NULL) {
				size_t const col = get_next_free_reg(alloc_env, bs);
				arch_register_t const *const new_reg = arch_register_for_index(env->cls, col);
				bitset_set(bs, new_reg->index);
				arch_set_irn_register(proj, new_reg);
				DBG((dbg, LEVEL_2, "\tsetting %+F to register %s\n", proj, new_reg->name));
			}
		}
	}

#if USE_HUNGARIAN
	hungarian_free(bp);
#else
	bipartite_free(bp);
#endif
	pmap_destroy(partners);

end:
	obstack_free(env->obst, base);
}

/**
 * Handle constraint nodes in each basic block.
 * handle_constraints() inserts Perm nodes which perm
 * over all values live at the constrained node right in front
 * of the constrained node. These Perms signal a constrained node.
 * For further comments, refer to handle_constraints().
 */
static void constraints(ir_node *const bl, void *const data)
{
	be_chordal_alloc_env_t *const env = (be_chordal_alloc_env_t*)data;
	for (ir_node *irn = sched_first(bl); !sched_is_end(irn);) {
		ir_node *const next = sched_next(irn);
		handle_constraints(env, irn);
		irn = next;
	}
}

static void assign(ir_node *const block, void *const env_ptr)
{
	be_chordal_alloc_env_t *const alloc_env = (be_chordal_alloc_env_t*)env_ptr;
	be_chordal_env_t       *const env       = alloc_env->chordal_env;
	bitset_t               *const live      = alloc_env->live;
	bitset_t               *const colors    = alloc_env->colors;
	struct list_head       *const head      = get_block_border_head(env, block);
	be_lv_t                *const lv        = be_get_irg_liveness(env->irg);

	bitset_clear_all(colors);
	bitset_clear_all(live);

	DBG((dbg, LEVEL_4, "Assigning colors for block %+F\n", block));
	DBG((dbg, LEVEL_4, "\tusedef chain for block\n"));
	foreach_border_head(head, b) {
		DBG((dbg, LEVEL_4, "\t%s %+F/%d\n", b->is_def ? "def" : "use",
					b->irn, get_irn_idx(b->irn)));
	}

	/* Add initial defs for all values live in.
	 * Since their colors have already been assigned (The dominators were
	 * allocated before), we have to mark their colors as used also. */
	be_lv_foreach(lv, block, be_lv_state_in, irn) {
		if (arch_irn_consider_in_reg_alloc(env->cls, irn)) {
			arch_register_t const *const reg = arch_get_irn_register(irn);

			assert(reg && "Node must have been assigned a register");
			DBG((dbg, LEVEL_4, "%+F has reg %s\n", irn, reg->name));

			/* Mark the color of the live in value as used. */
			bitset_set(colors, reg->index);

			/* Mark the value live in. */
			bitset_set(live, get_irn_idx(irn));
		}
	}

	/* Mind that the sequence of defs from back to front defines a perfect
	 * elimination order. So, coloring the definitions from first to last
	 * will work. */
	foreach_border_head(head, b) {
		ir_node *const irn = b->irn;
		int      const nr  = get_irn_idx(irn);

		/* Assign a color, if it is a local def. Global defs already have a
		 * color. */
		if (!b->is_def) {
			/* Clear the color upon a use. */
			arch_register_t const *const reg = arch_get_irn_register(irn);
			assert(reg && "Register must have been assigned");
			bitset_clear(colors, reg->index);
			bitset_clear(live, nr);
		} else if (!be_is_live_in(lv, block, irn)) {
			int                    col;
			arch_register_t const *reg = arch_get_irn_register(irn);
			if (reg) {
				col = reg->index;
				assert(!bitset_is_set(colors, col) && "pre-colored register must be free");
			} else {
				assert(!arch_irn_is_ignore(irn));
				col = get_next_free_reg(alloc_env, colors);
				reg = arch_register_for_index(env->cls, col);
				arch_set_irn_register(irn, reg);
			}
			bitset_set(colors, col);

			DBG((dbg, LEVEL_1, "\tassigning register %s(%d) to %+F\n", reg->name, col, irn));

			assert(!bitset_is_set(live, nr) && "Value's definition must not have been encountered");
			bitset_set(live, nr);
		}
	}
}

static void be_ra_chordal_color(be_chordal_env_t *const chordal_env)
{
	char            buf[256];
	ir_graph *const irg = chordal_env->irg;
	be_assure_live_sets(irg);
	assure_doms(irg);

	arch_register_class_t const *const cls      = chordal_env->cls;
	int                          const colors_n = arch_register_class_n_regs(cls);
	be_chordal_alloc_env_t             env;
	env.chordal_env = chordal_env;
	env.colors      = bitset_alloca(colors_n);
	env.tmp_colors  = bitset_alloca(colors_n);

	be_timer_push(T_SPLIT);
	if (chordal_env->opts->dump_flags & BE_CH_DUMP_SPLIT) {
		snprintf(buf, sizeof(buf), "%s-split", cls->name);
		dump_ir_graph(irg, buf);
	}
	be_timer_pop(T_SPLIT);

	be_timer_push(T_CONSTR);

	/* Handle register targeting constraints */
	dom_tree_walk_irg(irg, constraints, NULL, &env);

	if (chordal_env->opts->dump_flags & BE_CH_DUMP_CONSTR) {
		snprintf(buf, sizeof(buf), "%s-constr", cls->name);
		dump_ir_graph(irg, buf);
	}

	be_timer_pop(T_CONSTR);

	env.live = bitset_malloc(get_irg_last_idx(irg));

	/* First, determine the pressure */
	dom_tree_walk_irg(irg, create_borders, NULL, chordal_env);

	/* Assign the colors */
	dom_tree_walk_irg(irg, assign, NULL, &env);

	if (chordal_env->opts->dump_flags & BE_CH_DUMP_TREE_INTV) {
		ir_snprintf(buf, sizeof(buf), "ifg_%s_%F.eps", cls->name, irg);
		plotter_t *const plotter = new_plotter_ps(buf);
		draw_interval_tree(&draw_chordal_def_opts, chordal_env, plotter);
		plotter_free(plotter);
	}

	bitset_free(env.live);
}

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_chordal)
void be_init_chordal(void)
{
	static be_ra_chordal_coloring_t coloring = {
		be_ra_chordal_color
	};
	FIRM_DBG_REGISTER(dbg, "firm.be.chordal");

	be_register_chordal_coloring("default", &coloring);
}
