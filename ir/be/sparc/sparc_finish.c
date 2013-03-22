/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief    Peephole optimization and legalization of a sparc function
 * @author   Matthias Braun
 *
 * A note on sparc stackpointer (sp) behaviour:
 * The ABI expects SPARC_MIN_STACKSIZE bytes to be available at the
 * stackpointer. This space will be used to spill register windows,
 * and for spilling va_arg arguments (maybe we can optimize this away for
 * statically known not-va-arg-functions...)
 * This in effect means that we allocate that extra space at the function begin
 * which is easy. But this space isn't really fixed at the beginning of the
 * stackframe. Instead you should rather imagine the space as always being the
 * last-thing on the stack.
 * So when addressing anything stack-specific we have to account for this
 * area, while our compiler thinks the space is occupied at the beginning
 * of the stack frame. The code here among other things adjusts these offsets
 * accordingly.
 */
#include "config.h"

#include "bearch_sparc_t.h"
#include "gen_sparc_regalloc_if.h"
#include "sparc_new_nodes.h"
#include "sparc_transform.h"
#include "irprog.h"
#include "irgmod.h"
#include "ircons.h"
#include "irgwalk.h"
#include "heights.h"
#include "beirg.h"
#include "bepeephole.h"
#include "benode.h"
#include "besched.h"
#include "bespillslots.h"
#include "bestack.h"
#include "beirgmod.h"

static ir_heights_t *heights;

static void kill_unused_stacknodes(ir_node *node)
{
	if (get_irn_n_edges(node) > 0)
		return;

	if (be_is_IncSP(node)) {
		sched_remove(node);
		kill_node(node);
	} else if (is_Phi(node)) {
		int       arity = get_irn_arity(node);
		ir_node **ins   = ALLOCAN(ir_node*, arity);
		sched_remove(node);
		memcpy(ins, get_irn_in(node), arity*sizeof(ins[0]));
		kill_node(node);

		for (int i = 0; i < arity; ++i)
			kill_unused_stacknodes(ins[i]);
	}
}

static void introduce_epilog(ir_node *ret)
{
	arch_register_t const *const sp_reg = &sparc_registers[REG_SP];
	assert(arch_get_irn_register_req_in(ret, n_sparc_Return_sp) == sp_reg->single_req);

	ir_node           *const sp     = get_irn_n(ret, n_sparc_Return_sp);
	ir_node           *const block  = get_nodes_block(ret);
	ir_graph          *const irg    = get_irn_irg(ret);
	be_stack_layout_t *const layout = be_get_irg_stack_layout(irg);
	if (!layout->sp_relative) {
		arch_register_t const *const fp_reg  = &sparc_registers[REG_FRAME_POINTER];
		ir_node               *const fp      = be_get_initial_reg_value(irg, fp_reg);
		ir_node               *const new_sp  = be_get_initial_reg_value(irg, sp_reg);
		ir_node               *const restore = new_bd_sparc_RestoreZero(NULL, block, new_sp, fp);
		sched_add_before(ret, restore);
		arch_set_irn_register(restore, sp_reg);
		set_irn_n(ret, n_sparc_Return_sp, restore);
		kill_unused_stacknodes(sp);
	} else {
		ir_type *const frame_type = get_irg_frame_type(irg);
		unsigned const frame_size = get_type_size_bytes(frame_type);
		ir_node *const incsp      = be_new_IncSP(sp_reg, block, sp, -frame_size, 0);
		set_irn_n(ret, n_sparc_Return_sp, incsp);
		sched_add_before(ret, incsp);
	}
}

