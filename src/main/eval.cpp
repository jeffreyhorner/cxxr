/*CXXR $Id$
 *CXXR
 *CXXR This file is part of CXXR, a project to refactor the R interpreter
 *CXXR into C++.  It may consist in whole or in part of program code and
 *CXXR documentation taken from the R project itself, incorporated into
 *CXXR CXXR (and possibly MODIFIED) under the terms of the GNU General Public
 *CXXR Licence.
 *CXXR 
 *CXXR CXXR is Copyright (C) 2008-10 Andrew R. Runnalls, subject to such other
 *CXXR copyrights and copyright restrictions as may be stated below.
 *CXXR 
 *CXXR CXXR is not part of the R project, and bugs and other issues should
 *CXXR not be reported via r-bugs or other R project channels; instead refer
 *CXXR to the CXXR website.
 *CXXR */

/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996	Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998--2009	The R Development Core Team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

/** @file eval.cpp
 *
 * General evaluation of expressions, including implementation of R flow
 * control constructs, and R profiling.
 */

#undef HASHING

// For debugging:
#include <iostream>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <Defn.h>
#include <Rinterface.h>
#include <Fileio.h>
#include "arithmetic.h"
#include "basedecl.h"

#include "CXXR/BailoutContext.hpp"
#include "CXXR/ByteCode.hpp"
#include "CXXR/ClosureContext.hpp"
#include "CXXR/DottedArgs.hpp"
#include "CXXR/LoopBailout.hpp"
#include "CXXR/LoopException.hpp"
#include "CXXR/ReturnBailout.hpp"
#include "CXXR/ReturnException.hpp"

using namespace std;
using namespace CXXR;

#ifdef BYTECODE
static SEXP bcEval(SEXP, SEXP);
#endif

/*#define BC_PROFILING*/
#ifdef BC_PROFILING
static Rboolean bc_profiling = FALSE;
#endif

#ifdef R_PROFILING

/* BDR 2000-07-15
   Profiling is now controlled by the R function Rprof(), and should
   have negligible cost when not enabled.
*/

/* A simple mechanism for profiling R code.  When R_PROFILING is
   enabled, eval will write out the call stack every PROFSAMPLE
   microseconds using the SIGPROF handler triggered by timer signals
   from the ITIMER_PROF timer.  Since this is the same timer used by C
   profiling, the two cannot be used together.  Output is written to
   the file PROFOUTNAME.  This is a plain text file.  The first line
   of the file contains the value of PROFSAMPLE.  The remaining lines
   each give the call stack found at a sampling point with the inner
   most function first.

   To enable profiling, recompile eval.c with R_PROFILING defined.  It
   would be possible to selectively turn profiling on and off from R
   and to specify the file name from R as well, but for now I won't
   bother.

   The stack is traced by walking back along the context stack, just
   like the traceback creation in jump_to_toplevel.  One drawback of
   this approach is that it does not show BUILTIN's since they don't
   get a context.  With recent changes to pos.to.env it seems possible
   to insert a context around BUILTIN calls to that they show up in
   the trace.  Since there is a cost in establishing these contexts,
   they are only inserted when profiling is enabled. [BDR: we have since
   also added contexts for the BUILTIN calls to foreign code.]

   One possible advantage of not tracing BUILTIN's is that then
   profiling adds no cost when the timer is turned off.  This would be
   useful if we want to allow profiling to be turned on and off from
   within R.

   One thing that makes interpreting profiling output tricky is lazy
   evaluation.  When an expression f(g(x)) is profiled, lazy
   evaluation will cause g to be called inside the call to f, so it
   will appear as if g is called by f.

   L. T.  */

#ifdef Win32
# define WIN32_LEAN_AND_MEAN 1
# include <windows.h>		/* for CreateEvent, SetEvent */
# include <process.h>		/* for _beginthread, _endthread */
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# endif
# include <signal.h>
#endif /* not Win32 */

static FILE *R_ProfileOutfile = NULL;
static int R_Mem_Profiling=0;
extern void get_current_mem(unsigned long *,unsigned long *,unsigned long *); /* in memory.c */
extern unsigned long get_duplicate_counter(void);  /* in duplicate.c */
extern void reset_duplicate_counter(void);         /* in duplicate.c */

#ifdef Win32
HANDLE MainThread;
HANDLE ProfileEvent;

static void doprof(void)
{
    Evaluator::Context *cptr;
    char buf[1100];
    unsigned long bigv, smallv, nodes;
    int len;

    buf[0] = '\0';
    SuspendThread(MainThread);
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if((len = strlen(buf)) < 1000) {
		sprintf(buf+len, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    }
	    reset_duplicate_counter();
    }
    for (cptr = Evaluator::Context::innermost(); cptr; cptr = cptr->nextOut()) {
	if (TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    if(strlen(buf) < 1000) {
		strcat(buf, TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		       "<Anonymous>");
		strcat(buf, " ");
	    }
	}
    }
    ResumeThread(MainThread);
    if(strlen(buf))
	fprintf(R_ProfileOutfile, "%s\n", buf);
}

/* Profiling thread main function */
static void __cdecl ProfileThread(void *pwait)
{
    int wait = *((int *)pwait);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while(WaitForSingleObject(ProfileEvent, wait) != WAIT_OBJECT_0) {
	doprof();
    }
}
#else /* not Win32 */
static void doprof(int sig)
{
    FunctionContext *cptr;
    int newline = 0;
    unsigned long bigv, smallv, nodes;
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    reset_duplicate_counter();
    }
    for (cptr = FunctionContext::innermost();
	 cptr;
	 cptr = FunctionContext::innermost(cptr->nextOut())) {
	if (TYPEOF(cptr->call()) == LANGSXP) {
	    SEXP fun = CAR(cptr->call());
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, "\"%s\" ",
		    TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		    "<Anonymous>");
	}
    }
    if (newline) fprintf(R_ProfileOutfile, "\n");
    signal(SIGPROF, doprof);
}

static void doprof_null(int sig)
{
    signal(SIGPROF, doprof_null);
}
#endif /* not Win32 */


static void R_EndProfiling(void)
{
#ifdef Win32
    SetEvent(ProfileEvent);
    CloseHandle(MainThread);
#else /* not Win32 */
    struct itimerval itv;

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    signal(SIGPROF, doprof_null);
#endif /* not Win32 */
    if(R_ProfileOutfile) fclose(R_ProfileOutfile);
    R_ProfileOutfile = NULL;
    Evaluator::enableProfiling(false);
}

#if !defined(Win32) && defined(_R_HAVE_TIMING_)
// defined in unix/sys-unix.c
//double R_getClockIncrement(void);  // Use header files! 2007/06/11 arr
#endif

static void R_InitProfiling(SEXP filename, int append, double dinterval, int mem_profiling)
{
#ifndef Win32
    struct itimerval itv;
#else
    int wait;
    HANDLE Proc = GetCurrentProcess();
#endif
    int interval;

#if !defined(Win32) && defined(_R_HAVE_TIMING_)
    /* according to man setitimer, it waits until the next clock
       tick, usually 10ms, so avoid too small intervals here
    double clock_incr = R_getClockIncrement();
    int nclock = CXXRCONSTRUCT(int, floor(dinterval/clock_incr + 0.5));
    interval = 1e6 * ((nclock > 1)?nclock:1) * clock_incr + 0.5; */
    interval = CXXRCONSTRUCT(int, 1e6 * dinterval + 0.5);
#else
    interval = CXXRCONSTRUCT(int, 1e6 * dinterval + 0.5);
#endif
    if(R_ProfileOutfile != NULL) R_EndProfiling();
    R_ProfileOutfile = RC_fopen(filename, append ? "a" : "w", TRUE);
    if (R_ProfileOutfile == NULL)
	error(_("Rprof: cannot open profile file '%s'"),
	      translateChar(filename));
    if(mem_profiling)
	fprintf(R_ProfileOutfile, "memory profiling: sample.interval=%d\n", interval);
    else
	fprintf(R_ProfileOutfile, "sample.interval=%d\n", interval);

    R_Mem_Profiling=mem_profiling;
    if (mem_profiling)
	reset_duplicate_counter();

#ifdef Win32
    /* need to duplicate to make a real handle */
    DuplicateHandle(Proc, GetCurrentThread(), Proc, &MainThread,
		    0, FALSE, DUPLICATE_SAME_ACCESS);
    wait = interval/1000;
    if(!(ProfileEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) ||
       (_beginthread(ProfileThread, 0, &wait) == -1))
	R_Suicide("unable to create profiling thread");
    Sleep(wait/2); /* suspend this thread to ensure that the other one starts */
#else /* not Win32 */
    signal(SIGPROF, doprof);

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = interval;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = interval;
    if (setitimer(ITIMER_PROF, &itv, NULL) == -1)
	R_Suicide("setting profile timer failed");
#endif /* not Win32 */
    Evaluator::enableProfiling(true);
}

SEXP attribute_hidden do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP filename;
    int append_mode, mem_profiling;
    double dinterval;

#ifdef BC_PROFILING
    if (bc_profiling) {
	warning(_("can't use R profiling while byte code profiling"));
	return R_NilValue;
    }
#endif
    checkArity(op, args);
    if (!isString(CAR(args)) || (LENGTH(CAR(args))) != 1)
	error(_("invalid '%s' argument"), "filename");
    append_mode = asLogical(CADR(args));
    dinterval = asReal(CADDR(args));
    mem_profiling = asLogical(CADDDR(args));
    filename = STRING_ELT(CAR(args), 0);
    if (LENGTH(filename))
	R_InitProfiling(filename, append_mode, dinterval, mem_profiling);
    else
	R_EndProfiling();
    return R_NilValue;
}
#else /* not R_PROFILING */
SEXP attribute_hidden do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    error(_("R profiling is not available on this system"));
    return R_NilValue;		/* -Wall */
}
#endif /* not R_PROFILING */

/* NEEDED: A fixup is needed in browser, because it can trap errors,
 *	and currently does not reset the limit to the right value. */

static SEXP forcePromise(SEXP e)
{
    Promise* prom = SEXP_downcast<Promise*>(e);
    return prom->force();
}

/* Return value of "e" evaluated in "rho". */

SEXP eval(SEXP e, SEXP rho)
{
    Environment* env = 0;
    if (e)
	env = SEXP_downcast<Environment*>(rho);	
    return evaluate(e, env);
}

attribute_hidden
void SrcrefPrompt(const char * prefix, SEXP srcref)
{
    /* If we have a valid srcref, use it */
    if (srcref && srcref != R_NilValue) {
        if (TYPEOF(srcref) == VECSXP) srcref = VECTOR_ELT(srcref, 0);
	SEXP srcfile = getAttrib(srcref, R_SrcfileSymbol);
	if (TYPEOF(srcfile) == ENVSXP) {
	    SEXP filename = findVar(install("filename"), srcfile);
	    if (TYPEOF(filename) == STRSXP && length(filename) == 1) {
	    	Rprintf(_("%s at %s#%d: "), prefix, CHAR(STRING_ELT(filename, 0)), 
	                                    asInteger(srcref));
	        return;
	    }
	}
    }
    /* default: */
    Rprintf("%s: ", prefix);
}

/* Apply SEXP op of type CLOSXP to actuals */
// In CXXR, applyClosure() currently exists alongside
// Closure::apply(), with neither calling the other.  The resulting
// code duplication will be rectified in due course.

SEXP applyClosure(SEXP call, SEXP op, SEXP arglist, SEXP rho, SEXP suppliedenv)
{
    SEXP body, formals, savedrho;
    GCStackRoot<> actuals;
    GCStackRoot<Environment> newrho;
    SEXP f, a;
    GCStackRoot<> tmp;

    /* formals = list of formal parameters */
    /* actuals = values to be bound to formals */
    /* arglist = the tagged list of arguments */

    formals = FORMALS(op);
    body = BODY(op);
    savedrho = CLOENV(op);

    /*  Set up a context with the call in it so error has access to it */
    {
        Expression* callx = SEXP_downcast<Expression*>(call);
	Environment* call_env = SEXP_downcast<Environment*>(rho);
	Closure* func = SEXP_downcast<Closure*>(op);
	Environment* working_env = SEXP_downcast<Environment*>(savedrho);
	PairList* promargs = SEXP_downcast<PairList*>(arglist);
	ClosureContext cntxt(callx, call_env, func, working_env, promargs);

	/*  Build a list which matches the actual (unevaluated) arguments
	    to the formal paramters.  Build a new environment which
	    contains the matched pairs.  Ideally this environment sould be
	    hashed.  */

	actuals = matchArgs(formals, arglist, call);
	newrho = static_cast<Environment*>(NewEnvironment(formals, actuals,
							  savedrho));

	/*  Use the default code for unbound formals.  FIXME: It looks like
	    this code should preceed the building of the environment so that
	    this will also go into the hash table.  */

	/* This piece of code is destructively modifying the actuals list,
	   which is now also the list of bindings in the frame of newrho.
	   This is one place where internal structure of environment
	   bindings leaks out of envir.c.  It should be rewritten
	   eventually so as not to break encapsulation of the internal
	   environment layout.  We can live with it for now since it only
	   happens immediately after the environment creation.  LT */

	// The above rewriting is in progress for CXXR.

	f = formals;
	a = actuals;
	while (f != R_NilValue) {
	    if (CAR(a) == R_MissingArg && CAR(f) != R_MissingArg) {
		const Symbol* symbol = static_cast<Symbol*>(TAG(a));
		Frame::Binding* bdg = newrho->frame()->binding(symbol);
		bdg->setValue(mkPROMISE(CAR(f), newrho),
			      Frame::Binding::DEFAULTED);
	    }
	    f = CDR(f);
	    a = CDR(a);
	}

	/*  Fix up any extras that were supplied by usemethod. */

	// 2009-02-11: The test '&& suppliedenv != R_BaseEnv' is added in
	// CXXR.  CR at this point appears to rely on the fact that in CR,
	// the FRAME of the base environment will be empty, which is no
	// longer true in CXXR.  Possibly the base namespace ought also to
	// be excluded at this point.  ARR.
	if (suppliedenv != R_NilValue && suppliedenv != R_BaseEnv) {
	    for (tmp = FRAME(suppliedenv); tmp != R_NilValue; tmp = CDR(tmp)) {
		for (a = actuals; a != R_NilValue; a = CDR(a))
		    if (TAG(a) == TAG(tmp))
			break;
		if (a == R_NilValue)
		    /* Use defineVar instead of earlier version that added
		       bindings manually */
		    defineVar(TAG(tmp), CAR(tmp), newrho);
	    }
	}

	/*  Terminate the previous context and start a new one with the
	    correct environment. */
    }

    /*  If we have a generic function we need to use the sysparent of
	the generic as the sysparent of the method because the method
	is a straight substitution of the generic.  */
    {
        Environment* syspar = SEXP_downcast<Environment*>(rho);
	{
	    ClosureContext* innerctxt = ClosureContext::innermost();
	    if (innerctxt && innerctxt->isGeneric())
		syspar = innerctxt->callEnvironment();
	}
	Expression* callx = SEXP_downcast<Expression*>(call);
	Closure* func = SEXP_downcast<Closure*>(op);
	PairList* promargs = SEXP_downcast<PairList*>(arglist);
	ClosureContext cntxt(callx, syspar, func, newrho, promargs);

	/* The default return value is NULL.  FIXME: Is this really needed
	   or do we always get a sensible value returned?  */

	tmp = R_NilValue;

	/* Debugging */

	SET_ENV_DEBUG(newrho, CXXRCONSTRUCT(Rboolean, RDEBUG(op) || RSTEP(op)));
	if( RSTEP(op) ) SET_RSTEP(op, 0);
	if (ENV_DEBUG(newrho)) {
	    int old_bl = R_BrowseLines,
		blines = asInteger(GetOption(install("deparse.max.lines"),
					     R_BaseEnv));
	    Rprintf("debugging in: ");
	    if(blines != NA_INTEGER && blines > 0)
		R_BrowseLines = blines;
	    PrintValueRec(call,rho);
	    R_BrowseLines = old_bl;

	    /* Is the body a bare symbol (PR#6804) */
	    if (!isSymbol(body) & !isVectorAtomic(body)){
		/* Find out if the body is function with only one statement. */
		if (isSymbol(CAR(body)))
		    tmp = findFun(CAR(body), rho);
		else
		    tmp = eval(CAR(body), rho);
		if((TYPEOF(tmp) == BUILTINSXP || TYPEOF(tmp) == SPECIALSXP)
		   && !strcmp( PRIMNAME(tmp), "for")
		   && !strcmp( PRIMNAME(tmp), "{")
		   && !strcmp( PRIMNAME(tmp), "repeat")
		   && !strcmp( PRIMNAME(tmp), "while")
		   )
		    goto regdb;
	    }
	    SrcrefPrompt("debug", getAttrib(body, R_SrcrefSymbol));
	    PrintValue(body);
	    do_browser(call, op, R_NilValue, newrho);
	}

    regdb:
	Environment::ReturnScope returnscope(newrho);
	try {
	    {
		BailoutContext boctxt;
		tmp = Evaluator::evaluate(body, newrho);
	    }
	    if (tmp && tmp->sexptype() == BAILSXP) {
		ReturnBailout* rbo = dynamic_cast<ReturnBailout*>(tmp.get());
		if (!rbo || rbo->environment() != newrho)
		    abort();
		R_Visible = Rboolean(rbo->printResult());
		tmp = rbo->value();
	    }
	}
	catch (ReturnException& rx) {
	    if (rx.environment() != newrho)
		throw;
	    tmp = rx.value();
	}
    }

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
	PrintValueRec(call, rho);
    }
    Environment::monitorLeaks(tmp);
    newrho->maybeDetachFrame();
    return (tmp);
}

