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
 * @brief       Backend node support for generic backend nodes.
 * @author      Sebastian Hack
 * @date        17.05.2005
 * @version     $Id$
 *
 * Backend node support for generic backend nodes.
 * This file provides Perm, Copy, Spill and Reload nodes.
 */
#include "config.h"

#include <stdlib.h>

#include "obst.h"
#include "set.h"
#include "pmap.h"
#include "util.h"
#include "debug.h"
#include "fourcc.h"
#include "offset.h"
#include "bitfiddle.h"
#include "raw_bitset.h"
#include "error.h"
#include "array_t.h"

#include "irop_t.h"
#include "irmode_t.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "irprintf.h"
#include "irgwalk.h"
#include "iropt_t.h"

#include "be_t.h"
#include "belive_t.h"
#include "besched.h"
#include "benode.h"
#include "bearch.h"

#include "beirgmod.h"

#define get_irn_attr(irn) get_irn_generic_attr(irn)
#define get_irn_attr_const(irn) get_irn_generic_attr_const(irn)

typedef struct {
	const arch_register_req_t *in_req;
} be_reg_data_t;

/** The generic be nodes attribute type. */
typedef struct {
	be_reg_data_t *reg_data;
} be_node_attr_t;

/** The be_Return nodes attribute type. */
typedef struct {
	be_node_attr_t node_attr;     /**< base attributes of every be node. */
	int            num_ret_vals;  /**< number of return values */
	unsigned       pop;           /**< number of bytes that should be popped */
	int            emit_pop;      /**< if set, emit pop bytes, even if pop = 0 */
} be_return_attr_t;

/** The be_IncSP attribute type. */
typedef struct {
	be_node_attr_t node_attr;   /**< base attributes of every be node. */
	int            offset;      /**< The offset by which the stack shall be expanded/shrinked. */
	int            align;       /**< whether stack should be aligned after the
	                                 IncSP */
} be_incsp_attr_t;

/** The be_Frame attribute type. */
typedef struct {
	be_node_attr_t  node_attr;   /**< base attributes of every be node. */
	ir_entity      *ent;
	int             offset;
} be_frame_attr_t;

/** The be_Call attribute type. */
typedef struct {
	be_node_attr_t  node_attr;  /**< base attributes of every be node. */
	ir_entity      *ent;        /**< The called entity if this is a static call. */
	unsigned        pop;
	ir_type        *call_tp;    /**< The call type, copied from the original Call node. */
} be_call_attr_t;

typedef struct {
	be_node_attr_t   node_attr;  /**< base attributes of every be node. */
	ir_entity      **in_entities;
	ir_entity      **out_entities;
} be_memperm_attr_t;

ir_op *op_be_Spill;
ir_op *op_be_Reload;
ir_op *op_be_Perm;
ir_op *op_be_MemPerm;
ir_op *op_be_Copy;
ir_op *op_be_Keep;
ir_op *op_be_CopyKeep;
ir_op *op_be_Call;
ir_op *op_be_Return;
ir_op *op_be_IncSP;
ir_op *op_be_AddSP;
ir_op *op_be_SubSP;
ir_op *op_be_RegParams;
ir_op *op_be_FrameAddr;
ir_op *op_be_Barrier;

static const ir_op_ops be_node_op_ops;

#define N   irop_flag_none
#define L   irop_flag_labeled
#define C   irop_flag_commutative
#define X   irop_flag_cfopcode
#define I   irop_flag_ip_cfopcode
#define F   irop_flag_fragile
#define Y   irop_flag_forking
#define H   irop_flag_highlevel
#define c   irop_flag_constlike
#define K   irop_flag_keep
#define M   irop_flag_uses_memory

/**
 * Compare two be node attributes.
 *
 * @return zero if both attributes are identically
 */
static int node_cmp_attr(ir_node *a, ir_node *b)
{
	const be_node_attr_t *a_attr = get_irn_attr_const(a);
	const be_node_attr_t *b_attr = get_irn_attr_const(b);
	int i, len = ARR_LEN(a_attr->reg_data);

	if (len != ARR_LEN(b_attr->reg_data))
		return 1;

	if (!be_nodes_equal(a, b))
		return 1;

	for (i = len - 1; i >= 0; --i) {
		if (!reg_reqs_equal(a_attr->reg_data[i].in_req,
		                    b_attr->reg_data[i].in_req))
			return 1;
	}

	return 0;
}

/**
 * Compare the attributes of two be_FrameAddr nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int FrameAddr_cmp_attr(ir_node *a, ir_node *b)
{
	const be_frame_attr_t *a_attr = get_irn_attr_const(a);
	const be_frame_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->ent != b_attr->ent || a_attr->offset != b_attr->offset)
		return 1;

	return node_cmp_attr(a, b);
}

/**
 * Compare the attributes of two be_Return nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int Return_cmp_attr(ir_node *a, ir_node *b)
{
	const be_return_attr_t *a_attr = get_irn_attr_const(a);
	const be_return_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->num_ret_vals != b_attr->num_ret_vals)
		return 1;
	if (a_attr->pop != b_attr->pop)
		return 1;
	if (a_attr->emit_pop != b_attr->emit_pop)
		return 1;

	return node_cmp_attr(a, b);
}

/**
 * Compare the attributes of two be_IncSP nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int IncSP_cmp_attr(ir_node *a, ir_node *b) {
	const be_incsp_attr_t *a_attr = get_irn_attr_const(a);
	const be_incsp_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->offset != b_attr->offset)
		return 1;

	return node_cmp_attr(a, b);
}

/**
 * Compare the attributes of two be_Call nodes.
 *
 * @return zero if both nodes have identically attributes
 */
static int Call_cmp_attr(ir_node *a, ir_node *b)
{
	const be_call_attr_t *a_attr = get_irn_attr_const(a);
	const be_call_attr_t *b_attr = get_irn_attr_const(b);

	if (a_attr->ent != b_attr->ent ||
		a_attr->call_tp != b_attr->call_tp)
		return 1;

	return node_cmp_attr(a, b);
}

static arch_register_req_t *allocate_reg_req(const ir_node *node)
{
	ir_graph       *irg  = get_irn_irg(node);
	struct obstack *obst = be_get_birg_obst(irg);

	arch_register_req_t *req = obstack_alloc(obst, sizeof(*req));
	memset(req, 0, sizeof(*req));
	return req;
}

void be_set_constr_in(ir_node *node, int pos, const arch_register_req_t *req)
{
	const be_node_attr_t *attr = get_irn_attr_const(node);
	be_reg_data_t *rd = &attr->reg_data[pos];
	assert(pos < ARR_LEN(attr->reg_data));
	rd->in_req = req;
}

