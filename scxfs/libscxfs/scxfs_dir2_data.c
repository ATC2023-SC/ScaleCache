// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc.
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
#include "scxfs_dir2.h"
#include "scxfs_error.h"
#include "scxfs_trans.h"
#include "scxfs_buf_item.h"
#include "scxfs_log.h"

static scxfs_failaddr_t scxfs_dir2_data_freefind_verify(
		struct scxfs_dir2_data_hdr *hdr, struct scxfs_dir2_data_free *bf,
		struct scxfs_dir2_data_unused *dup,
		struct scxfs_dir2_data_free **bf_ent);

/*
 * Check the consistency of the data block.
 * The input can also be a block-format directory.
 * Return NULL if the buffer is good, otherwise the address of the error.
 */
scxfs_failaddr_t
__scxfs_dir3_data_check(
	struct scxfs_inode	*dp,		/* incore inode pointer */
	struct scxfs_buf		*bp)		/* data block's buffer */
{
	scxfs_dir2_dataptr_t	addr;		/* addr for leaf lookup */
	scxfs_dir2_data_free_t	*bf;		/* bestfree table */
	scxfs_dir2_block_tail_t	*btp=NULL;	/* block tail */
	int			count;		/* count of entries found */
	scxfs_dir2_data_hdr_t	*hdr;		/* data block header */
	scxfs_dir2_data_entry_t	*dep;		/* data entry */
	scxfs_dir2_data_free_t	*dfp;		/* bestfree entry */
	scxfs_dir2_data_unused_t	*dup;		/* unused entry */
	char			*endp;		/* end of useful data */
	int			freeseen;	/* mask of bestfrees seen */
	scxfs_dahash_t		hash;		/* hash of current name */
	int			i;		/* leaf index */
	int			lastfree;	/* last entry was unused */
	scxfs_dir2_leaf_entry_t	*lep=NULL;	/* block leaf entries */
	struct scxfs_mount	*mp = bp->b_mount;
	char			*p;		/* current data position */
	int			stale;		/* count of stale leaves */
	struct scxfs_name		name;
	const struct scxfs_dir_ops *ops;
	struct scxfs_da_geometry	*geo;

	geo = mp->m_dir_geo;

	/*
	 * We can be passed a null dp here from a verifier, so we need to go the
	 * hard way to get them.
	 */
	ops = scxfs_dir_get_ops(mp, dp);

	/*
	 * If this isn't a directory, or we don't get handed the dir ops,
	 * something is seriously wrong.  Bail out.
	 */
	if ((dp && !S_ISDIR(VFS_I(dp)->i_mode)) ||
	    ops != scxfs_dir_get_ops(mp, NULL))
		return __this_address;

	hdr = bp->b_addr;
	p = (char *)ops->data_entry_p(hdr);