Closure::DebugScope::DebugScope(const ClosureContext& context,
				Environment* argsenv)
    : m_context(context), m_argsenv(argsenv)
{
    Closure* closure = static_cast<Closure*>(m_context.function());
    if (closure->debugging()) {
	Expression* call = m_context.call();
	Environment* working_env = m_context.workingEnvironment();
	working_env->setSingleStepping(true);
	Rprintf("debugging in: ");
	// Print call:
	{
	    int old_bl = R_BrowseLines;
	    int blines = asInteger(GetOption(install("deparse.max.lines"),
					     R_BaseEnv));
	    if(blines != NA_INTEGER && blines > 0)
		R_BrowseLines = blines;
	    Rf_PrintValueRec(call, argsenv);
	    R_BrowseLines = old_bl;
	}
	RObject* body = closure->m_body;
	/* Is the body a bare symbol (PR#6804) */
	if (!isSymbol(body) & !isVectorAtomic(body)){
	    /* Find out if the body is function with only one statement. */
	    RObject* tmp;
	    if (isSymbol(CAR(body)))
		tmp = findFun(CAR(body), argsenv);
	    else
		tmp = eval(CAR(body), argsenv);
	    if((TYPEOF(tmp) == BUILTINSXP || TYPEOF(tmp) == SPECIALSXP)
	       && !strcmp( PRIMNAME(tmp), "for")
	       && !strcmp( PRIMNAME(tmp), "{")
	       && !strcmp( PRIMNAME(tmp), "repeat")
	       && !strcmp( PRIMNAME(tmp), "while")
	       )
		return;
	}
	Rprintf("debug: ");
	Rf_PrintValue(body);
	do_browser(call, closure, m_context.promiseArgs(), working_env);
    }
}

Closure::DebugScope::~DebugScope()
{
    Closure* closure = static_cast<Closure*>(m_context.function());
    if (closure->debugging()) {
	try {
	    Rprintf("exiting from: ");
	    int old_bl = R_BrowseLines;
	    int blines
		= asInteger(GetOption(install("deparse.max.lines"), R_BaseEnv));
	    if(blines != NA_INTEGER && blines > 0)
		R_BrowseLines = blines;
	    Rf_PrintValueRec(m_context.call(),
			     const_cast<Environment*>(m_argsenv));
	    R_BrowseLines = old_bl;
	}
	// Don't allow exceptions to escape destructor:
	catch (...) {}
    }
}   

/* **** FIXME: This code is factored out of applyClosure.  If we keep
   **** it we should change applyClosure to run through this routine
   **** to avoid code drift. */
static SEXP R_execClosure(SEXP call, SEXP op, SEXP arglist, SEXP rho,
			  SEXP newrho)
{
    SEXP body;
    GCStackRoot<> tmp;

    body = BODY(op);

    {
        Expression* callx = SEXP_downcast<Expression*>(call);
	Environment* call_env = SEXP_downcast<Environment*>(rho);
	Closure* func = SEXP_downcast<Closure*>(op);
	Environment* working_env = SEXP_downcast<Environment*>(newrho);
	PairList* promargs = SEXP_downcast<PairList*>(arglist);
	ClosureContext cntxt(callx, call_env, func, working_env, promargs);

	/* The default return value is NULL.  FIXME: Is this really needed
	   or do we always get a sensible value returned?  */

	tmp = R_NilValue;

	/* Debugging */

	SET_ENV_DEBUG(newrho, CXXRCONSTRUCT(Rboolean, RDEBUG(op) || RSTEP(op)));
	if( RSTEP(op) ) SET_RSTEP(op, 0);
	if (RDEBUG(op)) {
	    Rprintf("debugging in: ");
	    PrintValueRec(call,rho);
	    /* Find out if the body is function with only one statement. */
	    if (isSymbol(CAR(body)))
		tmp = findFun(CAR(body), rho);
	    else
		tmp = eval(CAR(body), rho);
	    if((TYPEOF(tmp) == BUILTINSXP || TYPEOF(tmp) == SPECIALSXP)
	       && !strcmp( PRIMNAME(tmp), "for")
	       && !strcmp( PRIMNAME(tmp), "{")
	       && !strcmp( PRIMNAME(tmp), "repeat")
	       && !strcmp( PRIMNAME(tmp), "while")
	       )
		goto regdb;
	    SrcrefPrompt("debug", getAttrib(body, R_SrcrefSymbol));
	    PrintValue(body);
	    do_browser(call, op, R_NilValue, newrho);
	}

    regdb:
	Environment* envir = SEXP_downcast<Environment*>(newrho);
	Environment::ReturnScope returnscope(envir);
	try {
	    {
		BailoutContext boctxt;
		tmp = Evaluator::evaluate(body, envir);
	    }
	    if (tmp && tmp->sexptype() == BAILSXP) {
		ReturnBailout* rbo = dynamic_cast<ReturnBailout*>(tmp.get());
		if (!rbo || rbo->environment() != envir)
		    abort();
		R_Visible = Rboolean(rbo->printResult());
		tmp = rbo->value();
	    }
	}
	catch (ReturnException& rx) {
	    if (rx.environment() != envir)
		throw;
	    tmp = rx.value();
	}
    }

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
	PrintValueRec(call, rho);
    }
    return (tmp);
}

/* **** FIXME: Temporary code to execute S4 methods in a way that
   **** preserves lexical scope. */

static SEXP R_dot_Generic = NULL;
static SEXP R_dot_Method = NULL;
static SEXP R_dot_Methods = NULL;
static SEXP R_dot_defined = NULL;
static SEXP R_dot_target = NULL;

/* called from methods_list_dispatch.c */
SEXP R_execMethod(SEXP op, SEXP rho)
{
    SEXP call, arglist, callerenv, newrho, next, val;
    ClosureContext *cptr;

    if (R_dot_Generic == NULL) {
	R_dot_Generic = install(".Generic");
	R_dot_Method = install(".Method");
	R_dot_Methods = install(".Methods");
	R_dot_defined = install(".defined");
	R_dot_target = install(".target");
    }

    /* create a new environment frame enclosed by the lexical
       environment of the method */
    PROTECT(newrho = Rf_NewEnvironment(R_NilValue, R_NilValue, CLOENV(op)));

    /* copy the bindings for the formal environment from the top frame
       of the internal environment of the generic call to the new
       frame.  need to make sure missingness information is preserved
       and the environments for any default expression promises are
       set to the new environment.  should move this to envir.c where
       it can be done more efficiently. */
    for (next = FORMALS(op); next != R_NilValue; next = CDR(next)) {
	SEXP symbol =  TAG(next);
	R_varloc_t loc;
	int missing;
	loc = R_findVarLocInFrame(rho,symbol);
	if(loc == NULL)
	    error(_("could not find symbol \"%s\" in environment of the generic function"),
		  CHAR(PRINTNAME(symbol)));
	missing = R_GetVarLocMISSING(loc);
	val = R_GetVarLocValue(loc);
	Environment* newenv = static_cast<Environment*>(newrho);
	const Symbol* sym = static_cast<Symbol*>(symbol);
	Frame::Binding* bdg = newenv->frame()->obtainBinding(sym);
	if (missing) {
	    if (TYPEOF(val) == PROMSXP && PRENV(val) == rho) {
		SEXP deflt;
		/* find the symbol in the method, copy its expression
		 * to the promise */
		for(deflt = FORMALS(op); deflt != R_NilValue; deflt = CDR(deflt)) {
		    if(TAG(deflt) == symbol)
			break;
		}
		if(deflt == R_NilValue)
		    error(_("symbol \"%s\" not in environment of method"),
			  CHAR(PRINTNAME(symbol)));
		val = mkPROMISE(CAR(deflt), newrho);
	    }
	}
	bdg->setValue(val, Frame::Binding::Origin(missing));
    }

    /* copy the bindings of the spacial dispatch variables in the top
       frame of the generic call to the new frame */
    defineVar(R_dot_defined, findVarInFrame(rho, R_dot_defined), newrho);
    defineVar(R_dot_Method, findVarInFrame(rho, R_dot_Method), newrho);
    defineVar(R_dot_target, findVarInFrame(rho, R_dot_target), newrho);

    /* copy the bindings for .Generic and .Methods.  We know (I think)
       that they are in the second frame, so we could use that. */
    defineVar(R_dot_Generic, findVar(R_dot_Generic, rho), newrho);
    defineVar(R_dot_Methods, findVar(R_dot_Methods, rho), newrho);

    /* Find the calling context.  Should be the innermost Context unless
       profiling has inserted a Context::BUILTIN frame. */
    cptr = ClosureContext::innermost();

    /* The calling environment should either be the environment of the
       generic, rho, or the environment of the caller of the generic,
       the current sysparent. */
    callerenv = cptr->callEnvironment(); /* or rho? */

    /* get the rest of the stuff we need from the current context,
       execute the method, and return the result */
    call = cptr->call();
    arglist = cptr->promiseArgs();
    val = R_execClosure(call, op, arglist, callerenv, newrho);
    UNPROTECT(1);
    return val;
}

static SEXP EnsureLocal(SEXP symbol, SEXP rho)
{
    SEXP vl;

    if ((vl = findVarInFrame3(rho, symbol, TRUE)) != R_UnboundValue) {
	vl = eval(symbol, rho);	/* for promises */
	if(NAMED(vl) == 2) {
	    PROTECT(vl = duplicate(vl));
	    defineVar(symbol, vl, rho);
	    UNPROTECT(1);
	}
	return vl;
    }

    vl = eval(symbol, ENCLOS(rho));
    if (vl == R_UnboundValue)
	error(_("object '%s' not found"), CHAR(PRINTNAME(symbol)));

    PROTECT(vl = duplicate(vl));
    defineVar(symbol, vl, rho);
    UNPROTECT(1);
    SET_NAMED(vl, 1);
    return vl;
}


/* Note: If val is a language object it must be protected */
/* to prevent evaluation.  As an example consider */
/* e <- quote(f(x=1,y=2); names(e) <- c("","a","b") */

static SEXP replaceCall(SEXP fun, SEXP val, SEXP args, SEXP rhs)
{
    SEXP tmp, ptmp;
    PROTECT(fun);
    PROTECT(args);
    PROTECT(rhs);
    PROTECT(val);
    GCStackRoot<PairList> tl(PairList::makeList(length(args) + 2));
    ptmp = tmp = GCNode::expose(new Expression(0, tl));
    UNPROTECT(4);
    SETCAR(ptmp, fun); ptmp = CDR(ptmp);
    SETCAR(ptmp, val); ptmp = CDR(ptmp);
    while(args != R_NilValue) {
	SETCAR(ptmp, CAR(args));
	SET_TAG(ptmp, TAG(args));
	ptmp = CDR(ptmp);
	args = CDR(args);
    }
    SETCAR(ptmp, rhs);
    SET_TAG(ptmp, install("value"));
    return tmp;
}


static SEXP assignCall(SEXP op, SEXP symbol, SEXP fun,
		       SEXP val, SEXP args, SEXP rhs)
{
    PROTECT(op);
    PROTECT(symbol);
    val = replaceCall(fun, val, args, rhs);
    UNPROTECT(2);
    return lang3(op, symbol, val);
}


static R_INLINE Rboolean asLogicalNoNA(SEXP s, SEXP call)
{
    Rboolean cond = CXXRCONSTRUCT(Rboolean, NA_LOGICAL);

    if (length(s) > 1)
	warningcall(call,
		    _("the condition has length > 1 and only the first element will be used"));
    if (length(s) > 0) {
	/* inline common cases for efficiency */
	switch(TYPEOF(s)) {
	case LGLSXP:
	    cond = CXXRCONSTRUCT(Rboolean, LOGICAL(s)[0]);
	    break;
	case INTSXP:
	    cond = CXXRCONSTRUCT(Rboolean, INTEGER(s)[0]); /* relies on NA_INTEGER == NA_LOGICAL */
	    break;
	default:
	    cond = CXXRCONSTRUCT(Rboolean, asLogical(s));
	}
    }

    if (cond == NA_LOGICAL) {
	char *msg = length(s) ? (isLogical(s) ?
				 _("missing value where TRUE/FALSE needed") :
				 _("argument is not interpretable as logical")) :
	    _("argument is of length zero");
	errorcall(call, msg);
    }
    return cond;
}


SEXP attribute_hidden do_if(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP Cond, Stmt=R_NilValue;
    int vis=0;

    PROTECT(Cond = eval(CAR(args), rho));
    if (asLogicalNoNA(Cond, call)) 
        Stmt = CAR(CDR(args));
    else {
        if (length(args) > 2) 
           Stmt = CAR(CDR(CDR(args)));
        else
           vis = 1;
    } 
    if( ENV_DEBUG(rho) ) {
	SrcrefPrompt("debug", R_Srcref);
        PrintValue(Stmt);
        do_browser(call, op, R_NilValue, rho);
    } 
    UNPROTECT(1);
    if( vis ) {
        R_Visible = FALSE; /* case of no 'else' so return invisible NULL */
        return Stmt;
    }

    {
	RObject* ans;
	{
	    BailoutContext bcntxt;
	    ans = eval(Stmt, rho);
	}
	if (ans && ans->sexptype() == BAILSXP) {
	    Evaluator::Context* callctxt
		= Evaluator::Context::innermost()->nextOut();
	    if (!callctxt || callctxt->type() != Evaluator::Context::BAILOUT)
		static_cast<Bailout*>(ans)->throwException();
	}
	return ans;
    }
}

namespace {
    inline int BodyHasBraces(SEXP body)
    {
	return (isLanguage(body) && CAR(body) == R_BraceSymbol) ? 1 : 0;
    }

    inline void DO_LOOP_RDEBUG(SEXP call, SEXP op, SEXP args, SEXP rho, int bgn)
    {
	if (bgn && ENV_DEBUG(rho)) {
	    SrcrefPrompt("debug", R_Srcref);
	    PrintValue(CAR(args));
	    do_browser(call, op, 0, rho);
	}
    }
}

SEXP attribute_hidden do_for(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    Rboolean dbg;
    int nm;
    volatile int i, n, bgn;
    SEXP sym, body;
    GCStackRoot<> ans, v, val;
    GCStackRoot<> argsrt(args), rhort(rho);

    sym = CAR(args);
    val = CADR(args);
    body = CADDR(args);

    if ( !isSymbol(sym) ) errorcall(call, _("non-symbol loop variable"));

    val = eval(val, rho);
    defineVar(sym, R_NilValue, rho);

    /* deal with the case where we are iterating over a factor
       we need to coerce to character - then iterate */

    if( inherits(val, "factor") ) {
        ans = asCharacterFactor(val);
	val = ans;
    }

    if (isList(val) || isNull(val)) {
	n = length(val);
	v = 0;
    }
    else {
	n = LENGTH(val);
	v = allocVector(TYPEOF(val), 1);
    }
    ans = R_NilValue;

    dbg = ENV_DEBUG(rho);
    bgn = BodyHasBraces(body);

    /* bump up NAMED count of sequence to avoid modification by loop code */
    if (NAMED(val) < 2) SET_NAMED(val, NAMED(val) + 1);

    /***** nm may not be needed anymore now that NAMED(val) is at
	   least 1.  LT */
    nm = NAMED(val);

    Environment* env = SEXP_downcast<Environment*>(rho);
    Environment::LoopScope loopscope(env);
    for (i = 0; i < n; i++) {
	DO_LOOP_RDEBUG(call, op, args, rho, bgn);
	switch (TYPEOF(val)) {
	case LGLSXP:
	    v = allocVector(TYPEOF(val), 1);
	    LOGICAL(v)[0] = LOGICAL(val)[i];
	    setVar(sym, v, rho);
	    break;
	case INTSXP:
	    v = allocVector(TYPEOF(val), 1);
	    INTEGER(v)[0] = INTEGER(val)[i];
	    setVar(sym, v, rho);
	    break;
	case REALSXP:
	    v = allocVector(TYPEOF(val), 1);
	    REAL(v)[0] = REAL(val)[i];
	    setVar(sym, v, rho);
	    break;
	case CPLXSXP:
	    v = allocVector(TYPEOF(val), 1);
	    COMPLEX(v)[0] = COMPLEX(val)[i];
	    setVar(sym, v, rho);
	    break;
	case STRSXP:
	    v = allocVector(TYPEOF(val), 1);
	    SET_STRING_ELT(v, 0, STRING_ELT(val, i));
	    setVar(sym, v, rho);
	    break;
	case RAWSXP:
	    v = allocVector(TYPEOF(val), 1);
	    RAW(v)[0] = RAW(val)[i];
	    setVar(sym, v, rho);
	    break;
	case EXPRSXP:
	    /* make sure loop variable is a copy if needed */
	    if(nm > 0) SET_NAMED(XVECTOR_ELT(val, i), 2);
	    setVar(sym, XVECTOR_ELT(val, i), rho);
	    break;
	case VECSXP:
	    /* make sure loop variable is a copy if needed */
	    if(nm > 0) SET_NAMED(VECTOR_ELT(val, i), 2);
	    setVar(sym, VECTOR_ELT(val, i), rho);
	    break;
	case LISTSXP:
	    /* make sure loop variable is a copy if needed */
	    if(nm > 0) SET_NAMED(CAR(val), 2);
	    setVar(sym, CAR(val), rho);
	    val = CDR(val);
	    break;
	default:
	    errorcall(call, _("invalid for() loop sequence"));
	}
	try {
	    BailoutContext bcntxt;
	    ans = eval(body, rho);
	}
	catch (LoopException& lx) {
	    if (lx.environment() != env)
		throw;
	    if (lx.next())
		continue;
	    else break;
	}
	if (ans && ans->sexptype() == BAILSXP) {
	    LoopBailout* lbo = dynamic_cast<LoopBailout*>(ans.get());
	    if (lbo) {
		if (lbo->environment() != rho)
		    abort();
		if (lbo->next())
		    continue;
		else break;
	    } else {  // This must be a ReturnBailout:
		SET_ENV_DEBUG(rho, dbg);
		Evaluator::Context* callctxt
		    = Evaluator::Context::innermost()->nextOut();
		if (!callctxt
		    || callctxt->type() != Evaluator::Context::BAILOUT)
		    static_cast<Bailout*>(ans.get())->throwException();
		return ans;
	    }
	}
    }
    SET_ENV_DEBUG(rho, dbg);
    return R_NilValue;
}


