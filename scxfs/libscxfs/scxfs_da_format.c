// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000,2002,2005 Silicon Graphics, Inc.
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

/*
 * Shortform directory ops
 */
static int
scxfs_dir2_sf_entsize(
	struct scxfs_dir2_sf_hdr	*hdr,
	int			len)
{
	int count = sizeof(struct scxfs_dir2_sf_entry);	/* namelen + offset */

	count += len;					/* name */
	count += hdr->i8count ? SCXFS_INO64_SIZE : SCXFS_INO32_SIZE; /* ino # */
	return count;
}

static int
scxfs_dir3_sf_entsize(
	struct scxfs_dir2_sf_hdr	*hdr,
	int			len)
{
	return scxfs_dir2_sf_entsize(hdr, len) + sizeof(uint8_t);
}

static struct scxfs_dir2_sf_entry *
scxfs_dir2_sf_nextentry(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep)
{
	return (struct scxfs_dir2_sf_entry *)
		((char *)sfep + scxfs_dir2_sf_entsize(hdr, sfep->namelen));
}

static struct scxfs_dir2_sf_entry *
scxfs_dir3_sf_nextentry(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep)
{
	return (struct scxfs_dir2_sf_entry *)
		((char *)sfep + scxfs_dir3_sf_entsize(hdr, sfep->namelen));
}


/*
 * For filetype enabled shortform directories, the file type field is stored at
 * the end of the name.  Because it's only a single byte, endian conversion is
 * not necessary. For non-filetype enable directories, the type is always
 * unknown and we never store the value.
 */
static uint8_t
scxfs_dir2_sfe_get_ftype(
	struct scxfs_dir2_sf_entry *sfep)
{
	return SCXFS_DIR3_FT_UNKNOWN;
}

static void
scxfs_dir2_sfe_put_ftype(
	struct scxfs_dir2_sf_entry *sfep,
	uint8_t			ftype)
{
	ASSERT(ftype < SCXFS_DIR3_FT_MAX);
}

static uint8_t
scxfs_dir3_sfe_get_ftype(
	struct scxfs_dir2_sf_entry *sfep)
{
	uint8_t		ftype;

	ftype = sfep->name[sfep->namelen];
	if (ftype >= SCXFS_DIR3_FT_MAX)
		return SCXFS_DIR3_FT_UNKNOWN;
	return ftype;
}

static void
scxfs_dir3_sfe_put_ftype(
	struct scxfs_dir2_sf_entry *sfep,
	uint8_t			ftype)
{
	ASSERT(ftype < SCXFS_DIR3_FT_MAX);

	sfep->name[sfep->namelen] = ftype;
}

/*
 * Inode numbers in short-form directories can come in two versions,
 * either 4 bytes or 8 bytes wide.  These helpers deal with the
 * two forms transparently by looking at the headers i8count field.
 *
 * For 64-bit inode number the most significant byte must be zero.
 */
static scxfs_ino_t
scxfs_dir2_sf_get_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	uint8_t			*from)
{
	if (hdr->i8count)
		return get_unaligned_be64(from) & 0x00ffffffffffffffULL;
	else
		return get_unaligned_be32(from);
}

static void
scxfs_dir2_sf_put_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	uint8_t			*to,
	scxfs_ino_t		ino)
{
	ASSERT((ino & 0xff00000000000000ULL) == 0);

	if (hdr->i8count)
		put_unaligned_be64(ino, to);
	else
		put_unaligned_be32(ino, to);
}

static scxfs_ino_t
scxfs_dir2_sf_get_parent_ino(
	struct scxfs_dir2_sf_hdr	*hdr)
{
	return scxfs_dir2_sf_get_ino(hdr, hdr->parent);
}

static void
scxfs_dir2_sf_put_parent_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	scxfs_ino_t		ino)
{
	scxfs_dir2_sf_put_ino(hdr, hdr->parent, ino);
}

/*
 * In short-form directory entries the inode numbers are stored at variable
 * offset behind the entry name. If the entry stores a filetype value, then it
 * sits between the name and the inode number. Hence the inode numbers may only
 * be accessed through the helpers below.
 */
