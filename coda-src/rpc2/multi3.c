#ifndef _BLURB_
#define _BLURB_
/*

            Coda: an Experimental Distributed File System
                             Release 4.0

          Copyright (c) 1987-1996 Carnegie Mellon University
                         All Rights Reserved

Permission  to  use, copy, modify and distribute this software and its
documentation is hereby granted,  provided  that  both  the  copyright
notice  and  this  permission  notice  appear  in  all  copies  of the
software, derivative works or  modified  versions,  and  any  portions
thereof, and that both notices appear in supporting documentation, and
that credit is given to Carnegie Mellon University  in  all  documents
and publicity pertaining to direct or indirect use of this code or its
derivatives.

CODA IS AN EXPERIMENTAL SOFTWARE SYSTEM AND IS  KNOWN  TO  HAVE  BUGS,
SOME  OF  WHICH MAY HAVE SERIOUS CONSEQUENCES.  CARNEGIE MELLON ALLOWS
FREE USE OF THIS SOFTWARE IN ITS "AS IS" CONDITION.   CARNEGIE  MELLON
DISCLAIMS  ANY  LIABILITY  OF  ANY  KIND  FOR  ANY  DAMAGES WHATSOEVER
RESULTING DIRECTLY OR INDIRECTLY FROM THE USE OF THIS SOFTWARE  OR  OF
ANY DERIVATIVE WORK.

Carnegie  Mellon  encourages  users  of  this  software  to return any
improvements or extensions that  they  make,  and  to  grant  Carnegie
Mellon the rights to redistribute these changes without encumbrance.
*/

static char *rcsid = "$Header: /coda/coda.cs.cmu.edu/project/coda/cvs/coda/coda-src/rpc2/Attic/multi3.c,v 4.2 1998/04/14 21:07:01 braam Exp $";
#endif /*_BLURB_*/


/*

                         IBM COPYRIGHT NOTICE

                          Copyright (C) 1986
             International Business Machines Corporation
                         All Rights Reserved

This  file  contains  some  code identical to or derived from the 1986
version of the Andrew File System ("AFS"), which is owned by  the  IBM
Corporation.    This  code is provded "AS IS" and IBM does not warrant
that it is free of infringement of  any  intellectual  rights  of  any
third  party.    IBM  disclaims  liability of any kind for any damages
whatsoever resulting directly or indirectly from use of this  software
or  of  any  derivative work.  Carnegie Mellon University has obtained
permission to distribute this code, which is based on Version 2 of AFS
and  does  not  contain the features and enhancements that are part of
Version 3 of AFS.  Version 3 of  AFS  is  commercially  available  and
supported by Transarc Corporation, Pittsburgh, PA.

*/


/*
	Multicast Utility Routines for MultiRPC
*/


#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <netdb.h>
#include "timer.h"
#include "preempt.h"
#include "lwp.h"
#include "rpc2.h"
#include "rpc2.private.h"
#include "se.h"
#include "trace.h"
#include "cbuf.h"


void rpc2_RemoveFromMgrp();
void rpc2_DeleteMgrp();

extern void FreeHeld();  /* sl.c */
extern void SavePacketForRetry();  /* rpc2a.c */

/* this definition was taken from sl.c */
#define BOGUS(p)	/* bogus packet; throw it away */\
    say(9, RPC2_DebugLevel, "Bogus packet, discarding\n");\
    rpc2_MRecvd.Bogus++;\
    RPC2_FreeBuffer(&p);

#define	MGRPHASHLENGTH	256		    /* must be power of 2 */
PRIVATE	struct bucket
    {
    struct MEntry   *chain;
    int		    count;
    } MgrpHashTable[MGRPHASHLENGTH];
PRIVATE	RPC2_Handle	LastMgrpidAllocated;
#define	LISTENERALLOCSIZE   8		    /* malloc/realloc granularity */


/* Initialize the multicast group data structures; all this requires
   is zeroing the hash table. */
void rpc2_InitMgrp()
{
	say(0, RPC2_DebugLevel, "In rpc2_InitMgrp()\n");
	
	bzero(MgrpHashTable, sizeof(MgrpHashTable));
	LastMgrpidAllocated = 0;
}


/* Implements simple hash algorithm. */
PRIVATE struct bucket *rpc2_GetBucket(host, portal, mgrpid)
    RPC2_HostIdent	*host;
    RPC2_PortalIdent	*portal;
    RPC2_Handle		mgrpid;
    {
    register int    index;

    index = (host->Value.InetAddress ^ mgrpid) & (MGRPHASHLENGTH - 1);
    say(9, RPC2_DebugLevel, "bucket = %d, count = %d\n", index, MgrpHashTable[index].count);
    return(&MgrpHashTable[index]);
    }