SEXP attribute_hidden do_while(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    Rboolean dbg;
    volatile int bgn;
    GCStackRoot<> t;
    volatile SEXP body;

    checkArity(op, args);

    dbg = ENV_DEBUG(rho);
    body = CADR(args);
    bgn = BodyHasBraces(body);

    Environment* env = SEXP_downcast<Environment*>(rho);
    Environment::LoopScope loopscope(env);

    while (asLogicalNoNA(eval(CAR(args), rho), call)) {
	RObject* ans;
	DO_LOOP_RDEBUG(call, op, args, rho, bgn);
	try {
	    BailoutContext bcntxt;
	    ans = eval(body, rho);
	}
	catch (LoopException& lx) {
	    if (lx.environment() != env)
		throw;
	    if (lx.next())
		continue;
	    else break;
	}
	if (ans && ans->sexptype() == BAILSXP) {
	    LoopBailout* lbo = dynamic_cast<LoopBailout*>(ans);
	    if (lbo) {
		if (lbo->environment() != rho)
		    abort();
		if (lbo->next())
		    continue;
		else break;
	    } else {  // This must be a ReturnBailout:
		SET_ENV_DEBUG(rho, dbg);
		Evaluator::Context* callctxt
		    = Evaluator::Context::innermost()->nextOut();
		if (!callctxt
		    || callctxt->type() != Evaluator::Context::BAILOUT)
		    static_cast<Bailout*>(ans)->throwException();
		return ans;
	    }
	}
    }
    SET_ENV_DEBUG(rho, dbg);
    return R_NilValue;
}


SEXP attribute_hidden do_repeat(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    Rboolean dbg;
    volatile int bgn;
    GCStackRoot<> t;
    volatile SEXP body;

    checkArity(op, args);

    dbg = ENV_DEBUG(rho);
    body = CAR(args);
    bgn = BodyHasBraces(body);

    Environment* env = SEXP_downcast<Environment*>(rho);
    Environment::LoopScope loopscope(env);
    for (;;) {
	RObject* ans;
	DO_LOOP_RDEBUG(call, op, args, rho, bgn);
	try {
	    BailoutContext bcntxt;
	    ans = eval(body, rho);
	}
	catch (LoopException& lx) {
	    if (lx.environment() != env)
		throw;
	    if (lx.next())
		continue;
	    else break;
	}
	if (ans && ans->sexptype() == BAILSXP) {
	    LoopBailout* lbo = dynamic_cast<LoopBailout*>(ans);
	    if (lbo) {
		if (lbo->environment() != rho)
		    abort();
		if (lbo->next())
		    continue;
		else break;
	    } else {  // This must be a ReturnBailout:
		SET_ENV_DEBUG(rho, dbg);
		Evaluator::Context* callctxt
		    = Evaluator::Context::innermost()->nextOut();
		if (!callctxt
		    || callctxt->type() != Evaluator::Context::BAILOUT)
		    static_cast<Bailout*>(ans)->throwException();
		return ans;
	    }
	}
    }

    SET_ENV_DEBUG(rho, dbg);
    return R_NilValue;
}


SEXP attribute_hidden do_break(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    Environment* env = SEXP_downcast<Environment*>(rho);
    if (!env->loopActive())
	Rf_error(_("no loop to break from, jumping to top level"));
    LoopBailout* lbo = GCNode::expose(new LoopBailout(env, PRIMVAL(op) == 1));
    Evaluator::Context* callctxt = Evaluator::Context::innermost()->nextOut();
    if (!callctxt || callctxt->type() != Evaluator::Context::BAILOUT)
	lbo->throwException();
    return lbo;
}


SEXP attribute_hidden do_paren(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return CAR(args);
}

/* this function gets the srcref attribute from a statement block, 
   and confirms it's in the expected format */
   
static SEXP getSrcrefs(SEXP call, SEXP args)
{
    SEXP srcrefs = getAttrib(call, R_SrcrefSymbol);
    if (   TYPEOF(srcrefs) == VECSXP
        && length(srcrefs) == length(args)+1 ) return srcrefs;
    return R_NilValue;
}

SEXP attribute_hidden do_begin(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP s = R_NilValue;
    if (args != R_NilValue) {
    	GCStackRoot<> srcrefs(getSrcrefs(call, args));
    	int i = 1;
    	R_Srcref = R_NilValue;
	while (args != R_NilValue) {
	    if (srcrefs != R_NilValue) {
	    	R_Srcref = VECTOR_ELT(srcrefs, i++);
	    	if (  TYPEOF(R_Srcref) != INTSXP
	    	    || length(R_Srcref) != 6) {
	    	    srcrefs = R_Srcref = R_NilValue;
	    	}
	    }
	    if (ENV_DEBUG(rho)) {
	    	SrcrefPrompt("debug", R_Srcref);
		PrintValue(CAR(args));
		do_browser(call, op, R_NilValue, rho);
	    }
	    {
		BailoutContext bcntxt;
		s = eval(CAR(args), rho);
	    }
	    if (s && s->sexptype() == BAILSXP) {
		Evaluator::Context* callctxt
		    = Evaluator::Context::innermost()->nextOut();
		if (!callctxt || callctxt->type() != Evaluator::Context::BAILOUT)
		    static_cast<Bailout*>(s)->throwException();
		R_Srcref = 0;
		return s;
	    }
	    args = CDR(args);
	}
	R_Srcref = R_NilValue;
    }
    return s;
}


SEXP attribute_hidden do_return(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    GCStackRoot<> v;

    if (args == R_NilValue) /* zero arguments provided */
	v = R_NilValue;
    else if (CDR(args) == R_NilValue) /* one argument */
	v = eval(CAR(args), rho);
    else {
	v = R_NilValue; /* to avoid compiler warnings */
	errorcall(call, _("multi-argument returns are not permitted"));
    }

    Environment* envir = SEXP_downcast<Environment*>(rho);
    if (!envir->canReturn())
	Rf_error(_("no function to return from, jumping to top level"));
    ReturnBailout* rbo = GCNode::expose(new ReturnBailout(envir, v));
    Evaluator::Context* callctxt = Evaluator::Context::innermost()->nextOut();
    if (!callctxt || callctxt->type() != Evaluator::Context::BAILOUT)
	rbo->throwException();
    return rbo;
}


SEXP attribute_hidden do_function(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    GCStackRoot<> rval;

    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMED(op, 2);
    }
    if (length(args) < 2)
	WrongArgCount("lambda");
    SEXP formals = CAR(args);
    if (formals && formals->sexptype() != LISTSXP)
	Rf_error(_("invalid formal argument list for 'function'"));
    rval = mkCLOSXP(formals, CADR(args), rho);
    setAttrib(rval, R_SourceSymbol, CADDR(args));
    return rval;
}


/*
 *  Assignments for complex LVAL specifications. This is the stuff that
 *  nightmares are made of ...	Note that "evalseq" preprocesses the LHS
 *  of an assignment.  Given an expression, it builds a list of partial
 *  values for the exression.  For example, the assignment x$a[3] <- 10
 *  with LHS x$a[3] yields the (improper) list:
 *
 *	 (eval(x$a[3])	eval(x$a)  eval(x)  .  x)
 *
 *  (Note the terminating symbol).  The partial evaluations are carried
 *  out efficiently using previously computed components.
 */

// CXXR here (necessarily) uses a proper list, with x as the CAR of
// the last element.

/*
  For complex superassignment  x[y==z]<<-w
  we want x required to be nonlocal, y,z, and w permitted to be local or nonlocal.
*/

static SEXP evalseq(SEXP expr, SEXP rho, int forcelocal,  R_varloc_t tmploc)
{
    SEXP val, nexpr;
    GCStackRoot<> nval;
    if (isNull(expr))
	error(_("invalid (NULL) left side of assignment"));
    if (isSymbol(expr)) {
	GCStackRoot<> exprr(expr);
	if(forcelocal) {
	    nval = EnsureLocal(expr, rho);
	}
	else {/* now we are down to the target symbol */
	  nval = eval(expr, ENCLOS(rho));
	}
	return PairList::construct(nval, PairList::construct(expr));
    }
    else if (isLanguage(expr)) {
	PROTECT(expr);
	PROTECT(val = evalseq(CADR(expr), rho, forcelocal, tmploc));
	R_SetVarLocValue(tmploc, CAR(val));
	PROTECT(nexpr = CONS(R_GetVarLocSymbol(tmploc), CDDR(expr)));
	PROTECT(nexpr = LCONS(CAR(expr), nexpr));
	nval = eval(nexpr, rho);
	UNPROTECT(4);
	return CONS(nval, val);
    }
    else error(_("target of assignment expands to non-language object"));
    return R_NilValue;	/*NOTREACHED*/
}

/* Main entry point for complex assignments */
/* We have checked to see that CAR(args) is a LANGSXP */

static const char * const asym[] = {":=", "<-", "<<-", "="};

static SEXP applydefine(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    GCStackRoot<> expr, lhs, rhs, saverhs, tmp, tmp2;
    R_varloc_t tmploc;
    char buf[32];

    expr = CAR(args);

    /*  It's important that the rhs get evaluated first because
	assignment is right associative i.e.  a <- b <- c is parsed as
	a <- (b <- c).  */

    saverhs = rhs = eval(CADR(args), rho);

    if (PRIMVAL(op) == 2)
	Environment::monitorLeaks(rhs);

    /*  FIXME: We need to ensure that this works for hashed
	environments.  This code only works for unhashed ones.  the
	syntax error here is a deliberate marker so I don't forget that
	this needs to be done.  The code used in "missing" will help
	here.  */

    /*  FIXME: This strategy will not work when we are working in the
	data frame defined by the system hash table.  The structure there
	is different.  Should we special case here?  */

    /*  We need a temporary variable to hold the intermediate values
	in the computation.  For efficiency reasons we record the
	location where this variable is stored.  */

    if (rho == R_BaseNamespace)
	errorcall(call, _("cannot do complex assignments in base namespace"));
    if (rho == R_BaseEnv)
	errorcall(call, _("cannot do complex assignments in base environment"));
    defineVar(R_TmpvalSymbol, R_NilValue, rho);
    tmploc = R_findVarLocInFrame(rho, R_TmpvalSymbol);
    /* Now use try-catch to remove it when we are done, even in the
     * case of an error.  This all helps error() provide a better call.
     */
    try {
	/*  Do a partial evaluation down through the LHS. */
	lhs = evalseq(CADR(expr), rho,
		      PRIMVAL(op)==1 || PRIMVAL(op)==3, tmploc);

	while (isLanguage(CADR(expr))) {
	    if (TYPEOF(CAR(expr)) != SYMSXP)
		error(_("invalid function in complex assignment"));
	    if(strlen(CHAR(PRINTNAME(CAR(expr)))) + 3 > 32)
		error(_("overlong name in '%s'"), CHAR(PRINTNAME(CAR(expr))));
	    sprintf(buf, "%s<-", CHAR(PRINTNAME(CAR(expr))));
	    tmp = install(buf);
	    R_SetVarLocValue(tmploc, CAR(lhs));
	    tmp2 = mkPROMISE(rhs, rho);
	    SET_PRVALUE(tmp2, rhs);
	    rhs = replaceCall(tmp, R_GetVarLocSymbol(tmploc), CDDR(expr), tmp2);
	    rhs = eval(rhs, rho);
	    lhs = CDR(lhs);
	    expr = CADR(expr);
	}
	if (TYPEOF(CAR(expr)) != SYMSXP)
	    error(_("invalid function in complex assignment"));
	if(strlen(CHAR(PRINTNAME(CAR(expr)))) + 3 > 32)
	    error(_("overlong name in '%s'"), CHAR(PRINTNAME(CAR(expr))));
	sprintf(buf, "%s<-", CHAR(PRINTNAME(CAR(expr))));
	R_SetVarLocValue(tmploc, CAR(lhs));
	tmp = mkPROMISE(CADR(args), rho);
	SET_PRVALUE(tmp, rhs);
        expr = assignCall(install(asym[PRIMVAL(op)]), CADR(lhs),  // CXXR change
			  install(buf), R_GetVarLocSymbol(tmploc),
			  CDDR(expr), tmp);
	expr = eval(expr, rho);
    }
    catch (...) {
	unbindVar(R_TmpvalSymbol, rho);
	throw;
    }

    unbindVar(R_TmpvalSymbol, rho);
#ifdef CONSERVATIVE_COPYING /* not default */
    return duplicate(saverhs);
#else
    /* we do not duplicate the value, so to be conservative mark the
       value as NAMED = 2 */
    SET_NAMED(saverhs, 2);
    return saverhs;
#endif
}

/* Defunct in 1.5.0
SEXP attribute_hidden do_alias(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op,args);
    Rprintf(".Alias is deprecated; there is no replacement \n");
    SET_NAMED(CAR(args), 0);
    return CAR(args);
}
*/

/*  Assignment in its various forms  */

SEXP attribute_hidden do_set(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP s;
    if (length(args) != 2)
	WrongArgCount(asym[PRIMVAL(op)]);
    if (isString(CAR(args))) {
	/* fix up a duplicate or args and recursively call do_set */
	SEXP val;
	PROTECT(args = duplicate(args));
	SETCAR(args, install(translateChar(STRING_ELT(CAR(args), 0))));
	val = do_set(call, op, args, rho);
	UNPROTECT(1);
	return val;
    }

    switch (PRIMVAL(op)) {
    case 1: case 3:					/* <-, = */
	if (isSymbol(CAR(args))) {
	    s = eval(CADR(args), rho);
#ifdef CONSERVATIVE_COPYING /* not default */
	    if (NAMED(s))
	    {
		SEXP t;
		PROTECT(s);
		t = duplicate(s);
		UNPROTECT(1);
		s = t;
	    }
	    PROTECT(s);
	    defineVar(CAR(args), s, rho);
	    UNPROTECT(1);
	    SET_NAMED(s, 1);
#else
	    switch (NAMED(s)) {
	    case 0: SET_NAMED(s, 1); break;
	    case 1: SET_NAMED(s, 2); break;
	    }
	    defineVar(CAR(args), s, rho);
#endif
	    R_Visible = FALSE;
	    return (s);
	}
	else if (isLanguage(CAR(args))) {
	    R_Visible = FALSE;
	    return applydefine(call, op, args, rho);
	}
	else errorcall(call,
		       _("invalid (do_set) left-hand side to assignment"));
    case 2:						/* <<- */
	if (isSymbol(CAR(args))) {
	    s = eval(CADR(args), rho);
	    Environment::monitorLeaks(s);
	    if (NAMED(s))
		s = duplicate(s);
	    PROTECT(s);
	    setVar(CAR(args), s, ENCLOS(rho));
	    UNPROTECT(1);
	    SET_NAMED(s, 1);
	    R_Visible = FALSE;
	    return s;
	}
	else if (isLanguage(CAR(args)))
	    return applydefine(call, op, args, rho);
	else error(_("invalid assignment left-hand side"));

    default:
	UNIMPLEMENTED("do_set");

    }
    return R_NilValue;/*NOTREACHED*/
}


/* Evaluate each expression in "el" in the environment "rho".  This is
   a naturally recursive algorithm, but we use the iterative form below
   because it is does not cause growth of the pointer protection stack,
   and because it is a little more efficient.
*/

#define COPY_TAG(to, from) do { \
  SEXP __tag__ = TAG(from); \
  if (__tag__ != R_NilValue) SET_TAG(to, __tag__); \
} while (0)

/* Used in eval and applyMethod (object.c) for builtin primitives,
   do_internal (names.c) for builtin .Internals
   and in evalArgs.

   'n' is the number of arguments already evaluated and hence not
   passed to evalArgs and hence to here.
 */
