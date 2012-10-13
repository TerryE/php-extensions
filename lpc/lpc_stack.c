/*
  +----------------------------------------------------------------------+
  | LPC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id: lpc_stack.c 307048 2011-01-03 23:53:17Z kalle $ */

#include "lpc.h"
#include "lpc_stack.h"

struct lpc_stack_t {
    void** data;
    int capacity;
    int size;
};

lpc_stack_t* lpc_stack_create(int size_hint TSRMLS_DC)
{ENTER(lpc_stack_create)
    lpc_stack_t* stack = (lpc_stack_t*) lpc_emalloc(sizeof(lpc_stack_t) TSRMLS_CC);

    stack->capacity = (size_hint > 0) ? size_hint : 10;
    stack->size = 0;
    stack->data = (void**) lpc_emalloc(sizeof(void*) * stack->capacity TSRMLS_CC);

    return stack;
}

void lpc_stack_destroy(lpc_stack_t* stack TSRMLS_DC)
{ENTER(lpc_stack_destroy)
    if (stack != NULL) {
        lpc_efree(stack->data TSRMLS_CC);
        lpc_efree(stack TSRMLS_CC);
    }
}

void lpc_stack_clear(lpc_stack_t* stack)
{ENTER(lpc_stack_clear)
    assert(stack != NULL);
    stack->size = 0;
}

void lpc_stack_push(lpc_stack_t* stack, void* item TSRMLS_DC)
{ENTER(lpc_stack_push)
    assert(stack != NULL);
    if (stack->size == stack->capacity) {
        stack->capacity *= 2;
        stack->data = lpc_erealloc(stack->data, sizeof(void*)*stack->capacity TSRMLS_CC);
    }
    stack->data[stack->size++] = item;
}

void* lpc_stack_pop(lpc_stack_t* stack)
{ENTER(lpc_stack_pop)
    assert(stack != NULL && stack->size > 0);
    return stack->data[--stack->size];
}

void* lpc_stack_top(lpc_stack_t* stack)
{ENTER(lpc_stack_top)
    assert(stack != NULL && stack->size > 0);
    return stack->data[stack->size-1];
}

void* lpc_stack_get(lpc_stack_t* stack, int n)
{ENTER(lpc_stack_get)
    assert(stack != NULL && stack->size > n);
    return stack->data[n];
}

int lpc_stack_size(lpc_stack_t* stack)
{ENTER(lpc_stack_size)
    assert(stack != NULL);
    return stack->size;
}


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
