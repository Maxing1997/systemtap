/*  -*- linux-c -*-
 * Stack tracing functions
 * Copyright (C) 2005-2009, 2014-2019 Red Hat Inc.
 * Copyright (C) 2005 Intel Corporation.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

/*
  The translator will only include this file if the session needs any
  of the backtrace functions.  Currently indicated by having the session
  need_unwind flag, which is set by tapset functions marked with
  pragme:unwind.
*/

#ifndef _STACK_C_
#define _STACK_C_

/** @file stack.c
 * @brief Stack Tracing Functions
 */

/** @addtogroup stack Stack Tracing Functions
 *
 * @{
 */

#include "sym.c"
#include "regs.h"

#include <linux/stacktrace.h>

#if defined(STAPCONF_KERNEL_STACKTRACE) || defined(STAPCONF_KERNEL_STACKTRACE_NO_BP)
#include <asm/stacktrace.h>
#endif

#if defined(STAPCONF_KERNEL_UNWIND_STACK)
#include <asm/unwind.h>
#endif

#if defined(STAPCONF_STACK_TRACE_SAVE_REGS) /* linux 5.2+ apprx. */
static __typeof__(stack_trace_save_regs) (*stack_trace_save_regs_fn); /* not exported */

static int
_stp_init_stack(void)
{
	//[maxing COMMENT]: STAPCONF_STACK_TRACE_SAVE_REGS
	stack_trace_save_regs_fn = (void*) kallsyms_lookup_name("stack_trace_save_regs");
	dbug_unwind(1, "stack_trace_saves_regs_fn=%lx for _stp_stack_print_fallback().\n",
		    (unsigned long) stack_trace_save_regs_fn);
	return 0;
}

#else /* ! STAPCONF_STACK_TRACE_SAVE_REGS */

static void (*(save_stack_trace_regs_fn))(struct pt_regs *regs,
				  struct stack_trace *trace);

static int
_stp_init_stack(void)
{
	/* check for save_stack_trace_regs function for fallback stack print */
	save_stack_trace_regs_fn = (void *)kallsyms_lookup_name("save_stack_trace_regs");
	dbug_unwind(1, "save_stack_trace_regs_fn=%lx for _stp_stack_print_fallback().\n",
		    (unsigned long) save_stack_trace_regs_fn);
	return 0;
}

#endif /* STAPCONF_STACK_TRACE_SAVE_REGS */



static void _stp_stack_print_fallback(struct context *, unsigned long,
				      struct pt_regs*, int, int, int);

#ifdef STP_USE_DWARF_UNWINDER
#ifdef STAPCONF_LINUX_UACCESS_H
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif
#include <linux/types.h>
#define intptr_t long
#define uintptr_t unsigned long
#endif

#if defined (__ia64__)
#include "stack-ia64.c"
#elif defined (__arm__)
#include "stack-arm.c"
#elif defined (__mips__)
#include "stack-mips.c"
#elif defined (__s390__)
#include "stack-s390.c"
#else
#ifndef STP_USE_DWARF_UNWINDER
#error "Unsupported architecture"
#endif
#endif

#if defined(STAPCONF_KERNEL_STACKTRACE) || defined(STAPCONF_KERNEL_STACKTRACE_NO_BP)

struct print_stack_data
{
        int flags;
        int levels;
        int skip;
};

#if defined(STAPCONF_STACKTRACE_OPS_WARNING)
static void print_stack_warning(void *data, char *msg)
{
}

static void
print_stack_warning_symbol(void *data, char *msg, unsigned long symbol)
{
}
#endif

static int print_stack_stack(void *data, char *name)
{
	return -1;
}

