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
                           none currently

#*/

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include "coda_assert.h"
#include "coda_string.h"
#include <stdio.h>
#include <struct.h>
#include <lwp/lwp.h>
#include <rpc2/rpc2.h>
#include <rpc2/errors.h>
#include <util.h>
#include <codadir.h>

#ifdef __cplusplus
}
#endif

#include <olist.h>
#include <res.h>
#include <volume.h>
#include <srv.h>
#include <inconsist.h>
#include <vlist.h>
#include <vrdb.h>
#include <volume.h>
#include <resutil.h>
#include <rescomm.h>
#include <resforce.h>
#include <lockqueue.h>
#include <timing.h>
#include "rvmrestiming.h"
#include "resstats.h"
#include "resolution.h"
#include "rescoord.h"

// ********** Private Routines *************
static int ComparePhase3Status(res_mgrpent *, int *, ViceStatus *, ViceStatus *);
static char *CoordPhase2(res_mgrpent *, ViceFid *, int *, int *, int *, unsigned long *,dirresstats *);
static int CoordPhase3(res_mgrpent*, ViceFid*, char*, int, int, ViceVersionVector**, dlist*, ResStatus**, unsigned long*, int*);
static int CoordPhase4(res_mgrpent *, ViceFid *, unsigned long *, int *);
static int CoordPhase34(res_mgrpent *, ViceFid *, dlist *, int *, int *);
static void AllocateBufs(res_mgrpent *, char **, int *);
static void DeAllocateBufs(char **);
static char *ConcatLogs(res_mgrpent *, char **, RPC2_Integer *, RPC2_Integer *, int *, int *);
static void UpdateStats(ViceFid *, dirresstats *);

// * Dir Resolution with logs in RVM
// * This consists of 4 phases
// *	Phase 1: Locking
// *		Volume gets locked at each subordinate
// *		Each server returns status of path from root
// *		Coordinator makes sure that the ancestors are all equal 
// *
// *	Phase 2: Log Collection and Merging
// *		Each subordinate returns the log of related objects 
// *			as a byte stream
// *		Coordinator merges these logs together into a big 
// *			linear buffer
// *
// *	Phase 3: Log Distribution and Compensation
// *		Coordinator distributes the combined logs 
// *		Subordinates parse logs, compute compensating operations
// *		and perform the operations
// * 		Subs return list of inconsistencies, if any, that arose.
// *
// *	Phase 3.5: (Phase34)
// *		Coordinator distributes list of inconsistencies to all sites
// *		Sub's make sure that inconsistency is present  
// *	Phase 4: 
// *		Coordinator ships out a new storeid for each subordinate's replica
// *		Subs stamp their replicas with storeid and unlock volume (not done currently)
// *			(for now subs also return directory contents)
// *		Coordinator compares contents and makes sure directories are equal
// *		(This isn't necessary in a production server)
// *     	Phase 5:
// *		In case of problems it marks the replicas inconsistent
//

