/* BLURB gpl

                           Coda File System
                              Release 5

          Copyright (c) 1987-1999 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights

#*/

/*
                         IBM COPYRIGHT NOTICE

                          Copyright (C) 1986
             International Business Machines Corporation
                         All Rights Reserved

This  file  contains  some  code identical to or derived from the 1986
version of the Andrew File System ("AFS"), which is owned by  the  IBM
Corporation.   This  code is provided "AS IS" and IBM does not warrant
that it is free of infringement of  any  intellectual  rights  of  any
third  party.    IBM  disclaims  liability of any kind for any damages
whatsoever resulting directly or indirectly from use of this  software
or  of  any  derivative work.  Carnegie Mellon University has obtained
permission to  modify,  distribute and sublicense this code,  which is
based on Version 2  of  AFS  and  does  not  contain  the features and
enhancements that are part of  Version 3 of  AFS.  Version 3 of AFS is
commercially   available   and  supported  by   Transarc  Corporation,
Pittsburgh, PA.

*/

#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/param.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "coda_string.h"
#include <unistd.h>
#include <stdlib.h>

#include <lwp.h>
#include <lock.h>
#include <util.h>
#include <vice.h>
#include "codadir.h"

#ifdef __cplusplus
}
#endif __cplusplus

#include <rec_smolist.h>
#include "voltypes.h"
#include "cvnode.h"
#include "volume.h"
#include "viceinode.h"
#include "partition.h"
#include "vutil.h"
#include "recov.h"
#include "index.h"
#include "coda_assert.h"


void PurgeIndex(Volume *vp, VnodeClass vclass);

/* Purge a volume and all its from the fileserver and recoverable storage */

/* N.B.  it's important here to use the partition pointed to by the
   volume header.  This routine can, under some circumstances, be
   called when two volumes with the same id exist on different
   partitions */

void VPurgeVolume(Volume *vp)
{
    Error ec;
    PurgeIndex(vp, vLarge);
    PurgeIndex(vp, vSmall);
    CODA_ASSERT(DeleteVolume(V_id(vp)) == 0);

    /* The following is done in VDetachVolume - but that calls*/
    /* fsync stuff also which I dont understand yet */
    /* IF some day we understand that code we should use VDetachVolume */
    /*			--- Puneet 03/9/90 */

    /* get rid of all traces of the volume in vm */
    DeleteVolumeFromHashTable(vp);
    vp->shuttingDown = 1;
    VPutVolume(vp);	    /* this frees the volume since shutting down = 1 */
    vp = 0;
}

/* Decrement the reference count for all files in this volume */
void PurgeIndex(Volume *vp, VnodeClass vclass) 
{
    struct VnodeClassInfo *vcp = &VnodeClassInfo_Array[vclass];
    char zerobuf[SIZEOF_LARGEDISKVNODE];
    struct VnodeDiskObject *zerovn = (struct VnodeDiskObject *) zerobuf;
    struct VolumeData *vdata = &(SRV_RVM(VolumeList[V_volumeindex(vp)]).data);
    rec_smolist *vnlist;
    int nLists;
    
    if (vclass == vSmall) {
	vnlist = vdata->smallVnodeLists;
	nLists = vdata->nsmallLists;
    } else {	/* Large */
	vnlist = vdata->largeVnodeLists;
	nLists = vdata->nlargeLists;
    }
    
    memset(zerovn, 0, SIZEOF_LARGEDISKVNODE);

    for (int i = 0; i < nLists; i++) {
	rec_smolink *p;
	VnodeDiskObject *vdo;

	while(p = vnlist[i].get()) {	/* Pull the vnode off the list. */
	    vdo = strbase(VnodeDiskObject, p, nextvn);
	    
	    if ((vdo->type != vNull) && (vdo->vnodeMagic != vcp->magic)){
		LogMsg(0, VolDebugLevel, stdout, "PurgeIndex:VnodeMagic field incorrect for vnode %d",i);
		CODA_ASSERT(0);
	    }
	    if (vdo->inodeNumber){
		/* decrement the reference count by one */
		if (vdo->type != vDirectory){
		    idec(vp->device, vdo->inodeNumber, V_parentId(vp));
		} else
		    DI_Dec((DirInode *)vdo->inodeNumber);
	    }	
	    /* Delete the vnode */
	    if ((vclass == vSmall) &&
	        (SRV_RVM(SmallVnodeIndex) < SMALLFREESIZE - 1)) {
		LogMsg(29, VolDebugLevel, stdout, 	"DeleteVolData:	Adding small vnode index %d to free list",i);
		rvmlib_modify_bytes(vdo, zerovn, SIZEOF_SMALLDISKVNODE);
		RVMLIB_MODIFY(SRV_RVM(SmallVnodeIndex),
			      SRV_RVM(SmallVnodeIndex) + 1);
		RVMLIB_MODIFY(SRV_RVM(SmallVnodeFreeList[SRV_RVM(SmallVnodeIndex)]), vdo);
	    }
	    else if ((vclass == vLarge) &&
		     (SRV_RVM(LargeVnodeIndex) < LARGEFREESIZE - 1)) {
		LogMsg(29, VolDebugLevel, stdout, 	"DeleteVolData:	Adding large vnode index %d to free list",i);
		rvmlib_modify_bytes(vdo, zerovn, SIZEOF_LARGEDISKVNODE);
		RVMLIB_MODIFY(SRV_RVM(LargeVnodeIndex),
			      SRV_RVM(LargeVnodeIndex) + 1);
		RVMLIB_MODIFY(SRV_RVM(LargeVnodeFreeList[SRV_RVM(LargeVnodeIndex)]), vdo);
	    } else {
		rvmlib_rec_free((char *)vdo);
		LogMsg(29, VolDebugLevel, stdout,  "DeleteVolData: Freeing small vnode index %d", i);
	    }
	}
    }
}

/* NEED TO REVIEW THIS CODE -- WRITTEN UNDER HASTE */