	switch (hdr->magic) {
	case cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC):
	case cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC):
		btp = scxfs_dir2_block_tail_p(geo, hdr);
		lep = scxfs_dir2_block_leaf_p(btp);

		/*
		 * The number of leaf entries is limited by the size of the
		 * block and the amount of space used by the data entries.
		 * We don't know how much space is used by the data entries yet,
		 * so just ensure that the count falls somewhere inside the
		 * block right now.
		 */
		if (be32_to_cpu(btp->count) >=
		    ((char *)btp - p) / sizeof(struct scxfs_dir2_leaf_entry))
			return __this_address;
		break;
	case cpu_to_be32(SCXFS_DIR3_DATA_MAGIC):
	case cpu_to_be32(SCXFS_DIR2_DATA_MAGIC):
		break;
	default:
		return __this_address;
	}
	endp = scxfs_dir3_data_endp(geo, hdr);
	if (!endp)
		return __this_address;

	/*
	 * Account for zero bestfree entries.
	 */
	bf = ops->data_bestfree_p(hdr);
	count = lastfree = freeseen = 0;
	if (!bf[0].length) {
		if (bf[0].offset)
			return __this_address;
		freeseen |= 1 << 0;
	}
	if (!bf[1].length) {
		if (bf[1].offset)
			return __this_address;
		freeseen |= 1 << 1;
	}
	if (!bf[2].length) {
		if (bf[2].offset)
			return __this_address;
		freeseen |= 1 << 2;
	}

	if (be16_to_cpu(bf[0].length) < be16_to_cpu(bf[1].length))
		return __this_address;
	if (be16_to_cpu(bf[1].length) < be16_to_cpu(bf[2].length))
		return __this_address;
	/*
	 * Loop over the data/unused entries.
	 */
	while (p < endp) {
		dup = (scxfs_dir2_data_unused_t *)p;
		/*
		 * If it's unused, look for the space in the bestfree table.
		 * If we find it, account for that, else make sure it
		 * doesn't need to be there.
		 */
		if (be16_to_cpu(dup->freetag) == SCXFS_DIR2_DATA_FREE_TAG) {
			scxfs_failaddr_t	fa;

			if (lastfree != 0)
				return __this_address;
			if (endp < p + be16_to_cpu(dup->length))
				return __this_address;
			if (be16_to_cpu(*scxfs_dir2_data_unused_tag_p(dup)) !=
			    (char *)dup - (char *)hdr)
				return __this_address;
			fa = scxfs_dir2_data_freefind_verify(hdr, bf, dup, &dfp);
			if (fa)
				return fa;
			if (dfp) {
				i = (int)(dfp - bf);
				if ((freeseen & (1 << i)) != 0)
					return __this_address;
				freeseen |= 1 << i;
			} else {
				if (be16_to_cpu(dup->length) >
				    be16_to_cpu(bf[2].length))
					return __this_address;
			}
			p += be16_to_cpu(dup->length);
			lastfree = 1;
			continue;
		}
		/*
		 * It's a real entry.  Validate the fields.
		 * If this is a block directory then make sure it's
		 * in the leaf section of the block.
		 * The linear search is crude but this is DEBUG code.
		 */
		dep = (scxfs_dir2_data_entry_t *)p;
		if (dep->namelen == 0)
			return __this_address;
		if (scxfs_dir_ino_validate(mp, be64_to_cpu(dep->inumber)))
			return __this_address;
		if (endp < p + ops->data_entsize(dep->namelen))
			return __this_address;
		if (be16_to_cpu(*ops->data_entry_tag_p(dep)) !=
		    (char *)dep - (char *)hdr)
			return __this_address;
		if (ops->data_get_ftype(dep) >= SCXFS_DIR3_FT_MAX)
			return __this_address;
		count++;
		lastfree = 0;
		if (hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
		    hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC)) {
			addr = scxfs_dir2_db_off_to_dataptr(geo, geo->datablk,
						(scxfs_dir2_data_aoff_t)
						((char *)dep - (char *)hdr));
			name.name = dep->name;
			name.len = dep->namelen;
			hash = mp->m_dirnameops->hashname(&name);
			for (i = 0; i < be32_to_cpu(btp->count); i++) {
				if (be32_to_cpu(lep[i].address) == addr &&
				    be32_to_cpu(lep[i].hashval) == hash)
					break;
			}
			if (i >= be32_to_cpu(btp->count))
				return __this_address;
		}
		p += ops->data_entsize(dep->namelen);
	}
	/*
	 * Need to have seen all the entries and all the bestfree slots.
	 */
	if (freeseen != 7)
		return __this_address;
	if (hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	    hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC)) {
		for (i = stale = 0; i < be32_to_cpu(btp->count); i++) {
			if (lep[i].address ==
			    cpu_to_be32(SCXFS_DIR2_NULL_DATAPTR))
				stale++;
			if (i > 0 && be32_to_cpu(lep[i].hashval) <
				     be32_to_cpu(lep[i - 1].hashval))
				return __this_address;
		}
		if (count != be32_to_cpu(btp->count) - be32_to_cpu(btp->stale))
			return __this_address;
		if (stale != be32_to_cpu(btp->stale))
			return __this_address;
	}
	return NULL;
}

#ifdef DEBUG
void
scxfs_dir3_data_check(
	struct scxfs_inode	*dp,
	struct scxfs_buf		*bp)
{
	scxfs_failaddr_t		fa;

	fa = __scxfs_dir3_data_check(dp, bp);
	if (!fa)
		return;
	scxfs_corruption_error(__func__, SCXFS_ERRLEVEL_LOW, dp->i_mount,
			bp->b_addr, BBTOB(bp->b_length), __FILE__, __LINE__,
			fa);
	ASSERT(0);
}
#endif

static scxfs_failaddr_t
scxfs_dir3_data_verify(
	struct scxfs_buf		*bp)
{
	struct scxfs_mount	*mp = bp->b_mount;
	struct scxfs_dir3_blk_hdr	*hdr3 = bp->b_addr;

	if (!scxfs_verify_magic(bp, hdr3->magic))
		return __this_address;

	if (scxfs_sb_version_hascrc(&mp->m_sb)) {
		if (!uuid_equal(&hdr3->uuid, &mp->m_sb.sb_meta_uuid))
			return __this_address;
		if (be64_to_cpu(hdr3->blkno) != bp->b_bn)
			return __this_address;
		if (!scxfs_log_check_lsn(mp, be64_to_cpu(hdr3->lsn)))
			return __this_address;
	}
	return __scxfs_dir3_data_check(NULL, bp);
}

/*
 * Readahead of the first block of the directory when it is opened is completely
 * oblivious to the format of the directory. Hence we can either get a block
 * format buffer or a data format buffer on readahead.
 */
static void
scxfs_dir3_data_reada_verify(
	struct scxfs_buf		*bp)
{
	struct scxfs_dir2_data_hdr *hdr = bp->b_addr;

	switch (hdr->magic) {
	case cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC):
	case cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC):
		bp->b_ops = &scxfs_dir3_block_buf_ops;
		bp->b_ops->verify_read(bp);
		return;
	case cpu_to_be32(SCXFS_DIR2_DATA_MAGIC):
	case cpu_to_be32(SCXFS_DIR3_DATA_MAGIC):
		bp->b_ops = &scxfs_dir3_data_buf_ops;
		bp->b_ops->verify_read(bp);
		return;
	default:
		scxfs_verifier_error(bp, -EFSCORRUPTED, __this_address);
		break;
	}
}

static void
scxfs_dir3_data_read_verify(
	struct scxfs_buf	*bp)
{
	struct scxfs_mount	*mp = bp->b_mount;
	scxfs_failaddr_t		fa;

