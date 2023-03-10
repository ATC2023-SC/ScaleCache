// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include <linux/mount.h>
#include <linux/fsmap.h>
#include "scxfs.h"
#include "scxfs_fs.h"
#include "scxfs_shared.h"
#include "scxfs_format.h"
#include "scxfs_log_format.h"
#include "scxfs_trans_resv.h"
#include "scxfs_mount.h"
#include "scxfs_inode.h"
#include "scxfs_iwalk.h"
#include "scxfs_itable.h"
#include "scxfs_fsops.h"
#include "scxfs_rtalloc.h"
#include "scxfs_attr.h"
#include "scxfs_ioctl.h"
#include "scxfs_ioctl32.h"
#include "scxfs_trace.h"
#include "scxfs_sb.h"

#define  _NATIVE_IOC(cmd, type) \
	  _IOC(_IOC_DIR(cmd), _IOC_TYPE(cmd), _IOC_NR(cmd), sizeof(type))

#ifdef BROKEN_X86_ALIGNMENT
STATIC int
scxfs_compat_flock64_copyin(
	scxfs_flock64_t		*bf,
	compat_scxfs_flock64_t	__user *arg32)
{
	if (get_user(bf->l_type,	&arg32->l_type) ||
	    get_user(bf->l_whence,	&arg32->l_whence) ||
	    get_user(bf->l_start,	&arg32->l_start) ||
	    get_user(bf->l_len,		&arg32->l_len) ||
	    get_user(bf->l_sysid,	&arg32->l_sysid) ||
	    get_user(bf->l_pid,		&arg32->l_pid) ||
	    copy_from_user(bf->l_pad,	&arg32->l_pad,	4*sizeof(u32)))
		return -EFAULT;
	return 0;
}

STATIC int
scxfs_compat_ioc_fsgeometry_v1(
	struct scxfs_mount	  *mp,
	compat_scxfs_fsop_geom_v1_t __user *arg32)
{
	struct scxfs_fsop_geom	  fsgeo;

	scxfs_fs_geometry(&mp->m_sb, &fsgeo, 3);
	/* The 32-bit variant simply has some padding at the end */
	if (copy_to_user(arg32, &fsgeo, sizeof(struct compat_scxfs_fsop_geom_v1)))
		return -EFAULT;
	return 0;
}

STATIC int
scxfs_compat_growfs_data_copyin(
	struct scxfs_growfs_data	 *in,
	compat_scxfs_growfs_data_t __user *arg32)
{
	if (get_user(in->newblocks, &arg32->newblocks) ||
	    get_user(in->imaxpct,   &arg32->imaxpct))
		return -EFAULT;
	return 0;
}

STATIC int
scxfs_compat_growfs_rt_copyin(
	struct scxfs_growfs_rt	 *in,
	compat_scxfs_growfs_rt_t	__user *arg32)
{
	if (get_user(in->newblocks, &arg32->newblocks) ||
	    get_user(in->extsize,   &arg32->extsize))
		return -EFAULT;
	return 0;
}

STATIC int
scxfs_fsinumbers_fmt_compat(
	struct scxfs_ibulk		*breq,
	const struct scxfs_inumbers	*ig)
{
	struct compat_scxfs_inogrp __user	*p32 = breq->ubuffer;
	struct scxfs_inogrp		ig1;
	struct scxfs_inogrp		*igrp = &ig1;

	scxfs_inumbers_to_inogrp(&ig1, ig);

	if (put_user(igrp->xi_startino,   &p32->xi_startino) ||
	    put_user(igrp->xi_alloccount, &p32->xi_alloccount) ||
	    put_user(igrp->xi_allocmask,  &p32->xi_allocmask))
		return -EFAULT;

	return scxfs_ibulk_advance(breq, sizeof(struct compat_scxfs_inogrp));
}

#else
#define scxfs_fsinumbers_fmt_compat scxfs_fsinumbers_fmt
#endif	/* BROKEN_X86_ALIGNMENT */

STATIC int
scxfs_ioctl32_bstime_copyin(
	scxfs_bstime_t		*bstime,
	compat_scxfs_bstime_t	__user *bstime32)
{
	compat_time_t		sec32;	/* tv_sec differs on 64 vs. 32 */

	if (get_user(sec32,		&bstime32->tv_sec)	||
	    get_user(bstime->tv_nsec,	&bstime32->tv_nsec))
		return -EFAULT;
	bstime->tv_sec = sec32;
	return 0;
}

