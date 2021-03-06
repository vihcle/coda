/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2016 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights
                           none currently

#*/

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "coda_assert.h"

#ifdef __cplusplus
}
#endif

#include <cvnode.h>
#include <vlist.h>

extern vle *FindVLE(dlist &dl, ViceFid *fid);

int AllowResolution   = 1;
int DumpVM            = 0;
int large             = 500;
int small             = 500;
const char *CodaSrvIp = NULL;

ViceVersionVector NullVV = { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0 }, 0 };

void PollAndYield()
{
    dlist dl;
    ViceFid fid;

    CODA_ASSERT(0);
    FindVLE(dl, &fid);
    return;
}

void Die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    CODA_ASSERT(0);
    return;
}

int GetFsObj(ViceFid *fid, Volume **volptr, Vnode **vptr, int lock,
             int VolumeLock, int ignoreIncon, int ignoreBQ, int getdirhandle)
{
    CODA_ASSERT(0);
    return (0);
}