SEXP attribute_hidden evalList(SEXP el, SEXP rho, SEXP call, int n)
{
    SEXP ans, h, tail;

    PROTECT(ans = tail = CONS(R_NilValue, R_NilValue));

    while (el != R_NilValue) {
	n++;

	if (CAR(el) == R_DotsSymbol) {
	    /* If we have a ... symbol, we look to see what it is bound to.
	     * If its binding is Null (i.e. zero length)
	     *	we just ignore it and return the cdr with all its expressions evaluated;
	     * if it is bound to a ... list of promises,
	     *	we force all the promises and then splice
	     *	the list of resulting values into the return value.
	     * Anything else bound to a ... symbol is an error
	     */
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP || h == R_NilValue) {
		while (h != R_NilValue) {
		    SETCDR(tail, CONS(eval(CAR(h), rho), R_NilValue));
		    tail = CDR(tail);
		    COPY_TAG(tail, h);
		    h = CDR(h);
		}
	    }
	    else if (h != R_MissingArg)
		error(_("'...' used in an incorrect context"));
	} else if (CAR(el) == R_MissingArg) {
	    /* It was an empty element: most likely get here from evalArgs
	       which may have been called on part of the args. */
	    errorcall(call, _("argument %d is empty"), n);
	} else if (isSymbol(CAR(el)) && R_isMissing(CAR(el), rho)) {
	    /* It was missing */
	    errorcall(call, _("'%s' is missing"), CHAR(PRINTNAME(CAR(el)))); 
	} else {
	    SETCDR(tail, CONS(eval(CAR(el), rho), R_NilValue));
	    tail = CDR(tail);
	    COPY_TAG(tail, el);
	}
	el = CDR(el);
    }
    UNPROTECT(1);
    return CDR(ans);
} /* evalList() */


/* A slight variation of evaluating each expression in "el" in "rho". */

/* used in evalArgs, arithmetic.c, seq.c */
SEXP attribute_hidden evalListKeepMissing(SEXP el, SEXP rho)
{
    SEXP ans, h, tail;

    PROTECT(ans = tail = CONS(R_NilValue, R_NilValue));

    while (el != R_NilValue) {

	/* If we have a ... symbol, we look to see what it is bound to.
	 * If its binding is Null (i.e. zero length)
	 *	we just ignore it and return the cdr with all its expressions evaluated;
	 * if it is bound to a ... list of promises,
	 *	we force all the promises and then splice
	 *	the list of resulting values into the return value.
	 * Anything else bound to a ... symbol is an error
	*/
	if (CAR(el) == R_DotsSymbol) {
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP || h == R_NilValue) {
		while (h != R_NilValue) {
		    if (CAR(h) == R_MissingArg)
			SETCDR(tail, CONS(R_MissingArg, R_NilValue));
		    else
			SETCDR(tail, CONS(eval(CAR(h), rho), R_NilValue));
		    tail = CDR(tail);
		    COPY_TAG(tail, h);
		    h = CDR(h);
		}
	    }
	    else if(h != R_MissingArg)
		error(_("'...' used in an incorrect context"));
	}
	else if (CAR(el) == R_MissingArg ||
                 (isSymbol(CAR(el)) && R_isMissing(CAR(el), rho))) {
	    SETCDR(tail, CONS(R_MissingArg, R_NilValue));
	    tail = CDR(tail);
	    COPY_TAG(tail, el);
	}
	else {
	    SETCDR(tail, CONS(eval(CAR(el), rho), R_NilValue));
	    tail = CDR(tail);
	    COPY_TAG(tail, el);
	}
	el = CDR(el);
    }
    UNPROTECT(1);
    return CDR(ans);
}

static SEXP VectorToPairListNamed(SEXP x)
{
    SEXP xptr, xnew, xnames;
    int i, len = 0, named;

    PROTECT(x);
    PROTECT(xnames = getAttrib(x, R_NamesSymbol)); /* isn't this protected via x? */
    named = (xnames != R_NilValue);
    if(named)
	for (i = 0; i < length(x); i++)
	    if (CHAR(STRING_ELT(xnames, i))[0] != '\0') len++;

    if(len) {
	PROTECT(xnew = allocList(len));
	xptr = xnew;
	for (i = 0; i < length(x); i++) {
	    if (CHAR(STRING_ELT(xnames, i))[0] != '\0') {
		SETCAR(xptr, VECTOR_ELT(x, i));
		SET_TAG(xptr, install(translateChar(STRING_ELT(xnames, i))));
		xptr = CDR(xptr);
	    }
	}
	UNPROTECT(1);
    } else xnew = allocList(0);
    UNPROTECT(2);
    return xnew;
}



/* "eval" and "eval.with.vis" : Evaluate the first argument */
/* in the environment specified by the second argument. */

SEXP attribute_hidden do_eval(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP encl, x, xptr;
    volatile SEXP expr, env, tmp;

    int frame;

    checkArity(op, args);
    expr = CAR(args);
    env = CADR(args);
    encl = CADDR(args);
    if (isNull(encl)) {
	/* This is supposed to be defunct, but has been kept here
	   (and documented as such) */
	encl = R_BaseEnv;
    } else if ( !isEnvironment(encl) )
	error(_("invalid '%s' argument"), "enclos");
    switch(TYPEOF(env)) {
    case NILSXP:
	env = encl;     /* so eval(expr, NULL, encl) works */
	/* falls through */
    case ENVSXP:
	/* This usage requires all the pairlist to be named */
	PROTECT(env);	/* so we can unprotect 2 at the end */
	break;
    case LISTSXP:
	/* This usage requires all the pairlist to be named */
	env = NewEnvironment(R_NilValue, duplicate(CADR(args)), encl);
	PROTECT(env);
	break;
    case VECSXP:
	/* PR#14035 */
	x = VectorToPairListNamed(CADR(args));
	for (xptr = x ; xptr != R_NilValue ; xptr = CDR(xptr))
	    SET_NAMED(CAR(xptr) , 2);
	env = NewEnvironment(R_NilValue, x, encl);
	PROTECT(env);
	break;
    case INTSXP:
    case REALSXP:
	if (length(env) != 1)
	    error(_("numeric 'envir' arg not of length one"));
	frame = asInteger(env);
	if (frame == NA_INTEGER)
	    error(_("invalid '%s' argument"), "envir");
	PROTECT(env = R_sysframe(frame, ClosureContext::innermost()));
	break;
    default:
	error(_("invalid '%s' argument"), "envir");
    }

    /* isLanguage include NILSXP, and that does not need to be
       evaluated
    if (isLanguage(expr) || isSymbol(expr) || isByteCode(expr)) { */
    if (TYPEOF(expr) == LANGSXP || TYPEOF(expr) == SYMSXP || isByteCode(expr)) {
	PROTECT(expr);
	{
	    Expression* callx = SEXP_downcast<Expression*>(call);
	    Environment* call_env = SEXP_downcast<Environment*>(rho);
	    FunctionBase* func = SEXP_downcast<FunctionBase*>(op);
	    Environment* working_env = SEXP_downcast<Environment*>(env);
	    PairList* promargs = SEXP_downcast<PairList*>(args);
	    ClosureContext cntxt(callx, call_env, func, working_env, promargs);
	    Environment::ReturnScope returnscope(working_env);
	    try {
		expr = eval(expr, env);
	    }
	    catch (ReturnException& rx) {
		if (rx.environment() != working_env)
		    throw;
		tmp = rx.value();
	    }
	}
	UNPROTECT(1);
    }
    else if (TYPEOF(expr) == EXPRSXP) {
	int i, n;
	PROTECT(expr);
	n = LENGTH(expr);
	tmp = R_NilValue;
	{
	    Expression* callx = SEXP_downcast<Expression*>(call);
	    Environment* call_env = SEXP_downcast<Environment*>(rho);
	    FunctionBase* func = SEXP_downcast<FunctionBase*>(op);
	    Environment* working_env = SEXP_downcast<Environment*>(env);
	    PairList* promargs = SEXP_downcast<PairList*>(args);
	    ClosureContext cntxt(callx, call_env, func, working_env, promargs);
	    Environment::ReturnScope returnscope(working_env);
	    try {
		for (i = 0 ; i < n ; i++)
		    tmp = eval(XVECTOR_ELT(expr, i), env);
	    }
	    catch (ReturnException& rx) {
		if (rx.environment() != working_env)
		    throw;
		tmp = rx.value();
	    }
	}
	UNPROTECT(1);
	expr = tmp;
    }
    else if( TYPEOF(expr) == PROMSXP ) {
	expr = eval(expr, rho);
    } /* else expr is returned unchanged */
    if (PRIMVAL(op)) { /* eval.with.vis(*) : */
	PROTECT(expr);
	PROTECT(env = allocVector(VECSXP, 2));
	PROTECT(encl = allocVector(STRSXP, 2));
	SET_STRING_ELT(encl, 0, mkChar("value"));
	SET_STRING_ELT(encl, 1, mkChar("visible"));
	SET_VECTOR_ELT(env, 0, expr);
	SET_VECTOR_ELT(env, 1, ScalarLogical(R_Visible));
	setAttrib(env, R_NamesSymbol, encl);
	expr = env;
	UNPROTECT(3);
    }
    UNPROTECT(1);
    return expr;
}

/* This is a special .Internal */
SEXP attribute_hidden do_withVisible(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x, nm, ret;

    checkArity(op, args);
    x = CAR(args);
    x = eval(x, rho);
    PROTECT(x);
    PROTECT(ret = allocVector(VECSXP, 2));
    PROTECT(nm = allocVector(STRSXP, 2));
    SET_STRING_ELT(nm, 0, mkChar("value"));
    SET_STRING_ELT(nm, 1, mkChar("visible"));
    SET_VECTOR_ELT(ret, 0, x);
    SET_VECTOR_ELT(ret, 1, ScalarLogical(R_Visible));
    setAttrib(ret, R_NamesSymbol, nm);
    UNPROTECT(3);
    return ret;
}

/* This is a special .Internal */
SEXP attribute_hidden do_recall(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    ClosureContext *cptr;
    SEXP s, ans ;
    cptr = ClosureContext::innermost();
    /* get the args supplied */
    while (cptr && cptr->workingEnvironment() != rho) {
	cptr = ClosureContext::innermost(cptr->nextOut());
    }
    args = cptr->promiseArgs();
    /* get the env recall was called from */
    s = ClosureContext::innermost()->callEnvironment();
    while (cptr && cptr->workingEnvironment() != s) {
	cptr = ClosureContext::innermost(cptr->nextOut());
    }
    if (cptr == NULL)
	error(_("'Recall' called from outside a closure"));

    /* If the function has been recorded in the context, use it
       otherwise search for it by name or evaluate the expression
       originally used to get it.
    */
    if (cptr->function() != R_NilValue)
	PROTECT(s = cptr->function());
    else if( TYPEOF(CAR(cptr->call())) == SYMSXP)
	PROTECT(s = findFun(CAR(cptr->call()), cptr->callEnvironment()));
    else
	PROTECT(s = eval(CAR(cptr->call()), cptr->callEnvironment()));
    ans = applyClosure(cptr->call(), s, args, cptr->callEnvironment(), R_BaseEnv);
    UNPROTECT(1);
    return ans;
}


static SEXP evalArgs(SEXP el, SEXP rho, int dropmissing, SEXP call, int n)
{
    if(dropmissing) return evalList(el, rho, call, n);
    else return evalListKeepMissing(el, rho);
}


/* A version of DispatchOrEval that checks for possible S4 methods for
 * any argument, not just the first.  Used in the code for `[` in
 * do_subset.  Differs in that all arguments are evaluated
 * immediately, rather than after the call to R_possible_dispatch.
 */
attribute_hidden
int DispatchAnyOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		      SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
    if(R_has_methods(op)) {
        SEXP argValue, el; 
	/* Rboolean hasS4 = FALSE; */ 
	int nprotect = 0, dispatch;
	if(!argsevald) {
            PROTECT(argValue = evalArgs(args, rho, dropmissing, call, 0));
	    nprotect++;
	    argsevald = TRUE;
	}
	else argValue = args;
	for(el = argValue; el != R_NilValue; el = CDR(el)) {
	    if(IS_S4_OBJECT(CAR(el))) {
	        pair<bool, RObject*> pr = R_possible_dispatch(call, op, argValue, rho, TRUE);
	        if(pr.first) {
		    *ans = pr.second;
		    UNPROTECT(nprotect);
		    return 1;
	        }
		else break;
	    }
	}
	 /* else, use the regular DispatchOrEval, but now with evaluated args */
	dispatch = DispatchOrEval(call, op, generic, argValue, rho, ans, dropmissing, argsevald);
	UNPROTECT(nprotect);
	return dispatch;
    }
    return DispatchOrEval(call, op, generic, args, rho, ans, dropmissing, argsevald);
}


/* DispatchOrEval is used in internal functions which dispatch to
 * object methods (e.g. "[" or "[[").  The code either builds promises
 * and dispatches to the appropriate method, or it evaluates the
 * (unevaluated) arguments it comes in with and returns them so that
 * the generic built-in C code can continue.

 * To call this an ugly hack would be to insult all existing ugly hacks
 * at large in the world.
 */
attribute_hidden
int DispatchOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		   SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
/* DispatchOrEval is called very frequently, most often in cases where
   no dispatching is needed and the isObject or the string-based
   pre-test fail.  To avoid degrading performance it is therefore
   necessary to avoid creating promises in these cases.  The pre-test
   does require that we look at the first argument, so that needs to
   be evaluated.  The complicating factor is that the first argument
   might come in with a "..." and that there might be other arguments
   in the "..." as well.  LT */

    SEXP x = R_NilValue;
    int dots = FALSE, nprotect = 0;;

    if( argsevald )
	{PROTECT(x = CAR(args)); nprotect++;}
    else {
	/* Find the object to dispatch on, dropping any leading
	   ... arguments with missing or empty values.  If there are no
	   arguments, R_NilValue is used. */
	for (; args != R_NilValue; args = CDR(args)) {
	    if (CAR(args) == R_DotsSymbol) {
		SEXP h = findVar(R_DotsSymbol, rho);
		if (TYPEOF(h) == DOTSXP) {
		    /* just a consistency check */
		    if (TYPEOF(CAR(h)) != PROMSXP)
			error(_("value in '...' is not a promise"));
		    dots = TRUE;
		    x = eval(CAR(h), rho);
		break;
		}
		else if (h != R_NilValue && h != R_MissingArg)
		    error(_("'...' used in an incorrect context"));
	    }
	    else {
		dots = FALSE;
		x = eval(CAR(args), rho);
		break;
	    }
	}
	PROTECT(x); nprotect++;
    }
	/* try to dispatch on the object */
    if( isObject(x) ) {
	char *pt;
	/* Try for formal method. */
	if(IS_S4_OBJECT(x) && R_has_methods(op)) {
	    SEXP argValue;
	    /* create a promise to pass down to applyClosure  */
	    if(!argsevald) {
		argValue = promiseArgs(args, rho);
		SET_PRVALUE(CAR(argValue), x);
	    } else argValue = args;
	    PROTECT(argValue); nprotect++;
	    /* This means S4 dispatch */
	    std::pair<bool, SEXP> pr
		= R_possible_dispatch(call, op, argValue, rho, TRUE);
	    if (pr.first) {
		*ans = pr.second;
		UNPROTECT(nprotect);
		return 1;
	    }
	    else {
		/* go on, with the evaluated args.  Not guaranteed to have
		   the same semantics as if the arguments were not
		   evaluated, in special cases (e.g., arg values that are
		   LANGSXP).
		   The use of the promiseArgs is supposed to prevent
		   multiple evaluation after the call to possible_dispatch.
		*/
		if (dots)
		    argValue = evalArgs(argValue, rho, dropmissing, call, 0);
		else {
		    argValue = CONS(x, evalArgs(CDR(argValue), rho, dropmissing, call, 1));
		    SET_TAG(argValue, CreateTag(TAG(args)));
		}
		PROTECT(args = argValue); nprotect++;
		argsevald = 1;
	    }
	}
	if (TYPEOF(CAR(call)) == SYMSXP)
	    pt = Rf_strrchr(CHAR(PRINTNAME(CAR(call))), '.');
	else
	    pt = NULL;

	if (pt == NULL || strcmp(pt,".default")) {
	    SEXP pargs, rho1;
	    PROTECT(pargs = promiseArgs(args, rho)); nprotect++;
	    /* The context set up here is needed because of the way
	       usemethod() is written.  DispatchGroup() repeats some
	       internal usemethod() code and avoids the need for a
	       context; perhaps the usemethod() code should be
	       refactored so the contexts around the usemethod() calls
	       in this file can be removed.

	       Using rho for current and calling environment can be
	       confusing for things like sys.parent() calls captured
	       in promises (Gabor G had an example of this).  Also,
	       since the context is established without a SETJMP using
	       an R-accessible environment allows a segfault to be
	       triggered (by something very obscure, but still).
	       Hence here and in the other usemethod() uses below a
	       new environment rho1 is created and used.  LT */
	    PROTECT(rho1 = NewEnvironment(R_NilValue, R_NilValue, rho)); nprotect++;
	    SET_PRVALUE(CAR(pargs), x);
	    int um;
	    {
	        Expression* callx = SEXP_downcast<Expression*>(call);
	        Environment* call_env = SEXP_downcast<Environment*>(rho);
	        FunctionBase* func = SEXP_downcast<FunctionBase*>(op);
		Environment* working_env = SEXP_downcast<Environment*>(rho1);
		PairList* promargs = SEXP_downcast<PairList*>(pargs);
		ClosureContext cntxt(callx, call_env, func,
				     working_env, promargs);
		um = usemethod(generic, x, call, pargs, rho1, rho, R_BaseEnv, ans);
	    }
	    if (um) {
		UNPROTECT(nprotect);
		return 1;
	    }
	}
    }
    if(!argsevald) {
	if (dots)
	    /* The first call argument was ... and may contain more than the
	       object, so it needs to be evaluated here.  The object should be
	       in a promise, so evaluating it again should be no problem. */
	    *ans = evalArgs(args, rho, dropmissing, call, 0);
	else {
	    PROTECT(*ans = CONS(x, evalArgs(CDR(args), rho, dropmissing, call, 1)));
	    SET_TAG(*ans, CreateTag(TAG(args)));
	    UNPROTECT(1);
	}
    }
    else *ans = args;
    UNPROTECT(nprotect);
    return 0;
}