void be_set_constr_out(ir_node *node, int pos, const arch_register_req_t *req)
{
	backend_info_t *info = be_get_info(node);
	info->out_infos[pos].req = req;
}

/**
 * Initializes the generic attribute of all be nodes and return it.
 */
static void *init_node_attr(ir_node *node, int n_inputs, int n_outputs)
{
	ir_graph       *irg  = get_irn_irg(node);
	struct obstack *obst = be_get_birg_obst(irg);
	be_node_attr_t *a    = get_irn_attr(node);
	backend_info_t *info = be_get_info(node);

	memset(a, 0, sizeof(get_op_attr_size(get_irn_op(node))));

	if (n_inputs >= 0) {
		int i;
		a->reg_data = NEW_ARR_D(be_reg_data_t, obst, n_inputs);
		for (i = 0; i < n_inputs; ++i) {
			a->reg_data[i].in_req = arch_no_register_req;
		}
	} else {
		a->reg_data = NEW_ARR_F(be_reg_data_t, 0);
	}

	if (n_outputs >= 0) {
		int i;
		info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_outputs);
		memset(info->out_infos, 0, n_outputs * sizeof(info->out_infos[0]));
		for (i = 0; i < n_outputs; ++i) {
			info->out_infos[i].req = arch_no_register_req;
		}
	} else {
		info->out_infos = NEW_ARR_F(reg_out_info_t, 0);
	}

	return a;
}

static void add_register_req_out(ir_node *node)
{
	backend_info_t *info = be_get_info(node);
	reg_out_info_t  out_info;
	memset(&out_info, 0, sizeof(out_info));
	out_info.req = arch_no_register_req;
	ARR_APP1(reg_out_info_t, info->out_infos, out_info);
}

static void add_register_req_in(ir_node *node)
{
	be_node_attr_t *a    = get_irn_attr(node);
	be_reg_data_t   regreq;
	memset(&regreq, 0, sizeof(regreq));
	regreq.in_req = arch_no_register_req;
	ARR_APP1(be_reg_data_t, a->reg_data, regreq);
}

ir_node *be_new_Spill(const arch_register_class_t *cls,
		const arch_register_class_t *cls_frame, ir_node *bl,
		ir_node *frame, ir_node *to_spill)
{
	be_frame_attr_t *a;
	ir_node         *in[2];
	ir_node         *res;
	ir_graph        *irg = get_Block_irg(bl);

	in[0]     = frame;
	in[1]     = to_spill;
	res       = new_ir_node(NULL, irg, bl, op_be_Spill, mode_M, 2, in);
	a         = init_node_attr(res, 2, 1);
	a->ent    = NULL;
	a->offset = 0;

	be_node_set_reg_class_in(res, be_pos_Spill_frame, cls_frame);
	be_node_set_reg_class_in(res, be_pos_Spill_val, cls);

	/*
	 * For spills and reloads, we return "none" as requirement for frame
	 * pointer, so every input is ok. Some backends need this (e.g. STA).
	 * Matze: we should investigate if this is really needed, this solution
	 *        looks very hacky to me
	 */
	be_set_constr_in(res, be_pos_Spill_frame, arch_no_register_req);

	return res;
}

ir_node *be_new_Reload(const arch_register_class_t *cls,
		const arch_register_class_t *cls_frame, ir_node *block,
		ir_node *frame, ir_node *mem, ir_mode *mode)
{
	ir_node  *in[2];
	ir_node  *res;
	ir_graph *irg = get_Block_irg(block);

	in[0] = frame;
	in[1] = mem;
	res   = new_ir_node(NULL, irg, block, op_be_Reload, mode, 2, in);

	init_node_attr(res, 2, 2);
	be_node_set_reg_class_out(res, 0, cls);
	be_node_set_reg_class_in(res, be_pos_Reload_frame, cls_frame);
	arch_irn_set_flags(res, arch_irn_flags_rematerializable);

	/*
	 * For spills and reloads, we return "none" as requirement for frame
	 * pointer, so every input is ok. Some backends need this (e.g. STA).
	 * Matze: we should investigate if this is really needed, this solution
	 *        looks very hacky to me
	 */
	be_set_constr_in(res, be_pos_Reload_frame, arch_no_register_req);

	return res;
}

ir_node *be_get_Reload_mem(const ir_node *irn)
{
	assert(be_is_Reload(irn));
	return get_irn_n(irn, be_pos_Reload_mem);
}

ir_node *be_get_Reload_frame(const ir_node *irn)
{
	assert(be_is_Reload(irn));
	return get_irn_n(irn, be_pos_Reload_frame);
}

ir_node *be_get_Spill_val(const ir_node *irn)
{
	assert(be_is_Spill(irn));
	return get_irn_n(irn, be_pos_Spill_val);
}

ir_node *be_get_Spill_frame(const ir_node *irn)
{
	assert(be_is_Spill(irn));
	return get_irn_n(irn, be_pos_Spill_frame);
}

ir_node *be_new_Perm(const arch_register_class_t *cls, ir_node *block,
                     int n, ir_node *in[])
{
	int      i;
	ir_graph *irg = get_Block_irg(block);

	ir_node *irn = new_ir_node(NULL, irg, block, op_be_Perm, mode_T, n, in);
	init_node_attr(irn, n, n);
	for (i = 0; i < n; ++i) {
		be_node_set_reg_class_in(irn, i, cls);
		be_node_set_reg_class_out(irn, i, cls);
	}

	return irn;
}

void be_Perm_reduce(ir_node *perm, int new_size, int *map)
{
	int            arity      = get_irn_arity(perm);
	be_reg_data_t  *old_data  = ALLOCAN(be_reg_data_t, arity);
	reg_out_info_t *old_infos = ALLOCAN(reg_out_info_t, arity);
	be_node_attr_t *attr      = get_irn_attr(perm);
	backend_info_t *info      = be_get_info(perm);
	ir_node        **new_in;

	int i;

	assert(be_is_Perm(perm));
	assert(new_size <= arity);

	new_in = alloca(new_size * sizeof(*new_in));

	/* save the old register data */
	memcpy(old_data, attr->reg_data, arity * sizeof(old_data[0]));
	memcpy(old_infos, info->out_infos, arity * sizeof(old_infos[0]));

	/* compose the new in array and set the new register data directly in place */
	for (i = 0; i < new_size; ++i) {
		int idx = map[i];
		new_in[i]          = get_irn_n(perm, idx);
		attr->reg_data[i]  = old_data[idx];
		info->out_infos[i] = old_infos[idx];
	}

	set_irn_in(perm, new_size, new_in);
}