	if (scxfs_sb_version_hascrc(&mp->m_sb) &&
	    !scxfs_buf_verify_cksum(bp, SCXFS_DIR3_DATA_CRC_OFF))
		scxfs_verifier_error(bp, -EFSBADCRC, __this_address);
	else {
		fa = scxfs_dir3_data_verify(bp);
		if (fa)
			scxfs_verifier_error(bp, -EFSCORRUPTED, fa);
	}
}

static void
scxfs_dir3_data_write_verify(
	struct scxfs_buf	*bp)
{
	struct scxfs_mount	*mp = bp->b_mount;
	struct scxfs_buf_log_item	*bip = bp->b_log_item;
	struct scxfs_dir3_blk_hdr	*hdr3 = bp->b_addr;
	scxfs_failaddr_t		fa;

	fa = scxfs_dir3_data_verify(bp);
	if (fa) {
		scxfs_verifier_error(bp, -EFSCORRUPTED, fa);
		return;
	}

	if (!scxfs_sb_version_hascrc(&mp->m_sb))
		return;

	if (bip)
		hdr3->lsn = cpu_to_be64(bip->bli_item.li_lsn);

	scxfs_buf_update_cksum(bp, SCXFS_DIR3_DATA_CRC_OFF);
}

const struct scxfs_buf_ops scxfs_dir3_data_buf_ops = {
	.name = "scxfs_dir3_data",
	.magic = { cpu_to_be32(SCXFS_DIR2_DATA_MAGIC),
		   cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) },
	.verify_read = scxfs_dir3_data_read_verify,
	.verify_write = scxfs_dir3_data_write_verify,
	.verify_struct = scxfs_dir3_data_verify,
};

static const struct scxfs_buf_ops scxfs_dir3_data_reada_buf_ops = {
	.name = "scxfs_dir3_data_reada",
	.magic = { cpu_to_be32(SCXFS_DIR2_DATA_MAGIC),
		   cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) },
	.verify_read = scxfs_dir3_data_reada_verify,
	.verify_write = scxfs_dir3_data_write_verify,
};


int
scxfs_dir3_data_read(
	struct scxfs_trans	*tp,
	struct scxfs_inode	*dp,
	scxfs_dablk_t		bno,
	scxfs_daddr_t		mapped_bno,
	struct scxfs_buf		**bpp)
{
	int			err;

	err = scxfs_da_read_buf(tp, dp, bno, mapped_bno, bpp,
				SCXFS_DATA_FORK, &scxfs_dir3_data_buf_ops);
	if (!err && tp && *bpp)
		scxfs_trans_buf_set_type(tp, *bpp, SCXFS_BLFT_DIR_DATA_BUF);
	return err;
}

int
scxfs_dir3_data_readahead(
	struct scxfs_inode	*dp,
	scxfs_dablk_t		bno,
	scxfs_daddr_t		mapped_bno)
{
	return scxfs_da_reada_buf(dp, bno, mapped_bno,
				SCXFS_DATA_FORK, &scxfs_dir3_data_reada_buf_ops);
}

/*
 * Find the bestfree entry that exactly coincides with unused directory space
 * or a verifier error because the bestfree data are bad.
 */
static scxfs_failaddr_t
scxfs_dir2_data_freefind_verify(
	struct scxfs_dir2_data_hdr	*hdr,
	struct scxfs_dir2_data_free	*bf,
	struct scxfs_dir2_data_unused	*dup,
	struct scxfs_dir2_data_free	**bf_ent)
{
	struct scxfs_dir2_data_free	*dfp;
	scxfs_dir2_data_aoff_t		off;
	bool				matched = false;
	bool				seenzero = false;

	*bf_ent = NULL;
	off = (scxfs_dir2_data_aoff_t)((char *)dup - (char *)hdr);

	/*
	 * Validate some consistency in the bestfree table.
	 * Check order, non-overlapping entries, and if we find the
	 * one we're looking for it has to be exact.
	 */
	for (dfp = &bf[0]; dfp < &bf[SCXFS_DIR2_DATA_FD_COUNT]; dfp++) {
		if (!dfp->offset) {
			if (dfp->length)
				return __this_address;
			seenzero = true;
			continue;
		}
		if (seenzero)
			return __this_address;
		if (be16_to_cpu(dfp->offset) == off) {
			matched = true;
			if (dfp->length != dup->length)
				return __this_address;
		} else if (be16_to_cpu(dfp->offset) > off) {
			if (off + be16_to_cpu(dup->length) >
					be16_to_cpu(dfp->offset))
				return __this_address;
		} else {
			if (be16_to_cpu(dfp->offset) +
					be16_to_cpu(dfp->length) > off)
				return __this_address;
		}
		if (!matched &&
		    be16_to_cpu(dfp->length) < be16_to_cpu(dup->length))
			return __this_address;
		if (dfp > &bf[0] &&
		    be16_to_cpu(dfp[-1].length) < be16_to_cpu(dfp[0].length))
			return __this_address;
	}

	/* Looks ok so far; now try to match up with a bestfree entry. */
	*bf_ent = scxfs_dir2_data_freefind(hdr, bf, dup);
	return NULL;
}

/*
 * Given a data block and an unused entry from that block,
 * return the bestfree entry if any that corresponds to it.
 */
scxfs_dir2_data_free_t *
scxfs_dir2_data_freefind(
	struct scxfs_dir2_data_hdr *hdr,		/* data block header */
	struct scxfs_dir2_data_free *bf,		/* bestfree table pointer */
	struct scxfs_dir2_data_unused *dup)	/* unused space */
{
	scxfs_dir2_data_free_t	*dfp;		/* bestfree entry */
	scxfs_dir2_data_aoff_t	off;		/* offset value needed */