long RecovDirResolve(res_mgrpent *mgrp, ViceFid *Fid, ViceVersionVector **VV, 
		     ResStatus **rstatusp, int *sizes)
{
    int reserror = EINCONS;
    char *AllLogs = NULL;
    int totalentries = 0;
    int totalsize = 0;
    int dirlengths[VSG_MEMBERS];
    unsigned long succFlags[VSG_MEMBERS];
    dlist *inclist = NULL;
    dirresstats drstats;
    long retval;
    int noinc = -1;		// used only for updating res stats (dept statistics)
    
    SLog(0, "Entering RecovDirResolve %s\n", FID_(Fid));

    // Check if regular Directory Resolution can deal with this case
    {	
	retval = RegDirResolution(mgrp, Fid, VV, rstatusp);
	if (!retval) {
	    SLog(0, "RecovDirResolve: RegDirResolution succeeded\n");
	    // for statistics collection
	    drstats.dir_nowork++;
	    drstats.dir_succ++;
	    noinc = 1;

	    reserror = 0;
	    goto Exit;
	}
	/* If the group was already marked as inconsistent, there is no need
	 * to tag them again or retry the log-based resolution */
	if (retval == EINCONS) {
	    reserror = 0;
	    goto Exit;
	}
	retval = 0;
    }

    // res stats stuff 
    {
	drstats.dir_nresolves++;
	if (mgrp->IncompleteVSG())
	    drstats.dir_incvsg++;
    }

    // Phase 1, locking the volume, has already been done by ViceResolve
    
    // Phase 2 
    {
	PROBE(tpinfo, RecovCoorP2Begin);
	AllLogs = CoordPhase2(mgrp, Fid, &totalentries, sizes, 
			      &totalsize, succFlags, &drstats);
	PROBE(tpinfo, RecovCoorP2End);
	if (!AllLogs) {
	    LogMsg(0, SrvDebugLevel, stdout,
		   "RecovDirResolve: Error during phase2\n");
	    goto Exit;
	}
	PollAndYield();
    }
    // Phase3
    {	
	PROBE(tpinfo, RecovCoorP3Begin);
	inclist = new dlist((CFN)CompareIlinkEntry);
	if (CoordPhase3(mgrp, Fid, AllLogs, totalsize, totalentries, VV, 
			inclist, rstatusp, succFlags, dirlengths)) {
	    LogMsg(0, SrvDebugLevel, stdout,
		   "RecovDirResolve: Error during phase 3\n");
	    goto Exit;
	}
	PROBE(tpinfo, RecovCoorP3End);
    }

    // Phase34
    {
	PROBE(tpinfo, RecovCoorP34Begin);
	if (CoordPhase34(mgrp, Fid, inclist, dirlengths, &noinc)) {
	    LogMsg(0, SrvDebugLevel, stdout,
		   "RecovDirResolve: Error during phase 34\n");
	    goto Exit;
	}	    
	PROBE(tpinfo, RecovCoorP34End);
    }
    
    // Phase 4
    {
	PROBE(tpinfo, RecovCoorP4Begin);
	if (!CoordPhase4(mgrp, Fid, succFlags, dirlengths)) {
	    reserror = 0;
	    noinc = 1;
	    drstats.dir_succ++;
	}
	PROBE(tpinfo, RecovCoorP4End);
    }
  Exit:
    // mark object inconsistent in case of error 
    // Phase5
    if (reserror) {
	MRPC_MakeMulti(MarkInc_OP, MarkInc_PTR, VSG_MEMBERS,
		       mgrp->rrcc.handles, mgrp->rrcc.retcodes, 
		       mgrp->rrcc.MIp, 0, 0, Fid);
	noinc = 0;
	drstats.dir_conf++;
    }
    // clean up
    {
	if (AllLogs) free(AllLogs);
	if (inclist) CleanIncList(inclist);
    }
    SLog(1, "RecovDirResolve returns %d\n", retval);
    UpdateStats(Fid, &drstats);
    return(retval);
}

