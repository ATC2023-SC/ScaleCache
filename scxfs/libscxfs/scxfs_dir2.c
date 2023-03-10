// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "scxfs.h"
#include "scxfs_fs.h"
#include "scxfs_shared.h"
#include "scxfs_format.h"
#include "scxfs_log_format.h"
#include "scxfs_trans_resv.h"
#include "scxfs_mount.h"
#include "scxfs_inode.h"
#include "scxfs_trans.h"
#include "scxfs_bmap.h"
#include "scxfs_dir2.h"
#include "scxfs_dir2_priv.h"
#include "scxfs_errortag.h"
#include "scxfs_error.h"
#include "scxfs_trace.h"

struct scxfs_name scxfs_name_dotdot = { (unsigned char *)"..", 2, SCXFS_DIR3_FT_DIR };

/*
 * Convert inode mode to directory entry filetype
 */
unsigned char
scxfs_mode_to_ftype(
	int		mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return SCXFS_DIR3_FT_REG_FILE;
	case S_IFDIR:
		return SCXFS_DIR3_FT_DIR;
	case S_IFCHR:
		return SCXFS_DIR3_FT_CHRDEV;
	case S_IFBLK:
		return SCXFS_DIR3_FT_BLKDEV;
	case S_IFIFO:
		return SCXFS_DIR3_FT_FIFO;
	case S_IFSOCK:
		return SCXFS_DIR3_FT_SOCK;
	case S_IFLNK:
		return SCXFS_DIR3_FT_SYMLINK;
	default:
		return SCXFS_DIR3_FT_UNKNOWN;
	}
}

/*
 * ASCII case-insensitive (ie. A-Z) support for directories that was
 * used in IRIX.
 */
STATIC scxfs_dahash_t
scxfs_ascii_ci_hashname(
	struct scxfs_name	*name)
{
	scxfs_dahash_t	hash;
	int		i;

	for (i = 0, hash = 0; i < name->len; i++)
		hash = tolower(name->name[i]) ^ rol32(hash, 7);

	return hash;
}

STATIC enum scxfs_dacmp
scxfs_ascii_ci_compname(
	struct scxfs_da_args *args,
	const unsigned char *name,
	int		len)
{
	enum scxfs_dacmp	result;
	int		i;

	if (args->namelen != len)
		return SCXFS_CMP_DIFFERENT;

	result = SCXFS_CMP_EXACT;
	for (i = 0; i < len; i++) {
		if (args->name[i] == name[i])
			continue;
		if (tolower(args->name[i]) != tolower(name[i]))
			return SCXFS_CMP_DIFFERENT;
		result = SCXFS_CMP_CASE;
	}

	return result;
}

static const struct scxfs_nameops scxfs_ascii_ci_nameops = {
	.hashname	= scxfs_ascii_ci_hashname,
	.compname	= scxfs_ascii_ci_compname,
};

int
scxfs_da_mount(
	struct scxfs_mount	*mp)
{
	struct scxfs_da_geometry	*dageo;
	int			nodehdr_size;


	ASSERT(mp->m_sb.sb_versionnum & SCXFS_SB_VERSION_DIRV2BIT);
	ASSERT(scxfs_dir2_dirblock_bytes(&mp->m_sb) <= SCXFS_MAX_BLOCKSIZE);

	mp->m_dir_inode_ops = scxfs_dir_get_ops(mp, NULL);
	mp->m_nondir_inode_ops = scxfs_nondir_get_ops(mp, NULL);

	nodehdr_size = mp->m_dir_inode_ops->node_hdr_size;
	mp->m_dir_geo = kmem_zalloc(sizeof(struct scxfs_da_geometry),
				    KM_MAYFAIL);
	mp->m_attr_geo = kmem_zalloc(sizeof(struct scxfs_da_geometry),
				     KM_MAYFAIL);
	if (!mp->m_dir_geo || !mp->m_attr_geo) {
		kmem_free(mp->m_dir_geo);
		kmem_free(mp->m_attr_geo);
		return -ENOMEM;
	}

	/* set up directory geometry */
	dageo = mp->m_dir_geo;
	dageo->blklog = mp->m_sb.sb_blocklog + mp->m_sb.sb_dirblklog;
	dageo->fsblog = mp->m_sb.sb_blocklog;
	dageo->blksize = scxfs_dir2_dirblock_bytes(&mp->m_sb);
	dageo->fsbcount = 1 << mp->m_sb.sb_dirblklog;