void sparc_introduce_prolog_epilog(ir_graph *irg)
{
	const arch_register_t *sp_reg     = &sparc_registers[REG_SP];
	ir_node               *start      = get_irg_start(irg);
	be_stack_layout_t     *layout     = be_get_irg_stack_layout(irg);
	ir_node               *block      = get_nodes_block(start);
	ir_node               *initial_sp = be_get_initial_reg_value(irg, sp_reg);
	ir_node               *schedpoint = start;
	ir_type               *frame_type = get_irg_frame_type(irg);
	unsigned               frame_size = get_type_size_bytes(frame_type);

	/* introduce epilog for every return node */
	ir_node *end_block = get_irg_end_block(irg);
	for (int i = 0, arity = get_irn_arity(end_block); i < arity; ++i) {
		ir_node *ret = get_irn_n(end_block, i);
		assert(is_sparc_Return(ret));
		introduce_epilog(ret);
	}

	while (be_is_Keep(sched_next(schedpoint)))
		schedpoint = sched_next(schedpoint);

	if (!layout->sp_relative) {
		ir_node *const save = new_bd_sparc_Save_imm(NULL, block, initial_sp, NULL, -(SPARC_MIN_STACKSIZE + frame_size));
		arch_set_irn_register(save, sp_reg);
		sched_add_after(schedpoint, save);
		schedpoint = save;

		edges_reroute_except(initial_sp, save, save);

		/* we still need the Save even if noone is explicitely using the
		 * value. (TODO: this isn't 100% correct yet, something at the end of
		 * the function should hold the Save, even if we use a restore
		 * which just overrides it instead of using the value)
		 */
		if (get_irn_n_edges(save) == 0) {
			ir_node *in[] = { save };
			ir_node *keep = be_new_Keep(block, 1, in);
			sched_add_after(schedpoint, keep);
		}
	} else {
		ir_node *const incsp = be_new_IncSP(sp_reg, block, initial_sp, frame_size, 0);
		edges_reroute_except(initial_sp, incsp, incsp);
		sched_add_after(schedpoint, incsp);
	}
}

/**
 * Creates a constant from an immediate value.
 */
static ir_node *create_constant_from_immediate(ir_node *node, int offset)
{
	dbg_info *dbgi  = get_irn_dbg_info(node);
	ir_node  *block = get_nodes_block(node);
	ir_node  *high  = new_bd_sparc_SetHi(dbgi, block, NULL, offset);

	sched_add_before(node, high);
	arch_set_irn_register(high, &sparc_registers[REG_G4]);

	if ((offset & 0x3ff) != 0) {
		ir_node *low = new_bd_sparc_Or_imm(dbgi, block, high, NULL, offset & 0x3ff);

		sched_add_before(node, low);
		arch_set_irn_register(low, &sparc_registers[REG_G4]);

		return low;
	}

	return high;
}

static void finish_sparc_Save(ir_node *node)
{
	sparc_attr_t *attr = get_sparc_attr(node);
	int offset = attr->immediate_value;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node               *base     = get_irn_n(node, n_sparc_Save_stack);
		dbg_info              *dbgi     = get_irn_dbg_info(node);
		ir_node               *block    = get_nodes_block(node);
		ir_node               *constant = create_constant_from_immediate(node, offset);
		ir_node               *new_save = new_bd_sparc_Save_reg(dbgi, block, base, constant);
		const arch_register_t *reg      = arch_get_irn_register(node);

		/* we have a Save with immediate */
		assert(get_irn_arity(node) == 1);

		sched_add_before(node, new_save);
		arch_set_irn_register(new_save, reg);
		be_peephole_exchange(node, new_save);
	}
}

/**
 * SPARC immediates are limited. Split IncSP with bigger immediates if
 * necessary.
 */
static void finish_be_IncSP(ir_node *node)
{
	int offset = be_get_IncSP_offset(node);

	/* we might have to break the IncSP apart if the constant has become too big */
	if (!sparc_is_value_imm_encodeable(offset) && !sparc_is_value_imm_encodeable(-offset)) {
		ir_node  *sp       = be_get_IncSP_pred(node);
		dbg_info *dbgi     = get_irn_dbg_info(node);
		ir_node  *block    = get_nodes_block(node);
		ir_node  *constant = create_constant_from_immediate(node, offset);
		ir_node  *sub      = new_bd_sparc_Sub_reg(dbgi, block, sp, constant);

		sched_add_before(node, sub);
		arch_set_irn_register(sub, &sparc_registers[REG_SP]);
		be_peephole_exchange(node, sub);
	}
}