/*
 * struct scxfs_bstat has differing alignment on intel, & bstime_t sizes
 * everywhere
 */
STATIC int
scxfs_ioctl32_bstat_copyin(
	struct scxfs_bstat		*bstat,
	struct compat_scxfs_bstat	__user	*bstat32)
{
	if (get_user(bstat->bs_ino,	&bstat32->bs_ino)	||
	    get_user(bstat->bs_mode,	&bstat32->bs_mode)	||
	    get_user(bstat->bs_nlink,	&bstat32->bs_nlink)	||
	    get_user(bstat->bs_uid,	&bstat32->bs_uid)	||
	    get_user(bstat->bs_gid,	&bstat32->bs_gid)	||
	    get_user(bstat->bs_rdev,	&bstat32->bs_rdev)	||
	    get_user(bstat->bs_blksize,	&bstat32->bs_blksize)	||
	    get_user(bstat->bs_size,	&bstat32->bs_size)	||
	    scxfs_ioctl32_bstime_copyin(&bstat->bs_atime, &bstat32->bs_atime) ||
	    scxfs_ioctl32_bstime_copyin(&bstat->bs_mtime, &bstat32->bs_mtime) ||
	    scxfs_ioctl32_bstime_copyin(&bstat->bs_ctime, &bstat32->bs_ctime) ||
	    get_user(bstat->bs_blocks,	&bstat32->bs_size)	||
	    get_user(bstat->bs_xflags,	&bstat32->bs_size)	||
	    get_user(bstat->bs_extsize,	&bstat32->bs_extsize)	||
	    get_user(bstat->bs_extents,	&bstat32->bs_extents)	||
	    get_user(bstat->bs_gen,	&bstat32->bs_gen)	||
	    get_user(bstat->bs_projid_lo, &bstat32->bs_projid_lo) ||
	    get_user(bstat->bs_projid_hi, &bstat32->bs_projid_hi) ||
	    get_user(bstat->bs_forkoff,	&bstat32->bs_forkoff)	||
	    get_user(bstat->bs_dmevmask, &bstat32->bs_dmevmask)	||
	    get_user(bstat->bs_dmstate,	&bstat32->bs_dmstate)	||
	    get_user(bstat->bs_aextents, &bstat32->bs_aextents))
		return -EFAULT;
	return 0;
}

/* SCXFS_IOC_FSBULKSTAT and friends */

STATIC int
scxfs_bstime_store_compat(
	compat_scxfs_bstime_t	__user *p32,
	const scxfs_bstime_t	*p)
{
	__s32			sec32;

	sec32 = p->tv_sec;
	if (put_user(sec32, &p32->tv_sec) ||
	    put_user(p->tv_nsec, &p32->tv_nsec))
		return -EFAULT;
	return 0;
}

/* Return 0 on success or positive error (to scxfs_bulkstat()) */
STATIC int
scxfs_fsbulkstat_one_fmt_compat(
	struct scxfs_ibulk		*breq,
	const struct scxfs_bulkstat	*bstat)
{
	struct compat_scxfs_bstat	__user	*p32 = breq->ubuffer;
	struct scxfs_bstat		bs1;
	struct scxfs_bstat		*buffer = &bs1;

	scxfs_bulkstat_to_bstat(breq->mp, &bs1, bstat);

	if (put_user(buffer->bs_ino,	  &p32->bs_ino)		||
	    put_user(buffer->bs_mode,	  &p32->bs_mode)	||
	    put_user(buffer->bs_nlink,	  &p32->bs_nlink)	||
	    put_user(buffer->bs_uid,	  &p32->bs_uid)		||
	    put_user(buffer->bs_gid,	  &p32->bs_gid)		||
	    put_user(buffer->bs_rdev,	  &p32->bs_rdev)	||
	    put_user(buffer->bs_blksize,  &p32->bs_blksize)	||
	    put_user(buffer->bs_size,	  &p32->bs_size)	||
	    scxfs_bstime_store_compat(&p32->bs_atime, &buffer->bs_atime) ||
	    scxfs_bstime_store_compat(&p32->bs_mtime, &buffer->bs_mtime) ||
	    scxfs_bstime_store_compat(&p32->bs_ctime, &buffer->bs_ctime) ||
	    put_user(buffer->bs_blocks,	  &p32->bs_blocks)	||
	    put_user(buffer->bs_xflags,	  &p32->bs_xflags)	||
	    put_user(buffer->bs_extsize,  &p32->bs_extsize)	||
	    put_user(buffer->bs_extents,  &p32->bs_extents)	||
	    put_user(buffer->bs_gen,	  &p32->bs_gen)		||
	    put_user(buffer->bs_projid,	  &p32->bs_projid)	||
	    put_user(buffer->bs_projid_hi,	&p32->bs_projid_hi)	||
	    put_user(buffer->bs_forkoff,  &p32->bs_forkoff)	||
	    put_user(buffer->bs_dmevmask, &p32->bs_dmevmask)	||
	    put_user(buffer->bs_dmstate,  &p32->bs_dmstate)	||
	    put_user(buffer->bs_aextents, &p32->bs_aextents))
		return -EFAULT;

	return scxfs_ibulk_advance(breq, sizeof(struct compat_scxfs_bstat));
}

