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

/*
 *
 * Implementation of the Venus Repair facility.
 *
 */


#ifdef __cplusplus
extern "C" {
#endif __cplusplus

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include "coda_string.h"
#include <unistd.h>
#include <stdlib.h>
#include <netdb.h>
#include <fcntl.h>

#include <rpc2/rpc2.h>
#include <vice.h>

#ifdef __cplusplus
}
#endif __cplusplus

/* interfaces */
#include <inconsist.h>
#include <copyfile.h>

/* from libal */
#include <prs.h>

/* from librepair */
#include <repio.h>

/* from venus */
#include "comm.h"
#include "fso.h"
#include "local.h"
#include "mariner.h"
#include "venus.private.h"
#include "venusvol.h"
#include "vproc.h"
#include "worker.h"


/*
 *    New Repair Strategy
 *
 *    The basic idea of the new strategy is to represent an
 *    inconsistent object as a (read-only) directory with children
 *    that map to the rw-replicas of the object.  The view a user has
 *    of the inconsistent object depends upon which mode the volume
 *    containing the object is in (with respect to this user?).  In
 *    normal mode, the view is the same that we have been providing
 *    all along, i.e., a dangling, read-only symbolic link whose
 *    contents encode the fid of the object.  In repair mode, the view
 *    is of a read-only subtree headed by the inconsistent object.
 *
 * The internal implications of this change are the following: *
 *          1. Each volume must keep state as to what mode it is in
 *          (per-user?)  (this state will not persist across restarts;
 *          initial mode will be normal)

 *          2. We must cope with "fake" fsobjs, representing both the
 *          * inconsistent object and the mountpoints which map to the
 *          * rw-replicas
 * */


/*
  BEGIN_HTML
  <a name="enablerepair"><strong> mark the volume to be in a repair state </strong></a>
  END_HTML
*/
/* AVSG is returned in rwvols. */
/* LockUids and LockWSs parameters are deprecated! */
int repvol::EnableRepair(vuid_t vuid, VolumeId *RWVols,
                         vuid_t *LockUids, unsigned long *LockWSs)
{
    LOG(100, ("volent::EnableRepair: vol = %x, uid = %d\n", vid, vuid));
    int code = 0, i;

    /* Place volume in "repair mode." */
    if (flags.repair_mode != 1)
	flags.repair_mode = 1;

    /* RWVols, LockUids, and LockWSs are OUT parameters. */
    memset(LockUids, 0, VSG_MEMBERS * sizeof(vuid_t));
    memset(LockWSs, 0, VSG_MEMBERS * sizeof(unsigned long));
    memset(RWVols, 0, VSG_MEMBERS * sizeof(VolumeId));
    for (i = 0; i < VSG_MEMBERS; i++)
        if (vsg[i]) RWVols[i] = vsg[i]->GetVid();

    return(code);
}

/* local-repair modification */
/* Attempt the Repair. */
int repvol::Repair(ViceFid *RepairFid, char *RepairFile, vuid_t vuid,
		    VolumeId *RWVols, int *ReturnCodes)
{
    LOG(100, ("volent::Repair: fid = (%x.%x.%x), file = %s, uid = %d\n",
	       RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique,
	       RepairFile, vuid));
    switch (state) {
    case Hoarding:
	return ConnectedRepair(RepairFid, RepairFile, vuid, RWVols, ReturnCodes);
    case Logging:
	if (1 /* to be replaced by a predicate for not being issued by ASR */)
	    return ConnectedRepair(RepairFid, RepairFile, vuid, RWVols, ReturnCodes);
	else
	    return DisconnectedRepair(RepairFid, RepairFile, vuid, RWVols, ReturnCodes);
    case Emulating:
	return ETIMEDOUT;
    case Resolving:
	return ERETRY;
    default: CHOKE("volent::Repair: bogus volume state %d", state);
    }

    return -1;
}