// collect logs for a directory 
// return pointer to buffer.
static char *CoordPhase2(res_mgrpent *mgrp, ViceFid *fid, 
			  int *totalentries, int *sizes, 
			  int *totalsize, unsigned long *successFlags, 
			  dirresstats *drstats) {
    
    char *bufs[VSG_MEMBERS];
    char *logbuffer = NULL;
    *totalsize = 0;
    *totalentries = 0;
    int errorCode = 0;
    
    LogMsg(9, SrvDebugLevel, stdout,  
	   "CoordPhase2: Fetching logs for %x.%x.%x",
	   fid->Volume, fid->Vnode, fid->Unique);
    
    AllocateBufs(mgrp, bufs, sizes);
    
    // set up the parameters 
    SE_Descriptor sid;
    memset(&sid, 0, sizeof(SE_Descriptor));
    sid.Tag = SMARTFTP;
    sid.Value.SmartFTPD.TransmissionDirection = SERVERTOCLIENT;
    sid.Value.SmartFTPD.Tag = FILEINVM;
    sid.Value.SmartFTPD.ByteQuota = -1;
    ARG_MARSHALL(IN_OUT_MODE, SE_Descriptor, sidvar, sid, VSG_MEMBERS);
    for (int i = 0; i < VSG_MEMBERS; i++) {
	sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqLen = 
	    sizes[i];
	sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.MaxSeqLen = 
	    sizes[i];
	sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqBody = 
	    (RPC2_ByteSeq)bufs[i];
    }
    int logsize = 0;
    ARG_MARSHALL(OUT_MODE, RPC2_Integer, logsizevar, logsize, VSG_MEMBERS);
    int nentries = 0;
    ARG_MARSHALL(OUT_MODE, RPC2_Integer, nentriesvar, nentries, VSG_MEMBERS);
    
    // fetch the logs 
    {
	LogMsg(9, SrvDebugLevel, stdout,  
	       "CoordPhase2: Going to do Multirpc fetch");
	MRPC_MakeMulti(FetchLogs_OP, FetchLogs_PTR, VSG_MEMBERS, 
		       mgrp->rrcc.handles, mgrp->rrcc.retcodes, 
		       mgrp->rrcc.MIp, 0, 0, fid, logsizevar_ptrs,
		       nentriesvar_ptrs, sidvar_bufs);
	LogMsg(39, SrvDebugLevel, stdout,  "CollectLogs: ret codes from FetchLog [%d %d %d %d %d %d %d %d]",
	       mgrp->rrcc.retcodes[0], mgrp->rrcc.retcodes[1], 
	       mgrp->rrcc.retcodes[2], mgrp->rrcc.retcodes[3], 
	       mgrp->rrcc.retcodes[4], mgrp->rrcc.retcodes[5], 
	       mgrp->rrcc.retcodes[6], mgrp->rrcc.retcodes[7]);
	mgrp->CheckResult();
	if ((errorCode = CheckRetCodes((unsigned long *)mgrp->rrcc.retcodes, 
				      mgrp->rrcc.hosts, successFlags))) {
	    LogMsg(0, SrvDebugLevel, stdout,  
		   "Error Code from at least one server sending log");
	   goto Exit;
       }
       LogMsg(9, SrvDebugLevel, stdout,  
	      "CoordPhase2: Returned from Multirpc fetch \n");
   }
   
   
   // concat into a big buf 
   {
       logbuffer = ConcatLogs(mgrp, bufs, logsizevar_bufs, 
			      nentriesvar_bufs, totalsize, totalentries);
   }

 Exit:
   {
       DeAllocateBufs(bufs);
   }
   if (logbuffer) {
       drstats->logshipstats.add(*totalsize, (int *)nentriesvar_bufs, VSG_MEMBERS);
   }
   return(logbuffer);
       
}

static void AllocateBufs(res_mgrpent *mgrp, char **bufs, int *sizes) {
    for (int i = 0; i < VSG_MEMBERS; i++) {
	if (sizes[i] > 0 && mgrp->rrcc.handles[i]) 
	    bufs[i] = (char *)malloc(sizes[i]);
	else {
	    sizes[i] = 0;
	    bufs[i] = 0;
	}
    }
    LogMsg(39, SrvDebugLevel, stdout,  
	   "AllocateBufs: Log sizes are [%d %d %d %d %d %d %d %d]",
	   sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], sizes[5], 
	   sizes[6], sizes[7]);
}

static void DeAllocateBufs(char **bufs) {
    for (int i = 0; i < VSG_MEMBERS; i++) 
	if (bufs[i]) {
	    free(bufs[i]);
	    bufs[i] = 0;
	}
}