#ifdef STAPCONF_STACKTRACE_OPS_INT_ADDRESS
static int print_stack_address(void *data, unsigned long addr, int reliable)
#else
static void print_stack_address(void *data, unsigned long addr, int reliable)
#endif
{
	struct print_stack_data *sdata = data;
	if (sdata->skip > 0)
		sdata->skip--;
	else if (sdata->levels > 0) {
		_stp_print_addr(addr,
				sdata->flags | (reliable ? 0 :_STP_SYM_INEXACT),
				NULL, NULL);
		sdata->levels--;
	}
#ifdef STAPCONF_STACKTRACE_OPS_INT_ADDRESS
	return 0;
#endif
}

static const struct stacktrace_ops print_stack_ops = {
#if defined(STAPCONF_STACKTRACE_OPS_WARNING)
	.warning = print_stack_warning,
	.warning_symbol = print_stack_warning_symbol,
#endif
	.stack = print_stack_stack,
	.address = print_stack_address,
#if defined(STAPCONF_WALK_STACK)
	.walk_stack = print_context_stack,
#endif
};

/* Used for kernel backtrace printing when other mechanisms fail. */
static void _stp_stack_print_fallback(struct context *c __attribute__((unused)),
				      unsigned long stack, struct pt_regs *regs,
				      int sym_flags, int levels, int skip)
{
        struct print_stack_data print_data;
        print_data.flags = sym_flags;
        print_data.levels = levels;
        print_data.skip = skip;
#if defined(STAPCONF_KERNEL_STACKTRACE)
        dbug_unwind(1, "fallback kernel stacktrace\n");
        dump_trace(current, NULL, (long *)stack, 0, &print_stack_ops,
                   &print_data);
#else
	/* STAPCONF_KERNEL_STACKTRACE_NO_BP */
        dbug_unwind(1, "fallback kernel stacktrace (no bp)\n");
        dump_trace(current, NULL, (long *)stack, &print_stack_ops,
                   &print_data);
#endif
}
#else
static void _stp_stack_print_fallback(struct context *c, unsigned long sp,
				      struct pt_regs *regs, int sym_flags,
				      int levels, int skip) {
        unsigned long *entries = c->kern_bt_entries;
        unsigned i;
        unsigned num_entries;
        
#if defined(STAPCONF_STACK_TRACE_SAVE_REGS) /* linux 5.2+ apprx. */
	if (!stack_trace_save_regs_fn) {
		dbug_unwind(1, "no fallback kernel stacktrace (giving up)\n");
		_stp_print_addr(0, sym_flags | _STP_SYM_INEXACT, NULL, c);
		return;
	}

        num_entries = ibt_wrapper(unsigned int,
				  (*stack_trace_save_regs_fn)(regs, &entries[0], MAXBACKTRACE, skip));
#else
	struct stack_trace trace;
	/* If don't have save_stack_trace_regs unwinder, just give up. */
	if (!save_stack_trace_regs_fn) {
		dbug_unwind(1, "no fallback kernel stacktrace (giving up)\n");
		_stp_print_addr(0, sym_flags | _STP_SYM_INEXACT, NULL, c);
		return;
	}

	/* Use kernel provided save_stack_trace_regs unwinder if available */
	dbug_unwind(1, "fallback kernel stacktrace (save_stack_trace_regs)\n");
	memset(&trace, 0, sizeof(trace));
	trace.max_entries = MAXBACKTRACE;
	trace.entries = &(entries[0]);
	trace.skip = skip;
	(* (save_stack_trace_regs_fn))(regs, &trace);

	dbug_unwind(1, "trace.nr_entries: %d\n", trace.nr_entries);
	dbug_unwind(1, "trace.max_entries: %d\n", trace.max_entries);
	dbug_unwind(1, "trace.skip %d\n", trace.skip);
        num_entries = trace.nr_entries;
#endif

	/* save_stack_trace_reg() adds a ULONG_MAX after last valid entry. Ignore it. */
	for (i=0; i<MAXBACKTRACE && i<num_entries && entries[i]!=ULONG_MAX; ++i) {
		/* When we have frame pointers, the unwind addresses can be
		   (mostly) trusted, otherwise it is all guesswork.  */
#ifdef CONFIG_FRAME_POINTER
		_stp_print_addr((unsigned long) entries[i], sym_flags, NULL, c);
#else
		_stp_print_addr((unsigned long) entries[i], sym_flags | _STP_SYM_INEXACT,
				NULL, c);
#endif
	}
}
#endif /* defined(STAPCONF_KERNEL_STACKTRACE) || defined(STAPCONF_KERNEL_STACKTRACE_NO_BP) */