ir_node *be_new_MemPerm(const arch_env_t *arch_env, ir_node *bl, int n, ir_node *in[])
{
	ir_graph                     *irg       = get_Block_irg(bl);
	ir_node                      *frame     = get_irg_frame(irg);
	const arch_register_class_t  *cls_frame = arch_get_irn_reg_class_out(frame);
	const arch_register_t        *sp        = arch_env->sp;
	ir_node                      *irn;
	be_memperm_attr_t            *attr;
	ir_node                     **real_in;
	int                           i;

	real_in = ALLOCAN(ir_node*, n + 1);
	real_in[0] = frame;
	memcpy(&real_in[1], in, n * sizeof(real_in[0]));

	irn =  new_ir_node(NULL, irg, bl, op_be_MemPerm, mode_T, n+1, real_in);

	init_node_attr(irn, n + 1, n + 1);
	be_node_set_reg_class_in(irn, 0, sp->reg_class);
	for (i = 0; i < n; ++i) {
		be_node_set_reg_class_in(irn, i + 1, cls_frame);
		be_node_set_reg_class_out(irn, i, cls_frame);
	}

	attr = get_irn_attr(irn);
	attr->in_entities  = OALLOCNZ(irg->obst, ir_entity*, n);
	attr->out_entities = OALLOCNZ(irg->obst, ir_entity*, n);

	return irn;
}

ir_node *be_new_Copy(const arch_register_class_t *cls, ir_node *bl, ir_node *op)
{
	ir_node *in[1];
	ir_node *res;
	arch_register_req_t *req;
	ir_graph *irg = get_Block_irg(bl);

	in[0] = op;
	res   = new_ir_node(NULL, irg, bl, op_be_Copy, get_irn_mode(op), 1, in);
	init_node_attr(res, 1, 1);
	be_node_set_reg_class_in(res, 0, cls);
	be_node_set_reg_class_out(res, 0, cls);

	req = allocate_reg_req(res);
	req->cls        = cls;
	req->type       = arch_register_req_type_should_be_same;
	req->other_same = 1U << 0;
	be_set_constr_out(res, 0, req);

	return res;
}

ir_node *be_get_Copy_op(const ir_node *cpy) {
	return get_irn_n(cpy, be_pos_Copy_op);
}

void be_set_Copy_op(ir_node *cpy, ir_node *op) {
	set_irn_n(cpy, be_pos_Copy_op, op);
}

ir_node *be_new_Keep(ir_node *block, int n, ir_node *in[])
{
	int i;
	ir_node *res;
	ir_graph *irg = get_Block_irg(block);

	res = new_ir_node(NULL, irg, block, op_be_Keep, mode_ANY, -1, NULL);
	init_node_attr(res, -1, 1);

	for (i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req_in(res);
	}
	keep_alive(res);

	return res;
}

void be_Keep_add_node(ir_node *keep, const arch_register_class_t *cls, ir_node *node)
{
	int n;

	assert(be_is_Keep(keep));
	n = add_irn_n(keep, node);
	add_register_req_in(keep);
	be_node_set_reg_class_in(keep, n, cls);
}

/* creates a be_Call */
ir_node *be_new_Call(dbg_info *dbg, ir_graph *irg, ir_node *bl, ir_node *mem,
		ir_node *sp, ir_node *ptr, int n_outs, int n, ir_node *in[],
		ir_type *call_tp)
{
	be_call_attr_t *a;
	int real_n = be_pos_Call_first_arg + n;
	ir_node *irn;
	ir_node **real_in;

	NEW_ARR_A(ir_node *, real_in, real_n);
	real_in[be_pos_Call_mem] = mem;
	real_in[be_pos_Call_sp]  = sp;
	real_in[be_pos_Call_ptr] = ptr;
	memcpy(&real_in[be_pos_Call_first_arg], in, n * sizeof(in[0]));

	irn = new_ir_node(dbg, irg, bl, op_be_Call, mode_T, real_n, real_in);
	a = init_node_attr(irn, real_n, n_outs);
	a->ent     = NULL;
	a->call_tp = call_tp;
	a->pop     = 0;
	return irn;
}

/* Gets the call entity or NULL if this is no static call. */
ir_entity *be_Call_get_entity(const ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	assert(be_is_Call(call));
	return a->ent;
}

/* Sets the call entity. */
void be_Call_set_entity(ir_node *call, ir_entity *ent) {
	be_call_attr_t *a = get_irn_attr(call);
	assert(be_is_Call(call));
	a->ent = ent;
}

/* Gets the call type. */
ir_type *be_Call_get_type(ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	assert(be_is_Call(call));
	return a->call_tp;
}

/* Sets the call type. */
void be_Call_set_type(ir_node *call, ir_type *call_tp) {
	be_call_attr_t *a = get_irn_attr(call);
	assert(be_is_Call(call));
	a->call_tp = call_tp;
}

void be_Call_set_pop(ir_node *call, unsigned pop) {
	be_call_attr_t *a = get_irn_attr(call);
	a->pop = pop;
}

unsigned be_Call_get_pop(const ir_node *call) {
	const be_call_attr_t *a = get_irn_attr_const(call);
	return a->pop;
}

/* Construct a new be_Return. */
ir_node *be_new_Return(dbg_info *dbg, ir_graph *irg, ir_node *block, int n_res,
                       unsigned pop, int n, ir_node *in[])
{
	be_return_attr_t *a;
	ir_node *res;
	int i;

	res = new_ir_node(dbg, irg, block, op_be_Return, mode_X, -1, NULL);
	init_node_attr(res, -1, 1);
	for (i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req_in(res);
	}
	be_set_constr_out(res, 0, arch_no_register_req);

	a = get_irn_attr(res);
	a->num_ret_vals = n_res;
	a->pop          = pop;
	a->emit_pop     = 0;

	return res;
}

/* Returns the number of real returns values */
int be_Return_get_n_rets(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->num_ret_vals;
}

/* return the number of bytes that should be popped from stack when executing the Return. */
unsigned be_Return_get_pop(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->pop;
}

/* return non-zero, if number of popped bytes must be always emitted */
int be_Return_get_emit_pop(const ir_node *ret) {
	const be_return_attr_t *a = get_irn_generic_attr_const(ret);
	return a->emit_pop;
}

/* return non-zero, if number of popped bytes must be always emitted */
void be_Return_set_emit_pop(ir_node *ret, int emit_pop) {
	be_return_attr_t *a = get_irn_generic_attr(ret);
	a->emit_pop = emit_pop;
}

