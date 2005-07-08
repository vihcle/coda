/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2003 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights
                           none currently

#*/



/* this file contains local subtree representation code */

#ifdef __cplusplus
extern "C" {
#endif

#include <struct.h>

#ifdef __cplusplus
}
#endif

/* interfaces */
#include <vcrcommon.h>

/* from venus */
#include "fso.h"
#include "local.h"
#include "venusrecov.h"
#include "venusvol.h"
#include "worker.h"

/* code is obsolete  -- Adam 7/8/05*/