/* copied from scxfs_ioctl.c */
STATIC int
scxfs_compat_ioc_fsbulkstat(
	scxfs_mount_t		  *mp,
	unsigned int		  cmd,
	struct compat_scxfs_fsop_bulkreq __user *p32)
{
	u32			addr;
	struct scxfs_fsop_bulkreq	bulkreq;
	struct scxfs_ibulk	breq = {
		.mp		= mp,
		.ocount		= 0,
	};
	scxfs_ino_t		lastino;
	int			error;

	/*
	 * Output structure handling functions.  Depending on the command,
	 * either the scxfs_bstat and scxfs_inogrp structures are written out
	 * to userpace memory via bulkreq.ubuffer.  Normally the compat
	 * functions and structure size are the correct ones to use ...
	 */
	inumbers_fmt_pf		inumbers_func = scxfs_fsinumbers_fmt_compat;
	bulkstat_one_fmt_pf	bs_one_func = scxfs_fsbulkstat_one_fmt_compat;

#ifdef CONFIG_X86_X32
	if (in_x32_syscall()) {
		/*
		 * ... but on x32 the input scxfs_fsop_bulkreq has pointers
		 * which must be handled in the "compat" (32-bit) way, while
		 * the scxfs_bstat and scxfs_inogrp structures follow native 64-
		 * bit layout convention.  So adjust accordingly, otherwise
		 * the data written out in compat layout will not match what
		 * x32 userspace expects.
		 */
		inumbers_func = scxfs_fsinumbers_fmt;
		bs_one_func = scxfs_fsbulkstat_one_fmt;
	}
#endif

