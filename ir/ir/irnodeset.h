/*
 * Copyright (C) 1995-2007 University of Karlsruhe.  All right reserved.
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
 * @author    Matthias Braun
 * @date      30.03.2007
 * @brief     A nodeset. This should be prefered over a simple pset, because it
 *            tries to guarantee deterministic behavior. (and is faster)
 * @version   $Id$
 * @note      Actually the bits to make the behaviour deterministic are not
 *            implemented yet...
 */
#ifndef _FIRM_IRNODESET_H_
#define _FIRM_IRNODESET_H_

#include "irnode.h"
#include "xmalloc.h"

#define HashSet          ir_nodeset_t
#define HashSetIterator  ir_nodeset_iterator_t
#define ValueType        ir_node*
#define DO_REHASH
#include "hashset.h"
#undef DO_REHASH
#undef ValueType
#undef HashSetIterator
#undef HashSet

/**
 * Initializes a nodeset with default size.
 *
 * @param nodeset      Pointer to allocated space for the nodeset
 */
void ir_nodeset_init(ir_nodeset_t *nodeset);

/**
 * Initializes a nodeset
 *
 * @param nodeset             Pointer to allocated space for the nodeset
 * @param expected_elements   Number of elements expected in the nodeset (roughly)
 */
void ir_nodeset_init_size(ir_nodeset_t *nodeset, size_t expected_elements);

/**
 * Destroys a nodeset and frees the memory allocated for hashtable. The memory of
 * the nodeset itself is not freed.
 *
 * @param nodeset   Pointer to the nodeset
 */
void ir_nodeset_destroy(ir_nodeset_t *nodeset);

/**
 * Allocates memory for a nodeset and initializes the set.
 *
 * @param expected_elements   Number of elements expected in the nodeset (roughly)
 * @return The initialized nodeset
 */
static INLINE ir_nodeset_t *ir_nodeset_new(size_t expected_elements) {
	ir_nodeset_t *res = xmalloc(sizeof(*res));
	ir_nodeset_init_size(res, expected_elements);
	return res;
}

/**
 * Destroys a nodeset and frees the memory of the nodeset itself.
 */
static INLINE void ir_nodeset_del(ir_nodeset_t *nodeset) {
	ir_nodeset_destroy(nodeset);
	xfree(nodeset);
}

/**
 * Inserts a node into a nodeset.
 *
 * @param nodeset   Pointer to the nodeset
 * @param node      node to insert into the nodeset
 * @returns         1 if the element has been inserted,
 *                  0 if it was already there
 */
int ir_nodeset_insert(ir_nodeset_t *nodeset, ir_node *node);

/**
 * Removes a node from a nodeset. Does nothing if the nodeset doesn't contain
 * the node.
 *
 * @param nodeset  Pointer to the nodeset
 * @param node     Node to remove from the nodeset
 */
void ir_nodeset_remove(ir_nodeset_t *nodeset, const ir_node *node);

/**
 * Tests whether a nodeset contains a specific node
 *
 * @param nodeset   Pointer to the nodeset
 * @param node      The pointer to find
 * @returns         1 if nodeset contains the node, 0 else
 */
int ir_nodeset_contains(const ir_nodeset_t *nodeset, const ir_node *node);

/**
 * Returns the number of pointers contained in the nodeset
 *
 * @param nodeset   Pointer to the nodeset
 * @returns       Number of pointers contained in the nodeset
 */
size_t ir_nodeset_size(const ir_nodeset_t *nodeset);

/**
 * Initializes a nodeset iterator. Sets the iterator before the first element in
 * the nodeset.
 *
 * @param iterator   Pointer to already allocated iterator memory
 * @param nodeset       Pointer to the nodeset
 */
void ir_nodeset_iterator_init(ir_nodeset_iterator_t *iterator,
                              const ir_nodeset_t *nodeset);

/**
 * Advances the iterator and returns the current element or NULL if all elements
 * in the nodeset have been processed.
 * @attention It is not allowed to use nodeset_insert or nodeset_remove while
 *            iterating over a nodeset.
 *
 * @param iterator  Pointer to the nodeset iterator.
 * @returns         Next element in the nodeset or NULL
 */
ir_node *ir_nodeset_iterator_next(ir_nodeset_iterator_t *iterator);

/**
 * Removes the element the iterator currently points to
 *
 * @param nodeset   Pointer to the nodeset
 * @param iterator  Pointer to the nodeset iterator.
 */
void ir_nodeset_remove_iterator(ir_nodeset_t *nodeset,
                                const ir_nodeset_iterator_t *iterator);

#define foreach_ir_nodeset(nodeset, irn, iter) \
	for(ir_nodeset_iterator_init(&iter, nodeset), \
        irn = ir_nodeset_iterator_next(&iter);    \
		irn != NULL; irn = ir_nodeset_iterator_next(&iter))

#endif