static scxfs_ino_t
scxfs_dir2_sfe_get_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep)
{
	return scxfs_dir2_sf_get_ino(hdr, &sfep->name[sfep->namelen]);
}

static void
scxfs_dir2_sfe_put_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep,
	scxfs_ino_t		ino)
{
	scxfs_dir2_sf_put_ino(hdr, &sfep->name[sfep->namelen], ino);
}

static scxfs_ino_t
scxfs_dir3_sfe_get_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep)
{
	return scxfs_dir2_sf_get_ino(hdr, &sfep->name[sfep->namelen + 1]);
}

static void
scxfs_dir3_sfe_put_ino(
	struct scxfs_dir2_sf_hdr	*hdr,
	struct scxfs_dir2_sf_entry *sfep,
	scxfs_ino_t		ino)
{
	scxfs_dir2_sf_put_ino(hdr, &sfep->name[sfep->namelen + 1], ino);
}


/*
 * Directory data block operations
 */

/*
 * For special situations, the dirent size ends up fixed because we always know
 * what the size of the entry is. That's true for the "." and "..", and
 * therefore we know that they are a fixed size and hence their offsets are
 * constant, as is the first entry.
 *
 * Hence, this calculation is written as a macro to be able to be calculated at
 * compile time and so certain offsets can be calculated directly in the
 * structure initaliser via the macro. There are two macros - one for dirents
 * with ftype and without so there are no unresolvable conditionals in the
 * calculations. We also use round_up() as SCXFS_DIR2_DATA_ALIGN is always a power
 * of 2 and the compiler doesn't reject it (unlike roundup()).
 */
#define SCXFS_DIR2_DATA_ENTSIZE(n)					\
	round_up((offsetof(struct scxfs_dir2_data_entry, name[0]) + (n) +	\
		 sizeof(scxfs_dir2_data_off_t)), SCXFS_DIR2_DATA_ALIGN)

#define SCXFS_DIR3_DATA_ENTSIZE(n)					\
	round_up((offsetof(struct scxfs_dir2_data_entry, name[0]) + (n) +	\
		 sizeof(scxfs_dir2_data_off_t) + sizeof(uint8_t)),	\
		SCXFS_DIR2_DATA_ALIGN)

static int
scxfs_dir2_data_entsize(
	int			n)
{
	return SCXFS_DIR2_DATA_ENTSIZE(n);
}

static int
scxfs_dir3_data_entsize(
	int			n)
{
	return SCXFS_DIR3_DATA_ENTSIZE(n);
}

static uint8_t
scxfs_dir2_data_get_ftype(
	struct scxfs_dir2_data_entry *dep)
{
	return SCXFS_DIR3_FT_UNKNOWN;
}

static void
scxfs_dir2_data_put_ftype(
	struct scxfs_dir2_data_entry *dep,
	uint8_t			ftype)
{
	ASSERT(ftype < SCXFS_DIR3_FT_MAX);
}

static uint8_t
scxfs_dir3_data_get_ftype(
	struct scxfs_dir2_data_entry *dep)
{
	uint8_t		ftype = dep->name[dep->namelen];

	if (ftype >= SCXFS_DIR3_FT_MAX)
		return SCXFS_DIR3_FT_UNKNOWN;
	return ftype;
}

static void
scxfs_dir3_data_put_ftype(
	struct scxfs_dir2_data_entry *dep,
	uint8_t			type)
{
	ASSERT(type < SCXFS_DIR3_FT_MAX);
	ASSERT(dep->namelen != 0);

	dep->name[dep->namelen] = type;
}

/*
 * Pointer to an entry's tag word.
 */
static __be16 *
scxfs_dir2_data_entry_tag_p(
	struct scxfs_dir2_data_entry *dep)
{
	return (__be16 *)((char *)dep +
		scxfs_dir2_data_entsize(dep->namelen) - sizeof(__be16));
}

static __be16 *
scxfs_dir3_data_entry_tag_p(
	struct scxfs_dir2_data_entry *dep)
{
	return (__be16 *)((char *)dep +
		scxfs_dir3_data_entsize(dep->namelen) - sizeof(__be16));
}

/*
 * location of . and .. in data space (always block 0)
 */