	/* done = 1 if there are more stats to get and if bulkstat */
	/* should be called again (unused here, but used in dmapi) */

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (SCXFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	if (get_user(addr, &p32->lastip))
		return -EFAULT;
	bulkreq.lastip = compat_ptr(addr);
	if (get_user(bulkreq.icount, &p32->icount) ||
	    get_user(addr, &p32->ubuffer))
		return -EFAULT;
	bulkreq.ubuffer = compat_ptr(addr);
	if (get_user(addr, &p32->ocount))
		return -EFAULT;
	bulkreq.ocount = compat_ptr(addr);

	if (copy_from_user(&lastino, bulkreq.lastip, sizeof(__s64)))
		return -EFAULT;

	if (bulkreq.icount <= 0)
		return -EINVAL;

	if (bulkreq.ubuffer == NULL)
		return -EINVAL;

	breq.ubuffer = bulkreq.ubuffer;
	breq.icount = bulkreq.icount;

	/*
	 * FSBULKSTAT_SINGLE expects that *lastip contains the inode number
	 * that we want to stat.  However, FSINUMBERS and FSBULKSTAT expect
	 * that *lastip contains either zero or the number of the last inode to
	 * be examined by the previous call and return results starting with
	 * the next inode after that.  The new bulk request back end functions
	 * take the inode to start with, so we have to compute the startino
	 * parameter from lastino to maintain correct function.  lastino == 0
	 * is a special case because it has traditionally meant "first inode
	 * in filesystem".
	 */
	if (cmd == SCXFS_IOC_FSINUMBERS_32) {
		breq.startino = lastino ? lastino + 1 : 0;
		error = scxfs_inumbers(&breq, inumbers_func);
		lastino = breq.startino - 1;
	} else if (cmd == SCXFS_IOC_FSBULKSTAT_SINGLE_32) {
		breq.startino = lastino;
		breq.icount = 1;
		error = scxfs_bulkstat_one(&breq, bs_one_func);
		lastino = breq.startino;
	} else if (cmd == SCXFS_IOC_FSBULKSTAT_32) {
		breq.startino = lastino ? lastino + 1 : 0;
		error = scxfs_bulkstat(&breq, bs_one_func);
		lastino = breq.startino - 1;
	} else {
		error = -EINVAL;
	}
	if (error)
		return error;

	if (bulkreq.lastip != NULL &&
	    copy_to_user(bulkreq.lastip, &lastino, sizeof(scxfs_ino_t)))
		return -EFAULT;

	if (bulkreq.ocount != NULL &&
	    copy_to_user(bulkreq.ocount, &breq.ocount, sizeof(__s32)))
		return -EFAULT;

	return 0;
}

STATIC int
scxfs_compat_handlereq_copyin(
	scxfs_fsop_handlereq_t		*hreq,
	compat_scxfs_fsop_handlereq_t	__user *arg32)
{
	compat_scxfs_fsop_handlereq_t	hreq32;

	if (copy_from_user(&hreq32, arg32, sizeof(compat_scxfs_fsop_handlereq_t)))
		return -EFAULT;

	hreq->fd = hreq32.fd;
	hreq->path = compat_ptr(hreq32.path);
	hreq->oflags = hreq32.oflags;
	hreq->ihandle = compat_ptr(hreq32.ihandle);
	hreq->ihandlen = hreq32.ihandlen;
	hreq->ohandle = compat_ptr(hreq32.ohandle);
	hreq->ohandlen = compat_ptr(hreq32.ohandlen);

	return 0;
}

STATIC struct dentry *
scxfs_compat_handlereq_to_dentry(
	struct file		*parfilp,
	compat_scxfs_fsop_handlereq_t *hreq)
{
	return scxfs_handle_to_dentry(parfilp,
			compat_ptr(hreq->ihandle), hreq->ihandlen);
}

STATIC int
scxfs_compat_attrlist_by_handle(
	struct file		*parfilp,
	void			__user *arg)
{
	int			error;
	attrlist_cursor_kern_t	*cursor;
	compat_scxfs_fsop_attrlist_handlereq_t __user *p = arg;
	compat_scxfs_fsop_attrlist_handlereq_t al_hreq;
	struct dentry		*dentry;
	char			*kbuf;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&al_hreq, arg,
			   sizeof(compat_scxfs_fsop_attrlist_handlereq_t)))
		return -EFAULT;
	if (al_hreq.buflen < sizeof(struct attrlist) ||
	    al_hreq.buflen > SCXFS_XATTR_LIST_MAX)
		return -EINVAL;

	/*
	 * Reject flags, only allow namespaces.
	 */
	if (al_hreq.flags & ~(ATTR_ROOT | ATTR_SECURE))
		return -EINVAL;

	dentry = scxfs_compat_handlereq_to_dentry(parfilp, &al_hreq.hreq);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	error = -ENOMEM;
	kbuf = kmem_zalloc_large(al_hreq.buflen, 0);
	if (!kbuf)
		goto out_dput;

	cursor = (attrlist_cursor_kern_t *)&al_hreq.pos;
	error = scxfs_attr_list(SCXFS_I(d_inode(dentry)), kbuf, al_hreq.buflen,
					al_hreq.flags, cursor);
	if (error)
		goto out_kfree;

	if (copy_to_user(&p->pos, cursor, sizeof(attrlist_cursor_kern_t))) {
		error = -EFAULT;
		goto out_kfree;
	}

	if (copy_to_user(compat_ptr(al_hreq.buffer), kbuf, al_hreq.buflen))
		error = -EFAULT;

out_kfree:
	kmem_free(kbuf);
out_dput:
	dput(dentry);
	return error;
}