	/*
	 * Now we've set up the block conversion variables, we can calculate the
	 * segment block constants using the geometry structure.
	 */
	dageo->datablk = scxfs_dir2_byte_to_da(dageo, SCXFS_DIR2_DATA_OFFSET);
	dageo->leafblk = scxfs_dir2_byte_to_da(dageo, SCXFS_DIR2_LEAF_OFFSET);
	dageo->freeblk = scxfs_dir2_byte_to_da(dageo, SCXFS_DIR2_FREE_OFFSET);
	dageo->node_ents = (dageo->blksize - nodehdr_size) /
				(uint)sizeof(scxfs_da_node_entry_t);
	dageo->magicpct = (dageo->blksize * 37) / 100;

	/* set up attribute geometry - single fsb only */
	dageo = mp->m_attr_geo;
	dageo->blklog = mp->m_sb.sb_blocklog;
	dageo->fsblog = mp->m_sb.sb_blocklog;
	dageo->blksize = 1 << dageo->blklog;
	dageo->fsbcount = 1;
	dageo->node_ents = (dageo->blksize - nodehdr_size) /
				(uint)sizeof(scxfs_da_node_entry_t);
	dageo->magicpct = (dageo->blksize * 37) / 100;

	if (scxfs_sb_version_hasasciici(&mp->m_sb))
		mp->m_dirnameops = &scxfs_ascii_ci_nameops;
	else
		mp->m_dirnameops = &scxfs_default_nameops;

	return 0;
}

void
scxfs_da_unmount(
	struct scxfs_mount	*mp)
{
	kmem_free(mp->m_dir_geo);
	kmem_free(mp->m_attr_geo);
}

/*
 * Return 1 if directory contains only "." and "..".
 */
int
scxfs_dir_isempty(
	scxfs_inode_t	*dp)
{
	scxfs_dir2_sf_hdr_t	*sfp;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	if (dp->i_d.di_size == 0)	/* might happen during shutdown. */
		return 1;
	if (dp->i_d.di_size > SCXFS_IFORK_DSIZE(dp))
		return 0;
	sfp = (scxfs_dir2_sf_hdr_t *)dp->i_df.if_u1.if_data;
	return !sfp->count;
}

/*
 * Validate a given inode number.
 */
int
scxfs_dir_ino_validate(
	scxfs_mount_t	*mp,
	scxfs_ino_t	ino)
{
	bool		ino_ok = scxfs_verify_dir_ino(mp, ino);

	if (unlikely(SCXFS_TEST_ERROR(!ino_ok, mp, SCXFS_ERRTAG_DIR_INO_VALIDATE))) {
		scxfs_warn(mp, "Invalid inode number 0x%Lx",
				(unsigned long long) ino);
		SCXFS_ERROR_REPORT("scxfs_dir_ino_validate", SCXFS_ERRLEVEL_LOW, mp);
		return -EFSCORRUPTED;
	}
	return 0;
}

/*
 * Initialize a directory with its "." and ".." entries.
 */
int
scxfs_dir_init(
	scxfs_trans_t	*tp,
	scxfs_inode_t	*dp,
	scxfs_inode_t	*pdp)
{
	struct scxfs_da_args *args;
	int		error;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	error = scxfs_dir_ino_validate(tp->t_mountp, pdp->i_ino);
	if (error)
		return error;

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->dp = dp;
	args->trans = tp;
	error = scxfs_dir2_sf_create(args, pdp->i_ino);
	kmem_free(args);
	return error;
}

/*
 * Enter a name in a directory, or check for available space.
 * If inum is 0, only the available space test is performed.
 */
int
scxfs_dir_createname(
	struct scxfs_trans	*tp,
	struct scxfs_inode	*dp,
	struct scxfs_name		*name,
	scxfs_ino_t		inum,		/* new entry inode number */
	scxfs_extlen_t		total)		/* bmap's total block count */
{
	struct scxfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	if (inum) {
		rval = scxfs_dir_ino_validate(tp->t_mountp, inum);
		if (rval)
			return rval;
		SCXFS_STATS_INC(dp->i_mount, xs_dir_create);
	}

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = dp->i_mount->m_dirnameops->hashname(name);
	args->inumber = inum;
	args->dp = dp;
	args->total = total;
	args->whichfork = SCXFS_DATA_FORK;
	args->trans = tp;
	args->op_flags = SCXFS_DA_OP_ADDNAME | SCXFS_DA_OP_OKNOENT;
	if (!inum)
		args->op_flags |= SCXFS_DA_OP_JUSTCHECK;

	if (dp->i_d.di_format == SCXFS_DINODE_FMT_LOCAL) {
		rval = scxfs_dir2_sf_addname(args);
		goto out_free;
	}

	rval = scxfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = scxfs_dir2_block_addname(args);
		goto out_free;
	}

	rval = scxfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = scxfs_dir2_leaf_addname(args);
	else
		rval = scxfs_dir2_node_addname(args);