/* gr needs to be protected on return from this function */
static void findmethod(SEXP Class, const char *group, const char *generic,
		       SEXP *sxp,  SEXP *gr, SEXP *meth, int *which,
		       char *buf, SEXP rho)
{
    int len, whichclass;

    len = length(Class);

    /* Need to interleave looking for group and generic methods
       e.g. if class(x) is c("foo", "bar") then x > 3 should invoke
       "Ops.foo" rather than ">.bar"
    */
    for (whichclass = 0 ; whichclass < len ; whichclass++) {
	const char *ss = translateChar(STRING_ELT(Class, whichclass));
	if(strlen(generic) + strlen(ss) + 2 > 512)
	    error(_("class name too long in '%s'"), generic);
	sprintf(buf, "%s.%s", generic, ss);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = mkString("");
	    break;
	}
	if(strlen(group) + strlen(ss) + 2 > 512)
	    error(_("class name too long in '%s'"), group);
	sprintf(buf, "%s.%s", group, ss);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = mkString(group);
	    break;
	}
    }
    *which = whichclass;
}

attribute_hidden
int DispatchGroup(const char* group, SEXP call, SEXP op, SEXP args, SEXP rho,
		  SEXP *ans)
{
    int i, j, nargs, lwhich, rwhich, set;
    GCStackRoot<> lclass;
    SEXP s, t, m, lmeth, lsxp, lgr, newrho;
    SEXP rclass, rmeth, rgr, rsxp, value;
    char lbuf[512], rbuf[512], generic[128], *pt;
    Rboolean useS4 = TRUE, isOps = FALSE;

    /* pre-test to avoid string computations when there is nothing to
       dispatch on because either there is only one argument and it
       isn't an object or there are two or more arguments but neither
       of the first two is an object -- both of these cases would be
       rejected by the code following the string examination code
       below */
    if (args != R_NilValue && ! isObject(CAR(args)) &&
	(CDR(args) == R_NilValue || ! isObject(CADR(args))))
	return 0;

    isOps = CXXRCONSTRUCT(Rboolean, strcmp(group, "Ops") == 0);

    /* try for formal method */
    if(length(args) == 1 && !IS_S4_OBJECT(CAR(args))) useS4 = FALSE;
    if(length(args) == 2 &&
       !IS_S4_OBJECT(CAR(args)) && !IS_S4_OBJECT(CADR(args))) useS4 = FALSE;
    if(useS4) {
	/* Remove argument names to ensure positional matching */
	if(isOps)
	    for(s = args; s != R_NilValue; s = CDR(s)) SET_TAG(s, R_NilValue);
	if(R_has_methods(op)) {
	    std::pair<bool, SEXP> pr
		= R_possible_dispatch(call, op, args, rho, FALSE);
	    if (pr.first) {
		*ans = pr.second;
		return 1;
	    }
	}
	/* else go on to look for S3 methods */
    }

    /* check whether we are processing the default method */
    if ( isSymbol(CAR(call)) ) {
	if(strlen(CHAR(PRINTNAME(CAR(call)))) >= 512)
	   error(_("call name too long in '%s'"), CHAR(PRINTNAME(CAR(call))));
	sprintf(lbuf, "%s", CHAR(PRINTNAME(CAR(call))) );
	pt = strtok(lbuf, ".");
	pt = strtok(NULL, ".");

	if( pt != NULL && !strcmp(pt, "default") )
	    return 0;
    }

    if(isOps)
	nargs = length(args);
    else
	nargs = 1;

    if( nargs == 1 && !isObject(CAR(args)) )
	return 0;

    if(!isObject(CAR(args)) && !isObject(CADR(args)))
	return 0;

    if(strlen(PRIMNAME(op)) >= 128)
	error(_("generic name too long in '%s'"), PRIMNAME(op));
    sprintf(generic, "%s", PRIMNAME(op) );

    lclass = IS_S4_OBJECT(CAR(args)) ? R_data_class2(CAR(args))
      : getAttrib(CAR(args), R_ClassSymbol);

    if( nargs == 2 )
	rclass = IS_S4_OBJECT(CADR(args)) ? R_data_class2(CADR(args))
      : getAttrib(CADR(args), R_ClassSymbol);
    else
	rclass = R_NilValue;

    lsxp = R_NilValue; lgr = R_NilValue; lmeth = R_NilValue;
    rsxp = R_NilValue; rgr = R_NilValue; rmeth = R_NilValue;

    findmethod(lclass, group, generic, &lsxp, &lgr, &lmeth, &lwhich,
	       lbuf, rho);
    PROTECT(lgr);
    if(isFunction(lsxp) && IS_S4_OBJECT(CAR(args)) && lwhich > 0
       && isBasicClass(translateChar(STRING_ELT(lclass, lwhich)))) {
	/* This and the similar test below implement the strategy
	 for S3 methods selected for S4 objects.  See ?Methods */
        value = CAR(args);
	if(NAMED(value)) SET_NAMED(value, 2);
	value = R_getS4DataSlot(value, S4SXP); /* the .S3Class obj. or NULL*/
	if(value != R_NilValue) /* use the S3Part as the inherited object */
	    SETCAR(args, value);
    }

    if( nargs == 2 )
	findmethod(rclass, group, generic, &rsxp, &rgr, &rmeth,
		   &rwhich, rbuf, rho);
    else
	rwhich = 0;

    if(isFunction(rsxp) && IS_S4_OBJECT(CADR(args)) && rwhich > 0
       && isBasicClass(translateChar(STRING_ELT(rclass, rwhich)))) {
        value = CADR(args);
	if(NAMED(value)) SET_NAMED(value, 2);
	value = R_getS4DataSlot(value, S4SXP);
	if(value != R_NilValue) SETCADR(args, value);
    }

    PROTECT(rgr);

    if( !isFunction(lsxp) && !isFunction(rsxp) ) {
	UNPROTECT(2);
	return 0; /* no generic or group method so use default*/
    }

    if( lsxp != rsxp ) {
	if ( isFunction(lsxp) && isFunction(rsxp) ) {
	    /* special-case some methods involving difftime */
	    const char *lname = CHAR(PRINTNAME(lmeth)),
		*rname = CHAR(PRINTNAME(rmeth));
	    if( streql(rname, "Ops.difftime") && 
		(streql(lname, "+.POSIXt") || streql(lname, "-.POSIXt") ||
		 streql(lname, "+.Date") || streql(lname, "-.Date")) )
		rsxp = R_NilValue;
	    else if (streql(lname, "Ops.difftime") && 
		     (streql(rname, "+.POSIXt") || streql(rname, "+.Date")) )
		lsxp = R_NilValue;
	    else {
		warning(_("Incompatible methods (\"%s\", \"%s\") for \"%s\""),
			lname, rname, generic);
		UNPROTECT(2);
		return 0;
	    }
	}
	/* if the right hand side is the one */
	if( !isFunction(lsxp) ) { /* copy over the righthand stuff */
	    lsxp = rsxp;
	    lmeth = rmeth;
	    lgr = rgr;
	    lclass = rclass;
	    lwhich = rwhich;
	    strcpy(lbuf, rbuf);
	}
    }

    /* we either have a group method or a class method */

    PROTECT(GCNode::expose(newrho = new Environment(0)));
    PROTECT(m = allocVector(STRSXP,nargs));
    s = args;
    for (i = 0 ; i < nargs ; i++) {
	t = IS_S4_OBJECT(CAR(s)) ? R_data_class2(CAR(s))
	  : getAttrib(CAR(s), R_ClassSymbol);
	set = 0;
	if (isString(t)) {
	    for (j = 0 ; j < length(t) ; j++) {
		if (!strcmp(translateChar(STRING_ELT(t, j)),
			    translateChar(STRING_ELT(lclass, lwhich)))) {
		    SET_STRING_ELT(m, i, mkChar(lbuf));
		    set = 1;
		    break;
		}
	    }
	}
	if( !set )
	    SET_STRING_ELT(m, i, R_BlankString);
	s = CDR(s);
    }

    defineVar(install(".Method"), m, newrho);
    UNPROTECT(1);
    PROTECT(t = mkString(generic));
    defineVar(install(".Generic"), t, newrho);
    UNPROTECT(1);
    defineVar(install(".Group"), lgr, newrho);
    set = length(lclass) - lwhich;
    PROTECT(t = allocVector(STRSXP, set));
    for(j = 0 ; j < set ; j++ )
	SET_STRING_ELT(t, j, duplicate(STRING_ELT(lclass, lwhich++)));
    defineVar(install(".Class"), t, newrho);
    UNPROTECT(1);
    defineVar(install(".GenericCallEnv"), rho, newrho);
    defineVar(install(".GenericDefEnv"), R_BaseEnv, newrho);

    PROTECT(t = LCONS(lmeth, CDR(call)));

    /* the arguments have been evaluated; since we are passing them */
    /* out to a closure we need to wrap them in promises so that */
    /* they get duplicated and things like missing/substitute work. */

    PROTECT(s = promiseArgs(CDR(call), rho));
    if (length(s) != length(args))
	error(_("dispatch error in group dispatch"));
    for (m = s ; m != R_NilValue ; m = CDR(m), args = CDR(args) ) {
	SET_PRVALUE(CAR(m), CAR(args));
	/* ensure positional matching for operators */
	if(isOps) SET_TAG(m, R_NilValue);
    }

    *ans = applyClosure(t, lsxp, s, rho, newrho);
    UNPROTECT(5);
    return 1;
}

#ifdef BYTECODE
static int R_bcVersion = 4;
static int R_bcMinVersion = 4;

static SEXP R_AddSym = NULL;
static SEXP R_SubSym = NULL;
static SEXP R_MulSym = NULL;
static SEXP R_DivSym = NULL;
static SEXP R_ExptSym = NULL;
static SEXP R_SqrtSym = NULL;
static SEXP R_ExpSym = NULL;
static SEXP R_EqSym = NULL;
static SEXP R_NeSym = NULL;
static SEXP R_LtSym = NULL;
static SEXP R_LeSym = NULL;
static SEXP R_GeSym = NULL;
static SEXP R_GtSym = NULL;
static SEXP R_AndSym = NULL;
static SEXP R_OrSym = NULL;
static SEXP R_NotSym = NULL;
static SEXP R_SubsetSym = NULL;
static SEXP R_SubassignSym = NULL;
static SEXP R_CSym = NULL;
static SEXP R_Subset2Sym = NULL;
static SEXP R_Subassign2Sym = NULL;
static SEXP FakeCall0 = NULL;
static SEXP FakeCall1 = NULL;
static SEXP FakeCall2 = NULL;
static SEXP R_TrueValue = NULL;
static SEXP R_FalseValue = NULL;

#if defined(__GNUC__) && ! defined(BC_PROFILING) && (! defined(NO_THREADED_CODE))
# define THREADED_CODE
#endif

attribute_hidden
void R_initialize_bcode(void)
{
  R_AddSym = install("+");
  R_SubSym = install("-");
  R_MulSym = install("*");
  R_DivSym = install("/");
  R_ExptSym = install("^");
  R_SqrtSym = install("sqrt");
  R_ExpSym = install("exp");
  R_EqSym = install("==");
  R_NeSym = install("!=");
  R_LtSym = install("<");
  R_LeSym = install("<=");
  R_GeSym = install(">=");
  R_GtSym = install(">");
  R_AndSym = install("&");
  R_OrSym = install("|");
  R_NotSym = install("!");
  R_SubsetSym = R_BracketSymbol; /* "[" */
  R_SubassignSym = install("[<-");
  R_CSym = install("c");
  R_Subset2Sym = R_Bracket2Symbol; /* "[[" */
  R_Subassign2Sym = install("[[<-");

  FakeCall0 = CONS(R_NilValue, R_NilValue);
  FakeCall1 = CONS(R_NilValue, FakeCall0);
  FakeCall2 = CONS(R_NilValue, FakeCall1);
  R_PreserveObject(FakeCall2);
  R_TrueValue = mkTrue();
  SET_NAMED(R_TrueValue, 2);
  R_PreserveObject(R_TrueValue);
  R_FalseValue = mkFalse();
  SET_NAMED(R_FalseValue, 2);
  R_PreserveObject(R_FalseValue);
#ifdef THREADED_CODE
  bcEval(NULL, NULL);
#endif
}

enum {
  BCMISMATCH_OP,
  RETURN_OP,
  GOTO_OP,
  BRIFNOT_OP,
  POP_OP,
  DUP_OP,
  PRINTVALUE_OP,
  STARTLOOPCNTXT_OP,
  ENDLOOPCNTXT_OP,
  DOLOOPNEXT_OP,
  DOLOOPBREAK_OP,
  STARTFOR_OP,
  STEPFOR_OP,
  ENDFOR_OP,
  SETLOOPVAL_OP,
  INVISIBLE_OP,
  LDCONST_OP,
  LDNULL_OP,
  LDTRUE_OP,
  LDFALSE_OP,
  GETVAR_OP,
  DDVAL_OP,
  SETVAR_OP,
  GETFUN_OP,
  GETGLOBFUN_OP,
  GETSYMFUN_OP,
  GETBUILTIN_OP,
  GETINTLBUILTIN_OP,
  CHECKFUN_OP,
  MAKEPROM_OP,
  DOMISSING_OP,
  SETTAG_OP,
  DODOTS_OP,
  PUSHARG_OP,
  PUSHCONSTARG_OP,
  PUSHNULLARG_OP,
  PUSHTRUEARG_OP,
  PUSHFALSEARG_OP,
  CALL_OP,
  CALLBUILTIN_OP,
  CALLSPECIAL_OP,
  MAKECLOSURE_OP,
  UMINUS_OP,
  UPLUS_OP,
  ADD_OP,
  SUB_OP,
  MUL_OP,
  DIV_OP,
  EXPT_OP,
  SQRT_OP,
  EXP_OP,
  EQ_OP,
  NE_OP,
  LT_OP,
  LE_OP,
  GE_OP,
  GT_OP,
  AND_OP,
  OR_OP,
  NOT_OP,
  DOTSERR_OP,
  STARTASSIGN_OP,
  ENDASSIGN_OP,
  STARTSUBSET_OP,
  DFLTSUBSET_OP,
  STARTSUBASSIGN_OP,
  DFLTSUBASSIGN_OP,
  STARTC_OP,
  DFLTC_OP,
  STARTSUBSET2_OP,
  DFLTSUBSET2_OP,
  STARTSUBASSIGN2_OP,
  DFLTSUBASSIGN2_OP,
  DOLLAR_OP,
  DOLLARGETS_OP,
  ISNULL_OP,
  ISLOGICAL_OP,
  ISINTEGER_OP,
  ISDOUBLE_OP,
  ISCOMPLEX_OP,
  ISCHARACTER_OP,
  ISSYMBOL_OP,
  ISOBJECT_OP,
  ISNUMERIC_OP,
  NVECELT_OP,
  NMATELT_OP,
  SETNVECELT_OP,
  SETNMATELT_OP,
  OPCOUNT
};


/* Use header files!  2007/06/11 arr
SEXP R_unary(SEXP, SEXP, SEXP);
SEXP R_binary(SEXP, SEXP, SEXP, SEXP);
SEXP do_math1(SEXP, SEXP, SEXP, SEXP);
SEXP do_relop_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_logic(SEXP, SEXP, SEXP, SEXP);
SEXP do_subset_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_subassign_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_c_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_subset2_dflt(SEXP, SEXP, SEXP, SEXP);
SEXP do_subassign2_dflt(SEXP, SEXP, SEXP, SEXP);
*/

#define DO_FAST_RELOP2(op,a,b) do { \
    double __a__ = (a), __b__ = (b); \
    SEXP val; \
    if (ISNAN(__a__) || ISNAN(__b__)) val = ScalarLogical(NA_LOGICAL); \
    else val = (__a__ op __b__) ? R_TrueValue : R_FalseValue; \
    R_BCNodeStackTop[-2] = val; \
    R_BCNodeStackTop--; \
    NEXT(); \
} while (0)

# define FastRelop2(op,opval,opsym) do { \
    SEXP x = R_BCNodeStackTop[-2]; \
    SEXP y = R_BCNodeStackTop[-1]; \
    if (ATTRIB(x) == R_NilValue && ATTRIB(y) == R_NilValue) { \
	if (TYPEOF(x) == REALSXP && LENGTH(x) == 1 && \
	    TYPEOF(y) == REALSXP && LENGTH(y) == 1) \
	    DO_FAST_RELOP2(op, REAL(x)[0], REAL(y)[0]); \
	else if (TYPEOF(x) == INTSXP && LENGTH(x) == 1 && \
		 TYPEOF(y) == REALSXP && LENGTH(y) == 1) { \
	    double xd = INTEGER(x)[0] == NA_INTEGER ? NA_REAL : INTEGER(x)[0];\
	    DO_FAST_RELOP2(op, xd, REAL(y)[0]); \
	} \
	else if (TYPEOF(x) == REALSXP && LENGTH(x) == 1 && \
		 TYPEOF(y) == INTSXP && LENGTH(y) == 1) { \
	    double yd = INTEGER(y)[0] == NA_INTEGER ? NA_REAL : INTEGER(y)[0];\
	    DO_FAST_RELOP2(op, REAL(x)[0], yd); \
	} \
	else if (TYPEOF(x) == INTSXP && LENGTH(x) == 1 && \
		 TYPEOF(y) == INTSXP && LENGTH(y) == 1) { \
	    double xd = INTEGER(x)[0] == NA_INTEGER ? NA_REAL : INTEGER(x)[0];\
	    double yd = INTEGER(y)[0] == NA_INTEGER ? NA_REAL : INTEGER(y)[0];\
	    DO_FAST_RELOP2(op, xd, yd); \
	} \
    } \
    Relop2(opval, opsym); \
} while (0)