static struct scxfs_dir2_data_entry *
scxfs_dir2_data_dot_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr));
}

static struct scxfs_dir2_data_entry *
scxfs_dir2_data_dotdot_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR2_DATA_ENTSIZE(1));
}

static struct scxfs_dir2_data_entry *
scxfs_dir2_data_first_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR2_DATA_ENTSIZE(1) +
				SCXFS_DIR2_DATA_ENTSIZE(2));
}

static struct scxfs_dir2_data_entry *
scxfs_dir2_ftype_data_dotdot_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1));
}

static struct scxfs_dir2_data_entry *
scxfs_dir2_ftype_data_first_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1) +
				SCXFS_DIR3_DATA_ENTSIZE(2));
}

static struct scxfs_dir2_data_entry *
scxfs_dir3_data_dot_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir3_data_hdr));
}

static struct scxfs_dir2_data_entry *
scxfs_dir3_data_dotdot_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir3_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1));
}

static struct scxfs_dir2_data_entry *
scxfs_dir3_data_first_entry_p(
	struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir3_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1) +
				SCXFS_DIR3_DATA_ENTSIZE(2));
}

static struct scxfs_dir2_data_free *
scxfs_dir2_data_bestfree_p(struct scxfs_dir2_data_hdr *hdr)
{
	return hdr->bestfree;
}

static struct scxfs_dir2_data_free *
scxfs_dir3_data_bestfree_p(struct scxfs_dir2_data_hdr *hdr)
{
	return ((struct scxfs_dir3_data_hdr *)hdr)->best_free;
}

static struct scxfs_dir2_data_entry *
scxfs_dir2_data_entry_p(struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr));
}

static struct scxfs_dir2_data_unused *
scxfs_dir2_data_unused_p(struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_unused *)
		((char *)hdr + sizeof(struct scxfs_dir2_data_hdr));
}

static struct scxfs_dir2_data_entry *
scxfs_dir3_data_entry_p(struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_entry *)
		((char *)hdr + sizeof(struct scxfs_dir3_data_hdr));
}

static struct scxfs_dir2_data_unused *
scxfs_dir3_data_unused_p(struct scxfs_dir2_data_hdr *hdr)
{
	return (struct scxfs_dir2_data_unused *)
		((char *)hdr + sizeof(struct scxfs_dir3_data_hdr));
}


/*
 * Directory Leaf block operations
 */
static int
scxfs_dir2_max_leaf_ents(struct scxfs_da_geometry *geo)
{
	return (geo->blksize - sizeof(struct scxfs_dir2_leaf_hdr)) /
		(uint)sizeof(struct scxfs_dir2_leaf_entry);
}

static struct scxfs_dir2_leaf_entry *
scxfs_dir2_leaf_ents_p(struct scxfs_dir2_leaf *lp)
{
	return lp->__ents;
}

static int
scxfs_dir3_max_leaf_ents(struct scxfs_da_geometry *geo)
{
	return (geo->blksize - sizeof(struct scxfs_dir3_leaf_hdr)) /
		(uint)sizeof(struct scxfs_dir2_leaf_entry);
}

static struct scxfs_dir2_leaf_entry *
scxfs_dir3_leaf_ents_p(struct scxfs_dir2_leaf *lp)
{
	return ((struct scxfs_dir3_leaf *)lp)->__ents;
}

static void
scxfs_dir2_leaf_hdr_from_disk(
	struct scxfs_dir3_icleaf_hdr	*to,
	struct scxfs_dir2_leaf		*from)
{
	to->forw = be32_to_cpu(from->hdr.info.forw);
	to->back = be32_to_cpu(from->hdr.info.back);
	to->magic = be16_to_cpu(from->hdr.info.magic);
	to->count = be16_to_cpu(from->hdr.count);
	to->stale = be16_to_cpu(from->hdr.stale);

	ASSERT(to->magic == SCXFS_DIR2_LEAF1_MAGIC ||
	       to->magic == SCXFS_DIR2_LEAFN_MAGIC);
}