	off = (scxfs_dir2_data_aoff_t)((char *)dup - (char *)hdr);

	/*
	 * If this is smaller than the smallest bestfree entry,
	 * it can't be there since they're sorted.
	 */
	if (be16_to_cpu(dup->length) <
	    be16_to_cpu(bf[SCXFS_DIR2_DATA_FD_COUNT - 1].length))
		return NULL;
	/*
	 * Look at the three bestfree entries for our guy.
	 */
	for (dfp = &bf[0]; dfp < &bf[SCXFS_DIR2_DATA_FD_COUNT]; dfp++) {
		if (!dfp->offset)
			return NULL;
		if (be16_to_cpu(dfp->offset) == off)
			return dfp;
	}
	/*
	 * Didn't find it.  This only happens if there are duplicate lengths.
	 */
	return NULL;
}

/*
 * Insert an unused-space entry into the bestfree table.
 */
scxfs_dir2_data_free_t *				/* entry inserted */
scxfs_dir2_data_freeinsert(
	struct scxfs_dir2_data_hdr *hdr,		/* data block pointer */
	struct scxfs_dir2_data_free *dfp,		/* bestfree table pointer */
	struct scxfs_dir2_data_unused *dup,	/* unused space */
	int			*loghead)	/* log the data header (out) */
{
	scxfs_dir2_data_free_t	new;		/* new bestfree entry */

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));

	new.length = dup->length;
	new.offset = cpu_to_be16((char *)dup - (char *)hdr);

	/*
	 * Insert at position 0, 1, or 2; or not at all.
	 */
	if (be16_to_cpu(new.length) > be16_to_cpu(dfp[0].length)) {
		dfp[2] = dfp[1];
		dfp[1] = dfp[0];
		dfp[0] = new;
		*loghead = 1;
		return &dfp[0];
	}
	if (be16_to_cpu(new.length) > be16_to_cpu(dfp[1].length)) {
		dfp[2] = dfp[1];
		dfp[1] = new;
		*loghead = 1;
		return &dfp[1];
	}
	if (be16_to_cpu(new.length) > be16_to_cpu(dfp[2].length)) {
		dfp[2] = new;
		*loghead = 1;
		return &dfp[2];
	}
	return NULL;
}

/*
 * Remove a bestfree entry from the table.
 */
STATIC void
scxfs_dir2_data_freeremove(
	struct scxfs_dir2_data_hdr *hdr,		/* data block header */
	struct scxfs_dir2_data_free *bf,		/* bestfree table pointer */
	struct scxfs_dir2_data_free *dfp,		/* bestfree entry pointer */
	int			*loghead)	/* out: log data header */
{

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));

	/*
	 * It's the first entry, slide the next 2 up.
	 */
	if (dfp == &bf[0]) {
		bf[0] = bf[1];
		bf[1] = bf[2];
	}
	/*
	 * It's the second entry, slide the 3rd entry up.
	 */
	else if (dfp == &bf[1])
		bf[1] = bf[2];
	/*
	 * Must be the last entry.
	 */
	else
		ASSERT(dfp == &bf[2]);
	/*
	 * Clear the 3rd entry, must be zero now.
	 */
	bf[2].length = 0;
	bf[2].offset = 0;
	*loghead = 1;
}

/*
 * Given a data block, reconstruct its bestfree map.
 */
void
scxfs_dir2_data_freescan_int(
	struct scxfs_da_geometry	*geo,
	const struct scxfs_dir_ops *ops,
	struct scxfs_dir2_data_hdr *hdr,
	int			*loghead)
{
	scxfs_dir2_data_entry_t	*dep;		/* active data entry */
	scxfs_dir2_data_unused_t	*dup;		/* unused data entry */
	struct scxfs_dir2_data_free *bf;
	char			*endp;		/* end of block's data */
	char			*p;		/* current entry pointer */

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));

	/*
	 * Start by clearing the table.
	 */
	bf = ops->data_bestfree_p(hdr);
	memset(bf, 0, sizeof(*bf) * SCXFS_DIR2_DATA_FD_COUNT);
	*loghead = 1;
	/*
	 * Set up pointers.
	 */
	p = (char *)ops->data_entry_p(hdr);
	endp = scxfs_dir3_data_endp(geo, hdr);
	/*
	 * Loop over the block's entries.
	 */
	while (p < endp) {
		dup = (scxfs_dir2_data_unused_t *)p;
		/*
		 * If it's a free entry, insert it.
		 */
		if (be16_to_cpu(dup->freetag) == SCXFS_DIR2_DATA_FREE_TAG) {
			ASSERT((char *)dup - (char *)hdr ==
			       be16_to_cpu(*scxfs_dir2_data_unused_tag_p(dup)));
			scxfs_dir2_data_freeinsert(hdr, bf, dup, loghead);
			p += be16_to_cpu(dup->length);
		}
		/*
		 * For active entries, check their tags and skip them.
		 */
		else {
			dep = (scxfs_dir2_data_entry_t *)p;
			ASSERT((char *)dep - (char *)hdr ==
			       be16_to_cpu(*ops->data_entry_tag_p(dep)));
			p += ops->data_entsize(dep->namelen);
		}
	}
}

