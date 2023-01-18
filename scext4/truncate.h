// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/scext4/truncate.h
 *
 * Common inline functions needed for truncate support
 */

/*
 * Truncate blocks that were not used by write. We have to truncate the
 * pagecache as well so that corresponding buffers get properly unmapped.
 */
static inline void scext4_truncate_failed_write(struct inode *inode)
{
	/*
	 * We don't need to call scext4_break_layouts() because the blocks we
	 * are truncating were never visible to userspace.
	 */
	down_write(&SCEXT4_I(inode)->i_mmap_sem);
	truncate_inode_pages(inode->i_mapping, inode->i_size);
	scext4_truncate(inode);
	up_write(&SCEXT4_I(inode)->i_mmap_sem);
}

/*
 * Work out how many blocks we need to proceed with the next chunk of a
 * truncate transaction.
 */
static inline unsigned long scext4_blocks_for_truncate(struct inode *inode)
{
	scext4_lblk_t needed;

	needed = inode->i_blocks >> (inode->i_sb->s_blocksize_bits - 9);

	/* Give ourselves just enough room to cope with inodes in which
	 * i_blocks is corrupt: we've seen disk corruptions in the past
	 * which resulted in random data in an inode which looked enough
	 * like a regular file for scext4 to try to delete it.  Things
	 * will go a bit crazy if that happens, but at least we should
	 * try not to panic the whole kernel. */
	if (needed < 2)
		needed = 2;

	/* But we need to bound the transaction so we don't overflow the
	 * journal. */
	if (needed > SCEXT4_MAX_TRANS_DATA)
		needed = SCEXT4_MAX_TRANS_DATA;

	return SCEXT4_DATA_TRANS_BLOCKS(inode->i_sb) + needed;
}