static void
scxfs_dir2_leaf_hdr_to_disk(
	struct scxfs_dir2_leaf		*to,
	struct scxfs_dir3_icleaf_hdr	*from)
{
	ASSERT(from->magic == SCXFS_DIR2_LEAF1_MAGIC ||
	       from->magic == SCXFS_DIR2_LEAFN_MAGIC);

	to->hdr.info.forw = cpu_to_be32(from->forw);
	to->hdr.info.back = cpu_to_be32(from->back);
	to->hdr.info.magic = cpu_to_be16(from->magic);
	to->hdr.count = cpu_to_be16(from->count);
	to->hdr.stale = cpu_to_be16(from->stale);
}

static void
scxfs_dir3_leaf_hdr_from_disk(
	struct scxfs_dir3_icleaf_hdr	*to,
	struct scxfs_dir2_leaf		*from)
{
	struct scxfs_dir3_leaf_hdr *hdr3 = (struct scxfs_dir3_leaf_hdr *)from;

	to->forw = be32_to_cpu(hdr3->info.hdr.forw);
	to->back = be32_to_cpu(hdr3->info.hdr.back);
	to->magic = be16_to_cpu(hdr3->info.hdr.magic);
	to->count = be16_to_cpu(hdr3->count);
	to->stale = be16_to_cpu(hdr3->stale);

	ASSERT(to->magic == SCXFS_DIR3_LEAF1_MAGIC ||
	       to->magic == SCXFS_DIR3_LEAFN_MAGIC);
}

static void
scxfs_dir3_leaf_hdr_to_disk(
	struct scxfs_dir2_leaf		*to,
	struct scxfs_dir3_icleaf_hdr	*from)
{
	struct scxfs_dir3_leaf_hdr *hdr3 = (struct scxfs_dir3_leaf_hdr *)to;

	ASSERT(from->magic == SCXFS_DIR3_LEAF1_MAGIC ||
	       from->magic == SCXFS_DIR3_LEAFN_MAGIC);

	hdr3->info.hdr.forw = cpu_to_be32(from->forw);
	hdr3->info.hdr.back = cpu_to_be32(from->back);
	hdr3->info.hdr.magic = cpu_to_be16(from->magic);
	hdr3->count = cpu_to_be16(from->count);
	hdr3->stale = cpu_to_be16(from->stale);
}


/*
 * Directory/Attribute Node block operations
 */
static struct scxfs_da_node_entry *
scxfs_da2_node_tree_p(struct scxfs_da_intnode *dap)
{
	return dap->__btree;
}

static struct scxfs_da_node_entry *
scxfs_da3_node_tree_p(struct scxfs_da_intnode *dap)
{
	return ((struct scxfs_da3_intnode *)dap)->__btree;
}

static void
scxfs_da2_node_hdr_from_disk(
	struct scxfs_da3_icnode_hdr	*to,
	struct scxfs_da_intnode		*from)
{
	ASSERT(from->hdr.info.magic == cpu_to_be16(SCXFS_DA_NODE_MAGIC));
	to->forw = be32_to_cpu(from->hdr.info.forw);
	to->back = be32_to_cpu(from->hdr.info.back);
	to->magic = be16_to_cpu(from->hdr.info.magic);
	to->count = be16_to_cpu(from->hdr.__count);
	to->level = be16_to_cpu(from->hdr.__level);
}

static void
scxfs_da2_node_hdr_to_disk(
	struct scxfs_da_intnode		*to,
	struct scxfs_da3_icnode_hdr	*from)
{
	ASSERT(from->magic == SCXFS_DA_NODE_MAGIC);
	to->hdr.info.forw = cpu_to_be32(from->forw);
	to->hdr.info.back = cpu_to_be32(from->back);
	to->hdr.info.magic = cpu_to_be16(from->magic);
	to->hdr.__count = cpu_to_be16(from->count);
	to->hdr.__level = cpu_to_be16(from->level);
}

static void
scxfs_da3_node_hdr_from_disk(
	struct scxfs_da3_icnode_hdr	*to,
	struct scxfs_da_intnode		*from)
{
	struct scxfs_da3_node_hdr *hdr3 = (struct scxfs_da3_node_hdr *)from;

