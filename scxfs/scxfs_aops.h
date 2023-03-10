// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __SCXFS_AOPS_H__
#define __SCXFS_AOPS_H__

extern struct bio_set scxfs_ioend_bioset;

/*
 * Structure for buffered I/O completions.
 */
struct scxfs_ioend {
	struct list_head	io_list;	/* next ioend in chain */
	int			io_fork;	/* inode fork written back */
	scxfs_exntst_t		io_state;	/* extent state */
	struct inode		*io_inode;	/* file being written to */
	size_t			io_size;	/* size of the extent */
	scxfs_off_t		io_offset;	/* offset in the file */
	struct scxfs_trans	*io_append_trans;/* xact. for size update */
	struct bio		*io_bio;	/* bio being built */
	struct bio		io_inline_bio;	/* MUST BE LAST! */
};

extern const struct address_space_operations scxfs_address_space_operations;
extern const struct address_space_operations scxfs_dax_aops;

int	scxfs_setfilesize(struct scxfs_inode *ip, scxfs_off_t offset, size_t size);

extern struct block_device *scxfs_find_bdev_for_inode(struct inode *);
extern struct dax_device *scxfs_find_daxdev_for_inode(struct inode *);

#endif /* __SCXFS_AOPS_H__ */