void
scxfs_dir2_data_freescan(
	struct scxfs_inode	*dp,
	struct scxfs_dir2_data_hdr *hdr,
	int			*loghead)
{
	return scxfs_dir2_data_freescan_int(dp->i_mount->m_dir_geo, dp->d_ops,
			hdr, loghead);
}

/*
 * Initialize a data block at the given block number in the directory.
 * Give back the buffer for the created block.
 */
int						/* error */
scxfs_dir3_data_init(
	scxfs_da_args_t		*args,		/* directory operation args */
	scxfs_dir2_db_t		blkno,		/* logical dir block number */
	struct scxfs_buf		**bpp)		/* output block buffer */
{
	struct scxfs_buf		*bp;		/* block buffer */
	scxfs_dir2_data_hdr_t	*hdr;		/* data block header */
	scxfs_inode_t		*dp;		/* incore directory inode */
	scxfs_dir2_data_unused_t	*dup;		/* unused entry pointer */
	struct scxfs_dir2_data_free *bf;
	int			error;		/* error return value */
	int			i;		/* bestfree index */
	scxfs_mount_t		*mp;		/* filesystem mount point */
	scxfs_trans_t		*tp;		/* transaction pointer */
	int                     t;              /* temp */

	dp = args->dp;
	mp = dp->i_mount;
	tp = args->trans;
	/*
	 * Get the buffer set up for the block.
	 */
	error = scxfs_da_get_buf(tp, dp, scxfs_dir2_db_to_da(args->geo, blkno),
			       -1, &bp, SCXFS_DATA_FORK);
	if (error)
		return error;
	bp->b_ops = &scxfs_dir3_data_buf_ops;
	scxfs_trans_buf_set_type(tp, bp, SCXFS_BLFT_DIR_DATA_BUF);

	/*
	 * Initialize the header.
	 */
	hdr = bp->b_addr;
	if (scxfs_sb_version_hascrc(&mp->m_sb)) {
		struct scxfs_dir3_blk_hdr *hdr3 = bp->b_addr;

		memset(hdr3, 0, sizeof(*hdr3));
		hdr3->magic = cpu_to_be32(SCXFS_DIR3_DATA_MAGIC);
		hdr3->blkno = cpu_to_be64(bp->b_bn);
		hdr3->owner = cpu_to_be64(dp->i_ino);
		uuid_copy(&hdr3->uuid, &mp->m_sb.sb_meta_uuid);

	} else
		hdr->magic = cpu_to_be32(SCXFS_DIR2_DATA_MAGIC);

	bf = dp->d_ops->data_bestfree_p(hdr);
	bf[0].offset = cpu_to_be16(dp->d_ops->data_entry_offset);
	for (i = 1; i < SCXFS_DIR2_DATA_FD_COUNT; i++) {
		bf[i].length = 0;
		bf[i].offset = 0;
	}

	/*
	 * Set up an unused entry for the block's body.
	 */
	dup = dp->d_ops->data_unused_p(hdr);
	dup->freetag = cpu_to_be16(SCXFS_DIR2_DATA_FREE_TAG);

	t = args->geo->blksize - (uint)dp->d_ops->data_entry_offset;
	bf[0].length = cpu_to_be16(t);
	dup->length = cpu_to_be16(t);
	*scxfs_dir2_data_unused_tag_p(dup) = cpu_to_be16((char *)dup - (char *)hdr);
	/*
	 * Log it and return it.
	 */
	scxfs_dir2_data_log_header(args, bp);
	scxfs_dir2_data_log_unused(args, bp, dup);
	*bpp = bp;
	return 0;
}

/*
 * Log an active data entry from the block.
 */
void
scxfs_dir2_data_log_entry(
	struct scxfs_da_args	*args,
	struct scxfs_buf		*bp,
	scxfs_dir2_data_entry_t	*dep)		/* data entry pointer */
{
	struct scxfs_dir2_data_hdr *hdr = bp->b_addr;

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));

	scxfs_trans_log_buf(args->trans, bp, (uint)((char *)dep - (char *)hdr),
		(uint)((char *)(args->dp->d_ops->data_entry_tag_p(dep) + 1) -
		       (char *)hdr - 1));
}

/*
 * Log a data block header.
 */
void
scxfs_dir2_data_log_header(
	struct scxfs_da_args	*args,
	struct scxfs_buf		*bp)
{
#ifdef DEBUG
	struct scxfs_dir2_data_hdr *hdr = bp->b_addr;

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));
#endif

	scxfs_trans_log_buf(args->trans, bp, 0,
			  args->dp->d_ops->data_entry_offset - 1);
}

/*
 * Log a data unused entry.
 */
void
scxfs_dir2_data_log_unused(
	struct scxfs_da_args	*args,
	struct scxfs_buf		*bp,
	scxfs_dir2_data_unused_t	*dup)		/* data unused pointer */
{
	scxfs_dir2_data_hdr_t	*hdr = bp->b_addr;

	ASSERT(hdr->magic == cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) ||
	       hdr->magic == cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC));

	/*
	 * Log the first part of the unused entry.
	 */
	scxfs_trans_log_buf(args->trans, bp, (uint)((char *)dup - (char *)hdr),
		(uint)((char *)&dup->length + sizeof(dup->length) -
		       1 - (char *)hdr));
	/*
	 * Log the end (tag) of the unused entry.
	 */
	scxfs_trans_log_buf(args->trans, bp,
		(uint)((char *)scxfs_dir2_data_unused_tag_p(dup) - (char *)hdr),
		(uint)((char *)scxfs_dir2_data_unused_tag_p(dup) - (char *)hdr +
		       sizeof(scxfs_dir2_data_off_t) - 1));
}