	ASSERT(from->hdr.info.magic == cpu_to_be16(SCXFS_DA3_NODE_MAGIC));
	to->forw = be32_to_cpu(hdr3->info.hdr.forw);
	to->back = be32_to_cpu(hdr3->info.hdr.back);
	to->magic = be16_to_cpu(hdr3->info.hdr.magic);
	to->count = be16_to_cpu(hdr3->__count);
	to->level = be16_to_cpu(hdr3->__level);
}

static void
scxfs_da3_node_hdr_to_disk(
	struct scxfs_da_intnode		*to,
	struct scxfs_da3_icnode_hdr	*from)
{
	struct scxfs_da3_node_hdr *hdr3 = (struct scxfs_da3_node_hdr *)to;

	ASSERT(from->magic == SCXFS_DA3_NODE_MAGIC);
	hdr3->info.hdr.forw = cpu_to_be32(from->forw);
	hdr3->info.hdr.back = cpu_to_be32(from->back);
	hdr3->info.hdr.magic = cpu_to_be16(from->magic);
	hdr3->__count = cpu_to_be16(from->count);
	hdr3->__level = cpu_to_be16(from->level);
}


/*
 * Directory free space block operations
 */
static int
scxfs_dir2_free_max_bests(struct scxfs_da_geometry *geo)
{
	return (geo->blksize - sizeof(struct scxfs_dir2_free_hdr)) /
		sizeof(scxfs_dir2_data_off_t);
}

static __be16 *
scxfs_dir2_free_bests_p(struct scxfs_dir2_free *free)
{
	return (__be16 *)((char *)free + sizeof(struct scxfs_dir2_free_hdr));
}

/*
 * Convert data space db to the corresponding free db.
 */
static scxfs_dir2_db_t
scxfs_dir2_db_to_fdb(struct scxfs_da_geometry *geo, scxfs_dir2_db_t db)
{
	return scxfs_dir2_byte_to_db(geo, SCXFS_DIR2_FREE_OFFSET) +
			(db / scxfs_dir2_free_max_bests(geo));
}

/*
 * Convert data space db to the corresponding index in a free db.
 */
static int
scxfs_dir2_db_to_fdindex(struct scxfs_da_geometry *geo, scxfs_dir2_db_t db)
{
	return db % scxfs_dir2_free_max_bests(geo);
}

static int
scxfs_dir3_free_max_bests(struct scxfs_da_geometry *geo)
{
	return (geo->blksize - sizeof(struct scxfs_dir3_free_hdr)) /
		sizeof(scxfs_dir2_data_off_t);
}

static __be16 *
scxfs_dir3_free_bests_p(struct scxfs_dir2_free *free)
{
	return (__be16 *)((char *)free + sizeof(struct scxfs_dir3_free_hdr));
}

/*
 * Convert data space db to the corresponding free db.
 */
static scxfs_dir2_db_t
scxfs_dir3_db_to_fdb(struct scxfs_da_geometry *geo, scxfs_dir2_db_t db)
{
	return scxfs_dir2_byte_to_db(geo, SCXFS_DIR2_FREE_OFFSET) +
			(db / scxfs_dir3_free_max_bests(geo));
}

/*
 * Convert data space db to the corresponding index in a free db.
 */
static int
scxfs_dir3_db_to_fdindex(struct scxfs_da_geometry *geo, scxfs_dir2_db_t db)
{
	return db % scxfs_dir3_free_max_bests(geo);
}

static void
scxfs_dir2_free_hdr_from_disk(
	struct scxfs_dir3_icfree_hdr	*to,
	struct scxfs_dir2_free		*from)
{
	to->magic = be32_to_cpu(from->hdr.magic);
	to->firstdb = be32_to_cpu(from->hdr.firstdb);
	to->nvalid = be32_to_cpu(from->hdr.nvalid);
	to->nused = be32_to_cpu(from->hdr.nused);
	ASSERT(to->magic == SCXFS_DIR2_FREE_MAGIC);
}

static void
scxfs_dir2_free_hdr_to_disk(
	struct scxfs_dir2_free		*to,
	struct scxfs_dir3_icfree_hdr	*from)
{
	ASSERT(from->magic == SCXFS_DIR2_FREE_MAGIC);