/* Clients call this routine with: <rpc2_LocalHost, rpc2_LocalPortal, NULL>
   Servers call this routine with: <ClientHost, ClientPortal, mgrpid>
*/
struct MEntry *rpc2_AllocMgrp(host, portal, handle)
    RPC2_HostIdent	*host;
    RPC2_PortalIdent	*portal;
    RPC2_Handle		handle;
{
    register struct MEntry  *me;
    register RPC2_Handle    mgrpid;
    register struct bucket  *bucket;

    assert((host->Tag == RPC2_HOSTBYINETADDR && portal->Tag == RPC2_PORTALBYINETNUMBER) || (host->Tag == RPC2_DUMMYHOST && portal->Tag == RPC2_DUMMYPORTAL));
    rpc2_AllocMgrps++;
    if (rpc2_MgrpFreeCount == 0)
	rpc2_Replenish(&rpc2_MgrpFreeList, &rpc2_MgrpFreeCount, sizeof(struct MEntry), &rpc2_MgrpCreationCount, OBJ_MENTRY);

    /* Clients allocate mgrpids sequentially; because they are 32-bits
       long we assume they are never reused.  Servers are told the
       mgrpid in the rpc2_initmulticast message.  Thus, the unique
       identifier is <client_host, client_portal, mgrpid, role>. */
    mgrpid = (handle == 0) ? ++LastMgrpidAllocated : handle;
    say(9, RPC2_DebugLevel, "Allocating Mgrp: host = 0x%lx\tportal = 0x%x\tmgrpid = 0x%lx\t", host->Value.InetAddress, portal->Value.InetPortNumber, mgrpid);
    bucket = rpc2_GetBucket(host, portal, mgrpid);

    me = (struct MEntry *)rpc2_MoveEntry(&rpc2_MgrpFreeList, &bucket->chain, NULL, &rpc2_MgrpFreeCount, &bucket->count);
    assert(me->MagicNumber == OBJ_MENTRY);
    me->ClientHost = *host;	    /* structure assignment */
    me->ClientPortal = *portal;	    /* structure assignment */
    me->MgroupID = mgrpid;
    me->Flags = 0;
    me->SEProcs = NULL;
    me->SideEffectPtr = NULL;
    return(me);
}


void rpc2_FreeMgrp(me)
    register struct MEntry  *me;
{
    register struct CEntry  *ce;
    register int	    i;
    register struct bucket  *bucket;

    assert(me != NULL && !TestRole(me, FREE));
    if (TestState(me, CLIENT, ~(C_THINK|C_HARDERROR)) ||
        TestState(me, SERVER, ~(S_AWAITREQUEST|S_REQINQUEUE|S_PROCESS|S_HARDERROR)))
	say(9, RPC2_DebugLevel, "WARNING: freeing busy mgroup\n");

    if (TestRole(me, CLIENT))
	{
	assert(me->listeners != NULL && me->maxlisteners >= me->howmanylisteners);
	for (i = 0; i < me->howmanylisteners; i++)
	    {
	    ce = me->listeners[i];
	    assert(ce->Mgrp == me);		    /* More sanity checks??? */
	    ce->Mgrp = (struct MEntry *)NULL;
	    }
	free(me->listeners);
	}
    else    /* Role == SERVER */
	{
	    ce = me->conn;
	    assert(ce->Mgrp == me);		    /* More sanity checks??? */
	    ce->Mgrp = (struct MEntry *)NULL;
	}

    rpc2_FreeMgrps++;
    SetRole(me, FREE);
    say(9, RPC2_DebugLevel, "Freeing Mgrp: ClientHost = 0x%lx\tClientPortal = 0x%x\tMgroupID = 0x%lx\t", me->ClientHost.Value.InetAddress, me->ClientPortal.Value.InetPortNumber, me->MgroupID);
    bucket = rpc2_GetBucket(&me->ClientHost, &me->ClientPortal, me->MgroupID);
    rpc2_MoveEntry(&bucket->chain, &rpc2_MgrpFreeList, me, &bucket->count, &rpc2_MgrpFreeCount);
}


struct MEntry *rpc2_GetMgrp(host, portal, handle, role)
    RPC2_HostIdent	*host;
    RPC2_PortalIdent	*portal;
    RPC2_Handle		handle;
    long		role;
    {
    register struct MEntry  *me;
    register struct bucket  *bucket;
    register int	    i;

    assert((host->Tag == RPC2_HOSTBYINETADDR && portal->Tag == RPC2_PORTALBYINETNUMBER) || (host->Tag == RPC2_DUMMYHOST && portal->Tag == RPC2_DUMMYPORTAL));
    bucket = rpc2_GetBucket(host, portal, handle);

    for (me = bucket->chain, i = 0; i < bucket->count; me = me->Next, i++) {
	say(9, RPC2_DebugLevel, "GetMgrp: 0x%lx.%u.%ld\n", me->ClientHost.Value.InetAddress, 
			   (unsigned) me->ClientPortal.Value.InetPortNumber, me->MgroupID);
	if (me->ClientHost.Value.InetAddress == host->Value.InetAddress &&
	    me->ClientPortal.Value.InetPortNumber == portal->Value.InetPortNumber &&
	    me->MgroupID == handle && TestRole(me, role))
	    {
	    assert(me->MagicNumber == OBJ_MENTRY);
	    return(me);
	    }
    }

    return((struct MEntry *)NULL);
    }