/*
 * Make a byte range in the data block unused.
 * Its current contents are unimportant.
 */
void
scxfs_dir2_data_make_free(
	struct scxfs_da_args	*args,
	struct scxfs_buf		*bp,
	scxfs_dir2_data_aoff_t	offset,		/* starting byte offset */
	scxfs_dir2_data_aoff_t	len,		/* length in bytes */
	int			*needlogp,	/* out: log header */
	int			*needscanp)	/* out: regen bestfree */
{
	scxfs_dir2_data_hdr_t	*hdr;		/* data block pointer */
	scxfs_dir2_data_free_t	*dfp;		/* bestfree pointer */
	char			*endptr;	/* end of data area */
	int			needscan;	/* need to regen bestfree */
	scxfs_dir2_data_unused_t	*newdup;	/* new unused entry */
	scxfs_dir2_data_unused_t	*postdup;	/* unused entry after us */
	scxfs_dir2_data_unused_t	*prevdup;	/* unused entry before us */
	struct scxfs_dir2_data_free *bf;

	hdr = bp->b_addr;

	/*
	 * Figure out where the end of the data area is.
	 */
	endptr = scxfs_dir3_data_endp(args->geo, hdr);
	ASSERT(endptr != NULL);

	/*
	 * If this isn't the start of the block, then back up to
	 * the previous entry and see if it's free.
	 */
	if (offset > args->dp->d_ops->data_entry_offset) {
		__be16			*tagp;	/* tag just before us */

		tagp = (__be16 *)((char *)hdr + offset) - 1;
		prevdup = (scxfs_dir2_data_unused_t *)((char *)hdr + be16_to_cpu(*tagp));
		if (be16_to_cpu(prevdup->freetag) != SCXFS_DIR2_DATA_FREE_TAG)
			prevdup = NULL;
	} else
		prevdup = NULL;
	/*
	 * If this isn't the end of the block, see if the entry after
	 * us is free.
	 */
	if ((char *)hdr + offset + len < endptr) {
		postdup =
			(scxfs_dir2_data_unused_t *)((char *)hdr + offset + len);
		if (be16_to_cpu(postdup->freetag) != SCXFS_DIR2_DATA_FREE_TAG)
			postdup = NULL;
	} else
		postdup = NULL;
	ASSERT(*needscanp == 0);
	needscan = 0;
	/*
	 * Previous and following entries are both free,
	 * merge everything into a single free entry.
	 */
	bf = args->dp->d_ops->data_bestfree_p(hdr);
	if (prevdup && postdup) {
		scxfs_dir2_data_free_t	*dfp2;	/* another bestfree pointer */

		/*
		 * See if prevdup and/or postdup are in bestfree table.
		 */
		dfp = scxfs_dir2_data_freefind(hdr, bf, prevdup);
		dfp2 = scxfs_dir2_data_freefind(hdr, bf, postdup);
		/*
		 * We need a rescan unless there are exactly 2 free entries
		 * namely our two.  Then we know what's happening, otherwise
		 * since the third bestfree is there, there might be more
		 * entries.
		 */
		needscan = (bf[2].length != 0);
		/*
		 * Fix up the new big freespace.
		 */
		be16_add_cpu(&prevdup->length, len + be16_to_cpu(postdup->length));
		*scxfs_dir2_data_unused_tag_p(prevdup) =
			cpu_to_be16((char *)prevdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, prevdup);
		if (!needscan) {
			/*
			 * Has to be the case that entries 0 and 1 are
			 * dfp and dfp2 (don't know which is which), and
			 * entry 2 is empty.
			 * Remove entry 1 first then entry 0.
			 */
			ASSERT(dfp && dfp2);
			if (dfp == &bf[1]) {
				dfp = &bf[0];
				ASSERT(dfp2 == dfp);
				dfp2 = &bf[1];
			}
			scxfs_dir2_data_freeremove(hdr, bf, dfp2, needlogp);
			scxfs_dir2_data_freeremove(hdr, bf, dfp, needlogp);
			/*
			 * Now insert the new entry.
			 */
			dfp = scxfs_dir2_data_freeinsert(hdr, bf, prevdup,
						       needlogp);
			ASSERT(dfp == &bf[0]);
			ASSERT(dfp->length == prevdup->length);
			ASSERT(!dfp[1].length);
			ASSERT(!dfp[2].length);
		}
	}
	/*
	 * The entry before us is free, merge with it.
	 */
	else if (prevdup) {
		dfp = scxfs_dir2_data_freefind(hdr, bf, prevdup);
		be16_add_cpu(&prevdup->length, len);
		*scxfs_dir2_data_unused_tag_p(prevdup) =
			cpu_to_be16((char *)prevdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, prevdup);
		/*
		 * If the previous entry was in the table, the new entry
		 * is longer, so it will be in the table too.  Remove
		 * the old one and add the new one.
		 */
		if (dfp) {
			scxfs_dir2_data_freeremove(hdr, bf, dfp, needlogp);
			scxfs_dir2_data_freeinsert(hdr, bf, prevdup, needlogp);
		}
		/*
		 * Otherwise we need a scan if the new entry is big enough.
		 */
		else {
			needscan = be16_to_cpu(prevdup->length) >
				   be16_to_cpu(bf[2].length);
		}
	}
	/*
	 * The following entry is free, merge with it.
	 */
	else if (postdup) {
		dfp = scxfs_dir2_data_freefind(hdr, bf, postdup);
		newdup = (scxfs_dir2_data_unused_t *)((char *)hdr + offset);
		newdup->freetag = cpu_to_be16(SCXFS_DIR2_DATA_FREE_TAG);
		newdup->length = cpu_to_be16(len + be16_to_cpu(postdup->length));
		*scxfs_dir2_data_unused_tag_p(newdup) =
			cpu_to_be16((char *)newdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup);
		/*
		 * If the following entry was in the table, the new entry
		 * is longer, so it will be in the table too.  Remove
		 * the old one and add the new one.
		 */
		if (dfp) {
			scxfs_dir2_data_freeremove(hdr, bf, dfp, needlogp);
			scxfs_dir2_data_freeinsert(hdr, bf, newdup, needlogp);
		}
		/*
		 * Otherwise we need a scan if the new entry is big enough.
		 */
		else {
			needscan = be16_to_cpu(newdup->length) >
				   be16_to_cpu(bf[2].length);
		}
	}
	/*
	 * Neither neighbor is free.  Make a new entry.
	 */
	else {
		newdup = (scxfs_dir2_data_unused_t *)((char *)hdr + offset);
		newdup->freetag = cpu_to_be16(SCXFS_DIR2_DATA_FREE_TAG);
		newdup->length = cpu_to_be16(len);
		*scxfs_dir2_data_unused_tag_p(newdup) =
			cpu_to_be16((char *)newdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup);
		scxfs_dir2_data_freeinsert(hdr, bf, newdup, needlogp);
	}
	*needscanp = needscan;
}