/**
 * Adjust sp-relative offsets.
 *
 * Split into multiple instructions if offset exceeds SPARC immediate range.
 */
static void finish_sparc_FrameAddr(ir_node *node)
{
	sparc_attr_t *attr   = get_sparc_attr(node);
	int           offset = attr->immediate_value;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node               *base          = get_irn_n(node, n_sparc_FrameAddr_base);
		dbg_info              *dbgi          = get_irn_dbg_info(node);
		ir_node               *block         = get_nodes_block(node);
		ir_node               *constant      = create_constant_from_immediate(node, offset);
		ir_node               *new_frameaddr = new_bd_sparc_Add_reg(dbgi, block, base, constant);
		const arch_register_t *reg           = arch_get_irn_register(node);

		sched_add_before(node, new_frameaddr);
		arch_set_irn_register(new_frameaddr, reg);
		be_peephole_exchange(node, new_frameaddr);
	}
}

static void finish_sparc_Ld(ir_node *node)
{
	sparc_attr_t                  *attr            = get_sparc_attr(node);
	int                            offset          = attr->immediate_value;
	const sparc_load_store_attr_t *load_store_attr = get_sparc_load_store_attr_const(node);

	if (!load_store_attr->is_frame_entity)
		return;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node                 *ptr             = get_irn_n(node, n_sparc_Ld_ptr);
		dbg_info                *dbgi            = get_irn_dbg_info(node);
		ir_node                 *block           = get_nodes_block(node);
		ir_node                 *mem             = get_irn_n(node, n_sparc_Ld_mem);
		ir_mode                 *load_store_mode = load_store_attr->load_store_mode;
		ir_node                 *constant        = create_constant_from_immediate(node, offset);
		ir_node                 *new_load        = new_bd_sparc_Ld_reg(dbgi, block, ptr, constant, mem, load_store_mode);
		sparc_load_store_attr_t *new_load_attr   = get_sparc_load_store_attr(new_load);

		new_load_attr->is_frame_entity = load_store_attr->is_frame_entity;
		new_load_attr->is_reg_reg      = load_store_attr->is_reg_reg;

		sched_add_before(node, new_load);
		be_foreach_out(node, i) {
			arch_set_irn_register_out(new_load, i, arch_get_irn_register_out(node, i));
		}
		be_peephole_exchange(node, new_load);
	}

}

static void split_sparc_ldf(ir_node *node)
{
	sparc_load_store_attr_t *attr = get_sparc_load_store_attr(node);
	unsigned                 bits = get_mode_size_bits(attr->load_store_mode);
	/* split 128bit loads into 2 64bit loads */
	if (bits == 128) {
		dbg_info *dbgi  = get_irn_dbg_info(node);
		ir_node  *block = get_nodes_block(node);
		ir_node  *ptr   = get_irn_n(node, n_sparc_Ldf_ptr);
		ir_node  *mem   = get_irn_n(node, n_sparc_Ldf_mem);
		ir_node  *new_load
			= new_bd_sparc_Ldf_d(dbgi, block, ptr, mem, mode_D,
			                     attr->base.immediate_value_entity,
			                     attr->base.immediate_value + 8,
			                     attr->is_frame_entity);
		ir_node  *new_mem = new_r_Proj(new_load, mode_M, pn_sparc_Ldf_M);

		const arch_register_t *reg
			= arch_get_irn_register_out(node, pn_sparc_Ldf_res);
		unsigned reg_index = reg->global_index;

		arch_set_irn_register_out(new_load, pn_sparc_Ldf_res,
		                          &sparc_registers[reg_index+2]);

		attr->load_store_mode = mode_D;
		set_irn_n(node, n_sparc_Ldf_mem, new_mem);
		sched_add_before(node, new_load);
	}
}