int be_Return_append_node(ir_node *ret, ir_node *node) {
	int pos;

	pos = add_irn_n(ret, node);
	add_register_req_in(ret);

	return pos;
}

ir_node *be_new_IncSP(const arch_register_t *sp, ir_node *bl,
                      ir_node *old_sp, int offset, int align)
{
	be_incsp_attr_t *a;
	ir_node *irn;
	ir_node *in[1];
	ir_graph *irg = get_Block_irg(bl);

	in[0]     = old_sp;
	irn       = new_ir_node(NULL, irg, bl, op_be_IncSP, sp->reg_class->mode,
	                        sizeof(in) / sizeof(in[0]), in);
	a         = init_node_attr(irn, 1, 1);
	a->offset = offset;
	a->align  = align;

	/* Set output constraint to stack register. */
	be_node_set_reg_class_in(irn, 0, sp->reg_class);
	be_set_constr_single_reg_out(irn, 0, sp, arch_register_req_type_produces_sp);

	return irn;
}

ir_node *be_new_AddSP(const arch_register_t *sp, ir_node *bl, ir_node *old_sp,
		ir_node *sz)
{
	be_node_attr_t *a;
	ir_node *irn;
	ir_node *in[be_pos_AddSP_last];
	const arch_register_class_t *cls;
	ir_graph *irg;

	in[be_pos_AddSP_old_sp] = old_sp;
	in[be_pos_AddSP_size]   = sz;

	irg = get_Block_irg(bl);
	irn = new_ir_node(NULL, irg, bl, op_be_AddSP, mode_T, be_pos_AddSP_last, in);
	a   = init_node_attr(irn, be_pos_AddSP_last, pn_be_AddSP_last);

	/* Set output constraint to stack register. */
	be_set_constr_single_reg_in(irn, be_pos_AddSP_old_sp, sp, 0);
	be_node_set_reg_class_in(irn, be_pos_AddSP_size, arch_register_get_class(sp));
	be_set_constr_single_reg_out(irn, pn_be_AddSP_sp, sp, arch_register_req_type_produces_sp);

	cls = arch_register_get_class(sp);

	return irn;
}

ir_node *be_new_SubSP(const arch_register_t *sp, ir_node *bl, ir_node *old_sp, ir_node *sz)
{
	be_node_attr_t *a;
	ir_node *irn;
	ir_node *in[be_pos_SubSP_last];
	ir_graph *irg;

	in[be_pos_SubSP_old_sp] = old_sp;
	in[be_pos_SubSP_size]   = sz;

	irg = get_Block_irg(bl);
	irn = new_ir_node(NULL, irg, bl, op_be_SubSP, mode_T, be_pos_SubSP_last, in);
	a   = init_node_attr(irn, be_pos_SubSP_last, pn_be_SubSP_last);

	/* Set output constraint to stack register. */
	be_set_constr_single_reg_in(irn, be_pos_SubSP_old_sp, sp, 0);
	be_node_set_reg_class_in(irn, be_pos_SubSP_size, arch_register_get_class(sp));
	be_set_constr_single_reg_out(irn, pn_be_SubSP_sp, sp, arch_register_req_type_produces_sp);

	return irn;
}

ir_node *be_new_RegParams(ir_node *bl, int n_outs)
{
	ir_node *res;
	int i;
	ir_graph *irg = get_Block_irg(bl);

	res = new_ir_node(NULL, irg, bl, op_be_RegParams, mode_T, 0, NULL);
	init_node_attr(res, 0, -1);
	for (i = 0; i < n_outs; ++i) {
		add_register_req_out(res);
	}

	return res;
}

ir_node *be_new_FrameAddr(const arch_register_class_t *cls_frame, ir_node *bl, ir_node *frame, ir_entity *ent)
{
	be_frame_attr_t *a;
	ir_node *irn;
	ir_node *in[1];
	ir_graph *irg = get_Block_irg(bl);

	in[0]  = frame;
	irn    = new_ir_node(NULL, irg, bl, op_be_FrameAddr, get_irn_mode(frame), 1, in);
	a      = init_node_attr(irn, 1, 1);
	a->ent = ent;
	a->offset = 0;
	be_node_set_reg_class_in(irn, 0, cls_frame);
	be_node_set_reg_class_out(irn, 0, cls_frame);

	return optimize_node(irn);
}

ir_node *be_get_FrameAddr_frame(const ir_node *node) {
	assert(be_is_FrameAddr(node));
	return get_irn_n(node, be_pos_FrameAddr_ptr);
}

ir_entity *be_get_FrameAddr_entity(const ir_node *node)
{
	const be_frame_attr_t *attr = get_irn_generic_attr_const(node);
	return attr->ent;
}

ir_node *be_new_CopyKeep(const arch_register_class_t *cls, ir_node *bl, ir_node *src, int n, ir_node *in_keep[], ir_mode *mode)
{
	ir_node  *irn;
	ir_node **in = ALLOCAN(ir_node*, n + 1);
	ir_graph *irg = get_Block_irg(bl);

	in[0] = src;
	memcpy(&in[1], in_keep, n * sizeof(in[0]));
	irn   = new_ir_node(NULL, irg, bl, op_be_CopyKeep, mode, n + 1, in);
	init_node_attr(irn, n + 1, 1);
	be_node_set_reg_class_in(irn, 0, cls);
	be_node_set_reg_class_out(irn, 0, cls);

	return irn;
}

ir_node *be_new_CopyKeep_single(const arch_register_class_t *cls, ir_node *bl, ir_node *src, ir_node *keep, ir_mode *mode)
{
	return be_new_CopyKeep(cls, bl, src, 1, &keep, mode);
}

ir_node *be_get_CopyKeep_op(const ir_node *cpy) {
	return get_irn_n(cpy, be_pos_CopyKeep_op);
}

void be_set_CopyKeep_op(ir_node *cpy, ir_node *op) {
	set_irn_n(cpy, be_pos_CopyKeep_op, op);
}

ir_node *be_new_Barrier(ir_node *bl, int n, ir_node *in[])
{
	ir_node *res;
	int i;
	ir_graph *irg = get_Block_irg(bl);

	res = new_ir_node(NULL, irg, bl, op_be_Barrier, mode_T, -1, NULL);
	init_node_attr(res, -1, -1);
	for (i = 0; i < n; ++i) {
		add_irn_n(res, in[i]);
		add_register_req_in(res);
		add_register_req_out(res);
	}

	return res;
}