static SEXP cmp_relop(SEXP call, int opval, SEXP opsym, SEXP x, SEXP y)
{
    SEXP op = SYMVALUE(opsym);
    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMED(op, 2);
    }
    if (isObject(x) || isObject(y)) {
	SEXP args, ans;
	args = CONS(x, CONS(y, R_NilValue));
	PROTECT(args);
	if (DispatchGroup("Ops", call, op, args, R_GlobalEnv, &ans)) {
	    UNPROTECT(1);
	    return ans;
	}
	UNPROTECT(1);
    }
    return do_relop_dflt(R_NilValue, op, x, y);
}

static SEXP cmp_arith1(SEXP call, SEXP op, SEXP x)
{
  if (isObject(x)) {
    SEXP args, ans;
    args = CONS(x, R_NilValue);
    PROTECT(args);
    if (DispatchGroup("Ops", call, op, args, R_GlobalEnv, &ans)) {
      UNPROTECT(1);
      return ans;
    }
    UNPROTECT(1);
  }
  return R_unary(R_NilValue, op, x);
}

static SEXP cmp_arith2(SEXP call, int opval, SEXP opsym, SEXP x, SEXP y)
{
    SEXP op = SYMVALUE(opsym);
    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMED(op, 2);
    }
    if (isObject(x) || isObject(y)) {
	SEXP args, ans;
	args = CONS(x, CONS(y, R_NilValue));
	PROTECT(args);
	if (DispatchGroup("Ops", call, op, args, R_GlobalEnv, &ans)) {
	    UNPROTECT(1);
	    return ans;
	}
	UNPROTECT(1);
    }
    return R_binary(R_NilValue, op, x, y);
}

#define Builtin1(do_fun,which) do { \
  R_BCNodeStackTop[-1] = CONS(R_BCNodeStackTop[-1], R_NilValue); \
  R_BCNodeStackTop[-1] = do_fun(FakeCall1, SYMVALUE(which), \
				R_BCNodeStackTop[-1], R_NilValue); \
  NEXT(); \
} while(0)

#define NewBuiltin1(do_fun,which) do { \
  SEXP x = R_BCNodeStackTop[-1]; \
  R_BCNodeStackTop[-1] = do_fun(FakeCall1, SYMVALUE(which), x); \
  NEXT(); \
} while(0)

#define Builtin2(do_fun,which) do { \
  SEXP tmp = CONS(R_BCNodeStackTop[-1], R_NilValue); \
  R_BCNodeStackTop[-2] = CONS(R_BCNodeStackTop[-2], tmp); \
  R_BCNodeStackTop--; \
  R_BCNodeStackTop[-1] = do_fun(FakeCall2, SYMVALUE(which), \
				R_BCNodeStackTop[-1], R_NilValue); \
  NEXT(); \
} while(0)

#define NewBuiltin2(do_fun,opval,opsym) do { \
  SEXP x = R_BCNodeStackTop[-2]; \
  SEXP y = R_BCNodeStackTop[-1]; \
  R_BCNodeStackTop[-2] = do_fun(FakeCall2, opval, opsym, x, y); \
  R_BCNodeStackTop--; \
  NEXT(); \
} while(0)

#define Arith1(which) NewBuiltin1(cmp_arith1,which)
#define Arith2(opval,opsym) NewBuiltin2(cmp_arith2,opval,opsym)
#define Math1(which) Builtin1(do_math1,which)
#define Relop2(opval,opsym) NewBuiltin2(cmp_relop,opval,opsym)

# define DO_FAST_BINOP(op,a,b) do { \
    SEXP val = allocVector(REALSXP, 1); \
    REAL(val)[0] = (a) op (b); \
    R_BCNodeStackTop[-2] = val; \
    R_BCNodeStackTop--; \
    NEXT(); \
} while (0)
# define FastBinary(op,opval,opsym) do { \
    SEXP x = R_BCNodeStackTop[-2]; \
    SEXP y = R_BCNodeStackTop[-1]; \
    if (ATTRIB(x) == R_NilValue && ATTRIB(y) == R_NilValue) { \
	if (TYPEOF(x) == REALSXP && LENGTH(x) == 1 && \
	    TYPEOF(y) == REALSXP && LENGTH(y) == 1) \
	    DO_FAST_BINOP(op, REAL(x)[0], REAL(y)[0]); \
	else if (TYPEOF(x) == INTSXP && LENGTH(x) == 1 && \
		 INTEGER(x)[0] != NA_INTEGER && \
		 TYPEOF(y) == REALSXP && LENGTH(y) == 1) \
	    DO_FAST_BINOP(op, INTEGER(x)[0], REAL(y)[0]); \
	else if (TYPEOF(x) == REALSXP && LENGTH(x) == 1 && \
		 TYPEOF(y) == INTSXP && LENGTH(y) == 1 && \
		 INTEGER(y)[0] != NA_INTEGER) \
	    DO_FAST_BINOP(op, REAL(x)[0], INTEGER(y)[0]); \
    } \
    Arith2(opval, opsym); \
} while (0)

static void nodeStackOverflow()
{
    error(_("node stack overflow"));
}

namespace {
    inline void BCNPUSH(SEXP v)
    {
	SEXP *ntop = R_BCNodeStackTop + 1;
	if (ntop > R_BCNodeStackEnd) nodeStackOverflow();
	ntop[-1] = v;
	R_BCNodeStackTop = ntop;
    }

    inline SEXP BCNPOP() {R_BCNodeStackTop--; return R_BCNodeStackTop[0];}

    inline void BCNPOP_IGNORE_VALUE() {R_BCNodeStackTop--;}

    inline void BCNSTACKCHECK(int /*unused*/)
    {
	if (R_BCNodeStackTop + 1 > R_BCNodeStackEnd) nodeStackOverflow();
    }
}

#define BCIPUSHPTR(v)  do { \
  void *__value__ = (v); \
  IStackval *__ntop__ = R_BCIntStackTop + 1; \
  if (__ntop__ > R_BCIntStackEnd) intStackOverflow(); \
  *__ntop__[-1].p = __value__; \
  R_BCIntStackTop = __ntop__; \
} while (0)

#define BCIPUSHINT(v)  do { \
  int __value__ = (v); \
  IStackval *__ntop__ = R_BCIntStackTop + 1; \
  if (__ntop__ > R_BCIntStackEnd) intStackOverflow(); \
  __ntop__[-1].i = __value__; \
  R_BCIntStackTop = __ntop__; \
} while (0)

#define BCIPOPPTR() ((--R_BCIntStackTop)->p)
#define BCIPOPINT() ((--R_BCIntStackTop)->i)

#define BCCONSTS(e) BCODE_CONSTS(e)

#ifdef BC_INT_STACK
static void intStackOverflow()
{
    error(_("integer stack overflow"));
}
#endif

static SEXP bytecodeExpr(SEXP e)
{
    if (isByteCode(e)) {
	if (LENGTH(BCCONSTS(e)) > 0)
	    return VECTOR_ELT(BCCONSTS(e), 0);
	else return R_NilValue;
    }
    else return e;
}

SEXP R_PromiseExpr(SEXP p)
{
    return bytecodeExpr(PRCODE(p));
}

SEXP R_ClosureExpr(SEXP p)
{
    return bytecodeExpr(BODY(p));
}

#ifdef THREADED_CODE
typedef union { void *v; int i; } BCODE;

static struct { void *addr; int argc; } opinfo[OPCOUNT];

#define OP(name,n) \
  case name##_OP: opinfo[name##_OP].addr = (__extension__ &&op_##name); \
    opinfo[name##_OP].argc = (n); \
    goto loop; \
    op_##name

#define BEGIN_MACHINE  NEXT(); init: { loop: switch(which++)
#define LASTOP } value = R_NilValue; goto done
#define INITIALIZE_MACHINE() if (body == NULL) goto init

#define NEXT() (__extension__ ({goto *(*pc++).v;}))
#define GETOP() (*pc++).i

#define BCCODE(e) reinterpret_cast<BCODE *>( INTEGER(BCODE_CODE(e)))
#else
typedef int BCODE;

#define OP(name,argc) case name##_OP

#ifdef BC_PROFILING
#define BEGIN_MACHINE  loop: current_opcode = *pc; switch(*pc++)
#else
#define BEGIN_MACHINE  loop: switch(*pc++)
#endif
#define LASTOP  default: error(_("Bad opcode"))
#define INITIALIZE_MACHINE()

#define NEXT() goto loop
#define GETOP() *pc++

#define BCCODE(e) INTEGER(BCODE_CODE(e))
#endif

#define DO_GETVAR(dd) do { \
  SEXP symbol = VECTOR_ELT(constants, GETOP()); \
  value = (dd) ? ddfindVar(symbol, rho) : findVar(symbol, rho); \
  R_Visible = TRUE; \
  if (value == R_UnboundValue) \
    error(_("object '%s' not found"), CHAR(PRINTNAME(symbol))); \
  else if (value == R_MissingArg) { \
    const char *n = CHAR(PRINTNAME(symbol)); \
    if(*n) error(_("argument \"%s\" is missing, with no default"), n); \
    else error(_("argument is missing, with no default")); \
  } \
  else if (TYPEOF(value) == PROMSXP) { \
    value = forcePromise(value); \
    SET_NAMED(value, 2); \
  } \
  else if (!isNull(value) && NAMED(value) < 1) \
    SET_NAMED(value, 1); \
  BCNPUSH(value); \
  NEXT(); \
} while (0)

namespace {
    inline void PUSHCALLARG_CELL(SEXP cell)
    {
	if (R_BCNodeStackTop[-2]) R_BCNodeStackTop[-2] = cell;
	else SETCDR(R_BCNodeStackTop[-1], cell);
	R_BCNodeStackTop[-1] = cell;
    }

    inline void PUSHCALLARG(SEXP v) {PUSHCALLARG_CELL(CONS(v, R_NilValue));}
}

/* making sure the constant is NAMED can be done at assembly time
   once duplicate is set up to not copy the constant portion of code
   and once load is set to make the constants NAMED--basically once
   there is a proper code data type with appropriate support. */
#define DO_LDCONST(v) do { \
  v = VECTOR_ELT(constants, GETOP()); \
  if (! NAMED(v)) SET_NAMED(v, 1); \
} while (0)

static int tryDispatch(CXXRCONST char *generic, SEXP call, SEXP x, SEXP rho, SEXP *pv)
{
  SEXP pargs, rho1;
  int dispatched = FALSE;

  PROTECT(pargs = promiseArgs(CDR(call), rho));
  /* See comment at first usemethod() call in this file. LT */
  PROTECT(rho1 = NewEnvironment(R_NilValue, R_NilValue, rho));
  SET_PRVALUE(CAR(pargs), x);
  {
      Expression* callx = SEXP_downcast<Expression*>(call);
      Environment* call_env = SEXP_downcast<Environment*>(rho);
      Environment* working_env = SEXP_downcast<Environment*>(rho1);
      PairList* promargs = SEXP_downcast<PairList*>(pargs);
      // FIXME: put in op:
      ClosureContext cntxt(callx, call_env, 0, working_env, promargs);
      if (usemethod(generic, x, call, pargs, rho1, rho, R_BaseEnv, pv))
	  dispatched = TRUE;
  }
  UNPROTECT(2);
  return dispatched;
}

#define DO_STARTDISPATCH(generic) do { \
  SEXP call = VECTOR_ELT(constants, GETOP()); \
  int label = GETOP(); \
  value = R_BCNodeStackTop[-1]; \
  if (isObject(value) && tryDispatch(generic, call, value, rho, &value)) {\
    R_BCNodeStackTop[-1] = value; \
    BC_CHECK_SIGINT(); \
    pc = codebase + label; \
  } \
  else { \
    SEXP tag = TAG(CDR(call)); \
    SEXP cell = CONS(value, R_NilValue); \
    BCNSTACKCHECK(3); \
    R_BCNodeStackTop[0] = call; \
    R_BCNodeStackTop[1] = cell; \
    R_BCNodeStackTop[2] = cell; \
    R_BCNodeStackTop += 3; \
    if (tag != R_NilValue) \
      SET_TAG(cell, CreateTag(tag)); \
  } \
  NEXT(); \
} while (0)

#define DO_DFLTDISPATCH(fun, symbol) do { \
  SEXP call = R_BCNodeStackTop[-3]; \
  SEXP args = R_BCNodeStackTop[-2]; \
  value = fun(call, symbol, args, rho); \
  R_BCNodeStackTop -= 3; \
  R_BCNodeStackTop[-1] = value; \
  NEXT(); \
} while (0)

#define DO_ISTEST(fun) do { \
  R_BCNodeStackTop[-1] = fun(R_BCNodeStackTop[-1]) ? \
			 R_TrueValue : R_FalseValue; \
  NEXT(); \
} while(0)
#define DO_ISTYPE(type) do { \
  R_BCNodeStackTop[-1] = TYPEOF(R_BCNodeStackTop[-1]) == type ? \
			 mkTrue() : mkFalse(); \
  NEXT(); \
} while (0)
#define isNumericOnly(x) (isNumeric(x) && ! isLogical(x))

#ifdef BC_PROFILING
#define NO_CURRENT_OPCODE -1
static int current_opcode = NO_CURRENT_OPCODE;
static int opcode_counts[OPCOUNT];
#endif

#define BC_COUNT_DELTA 1000

#define BC_CHECK_SIGINT() do { \
  if (++evalcount > BC_COUNT_DELTA) { \
      R_CheckUserInterrupt(); \
      evalcount = 0; \
  } \
} while (0)

static void loopWithContect(volatile SEXP code, volatile SEXP rho)
{
    Environment* env = SEXP_downcast<Environment*>(rho);
    Environment::LoopScope loopscope(env);
    bool redo;
    do {
	redo = false;
	try {
	    bcEval(code, rho);
	}
	catch (LoopException& lx) {
	    if (lx.environment() != env)
		throw;
	    redo = lx.next();
	}
    } while (redo);
}

static void checkVectorSubscript(SEXP vec, int k)
{
    switch (TYPEOF(vec)) {
    case REALSXP:
    case INTSXP:
    case LGLSXP:
    case CPLXSXP:
    case STRSXP:
    case VECSXP:
    case EXPRSXP:
    case RAWSXP:
	if (k < 0 || k >= LENGTH(vec))
	    error(_("subscript out of bounds"));
	break;
    default: error(_("not a vector object"));
    }
}

static SEXP numVecElt(SEXP vec, SEXP idx)
{
    int i = asInteger(idx) - 1;
    if (OBJECT(vec))
	error(_("can only handle simple real vectors"));
    checkVectorSubscript(vec, i);
    switch (TYPEOF(vec)) {
    case REALSXP: return ScalarReal(REAL(vec)[i]);
    case INTSXP: return ScalarInteger(INTEGER(vec)[i]);
    case LGLSXP: return ScalarLogical(LOGICAL(vec)[i]);
    case CPLXSXP: return ScalarComplex(COMPLEX(vec)[i]);
    case RAWSXP: return ScalarRaw(RAW(vec)[i]);
    default:
	error(_("not a simple vector"));
	return R_NilValue; /* keep -Wall happy */
    }
}

static SEXP numMatElt(SEXP mat, SEXP idx, SEXP jdx)
{
    SEXP dim;
    int k, nrow;
    int i = asInteger(idx);
    int j = asInteger(jdx);

    if (OBJECT(mat))
	error(_("can only handle simple real vectors"));

    dim = getAttrib(mat, R_DimSymbol);
    if (mat == R_NilValue || TYPEOF(dim) != INTSXP || LENGTH(dim) != 2)
	error(_("incorrect number of subscripts"));
    nrow = INTEGER(dim)[0];
    k = i - 1 + nrow * (j - 1);
    checkVectorSubscript(mat, k);

    switch (TYPEOF(mat)) {
    case REALSXP: return ScalarReal(REAL(mat)[k]);
    case INTSXP: return ScalarInteger(INTEGER(mat)[k]);
    case LGLSXP: return ScalarLogical(LOGICAL(mat)[k]);
    case CPLXSXP: return ScalarComplex(COMPLEX(mat)[k]);
    default:
	error(_("not a simple matrix"));
	return R_NilValue; /* keep -Wall happy */
    }
}

static SEXP setNumVecElt(SEXP vec, SEXP idx, SEXP value)
{
    int i = asInteger(idx) - 1;
    if (OBJECT(vec))
	error(_("can only handle simple real vectors"));
    checkVectorSubscript(vec, i);
    if (NAMED(vec) > 1)
	vec = duplicate(vec);
    PROTECT(vec);
    switch (TYPEOF(vec)) {
    case REALSXP: REAL(vec)[i] = asReal(value); break;
    case INTSXP: INTEGER(vec)[i] = asInteger(value); break;
    case LGLSXP: LOGICAL(vec)[i] = asLogical(value); break;
    case CPLXSXP: COMPLEX(vec)[i] = asComplex(value); break;
    default: error(_("not a simple vector"));
    }
    UNPROTECT(1);
    return vec;
}