/** Gets user space registers when available, also sets context
 * full_uregs_p if appropriate.  Should be used instead of accessing
 * context uregs field directly when (full) uregs are needed from
 * kernel context.
 */
static struct pt_regs *_stp_get_uregs(struct context *c)
{
  /* When the probe occurred in user context uregs are always complete. */
  if (c->uregs && c->user_mode_p)
    c->full_uregs_p = 1;
  else if (c->uregs == NULL)
    {
      dbug_unwind(1, "computing uregs\n");
      /* First try simple recovery through task_pt_regs,
	 on some platforms that already provides complete uregs. */
      c->uregs = _stp_current_pt_regs();
      if (c->uregs && _stp_task_pt_regs_valid(current, c->uregs))
	c->full_uregs_p = 1;

/* Sadly powerpc does support the dwarf unwinder, but doesn't have enough
   CFI in the kernel to recover fully to user space. */
#if defined(STP_USE_DWARF_UNWINDER) && !defined (__powerpc__)
      else if (c->uregs != NULL && c->kregs != NULL && !c->user_mode_p)
	{
	  struct unwind_frame_info *info = &c->uwcontext_kernel.info;
	  int ret = 0;
	  int levels;

	  /* We might be lucky and this probe already ran the kernel
	     unwind to end up in the user regs. */
	  if (UNW_PC(info) == REG_IP(c->uregs))
	    {
	      levels = 0;
	      dbug_unwind(1, "feeling lucky, info pc == uregs pc\n");
	    }
	  else
	    {
	      /* Try to recover the uregs by unwinding from the the kernel
		 probe location. */
	      levels = MAXBACKTRACE;
	      arch_unw_init_frame_info(info, c->kregs, 0);
	      dbug_unwind(1, "Trying to recover... searching for 0x%llx\n",
			  (unsigned long long) REG_IP(c->uregs));

	      /* Mark the kernel unwind cache as invalid
		 (uwcache_kernel.depth is no longer consistent with
		 the actual current depth of the unwind).

		 We don't save PCs in the cache at this point because
		 this kernel unwind procedure does not fetch the top
		 level PC, so uwcache_kernel.pc[0] would be left
		 unpopulated. We would have to either fetch the
		 current PC here, or specially represent this state of
		 the cache, something we don't bother with at this
		 stage.

	         XXX: this can create (tolerable amounts of) inefficiency
	         if the probe intersperses user and kernel unwind calls,
	         since the other unwind code can clear uregs, triggering
	         a redundant unwind the next time we need them. */
	      dbug_unwind(1, "clearing kernel unwind cache\n");
	      c->uwcache_kernel.state = uwcache_uninitialized;
	    }

	  while (levels > 0 && ret == 0 && UNW_PC(info) != REG_IP(c->uregs))
	    {
	      levels--;
	      ret = unwind(&c->uwcontext_kernel, 0);
	      dbug_unwind(1, "unwind levels: %d, ret: %d, pc=0x%llx\n",
			  levels, ret, (unsigned long long) UNW_PC(info));
	    }

	  /* Have we arrived where we think user space currently is? */
	  if (ret == 0 && UNW_PC(info) == REG_IP(c->uregs))
	    {
	      /* Note we need to clear this state again when the unwinder
		 has been rerun. See __stp_stack_print invocation below. */
	      UNW_SP(info) = REG_SP(c->uregs); /* Fix up user stack */
	      c->uregs = &info->regs;
	      c->full_uregs_p = 1;
	      dbug_unwind(1, "recovered with pc=0x%llx sp=0x%llx\n",
			  (unsigned long long) UNW_PC(info),
			  (unsigned long long) UNW_SP(info));
	    }
	  else
	    dbug_unwind(1, "failed to recover user reg state\n");
	}
#endif
    }
  return c->uregs;
}


