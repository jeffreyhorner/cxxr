/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1999-2014  The R Core Team
 *  Copyright (C) 2008-2014  Andrew R. Runnalls.
 *  Copyright (C) 2014 and onwards the CXXR Project Authors.
 *
 *  CXXR is not part of the R project, and bugs and other issues should
 *  not be reported via r-bugs or other R project channels; instead refer
 *  to the CXXR website.
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

#ifndef STARTUP_H_
#define STARTUP_H_

#include <R_ext/RStartup.h>	/* The meat here */
#ifdef _WIN32
extern UImode  CharacterMode;
#endif


#ifdef __cplusplus
extern "C" {
#endif

/* originally from Defn.h : */

void R_CleanUp(SA_TYPE, int, int);
void R_StartUp(void);

FILE *R_OpenInitFile(void);
FILE *R_OpenSysInitFile(void);
FILE *R_OpenSiteFile(void);

#ifdef __cplusplus
}
#endif

#endif /* STARTUP_H */
