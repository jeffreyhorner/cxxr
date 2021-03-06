/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1999-2006   The R Development Core Team.
 *  Copyright (C) 2008-2014  Andrew R. Runnalls.
 *  Copyright (C) 2014 and onwards the CXXR Project Authors.
 *
 *  CXXR is not part of the R project, and bugs and other issues should
 *  not be reported via r-bugs or other R project channels; instead refer
 *  to the CXXR website.
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

/** @file ComplexVector.h
 * @brief Class CXXR::ComplexVector and associated C interface.
 */

#ifndef COMPLEXVECTOR_H
#define COMPLEXVECTOR_H

#include "CXXR/VectorBase.h"
#include "R_ext/Complex.h"

#ifdef __cplusplus

#include "R_ext/Arith.h"
#include "CXXR/Complex.hpp"
#include "CXXR/FixedVector.hpp"
#include "CXXR/SEXP_downcast.hpp"

namespace CXXR {

    template <>
    inline const char* FixedVector<Complex, CPLXSXP>::staticTypeName()
    {
	return "complex";
    }

    /** @brief Vector of complex numbers.
     */
    typedef CXXR::FixedVector<Complex, CPLXSXP> ComplexVector;
}  // namespace CXXR

extern "C" {
#endif /* __cplusplus */

    /**
     * @param s Pointer to an RObject.
     * @return TRUE iff the RObject pointed to by \a s is a complex vector.
     */
#ifndef __cplusplus
    Rboolean Rf_isComplex(SEXP s);
#else
    inline Rboolean Rf_isComplex(SEXP s)
    {
	return Rboolean(s && TYPEOF(s) == CPLXSXP);
    }
#endif

/**
 * @param x Pointer to a CXXR::ComplexVector (i.e. an R complex vector).
 *          An error is generated if \a x is not a non-null pointer to a
 *          CXXR::ComplexVector .
 *
 * @return Pointer to element 0 of \a x .
 */
#ifndef __cplusplus
Rcomplex *COMPLEX(SEXP x);
#else
inline Rcomplex *COMPLEX(SEXP x)
{
    using namespace CXXR;
    return &(*SEXP_downcast<ComplexVector*>(x, false))[0];
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* COMPLEXVECTOR_H */