out_free:
	kmem_free(args);
	return rval;
}

/*
 * If doing a CI lookup and case-insensitive match, dup actual name into
 * args.value. Return EEXIST for success (ie. name found) or an error.
 */
int
scxfs_dir_cilookup_result(
	struct scxfs_da_args *args,
	const unsigned char *name,
	int		len)
{
	if (args->cmpresult == SCXFS_CMP_DIFFERENT)
		return -ENOENT;
	if (args->cmpresult != SCXFS_CMP_CASE ||
					!(args->op_flags & SCXFS_DA_OP_CILOOKUP))
		return -EEXIST;

	args->value = kmem_alloc(len, KM_NOFS | KM_MAYFAIL);
	if (!args->value)
		return -ENOMEM;

	memcpy(args->value, name, len);
	args->valuelen = len;
	return -EEXIST;
}

/*
 * Lookup a name in a directory, give back the inode number.
 * If ci_name is not NULL, returns the actual name in ci_name if it differs
 * to name, or ci_name->name is set to NULL for an exact match.
 */

int
scxfs_dir_lookup(
	scxfs_trans_t	*tp,
	scxfs_inode_t	*dp,
	struct scxfs_name	*name,
	scxfs_ino_t	*inum,		/* out: inode number */
	struct scxfs_name *ci_name)	/* out: actual name if CI match */
{
	struct scxfs_da_args *args;
	int		rval;
	int		v;		/* type-checking value */
	int		lock_mode;

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	SCXFS_STATS_INC(dp->i_mount, xs_dir_lookup);

	/*
	 * We need to use KM_NOFS here so that lockdep will not throw false
	 * positive deadlock warnings on a non-transactional lookup path. It is
	 * safe to recurse into inode recalim in that case, but lockdep can't
	 * easily be taught about it. Hence KM_NOFS avoids having to add more
	 * lockdep Doing this avoids having to add a bunch of lockdep class
	 * annotations into the reclaim path for the ilock.
	 */
	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = dp->i_mount->m_dirnameops->hashname(name);
	args->dp = dp;
	args->whichfork = SCXFS_DATA_FORK;
	args->trans = tp;
	args->op_flags = SCXFS_DA_OP_OKNOENT;
	if (ci_name)
		args->op_flags |= SCXFS_DA_OP_CILOOKUP;

	lock_mode = scxfs_ilock_data_map_shared(dp);
	if (dp->i_d.di_format == SCXFS_DINODE_FMT_LOCAL) {
		rval = scxfs_dir2_sf_lookup(args);
		goto out_check_rval;
	}

	rval = scxfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = scxfs_dir2_block_lookup(args);
		goto out_check_rval;
	}

	rval = scxfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = scxfs_dir2_leaf_lookup(args);
	else
		rval = scxfs_dir2_node_lookup(args);

out_check_rval:
	if (rval == -EEXIST)
		rval = 0;
	if (!rval) {
		*inum = args->inumber;
		if (ci_name) {
			ci_name->name = args->value;
			ci_name->len = args->valuelen;
		}
	}
out_free:
	scxfs_iunlock(dp, lock_mode);
	kmem_free(args);
	return rval;
}

/*
 * Remove an entry from a directory.
 */