static unsigned long
_stp_stack_unwind_one_kernel(struct context *c, unsigned depth)
{
	struct pt_regs *regs = NULL;
	struct unwind_frame_info *info = NULL;
	int ret;

	if (depth == 0) { /* Start by fetching the current PC. */
		dbug_unwind(1, "STARTING kernel unwind\n");

		if (! c->kregs) {
			/* Even the current PC is unknown; so we have
			 * absolutely no data at any depth.
			 *
			 * Note that unlike _stp_stack_kernel_print(),
			 * we can't fall back to calling dump_trace()
			 * to obtain the backtrace -- since that
			 * returns a string, which we would have to
			 * tokenize. Callers that want to use the
			 * dump_trace() fallback should call
			 * _stp_stack_kernel_print() and do their own
			 * tokenization of the result. */
#if defined (__i386__) || defined (__x86_64__)
		        arch_unw_init_frame_info(&c->uwcontext_kernel.info, NULL, 0);
		        return UNW_PC(&c->uwcontext_kernel.info);
#else
			return 0;
#endif
		} else if (c->probe_type == stp_probe_type_kretprobe
			   && c->ips.krp.pi) {
			return (unsigned long)_stp_ret_addr_r(c->ips.krp.pi);
		} else {
			return REG_IP(c->kregs);
		}
	}

#ifdef STP_USE_DWARF_UNWINDER
	/* Otherwise, use the DWARF unwinder to unwind one step. */

	regs = c->kregs;

	info = &c->uwcontext_kernel.info;

	dbug_unwind(1, "CONTINUING kernel unwind to depth %d\n", depth);

	if (depth == 1) {
                /* First step of actual DWARF unwind;
		   need to clear uregs& set up uwcontext->info. */
		if (c->uregs == &c->uwcontext_kernel.info.regs) {
			dbug_unwind(1, "clearing uregs\n");
			/* Unwinder needs the reg state, clear uregs ref. */
			c->uregs = NULL;
			c->full_uregs_p = 0;
		}

		arch_unw_init_frame_info(info, regs, 0);
	}

	ret = unwind(&c->uwcontext_kernel, 0);
	dbug_unwind(1, "ret=%d PC=%llx SP=%llx\n", ret,
		    (unsigned long long) UNW_PC(info),
		    (unsigned long long) UNW_SP(info));

	/* check if unwind hit an error */
	if (ret || _stp_lookup_bad_addr(VERIFY_READ, sizeof(long),
					UNW_PC(info), STP_KERNEL_DS)) {
		return 0;
	}

	return UNW_PC(info);
#else
	return 0;
#endif
}

static unsigned long _stp_stack_kernel_get(struct context *c, unsigned depth)
{
	unsigned long pc = 0;

	if (c->uwcache_kernel.state == uwcache_uninitialized) {
		c->uwcache_kernel.depth = 0;
		c->uwcache_kernel.state = uwcache_partial;
	}

	if (unlikely(depth >= MAXBACKTRACE))
		return 0;

	/* Obtain cached value if available. */
	if (depth < c->uwcache_kernel.depth)
		return c->uwcache_kernel.pc[depth];
	else if (c->uwcache_kernel.state == uwcache_finished)
		return 0; /* unwind does not reach this far */

	/* Advance uwcontext to the required depth. */
	while (c->uwcache_kernel.depth <= depth) {
		pc = c->uwcache_kernel.pc[c->uwcache_kernel.depth]
		   = _stp_stack_unwind_one_kernel(c, c->uwcache_kernel.depth);
		c->uwcache_kernel.depth ++;
		if (pc == 0 || pc == _stp_kretprobe_trampoline) {
			/* Mark unwind completed. */
			c->uwcache_kernel.state = uwcache_finished;
			break;
			/* XXX: is there a way to unwind across kretprobe trampolines? PR9999 */
		}
	}

	/* Return the program counter at the current depth. */
	return pc;
}