/* Client-side operation only. */
long RPC2_CreateMgrp(OUT MgroupHandle, IN MulticastHost, IN MulticastPortal, IN  Subsys,
	     SecurityLevel, SessionKey, EncryptionType, SideEffectType)
    RPC2_Handle		*MgroupHandle;
    RPC2_McastIdent	*MulticastHost;
    RPC2_PortalIdent	*MulticastPortal;
    RPC2_SubsysIdent	*Subsys;
    RPC2_Integer	SecurityLevel;
    RPC2_EncryptionKey	SessionKey;
    RPC2_Integer	EncryptionType;
    long		SideEffectType;
    {
    register struct MEntry	*me;
    register struct servent	*sentry;
    long			secode;

    rpc2_Enter();
    say(0, RPC2_DebugLevel, "In RPC2_CreateMgrp()\n");

#ifdef RPC2DEBUG
    TR_CREATEMGRP();
#endif RPC2DEBUG

    /* Validate the security parameters. */
    switch ((int) SecurityLevel)
	{
	case RPC2_OPENKIMONO:
		break;

	case RPC2_AUTHONLY:
	case RPC2_HEADERSONLY:
	case RPC2_SECURE:
		if ((EncryptionType & RPC2_ENCRYPTIONTYPES) == 0) rpc2_Quit(RPC2_FAIL); 	/* unknown encryption type */
		if (MORETHANONEBITSET(EncryptionType)) rpc2_Quit(RPC2_FAIL);	/* tell me just one */
		break;
		
	default:    rpc2_Quit(RPC2_FAIL);	/* bogus security level */
	}

    /* Get an mgrp entry and initialize it. */
    me = rpc2_AllocMgrp(&rpc2_LocalHost, &rpc2_LocalPortal, NULL);
    assert(me != NULL);
    *MgroupHandle = me->MgroupID;

    SetRole(me, CLIENT);
    SetState(me, C_THINK);
    me->NextSeqNumber = 0;

    me->SecurityLevel = SecurityLevel;
    if (me->SecurityLevel == RPC2_OPENKIMONO) {
	bzero(me->SessionKey, sizeof(RPC2_EncryptionKey));
	me->EncryptionType = 0;
    }
    else {
    	bcopy(SessionKey, me->SessionKey, sizeof(RPC2_EncryptionKey));
    	me->EncryptionType = EncryptionType;
    }

    me->listeners = (struct CEntry **)malloc(LISTENERALLOCSIZE*sizeof(struct CEntry *));
    assert(me->listeners != NULL);
    bzero(me->listeners, LISTENERALLOCSIZE*sizeof(struct CEntry *));
    me->howmanylisteners = 0;
    me->maxlisteners = LISTENERALLOCSIZE;

    me->CurrentPacket = (RPC2_PacketBuffer *)NULL;

    /* Following is analagous to ResolveBindParms() */
    switch(MulticastHost->Tag)
	{
	case RPC2_MGRPBYINETADDR:	/* you passed it in network order */
	    me->IPMHost.Tag = (HostTag) RPC2_MGRPBYINETADDR;
	    me->IPMHost.Value.InetAddress = MulticastHost->Value.InetAddress;
	    break;

	case RPC2_MGRPBYNAME:		/* NOT yet supported */
	    rpc2_FreeMgrp(me);
	    say(9, RPC2_DebugLevel, "MGRPBYNAME not supported\n");
	    rpc2_Quit(RPC2_FAIL);

	default:    assert(FALSE);
	}

    switch(MulticastPortal->Tag)
	{
	case RPC2_PORTALBYINETNUMBER:	/* you passed it in network order */
	    me->IPMPortal.Tag = RPC2_PORTALBYINETNUMBER;
	    me->IPMPortal.Value.InetPortNumber = MulticastPortal->Value.InetPortNumber;
	    break;

	case RPC2_PORTALBYNAME:
	    if ((sentry = getservbyname(MulticastPortal->Value.Name, NULL)) == NULL)
		{
		rpc2_FreeMgrp(me);
		say(9, RPC2_DebugLevel, "no entry for portal name %s\n", MulticastPortal->Value.Name);
		rpc2_Quit(RPC2_FAIL);
		}
	    if (htonl(1) == 1)
		{
		me->IPMPortal.Value.InetPortNumber = sentry->s_port;
		}
	    else
		{
		bcopy(&sentry->s_port, &me->IPMPortal.Value.InetPortNumber, sizeof(short));
		/* ghastly, but true: s_port is in network order, but
			stored as a 2-byte byte string in a 4-byte
			field */
		}
	    me->IPMPortal.Tag = RPC2_PORTALBYINETNUMBER;
	    break;

	default:    assert(FALSE);
	}

    switch(Subsys->Tag)
	{
	case RPC2_SUBSYSBYID:
	    me->SubsysId = Subsys->Value.SubsysId;
	    break;

	case RPC2_SUBSYSBYNAME:
		say(-1, RPC2_DebugLevel, "RPC2_SUBSYSBYNAME has been banned\n");
	default:    assert(FALSE);
	}

    /* Obtain pointer to appropriate set of side effect routines */
    if (SideEffectType != 0)
	{
	int	i;

	for (i = 0; i < SE_DefCount; i++)
	    if (SE_DefSpecs[i].SideEffectType == SideEffectType) break;
	if (i >= SE_DefCount)
	    {
	    rpc2_FreeMgrp(me);
	    say(9, RPC2_DebugLevel, "Bogus side effect specified (%ld)\n", SideEffectType);
	    rpc2_Quit(RPC2_FAIL);	/* bogus side effect */
	    }
	me->SEProcs = &SE_DefSpecs[i];
	}
    else me->SEProcs = NULL;

    /* Call side effect routine if present */
    if (me->SEProcs != NULL && me->SEProcs->SE_CreateMgrp != NULL)
	if ((secode = (*me->SEProcs->SE_CreateMgrp)(*MgroupHandle)) != RPC2_SUCCESS)
	    {
	    rpc2_FreeMgrp(me);
	    if (secode > RPC2_FLIMIT)
		rpc2_Quit(RPC2_SEFAIL1);
	    else
		rpc2_Quit(RPC2_SEFAIL2);	/* not really applicable??? */
	    }

    rpc2_Quit(RPC2_SUCCESS);
    }