int
scxfs_dir_removename(
	struct scxfs_trans	*tp,
	struct scxfs_inode	*dp,
	struct scxfs_name		*name,
	scxfs_ino_t		ino,
	scxfs_extlen_t		total)		/* bmap's total block count */
{
	struct scxfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));
	SCXFS_STATS_INC(dp->i_mount, xs_dir_remove);

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = dp->i_mount->m_dirnameops->hashname(name);
	args->inumber = ino;
	args->dp = dp;
	args->total = total;
	args->whichfork = SCXFS_DATA_FORK;
	args->trans = tp;

	if (dp->i_d.di_format == SCXFS_DINODE_FMT_LOCAL) {
		rval = scxfs_dir2_sf_removename(args);
		goto out_free;
	}

	rval = scxfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = scxfs_dir2_block_removename(args);
		goto out_free;
	}

	rval = scxfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = scxfs_dir2_leaf_removename(args);
	else
		rval = scxfs_dir2_node_removename(args);
out_free:
	kmem_free(args);
	return rval;
}

/*
 * Replace the inode number of a directory entry.
 */
int
scxfs_dir_replace(
	struct scxfs_trans	*tp,
	struct scxfs_inode	*dp,
	struct scxfs_name		*name,		/* name of entry to replace */
	scxfs_ino_t		inum,		/* new inode number */
	scxfs_extlen_t		total)		/* bmap's total block count */
{
	struct scxfs_da_args	*args;
	int			rval;
	int			v;		/* type-checking value */

	ASSERT(S_ISDIR(VFS_I(dp)->i_mode));

	rval = scxfs_dir_ino_validate(tp->t_mountp, inum);
	if (rval)
		return rval;

	args = kmem_zalloc(sizeof(*args), KM_NOFS);
	if (!args)
		return -ENOMEM;

	args->geo = dp->i_mount->m_dir_geo;
	args->name = name->name;
	args->namelen = name->len;
	args->filetype = name->type;
	args->hashval = dp->i_mount->m_dirnameops->hashname(name);
	args->inumber = inum;
	args->dp = dp;
	args->total = total;
	args->whichfork = SCXFS_DATA_FORK;
	args->trans = tp;

	if (dp->i_d.di_format == SCXFS_DINODE_FMT_LOCAL) {
		rval = scxfs_dir2_sf_replace(args);
		goto out_free;
	}

	rval = scxfs_dir2_isblock(args, &v);
	if (rval)
		goto out_free;
	if (v) {
		rval = scxfs_dir2_block_replace(args);
		goto out_free;
	}

	rval = scxfs_dir2_isleaf(args, &v);
	if (rval)
		goto out_free;
	if (v)
		rval = scxfs_dir2_leaf_replace(args);
	else
		rval = scxfs_dir2_node_replace(args);
out_free:
	kmem_free(args);
	return rval;
}

/*
 * See if this entry can be added to the directory without allocating space.
 */
int
scxfs_dir_canenter(
	scxfs_trans_t	*tp,
	scxfs_inode_t	*dp,
	struct scxfs_name	*name)		/* name of entry to add */
{
	return scxfs_dir_createname(tp, dp, name, 0, 0);
}

/*
 * Utility routines.
 */

/*
 * Add a block to the directory.
 *
 * This routine is for data and free blocks, not leaf/node blocks which are
 * handled by scxfs_da_grow_inode.
 */
int
scxfs_dir2_grow_inode(
	struct scxfs_da_args	*args,
	int			space,	/* v2 dir's space SCXFS_DIR2_xxx_SPACE */
	scxfs_dir2_db_t		*dbp)	/* out: block number added */
{
	struct scxfs_inode	*dp = args->dp;
	struct scxfs_mount	*mp = dp->i_mount;
	scxfs_fileoff_t		bno;	/* directory offset of new block */
	int			count;	/* count of filesystem blocks */
	int			error;

	trace_scxfs_dir2_grow_inode(args, space);

	/*
	 * Set lowest possible block in the space requested.
	 */
	bno = SCXFS_B_TO_FSBT(mp, space * SCXFS_DIR2_SPACE_SIZE);
	count = args->geo->fsbcount;

	error = scxfs_da_grow_inode_int(args, &bno, count);
	if (error)
		return error;

	*dbp = scxfs_dir2_da_to_db(args->geo, (scxfs_dablk_t)bno);

	/*
	 * Update file's size if this is the data space and it grew.
	 */
	if (space == SCXFS_DIR2_DATA_SPACE) {
		scxfs_fsize_t	size;		/* directory file (data) size */

		size = SCXFS_FSB_TO_B(mp, bno + count);
		if (size > dp->i_d.di_size) {
			dp->i_d.di_size = size;
			scxfs_trans_log_inode(args->trans, dp, SCXFS_ILOG_CORE);
		}
	}
	return 0;
}

