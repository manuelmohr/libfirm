#ifndef __BE_DBGOUT_H__
#define __BE_DBGOUT_H__
/**
 * Debug output support.
 *
 * @author Michael Beck
 * @date   11.9.2006
 * @cvsid  $Id$
 */
#include "obst.h"
#include "beabi_t.h"

typedef struct dbg_handle dbg_handle;

/**
 * Debug operations.
 */
typedef struct debug_ops {
	/** close the stabs handler. */
	void (*close)(dbg_handle *handle);

	/** start a new source object (compilation unit) */
	void (*so)(dbg_handle *handle, const char *filename);

	/** Main Program */
	void (*main_program)(dbg_handle *handle);

	/** dumps the stabs for a method begin */
	void (*method_begin)(dbg_handle *handle, entity *ent, const be_stack_layout_t *layout);

	/** dumps the stabs for a method end */
	void (*method_end)(dbg_handle *handle);

	/** dumps a line number */
	void (*line)(dbg_handle *handle, unsigned lineno, const char *address);

	/** dump types */
	void (*types)(dbg_handle *handle);

	/** dump a global */
	void (*global)(dbg_handle *h, struct obstack *obst, entity *ent);

} debug_ops;

/** The base class of all debug implementations. */
struct dbg_handle {
	const debug_ops *ops;
};

/** close a debug handler. */
void be_dbg_close(dbg_handle *handle);

/** start a new source object (compilation unit) */
void be_dbg_so(dbg_handle *handle, const char *filename);

/** Main program */
void be_dbg_main_program(dbg_handle *handle);

/** debug for a method begin */
void be_dbg_method_begin(dbg_handle *handle, entity *ent, const be_stack_layout_t *layout);

/** debug for a method end */
void be_dbg_method_end(dbg_handle *handle);

/** debug for line number */
void be_dbg_line(dbg_handle *handle, unsigned lineno, const char *address);

/** dump types */
void be_dbg_types(dbg_handle *handle);

/** dump a global */
void be_dbg_global(dbg_handle *handle, struct obstack *obst, entity *ent);

/** Opens the NULL handler: no debug support. */
dbg_handle *be_nulldbg_open(void);

/** Opens a stabs handler. */
dbg_handle *be_stabs_open(FILE *out);

#endif /* __BE_DBGOUT_H__ */