static void finish_sparc_Ldf(ir_node *node)
{
	sparc_attr_t                  *attr            = get_sparc_attr(node);
	int                            offset          = attr->immediate_value;
	const sparc_load_store_attr_t *load_store_attr = get_sparc_load_store_attr_const(node);

	if (!load_store_attr->is_frame_entity)
		return;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node                 *ptr             = get_irn_n(node, n_sparc_Ldf_ptr);
		dbg_info                *dbgi            = get_irn_dbg_info(node);
		ir_node                 *block           = get_nodes_block(node);
		ir_node                 *mem             = get_irn_n(node, n_sparc_Ldf_mem);
		ir_mode                 *load_store_mode = load_store_attr->load_store_mode;
		ir_node                 *constant        = create_constant_from_immediate(node, offset);
		ir_node                 *new_ptr         = new_bd_sparc_Add_reg(dbgi, block, ptr, constant);
		ir_node                 *new_load        = new_bd_sparc_Ldf_s(dbgi, block, new_ptr, mem, load_store_mode, NULL, 0, true);
		sparc_load_store_attr_t *new_load_attr   = get_sparc_load_store_attr(new_load);

		new_load_attr->is_frame_entity = load_store_attr->is_frame_entity;
		new_load_attr->is_reg_reg      = load_store_attr->is_reg_reg;

		sched_add_before(node, new_load);
		be_foreach_out(node, i) {
			arch_set_irn_register_out(new_load, i, arch_get_irn_register_out(node, i));
		}
		be_peephole_exchange(node, new_load);
	}

}

static void finish_sparc_St(ir_node *node)
{
	sparc_attr_t                  *attr            = get_sparc_attr(node);
	int                            offset          = attr->immediate_value;
	const sparc_load_store_attr_t *load_store_attr = get_sparc_load_store_attr_const(node);

	if (!load_store_attr->is_frame_entity)
		return;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node                 *ptr             = get_irn_n(node, n_sparc_St_ptr);
		dbg_info                *dbgi            = get_irn_dbg_info(node);
		ir_node                 *block           = get_nodes_block(node);
		ir_node                 *mem             = get_irn_n(node, n_sparc_St_mem);
		ir_node                 *value           = get_irn_n(node, n_sparc_St_val);
		ir_mode                 *load_store_mode = load_store_attr->load_store_mode;
		ir_node                 *constant        = create_constant_from_immediate(node, offset);
		ir_node                 *new_load        = new_bd_sparc_St_reg(dbgi, block, value, ptr, constant, mem, load_store_mode);
		sparc_load_store_attr_t *new_load_attr   = get_sparc_load_store_attr(new_load);

		new_load_attr->is_frame_entity = load_store_attr->is_frame_entity;
		new_load_attr->is_reg_reg      = load_store_attr->is_reg_reg;

		sched_add_before(node, new_load);
		be_foreach_out(node, i) {
			arch_set_irn_register_out(new_load, i, arch_get_irn_register_out(node, i));
		}
		be_peephole_exchange(node, new_load);
	}

}

static void finish_sparc_Stf(ir_node *node)
{
	sparc_attr_t                  *attr            = get_sparc_attr(node);
	int                            offset          = attr->immediate_value;
	const sparc_load_store_attr_t *load_store_attr = get_sparc_load_store_attr_const(node);

	if (!load_store_attr->is_frame_entity)
		return;

	if (!sparc_is_value_imm_encodeable(offset)) {
		ir_node  *ptr             = get_irn_n(node, n_sparc_Stf_ptr);
		dbg_info *dbgi            = get_irn_dbg_info(node);
		ir_node  *block           = get_nodes_block(node);
		ir_node  *mem             = get_irn_n(node, n_sparc_Stf_mem);
		ir_node  *value           = get_irn_n(node, n_sparc_Stf_val);
		ir_mode  *load_store_mode = load_store_attr->load_store_mode;
		ir_node  *constant        = create_constant_from_immediate(node, offset);
		ir_node  *new_ptr         = new_bd_sparc_Add_reg(dbgi, block, ptr, constant);
		ir_node  *new_load        = new_bd_sparc_Stf_s(dbgi, block, value, new_ptr, mem, load_store_mode, NULL, 0, true);
		sparc_load_store_attr_t *new_load_attr = get_sparc_load_store_attr(new_load);

		new_load_attr->is_frame_entity = load_store_attr->is_frame_entity;
		new_load_attr->is_reg_reg      = load_store_attr->is_reg_reg;

		sched_add_before(node, new_load);
		be_foreach_out(node, i) {
			arch_set_irn_register_out(new_load, i, arch_get_irn_register_out(node, i));
		}
		be_peephole_exchange(node, new_load);
	}

}