STATIC int
scxfs_compat_attrmulti_by_handle(
	struct file				*parfilp,
	void					__user *arg)
{
	int					error;
	compat_scxfs_attr_multiop_t		*ops;
	compat_scxfs_fsop_attrmulti_handlereq_t	am_hreq;
	struct dentry				*dentry;
	unsigned int				i, size;
	unsigned char				*attr_name;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&am_hreq, arg,
			   sizeof(compat_scxfs_fsop_attrmulti_handlereq_t)))
		return -EFAULT;

	/* overflow check */
	if (am_hreq.opcount >= INT_MAX / sizeof(compat_scxfs_attr_multiop_t))
		return -E2BIG;

	dentry = scxfs_compat_handlereq_to_dentry(parfilp, &am_hreq.hreq);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	error = -E2BIG;
	size = am_hreq.opcount * sizeof(compat_scxfs_attr_multiop_t);
	if (!size || size > 16 * PAGE_SIZE)
		goto out_dput;

	ops = memdup_user(compat_ptr(am_hreq.ops), size);
	if (IS_ERR(ops)) {
		error = PTR_ERR(ops);
		goto out_dput;
	}

	error = -ENOMEM;
	attr_name = kmalloc(MAXNAMELEN, GFP_KERNEL);
	if (!attr_name)
		goto out_kfree_ops;

	error = 0;
	for (i = 0; i < am_hreq.opcount; i++) {
		ops[i].am_flags &= ~ATTR_KERNEL_FLAGS;

		ops[i].am_error = strncpy_from_user((char *)attr_name,
				compat_ptr(ops[i].am_attrname),
				MAXNAMELEN);
		if (ops[i].am_error == 0 || ops[i].am_error == MAXNAMELEN)
			error = -ERANGE;
		if (ops[i].am_error < 0)
			break;

		switch (ops[i].am_opcode) {
		case ATTR_OP_GET:
			ops[i].am_error = scxfs_attrmulti_attr_get(
					d_inode(dentry), attr_name,
					compat_ptr(ops[i].am_attrvalue),
					&ops[i].am_length, ops[i].am_flags);
			break;
		case ATTR_OP_SET:
			ops[i].am_error = mnt_want_write_file(parfilp);
			if (ops[i].am_error)
				break;
			ops[i].am_error = scxfs_attrmulti_attr_set(
					d_inode(dentry), attr_name,
					compat_ptr(ops[i].am_attrvalue),
					ops[i].am_length, ops[i].am_flags);
			mnt_drop_write_file(parfilp);
			break;
		case ATTR_OP_REMOVE:
			ops[i].am_error = mnt_want_write_file(parfilp);
			if (ops[i].am_error)
				break;
			ops[i].am_error = scxfs_attrmulti_attr_remove(
					d_inode(dentry), attr_name,
					ops[i].am_flags);
			mnt_drop_write_file(parfilp);
			break;
		default:
			ops[i].am_error = -EINVAL;
		}
	}

	if (copy_to_user(compat_ptr(am_hreq.ops), ops, size))
		error = -EFAULT;

	kfree(attr_name);
 out_kfree_ops:
	kfree(ops);
 out_dput:
	dput(dentry);
	return error;
}

STATIC int
scxfs_compat_fssetdm_by_handle(
	struct file		*parfilp,
	void			__user *arg)
{
	int			error;
	struct fsdmidata	fsd;
	compat_scxfs_fsop_setdm_handlereq_t dmhreq;
	struct dentry		*dentry;

	if (!capable(CAP_MKNOD))
		return -EPERM;
	if (copy_from_user(&dmhreq, arg,
			   sizeof(compat_scxfs_fsop_setdm_handlereq_t)))
		return -EFAULT;

	dentry = scxfs_compat_handlereq_to_dentry(parfilp, &dmhreq.hreq);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	if (IS_IMMUTABLE(d_inode(dentry)) || IS_APPEND(d_inode(dentry))) {
		error = -EPERM;
		goto out;
	}

	if (copy_from_user(&fsd, compat_ptr(dmhreq.data), sizeof(fsd))) {
		error = -EFAULT;
		goto out;
	}

	error = scxfs_set_dmattrs(SCXFS_I(d_inode(dentry)), fsd.fsd_dmevmask,
				 fsd.fsd_dmstate);

out:
	dput(dentry);
	return error;
}