ir_node *be_Barrier_append_node(ir_node *barrier, ir_node *node)
{
	ir_node *block = get_nodes_block(barrier);
	ir_mode *mode = get_irn_mode(node);
	int n = add_irn_n(barrier, node);

	ir_node *proj = new_r_Proj(block, barrier, mode, n);
	add_register_req_in(barrier);
	add_register_req_out(barrier);

	return proj;
}

int be_has_frame_entity(const ir_node *irn)
{
	switch (get_irn_opcode(irn)) {
	case beo_Spill:
	case beo_Reload:
	case beo_FrameAddr:
		return 1;
	default:
		return 0;
	}
}

ir_entity *be_get_frame_entity(const ir_node *irn)
{
	if (be_has_frame_entity(irn)) {
		const be_frame_attr_t *a = get_irn_attr_const(irn);
		return a->ent;
	}
	return NULL;
}

int be_get_frame_offset(const ir_node *irn)
{
	assert(is_be_node(irn));
	if (be_has_frame_entity(irn)) {
		const be_frame_attr_t *a = get_irn_attr_const(irn);
		return a->offset;
	}
	return 0;
}

void be_set_MemPerm_in_entity(const ir_node *irn, int n, ir_entity *ent)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	attr->in_entities[n] = ent;
}

ir_entity* be_get_MemPerm_in_entity(const ir_node* irn, int n)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	return attr->in_entities[n];
}

void be_set_MemPerm_out_entity(const ir_node *irn, int n, ir_entity *ent)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	attr->out_entities[n] = ent;
}

ir_entity* be_get_MemPerm_out_entity(const ir_node* irn, int n)
{
	const be_memperm_attr_t *attr = get_irn_attr_const(irn);

	assert(be_is_MemPerm(irn));
	assert(n < be_get_MemPerm_entity_arity(irn));

	return attr->out_entities[n];
}

int be_get_MemPerm_entity_arity(const ir_node *irn)
{
	return get_irn_arity(irn) - 1;
}

static const arch_register_req_t *get_single_req(struct obstack *obst,
		const arch_register_t *reg, arch_register_req_type_t additional_types)
{
	arch_register_req_t         *req = obstack_alloc(obst, sizeof(*req));
	const arch_register_class_t *cls = arch_register_get_class(reg);
	unsigned                    *limited_bitset;

	limited_bitset = rbitset_obstack_alloc(obst, arch_register_class_n_regs(cls));
	rbitset_set(limited_bitset, arch_register_get_index(reg));

	req->type    = arch_register_req_type_limited | additional_types;
	req->cls     = cls;
	req->limited = limited_bitset;
	return req;
}

void be_set_constr_single_reg_in(ir_node *node, int pos,
		const arch_register_t *reg, arch_register_req_type_t additional_types)
{
	const arch_register_req_t *req;

	if (additional_types == 0) {
		req = reg->single_req;
	} else {
		ir_graph       *irg  = get_irn_irg(node);
		struct obstack *obst = be_get_birg_obst(irg);
		req = get_single_req(obst, reg, additional_types);
	}
	be_set_constr_in(node, pos, req);
}

void be_set_constr_single_reg_out(ir_node *node, int pos,
		const arch_register_t *reg, arch_register_req_type_t additional_types)
{
	const arch_register_req_t *req;

	/* if we have an ignore register, add ignore flag and just assign it */
	if (reg->type & arch_register_type_ignore) {
		additional_types |= arch_register_req_type_ignore;
	}

	if (additional_types == 0) {
		req = reg->single_req;
	} else {
		ir_graph       *irg  = get_irn_irg(node);
		struct obstack *obst = be_get_birg_obst(irg);
		req = get_single_req(obst, reg, additional_types);
	}

	arch_irn_set_register(node, pos, reg);
	be_set_constr_out(node, pos, req);
}

void be_node_set_reg_class_in(ir_node *irn, int pos,
                              const arch_register_class_t *cls)
{
	be_set_constr_in(irn, pos, cls->class_req);
}

void be_node_set_reg_class_out(ir_node *irn, int pos,
                               const arch_register_class_t *cls)
{
	be_set_constr_out(irn, pos, cls->class_req);
}

ir_node *be_get_IncSP_pred(ir_node *irn) {
	assert(be_is_IncSP(irn));
	return get_irn_n(irn, 0);
}

void be_set_IncSP_pred(ir_node *incsp, ir_node *pred) {
	assert(be_is_IncSP(incsp));
	set_irn_n(incsp, 0, pred);
}

void be_set_IncSP_offset(ir_node *irn, int offset)
{
	be_incsp_attr_t *a = get_irn_attr(irn);
	assert(be_is_IncSP(irn));
	a->offset = offset;
}

int be_get_IncSP_offset(const ir_node *irn)
{
	const be_incsp_attr_t *a = get_irn_attr_const(irn);
	assert(be_is_IncSP(irn));
	return a->offset;
}

int be_get_IncSP_align(const ir_node *irn)
{
	const be_incsp_attr_t *a = get_irn_attr_const(irn);
	assert(be_is_IncSP(irn));
	return a->align;
}

ir_node *be_spill(ir_node *block, ir_node *irn)
{
	ir_graph                    *irg       = get_Block_irg(block);
	ir_node                     *frame     = get_irg_frame(irg);
	const arch_register_class_t *cls       = arch_get_irn_reg_class_out(irn);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class_out(frame);
	ir_node                     *spill;

	spill = be_new_Spill(cls, cls_frame, block, frame, irn);
	return spill;
}

ir_node *be_reload(const arch_register_class_t *cls, ir_node *insert, ir_mode *mode, ir_node *spill)
{
	ir_node  *reload;
	ir_node  *bl    = is_Block(insert) ? insert : get_nodes_block(insert);
	ir_graph *irg   = get_Block_irg(bl);
	ir_node  *frame = get_irg_frame(irg);
	const arch_register_class_t *cls_frame = arch_get_irn_reg_class_out(frame);

	assert(be_is_Spill(spill) || (is_Phi(spill) && get_irn_mode(spill) == mode_M));

	reload = be_new_Reload(cls, cls_frame, bl, frame, spill, mode);

	if (is_Block(insert)) {
		insert = sched_skip(insert, 0, sched_skip_cf_predicator, NULL);
		sched_add_after(insert, reload);
	} else {
		sched_add_before(insert, reload);
	}

	return reload;
}

/*
  ____              ____
 |  _ \ ___  __ _  |  _ \ ___  __ _ ___
 | |_) / _ \/ _` | | |_) / _ \/ _` / __|
 |  _ <  __/ (_| | |  _ <  __/ (_| \__ \
 |_| \_\___|\__, | |_| \_\___|\__, |___/
            |___/                |_|

*/