	to->hdr.magic = cpu_to_be32(from->magic);
	to->hdr.firstdb = cpu_to_be32(from->firstdb);
	to->hdr.nvalid = cpu_to_be32(from->nvalid);
	to->hdr.nused = cpu_to_be32(from->nused);
}

static void
scxfs_dir3_free_hdr_from_disk(
	struct scxfs_dir3_icfree_hdr	*to,
	struct scxfs_dir2_free		*from)
{
	struct scxfs_dir3_free_hdr *hdr3 = (struct scxfs_dir3_free_hdr *)from;

	to->magic = be32_to_cpu(hdr3->hdr.magic);
	to->firstdb = be32_to_cpu(hdr3->firstdb);
	to->nvalid = be32_to_cpu(hdr3->nvalid);
	to->nused = be32_to_cpu(hdr3->nused);

	ASSERT(to->magic == SCXFS_DIR3_FREE_MAGIC);
}

static void
scxfs_dir3_free_hdr_to_disk(
	struct scxfs_dir2_free		*to,
	struct scxfs_dir3_icfree_hdr	*from)
{
	struct scxfs_dir3_free_hdr *hdr3 = (struct scxfs_dir3_free_hdr *)to;

	ASSERT(from->magic == SCXFS_DIR3_FREE_MAGIC);

	hdr3->hdr.magic = cpu_to_be32(from->magic);
	hdr3->firstdb = cpu_to_be32(from->firstdb);
	hdr3->nvalid = cpu_to_be32(from->nvalid);
	hdr3->nused = cpu_to_be32(from->nused);
}

static const struct scxfs_dir_ops scxfs_dir2_ops = {
	.sf_entsize = scxfs_dir2_sf_entsize,
	.sf_nextentry = scxfs_dir2_sf_nextentry,
	.sf_get_ftype = scxfs_dir2_sfe_get_ftype,
	.sf_put_ftype = scxfs_dir2_sfe_put_ftype,
	.sf_get_ino = scxfs_dir2_sfe_get_ino,
	.sf_put_ino = scxfs_dir2_sfe_put_ino,
	.sf_get_parent_ino = scxfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = scxfs_dir2_sf_put_parent_ino,

	.data_entsize = scxfs_dir2_data_entsize,
	.data_get_ftype = scxfs_dir2_data_get_ftype,
	.data_put_ftype = scxfs_dir2_data_put_ftype,
	.data_entry_tag_p = scxfs_dir2_data_entry_tag_p,
	.data_bestfree_p = scxfs_dir2_data_bestfree_p,

	.data_dot_offset = sizeof(struct scxfs_dir2_data_hdr),
	.data_dotdot_offset = sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR2_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR2_DATA_ENTSIZE(1) +
				SCXFS_DIR2_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct scxfs_dir2_data_hdr),

	.data_dot_entry_p = scxfs_dir2_data_dot_entry_p,
	.data_dotdot_entry_p = scxfs_dir2_data_dotdot_entry_p,
	.data_first_entry_p = scxfs_dir2_data_first_entry_p,
	.data_entry_p = scxfs_dir2_data_entry_p,
	.data_unused_p = scxfs_dir2_data_unused_p,

	.leaf_hdr_size = sizeof(struct scxfs_dir2_leaf_hdr),
	.leaf_hdr_to_disk = scxfs_dir2_leaf_hdr_to_disk,
	.leaf_hdr_from_disk = scxfs_dir2_leaf_hdr_from_disk,
	.leaf_max_ents = scxfs_dir2_max_leaf_ents,
	.leaf_ents_p = scxfs_dir2_leaf_ents_p,

	.node_hdr_size = sizeof(struct scxfs_da_node_hdr),
	.node_hdr_to_disk = scxfs_da2_node_hdr_to_disk,
	.node_hdr_from_disk = scxfs_da2_node_hdr_from_disk,
	.node_tree_p = scxfs_da2_node_tree_p,

	.free_hdr_size = sizeof(struct scxfs_dir2_free_hdr),
	.free_hdr_to_disk = scxfs_dir2_free_hdr_to_disk,
	.free_hdr_from_disk = scxfs_dir2_free_hdr_from_disk,
	.free_max_bests = scxfs_dir2_free_max_bests,
	.free_bests_p = scxfs_dir2_free_bests_p,
	.db_to_fdb = scxfs_dir2_db_to_fdb,
	.db_to_fdindex = scxfs_dir2_db_to_fdindex,
};