static SEXP setNumMatElt(SEXP mat, SEXP idx, SEXP jdx, SEXP value)
{
    SEXP dim;
    int k, nrow;
    int i = asInteger(idx);
    int j = asInteger(jdx);

    if (OBJECT(mat))
	error(_("can only handle simple real vectors"));

    dim = getAttrib(mat, R_DimSymbol);
    if (mat == R_NilValue || TYPEOF(dim) != INTSXP || LENGTH(dim) != 2)
	error(_("incorrect number of subscripts"));
    nrow = INTEGER(dim)[0];
    k = i - 1 + nrow * (j - 1);
    checkVectorSubscript(mat, k);

    if (NAMED(mat) > 1)
	mat = duplicate(mat);

    PROTECT(mat);
    switch (TYPEOF(mat)) {
    case REALSXP: REAL(mat)[k] = asReal(value); break;
    case INTSXP: INTEGER(mat)[k] = asInteger(value); break;
    case LGLSXP: LOGICAL(mat)[k] = asLogical(value); break;
    case CPLXSXP: COMPLEX(mat)[k] = asComplex(value); break;
    default: error(_("not a simple matrix"));
    }
    UNPROTECT(1);
    return mat;
}

static SEXP bcEval(SEXP body, SEXP rho)
{
  SEXP value, constants;
  BCODE *pc, *codebase;
  int ftype = 0;
  SEXP *oldntop = R_BCNodeStackTop;
  static int evalcount = 0;
#ifdef BC_INT_STACK
  IStackval *olditop = R_BCIntStackTop;
#endif
#ifdef BC_PROFILING
  int old_current_opcode = current_opcode;
#endif
#ifdef THREADED_CODE
  int which = 0;
#endif

  BC_CHECK_SIGINT();

  INITIALIZE_MACHINE();
  codebase = pc = BCCODE(body);
  constants = BCCONSTS(body);

  /* check version */
  {
      int version = GETOP();
      if (version < R_bcMinVersion || version > R_bcVersion) {
	  if (version >= 2) {
	      static Rboolean warned = FALSE;
	      if (! warned) {
		  warned = TRUE;
		  warning(_("bytecode version mismatch; using eval"));
	      }
	      return eval(bytecodeExpr(body), rho);
	  }
	  else if (version < R_bcMinVersion)
	      error(_("bytecode version is too old"));
	  else error(_("bytecode version is too new"));
      }
  }

  BEGIN_MACHINE {
    OP(BCMISMATCH, 0): error(_("byte code version mismatch"));
    OP(RETURN, 0): value = R_BCNodeStackTop[-1]; goto done;
    OP(GOTO, 1):
      {
	int label = GETOP();
	BC_CHECK_SIGINT();
	pc = codebase + label;
	NEXT();
      }
    OP(BRIFNOT, 1):
      {
	int label = GETOP(), cond;
	value = BCNPOP();
	cond = asLogical(value);
	if (cond == NA_LOGICAL)
	  error(isLogical(value)
		? _("missing value where logical needed")
		: _("argument of if(*) is not interpretable as logical"));
	if (! cond) {
	    BC_CHECK_SIGINT();
	    pc = codebase + label;
	}
	NEXT();
      }
    OP(POP, 0): BCNPOP_IGNORE_VALUE(); NEXT();
    OP(DUP, 0): value = R_BCNodeStackTop[-1]; BCNPUSH(value); NEXT();
    OP(PRINTVALUE, 0): PrintValue(BCNPOP()); NEXT();
    OP(STARTLOOPCNTXT, 1):
	{
	    SEXP code = VECTOR_ELT(constants, GETOP());
	    loopWithContect(code, rho);
	    NEXT();
	}
    OP(ENDLOOPCNTXT, 0): value = R_NilValue; goto done;
    OP(DOLOOPNEXT, 0):
	{
	    Environment* env = SEXP_downcast<Environment*>(rho);
	    throw LoopException(env, true);
	}
    OP(DOLOOPBREAK, 0):
	{
	    Environment* env = SEXP_downcast<Environment*>(rho);
	    throw LoopException(env, false);
	}
    OP(STARTFOR, 2):
      {
	SEXP seq = R_BCNodeStackTop[-1];
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	int label = GETOP();

	defineVar(symbol, R_NilValue, rho);
	BCNPUSH(reinterpret_cast<SEXP>( R_findVarLocInFrame(rho, symbol)));

	value = allocVector(INTSXP, 2);
	INTEGER(value)[0] = -1;
	if (isVector(seq))
	  INTEGER(value)[1] = LENGTH(seq);
	else if (isList(seq) || isNull(seq))
	  INTEGER(value)[1] = length(seq);
	else error(_("invalid sequence argument in for loop"));
	BCNPUSH(value);

	/* bump up NAMED count of seq to avoid modification by loop code */
	if (NAMED(seq) < 2) SET_NAMED(seq, NAMED(seq) + 1);

	BCNPUSH(R_NilValue);

	BC_CHECK_SIGINT();
	pc = codebase + label;
	NEXT();
      }
    OP(STEPFOR, 1):
      {
	int label = GETOP();
	int i = ++(INTEGER(R_BCNodeStackTop[-2])[0]);
	int n = INTEGER(R_BCNodeStackTop[-2])[1];
	if (i < n) {
	  SEXP seq = R_BCNodeStackTop[-4];
	  SEXP cell = R_BCNodeStackTop[-3];
	  switch (TYPEOF(seq)) {
	  case LGLSXP:
	  case INTSXP:
	    value = allocVector(TYPEOF(seq), 1);
	    INTEGER(value)[0] = INTEGER(seq)[i];
	    break;
	  case REALSXP:
	    value = allocVector(TYPEOF(seq), 1);
	    REAL(value)[0] = REAL(seq)[i];
	    break;
	  case CPLXSXP:
	    value = allocVector(TYPEOF(seq), 1);
	    COMPLEX(value)[0] = COMPLEX(seq)[i];
	    break;
	  case STRSXP:
	    value = allocVector(TYPEOF(seq), 1);
	    SET_STRING_ELT(value, 0, STRING_ELT(seq, i));
	    break;
	  case EXPRSXP:
	    value = XVECTOR_ELT(seq, i);
	    break;
	  case VECSXP:
	    value = VECTOR_ELT(seq, i);
	    break;
	  case LISTSXP:
	    value = CAR(seq);
	    R_BCNodeStackTop[-4] = CDR(seq);
	    break;
	  default:  // -Wswitch
	      break;
	  }
	  R_SetVarLocValue(reinterpret_cast<R_varloc_t>( cell), value);
	  BC_CHECK_SIGINT();
	  pc = codebase + label;
	}
	NEXT();
      }
    OP(ENDFOR, 0):
      {
	value = R_BCNodeStackTop[-1];
	R_BCNodeStackTop -= 3;
	R_BCNodeStackTop[-1] = value;
	NEXT();
      }
    OP(SETLOOPVAL, 0): BCNPOP(); R_BCNodeStackTop[-1] = R_NilValue; NEXT();
    OP(INVISIBLE,0): R_Visible = FALSE; NEXT();
    OP(LDCONST, 1): DO_LDCONST(value); BCNPUSH(value); NEXT();
    OP(LDNULL, 0):  BCNPUSH(R_NilValue); NEXT();
    OP(LDTRUE, 0):  BCNPUSH(R_TrueValue); NEXT();
    OP(LDFALSE, 0):  BCNPUSH(R_FalseValue); NEXT();
    OP(GETVAR, 1): DO_GETVAR(FALSE);
    OP(DDVAL, 1): DO_GETVAR(TRUE);
    OP(SETVAR, 1):
      {
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = R_BCNodeStackTop[-1];
	switch (NAMED(value)) {
	case 0: SET_NAMED(value, 1); break;
	case 1: SET_NAMED(value, 2); break;
	}
	defineVar(symbol, value, rho);
	NEXT();
      }
    OP(GETFUN, 1):
      {
	/* get the function */
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = findFun(symbol, rho);
	if(RTRACE(value)) {
	  Rprintf("trace: ");
	  PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	R_BCNodeStackTop[0] = value;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop[2] = R_NilValue;
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETGLOBFUN, 1):
      {
	/* get the function */
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = findFun(symbol, R_GlobalEnv);
	if(RTRACE(value)) {
	  Rprintf("trace: ");
	  PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	R_BCNodeStackTop[0] = value;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop[2] = R_NilValue;
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETSYMFUN, 1):
      {
	/* get the function */
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = SYMVALUE(symbol);
	if (TYPEOF(value) == PROMSXP) {
	    value = forcePromise(value);
	    SET_NAMED(value, 2);
	}
	if(RTRACE(value)) {
	  Rprintf("trace: ");
	  PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	R_BCNodeStackTop[0] = value;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop[2] = R_NilValue;
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETBUILTIN, 1):
      {
	/* get the function */
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = SYMVALUE(symbol);
	if (TYPEOF(value) == PROMSXP) {
	    value = forcePromise(value);
	    SET_NAMED(value, 2);
	}
	if (TYPEOF(value) != BUILTINSXP)
	  error(_("not a BUILTIN function"));
	if(RTRACE(value)) {
	  Rprintf("trace: ");
	  PrintValue(symbol);
	}

	/* push the function and push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	R_BCNodeStackTop[0] = value;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop[2] = R_NilValue;
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETINTLBUILTIN, 1):
      {
	/* get the function */
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	value = INTERNAL(symbol);
	if (TYPEOF(value) != BUILTINSXP)
	  error(_("not a BUILTIN function"));

	/* push the function and push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	R_BCNodeStackTop[0] = value;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop[2] = R_NilValue;
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(CHECKFUN, 0):
      {
	/* check then the value on the stack is a function */
	value = R_BCNodeStackTop[-1];
	if (TYPEOF(value) != CLOSXP && TYPEOF(value) != BUILTINSXP &&
	    TYPEOF(value) != SPECIALSXP)
	  error(_("attempt to apply non-function"));

	/* initialize the function type register, and push space for
	   creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(2);
	R_BCNodeStackTop[0] = R_NilValue;
	R_BCNodeStackTop[1] = R_NilValue;
	R_BCNodeStackTop += 2;
	NEXT();
      }
    OP(MAKEPROM, 1):
      {
	SEXP code = VECTOR_ELT(constants, GETOP());
	if (ftype != SPECIALSXP) {
	  if (ftype == BUILTINSXP)
	    value = bcEval(code, rho);
	  else
	    value = mkPROMISE(code, rho);
	  PUSHCALLARG(value);
	}
	NEXT();
      }
    OP(DOMISSING, 0):
      {
	if (ftype != SPECIALSXP)
	  PUSHCALLARG(R_MissingArg);
	NEXT();
      }
    OP(SETTAG, 1):
      {
	SEXP tag = VECTOR_ELT(constants, GETOP());
	SEXP cell = R_BCNodeStackTop[-1];
	if (ftype != SPECIALSXP && cell != R_NilValue)
	  SET_TAG(cell, CreateTag(tag));
	NEXT();
      }
    OP(DODOTS, 0):
      {
	if (ftype != SPECIALSXP) {
	  SEXP h = findVar(R_DotsSymbol, rho);
	  if (TYPEOF(h) == DOTSXP || h == R_NilValue) {
	    for (; h != R_NilValue; h = CDR(h)) {
	      SEXP val, cell;
	      if (ftype == BUILTINSXP) val = eval(CAR(h), rho);
	      else val = mkPROMISE(CAR(h), rho);
	      cell = CONS(val, R_NilValue);
	      PUSHCALLARG_CELL(cell);
	      if (TAG(h) != R_NilValue) SET_TAG(cell, CreateTag(TAG(h)));
	    }
	  }
	  else if (h != R_MissingArg)
	    error(_("'...' used in an incorrect context"));
	}
	NEXT();
      }
    OP(PUSHARG, 0): PUSHCALLARG(BCNPOP()); NEXT();
    OP(PUSHCONSTARG, 1): DO_LDCONST(value); PUSHCALLARG(value); NEXT();
    OP(PUSHNULLARG, 0): PUSHCALLARG(R_NilValue); NEXT();
    OP(PUSHTRUEARG, 0): PUSHCALLARG(R_TrueValue); NEXT();
    OP(PUSHFALSEARG, 0): PUSHCALLARG(R_FalseValue); NEXT();
    OP(CALL, 1):
      {
	SEXP fun = R_BCNodeStackTop[-3];
	SEXP call = VECTOR_ELT(constants, GETOP());
	SEXP args = R_BCNodeStackTop[-2];
	int flag;
	switch (ftype) {
	case BUILTINSXP:
	  flag = PRIMPRINT(fun);
	  R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	  value = PRIMFUN(fun) (call, fun, args, rho);
	  if (flag < 2) R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	  break;
	case SPECIALSXP:
	  flag = PRIMPRINT(fun);
	  R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	  value = PRIMFUN(fun) (call, fun, CDR(call), rho);
	  if (flag < 2) R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	  break;
	case CLOSXP:
	  value = applyClosure(call, fun, args, rho, R_BaseEnv);
	  break;
	default: error(_("bad function"));
	}
	R_BCNodeStackTop -= 2;
	R_BCNodeStackTop[-1] = value;
	NEXT();
      }
    OP(CALLBUILTIN, 1):
      {
	SEXP fun = R_BCNodeStackTop[-3];
	SEXP call = VECTOR_ELT(constants, GETOP());
	SEXP args = R_BCNodeStackTop[-2];
	int flag;
	const void *vmax = vmaxget();
	if (TYPEOF(fun) != BUILTINSXP)
	  error(_("not a BUILTIN function"));
	flag = PRIMPRINT(fun);
	R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	value = PRIMFUN(fun) (call, fun, args, rho);
	if (flag < 2) R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	vmaxset(vmax);
	R_BCNodeStackTop -= 2;
	R_BCNodeStackTop[-1] = value;
	NEXT();
      }
    OP(CALLSPECIAL, 1):
      {
	SEXP call = VECTOR_ELT(constants, GETOP());
	SEXP symbol = CAR(call);
	SEXP fun = SYMVALUE(symbol);
	int flag;
	const void *vmax = vmaxget();
	if (TYPEOF(fun) == PROMSXP) {
	    fun = forcePromise(fun);
	    SET_NAMED(fun, 2);
	}
	if(RTRACE(fun)) {
	  Rprintf("trace: ");
	  PrintValue(symbol);
	}
	if (TYPEOF(fun) != SPECIALSXP)
	  error(_("not a SPECIAL function"));
	flag = PRIMPRINT(fun);
	R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	value = PRIMFUN(fun) (call, fun, CDR(call), rho);
	if (flag < 2) R_Visible = CXXRCONSTRUCT(Rboolean, flag != 1);
	vmaxset(vmax);
	BCNPUSH(value);
	NEXT();
      }
    OP(MAKECLOSURE, 1):
      {
	SEXP fb = VECTOR_ELT(constants, GETOP());
	SEXP forms = VECTOR_ELT(fb, 0);
	SEXP body = VECTOR_ELT(fb, 1);
	value = mkCLOSXP(forms, body, rho);
	BCNPUSH(value);
	NEXT();
      }
    OP(UMINUS, 0): Arith1(R_SubSym);
    OP(UPLUS, 0): Arith1(R_AddSym);
    OP(ADD, 0): FastBinary(+, PLUSOP, R_AddSym);
    OP(SUB, 0): FastBinary(-, MINUSOP, R_SubSym);
    OP(MUL, 0): FastBinary(*, TIMESOP, R_MulSym);
    OP(DIV, 0): FastBinary(/, DIVOP, R_DivSym);
    OP(EXPT, 0): Arith2(POWOP, R_ExptSym);
    OP(SQRT, 0): Math1(R_SqrtSym);
    OP(EXP, 0): Math1(R_ExpSym);
    OP(EQ, 0): FastRelop2(==, EQOP, R_EqSym);
    OP(NE, 0): FastRelop2(!=, NEOP, R_NeSym);
    OP(LT, 0): FastRelop2(<, LTOP, R_LtSym);
    OP(LE, 0): FastRelop2(<=, LEOP, R_LeSym);
    OP(GE, 0): FastRelop2(>=, GEOP, R_GeSym);
    OP(GT, 0): FastRelop2(>, GTOP, R_GtSym);
    OP(AND, 0): Builtin2(do_logic, R_AndSym);
    OP(OR, 0): Builtin2(do_logic, R_OrSym);
    OP(NOT, 0): Builtin1(do_logic, R_NotSym);
    OP(DOTSERR, 0): error(_("'...' used in an incorrect context"));
    OP(STARTASSIGN, 2):
      {
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	SEXP valsym = VECTOR_ELT(constants, GETOP());
	EnsureLocal(symbol, rho);
	value = R_BCNodeStackTop[-1];
	defineVar(valsym, value, rho); /**** not adjusting NAMED OK? */
	/* right-hand side value is now on top of stack */
	NEXT();
      }
    OP(ENDASSIGN, 2):
      {
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	SEXP valsym = VECTOR_ELT(constants, GETOP());
	value = BCNPOP();
	switch (NAMED(value)) {
	case 0: SET_NAMED(value, 1); break;
	case 1: SET_NAMED(value, 2); break;
	}
	defineVar(symbol, value, rho);
	unbindVar(valsym, rho);
	/* original right-hand side value is now on top of stack again */
	NEXT();
      }
    OP(STARTSUBSET, 2): DO_STARTDISPATCH("[");
    OP(DFLTSUBSET, 0): DO_DFLTDISPATCH(do_subset_dflt, R_SubsetSym);
    OP(STARTSUBASSIGN, 2): DO_STARTDISPATCH("[<-");
    OP(DFLTSUBASSIGN, 0): DO_DFLTDISPATCH(do_subassign_dflt, R_SubassignSym);
    OP(STARTC, 2): DO_STARTDISPATCH("c");
    OP(DFLTC, 0): DO_DFLTDISPATCH(do_c_dflt, R_CSym);
    OP(STARTSUBSET2, 2): DO_STARTDISPATCH("[[");
    OP(DFLTSUBSET2, 0): DO_DFLTDISPATCH(do_subset2_dflt, R_Subset2Sym);
    OP(STARTSUBASSIGN2, 2): DO_STARTDISPATCH("[[<-");
    OP(DFLTSUBASSIGN2, 0):
      DO_DFLTDISPATCH(do_subassign2_dflt, R_Subassign2Sym);
    OP(DOLLAR, 2):
      {
	int dispatched = FALSE;
	SEXP call = VECTOR_ELT(constants, GETOP());
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	SEXP x = R_BCNodeStackTop[-1];
	if (isObject(x)) {
	  SEXP pargs, str, rho1;
	  PROTECT(pargs = promiseArgs(CDR(call), rho));
	  /* See comment at first usemethod() call in this file. LT */
	  PROTECT(rho1 = NewEnvironment(R_NilValue, R_NilValue, rho));
	  SET_PRVALUE(CAR(pargs), x);
	  str = ScalarString(PRINTNAME(symbol));
	  SET_PRVALUE(CADR(pargs), str);
	  {
	      Expression* callx = SEXP_downcast<Expression*>(call);
	      Environment* call_env = SEXP_downcast<Environment*>(rho);
	      Environment* working_env = SEXP_downcast<Environment*>(rho1);
	      PairList* promargs = SEXP_downcast<PairList*>(pargs);
	      // FIXME: put in op:
	      ClosureContext cntxt(callx, call_env, 0, working_env, promargs);
	      if (usemethod("$", x, call, pargs, rho1, rho, R_BaseEnv, &value))
		  dispatched = TRUE;
	  }
	  UNPROTECT(2);
	}
	if (dispatched)
	  R_BCNodeStackTop[-1] = value;
	else
	    R_BCNodeStackTop[-1] = R_subset3_dflt(x, PRINTNAME(symbol), R_NilValue);
	NEXT();
      }
    OP(DOLLARGETS, 2):
      {
	int dispatched = FALSE;
	SEXP call = VECTOR_ELT(constants, GETOP());
	SEXP symbol = VECTOR_ELT(constants, GETOP());
	SEXP x = R_BCNodeStackTop[-1];
	value = R_BCNodeStackTop[-2];
	if (isObject(x)) {
	  SEXP pargs, str, rho1;
	  PROTECT(pargs = promiseArgs(CDR(call), rho));
	  /* See comment at first usemethod() call in this file. LT */
	  PROTECT(rho1 = NewEnvironment(R_NilValue, R_NilValue, rho));
	  SET_PRVALUE(CAR(pargs), x);
	  str = ScalarString(PRINTNAME(symbol));
	  SET_PRVALUE(CADR(pargs), str);
	  SET_PRVALUE(CADDR(pargs), value);
	  {
	      Expression* callx = SEXP_downcast<Expression*>(call);
	      Environment* call_env = SEXP_downcast<Environment*>(rho);
	      Environment* working_env = SEXP_downcast<Environment*>(rho1);
	      PairList* promargs = SEXP_downcast<PairList*>(pargs);
	      // FIXME: put in op:
	      ClosureContext cntxt(callx, call_env, 0, working_env, promargs);
	      if (usemethod("$<-", x, call, pargs, rho1, rho, R_BaseEnv, &value))
		  dispatched = TRUE;
	  }
	  UNPROTECT(2);
	}
	R_BCNodeStackTop--;
	if (dispatched)
	  R_BCNodeStackTop[-1] = value;
	else
	  R_BCNodeStackTop[-1] = R_subassign3_dflt(call, x, symbol, value);
	NEXT();
      }
    OP(ISNULL, 0): DO_ISTEST(isNull);
    OP(ISLOGICAL, 0): DO_ISTYPE(LGLSXP);
    OP(ISINTEGER, 0): DO_ISTYPE(INTSXP);
    OP(ISDOUBLE, 0): DO_ISTYPE(REALSXP);
    OP(ISCOMPLEX, 0): DO_ISTYPE(CPLXSXP);
    OP(ISCHARACTER, 0): DO_ISTYPE(STRSXP);
    OP(ISSYMBOL, 0): DO_ISTYPE(SYMSXP);
    OP(ISOBJECT, 0): DO_ISTEST(OBJECT);
    OP(ISNUMERIC, 0): DO_ISTEST(isNumericOnly);
    OP(NVECELT, 2): {
	SEXP vec = R_BCNodeStackTop[-2];
	SEXP idx = R_BCNodeStackTop[-1];
	value = numVecElt(vec, idx);
	R_BCNodeStackTop--;
	R_BCNodeStackTop[-1] = value;
	NEXT();
    }
    OP(NMATELT, 3): {
	SEXP mat = R_BCNodeStackTop[-3];
	SEXP idx = R_BCNodeStackTop[-2];
	SEXP jdx = R_BCNodeStackTop[-1];
	value = numMatElt(mat, idx, jdx);
	R_BCNodeStackTop -= 2;
	R_BCNodeStackTop[-1] = value;
	NEXT();
    }
    OP(SETNVECELT, 3): {
	SEXP vec = R_BCNodeStackTop[-3];
	SEXP idx = R_BCNodeStackTop[-2];
	value = R_BCNodeStackTop[-1];
	value = setNumVecElt(vec, idx, value);
	R_BCNodeStackTop -= 2;
	R_BCNodeStackTop[-1] = value;
	NEXT();
    }
    OP(SETNMATELT, 4): {
	SEXP mat = R_BCNodeStackTop[-4];
	SEXP idx = R_BCNodeStackTop[-3];
	SEXP jdx = R_BCNodeStackTop[-2];
	value = R_BCNodeStackTop[-1];
	value = setNumMatElt(mat, idx, jdx, value);
	R_BCNodeStackTop -= 3;
	R_BCNodeStackTop[-1] = value;
	NEXT();
    }
    LASTOP;
  }

 done:
  R_BCNodeStackTop = oldntop;
#ifdef BC_INT_STACK
  R_BCIntStackTop = olditop;
#endif
#ifdef BC_PROFILING
  current_opcode = old_current_opcode;
#endif
  return value;
}

RObject* ByteCode::evaluate(Environment* env)
{
    return bcEval(this, env);
}

#ifdef THREADED_CODE
SEXP R_bcEncode(SEXP bytes)
{
    SEXP code;
    BCODE *pc;
    int *ipc, i, n, m, v;

    m = (sizeof(BCODE) + sizeof(int) - 1) / sizeof(int);

    n = LENGTH(bytes);
    ipc = INTEGER(bytes);

    v = ipc[0];
    if (v < R_bcMinVersion || v > R_bcVersion) {
	code = allocVector(INTSXP, m * 2);
	pc = reinterpret_cast<BCODE *>( CHAR_RW(code));
	pc[0].i = v;
	pc[1].v = opinfo[BCMISMATCH_OP].addr;
	return code;
    }
    else {
	code = allocVector(INTSXP, m * n);
	pc = reinterpret_cast<BCODE *>( CHAR_RW(code));

	for (i = 0; i < n; i++) pc[i].i = ipc[i];

	/* install the current version number */
	pc[0].i = R_bcVersion;

	for (i = 1; i < n;) {
	    int op = pc[i].i;
	    pc[i].v = opinfo[op].addr;
	    i += opinfo[op].argc + 1;
	}

	return code;
    }
}

static int findOp(void *addr)
{
    int i;

    for (i = 0; i < OPCOUNT; i++)
	if (opinfo[i].addr == addr)
	    return i;
    error(_("cannot find index for threaded code address"));
    return 0; /* not reached */
}

SEXP R_bcDecode(SEXP code) {
    int n, i, j, *ipc;
    BCODE *pc;
    SEXP bytes;

    int m = (sizeof(BCODE) + sizeof(int) - 1) / sizeof(int);

    n = LENGTH(code) / m;
    pc = reinterpret_cast<BCODE *>( CHAR_RW(code));

    bytes = allocVector(INTSXP, n);
    ipc = INTEGER(bytes);

    /* copy the version number */
    ipc[0] = pc[0].i;

    for (i = 1; i < n;) {
	int op = findOp(pc[i].v);
	int argc = opinfo[op].argc;
	ipc[i] = op;
	i++;
	for (j = 0; j < argc; j++, i++)
	    ipc[i] = pc[i].i;
    }

    return bytes;
}
#else
SEXP R_bcEncode(SEXP x) { return x; }
SEXP R_bcDecode(SEXP x) { return duplicate(x); }
#endif

SEXP attribute_hidden do_mkcode(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP bytes, consts;

    checkArity(op, args);
    bytes = CAR(args);
    consts = CADR(args);
    GCStackRoot<> enc(R_bcEncode(bytes));
    GCStackRoot<PairList> pl(SEXP_downcast<PairList*>(consts));
    return GCNode::expose(new ByteCode(enc, pl));
}

SEXP attribute_hidden do_bcclose(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP forms, body, env;

    checkArity(op, args);
    forms = CAR(args);
    body = CADR(args);
    env = CADDR(args);

    if (! isByteCode(body))
	errorcall(call, _("invalid environment"));

    if (isNull(env)) {
	error(_("use of NULL environment is defunct"));
	env = R_BaseEnv;
    } else
    if (!isEnvironment(env))
	errorcall(call, _("invalid environment"));

    return mkCLOSXP(forms, body, env);
}

SEXP attribute_hidden do_is_builtin_internal(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP symbol, i;

    checkArity(op, args);
    symbol = CAR(args);

    if (!isSymbol(symbol))
	errorcall(call, _("invalid symbol"));

    if ((i = INTERNAL(symbol)) != R_NilValue && TYPEOF(i) == BUILTINSXP)
	return R_TrueValue;
    else
	return R_FalseValue;
}

static SEXP disassemble(SEXP bc)
{
  SEXP ans, dconsts;
  int i;
  SEXP code = BCODE_CODE(bc);
  SEXP consts = BCODE_CONSTS(bc);
  SEXP expr = BCODE_EXPR(bc);
  int nc = LENGTH(consts);

  PROTECT(ans = allocVector(VECSXP, expr != R_NilValue ? 4 : 3));
  SET_VECTOR_ELT(ans, 0, install(".Code"));
  SET_VECTOR_ELT(ans, 1, R_bcDecode(code));
  SET_VECTOR_ELT(ans, 2, allocVector(VECSXP, nc));
  if (expr != R_NilValue)
      SET_VECTOR_ELT(ans, 3, duplicate(expr));

  dconsts = VECTOR_ELT(ans, 2);
  for (i = 0; i < nc; i++) {
    SEXP c = VECTOR_ELT(consts, i);
    if (isByteCode(c))
      SET_VECTOR_ELT(dconsts, i, disassemble(c));
    else
      SET_VECTOR_ELT(dconsts, i, duplicate(c));
  }

  UNPROTECT(1);
  return ans;
}

SEXP attribute_hidden do_disassemble(SEXP call, SEXP op, SEXP args, SEXP rho)
{
  SEXP code;

  checkArity(op, args);
  code = CAR(args);
  if (! isByteCode(code))
    errorcall(call, _("argument is not a byte code object"));
  return disassemble(code);
}

SEXP attribute_hidden do_bcversion(SEXP call, SEXP op, SEXP args, SEXP rho)
{
  SEXP ans = allocVector(INTSXP, 1);
  INTEGER(ans)[0] = R_bcVersion;
  return ans;
}

SEXP attribute_hidden do_loadfile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP file, s;
    FILE *fp;

    checkArity(op, args);

    PROTECT(file = coerceVector(CAR(args), STRSXP));

    if (! isValidStringF(file))
	errorcall(call, _("bad file name"));

    fp = RC_fopen(STRING_ELT(file, 0), "rb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));
    s = R_LoadFromFile(fp, 0);
    fclose(fp);

    UNPROTECT(1);
    return s;
}

SEXP attribute_hidden do_savefile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    FILE *fp;

    checkArity(op, args);

    if (!isValidStringF(CADR(args)))
	errorcall(call, _("'file' must be non-empty string"));
    if (TYPEOF(CADDR(args)) != LGLSXP)
	errorcall(call, _("'ascii' must be logical"));

    fp = RC_fopen(STRING_ELT(CADR(args), 0), "wb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));

    R_SaveToFileV(CAR(args), fp, INTEGER(CADDR(args))[0], 0);

    fclose(fp);
    return R_NilValue;
}

#define R_COMPILED_EXTENSION ".Rc"

/* neither of these functions call R_ExpandFileName -- the caller
   should do that if it wants to */
char *R_CompiledFileName(char *fname, char *buf, size_t bsize)
{
    char *basename, *ext;

    /* find the base name and the extension */
    basename = Rf_strrchr(fname, FILESEP[0]);
    if (basename == NULL) basename = fname;
    ext = Rf_strrchr(basename, '.');

    if (ext != NULL && strcmp(ext, R_COMPILED_EXTENSION) == 0) {
	/* the supplied file name has the compiled file extension, so
	   just copy it to the buffer and return the buffer pointer */
	if (snprintf(buf, bsize, "%s", fname) < 0)
	    error(_("R_CompiledFileName: buffer too small"));
	return buf;
    }
    else if (ext == NULL) {
	/* if the requested file has no extention, make a name that
	   has the extenrion added on to the expanded name */
	if (snprintf(buf, bsize, "%s%s", fname, R_COMPILED_EXTENSION) < 0)
	    error(_("R_CompiledFileName: buffer too small"));
	return buf;
    }
    else {
	/* the supplied file already has an extention, so there is no
	   corresponding compiled file name */
	return NULL;
    }
}

FILE *R_OpenCompiledFile(char *fname, char *buf, size_t bsize)
{
    char *cname = R_CompiledFileName(fname, buf, bsize);

    if (cname != NULL && R_FileExists(cname) &&
	(strcmp(fname, cname) == 0 ||
	 ! R_FileExists(fname) ||
	 R_FileMtime(cname) > R_FileMtime(fname)))
	/* the compiled file cname exists, and either fname does not
	   exist, or it is the same as cname, or both exist and cname
	   is newer */
	return R_fopen(buf, "rb");
    else return NULL;
}

SEXP attribute_hidden do_putconst(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP code, c, ans;
    int i, n;

    checkArity(op, args);
    code = CAR(args);
    if (TYPEOF(code) != VECSXP)
	error(_("code must be a generic vector"));
    c = CADR(args);

    n = LENGTH(code);
    ans = allocVector(VECSXP, n + 1);
    for (i = 0; i < n; i++)
	SET_VECTOR_ELT(ans, i, VECTOR_ELT(code, i));
    SET_VECTOR_ELT(ans, n, c);

    return ans;
}

#ifdef BC_PROFILING
SEXP R_getbcprofcounts()
{
    SEXP val;
    int i;

    val = allocVector(INTSXP, OPCOUNT);
    for (i = 0; i < OPCOUNT; i++)
	INTEGER(val)[i] = opcode_counts[i];
    return val;
}

static void dobcprof(int sig)
{
    if (current_opcode >= 0 && current_opcode < OPCOUNT)
	opcode_counts[current_opcode]++;
    signal(SIGPROF, dobcprof);
}

SEXP R_startbcprof()
{
    struct itimerval itv;
    int interval;
    double dinterval = 0.02;
    int i;

    if (Evaluator::profiling())
	error(_("profile timer in use"));
    if (bc_profiling)
	error(_("already byte code profiling"));

    /* according to man setitimer, it waits until the next clock
       tick, usually 10ms, so avoid too small intervals here */
    interval = 1e6 * dinterval + 0.5;

    /* initialize the profile data */
    current_opcode = NO_CURRENT_OPCODE;
    for (i = 0; i < OPCOUNT; i++)
	opcode_counts[i] = 0;

    signal(SIGPROF, dobcprof);

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = interval;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = interval;
    if (setitimer(ITIMER_PROF, &itv, NULL) == -1)
	error(_("setting profile timer failed"));

    bc_profiling = TRUE;

    return R_NilValue;
}

static void dobcprof_null(int sig)
{
    signal(SIGPROF, dobcprof_null);
}

SEXP R_stopbcprof()
{
    struct itimerval itv;

    if (! bc_profiling)
	error(_("not byte code profiling"));

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    signal(SIGPROF, dobcprof_null);

    bc_profiling = FALSE;

    return R_NilValue;
}
#else
SEXP R_getbcprofcounts() { return R_NilValue; }
SEXP R_startbcprof() { return R_NilValue; }
SEXP R_stopbcprof() { return R_NilValue; }
#endif
#endif

// Suppress Rf_match expansion:
#undef match

#include "CXXR/ArgMatcher.hpp"

/* Create a promise to evaluate each argument.	Although this is most */
/* naturally attacked with a recursive algorithm, we use the iterative */
/* form below because it is does not cause growth of the pointer */
/* protection stack, and because it is a little more efficient. */

SEXP attribute_hidden promiseArgs(SEXP el, SEXP rho)
{
    return ArgMatcher::prepareArgs(SEXP_downcast<PairList*>(el),
				   SEXP_downcast<Environment*>(rho));
}