/*
 * See if the directory is a single-block form directory.
 */
int
scxfs_dir2_isblock(
	struct scxfs_da_args	*args,
	int			*vp)	/* out: 1 is block, 0 is not block */
{
	scxfs_fileoff_t		last;	/* last file offset */
	int			rval;

	if ((rval = scxfs_bmap_last_offset(args->dp, &last, SCXFS_DATA_FORK)))
		return rval;
	rval = SCXFS_FSB_TO_B(args->dp->i_mount, last) == args->geo->blksize;
	if (rval != 0 && args->dp->i_d.di_size != args->geo->blksize)
		return -EFSCORRUPTED;
	*vp = rval;
	return 0;
}

/*
 * See if the directory is a single-leaf form directory.
 */
int
scxfs_dir2_isleaf(
	struct scxfs_da_args	*args,
	int			*vp)	/* out: 1 is block, 0 is not block */
{
	scxfs_fileoff_t		last;	/* last file offset */
	int			rval;

	if ((rval = scxfs_bmap_last_offset(args->dp, &last, SCXFS_DATA_FORK)))
		return rval;
	*vp = last == args->geo->leafblk + args->geo->fsbcount;
	return 0;
}

/*
 * Remove the given block from the directory.
 * This routine is used for data and free blocks, leaf/node are done
 * by scxfs_da_shrink_inode.
 */
int
scxfs_dir2_shrink_inode(
	struct scxfs_da_args	*args,
	scxfs_dir2_db_t		db,
	struct scxfs_buf		*bp)
{
	scxfs_fileoff_t		bno;		/* directory file offset */
	scxfs_dablk_t		da;		/* directory file offset */
	int			done;		/* bunmap is finished */
	struct scxfs_inode	*dp;
	int			error;
	struct scxfs_mount	*mp;
	struct scxfs_trans	*tp;

	trace_scxfs_dir2_shrink_inode(args, db);

	dp = args->dp;
	mp = dp->i_mount;
	tp = args->trans;
	da = scxfs_dir2_db_to_da(args->geo, db);

	/* Unmap the fsblock(s). */
	error = scxfs_bunmapi(tp, dp, da, args->geo->fsbcount, 0, 0, &done);
	if (error) {
		/*
		 * ENOSPC actually can happen if we're in a removename with no
		 * space reservation, and the resulting block removal would
		 * cause a bmap btree split or conversion from extents to btree.
		 * This can only happen for un-fragmented directory blocks,
		 * since you need to be punching out the middle of an extent.
		 * In this case we need to leave the block in the file, and not
		 * binval it.  So the block has to be in a consistent empty
		 * state and appropriately logged.  We don't free up the buffer,
		 * the caller can tell it hasn't happened since it got an error
		 * back.
		 */
		return error;
	}
	ASSERT(done);
	/*
	 * Invalidate the buffer from the transaction.
	 */
	scxfs_trans_binval(tp, bp);
	/*
	 * If it's not a data block, we're done.
	 */
	if (db >= scxfs_dir2_byte_to_db(args->geo, SCXFS_DIR2_LEAF_OFFSET))
		return 0;
	/*
	 * If the block isn't the last one in the directory, we're done.
	 */
	if (dp->i_d.di_size > scxfs_dir2_db_off_to_byte(args->geo, db + 1, 0))
		return 0;
	bno = da;
	if ((error = scxfs_bmap_last_before(tp, dp, &bno, SCXFS_DATA_FORK))) {
		/*
		 * This can't really happen unless there's kernel corruption.
		 */
		return error;
	}
	if (db == args->geo->datablk)
		ASSERT(bno == 0);
	else
		ASSERT(bno > 0);
	/*
	 * Set the size to the new last block.
	 */
	dp->i_d.di_size = SCXFS_FSB_TO_B(mp, bno);
	scxfs_trans_log_inode(tp, dp, SCXFS_ILOG_CORE);
	return 0;
}

/* Returns true if the directory entry name is valid. */
bool
scxfs_dir2_namecheck(
	const void	*name,
	size_t		length)
{
	/*
	 * MAXNAMELEN includes the trailing null, but (name/length) leave it
	 * out, so use >= for the length check.
	 */
	if (length >= MAXNAMELEN)
		return false;

	/* There shouldn't be any slashes or nulls here */
	return !memchr(name, '/', length) && !memchr(name, 0, length);
}