static char *ConcatLogs(res_mgrpent *mgrp, char **bufs, 
			 RPC2_Integer *sizes, RPC2_Integer *entries, 
			 int *totalsize, int *totalentries) {
    char *logbuffer = NULL;
    // calculate the size 
    for (int i = 0; i < VSG_MEMBERS; i++) {
	if (mgrp->rrcc.hosts[i] && mgrp->rrcc.retcodes[i] == 0) {
	    *totalsize += sizes[i];
	    *totalentries += entries[i];
	}
	else {
	    sizes[i] = 0;
	    entries[i] = 0;
	}
    }
    if ((*totalsize > 0) && (*totalentries > 0)) 
	logbuffer = (char *)malloc(*totalsize);

    /* copy into buf */
    char *tmp = logbuffer;
    if (logbuffer) {
	for (int i = 0; i < VSG_MEMBERS; i++) 
	    if (mgrp->rrcc.hosts[i] &&
		(mgrp->rrcc.retcodes[i] == 0) &&
		bufs[i]) {
		memmove((void *)tmp, (const void *)bufs[i], sizes[i]);
		tmp += sizes[i];
	    }
    }
    return(logbuffer);
}


static int CoordPhase3(res_mgrpent *mgrp, ViceFid *Fid, char *AllLogs,
		       int logsize, int totalentries, ViceVersionVector **VV,
		       dlist *inclist, ResStatus **rstatusp,
		       unsigned long *successFlags, int *dirlengths)
{
    RPC2_BoundedBS PBinc;
    char buf[RESCOMM_MAXBSLEN];
    SE_Descriptor	sid;
    ViceStatus status;
    // init parms PB, sid, status block
    {
	PBinc.SeqBody = (RPC2_ByteSeq)buf;
	PBinc.SeqLen = RESCOMM_MAXBSLEN;
	PBinc.MaxSeqLen = RESCOMM_MAXBSLEN;
	
	memset(&sid, 0, sizeof(SE_Descriptor));
	sid.Tag = SMARTFTP;
	sid.Value.SmartFTPD.TransmissionDirection = CLIENTTOSERVER;
	sid.Value.SmartFTPD.ByteQuota = -1;
	sid.Value.SmartFTPD.Tag = FILEINVM;
	sid.Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqLen = logsize;
	sid.Value.SmartFTPD.FileInfo.ByAddr.vmfile.MaxSeqLen = logsize;
	sid.Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqBody = 
	    (RPC2_ByteSeq)AllLogs;

	// Get final version of Vicestatus from all the status blocks 
	{
	    GetResStatus(successFlags, rstatusp, &status);
	    GetMaxVV(&status.VV, VV, -1);
	    AllocStoreId(&status.VV.StoreId);
	}
    }
    ARG_MARSHALL(IN_OUT_MODE, SE_Descriptor, sidvar, sid, VSG_MEMBERS);
    ARG_MARSHALL_BS(IN_OUT_MODE, RPC2_BoundedBS, PBincvar, PBinc, VSG_MEMBERS, RESCOMM_MAXBSLEN);
    ARG_MARSHALL(IN_OUT_MODE, ViceStatus, statusvar, status, VSG_MEMBERS);

    // Ship log to Subordinates & Parse results
    {
	MRPC_MakeMulti(ShipLogs_OP, ShipLogs_PTR, VSG_MEMBERS,
		       mgrp->rrcc.handles, mgrp->rrcc.retcodes, 
		       mgrp->rrcc.MIp, 0, 0, Fid, logsize, 
		       totalentries, statusvar_ptrs, PBincvar_ptrs, 
		       sidvar_bufs);
	mgrp->CheckResult();
	int errorCode = 0;
	if ((errorCode = CheckRetCodes((unsigned long *)mgrp->rrcc.retcodes, 
				      mgrp->rrcc.hosts, successFlags))) {
	    LogMsg(0, SrvDebugLevel, stdout,  
		   "CoordPhase3: Error %d in ShipLogs", errorCode);
	    return(errorCode);
	}

	if (ComparePhase3Status(mgrp, dirlengths, statusvar_bufs, &status)) {
	    LogMsg(0, SrvDebugLevel, stdout,
		   "CoordPhase3: Status blocks do not match\n");
	    return(EINCONS);
	}
    }

    /* Parse inconsistencies */
    {
	for (int i = 0; i < VSG_MEMBERS; i++) 
	    if (mgrp->rrcc.hosts[i] && !mgrp->rrcc.retcodes[i]) 
		BSToDlist(PBincvar_ptrs[i], inclist);
	LogMsg(9, SrvDebugLevel, stdout,  "Phase1 returns 0");
	return(0);
    }
}