/** Prints the stack backtrace
 * @param regs A pointer to the struct pt_regs.
 * @param verbose _STP_SYM_FULL or _STP_SYM_BRIEF
 */

static void _stp_stack_kernel_print(struct context *c, int sym_flags)
{
	unsigned n, remaining;
	unsigned long l;
	bool print_addr_seen = 0;

	/* print the current address */
	if (c->probe_type == stp_probe_type_kretprobe && c->ips.krp.pi
	    && (sym_flags & _STP_SYM_FULL) == _STP_SYM_FULL) {
		_stp_print("Returning from: ");
		_stp_print_addr((unsigned long)_stp_probe_addr_r(c->ips.krp.pi),
				sym_flags, NULL, c);
		_stp_print("Returning to  : ");
	}
	_stp_print_addr(_stp_stack_kernel_get(c, 0), sym_flags, NULL, c);

#ifdef STP_USE_DWARF_UNWINDER
	for (n = 1; n < MAXBACKTRACE; n++) {
		l = _stp_stack_kernel_get(c, n);
		if (l == 0) {
			remaining = MAXBACKTRACE - n;
                        /* In case _stp_print_addr() successfully managed to do
                         * some dwarf unwinding, there's no need to fall back to
                         * _stp_stack_print_fallback().  Doing it might cause
                         * duplicate output in the resulting stack trace.  The
                         * print_addr_seen helps to prevent that.  Test covered
                         * by backtrace.exp. */
			if (!print_addr_seen)
				_stp_stack_print_fallback(c, UNW_SP(&c->uwcontext_kernel.info),
							  &c->uwcontext_kernel.info.regs,
							  sym_flags, remaining, 0);
			break;
		} else {
			_stp_print_addr(l, sym_flags, NULL, c);
			print_addr_seen = 1;
		}
	}
#else
	if (! c->kregs) {
		/* This is a fatal block for _stp_stack_kernel_get,
		 * but when printing a backtrace we can use this
		 * inexact fallback.
		 *
		 * When compiled with frame pointers we can do
		 * a pretty good guess at the stack value,
		 * otherwise let dump_stack guess it
		 * (and skip some framework frames). */
#if defined(STAPCONF_KERNEL_STACKTRACE) || defined(STAPCONF_KERNEL_STACKTRACE_NO_BP)
		unsigned long sp;
		int skip;
#ifdef CONFIG_FRAME_POINTER
		sp  = *(unsigned long *) __builtin_frame_address (0);
		skip = 1; /* Skip just this frame. */
#else
		sp = 0;
		skip = 5; /* yes, that many framework frames. */
#endif
		_stp_stack_print_fallback(c, sp, NULL, sym_flags,
					  MAXBACKTRACE, skip);
#else
		if (sym_flags & _STP_SYM_SYMBOL)
			_stp_printf("<no kernel backtrace at %s>\n",
				    c->probe_point);
		else
			_stp_print("\n");
#endif
		return;
	}
	else
		/* Arch specific fallback for kernel backtraces. */
		__stp_stack_print(c->kregs, sym_flags, MAXBACKTRACE);
#endif
}