static const struct scxfs_dir_ops scxfs_dir2_ftype_ops = {
	.sf_entsize = scxfs_dir3_sf_entsize,
	.sf_nextentry = scxfs_dir3_sf_nextentry,
	.sf_get_ftype = scxfs_dir3_sfe_get_ftype,
	.sf_put_ftype = scxfs_dir3_sfe_put_ftype,
	.sf_get_ino = scxfs_dir3_sfe_get_ino,
	.sf_put_ino = scxfs_dir3_sfe_put_ino,
	.sf_get_parent_ino = scxfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = scxfs_dir2_sf_put_parent_ino,

	.data_entsize = scxfs_dir3_data_entsize,
	.data_get_ftype = scxfs_dir3_data_get_ftype,
	.data_put_ftype = scxfs_dir3_data_put_ftype,
	.data_entry_tag_p = scxfs_dir3_data_entry_tag_p,
	.data_bestfree_p = scxfs_dir2_data_bestfree_p,

	.data_dot_offset = sizeof(struct scxfs_dir2_data_hdr),
	.data_dotdot_offset = sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct scxfs_dir2_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1) +
				SCXFS_DIR3_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct scxfs_dir2_data_hdr),

	.data_dot_entry_p = scxfs_dir2_data_dot_entry_p,
	.data_dotdot_entry_p = scxfs_dir2_ftype_data_dotdot_entry_p,
	.data_first_entry_p = scxfs_dir2_ftype_data_first_entry_p,
	.data_entry_p = scxfs_dir2_data_entry_p,
	.data_unused_p = scxfs_dir2_data_unused_p,

	.leaf_hdr_size = sizeof(struct scxfs_dir2_leaf_hdr),
	.leaf_hdr_to_disk = scxfs_dir2_leaf_hdr_to_disk,
	.leaf_hdr_from_disk = scxfs_dir2_leaf_hdr_from_disk,
	.leaf_max_ents = scxfs_dir2_max_leaf_ents,
	.leaf_ents_p = scxfs_dir2_leaf_ents_p,

	.node_hdr_size = sizeof(struct scxfs_da_node_hdr),
	.node_hdr_to_disk = scxfs_da2_node_hdr_to_disk,
	.node_hdr_from_disk = scxfs_da2_node_hdr_from_disk,
	.node_tree_p = scxfs_da2_node_tree_p,

	.free_hdr_size = sizeof(struct scxfs_dir2_free_hdr),
	.free_hdr_to_disk = scxfs_dir2_free_hdr_to_disk,
	.free_hdr_from_disk = scxfs_dir2_free_hdr_from_disk,
	.free_max_bests = scxfs_dir2_free_max_bests,
	.free_bests_p = scxfs_dir2_free_bests_p,
	.db_to_fdb = scxfs_dir2_db_to_fdb,
	.db_to_fdindex = scxfs_dir2_db_to_fdindex,
};