int repvol::ConnectedRepair(ViceFid *RepairFid, char *RepairFile, vuid_t vuid,
			    VolumeId *RWVols, int *ReturnCodes)
{
    int code = 0;
    int i, j, fd, localFake = 0;
    fsobj *RepairF = NULL;
    struct ViceFid gfid, *rFid = NULL;
    
    memset(ReturnCodes, 0, VSG_MEMBERS * sizeof(int));
    memset(RWVols, 0, VSG_MEMBERS * sizeof(VolumeId));
    for (i = 0; i < VSG_MEMBERS; i++)
        if (vsg[i]) RWVols[i] = vsg[i]->GetVid();

    /* Verify that RepairFid is inconsistent. */
    {
	fsobj *f = NULL;

	code = FSDB->Get(&f, RepairFid, vuid, RC_STATUS);
	if (!(code == 0 && f->IsFakeDir()) && code != EINCONS) {
	    if (code == 0) {
		eprint("Repair: %s (%x.%x.%x) consistent",
		       f->comp, RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique);
		code = EINVAL;	    /* XXX -JJK */
	    }
	    FSDB->Put(&f);
	    return(code);
	}

	/* Check for local repair expansion */
	if ((code == 0) && FAKEROOTFID(*RepairFid)) {
	    fsobj *g = NULL;
	    localFake = 1;
	    ViceFid inc; /* Required for getting Lookup() to traverse mount points */
	    f->Lookup(&g, &inc, "global", vuid, CLU_CASE_SENSITIVE);
	    LOG(100, ("Local-Repair expansion, got (%x.%x.%x) inc (%x.%x.%x)\n", 
		      g->fid.Volume, g->fid.Vnode, g->fid.Unique, inc.Volume, inc.Vnode, inc.Unique));
	    gfid = g->fid;
	    rFid = &gfid;
	    FSDB->Put(&g);
	}
	else /* normal repair, proceed as usual */
	    rFid = RepairFid; 

	FSDB->Put(&f);
    }

    /* Flush all COP2 entries. */
    /* This would NOT be necessary if ViceRepair took a "PiggyCOP2" parameter! */
    {
	code = FlushCOP2();
	if (code != 0) return(code);
    }

    /* Translate RepairFile to cache entry if "REPAIRFILE_BY_FID." */
    {
	ViceFid RepairFileFid;
	if (sscanf(RepairFile, "@%lx.%lx.%lx", &RepairFileFid.Volume,
	   &RepairFileFid.Vnode, &RepairFileFid.Unique) == 3) {
	    code = FSDB->Get(&RepairF, &RepairFileFid, vuid, RC_DATA);
	    if (code != 0) return(code);

	    if (!RepairF->IsFile()) {
		FSDB->Put(&RepairF);
		return(EINVAL);
	    }
	}
    }

    Recov_BeginTrans();
    ViceStoreId sid = GenerateStoreId();
    Recov_EndTrans(MAXFP);

    mgrpent *m = 0;
    int asy_resolve = 0;

    /* Acquire an Mgroup. */
    code = GetMgrp(&m, vuid);
    if (code != 0) goto Exit;

    /* The COP1 call. */
    vv_t UpdateSet;
    {
	/* Compute template VV. */
	vv_t tvv = NullVV;
	vv_t *RepairVVs[VSG_MEMBERS];
	memset((void *)RepairVVs, 0, VSG_MEMBERS * (int)sizeof(vv_t *));
	for (i = 0; i < VSG_MEMBERS; i++)
	    if (vsg[i]) {
		fsobj *f = 0;
		ViceFid rwfid;
		rwfid.Volume = vsg[i]->GetVid();
		rwfid.Vnode = rFid->Vnode;
		rwfid.Unique = rFid->Unique;
		if (FSDB->Get(&f, &rwfid, vuid, RC_STATUS) != 0)
		    continue;
		RepairVVs[i] = &f->stat.VV;	/* XXX */
		FSDB->Put(&f);
	    }
	GetMaxVV(&tvv, RepairVVs, -2);
	
	/* Set-up the status block. */
	ViceStatus status;
	memset((void *)&status, 0, (int)sizeof(ViceStatus));
	if (RepairF != 0) {
	    status.Length = RepairF->stat.Length;
	    status.Date = RepairF->stat.Date;
	    status.Owner = RepairF->stat.Owner;
	    status.Mode = RepairF->stat.Mode;
	    status.LinkCount = RepairF->stat.LinkCount;
	    status.VnodeType = RepairF->stat.VnodeType;
	}
	else {
	    struct stat tstat;
	    if (::stat(RepairFile, &tstat) < 0) {
		code = errno;
		goto Exit;
	    }

	    status.Length = (RPC2_Unsigned)tstat.st_size;
	    status.Date = (Date_t)tstat.st_mtime;
	    RPC2_Integer se_uid = (short)tstat.st_uid;		/* sign-extend uid! */
	    status.Owner = (UserId)se_uid;
	    status.Mode = (RPC2_Unsigned)tstat.st_mode & 0777;
	    status.LinkCount = (RPC2_Integer)tstat.st_nlink;
	    switch(tstat.st_mode & S_IFMT) {
		case S_IFREG:
		    status.VnodeType = File;
		    break;

		case S_IFDIR:
		    status.VnodeType = Directory;
		    break;
#ifdef S_IFLNK
		case S_IFLNK:
		    status.VnodeType = SymbolicLink;
		    break;
#endif
		default:
		    code = EINVAL;
		    goto Exit;
	    }
	}
	status.DataVersion = (FileVersion)0; /* Anything but -1? -JJK */
	status.VV = tvv;

	/* A little debugging help. */
	if (LogLevel >= 1) {
	    fprintf(logFile, "Repairing %s:\n", FID_(rFid));
	    fprintf(logFile, "\tIV = %d, VT = %d, LC = %d, LE = %ld, DV = %d, DA = %d\n",
		    status.InterfaceVersion, status.VnodeType, status.LinkCount,
		    status.Length, status.DataVersion, status.Date);
	    fprintf(logFile, "\tAU = %d, OW = %d, CB = %d, MA = %d, AA = %d, MO = %d\n",
		    status.Author, status.Owner, status.CallBack,
		    status.MyAccess, status.AnyAccess, status.Mode);
	    vv_t *tvvs[VSG_MEMBERS];
	    memset((void *)tvvs, 0, VSG_MEMBERS * (int)sizeof(vv_t *));
	    tvvs[0] = &status.VV;
	    VVPrint(logFile, tvvs);
	    fflush(logFile);
	}

	/* Set up the SE descriptor. */
	SE_Descriptor sed;
	memset(&sed, 0, sizeof(SE_Descriptor));
	sed.Tag = SMARTFTP;
	struct SFTP_Descriptor *sei; sei = &sed.Value.SmartFTPD;
	sei->SeekOffset = 0;
	sei->hashmark = 0;
	sei->TransmissionDirection = CLIENTTOSERVER;

        /* and open a safe fd to the containerfile */
        if (RepairF) fd = RepairF->data.file->Open(O_RDONLY);
        else         fd = open(RepairFile, O_RDONLY, (int)V_MODE);

        sei->Tag = FILEBYFD;
        sei->FileInfo.ByFD.fd = fd;

	/* Make multiple copies of the IN/OUT and OUT parameters. */
	ARG_MARSHALL(IN_OUT_MODE, ViceStatus, statusvar, status, VSG_MEMBERS);
	ARG_MARSHALL(IN_OUT_MODE, SE_Descriptor, sedvar, sed, VSG_MEMBERS);

/*
  BEGIN_HTML
  <a name="vice"><strong> call the Coda server to perform the actual
  repair actions </strong> </a>
  END_HTML
*/

	/* Make the RPC call. */
	MarinerLog("store::Repair (%x.%x.%x)\n",
		   rFid->Volume, rFid->Vnode, rFid->Unique);
	MULTI_START_MESSAGE(ViceRepair_OP);
	code = MRPC_MakeMulti(ViceRepair_OP, ViceRepair_PTR,
			      VSG_MEMBERS, m->rocc.handles,
			      m->rocc.retcodes, m->rocc.MIp, 0, 0,
			      rFid, statusvar_ptrs, &sid, sedvar_bufs);
	MULTI_END_MESSAGE(ViceRepair_OP);
	MarinerLog("store::repair done\n");

	/* Collate responses from individual servers and decide what to do next. */
	/* Valid return codes are: {0, EINTR, ETIMEDOUT, ESYNRESOLVE, ERETRY}. */
	code = Collate_COP1(m, code, &UpdateSet);
	if (code == EASYRESOLVE) { asy_resolve = 1; code = 0; }
	MULTI_RECORD_STATS(ViceRepair_OP);
	if (code != 0 && code != ESYNRESOLVE) goto Exit;

	/* Collate ReturnCodes. */
	struct in_addr VSGHosts[VSG_MEMBERS];
	GetHosts(VSGHosts);
	int HostCount = 0;	/* for sanity check */
	for (i = 0; i < VSG_MEMBERS; i++)
	    if (vsg[i]) {
		for (j = 0; j < VSG_MEMBERS; j++)
		    if (VSGHosts[i].s_addr == m->rocc.hosts[j].s_addr) {
			ReturnCodes[i] = (m->rocc.retcodes[j] >= 0)
			  ? m->rocc.retcodes[j]
			  : ETIMEDOUT;
			HostCount++;
			break;
		    }
		if (j == VSG_MEMBERS)
		    ReturnCodes[i] = ETIMEDOUT;
	    }
	if (HostCount != m->rocc.HowMany)
	    CHOKE("volent::Repair: collate failed");
	if (code != 0) goto Exit;
    }

    /* For directory conflicts only! (for file conflicts, there is no fixfile)
     * Prune CML entries if localhost is specified in fixfile */
    if (ISDIR(*RepairFid)) 
    {
	time_t modtime;
	int hcount, rc, repLC, mvLC;
	struct listhdr *hlist, *l = NULL;
	struct repair *rep_ent;
	struct ViceFid entryFid, mvFid, mvPFid;

	/* parse input file and obtain internal rep  */
	rc = ::lseek(fd, 0, SEEK_SET);
	if (rc != 0) { code = errno; goto Exit; }
	if (repair_getdfile(fd, &hcount, &hlist) < 0) {
	    code = errno; /* XXXX - Could use a more meaningful return code here */
	    goto Exit;
	}

	for (i = 0; i < hcount; i++)
	    if (hlist[i].replicaId == LOCAL_VID) 
		{ l = &(hlist[i]); break; }
	if (l != NULL) {
	    /* found localhost in fixfile */
	    rep_ent = l->repairList;

	    rc = time(&modtime);
	    if (rc == (time_t)-1) {
	      code = errno;
	      goto Exit;
	    }

	    for (i = 0; i < l->repairCount; i++) {

		if   ( (rep_ent[i].opcode != REPAIR_SETMODE)
		    && (rep_ent[i].opcode != REPAIR_SETOWNER)
		    && (rep_ent[i].opcode != REPAIR_SETMTIME))
		/* Lookup fid of the current CML entry name */
		{
		    fsobj *f = NULL, *e = NULL;

		    code = FSDB->Get(&f, rFid, vuid, RC_STATUS);
		    if (code != 0) {
			LOG(15, ("Repair: FSDB->Get(%x.%x.%x) error", 
				 rFid->Volume, rFid->Vnode, rFid->Unique));
			goto Exit;
		    }
		    
		    code = f->Lookup(&e, NULL, rep_ent[i].name, vuid, CLU_CASE_SENSITIVE);
		    if (code != 0) {
			LOG(15, ("Repair: (%x.%x.%x)->Lookup(%s) error", 
				 rFid->Volume, rFid->Vnode, rFid->Unique, rep_ent[i].name));
			FSDB->Put(&f);
			goto Exit;
		    }
		    entryFid = e->fid;
		    repLC = e->stat.LinkCount;
		    FSDB->Put(&e);

		    if (rep_ent[i].opcode == REPAIR_RENAME) {
			code = f->Lookup(&e, NULL, rep_ent[i].newname, vuid, CLU_CASE_SENSITIVE);
			if (code != 0) {
			    LOG(15, ("Repair: (%x.%x.%x)->Lookup(%s) error", 
				     rFid->Volume, rFid->Vnode, rFid->Unique, rep_ent[i].newname));
			    FSDB->Put(&f);
			    goto Exit;
			}
		    }
		    mvFid = e->fid;
		    mvPFid = e->pfid;
		    mvLC = e->stat.LinkCount;
		    FSDB->Put(&e);

		    FSDB->Put(&f);
		}

		switch (rep_ent[i].opcode) {

		case REPAIR_REMOVEFSL: /* Remove file or (hard) link */
		    Recov_BeginTrans();
		    code = LogRemove(modtime, vuid, rFid, rep_ent[i].name,
				     &entryFid, repLC, UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;
		case REPAIR_REMOVED:   /* Remove dir */
		    Recov_BeginTrans();
		    code = LogRmdir(modtime, vuid, rFid, rep_ent[i].name,
				    &entryFid, UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;
		case REPAIR_SETMODE:
		    Recov_BeginTrans();
		    code = LogChmod(modtime, vuid, rFid, 
				    (RPC2_Unsigned)rep_ent[i].parms[0], UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;
		case REPAIR_SETOWNER: /* Have to be a sys administrator for this */
		    Recov_BeginTrans();
		    code = LogChown(modtime, vuid, rFid, 
				    (UserId)rep_ent[i].parms[0], UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;
		case REPAIR_SETMTIME:
		    Recov_BeginTrans();
		    code = LogUtimes(modtime, vuid, rFid, 
				     (Date_t)rep_ent[i].parms[0], UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;
		case REPAIR_RENAME:
		    Recov_BeginTrans();
		    code = LogRename(modtime, vuid, rFid, rep_ent[i].name, &mvPFid, 
				     rep_ent[i].name, &entryFid, &mvFid, mvLC, UNSET_TID);
		    Recov_EndTrans(CMFP);
		    break;

		/* These should never occur in the fixfile for the local replica */
		case REPAIR_CREATEF: 	 /* Create file */
		case REPAIR_CREATED:	 /* Create directory */
		case REPAIR_CREATES:	 /* Create sym link */
		case REPAIR_CREATEL:	 /* Create (hard) link */
		case REPAIR_SETACL:	 /* Set rights */
		case REPAIR_SETNACL:	 /* Set negative rights */
		    LOG(1, ("Unexpected local repair command code (%d)", rep_ent[i].opcode));
		    code = EINVAL; /* XXXX - Could use a more meaningful return code here */
		    break;

		case REPAIR_REPLICA: 
		    LOG(1, ("Unexpected REPAIR_REPLICA -- truncated fixfile?"));
		    code = EINVAL; /* XXXX - Could use a more meaningful return code here */
		    break;

		default:
		    LOG(1, ("Unknown local repair command code (%d)", rep_ent[i].opcode));
		    code = EINVAL; /* XXXX - Could use a more meaningful return code here */
		    break;
		}
		
		if (code != 0) goto Exit;
	    }

	}
    }

    /* Send the COP2 message.  Don't Piggy!  */
    (void)COP2(m, &sid, &UpdateSet);

Exit:
    if (RepairF) RepairF->data.file->Close(fd);
    else         close(fd);

    PutMgrp(&m);
    FSDB->Put(&RepairF);

    if ((code == 0) && !localFake) {
	/* Purge the fake object. */
	fsobj *f = FSDB->Find(RepairFid);
	if (f != 0) {
	    f->Lock(WR);
	    Recov_BeginTrans();
	        f->Kill();
	    Recov_EndTrans(MAXFP);
	    FSDB->Put(&f);

	    /* Ought to flush its descendents too! */
	}

	/* Invoke an asynchronous resolve for directories. */
	if (ISDIR(*RepairFid)) {
	    ResSubmit(0, RepairFid);
	}
    }
    if (code ==	ESYNRESOLVE) code = EMULTRSLTS;	/* "multiple results" */

    return(code);
}

/*
  BEGIN_HTML
  <a name="disablerepair"><strong> unset the volume from the repair state </strong></a>
  END_HTML
*/
int repvol::DisconnectedRepair(ViceFid *RepairFid, char *RepairFile,
			       vuid_t vuid, VolumeId *RWVols, int *ReturnCodes)
{
    int code = 0, i;
    fsobj *RepairF = 0;
    ViceStatus status;
    vproc *vp = VprocSelf();
    CODA_ASSERT(vp);
    
    LOG(10, ("volent::DisConnectedRepair: fid = (%x.%x.%x), file = %s, uid = %d\n",
	    RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique,
	    RepairFile, vuid));

    ViceFid tpfid;
    tpfid.Volume = RepairFid->Volume;
    memset(ReturnCodes, 0, VSG_MEMBERS * sizeof(int));
    memset(RWVols, 0, VSG_MEMBERS * sizeof(VolumeId));
    for (i = 0; i < VSG_MEMBERS; i++)
        if (vsg[i]) RWVols[i] = vsg[i]->GetVid();

    /* Verify that RepairFid is a file fid */
    /* can't repair directories while disconnected */
    if (ISDIR(*RepairFid)) {
	eprint("DisconnectedRepair: (%x.%x.%x) is a dir - cannot repair\n",
	       RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique);
	return(EINVAL);		/* XXX - PK*/
    }

    /* Verify that RepairFid is inconsistent. */
    {
	fsobj *f = 0;
	code = FSDB->Get(&f, RepairFid, vuid, RC_STATUS);
	if (!(code == 0 && f->IsFakeDir()) && code != EINCONS) {
	    if (code == 0) {
		eprint("DisconnectedRepair: %s (%x.%x.%x) consistent",
		       f->comp, RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique);
		code = EINVAL;	    /* XXX */
	    }
	    FSDB->Put(&f);
	    return(code);
	}
	/* save the fid of the parent of the inconsistent object */
	tpfid.Vnode = f->pfid.Vnode;
	tpfid.Unique = f->pfid.Unique;
	FSDB->Put(&f);
    }
    /* check rights - can user write the file to be repaired */
    {
	LOG(100, ("DisconnectedRepair: Going to check access control (%x.%x.%x)\n",
		  tpfid.Volume, tpfid.Vnode, tpfid.Unique));
	if (!tpfid.Vnode) {
	    LOG(10, ("DisconnectedRepair: Parent fid is NULL - cannot check access control\n"));
	    return(EACCES);
	}
	/* check the parent's rights for write permission*/
	fsobj *parentf = 0;
	code = FSDB->Get(&parentf, &tpfid, vuid, RC_STATUS);
	if (code == 0) {
	    code = parentf->Access(PRSFS_WRITE, W_OK, CRTORUID(vp->u.u_cred));
	    if (code) {
		LOG(10, ("DisconnectedRepair: Access disallowed (%x.%x.%x)\n",
			 tpfid.Volume, tpfid.Vnode, tpfid.Unique));
		FSDB->Put(&parentf);
		return(code);
	    }
	}
	else {
	    LOG(100, ("DisconnectedRepair: Couldn't get parent (%x.%x.%x)\n",
		      tpfid.Volume, tpfid.Vnode, tpfid.Unique));
	    if (parentf) FSDB->Put(&parentf);
	    return(code);
	}
	FSDB->Put(&parentf);
    }
    LOG(100, ("DisconnectedRepair: going to check repair file %s\n", RepairFile));
    /* Translate RepairFile to cache entry if "REPAIRFILE_BY_FID." */
    {
	ViceFid RepairFileFid;
	if (sscanf(RepairFile, "@%lx.%lx.%lx", &RepairFileFid.Volume,
		   &RepairFileFid.Vnode, &RepairFileFid.Unique) == 3) {
	    code = FSDB->Get(&RepairF, &RepairFileFid, vuid, RC_DATA);
	    if (code != 0) return(code);

	    if (!RepairF->IsFile()) {
		eprint("DisconnectedRepair: Repair file (%x.%x.%x) isn't a file\n",
		       RepairFileFid.Volume, RepairFileFid.Vnode, RepairFileFid.Unique);
		FSDB->Put(&RepairF);
		return(EINVAL);
	    }
	}
    }

    /* prepare to fake the call */
    {
	/* Compute template VV. */
	vv_t tvv = NullVV;
	vv_t *RepairVVs[VSG_MEMBERS];
        memset(RepairVVs, 0, VSG_MEMBERS * sizeof(vv_t *));

	for (int i = 0; i < VSG_MEMBERS; i++)
	    if (vsg[i]) {
		fsobj *f = 0;
		ViceFid rwfid;
		rwfid.Volume = vsg[i]->GetVid();
		rwfid.Vnode = RepairFid->Vnode;
		rwfid.Unique = RepairFid->Unique;
		if (FSDB->Get(&f, &rwfid, vuid, RC_STATUS) != 0)
		    continue;
		RepairVVs[i] = &f->stat.VV;	/* XXX */
		if (tpfid.Vnode && f->pfid.Vnode) {
		    CODA_ASSERT(tpfid.Vnode == f->pfid.Vnode);
		    CODA_ASSERT(tpfid.Unique == f->pfid.Unique);
		}
		FSDB->Put(&f);
	    }
	GetMaxVV(&tvv, RepairVVs, -2);
	/* don't generate a new storeid yet - LogRepair will do that */

	/* set up status block */
	memset((void *)&status, 0, (int)sizeof(ViceStatus));
	if (RepairF != 0) {
	    status.Length = RepairF->stat.Length;
	    status.Date = RepairF->stat.Date;
	    status.Owner = RepairF->stat.Owner;
	    status.Mode = RepairF->stat.Mode;
	    status.LinkCount = RepairF->stat.LinkCount;
	    status.VnodeType = RepairF->stat.VnodeType;
	}
	else {
	    struct stat tstat;
	    if (::stat(RepairFile, &tstat) < 0) {
		code = errno;
		goto Exit;
	    }

	    status.Length = (RPC2_Unsigned)tstat.st_size;
	    status.Date = (Date_t)tstat.st_mtime;
	    RPC2_Integer se_uid = (short)tstat.st_uid;		/* sign-extend uid! */
	    status.Owner = (UserId)se_uid;
	    status.Mode = (RPC2_Unsigned)tstat.st_mode & 0777;
	    status.LinkCount = (RPC2_Integer)tstat.st_nlink;
	    if (tstat.st_mode & S_IFMT != S_IFREG) {
		code = EINVAL;
		goto Exit;
	    }
	}
	status.DataVersion = (FileVersion)1;	  /* Anything but -1? -JJK */
	status.VV = tvv;

    }

    /* fake the call */
    {
	/* first kill the fake directory if it exists */
	fsobj *f = FSDB->Find(RepairFid);
	if (f != 0) {
	    LOG(0, ("DisconnectedRepair: Going to kill %x.%x.%x \n",
		    RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique));
	    f->Lock(WR);
	    Recov_BeginTrans();
	       f->Kill();
	     Recov_EndTrans(MAXFP);

	    if (f->refcnt > 1) {
		/* Put isn't going to release the object so can't call create
		 * Instead of failing, put an informative message on console
		 * and ask user to retry */
		f->ClearRcRights();
		FSDB->Put(&f);
		LOG(0, ("DisconnectedRepair: (%x.%x.%x) has active references - cannot repair\n",
			RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique));
		code = ERETRY;
		goto Exit;
	    }
	    FSDB->Put(&f);
	    /* Ought to flush its descendents too? XXX -PK */
	}
	/* attempt the create now */
	LOG(100, ("DisconnectedRepair: Going to create %x.%x.%x\n",
		  RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique)); 
	/* need to get the priority from the vproc pointer */
	f = FSDB->Create(RepairFid, WR, vp->u.u_priority, (char *)NULL);
			/* don't know the component name */
	if (f == 0) {
	    UpdateCacheStats(&FSDB->FileAttrStats, NOSPACE, NBLOCKS(sizeof(fsobj)));
	    code = ENOSPC;
	    goto Exit; 
	}

	Date_t Mtime = Vtime();

	LOG(100, ("DisconnectedRepair: Going to call LocalRepair(%x.%x.%x)\n",
		  RepairFid->Volume, RepairFid->Vnode, RepairFid->Unique));
	Recov_BeginTrans();
	   code = LogRepair(Mtime, vuid, RepairFid, status.Length, status.Date,
			    status.Owner, status.Mode);
	   /*
	    * LogRepair puts a ViceRepair_OP record into the CML and it
	    * will be reintegrated to the servers at the end of the ASR
	    * execution. During the reintegration process on the server,
	    * a ViceRepair() call will be made, therefore the inconsistent
	    * file object on servers will get the new data and its inconsistent
	    * bit will be cleared. Because the VV of the file object is
	    * incremented as a result, the next FSDB::get() tries to get 
	    * it, it will fetch the new clean server version and throw
	    * away the local fakeified object.
	    */
	   if (code == 0) 
	       code = LocalRepair(f, &status,
				  RepairF ? RepairF->data.file->Name() : RepairFile,
				  &tpfid);
	  Recov_EndTrans(DMFP);
	  
	if (code != 0) {
	    /* kill the object? - XXX */
	    Recov_BeginTrans();
		   f->Kill();
	    Recov_EndTrans(MAXFP);
	}
	FSDB->Put(&f);
    }	    

  Exit:
    if (RepairF) FSDB->Put(&RepairF);

    LOG(0, ("DisconnectedRepair: returns %u\n", code));
    return(code);
}

/* MUST be called from within a transaction */
int repvol::LocalRepair(fsobj *f, ViceStatus *status, char *fname, ViceFid *pfid) {
    LOG(100, ("LocalRepair: %x.%x.%x local file %s \n",
	      f->fid.Volume, f->fid.Vnode, f->fid.Unique, fname));
    RVMLIB_REC_OBJECT(*f);

    f->stat.VnodeType = status->VnodeType;
    f->stat.LinkCount = status->LinkCount;	/* XXX - this could be wrong */
    f->stat.Length = status->Length;
    f->stat.DataVersion = status->DataVersion;
    f->stat.VV = status->VV;
    f->stat.Date = status->Date;
    f->stat.Owner = status->Owner;
    f->stat.Author = status->Author;
    f->stat.Mode = status->Mode;

    f->Matriculate();

    /* for now the parent pointers are just set to NULL */
    if (pfid) f->pfid = *pfid;
    f->pfso = NULL;	

    /* now store the new contents of the file */
    {
        RVMLIB_REC_OBJECT(f->cf);
	f->data.file = &f->cf;
        f->data.file->Create();

	int srcfd = open(fname, O_RDONLY | O_BINARY, V_MODE);
	CODA_ASSERT(srcfd);
	LOG(100, ("LocalRepair: Going to open %s\n", f->data.file->Name()));
        int tgtfd = open(f->data.file->Name(), O_WRONLY|O_TRUNC|O_BINARY,
                         V_MODE);
	CODA_ASSERT(tgtfd>0);
	int rc;
	struct stat stbuf;

	rc = copyfile(srcfd, tgtfd);
	CODA_ASSERT(rc == 0);

	rc = fstat(tgtfd, &stbuf);
	CODA_ASSERT(rc == 0);

	rc = close(srcfd);
	CODA_ASSERT(rc == 0);

	rc = close(tgtfd);
	CODA_ASSERT(rc == 0);

	if (stbuf.st_size != status->Length) {
	    LOG(10, ("LocalRepair: Length mismatch - actual stored %u bytes, expected %u bytes\n",
		     stbuf.st_size, status->Length));
	    return(EIO); 	/* XXX - what else can we return? */
	}
	f->data.file->SetLength((unsigned int) stbuf.st_size);
    }

    /* set the flags of the object before returning */
    f->flags.dirty = 1;

    return(0);
}    

int repvol::DisableRepair(vuid_t vuid)
{
    LOG(100, ("volent::DisableRepair: vol = %x, uid = %d\n", vid, vuid));

    if (IsUnderRepair(vuid))
	flags.repair_mode = 0;
    else
	LOG(0, ("volent::DisableRepair: %x not under repair", vid));

    return(0);
}


/* If (vuid == ALL_UIDS) the enquiry is taken to be "does anyone on the WS have the volume under repair". */
int repvol::IsUnderRepair(vuid_t vuid)
{
    LOG(100, ("repvol::IsUnderRepair: vol = %x, vuid = %d\n", vid, vuid));

    return (flags.repair_mode);
}

/* Enable ASR invocation for this volume */
int repvol::EnableASR(vuid_t vuid)
{
    LOG(100, ("repvol::EnableASR: vol = %x, uid = %d\n", vid, vuid));

    /* Place volume in "repair mode." */
    if (flags.allow_asrinvocation != 1) {
	flags.allow_asrinvocation = 1;
	(void)k_Purge();  /* we should be able to do this on a volume/user basis! */
    }
    
    return(0);
}

int repvol::DisableASR(vuid_t vuid)
{
    LOG(100, ("repvol::DisableASR: vol = %x, uid = %d\n", vid, vuid));

    if (!IsASRAllowed()) {
	LOG(100, ("volent::DisableASr: ASR for %x already disabled", vid));
    }
    else {
	LOG(0, ("volent::DisableASR: disabling asr for %x\n", vid));
	flags.allow_asrinvocation = 0; 
	(void)k_Purge();     /* we should be able to do this on a volume/user basis! */
    }

    return(0);
}

void repvol::lock_asr()
{
    CODA_ASSERT(flags.asr_running == 0);
    flags.asr_running = 1;
}

void repvol::unlock_asr() {
    CODA_ASSERT(flags.asr_running == 1);
    flags.asr_running = 0;
}

void repvol::asr_id(int id) {
    CODA_ASSERT(flags.asr_running == 1);
    lc_asr = id;
}