static unsigned long
_stp_stack_unwind_one_user(struct context *c, unsigned depth)
{
	struct pt_regs *regs = NULL;
	int uregs_valid = 0;
	struct uretprobe_instance *ri = NULL;
	struct unwind_frame_info *info = NULL;
	int ret;
#ifdef STAPCONF_UPROBE_GET_PC
	unsigned long maybe_pc;
#endif

	if (c->probe_type == stp_probe_type_uretprobe)
		ri = c->ips.ri;
#ifdef STAPCONF_UPROBE_GET_PC
	else if (c->probe_type == stp_probe_type_uprobe)
		ri = GET_PC_URETPROBE_NONE;
#endif

	/* XXX: The computation that gives this is cached, so calling
	 * _stp_get_uregs multiple times is okay... probably. */
	regs = _stp_get_uregs(c);
	uregs_valid = c->full_uregs_p;

	if (! current->mm || ! regs)
		return 0; // no user backtrace at this probe point

	if (depth == 0) { /* Start by fetching the current PC. */
		dbug_unwind(1, "STARTING user unwind\n");

#ifdef STAPCONF_UPROBE_GET_PC
		if (c->probe_type == stp_probe_type_uretprobe && ri) {
			return ri->ret_addr;
		} else {
			return REG_IP(regs);
		}
#else
		return REG_IP(regs);
#endif
	}

#ifdef STP_USE_DWARF_UNWINDER
	info = &c->uwcontext_user.info;

	dbug_unwind(1, "CONTINUING user unwind to depth %d\n", depth);

	if (depth == 1) { /* need to clear uregs & set up uwcontext->info */
		if (c->uregs == &c->uwcontext_user.info.regs) {
			dbug_unwind(1, "clearing uregs\n");
			/* Unwinder needs the reg state, clear uregs ref. */
			c->uregs = NULL;
			c->full_uregs_p = 0;
		}

		arch_unw_init_frame_info(info, regs, 0);
	}

	ret = unwind(&c->uwcontext_user, 1);
#ifdef STAPCONF_UPROBE_GET_PC
	maybe_pc = 0;
	if (ri) {
		maybe_pc = uprobe_get_pc(ri, UNW_PC(info), UNW_SP(info));
		if (!maybe_pc)
			printk("SYSTEMTAP ERROR: uprobe_get_return returned 0\n");
		else
			UNW_PC(info) = maybe_pc;
	}
#endif
	dbug_unwind(1, "ret=%d PC=%llx SP=%llx\n", ret,
		    (unsigned long long) UNW_PC(info), (unsigned long long) UNW_SP(info));

	/* check if unwind hit an error */
	if (ret || _stp_lookup_bad_addr(VERIFY_READ, sizeof(long),
					UNW_PC(info), STP_USER_DS)) {
		return 0;
	}

	return UNW_PC(info);
#else
	/* User stack traces only supported for arches with dwarf unwinder. */
	return 0;
#endif
}

static unsigned long _stp_stack_user_get(struct context *c, unsigned depth)
{
	unsigned long pc = 0;

	if (c->uwcache_user.state == uwcache_uninitialized) {
		c->uwcache_user.depth = 0;
		c->uwcache_user.state = uwcache_partial;
	}

	if (unlikely(depth >= MAXBACKTRACE))
		return 0;

	/* Obtain cached value if available. */
	if (depth < c->uwcache_user.depth)
		return c->uwcache_user.pc[depth];
	else if (c->uwcache_user.state == uwcache_finished)
		return 0; /* unwind does not reach this far */

	/* Advance uwcontext to the required depth. */
	while (c->uwcache_user.depth <= depth) {
		pc = c->uwcache_user.pc[c->uwcache_user.depth]
		   = _stp_stack_unwind_one_user(c, c->uwcache_user.depth);
		c->uwcache_user.depth ++;
		if (pc == 0) {
			/* Mark unwind completed. */
			c->uwcache_user.state = uwcache_finished;
			break;
		}
	}

	/* Return the program counter at the current depth. */
	return pc;
}