long
scxfs_file_compat_ioctl(
	struct file		*filp,
	unsigned		cmd,
	unsigned long		p)
{
	struct inode		*inode = file_inode(filp);
	struct scxfs_inode	*ip = SCXFS_I(inode);
	struct scxfs_mount	*mp = ip->i_mount;
	void			__user *arg = compat_ptr(p);
	int			error;

	trace_scxfs_file_compat_ioctl(ip);

	switch (cmd) {
#if defined(BROKEN_X86_ALIGNMENT)
	case SCXFS_IOC_ALLOCSP_32:
	case SCXFS_IOC_FREESP_32:
	case SCXFS_IOC_ALLOCSP64_32:
	case SCXFS_IOC_FREESP64_32:
	case SCXFS_IOC_RESVSP_32:
	case SCXFS_IOC_UNRESVSP_32:
	case SCXFS_IOC_RESVSP64_32:
	case SCXFS_IOC_UNRESVSP64_32:
	case SCXFS_IOC_ZERO_RANGE_32: {
		struct scxfs_flock64	bf;

		if (scxfs_compat_flock64_copyin(&bf, arg))
			return -EFAULT;
		cmd = _NATIVE_IOC(cmd, struct scxfs_flock64);
		return scxfs_ioc_space(filp, cmd, &bf);
	}
	case SCXFS_IOC_FSGEOMETRY_V1_32:
		return scxfs_compat_ioc_fsgeometry_v1(mp, arg);
	case SCXFS_IOC_FSGROWFSDATA_32: {
		struct scxfs_growfs_data	in;

		if (scxfs_compat_growfs_data_copyin(&in, arg))
			return -EFAULT;
		error = mnt_want_write_file(filp);
		if (error)
			return error;
		error = scxfs_growfs_data(mp, &in);
		mnt_drop_write_file(filp);
		return error;
	}
	case SCXFS_IOC_FSGROWFSRT_32: {
		struct scxfs_growfs_rt	in;

		if (scxfs_compat_growfs_rt_copyin(&in, arg))
			return -EFAULT;
		error = mnt_want_write_file(filp);
		if (error)
			return error;
		error = scxfs_growfs_rt(mp, &in);
		mnt_drop_write_file(filp);
		return error;
	}
#endif
	/* long changes size, but scxfs only copiese out 32 bits */
	case SCXFS_IOC_GETXFLAGS_32:
	case SCXFS_IOC_SETXFLAGS_32:
	case SCXFS_IOC_GETVERSION_32:
		cmd = _NATIVE_IOC(cmd, long);
		return scxfs_file_ioctl(filp, cmd, p);
	case SCXFS_IOC_SWAPEXT_32: {
		struct scxfs_swapext	  sxp;
		struct compat_scxfs_swapext __user *sxu = arg;

		/* Bulk copy in up to the sx_stat field, then copy bstat */
		if (copy_from_user(&sxp, sxu,
				   offsetof(struct scxfs_swapext, sx_stat)) ||
		    scxfs_ioctl32_bstat_copyin(&sxp.sx_stat, &sxu->sx_stat))
			return -EFAULT;
		error = mnt_want_write_file(filp);
		if (error)
			return error;
		error = scxfs_ioc_swapext(&sxp);
		mnt_drop_write_file(filp);
		return error;
	}
	case SCXFS_IOC_FSBULKSTAT_32:
	case SCXFS_IOC_FSBULKSTAT_SINGLE_32:
	case SCXFS_IOC_FSINUMBERS_32:
		return scxfs_compat_ioc_fsbulkstat(mp, cmd, arg);
	case SCXFS_IOC_FD_TO_HANDLE_32:
	case SCXFS_IOC_PATH_TO_HANDLE_32:
	case SCXFS_IOC_PATH_TO_FSHANDLE_32: {
		struct scxfs_fsop_handlereq	hreq;

		if (scxfs_compat_handlereq_copyin(&hreq, arg))
			return -EFAULT;
		cmd = _NATIVE_IOC(cmd, struct scxfs_fsop_handlereq);
		return scxfs_find_handle(cmd, &hreq);
	}
	case SCXFS_IOC_OPEN_BY_HANDLE_32: {
		struct scxfs_fsop_handlereq	hreq;

		if (scxfs_compat_handlereq_copyin(&hreq, arg))
			return -EFAULT;
		return scxfs_open_by_handle(filp, &hreq);
	}
	case SCXFS_IOC_READLINK_BY_HANDLE_32: {
		struct scxfs_fsop_handlereq	hreq;

		if (scxfs_compat_handlereq_copyin(&hreq, arg))
			return -EFAULT;
		return scxfs_readlink_by_handle(filp, &hreq);
	}
	case SCXFS_IOC_ATTRLIST_BY_HANDLE_32:
		return scxfs_compat_attrlist_by_handle(filp, arg);
	case SCXFS_IOC_ATTRMULTI_BY_HANDLE_32:
		return scxfs_compat_attrmulti_by_handle(filp, arg);
	case SCXFS_IOC_FSSETDM_BY_HANDLE_32:
		return scxfs_compat_fssetdm_by_handle(filp, arg);
	default:
		/* try the native version */
		return scxfs_file_ioctl(filp, cmd, (unsigned long)arg);
	}
}