static void peephole_be_IncSP(ir_node *node)
{
	node = be_peephole_IncSP_IncSP(node);
	if (!be_is_IncSP(node))
		return;

	ir_node *pred = be_get_IncSP_pred(node);
	if (is_sparc_Save(pred) && be_has_only_one_user(pred)) {
		int offset = -be_get_IncSP_offset(node);
		sparc_attr_t *attr = get_sparc_attr(pred);
		attr->immediate_value += offset;
		be_peephole_exchange(node, pred);
	}
}

static void peephole_sparc_FrameAddr(ir_node *node)
{
	/* the peephole code currently doesn't allow this since it changes
	 * the register. Find out why and how to workaround this... */
	(void) node;
}

/* output must not be local, or out reg. Since the destination of the restore
 * is the rotated register-file where only the old in-registers are still
 * visible (as out-registers) */
static bool is_restorezeroopt_reg(const arch_register_t *reg)
{
	unsigned index = reg->global_index;
	return (index >= REG_G0 && index <= REG_G7)
	    || (index >= REG_I0 && index <= REG_I7);
}

static void replace_with_restore_reg(ir_node *node, ir_node *replaced,
									 ir_node *op0, ir_node *op1)
{
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *stack_in = get_irn_n(node, n_sparc_RestoreZero_stack);
	ir_node  *fp       = get_irn_n(node, n_sparc_RestoreZero_frame_pointer);
	ir_node  *block    = get_nodes_block(node);
	ir_mode  *mode     = get_irn_mode(node);
	ir_node  *new_node = new_bd_sparc_Restore_reg(dbgi, block, stack_in, fp,
	                                              op0, op1);
	ir_node  *stack    = new_r_Proj(new_node, mode, pn_sparc_Restore_stack);
	ir_node  *res      = new_r_Proj(new_node, mode, pn_sparc_Restore_res);
	const arch_register_t *reg = arch_get_irn_register(replaced);
	const arch_register_t *sp  = &sparc_registers[REG_SP];
	arch_set_irn_register_out(new_node, pn_sparc_Restore_stack, sp);
	arch_set_irn_register_out(new_node, pn_sparc_Restore_res, reg);

	sched_add_before(node, new_node);
	be_peephole_exchange(node, stack);
	be_peephole_exchange(replaced, res);
}

static void replace_with_restore_imm(ir_node *node, ir_node *replaced,
									 ir_node *op, ir_entity *imm_entity,
									 int32_t immediate)
{
	dbg_info *dbgi     = get_irn_dbg_info(node);
	ir_node  *stack_in = get_irn_n(node, n_sparc_RestoreZero_stack);
	ir_node  *fp       = get_irn_n(node, n_sparc_RestoreZero_frame_pointer);
	ir_node  *block    = get_nodes_block(node);
	ir_mode  *mode     = get_irn_mode(node);
	ir_node  *new_node = new_bd_sparc_Restore_imm(dbgi, block, stack_in, fp,
	                                              op, imm_entity, immediate);
	ir_node  *stack    = new_r_Proj(new_node, mode, pn_sparc_Restore_stack);
	ir_node  *res      = new_r_Proj(new_node, mode, pn_sparc_Restore_res);
	const arch_register_t *reg = arch_get_irn_register(replaced);
	const arch_register_t *sp  = &sparc_registers[REG_SP];
	arch_set_irn_register_out(new_node, pn_sparc_Restore_stack, sp);
	arch_set_irn_register_out(new_node, pn_sparc_Restore_res, reg);

	sched_add_before(node, new_node);
	be_peephole_exchange(node, stack);
	be_peephole_exchange(replaced, res);
}