static void _stp_stack_user_print(struct context *c, int sym_flags)
{
	struct pt_regs *regs = NULL;
	struct uretprobe_instance *ri = NULL;
	unsigned n; unsigned long l;

	if (c->probe_type == stp_probe_type_uretprobe)
		ri = c->ips.ri;
#ifdef STAPCONF_UPROBE_GET_PC
	else if (c->probe_type == stp_probe_type_uprobe)
		ri = GET_PC_URETPROBE_NONE;
#endif

	regs = _stp_get_uregs(c);

	if (! current->mm || ! regs) {
		if (sym_flags & _STP_SYM_SYMBOL)
			_stp_printf("<no user backtrace at %s>\n",
				    c->probe_point);
		else
			_stp_print("\n");
		return;
	}

	/* print the current address */
#ifdef STAPCONF_UPROBE_GET_PC
	if (c->probe_type == stp_probe_type_uretprobe && ri) {
		if ((sym_flags & _STP_SYM_FULL) == _STP_SYM_FULL) {
			_stp_print("Returning from: ");
			/* ... otherwise this dereference fails */
			_stp_print_addr(ri->rp->u.vaddr, sym_flags, current, c);
			_stp_print("Returning to  : ");
		}
	}
#endif
	_stp_print_addr(_stp_stack_user_get(c, 0), sym_flags, current, c);

	/* print rest of stack... */
#ifdef STP_USE_DWARF_UNWINDER
	for (n = 1; n < MAXBACKTRACE; n++) {
		l = _stp_stack_user_get(c, n);
		if (l == 0) break; // No user space fallback available
		_stp_print_addr(l, sym_flags, current, c);
	}
#else
	/* User stack traces only supported for arches with dwarf unwinder. */
	if (sym_flags & _STP_SYM_SYMBOL)
		_stp_printf("<no user backtrace support on arch>\n");
	else
		_stp_print("\n");
#endif
}

static void __stp_sprint_begin(struct _stp_log *log)
{
	__stp_print_flush(log);
	log->no_flush = true;
}

static void __stp_sprint_end(struct _stp_log *log)
{
	log->no_flush = false;
	log->is_full = false;
	log->len = 0;
}

/** Writes stack backtrace to a string
 *
 * @param str string
 * @param regs A pointer to the struct pt_regs.
 * @returns void
 */
static void _stp_stack_kernel_sprint(char *str, int size, struct context* c,
				     int sym_flags)
{
	/* To get an hex string, we use a simple trick.
	 * First flush the print buffer,
	 * then call _stp_stack_print,
	 * then copy the result into the output string
	 * and clear the print buffer. */
	struct _stp_log *log;
	unsigned long flags;
	int bytes;

	if (!_stp_print_trylock_irqsave(&flags)) {
		*str = '\0';
		return;
	}

	log = per_cpu_ptr(_stp_log_pcpu, raw_smp_processor_id());
	__stp_sprint_begin(log);
	_stp_stack_kernel_print(c, sym_flags);
	bytes = min_t(int, size - 1, log->len);
	memcpy(str, log->buf, bytes);
	str[bytes] = '\0';
	__stp_sprint_end(log);
	_stp_print_unlock_irqrestore(&flags);
}

static void _stp_stack_user_sprint(char *str, int size, struct context* c,
				   int sym_flags)
{
	/* To get an hex string, we use a simple trick.
	 * First flush the print buffer,
	 * then call _stp_stack_print,
	 * then copy the result into the output string
	 * and clear the print buffer. */
	struct _stp_log *log;
	unsigned long flags;
	int bytes;

	if (!_stp_print_trylock_irqsave(&flags)) {
		*str = '\0';
		return;
	}

	log = per_cpu_ptr(_stp_log_pcpu, raw_smp_processor_id());
	__stp_sprint_begin(log);
	_stp_stack_user_print(c, sym_flags);
	bytes = min_t(int, size - 1, log->len);
	memcpy(str, log->buf, bytes);
	str[bytes] = '\0';
	__stp_sprint_end(log);
	_stp_print_unlock_irqrestore(&flags);
}

#endif /* _STACK_C_ */