static const struct scxfs_dir_ops scxfs_dir3_ops = {
	.sf_entsize = scxfs_dir3_sf_entsize,
	.sf_nextentry = scxfs_dir3_sf_nextentry,
	.sf_get_ftype = scxfs_dir3_sfe_get_ftype,
	.sf_put_ftype = scxfs_dir3_sfe_put_ftype,
	.sf_get_ino = scxfs_dir3_sfe_get_ino,
	.sf_put_ino = scxfs_dir3_sfe_put_ino,
	.sf_get_parent_ino = scxfs_dir2_sf_get_parent_ino,
	.sf_put_parent_ino = scxfs_dir2_sf_put_parent_ino,

	.data_entsize = scxfs_dir3_data_entsize,
	.data_get_ftype = scxfs_dir3_data_get_ftype,
	.data_put_ftype = scxfs_dir3_data_put_ftype,
	.data_entry_tag_p = scxfs_dir3_data_entry_tag_p,
	.data_bestfree_p = scxfs_dir3_data_bestfree_p,

	.data_dot_offset = sizeof(struct scxfs_dir3_data_hdr),
	.data_dotdot_offset = sizeof(struct scxfs_dir3_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1),
	.data_first_offset =  sizeof(struct scxfs_dir3_data_hdr) +
				SCXFS_DIR3_DATA_ENTSIZE(1) +
				SCXFS_DIR3_DATA_ENTSIZE(2),
	.data_entry_offset = sizeof(struct scxfs_dir3_data_hdr),

	.data_dot_entry_p = scxfs_dir3_data_dot_entry_p,
	.data_dotdot_entry_p = scxfs_dir3_data_dotdot_entry_p,
	.data_first_entry_p = scxfs_dir3_data_first_entry_p,
	.data_entry_p = scxfs_dir3_data_entry_p,
	.data_unused_p = scxfs_dir3_data_unused_p,

	.leaf_hdr_size = sizeof(struct scxfs_dir3_leaf_hdr),
	.leaf_hdr_to_disk = scxfs_dir3_leaf_hdr_to_disk,
	.leaf_hdr_from_disk = scxfs_dir3_leaf_hdr_from_disk,
	.leaf_max_ents = scxfs_dir3_max_leaf_ents,
	.leaf_ents_p = scxfs_dir3_leaf_ents_p,

	.node_hdr_size = sizeof(struct scxfs_da3_node_hdr),
	.node_hdr_to_disk = scxfs_da3_node_hdr_to_disk,
	.node_hdr_from_disk = scxfs_da3_node_hdr_from_disk,
	.node_tree_p = scxfs_da3_node_tree_p,

	.free_hdr_size = sizeof(struct scxfs_dir3_free_hdr),
	.free_hdr_to_disk = scxfs_dir3_free_hdr_to_disk,
	.free_hdr_from_disk = scxfs_dir3_free_hdr_from_disk,
	.free_max_bests = scxfs_dir3_free_max_bests,
	.free_bests_p = scxfs_dir3_free_bests_p,
	.db_to_fdb = scxfs_dir3_db_to_fdb,
	.db_to_fdindex = scxfs_dir3_db_to_fdindex,
};

static const struct scxfs_dir_ops scxfs_dir2_nondir_ops = {
	.node_hdr_size = sizeof(struct scxfs_da_node_hdr),
	.node_hdr_to_disk = scxfs_da2_node_hdr_to_disk,
	.node_hdr_from_disk = scxfs_da2_node_hdr_from_disk,
	.node_tree_p = scxfs_da2_node_tree_p,
};

static const struct scxfs_dir_ops scxfs_dir3_nondir_ops = {
	.node_hdr_size = sizeof(struct scxfs_da3_node_hdr),
	.node_hdr_to_disk = scxfs_da3_node_hdr_to_disk,
	.node_hdr_from_disk = scxfs_da3_node_hdr_from_disk,
	.node_tree_p = scxfs_da3_node_tree_p,
};

/*
 * Return the ops structure according to the current config.  If we are passed
 * an inode, then that overrides the default config we use which is based on
 * feature bits.
 */
const struct scxfs_dir_ops *
scxfs_dir_get_ops(
	struct scxfs_mount	*mp,
	struct scxfs_inode	*dp)
{
	if (dp)
		return dp->d_ops;
	if (mp->m_dir_inode_ops)
		return mp->m_dir_inode_ops;
	if (scxfs_sb_version_hascrc(&mp->m_sb))
		return &scxfs_dir3_ops;
	if (scxfs_sb_version_hasftype(&mp->m_sb))
		return &scxfs_dir2_ftype_ops;
	return &scxfs_dir2_ops;
}

const struct scxfs_dir_ops *
scxfs_nondir_get_ops(
	struct scxfs_mount	*mp,
	struct scxfs_inode	*dp)
{
	if (dp)
		return dp->d_ops;
	if (mp->m_nondir_inode_ops)
		return mp->m_nondir_inode_ops;
	if (scxfs_sb_version_hascrc(&mp->m_sb))
		return &scxfs_dir3_nondir_ops;
	return &scxfs_dir2_nondir_ops;
}