static int ComparePhase3Status(res_mgrpent *mgrp, int *dirlengths, 
				ViceStatus *status_bufs, ViceStatus *status) {
    int statusgotalready = 0;
    for (int i = 0; i < VSG_MEMBERS; i++) {
	dirlengths[i] = 0;
	if (mgrp->rrcc.hosts[i] && !mgrp->rrcc.retcodes[i]) {
	    dirlengths[i] = status_bufs[i].Length;
	    if (!statusgotalready) 
		*status = status_bufs[i];
	    else {
		ViceStatus *vs = &status_bufs[i];
		int unequal = ((vs->Author != status->Author) ||
			       (vs->Owner != status->Owner) ||
			       (vs->Mode != status->Mode) ||
			       (vs->vparent != status->vparent) ||
			       (vs->uparent != status->uparent));
		if (unequal) {
		    LogMsg(0, SrvDebugLevel, stdout,  
			   "Phase3: replica status not equal at end of phase 3");
		    return(EINCONS);
		}
	    }
	}
    }
    return(0);
}


static int CoordPhase4(res_mgrpent *mgrp, ViceFid *Fid, 
			unsigned long *succflags, int *dirlengths) {
    ViceVersionVector UpdateSet;
    char *dirbufs[VSG_MEMBERS];
    int Phase4Err = 0;
    SE_Descriptor sid;

    // initialize parameters for call to subordinate VV, sid, 
    {
	
	for (int i = 0; i < VSG_MEMBERS; i++) 
		(&(UpdateSet.Versions.Site0))[i] = 0;
	    
    { /* drop scope for int i below; to avoid identifier clash */
	for (int i = 0; i < VSG_MEMBERS; i++) 
	    if (succflags[i]) {
		// find the index in the update set 
		vrent *vre = VRDB.find(Fid->Volume);
		CODA_ASSERT(vre);
		(&(UpdateSet.Versions.Site0))[vre->index(succflags[i])] = 1;
	    }
    } /* drop scope for int i above; to avoid identifier clash */
	AllocStoreId(&UpdateSet.StoreId);
	
	memset(&sid, 0, sizeof(SE_Descriptor));
	sid.Tag = SMARTFTP;
	sid.Value.SmartFTPD.TransmissionDirection = SERVERTOCLIENT;
	sid.Value.SmartFTPD.Tag = FILEINVM;
	sid.Value.SmartFTPD.ByteQuota = -1;
    }
    ARG_MARSHALL(IN_OUT_MODE, SE_Descriptor, sidvar, sid, VSG_MEMBERS);
    for (int i = 0; i < VSG_MEMBERS; i++)  {
	if (dirlengths[i]) {
	    int maxlen = dirlengths[i] + VAclSize(NULL) + 2 * sizeof(int);
	    dirbufs[i] = (char *)malloc(maxlen);
	    CODA_ASSERT(dirbufs[i]);
	    sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqLen = 0;
	    sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.MaxSeqLen =
		maxlen;
	    sidvar_bufs[i].Value.SmartFTPD.FileInfo.ByAddr.vmfile.SeqBody = 
		(RPC2_ByteSeq)dirbufs[i];
	}
	else dirbufs[i] = NULL;
    }
    // call the subordinate
    {
	LogMsg(9, SrvDebugLevel, stdout,  
	       "CoordPhase4: Doing Phase 4");
	// for now use old res interface 
	MRPC_MakeMulti(InstallVV_OP, InstallVV_PTR, VSG_MEMBERS,
		       mgrp->rrcc.handles, mgrp->rrcc.retcodes,
		       mgrp->rrcc.MIp, 0, 0, Fid, &UpdateSet, sidvar_bufs);
	mgrp->CheckResult();
	
	if ((Phase4Err = CheckRetCodes((unsigned long *)mgrp->rrcc.retcodes, 
				      mgrp->rrcc.hosts, succflags)) )
	    LogMsg(0, SrvDebugLevel, stdout,  
		   "CoordPhase4: Phase4 Error %d", Phase4Err);
	PollAndYield();

    }
    // compare contents of directory replicas 
    {
	if (!Phase4Err  && ((Phase4Err = CompareDirContents(sidvar_bufs, Fid)) == 0))
	    LogMsg(9, SrvDebugLevel, stdout,  
		   "CoordPhase4: Dir Contents equal after phase4");
    }

    // clean up 
    {
	for (int i = 0; i < VSG_MEMBERS; i++) 
	    if (dirbufs[i]) free(dirbufs[i]);
    }
    LogMsg(1, SrvDebugLevel, stdout,  
	   "CoordPhase4: returns %d\n", Phase4Err);
    return(Phase4Err);
}