static void peephole_sparc_RestoreZero(ir_node *node)
{
	/* restore gives us a free "add" instruction, let's try to use that to fold
	 * an instruction in. We can do the following:
	 *
	 * - Copy values                  (g0 + reg)
	 * - Produce constants            (g0 + immediate)
	 * - Perform an add               (reg + reg)
	 * - Perform a sub with immediate (reg + (-immediate))
	 *
	 * Note: In an ideal world, this would not be a peephole optimization but
	 * already performed during code selection. Since about all foldable ops are
	 * arguments of the return node. However we have a hard time doing this
	 * since we construct epilogue code only after register allocation
	 * (and therefore after code selection).
	 */
	int n_tries = 10; /* limit our search */

	sched_foreach_reverse_before(node, schedpoint) {
		if (--n_tries == 0)
			break;

		if (arch_get_irn_n_outs(schedpoint) == 0)
			continue;

		if (!mode_is_data(get_irn_mode(schedpoint)))
			return;

		arch_register_t const *const reg = arch_get_irn_register(schedpoint);
		if (!is_restorezeroopt_reg(reg))
			continue;

		if (be_is_Copy(schedpoint) && be_can_move_down(heights, schedpoint, node)) {
			ir_node *const op = be_get_Copy_op(schedpoint);
			replace_with_restore_imm(node, schedpoint, op, NULL, 0);
		} else if (is_sparc_Or(schedpoint) &&
		           arch_get_irn_flags(schedpoint) & ((arch_irn_flags_t)sparc_arch_irn_flag_immediate_form) &&
		           arch_get_irn_register_in(schedpoint, 0) == &sparc_registers[REG_G0] &&
		           be_can_move_down(heights, schedpoint, node)) {
			/* it's a constant */
			const sparc_attr_t *attr      = get_sparc_attr_const(schedpoint);
			ir_entity          *entity    = attr->immediate_value_entity;
			int32_t             immediate = attr->immediate_value;
			ir_node            *g0        = get_irn_n(schedpoint, 0);
			replace_with_restore_imm(node, schedpoint, g0, entity, immediate);
		} else if (is_sparc_Add(schedpoint) &&
		           be_can_move_down(heights, schedpoint, node)) {
			if (arch_get_irn_flags(schedpoint) & ((arch_irn_flags_t)sparc_arch_irn_flag_immediate_form)) {
				ir_node            *op     = get_irn_n(schedpoint, 0);
				const sparc_attr_t *attr   = get_sparc_attr_const(schedpoint);
				ir_entity          *entity = attr->immediate_value_entity;
				int32_t             imm    = attr->immediate_value;
				replace_with_restore_imm(node, schedpoint, op, entity, imm);
			} else {
				ir_node *op0 = get_irn_n(schedpoint, 0);
				ir_node *op1 = get_irn_n(schedpoint, 1);
				replace_with_restore_reg(node, schedpoint, op0, op1);
			}
		} else if (is_sparc_Sub(schedpoint) &&
		           arch_get_irn_flags(schedpoint) & ((arch_irn_flags_t)sparc_arch_irn_flag_immediate_form) &&
		           arch_get_irn_register_in(schedpoint, 0) == &sparc_registers[REG_G0] &&
		           be_can_move_down(heights, schedpoint, node)) {
			/* it's a constant */
			const sparc_attr_t *attr   = get_sparc_attr_const(schedpoint);
			ir_entity          *entity = attr->immediate_value_entity;
			int32_t             imm    = attr->immediate_value;
			if (entity == NULL && sparc_is_value_imm_encodeable(-imm)) {
				ir_node *g0 = get_irn_n(schedpoint, 0);
				replace_with_restore_imm(node, schedpoint, g0, NULL, -imm);
			} else {
				continue;
			}
		}
		/* when we're here then we performed a folding and are done */
		return;
	}
}

static void finish_sparc_Return(ir_node *node)
{
	/* Ensure that the restore is directly before the return. */
	sched_foreach_reverse_before(node, restore) {
		if (is_sparc_Restore(restore) || is_sparc_RestoreZero(restore)) {
			sched_remove(restore);
			sched_add_before(node, restore);
			break;
		}
	}
}

