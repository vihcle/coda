/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2004 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights

#*/

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <stdio.h>

#ifdef __cplusplus
}
#endif

#include "cvnode.h"
#include "volume.h"
#include "al.h"

/* Debugging routine in file separate from volume.cc to simplify 
ease of use from other modules (e.g. codadump2tar.cc in volutil).
Otherwise, a lot of baggage gets linked in (Satya, May 2004) */

void PrintVolumeDiskData(FILE *outfile, VolumeDiskData *vdd){
  fprintf(outfile, "\nVolumeDiskData:\n");
  fprintf(outfile, "  id = 0x%08x  partition = '%s'  name = '%s'\n",
	  vdd->id, vdd->partition, vdd->name);
  fprintf(outfile, "  inUse = 0x%02x  inService = 0x%02x  blessed = 0x%02x  needsSalvaged = 0x%02x\n", vdd->inUse, vdd->inService, vdd->blessed,
	  vdd->needsSalvaged);

  fprintf(outfile, "  uniquifier = 0x%08x  type = 0x%08x  needsSalvaged = 0x%02x\n", vdd->uniquifier, vdd->type, vdd->needsSalvaged);

  fprintf(outfile, "  parentId = 0x%08x  groupId = 0x%08x  cloneId = 0x%08x\n", vdd->parentId, vdd->groupId, vdd->cloneId);

  fprintf(outfile, "  backupId = 0x%08x  restoredFromId = 0x%08x  needsCallback = 0x%02x\n", vdd->backupId, vdd->restoredFromId, vdd->needsCallback);

  fprintf(outfile, "  destroyMe = 0x%02x  dontSalvage = 0x%02x  ResOn = 0x%02x\n", vdd->destroyMe, vdd->dontSalvage, vdd->ResOn);

  fprintf(outfile, "  maxquota = %d  minquota = %d  maxfiles = %d\n", vdd->maxquota, vdd->minquota, vdd->maxfiles);

  fprintf(outfile, "  accountNumber = 0x08%x  owner = 0x08%x\n", vdd->accountNumber, vdd->owner);

  fprintf(outfile, "  filecount = %d  linkcount = %d  diskused = %d\n", vdd->filecount, vdd->linkcount, vdd->diskused);

}

void PrintVnodeDiskObject(FILE *outfile, VnodeDiskObject *vnode,
			  VnodeId vnodeNumber)
{
    fprintf(outfile, "Vnode 0x%08lx.%08lx.%08lx, cloned = %u, length = %lu, inode = %lu\n",
        vnodeNumber, vnode->uniquifier, vnode->dataVersion, vnode->cloned,
	vnode->length, vnode->inodeNumber);
    fprintf(outfile, "link count = %u, type = %u, volume index = %d\n", vnode->linkCount, vnode->type, vnode->vol_index);
    fprintf(outfile, "parent = 0x%08lx.%08lx\n", vnode->vparent, vnode->uparent);
    PrintVV(outfile, &(vnode->versionvector));

    /* Print the ACL if there is one */
    if (vnode->type == vDirectory) {
      AL_AccessList *a = VVnodeDiskACL(vnode);
      fprintf(outfile, "ACL: TotalNoOfEntries = %d, PlusEntriesInUse = %d, MinusEntriesInUse = %d\n",
	      a->TotalNoOfEntries, a->PlusEntriesInUse, a->MinusEntriesInUse);
      for (int i = 0; i < a->PlusEntriesInUse; i++)
	fprintf(outfile, "Id = %d  Rights = 0x%08x\n", 
		a->ActualEntries[i].Id, a->ActualEntries[i].Rights);

      for (int i = 0; i < a->MinusEntriesInUse; i++){
	int j = a->PlusEntriesInUse - 1 + i;
	fprintf(outfile, "Id = %d  Rights = -0x%08x\n", 
		a->ActualEntries[j].Id, a->ActualEntries[j].Rights);
      }
    }
    return;
}