static int CoordPhase34(res_mgrpent *mgrp, ViceFid *Fid, 
			 dlist *inclist, int *dirlengths, int *noinc) {
    RPC2_BoundedBS PB;
    char buf[RESCOMM_MAXBSLEN];
    int errorCode = 0;
    ViceStatus status;

    if (inclist->count() ==  0) {
	*noinc = 1;
	return(0);
    }
    
    *noinc = 0;
    /* pack list of inconsistencies into a BoundedBS */
    {
	PB.MaxSeqLen = RESCOMM_MAXBSLEN;
	PB.SeqBody = (RPC2_ByteSeq)buf;
	PB.SeqLen = 0;
	DlistToBS(inclist, &PB);
    }

    ViceStoreId logid;
    AllocStoreId(&logid);
    ARG_MARSHALL(OUT_MODE, ViceStatus, statusvar, status, VSG_MEMBERS);
    MRPC_MakeMulti(HandleInc_OP, HandleInc_PTR, VSG_MEMBERS,
		   mgrp->rrcc.handles, mgrp->rrcc.retcodes,
		   mgrp->rrcc.MIp, 0, 0, Fid, &logid, statusvar_ptrs, &PB);
    for (int i = 0; i < VSG_MEMBERS; i++) {
	if (mgrp->rrcc.hosts[i] && !mgrp->rrcc.retcodes[i])
	    dirlengths[i] = statusvar_bufs[i].Length;
	else 
	    dirlengths[i] = 0;
    }
    mgrp->CheckResult();
    unsigned long hosts[VSG_MEMBERS];
    if ((errorCode = CheckRetCodes((unsigned long *)mgrp->rrcc.retcodes, mgrp->rrcc.hosts, hosts))){
	LogMsg(0, SrvDebugLevel, stdout,  
	       "CoordPhase34: Error %d in DirResPhase2", 
	       errorCode);
	return(errorCode);
    }
    return(0);
}

static void UpdateStats(ViceFid *Fid, dirresstats *drstats)
{
    VolumeId vid = Fid->Volume;
    Volume *volptr = 0;
    if (XlateVid(&vid)) {
	if (!GetVolObj(vid, &volptr, VOL_NO_LOCK, 0, 0)) {
	    if (AllowResolution && V_RVMResOn(volptr)) 
		V_VolLog(volptr)->vmrstats->update(drstats);
	}
	else { 
	    LogMsg(0, SrvDebugLevel, stdout,
	       "UpdateStats: couldn't get vol obj 0x%x\n", vid);
	    volptr = 0;
	}
    }
    else 
	LogMsg(0, SrvDebugLevel, stdout,
	       "UpdateStats: couldn't Xlate Fid 0x%x\n", vid);
    if (volptr) 
	PutVolObj(&volptr, VOL_NO_LOCK, 0);

}

