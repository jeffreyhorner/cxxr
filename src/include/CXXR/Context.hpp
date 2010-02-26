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
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

/** @file Context.hpp
 *
 * @brief Class CXXR::Context.
 */

#ifndef CONTEXT_HPP
#define CONTEXT_HPP 1

#include "CXXR/Evaluator.h"
#include "CXXR/GCStackRoot.h"
#include "CXXR/Promise.h"

extern "C" {
    // Parked here pending the creation of an ErrorHandling class:
    extern CXXR::GCRoot<> R_HandlerStack;  // Condition handler stack
    extern CXXR::GCRoot<> R_RestartStack;  // Stack of available restarts

    // Parked here temporarily:
    extern CXXR::RObject* R_Srcref;
}

namespace CXXR {
    class Context {
    public:
	/* The Various Context Types.
	 *
	* In general the type is a bitwise OR of the values below.
	* Note that LOOP is already the or of NEXT and BREAK.
	* Only functions should have the third bit turned on;
	* this allows us to move up the context stack easily
	* with either RETURN's or GENERIC's or RESTART's.
	* If you add a new context type for functions make sure
	* NEWTYPE & FUNCTION > 0
	*/
	enum Type {
	    TOPLEVEL = 0,
	    NEXT     = 1,
	    BREAK    = 2,
	    LOOP     = 3,	// break OR next target
	    FUNCTION = 4,
	    RETURN   = 12,
	    BROWSER  = 16,
	    GENERIC  = 20,
	    RESTART  = 32,
	    BUILTIN  = 64  // used in profiling
	};

	Context();

	/** @brief The future destructor.
	 *
	 * Restores values saved by the constructor.
	 */
	void end();

	Context *nextcontext;        // The next context up the chain
	Type callflag;		     // The context "type"
	unsigned int cstacktop;	     // Top of the pointer protection stack
	int evaldepth;	             // evaluation depth at inception
	GCStackRoot<> promargs;      // Promises supplied to closure
	GCStackRoot<> callfun;       // The closure called
	GCStackRoot<> sysparent;     // environment the closure was called from
	GCStackRoot<> call;          // The call that effected this context
	GCStackRoot<Environment> cloenv;  // The environment
	GCStackRoot<> conexit;	     // Interpreted "on.exit" code
	void *vmax;	             // size of R_alloc stack
	Rboolean intsusp;            // interrupts are suspended
	GCStackRoot<> handlerstack;  // condition handler stack
	GCStackRoot<> restartstack;  // stack of available restarts
	GCStackRoot<> srcref;	     // The source line in effect
#ifdef BYTECODE
	SEXP *nodestack;
#ifdef BC_INT_STACK
	IStackval *intstack;
#endif
#endif
    };
}  // namespace CXXR

extern SEXP R_ReturnedValue;

#define IS_RESTART_BIT_SET(flags) ((flags) & Context::RESTART)
#define SET_RESTART_BIT_ON(flags) (flags = Context::Type(flags | Context::RESTART))
#define SET_RESTART_BIT_OFF(flags) (flags = Context::Type(flags & ~Context::RESTART))

extern CXXR::Context* R_Toplevel;         /* The ultimate toplevel environment */
extern CXXR::Context* R_ToplevelContext;  /* The toplevel environment */
extern CXXR::Context* R_GlobalContext;    /* The global environment */

void Rf_begincontext(CXXR::Context*, CXXR::Context::Type, SEXP, SEXP, SEXP,
		     SEXP, SEXP);
SEXP Rf_dynamicfindVar(SEXP, CXXR::Context*);
void Rf_endcontext(CXXR::Context*);
void Rf_findcontext(int, SEXP, SEXP);
int Rf_framedepth(CXXR::Context*);
void R_InsertRestartHandlers(CXXR::Context *, Rboolean);
void R_JumpToContext(CXXR::Context *, int, SEXP);
SEXP R_syscall(int,CXXR::Context*);
int R_sysparent(int,CXXR::Context*);
SEXP R_sysframe(int,CXXR::Context*);
SEXP R_sysfunction(int,CXXR::Context*);
void R_run_onexits(CXXR::Context *);
void R_restore_globals(CXXR::Context *);

extern "C" {
    void Rf_jump_to_toplevel(void);
}

#endif  // CONTEXT_HPP