/* Check our free data for obvious signs of corruption. */
static inline scxfs_failaddr_t
scxfs_dir2_data_check_free(
	struct scxfs_dir2_data_hdr	*hdr,
	struct scxfs_dir2_data_unused	*dup,
	scxfs_dir2_data_aoff_t		offset,
	scxfs_dir2_data_aoff_t		len)
{
	if (hdr->magic != cpu_to_be32(SCXFS_DIR2_DATA_MAGIC) &&
	    hdr->magic != cpu_to_be32(SCXFS_DIR3_DATA_MAGIC) &&
	    hdr->magic != cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC) &&
	    hdr->magic != cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC))
		return __this_address;
	if (be16_to_cpu(dup->freetag) != SCXFS_DIR2_DATA_FREE_TAG)
		return __this_address;
	if (offset < (char *)dup - (char *)hdr)
		return __this_address;
	if (offset + len > (char *)dup + be16_to_cpu(dup->length) - (char *)hdr)
		return __this_address;
	if ((char *)dup - (char *)hdr !=
			be16_to_cpu(*scxfs_dir2_data_unused_tag_p(dup)))
		return __this_address;
	return NULL;
}

/* Sanity-check a new bestfree entry. */
static inline scxfs_failaddr_t
scxfs_dir2_data_check_new_free(
	struct scxfs_dir2_data_hdr	*hdr,
	struct scxfs_dir2_data_free	*dfp,
	struct scxfs_dir2_data_unused	*newdup)
{
	if (dfp == NULL)
		return __this_address;
	if (dfp->length != newdup->length)
		return __this_address;
	if (be16_to_cpu(dfp->offset) != (char *)newdup - (char *)hdr)
		return __this_address;
	return NULL;
}

/*
 * Take a byte range out of an existing unused space and make it un-free.
 */