static const arch_register_req_t *be_node_get_out_reg_req(
		const ir_node *irn, int pos)
{
	const backend_info_t *info = be_get_info(irn);
	assert(pos < ARR_LEN(info->out_infos));
	return info->out_infos[pos].req;
}

static const arch_register_req_t *be_node_get_in_reg_req(
		const ir_node *irn, int pos)
{
	const be_node_attr_t *a = get_irn_attr_const(irn);

	assert(pos >= 0);
	if (pos >= get_irn_arity(irn) || pos >= ARR_LEN(a->reg_data))
		return arch_no_register_req;

	return a->reg_data[pos].in_req;
}

static arch_irn_class_t be_node_classify(const ir_node *irn)
{
	switch (get_irn_opcode(irn)) {
		case beo_Spill:  return arch_irn_class_spill;
		case beo_Reload: return arch_irn_class_reload;
		case beo_Perm:   return arch_irn_class_perm;
		case beo_Copy:   return arch_irn_class_copy;
		default:         return 0;
	}
}

static ir_entity *be_node_get_frame_entity(const ir_node *irn)
{
	return be_get_frame_entity(irn);
}

static void be_node_set_frame_entity(ir_node *irn, ir_entity *ent)
{
	be_frame_attr_t *a;

	assert(be_has_frame_entity(irn));

	a = get_irn_attr(irn);
	a->ent = ent;
}

static void be_node_set_frame_offset(ir_node *irn, int offset)
{
	be_frame_attr_t *a;

	if (!be_has_frame_entity(irn))
		return;

	a = get_irn_attr(irn);
	a->offset = offset;
}

static int be_node_get_sp_bias(const ir_node *irn)
{
	if (be_is_IncSP(irn))
		return be_get_IncSP_offset(irn);
	if (be_is_Call(irn))
		return -(int)be_Call_get_pop(irn);

	return 0;
}

/*
  ___ ____  _   _   _   _                 _ _
 |_ _|  _ \| \ | | | | | | __ _ _ __   __| | | ___ _ __
  | || |_) |  \| | | |_| |/ _` | '_ \ / _` | |/ _ \ '__|
  | ||  _ <| |\  | |  _  | (_| | | | | (_| | |  __/ |
 |___|_| \_\_| \_| |_| |_|\__,_|_| |_|\__,_|_|\___|_|

*/

