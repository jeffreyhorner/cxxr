/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1999-2013 The R Core Team.
 *  Copyright (C) 2008-2014  Andrew R. Runnalls.
 *  Copyright (C) 2014 and onwards the CXXR Project Authors.
 *
 *  CXXR is not part of the R project, and bugs and other issues should
 *  not be reported via r-bugs or other R project channels; instead refer
 *  to the CXXR website.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

#ifndef R_DEFINES_H
#define R_DEFINES_H

#if !defined(R_R_H) && !defined(R_S_H)
/* user forgot to include R.h or S.h */
# include <R_ext/Memory.h>
# include <R_ext/RS.h>
#endif

#include <Rinternals.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Much is from John Chambers' "Programming With Data".
 *  Some of this is from Doug Bates.
 *
 *  It is presented here to support a joint programming style which
 *  will work in both R and S.  In particular it helps with:
 *
 *    1. S/R <-> CORBA code.
 *    2. S/R <-> Java Code.
 *
 *  And to hide some internal nastiness.
 */


/*
 *  Added some macros defined in S.h from Splus 5.1
 */

#define NULL_USER_OBJECT	R_NilValue

#define AS_LOGICAL(x)		coerceVector(x,LGLSXP)
#define AS_INTEGER(x)		coerceVector(x,INTSXP)
#define AS_NUMERIC(x)		coerceVector(x,REALSXP)
#define AS_CHARACTER(x)		coerceVector(x,STRSXP)
#define AS_COMPLEX(x)		coerceVector(x,CPLXSXP)
#define AS_VECTOR(x)		coerceVector(x,VECSXP)
#define AS_LIST(x)		coerceVector(x,VECSXP)
#define AS_RAW(x)		coerceVector(x,RAWSXP)

#define IS_LOGICAL(x)		Rf_isLogical(x)
#define IS_INTEGER(x)		Rf_isInteger(x)
#define IS_NUMERIC(x)		Rf_isReal(x)
#define IS_CHARACTER(x)		Rf_isString(x)
#define IS_COMPLEX(x)		Rf_isComplex(x)
/* NB: is this right? It means atomic or VECSXP or EXPRSXP */
#define IS_VECTOR(x)		Rf_isVector(x)
/* And this cannot be right: isVectorList(x)? */
#define IS_LIST(x)		IS_VECTOR(x)
#define IS_RAW(x)		(TYPEOF(x) == RAWSXP)

#define NEW_LOGICAL(n)		Rf_allocVector(LGLSXP,n)
#define NEW_INTEGER(n)		Rf_allocVector(INTSXP,n)
#define NEW_NUMERIC(n)		Rf_allocVector(REALSXP,n)
#define NEW_CHARACTER(n)	Rf_allocVector(STRSXP,n)
#define NEW_COMPLEX(n)		Rf_allocVector(CPLXSXP,n)
#define NEW_LIST(n)		Rf_allocVector(VECSXP,n)
#define NEW_STRING(n)		NEW_CHARACTER(n)
#define NEW_RAW(n)		Rf_allocVector(RAWSXP,n)

#define LOGICAL_POINTER(x)	LOGICAL(x)
#define INTEGER_POINTER(x)	INTEGER(x)
#define NUMERIC_POINTER(x)	REAL(x)
#define COMPLEX_POINTER(x)	COMPLEX(x)
#define RAW_POINTER(x)		RAW(x)

/* The following are not defined in `Programming with Data' but are
   defined in S.h in Svr4 */

/*
 * Note that LIST_DATA and RAW_DATA are missing.
 * This is consistent with Svr4.
 */

#define LOGICAL_DATA(x)		(LOGICAL(x))
#define INTEGER_DATA(x)		(INTEGER(x))
#define DOUBLE_DATA(x)		(REAL(x))
#define NUMERIC_DATA(x)		(REAL(x))
#define COMPLEX_DATA(x)		(COMPLEX(x))

#define LOGICAL_VALUE(x)	asLogical(x)
#define INTEGER_VALUE(x)	asInteger(x)
#define NUMERIC_VALUE(x)	asReal(x)
#define CHARACTER_VALUE(x)	CHAR(asChar(x))
#define STRING_VALUE(x)		CHAR(asChar(x))
#define LIST_VALUE(x)		error("the 'value' of a list object is not defined")
#define RAW_VALUE(x)		error("the 'value' of a raw object is not defined")

#define SET_ELEMENT(x, i, val)	SET_VECTOR_ELT(x, i, val)
#define GET_ATTR(x,what)       	getAttrib(x, what)
#define GET_CLASS(x)       	getAttrib(x, R_ClassSymbol)
#define GET_DIM(x)       	getAttrib(x, R_DimSymbol)
#define GET_DIMNAMES(x)       	getAttrib(x, R_DimNamesSymbol)
#define GET_COLNAMES(x)       	GetColNames(x)
#define GET_ROWNAMES(x)       	GetRowNames(x)
#define GET_LEVELS(x)       	getAttrib(x, R_LevelsSymbol)
#define GET_TSP(x)       	getAttrib(x, R_TspSymbol)
#define GET_NAMES(x)		getAttrib(x, R_NamesSymbol)
#define SET_ATTR(x, what, n)     	setAttrib(x, what, n)
#define SET_CLASS(x, n)     	setAttrib(x, R_ClassSymbol, n)
#define SET_DIM(x, n)     	setAttrib(x, R_DimSymbol, n)
#define SET_DIMNAMES(x, n)     	setAttrib(x, R_DimNamesSymbol, n)
#define SET_LEVELS(x, l)       	setAttrib(x, R_LevelsSymbol, l)
#define SET_NAMES(x, n)		setAttrib(x, R_NamesSymbol, n)
/* These do not support long vectors */
#define GET_LENGTH(x)		length(x)
#define SET_LENGTH(x, n)	(x = lengthgets(x, n))

#define GET_SLOT(x, what)       R_do_slot(x, what)
#define SET_SLOT(x, what, value)  R_do_slot_assign(x, what, value)

#define MAKE_CLASS(what)	R_do_MAKE_CLASS(what)
/* NEW_OBJECT is recommended; NEW is for green book compatibility */
#define NEW_OBJECT(class_def)	R_do_new_object(class_def)
#define NEW(class_def)		R_do_new_object(class_def)

#define s_object                SEXPREC
#define S_EVALUATOR             /**/

/* These conflict with definitions in R_ext/Boolean.h,
   but spatstat relies on them in a C file */
#ifdef __cplusplus
# ifndef R_EXT_BOOLEAN_H_
#  ifndef TRUE
#   define TRUE 1
#  endif
#  ifndef FALSE
#   define FALSE 0
#  endif
# endif
#else
#  ifndef TRUE
#   define TRUE 1
#  endif
#  ifndef FALSE
#   define FALSE 0
#  endif
#endif

#define COPY_TO_USER_STRING(x)	mkChar(x)
#define CREATE_STRING_VECTOR(x)	mkChar(x)

#define CREATE_FUNCTION_CALL(name, argList) createFunctionCall(name, argList)

#define EVAL(x)			eval(x,R_GlobalEnv)

#ifdef __cplusplus
}
#endif

#endif