int
scxfs_dir2_data_use_free(
	struct scxfs_da_args	*args,
	struct scxfs_buf		*bp,
	scxfs_dir2_data_unused_t	*dup,		/* unused entry */
	scxfs_dir2_data_aoff_t	offset,		/* starting offset to use */
	scxfs_dir2_data_aoff_t	len,		/* length to use */
	int			*needlogp,	/* out: need to log header */
	int			*needscanp)	/* out: need regen bestfree */
{
	scxfs_dir2_data_hdr_t	*hdr;		/* data block header */
	scxfs_dir2_data_free_t	*dfp;		/* bestfree pointer */
	scxfs_dir2_data_unused_t	*newdup;	/* new unused entry */
	scxfs_dir2_data_unused_t	*newdup2;	/* another new unused entry */
	struct scxfs_dir2_data_free *bf;
	scxfs_failaddr_t		fa;
	int			matchback;	/* matches end of freespace */
	int			matchfront;	/* matches start of freespace */
	int			needscan;	/* need to regen bestfree */
	int			oldlen;		/* old unused entry's length */

	hdr = bp->b_addr;
	fa = scxfs_dir2_data_check_free(hdr, dup, offset, len);
	if (fa)
		goto corrupt;
	/*
	 * Look up the entry in the bestfree table.
	 */
	oldlen = be16_to_cpu(dup->length);
	bf = args->dp->d_ops->data_bestfree_p(hdr);
	dfp = scxfs_dir2_data_freefind(hdr, bf, dup);
	ASSERT(dfp || oldlen <= be16_to_cpu(bf[2].length));
	/*
	 * Check for alignment with front and back of the entry.
	 */
	matchfront = (char *)dup - (char *)hdr == offset;
	matchback = (char *)dup + oldlen - (char *)hdr == offset + len;
	ASSERT(*needscanp == 0);
	needscan = 0;
	/*
	 * If we matched it exactly we just need to get rid of it from
	 * the bestfree table.
	 */
	if (matchfront && matchback) {
		if (dfp) {
			needscan = (bf[2].offset != 0);
			if (!needscan)
				scxfs_dir2_data_freeremove(hdr, bf, dfp,
							 needlogp);
		}
	}
	/*
	 * We match the first part of the entry.
	 * Make a new entry with the remaining freespace.
	 */
	else if (matchfront) {
		newdup = (scxfs_dir2_data_unused_t *)((char *)hdr + offset + len);
		newdup->freetag = cpu_to_be16(SCXFS_DIR2_DATA_FREE_TAG);
		newdup->length = cpu_to_be16(oldlen - len);
		*scxfs_dir2_data_unused_tag_p(newdup) =
			cpu_to_be16((char *)newdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup);
		/*
		 * If it was in the table, remove it and add the new one.
		 */
		if (dfp) {
			scxfs_dir2_data_freeremove(hdr, bf, dfp, needlogp);
			dfp = scxfs_dir2_data_freeinsert(hdr, bf, newdup,
						       needlogp);
			fa = scxfs_dir2_data_check_new_free(hdr, dfp, newdup);
			if (fa)
				goto corrupt;
			/*
			 * If we got inserted at the last slot,
			 * that means we don't know if there was a better
			 * choice for the last slot, or not.  Rescan.
			 */
			needscan = dfp == &bf[2];
		}
	}
	/*
	 * We match the last part of the entry.
	 * Trim the allocated space off the tail of the entry.
	 */
	else if (matchback) {
		newdup = dup;
		newdup->length = cpu_to_be16(((char *)hdr + offset) - (char *)newdup);
		*scxfs_dir2_data_unused_tag_p(newdup) =
			cpu_to_be16((char *)newdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup);
		/*
		 * If it was in the table, remove it and add the new one.
		 */
		if (dfp) {
			scxfs_dir2_data_freeremove(hdr, bf, dfp, needlogp);
			dfp = scxfs_dir2_data_freeinsert(hdr, bf, newdup,
						       needlogp);
			fa = scxfs_dir2_data_check_new_free(hdr, dfp, newdup);
			if (fa)
				goto corrupt;
			/*
			 * If we got inserted at the last slot,
			 * that means we don't know if there was a better
			 * choice for the last slot, or not.  Rescan.
			 */
			needscan = dfp == &bf[2];
		}
	}
	/*
	 * Poking out the middle of an entry.
	 * Make two new entries.
	 */
	else {
		newdup = dup;
		newdup->length = cpu_to_be16(((char *)hdr + offset) - (char *)newdup);
		*scxfs_dir2_data_unused_tag_p(newdup) =
			cpu_to_be16((char *)newdup - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup);
		newdup2 = (scxfs_dir2_data_unused_t *)((char *)hdr + offset + len);
		newdup2->freetag = cpu_to_be16(SCXFS_DIR2_DATA_FREE_TAG);
		newdup2->length = cpu_to_be16(oldlen - len - be16_to_cpu(newdup->length));
		*scxfs_dir2_data_unused_tag_p(newdup2) =
			cpu_to_be16((char *)newdup2 - (char *)hdr);
		scxfs_dir2_data_log_unused(args, bp, newdup2);
		/*
		 * If the old entry was in the table, we need to scan
		 * if the 3rd entry was valid, since these entries
		 * are smaller than the old one.
		 * If we don't need to scan that means there were 1 or 2
		 * entries in the table, and removing the old and adding
		 * the 2 new will work.
		 */
		if (dfp) {
			needscan = (bf[2].length != 0);
			if (!needscan) {
				scxfs_dir2_data_freeremove(hdr, bf, dfp,
							 needlogp);
				scxfs_dir2_data_freeinsert(hdr, bf, newdup,
							 needlogp);
				scxfs_dir2_data_freeinsert(hdr, bf, newdup2,
							 needlogp);
			}
		}
	}
	*needscanp = needscan;
	return 0;
corrupt:
	scxfs_corruption_error(__func__, SCXFS_ERRLEVEL_LOW, args->dp->i_mount,
			hdr, sizeof(*hdr), __FILE__, __LINE__, fa);
	return -EFSCORRUPTED;
}

/* Find the end of the entry data in a data/block format dir block. */
void *
scxfs_dir3_data_endp(
	struct scxfs_da_geometry		*geo,
	struct scxfs_dir2_data_hdr	*hdr)
{
	switch (hdr->magic) {
	case cpu_to_be32(SCXFS_DIR3_BLOCK_MAGIC):
	case cpu_to_be32(SCXFS_DIR2_BLOCK_MAGIC):
		return scxfs_dir2_block_leaf_p(scxfs_dir2_block_tail_p(geo, hdr));
	case cpu_to_be32(SCXFS_DIR3_DATA_MAGIC):
	case cpu_to_be32(SCXFS_DIR2_DATA_MAGIC):
		return (char *)hdr + geo->blksize;
	default:
		return NULL;
	}
}