static void register_peephole_optimisation(ir_op *op, peephole_opt_func func)
{
	assert(op->ops.generic == NULL);
	op->ops.generic = (op_func) func;
}

static void sparc_collect_frame_entity_nodes(ir_node *node, void *data)
{
	be_fec_env_t  *env = (be_fec_env_t*)data;

	if (be_is_Reload(node) && be_get_frame_entity(node) == NULL) {
		ir_mode *mode  = get_irn_mode(node);
		unsigned align = get_mode_size_bytes(mode);
		be_node_needs_frame_entity(env, node, mode, align);
		return;
	}

	if (!is_sparc_Ld(node) && !is_sparc_Ldf(node))
		return;

	const sparc_load_store_attr_t *attr = get_sparc_load_store_attr_const(node);
	ir_entity *entity = attr->base.immediate_value_entity;
	ir_mode   *mode   = attr->load_store_mode;
	if (entity != NULL)
		return;
	if (!attr->is_frame_entity)
		return;
	if (arch_get_irn_flags(node) & sparc_arch_irn_flag_needs_64bit_spillslot)
		mode = mode_Lu;
	unsigned align = get_mode_size_bytes(mode);
	be_node_needs_frame_entity(env, node, mode, align);
}

static void sparc_set_frame_entity(ir_node *node, ir_entity *entity)
{
	if (is_be_node(node)) {
		be_node_set_frame_entity(node, entity);
	} else {
		/* we only say be_node_needs_frame_entity on nodes with load_store
		 * attributes, so this should be fine */
		sparc_load_store_attr_t *attr = get_sparc_load_store_attr(node);
		assert(attr->is_frame_entity);
		assert(attr->base.immediate_value_entity == NULL);
		attr->base.immediate_value_entity = entity;
	}
}

void sparc_finish_graph(ir_graph *irg)
{
	be_stack_layout_t *stack_layout = be_get_irg_stack_layout(irg);
	bool               at_begin     = stack_layout->sp_relative ? true : false;
	be_fec_env_t      *fec_env      = be_new_frame_entity_coalescer(irg);

	irg_walk_graph(irg, NULL, sparc_collect_frame_entity_nodes, fec_env);
	be_assign_entities(fec_env, sparc_set_frame_entity, at_begin);
	be_free_frame_entity_coalescer(fec_env);
	sparc_adjust_stack_entity_offsets(irg);

	sparc_introduce_prolog_epilog(irg);

	/* fix stack entity offsets */
	be_abi_fix_stack_nodes(irg);
	sparc_fix_stack_bias(irg);

	heights = heights_new(irg);

	/* perform peephole optimizations */
	ir_clear_opcodes_generic_func();
	register_peephole_optimisation(op_be_IncSP,        peephole_be_IncSP);
	register_peephole_optimisation(op_sparc_FrameAddr, peephole_sparc_FrameAddr);
	register_peephole_optimisation(op_sparc_RestoreZero,
	                               peephole_sparc_RestoreZero);
	register_peephole_optimisation(op_sparc_Ldf, split_sparc_ldf);
	be_peephole_opt(irg);

	/* perform legalizations (mostly fix nodes with too big immediates) */
	ir_clear_opcodes_generic_func();
	register_peephole_optimisation(op_be_IncSP,        finish_be_IncSP);
	register_peephole_optimisation(op_sparc_FrameAddr, finish_sparc_FrameAddr);
	register_peephole_optimisation(op_sparc_Ld,        finish_sparc_Ld);
	register_peephole_optimisation(op_sparc_Ldf,       finish_sparc_Ldf);
	register_peephole_optimisation(op_sparc_Return,    finish_sparc_Return);
	register_peephole_optimisation(op_sparc_Save,      finish_sparc_Save);
	register_peephole_optimisation(op_sparc_St,        finish_sparc_St);
	register_peephole_optimisation(op_sparc_Stf,       finish_sparc_Stf);
	be_peephole_opt(irg);

	heights_free(heights);

	be_remove_dead_nodes_from_schedule(irg);
}