/* Client-side operation only. */
long RPC2_AddToMgrp(IN MgroupHandle, IN ConnHandle)
    RPC2_Handle	MgroupHandle;
    RPC2_Handle	ConnHandle;
    {
    register struct MEntry		*me;
    register struct CEntry		*ce;
    RPC2_PacketBuffer			*pb;    	/* register cannot be used */
    register struct InitMulticastBody	*imb;
    struct SL_Entry			*sl;		/* register cannot be used */
    long				rc, secode;
    RPC2_PacketBuffer			*savedpkt;	/* in case SE reallocates */

    rpc2_Enter();
    say(0, RPC2_DebugLevel, "In RPC2_AddToMgrp()\n");

#ifdef RPC2DEBUG
    TR_ADDTOMGRP();
#endif RPC2DEBUG

    /* Validate multicast group and connection. */
    while (TRUE)
	{
	me = rpc2_GetMgrp(&rpc2_LocalHost, &rpc2_LocalPortal, MgroupHandle, CLIENT);
	if (me == NULL) rpc2_Quit(RPC2_NOMGROUP);
	if (TestState(me, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(me, CLIENT, ~C_THINK))
	    {
	    /*	if (!EnqueueRequest) rpc2_Quit(RPC2_MGRPBUSY);*/
	    say(0, RPC2_DebugLevel, "Enqueuing on mgrp 0x%lx\n",MgroupHandle);
	    LWP_WaitProcess((char *)me);
	    say(0, RPC2_DebugLevel, "Dequeueing on mgrp 0x%lx\n", MgroupHandle);
	    continue;
	    }

	ce = rpc2_GetConn(ConnHandle);
	if (ce == NULL) rpc2_Quit(RPC2_NOCONNECTION);
	assert(TestRole(ce, CLIENT));
	if (TestState(ce, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(ce, CLIENT, C_THINK))
	    {
	    if (ce->Mgrp != (struct MEntry *)NULL)
		if (ce->Mgrp == me) rpc2_Quit(RPC2_DUPLICATEMEMBER);
		else rpc2_Quit(RPC2_FAIL);
	    break;
	    }

	/*  if (!EnqueueRequest) rpc2_Quit(RPC2_CONNBUSY);*/
say(0, RPC2_DebugLevel, "Enqueuing on connection 0x%lx\n",ConnHandle);
	LWP_WaitProcess((char *)ce);
	say(0, RPC2_DebugLevel, "Dequeueing on connection 0x%lx\n", ConnHandle);
	}

    /* Check that the connection's Portal Number and SubsysId match that of the mgrp. */
    assert((me->IPMPortal.Tag == RPC2_PORTALBYINETNUMBER) && (ce->PeerPortal.Tag == RPC2_PORTALBYINETNUMBER));
    if (me->IPMPortal.Tag != ce->PeerPortal.Tag ||
        me->IPMPortal.Value.InetPortNumber != ce->PeerPortal.Value.InetPortNumber ||
        me->SubsysId != ce->SubsysId)
	rpc2_Quit(RPC2_BADMGROUP);

    /* Check that the connection's security level and encryption type
       match that of the mgrp. */
    /* QUESTION: if both Mgroup and Connection security levels = open
       kimono, should we bother checking that the Encryption Types are
       equivalent? */
    if ((me->SecurityLevel != ce->SecurityLevel) || 
        (me->SecurityLevel != RPC2_OPENKIMONO && me->EncryptionType != ce->EncryptionType))
	    rpc2_Quit(RPC2_BADMGROUP);
    
    /* Check that the connection has the same side-effect type as the
       multicast group */
    if (me->SEProcs != ce->SEProcs)
	    rpc2_Quit(RPC2_BADMGROUP);

    /* Construct an InitMulticast packet. */
    SetState(ce, C_AWAITREPLY);
    SetState(me, C_AWAITREPLY);	    /* Lock mgrp! */

    RPC2_AllocBuffer(sizeof(struct InitMulticastBody), &pb);
    rpc2_InitPacket(pb, ce, sizeof(struct InitMulticastBody));
    pb->Header.Opcode = RPC2_INITMULTICAST;
    pb->Header.SeqNumber = ce->NextSeqNumber;

    imb = (struct InitMulticastBody *)pb->Body;
    imb->MgroupHandle = htonl(me->MgroupID);
    imb->InitialSeqNumber = htonl(me->NextSeqNumber);
    bcopy(me->SessionKey, imb->SessionKey, sizeof(RPC2_EncryptionKey));

    /* Notify side-effect routine, if any */
    savedpkt = pb;
    if (me->SEProcs != NULL && me->SEProcs->SE_AddToMgrp != NULL){
	    secode = (*me->SEProcs->SE_AddToMgrp)(MgroupHandle, ConnHandle, &pb);
	    if (pb != savedpkt) 
		    RPC2_FreeBuffer(&savedpkt);	    /* free old buffer */
	    if (secode != RPC2_SUCCESS) {
		    RPC2_FreeBuffer(&pb);
		    if (secode > RPC2_FLIMIT) {
			    SetState(ce, C_THINK);
			    LWP_NoYieldSignal((char *)ce);
			    SetState(me, C_THINK);
			    LWP_NoYieldSignal((char *)me);
			    rpc2_Quit(RPC2_SEFAIL1);
		    }   else {
			    rpc2_SetConnError(ce);
			    SetState(me, C_THINK);
			    LWP_NoYieldSignal((char *)me);
			    rpc2_Quit(RPC2_SEFAIL2);
		    }
	    }
    }

    rpc2_htonp(pb);
    rpc2_ApplyE(pb, ce);

    /* Send packet and await positive acknowledgment. */
    say(9, RPC2_DebugLevel, "Sending INITMULTICAST packet on 0x%lx\n", ConnHandle);
    /* create call entry */
    sl = rpc2_AllocSle(OTHER, ce);
    rpc2_SendReliably(ce, sl, pb, NULL);

    switch(sl->ReturnCode)
	{
	case ARRIVED:
	    say(9, RPC2_DebugLevel, "Received INITMULTICAST response on 0x%lx\n", ConnHandle);
	    RPC2_FreeBuffer(&pb);	/* release the request packet */
	    pb = sl->Packet;		/* and get the response packet */
	    rpc2_FreeSle(&sl);
	    break;

	case NAKED:
	case TIMEOUT:
	    /* free connection, buffers, and quit */
	    say(9, RPC2_DebugLevel, "Failed to send INITMULTICAST packet on 0x%lx\n", ConnHandle);
	    RPC2_FreeBuffer(&pb);	/* release the request packet */
	    rc = sl->ReturnCode == NAKED ? RPC2_NAKED : RPC2_DEAD;
	    rpc2_FreeSle(&sl);
	    rpc2_SetConnError(ce);
	    SetState(me, C_THINK);
	    LWP_NoYieldSignal((char *)me);
	    rpc2_Quit(rc);

	default:	assert(FALSE);
	}

    rc = pb->Header.ReturnCode;
    say(9, RPC2_DebugLevel, "INITMULTICAST return code = %ld\n", rc);
    RPC2_FreeBuffer(&pb);
    if (rc != RPC2_SUCCESS)
	{
	LWP_NoYieldSignal((char *)ce);
	SetState(me, C_THINK);
	LWP_NoYieldSignal((char *)me);
	rpc2_Quit(rc);
	}

    /* Install ce into listener array. realloc() if necessary. */
    if (me->howmanylisteners == me->maxlisteners)
	{
	me->listeners = (struct CEntry **)realloc(me->listeners, (me->maxlisteners + LISTENERALLOCSIZE)*sizeof(struct CEntry *));
	assert(me->listeners != NULL);
	bzero(me->listeners + me->maxlisteners, LISTENERALLOCSIZE*sizeof(struct CEntry *));
	me->maxlisteners += LISTENERALLOCSIZE;
	}
    me->listeners[me->howmanylisteners] = ce;
    me->howmanylisteners++;
    ce->Mgrp = me;

    LWP_NoYieldSignal((char *)ce);
    SetState(me, C_THINK);
    LWP_NoYieldSignal((char *)me);
    rpc2_Quit(RPC2_SUCCESS);
    }


/* RPC2 internal version. */
/* This routine does NOT prohibit the removal of a connection from a
   busy multicast group. */
void rpc2_RemoveFromMgrp(me, ce)
    register struct MEntry  *me;
    register struct CEntry  *ce;
    {
    register int	    i;

#ifdef RPC2DEBUG
    TR_REMOVEFROMMGRP();
#endif RPC2DEBUG

    assert(me != NULL && !TestRole(me, FREE));
    if (TestState(me, CLIENT, ~(C_THINK|C_HARDERROR)) ||
        TestState(me, SERVER, ~(S_AWAITREQUEST|S_REQINQUEUE|S_PROCESS|S_HARDERROR)))
	say(9, RPC2_DebugLevel, "WARNING: connection being removed from busy mgroup\n");

    /* Find and remove the connection. */
    assert(ce != NULL && ce->Mgrp == me);
    if (TestRole(me, SERVER))
	{
	assert(me->conn == ce);
	rpc2_DeleteMgrp(me);	/* connection will be removed in FreeMgrp() */
	}
    else
	{
	for (i = 0; i < me->howmanylisteners; i++)
	    {
	    assert(me->listeners[i]->Mgrp == me);
	    if (me->listeners[i] == ce)	/* shuffle-up remaining entries */
		{
		for (; i < me->howmanylisteners	- 1; i++)   /* reuse i */
		    {
		    assert(me->listeners[i+1]->Mgrp == me);
		    me->listeners[i] = me->listeners[i+1];
		    }
		me->howmanylisteners--;
		ce->Mgrp = (struct MEntry *)NULL;
		return;
		}
	    }
	/* Didn't find connection in list; ce->Mgrp is a bogus pointer. */
	assert(FALSE);
	}
    }


/* User callable version.  Only available on client-side. */
long RPC2_RemoveFromMgrp(IN MgroupHandle, IN ConnHandle)
    RPC2_Handle	MgroupHandle;
    RPC2_Handle	ConnHandle;
    {
    register struct MEntry  *me;
    register struct CEntry  *ce;

    rpc2_Enter();
    say(0, RPC2_DebugLevel, "In RPC2_RemoveFromMgrp()\n");

    /* Validate multicast group and connection. */
    while (TRUE)
	{
	me = rpc2_GetMgrp(&rpc2_LocalHost, &rpc2_LocalPortal, MgroupHandle, CLIENT);
	if (me == NULL) rpc2_Quit(RPC2_NOMGROUP);
	if (TestState(me, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(me, CLIENT, ~C_THINK))
	    {
	    /*	if (!EnqueueRequest) rpc2_Quit(RPC2_MGRPBUSY);*/
	    say(0, RPC2_DebugLevel, "Enqueuing on mgrp 0x%lx\n",MgroupHandle);
	    LWP_WaitProcess((char *)me);
	    say(0, RPC2_DebugLevel, "Dequeueing on mgrp 0x%lx\n", MgroupHandle);
	    continue;
	    }

	ce = rpc2_GetConn(ConnHandle);
	if (ce == NULL) rpc2_Quit(RPC2_NOCONNECTION);
	assert(TestRole(ce, CLIENT));
	if (TestState(ce, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(ce, CLIENT, C_THINK))
	    {
	    if (ce->Mgrp != me) rpc2_Quit(RPC2_NOTGROUPMEMBER);
	    break;
	    }

	/*  if (!EnqueueRequest) rpc2_Quit(RPC2_CONNBUSY);*/
	say(0, RPC2_DebugLevel, "Enqueuing on connection 0x%lx\n",ConnHandle);
	LWP_WaitProcess((char *)ce);
	say(0, RPC2_DebugLevel, "Dequeueing on connection 0x%lx\n", ConnHandle);
	}

    rpc2_RemoveFromMgrp(me, ce);
    rpc2_Quit(RPC2_SUCCESS);
    }


/* RPC2 internal version. */
/* This routine does NOT prohibit the deletion of a busy multicast group. */
void rpc2_DeleteMgrp(me)
    register struct MEntry  *me;
    {
    assert(me != NULL && !TestRole(me, FREE));
    if (TestState(me, CLIENT, ~(C_THINK|C_HARDERROR)) ||
        TestState(me, SERVER, ~(S_AWAITREQUEST|S_REQINQUEUE|S_PROCESS|S_HARDERROR)))
	say(9, RPC2_DebugLevel, "WARNING: deleting busy mgroup\n");

    /* Call side-effect routine if appropriate; ignore result */
    if (me->SEProcs != NULL && me->SEProcs->SE_DeleteMgrp != NULL)  /* ignore result */
	(*me->SEProcs->SE_DeleteMgrp)(me->MgroupID, &me->ClientHost, &me->ClientPortal, (TestRole(me, SERVER) ? SERVER : CLIENT));

    rpc2_FreeMgrp(me);
    }


/* User callable version.  Only available on client-side. */
long RPC2_DeleteMgrp(IN MgroupHandle)
    RPC2_Handle	MgroupHandle;
    {
    register struct MEntry  *me;

    rpc2_Enter();
    say(0, RPC2_DebugLevel, "In RPC2_DeleteMgrp()\n");

    /* Validate multicast group. */
    while (TRUE)
	{
	me = rpc2_GetMgrp(&rpc2_LocalHost, &rpc2_LocalPortal, MgroupHandle, CLIENT);
	if (me == NULL) return(RPC2_NOMGROUP);
	if (TestState(me, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(me, CLIENT, C_THINK)) break;

	/*  if (!EnqueueRequest) rpc2_Quit(RPC2_MGRPBUSY);*/
	say(0, RPC2_DebugLevel, "Enqueuing on mgrp 0x%lx\n",MgroupHandle);
	LWP_WaitProcess((char *)me);
	say(0, RPC2_DebugLevel, "Dequeueing on mgrp 0x%lx\n", MgroupHandle);
	}

    rpc2_DeleteMgrp(me);
    rpc2_Quit(RPC2_SUCCESS);
    }


/* Either expand the MgroupHandle into the list of associated
   connections, or validate the connections that were passed in,
   whichever the caller has requested. */
long SetupMulticast(MCast, meaddr, HowMany, ConnHandleList)
    RPC2_Multicast  *MCast;
    struct MEntry   **meaddr;
    int		    HowMany;
    RPC2_Handle	    ConnHandleList[];
    {
    struct MEntry   *me;
    int		    i;

    /* initialize out parameter in case of failure */
    *meaddr = 0;

    /* Validate multicast group. */
    while (TRUE)
	{
	me = rpc2_GetMgrp(&rpc2_LocalHost, &rpc2_LocalPortal, MCast->Mgroup, CLIENT);
	if (me == NULL) return(RPC2_NOMGROUP);
	if (TestState(me, CLIENT, C_HARDERROR)) rpc2_Quit(RPC2_FAIL);

	if (TestState(me, CLIENT, C_THINK)) break;

	/*  if (!EnqueueRequest) rpc2_Quit(RPC2_MGRPBUSY);*/
	say(0, RPC2_DebugLevel, "Enqueuing on mgrp 0x%lx\n", MCast->Mgroup);
	LWP_WaitProcess((char *)me);
	say(0, RPC2_DebugLevel, "Dequeueing on mgrp 0x%lx\n", MCast->Mgroup);
	}

    assert(me->listeners != NULL && me->maxlisteners >= me->howmanylisteners);
    if (me->howmanylisteners == 0) return(RPC2_BADMGROUP);
    if (MCast->ExpandHandle)
	{
	if (me->howmanylisteners != HowMany) return(RPC2_BADMGROUP);
	for (i = 0; i < me->howmanylisteners; i++)
	    {
	    assert(me->listeners[i]->Mgrp == me);
	    ConnHandleList[i] = me->listeners[i]->UniqueCID;
	    }
	}
    else
	{
	int count = 0;
	for (i = 0; i < HowMany; i++)
	    {
	    struct CEntry *ce;

	    if (ConnHandleList[i] == 0) continue;
	    ce = rpc2_GetConn(ConnHandleList[i]);
	    if (ce == NULL) return(RPC2_BADMGROUP);
	    if (ce->Mgrp != me) return(RPC2_BADMGROUP);
	    count++;
	    }
	if (me->howmanylisteners != count) return(RPC2_BADMGROUP);
	}

    SetState(me, C_AWAITREPLY);	    /* lock mgrp! */
    *meaddr = me;
    return(RPC2_SUCCESS);
    }


void HandleInitMulticast(pb, ce)
    RPC2_PacketBuffer	*pb;
    struct CEntry	*ce;
    {
    register struct SL_Entry		*sl;
    register struct MEntry		*me;
    register long			rc;
    register struct InitMulticastBody	*imb;
    register unsigned long              ts;

    say(0, RPC2_DebugLevel, "In HandleInitMulticast()\n");

    rpc2_Recvd.Requests++;	    /* classify this as a normal request? */

    sl = ce->MySl;
    /* Free held packet and SL entry */
    if (sl != NULL)
    	{
	rpc2_DeactivateSle(sl, 0);
	FreeHeld(sl);
	}

    rpc2_IncrementSeqNumber(ce);

    imb = (struct InitMulticastBody *)pb->Body;
    imb->MgroupHandle = ntohl(imb->MgroupHandle);
    imb->InitialSeqNumber = ntohl(imb->InitialSeqNumber);

    /* If this connection is already bound to an Mgrp, remove it. */
    if (ce->Mgrp != NULL) rpc2_RemoveFromMgrp(ce->Mgrp, ce);

    /* If some other connection is bound to this Mgrp, remove it. */
    me = rpc2_GetMgrp(&ce->PeerHost, &ce->PeerPortal, imb->MgroupHandle, SERVER);
    if (me != NULL) rpc2_RemoveFromMgrp(me, me->conn);

    /* Allocate a fresh Mgrp and initialize it. */
    rc = RPC2_SUCCESS;		/* tentatively */
    me = rpc2_AllocMgrp(&ce->PeerHost, &ce->PeerPortal, imb->MgroupHandle);
    SetRole(me, SERVER);
    SetState(me, S_AWAITREQUEST);
    me->SubsysId = ce->SubsysId;
    me->NextSeqNumber = imb->InitialSeqNumber;
    me->SecurityLevel = ce->SecurityLevel;
    bcopy(imb->SessionKey, me->SessionKey, sizeof(RPC2_EncryptionKey));
    me->EncryptionType = ce->EncryptionType;
    me->conn = ce;
    ce->Mgrp = me;
    me->SEProcs = ce->SEProcs;
    if (me->SEProcs != NULL && me->SEProcs->SE_InitMulticast != NULL)
	if ((rc = (*me->SEProcs->SE_InitMulticast)(me->MgroupID, ce->UniqueCID, pb)) != RPC2_SUCCESS)
	    rpc2_FreeMgrp(me);

    ts = pb->Header.TimeStamp;
    RPC2_FreeBuffer(&pb);		    /* get rid of request packet */
    RPC2_AllocBuffer(0, &pb);		    /* get fresh buffer for reply */
    rpc2_InitPacket(pb, ce, 0);
    pb->Header.SeqNumber = ce->NextSeqNumber - 1;
    pb->Header.Opcode = RPC2_REPLY;
    pb->Header.ReturnCode = rc;
    pb->Header.TimeStamp = ts;
    rpc2_htonp(pb);
    rpc2_ApplyE(pb, ce);

    say(9, RPC2_DebugLevel, "Sending InitMulticast reply\n");
    rpc2_XmitPacket(rpc2_RequestSocket, pb, &ce->PeerHost, &ce->PeerPortal);

    /* Save reply for retransmission. */
    SavePacketForRetry(pb, ce);        
    }


bool XlateMcastPacket(pb, ThisHost, ThisPortal)
    RPC2_PacketBuffer	*pb;
    RPC2_HostIdent *ThisHost;
    RPC2_PortalIdent *ThisPortal;
    {
    register struct MEntry  *me;
    register struct CEntry  *ce;
    long    h_RemoteHandle = ntohl(pb->Header.RemoteHandle),
	    h_LocalHandle = ntohl(pb->Header.LocalHandle),
	    h_Flags = ntohl(pb->Header.Flags),
	    h_SeqNumber;				/* decrypt first */

    say(9, RPC2_DebugLevel, "In XlateMcastPacket()\n");

#ifdef RPC2DEBUG
    TR_XLATEMCASTPACKET();
#endif RPC2DEBUG

    /* This is a hack that may have to be changed.  Packets which are initially
       multicasted are sent on the point-to-point connection if and when they
       are retried.  In order for a server to maintain its multicast sequence
       number properly, it must be able to distinguish between retries on the
       point-to-point connection that were initially multicast and those which
       were not.  The hack currently adopted is to set the MULTICAST bit on 
       retries which were initially multicasted, even though the retry packet
       was not actually multicasted.  Obviously, this will not be sufficient
       if we ever physically multicast retries. */
    if (h_Flags & RPC2_RETRY) return(TRUE);	/* pass the packet untranslated */

    /* Lookup the multicast connection handle. */
    assert(h_RemoteHandle != 0);	/* would be a multicast Bind request! */
    assert(h_LocalHandle == 0);	/* extra sanity check */
    me = rpc2_GetMgrp(ThisHost, ThisPortal, h_RemoteHandle, SERVER);
    if (me == NULL) {BOGUS(pb); return(FALSE);}
/*    assert(TestRole(me, SERVER));	/* redundant check */
    ce = me->conn;
    assert(ce != NULL && TestRole(ce, SERVER) && ce->Mgrp == me);

    /* Drop ANY multicast packet that is out of the ordinary. */
    /* I suspect that only the checks on the me state and the me sequence
       number (further down) are strictly necessary at this point, but we'll
       be conservative for now. */
    if (TestState(me, SERVER, ~S_AWAITREQUEST) ||
        TestState(ce, SERVER, ~S_AWAITREQUEST) ||
        h_Flags & RPC2_RETRY != 0) {BOGUS(pb); return(FALSE);}

    say(9, RPC2_DebugLevel, "Host = 0x%lx\tPortal = 0x%x\tMgrp = 0x%lx\n", ThisHost->Value.InetAddress, ThisPortal->Value.InetPortNumber, h_RemoteHandle);

    /* Decrypt the packet with the MULTICAST session key. Clear the encrypted 
       bit so that we don't decrypt again with the connection session key. */
    if (h_Flags & RPC2_ENCRYPTED)
	{
	switch((int) me->SecurityLevel)
	    {
	    case RPC2_HEADERSONLY:
		rpc2_Decrypt((char *)&pb->Header.BodyLength,
				(char *)&pb->Header.BodyLength, 
				sizeof(struct RPC2_PacketHeader)-4*sizeof(RPC2_Integer),
				me->SessionKey, me->EncryptionType);
		break;
	
	    case RPC2_SECURE:
		rpc2_Decrypt(	(char *)&pb->Header.BodyLength,
				(char *)&pb->Header.BodyLength, 
				pb->Prefix.LengthOfPacket-4*sizeof(RPC2_Integer),
				me->SessionKey, me->EncryptionType);
		break;

	    default:    break;
	    }
	pb->Header.Flags = htonl(h_Flags &= ~RPC2_ENCRYPTED);
	}

    /* Make sure that the incoming and our stored multicast sequence numbers 
       agree.  If they do, transform the packet so that it will appear to have
       arrived on the (point-to-point) connection. */
    /* QUESTION: Don't we need some check here for MultiRpc packets ahead of
       sequence analagous to that in DecodePacket()? */
    h_SeqNumber = ntohl(pb->Header.SeqNumber);
    if (me->NextSeqNumber != h_SeqNumber) {BOGUS(pb); return(FALSE);}
    pb->Header.RemoteHandle = htonl(ce->UniqueCID);
    pb->Header.LocalHandle = htonl(ce->PeerHandle);
    pb->Header.SeqNumber = htonl(ce->NextSeqNumber);

    /* We do NOT update the multicast connection state and sequence number here.
       Instead, we leave the multicast flag-bit set so that routines which
       update connection state and sequence numbers can do the same for the
       multicast connection.  This avoids having to reverse the multicast state
       if we later decide to NAK or drop the packet. */

    say(9, RPC2_DebugLevel, "In XlateMcastPacket(): returning success\n");
    return(TRUE);
    }