/* for be nodes */
static const arch_irn_ops_t be_node_irn_ops = {
	be_node_get_in_reg_req,
	be_node_get_out_reg_req,
	be_node_classify,
	be_node_get_frame_entity,
	be_node_set_frame_entity,
	be_node_set_frame_offset,
	be_node_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

static const arch_register_req_t *dummy_reg_req(
		const ir_node *node, int pos)
{
	(void) node;
	(void) pos;
	return arch_no_register_req;
}

static arch_irn_class_t dummy_classify(const ir_node *node)
{
	(void) node;
	return 0;
}

static ir_entity* dummy_get_frame_entity(const ir_node *node)
{
	(void) node;
	return NULL;
}

static void dummy_set_frame_entity(ir_node *node, ir_entity *entity)
{
	(void) node;
	(void) entity;
	panic("dummy_set_frame_entity() should not be called");
}

static void dummy_set_frame_offset(ir_node *node, int bias)
{
	(void) node;
	(void) bias;
	panic("dummy_set_frame_offset() should not be called");
}

static int dummy_get_sp_bias(const ir_node *node)
{
	(void) node;
	return 0;
}

/* for "middleend" nodes */
static const arch_irn_ops_t dummy_be_irn_ops = {
	dummy_reg_req,
	dummy_reg_req,
	dummy_classify,
	dummy_get_frame_entity,
	dummy_set_frame_entity,
	dummy_set_frame_offset,
	dummy_get_sp_bias,
	NULL,      /* get_inverse           */
	NULL,      /* get_op_estimated_cost */
	NULL,      /* possible_memory_operand */
	NULL,      /* perform_memory_operand */
};

/*
  ____  _     _   ___ ____  _   _   _   _                 _ _
 |  _ \| |__ (_) |_ _|  _ \| \ | | | | | | __ _ _ __   __| | | ___ _ __
 | |_) | '_ \| |  | || |_) |  \| | | |_| |/ _` | '_ \ / _` | |/ _ \ '__|
 |  __/| | | | |  | ||  _ <| |\  | |  _  | (_| | | | | (_| | |  __/ |
 |_|   |_| |_|_| |___|_| \_\_| \_| |_| |_|\__,_|_| |_|\__,_|_|\___|_|

*/

/**
 * Guess correct register class of a phi node by looking at its arguments
 */
static const arch_register_req_t *get_Phi_reg_req_recursive(const ir_node *phi,
                                                            pset **visited)
{
	int n = get_irn_arity(phi);
	ir_node *op;
	int i;

	if (*visited && pset_find_ptr(*visited, phi))
		return NULL;

	for (i = 0; i < n; ++i) {
		op = get_irn_n(phi, i);
		/* Matze: don't we unnecessary constraint our phis with this?
		 * we only need to take the regclass IMO*/
		if (!is_Phi(op))
			return arch_get_register_req_out(op);
	}

	/*
	 * The operands of that Phi were all Phis themselves.
	 * We have to start a DFS for a non-Phi argument now.
	 */
	if (!*visited)
		*visited = pset_new_ptr(16);

	pset_insert_ptr(*visited, phi);

	for (i = 0; i < n; ++i) {
		const arch_register_req_t *req;
		op = get_irn_n(phi, i);
		req = get_Phi_reg_req_recursive(op, visited);
		if (req != NULL)
			return req;
	}

	return NULL;
}

static const arch_register_req_t *phi_get_irn_reg_req(const ir_node *node,
                                                      int pos)
{
	backend_info_t            *info = be_get_info(node);
	const arch_register_req_t *req  = info->out_infos[0].req;
	(void) pos;

	if (req == NULL) {
		if (!mode_is_datab(get_irn_mode(node))) {
			req = arch_no_register_req;
		} else {
			pset *visited = NULL;

			req = get_Phi_reg_req_recursive(node, &visited);
			assert(req->cls != NULL);
			req = req->cls->class_req;

			if (visited != NULL)
				del_pset(visited);
		}
		info->out_infos[0].req = req;
	}

	return req;
}

void be_set_phi_reg_req(ir_node *node, const arch_register_req_t *req)
{
	backend_info_t *info = be_get_info(node);
	info->out_infos[0].req = req;

	assert(mode_is_datab(get_irn_mode(node)));
}

int be_dump_phi_reg_reqs(ir_node *node, FILE *F, dump_reason_t reason)
{
	backend_info_t *info;
	int i;
	int arity;

	switch(reason) {
	case dump_node_opcode_txt:
		fputs(get_op_name(get_irn_op(node)), F);
		break;
	case dump_node_mode_txt:
		fprintf(F, "%s", get_mode_name(get_irn_mode(node)));
		break;
	case dump_node_nodeattr_txt:
		break;
	case dump_node_info_txt:
		info = be_get_info(node);

		/* we still have a little problem with the initialisation order. This
		   dump function is attached to the Phi ops before we can be sure
		   that all backend infos have been constructed... */
		if (info != NULL) {
			const arch_register_req_t *req = info->out_infos[0].req;
			const arch_register_t     *reg = arch_irn_get_register(node, 0);

			arity = get_irn_arity(node);
			for (i = 0; i < arity; ++i) {
				fprintf(F, "inreq #%d = ", i);
				arch_dump_register_req(F, req, node);
				fputs("\n", F);
			}
			fprintf(F, "outreq #0 = ");
			arch_dump_register_req(F, req, node);
			fputs("\n", F);

			fputs("\n", F);

			fprintf(F, "reg #0 = %s\n", reg != NULL ? reg->name : "n/a");
		}

		break;

	default:
		break;
	}

	return 0;
}

static const arch_irn_ops_t phi_irn_ops = {
	phi_get_irn_reg_req,
	phi_get_irn_reg_req,
	dummy_classify,
	dummy_get_frame_entity,
	dummy_set_frame_entity,
	dummy_set_frame_offset,
	dummy_get_sp_bias,
	NULL,    /* get_inverse             */
	NULL,    /* get_op_estimated_cost   */
	NULL,    /* possible_memory_operand */
	NULL,    /* perform_memory_operand  */
};

/*
  _   _           _        ____                        _
 | \ | | ___   __| | ___  |  _ \ _   _ _ __ ___  _ __ (_)_ __   __ _
 |  \| |/ _ \ / _` |/ _ \ | | | | | | | '_ ` _ \| '_ \| | '_ \ / _` |
 | |\  | (_) | (_| |  __/ | |_| | |_| | | | | | | |_) | | | | | (_| |
 |_| \_|\___/ \__,_|\___| |____/ \__,_|_| |_| |_| .__/|_|_| |_|\__, |
                                                |_|            |___/
*/

/**
 * Dumps node register requirements to a file.
 */
static void dump_node_reqs(FILE *F, ir_node *node)
{
	int i;
	be_node_attr_t *a = get_irn_attr(node);
	int len = ARR_LEN(a->reg_data);
	const backend_info_t *info = be_get_info(node);

	for (i = 0; i < len; ++i) {
		const arch_register_req_t *req = a->reg_data[i].in_req;
		if (req->cls == NULL)
			continue;
		fprintf(F, "inreq #%d = ", i);
		arch_dump_register_req(F, req, node);
		fputs("\n", F);
	}

	for (i = 0; i < len; ++i) {
		const arch_register_req_t *req = info->out_infos[i].req;
		if (req->cls == NULL)
			continue;
		fprintf(F, "outreq #%d = ", i);
		arch_dump_register_req(F, req, node);
		fputs("\n", F);
	}

	fputs("\n", F);

	for (i = 0; i < len; ++i) {
		const arch_register_t *reg = arch_irn_get_register(node, i);
		fprintf(F, "reg #%d = %s\n", i, reg != NULL ? reg->name : "n/a");
	}
}

/**
 * ir_op-Operation: dump a be node to file
 */
static int dump_node(ir_node *irn, FILE *f, dump_reason_t reason)
{
	be_node_attr_t *at = get_irn_attr(irn);

	assert(is_be_node(irn));

	switch(reason) {
		case dump_node_opcode_txt:
			fputs(get_op_name(get_irn_op(irn)), f);
			break;
		case dump_node_mode_txt:
			if (be_is_Perm(irn) || be_is_Copy(irn) || be_is_CopyKeep(irn)) {
				fprintf(f, " %s", get_mode_name(get_irn_mode(irn)));
			}
			break;
		case dump_node_nodeattr_txt:
			if (be_is_Call(irn)) {
				be_call_attr_t *a = (be_call_attr_t *) at;
				if (a->ent)
					fprintf(f, " [%s] ", get_entity_name(a->ent));
			}
			if (be_is_IncSP(irn)) {
				const be_incsp_attr_t *attr = get_irn_generic_attr_const(irn);
				if (attr->offset == BE_STACK_FRAME_SIZE_EXPAND) {
					fprintf(f, " [Setup Stackframe] ");
				} else if (attr->offset == BE_STACK_FRAME_SIZE_SHRINK) {
					fprintf(f, " [Destroy Stackframe] ");
				} else {
					fprintf(f, " [%d] ", attr->offset);
				}
			}
			break;
		case dump_node_info_txt:
			dump_node_reqs(f, irn);

			if (be_has_frame_entity(irn)) {
				be_frame_attr_t *a = (be_frame_attr_t *) at;
				if (a->ent) {
					unsigned size = get_type_size_bytes(get_entity_type(a->ent));
					ir_fprintf(f, "frame entity: %+F, offset 0x%x (%d), size 0x%x (%d) bytes\n",
					  a->ent, a->offset, a->offset, size, size);
				}

			}

			switch (get_irn_opcode(irn)) {
			case beo_IncSP:
				{
					be_incsp_attr_t *a = (be_incsp_attr_t *) at;
					if (a->offset == BE_STACK_FRAME_SIZE_EXPAND)
						fprintf(f, "offset: FRAME_SIZE\n");
					else if (a->offset == BE_STACK_FRAME_SIZE_SHRINK)
						fprintf(f, "offset: -FRAME SIZE\n");
					else
						fprintf(f, "offset: %u\n", a->offset);
				}
				break;
			case beo_Call:
				{
					be_call_attr_t *a = (be_call_attr_t *) at;

					if (a->ent)
						fprintf(f, "\ncalling: %s\n", get_entity_name(a->ent));
				}
				break;
			case beo_MemPerm:
				{
					int i;
					for (i = 0; i < be_get_MemPerm_entity_arity(irn); ++i) {
						ir_entity *in, *out;
						in = be_get_MemPerm_in_entity(irn, i);
						out = be_get_MemPerm_out_entity(irn, i);
						if (in) {
							fprintf(f, "\nin[%d]: %s\n", i, get_entity_name(in));
						}
						if (out) {
							fprintf(f, "\nout[%d]: %s\n", i, get_entity_name(out));
						}
					}
				}
				break;

			default:
				break;
			}
	}

	return 0;
}

/**
 * ir_op-Operation:
 * Copies the backend specific attributes from old node to new node.
 */
static void copy_attr(const ir_node *old_node, ir_node *new_node)
{
	const be_node_attr_t *old_attr = get_irn_attr_const(old_node);
	be_node_attr_t *new_attr = get_irn_attr(new_node);
	ir_graph       *irg      = get_irn_irg(new_node);
	struct obstack *obst     = be_get_birg_obst(irg);
	backend_info_t *old_info = be_get_info(old_node);
	backend_info_t *new_info = be_get_info(new_node);

	assert(is_be_node(old_node));
	assert(is_be_node(new_node));

	memcpy(new_attr, old_attr, get_op_attr_size(get_irn_op(old_node)));

	if (old_info->out_infos != NULL) {
		unsigned n_outs = ARR_LEN(old_info->out_infos);
		/* need dyanmic out infos? */
		if (be_is_RegParams(new_node) || be_is_Barrier(new_node)
				|| be_is_Perm(new_node)) {
			new_info->out_infos = NEW_ARR_F(reg_out_info_t, n_outs);
		} else {
			new_info->out_infos = NEW_ARR_D(reg_out_info_t, obst, n_outs);
		}
		memcpy(new_info->out_infos, old_info->out_infos,
			   n_outs * sizeof(new_info->out_infos[0]));
	} else {
		new_info->out_infos = NULL;
	}

	/* input infos */
	if (old_attr->reg_data != NULL) {
		unsigned n_ins = ARR_LEN(old_attr->reg_data);
		/* need dynamic in infos? */
		if (get_irn_op(old_node)->opar == oparity_dynamic) {
			new_attr->reg_data = NEW_ARR_F(be_reg_data_t, n_ins);
		} else {
			new_attr->reg_data = NEW_ARR_D(be_reg_data_t, obst, n_ins);
		}
		memcpy(new_attr->reg_data, old_attr->reg_data,
		       n_ins * sizeof(be_reg_data_t));
	} else {
		new_attr->reg_data = NULL;
	}
}

static const ir_op_ops be_node_op_ops = {
	firm_default_hash,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	copy_attr,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	dump_node,
	NULL,
	&be_node_irn_ops
};

int is_be_node(const ir_node *irn)
{
	return get_op_ops(get_irn_op(irn))->be_ops == &be_node_irn_ops;
}

void be_init_op(void)
{
	ir_opcode opc;

	/* Acquire all needed opcodes. */
	op_be_Spill      = new_ir_op(beo_Spill,     "be_Spill",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Reload     = new_ir_op(beo_Reload,    "be_Reload",    op_pin_state_pinned, N,   oparity_zero,     0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Perm       = new_ir_op(beo_Perm,      "be_Perm",      op_pin_state_pinned, N,   oparity_variable, 0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_MemPerm    = new_ir_op(beo_MemPerm,   "be_MemPerm",   op_pin_state_pinned, N,   oparity_variable, 0, sizeof(be_memperm_attr_t), &be_node_op_ops);
	op_be_Copy       = new_ir_op(beo_Copy,      "be_Copy",      op_pin_state_floats, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_Keep       = new_ir_op(beo_Keep,      "be_Keep",      op_pin_state_floats, K,   oparity_dynamic,  0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_CopyKeep   = new_ir_op(beo_CopyKeep,  "be_CopyKeep",  op_pin_state_floats, K,   oparity_variable, 0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_Call       = new_ir_op(beo_Call,      "be_Call",      op_pin_state_pinned, F|M, oparity_variable, 0, sizeof(be_call_attr_t),    &be_node_op_ops);
	op_be_Return     = new_ir_op(beo_Return,    "be_Return",    op_pin_state_pinned, X,   oparity_dynamic,  0, sizeof(be_return_attr_t),  &be_node_op_ops);
	op_be_AddSP      = new_ir_op(beo_AddSP,     "be_AddSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_SubSP      = new_ir_op(beo_SubSP,     "be_SubSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_IncSP      = new_ir_op(beo_IncSP,     "be_IncSP",     op_pin_state_pinned, N,   oparity_unary,    0, sizeof(be_incsp_attr_t),   &be_node_op_ops);
	op_be_RegParams  = new_ir_op(beo_RegParams, "be_RegParams", op_pin_state_pinned, N,   oparity_zero,     0, sizeof(be_node_attr_t),    &be_node_op_ops);
	op_be_FrameAddr  = new_ir_op(beo_FrameAddr, "be_FrameAddr", op_pin_state_floats, N,   oparity_unary,    0, sizeof(be_frame_attr_t),   &be_node_op_ops);
	op_be_Barrier    = new_ir_op(beo_Barrier,   "be_Barrier",   op_pin_state_pinned, N,   oparity_dynamic,  0, sizeof(be_node_attr_t),    &be_node_op_ops);

	op_be_Spill->ops.node_cmp_attr     = FrameAddr_cmp_attr;
	op_be_Reload->ops.node_cmp_attr    = FrameAddr_cmp_attr;
	op_be_Perm->ops.node_cmp_attr      = node_cmp_attr;
	op_be_MemPerm->ops.node_cmp_attr   = node_cmp_attr;
	op_be_Copy->ops.node_cmp_attr      = node_cmp_attr;
	op_be_Keep->ops.node_cmp_attr      = node_cmp_attr;
	op_be_CopyKeep->ops.node_cmp_attr  = node_cmp_attr;
	op_be_Call->ops.node_cmp_attr      = Call_cmp_attr;
	op_be_Return->ops.node_cmp_attr    = Return_cmp_attr;
	op_be_AddSP->ops.node_cmp_attr     = node_cmp_attr;
	op_be_SubSP->ops.node_cmp_attr     = node_cmp_attr;
	op_be_IncSP->ops.node_cmp_attr     = IncSP_cmp_attr;
	op_be_RegParams->ops.node_cmp_attr = node_cmp_attr;
	op_be_FrameAddr->ops.node_cmp_attr = FrameAddr_cmp_attr;
	op_be_Barrier->ops.node_cmp_attr   = node_cmp_attr;

	/* attach out dummy_ops to middle end nodes */
	for (opc = iro_First; opc <= iro_Last; ++opc) {
		ir_op *op = get_irp_opcode(opc);
		assert(op->ops.be_ops == NULL);
		op->ops.be_ops = &dummy_be_irn_ops;
	}

	op_Phi->ops.be_ops = &phi_irn_ops;
}
