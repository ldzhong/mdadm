/*
 * mdadm - Intel(R) Matrix Storage Manager Support
 *
 * Copyright (C) 2002-2008 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define HAVE_STDINT_H 1
#include "mdadm.h"
#include "mdmon.h"
#include "sha1.h"
#include <values.h>
#include <scsi/sg.h>
#include <ctype.h>

/* MPB == Metadata Parameter Block */
#define MPB_SIGNATURE "Intel Raid ISM Cfg Sig. "
#define MPB_SIG_LEN (strlen(MPB_SIGNATURE))
#define MPB_VERSION_RAID0 "1.0.00"
#define MPB_VERSION_RAID1 "1.1.00"
#define MPB_VERSION_MANY_VOLUMES_PER_ARRAY "1.2.00"
#define MPB_VERSION_3OR4_DISK_ARRAY "1.2.01"
#define MPB_VERSION_RAID5 "1.2.02"
#define MPB_VERSION_5OR6_DISK_ARRAY "1.2.04"
#define MPB_VERSION_CNG "1.2.06"
#define MPB_VERSION_ATTRIBS "1.3.00"
#define MAX_SIGNATURE_LENGTH  32
#define MAX_RAID_SERIAL_LEN   16

#define MPB_ATTRIB_CHECKSUM_VERIFY __cpu_to_le32(0x80000000)
#define MPB_ATTRIB_PM      __cpu_to_le32(0x40000000)
#define MPB_ATTRIB_2TB     __cpu_to_le32(0x20000000)
#define MPB_ATTRIB_RAID0   __cpu_to_le32(0x00000001)
#define MPB_ATTRIB_RAID1   __cpu_to_le32(0x00000002)
#define MPB_ATTRIB_RAID10  __cpu_to_le32(0x00000004)
#define MPB_ATTRIB_RAID1E  __cpu_to_le32(0x00000008)
#define MPB_ATTRIB_RAID5   __cpu_to_le32(0x00000010)
#define MPB_ATTRIB_RAIDCNG __cpu_to_le32(0x00000020)

#define MPB_SECTOR_CNT 418
#define IMSM_RESERVED_SECTORS 4096

/* Disk configuration info. */
#define IMSM_MAX_DEVICES 255
struct imsm_disk {
	__u8 serial[MAX_RAID_SERIAL_LEN];/* 0xD8 - 0xE7 ascii serial number */
	__u32 total_blocks;		 /* 0xE8 - 0xEB total blocks */
	__u32 scsi_id;			 /* 0xEC - 0xEF scsi ID */
#define SPARE_DISK      __cpu_to_le32(0x01)  /* Spare */
#define CONFIGURED_DISK __cpu_to_le32(0x02)  /* Member of some RaidDev */
#define FAILED_DISK     __cpu_to_le32(0x04)  /* Permanent failure */
#define USABLE_DISK     __cpu_to_le32(0x08)  /* Fully usable unless FAILED_DISK is set */
	__u32 status;			 /* 0xF0 - 0xF3 */
	__u32 owner_cfg_num; /* which config 0,1,2... owns this disk */ 
#define	IMSM_DISK_FILLERS	4
	__u32 filler[IMSM_DISK_FILLERS]; /* 0xF4 - 0x107 MPB_DISK_FILLERS for future expansion */
};

/* RAID map configuration infos. */
struct imsm_map {
	__u32 pba_of_lba0;	/* start address of partition */
	__u32 blocks_per_member;/* blocks per member */
	__u32 num_data_stripes;	/* number of data stripes */
	__u16 blocks_per_strip;
	__u8  map_state;	/* Normal, Uninitialized, Degraded, Failed */
#define IMSM_T_STATE_NORMAL 0
#define IMSM_T_STATE_UNINITIALIZED 1
#define IMSM_T_STATE_DEGRADED 2
#define IMSM_T_STATE_FAILED 3
	__u8  raid_level;
#define IMSM_T_RAID0 0
#define IMSM_T_RAID1 1
#define IMSM_T_RAID5 5		/* since metadata version 1.2.02 ? */
	__u8  num_members;	/* number of member disks */
	__u8  num_domains;	/* number of parity domains */
	__u8  failed_disk_num;  /* valid only when state is degraded */
	__u8  reserved[1];
	__u32 filler[7];	/* expansion area */
#define IMSM_ORD_REBUILD (1 << 24)
	__u32 disk_ord_tbl[1];	/* disk_ord_tbl[num_members],
				 * top byte contains some flags
				 */
} __attribute__ ((packed));

struct imsm_vol {
	__u32 curr_migr_unit;
	__u32 checkpoint_id;	/* id to access curr_migr_unit */
	__u8  migr_state;	/* Normal or Migrating */
#define MIGR_INIT 0
#define MIGR_REBUILD 1
#define MIGR_VERIFY 2 /* analagous to echo check > sync_action */
#define MIGR_GEN_MIGR 3
#define MIGR_STATE_CHANGE 4
	__u8  migr_type;	/* Initializing, Rebuilding, ... */
	__u8  dirty;
	__u8  fs_state;		/* fast-sync state for CnG (0xff == disabled) */
	__u16 verify_errors;	/* number of mismatches */
	__u16 bad_blocks;	/* number of bad blocks during verify */
	__u32 filler[4];
	struct imsm_map map[1];
	/* here comes another one if migr_state */
} __attribute__ ((packed));

struct imsm_dev {
	__u8  volume[MAX_RAID_SERIAL_LEN];
	__u32 size_low;
	__u32 size_high;
#define DEV_BOOTABLE		__cpu_to_le32(0x01)
#define DEV_BOOT_DEVICE		__cpu_to_le32(0x02)
#define DEV_READ_COALESCING	__cpu_to_le32(0x04)
#define DEV_WRITE_COALESCING	__cpu_to_le32(0x08)
#define DEV_LAST_SHUTDOWN_DIRTY	__cpu_to_le32(0x10)
#define DEV_HIDDEN_AT_BOOT	__cpu_to_le32(0x20)
#define DEV_CURRENTLY_HIDDEN	__cpu_to_le32(0x40)
#define DEV_VERIFY_AND_FIX	__cpu_to_le32(0x80)
#define DEV_MAP_STATE_UNINIT	__cpu_to_le32(0x100)
#define DEV_NO_AUTO_RECOVERY	__cpu_to_le32(0x200)
#define DEV_CLONE_N_GO		__cpu_to_le32(0x400)
#define DEV_CLONE_MAN_SYNC	__cpu_to_le32(0x800)
#define DEV_CNG_MASTER_DISK_NUM	__cpu_to_le32(0x1000)
	__u32 status;	/* Persistent RaidDev status */
	__u32 reserved_blocks; /* Reserved blocks at beginning of volume */
	__u8  migr_priority;
	__u8  num_sub_vols;
	__u8  tid;
	__u8  cng_master_disk;
	__u16 cache_policy;
	__u8  cng_state;
	__u8  cng_sub_state;
#define IMSM_DEV_FILLERS 10
	__u32 filler[IMSM_DEV_FILLERS];
	struct imsm_vol vol;
} __attribute__ ((packed));

struct imsm_super {
	__u8 sig[MAX_SIGNATURE_LENGTH];	/* 0x00 - 0x1F */
	__u32 check_sum;		/* 0x20 - 0x23 MPB Checksum */
	__u32 mpb_size;			/* 0x24 - 0x27 Size of MPB */
	__u32 family_num;		/* 0x28 - 0x2B Checksum from first time this config was written */
	__u32 generation_num;		/* 0x2C - 0x2F Incremented each time this array's MPB is written */
	__u32 error_log_size;		/* 0x30 - 0x33 in bytes */
	__u32 attributes;		/* 0x34 - 0x37 */
	__u8 num_disks;			/* 0x38 Number of configured disks */
	__u8 num_raid_devs;		/* 0x39 Number of configured volumes */
	__u8 error_log_pos;		/* 0x3A  */
	__u8 fill[1];			/* 0x3B */
	__u32 cache_size;		/* 0x3c - 0x40 in mb */
	__u32 orig_family_num;		/* 0x40 - 0x43 original family num */
	__u32 pwr_cycle_count;		/* 0x44 - 0x47 simulated power cycle count for array */
	__u32 bbm_log_size;		/* 0x48 - 0x4B - size of bad Block Mgmt Log in bytes */
#define IMSM_FILLERS 35
	__u32 filler[IMSM_FILLERS];	/* 0x4C - 0xD7 RAID_MPB_FILLERS */
	struct imsm_disk disk[1];	/* 0xD8 diskTbl[numDisks] */
	/* here comes imsm_dev[num_raid_devs] */
	/* here comes BBM logs */
} __attribute__ ((packed));

#define BBM_LOG_MAX_ENTRIES 254

struct bbm_log_entry {
	__u64 defective_block_start;
#define UNREADABLE 0xFFFFFFFF
	__u32 spare_block_offset;
	__u16 remapped_marked_count;
	__u16 disk_ordinal;
} __attribute__ ((__packed__));

struct bbm_log {
	__u32 signature; /* 0xABADB10C */
	__u32 entry_count;
	__u32 reserved_spare_block_count; /* 0 */
	__u32 reserved; /* 0xFFFF */
	__u64 first_spare_lba;
	struct bbm_log_entry mapped_block_entries[BBM_LOG_MAX_ENTRIES];
} __attribute__ ((__packed__));


#ifndef MDASSEMBLE
static char *map_state_str[] = { "normal", "uninitialized", "degraded", "failed" };
#endif

static unsigned int sector_count(__u32 bytes)
{
	return ((bytes + (512-1)) & (~(512-1))) / 512;
}

static unsigned int mpb_sectors(struct imsm_super *mpb)
{
	return sector_count(__le32_to_cpu(mpb->mpb_size));
}

/* internal representation of IMSM metadata */
struct intel_super {
	union {
		void *buf; /* O_DIRECT buffer for reading/writing metadata */
		struct imsm_super *anchor; /* immovable parameters */
	};
	size_t len; /* size of the 'buf' allocation */
	void *next_buf; /* for realloc'ing buf from the manager */
	size_t next_len;
	int updates_pending; /* count of pending updates for mdmon */
	int creating_imsm; /* flag to indicate container creation */
	int current_vol; /* index of raid device undergoing creation */
	#define IMSM_MAX_RAID_DEVS 2
	struct imsm_dev *dev_tbl[IMSM_MAX_RAID_DEVS];
	struct dl {
		struct dl *next;
		int index;
		__u8 serial[MAX_RAID_SERIAL_LEN];
		int major, minor;
		char *devname;
		struct imsm_disk disk;
		int fd;
	} *disks;
	struct dl *add; /* list of disks to add while mdmon active */
	struct dl *missing; /* disks removed while we weren't looking */
	struct bbm_log *bbm_log;
};

struct extent {
	unsigned long long start, size;
};

/* definition of messages passed to imsm_process_update */
enum imsm_update_type {
	update_activate_spare,
	update_create_array,
	update_add_disk,
};

struct imsm_update_activate_spare {
	enum imsm_update_type type;
	struct dl *dl;
	int slot;
	int array;
	struct imsm_update_activate_spare *next;
};

struct imsm_update_create_array {
	enum imsm_update_type type;
	int dev_idx;
	struct imsm_dev dev;
};

struct imsm_update_add_disk {
	enum imsm_update_type type;
};

static struct supertype *match_metadata_desc_imsm(char *arg)
{
	struct supertype *st;

	if (strcmp(arg, "imsm") != 0 &&
	    strcmp(arg, "default") != 0
		)
		return NULL;

	st = malloc(sizeof(*st));
	memset(st, 0, sizeof(*st));
	st->ss = &super_imsm;
	st->max_devs = IMSM_MAX_DEVICES;
	st->minor_version = 0;
	st->sb = NULL;
	return st;
}

#ifndef MDASSEMBLE
static __u8 *get_imsm_version(struct imsm_super *mpb)
{
	return &mpb->sig[MPB_SIG_LEN];
}
#endif 

/* retrieve a disk directly from the anchor when the anchor is known to be
 * up-to-date, currently only at load time
 */
static struct imsm_disk *__get_imsm_disk(struct imsm_super *mpb, __u8 index)
{
	if (index >= mpb->num_disks)
		return NULL;
	return &mpb->disk[index];
}

#ifndef MDASSEMBLE
/* retrieve a disk from the parsed metadata */
static struct imsm_disk *get_imsm_disk(struct intel_super *super, __u8 index)
{
	struct dl *d;

	for (d = super->disks; d; d = d->next)
		if (d->index == index)
			return &d->disk;
	
	return NULL;
}
#endif

/* generate a checksum directly from the anchor when the anchor is known to be
 * up-to-date, currently only at load or write_super after coalescing
 */
static __u32 __gen_imsm_checksum(struct imsm_super *mpb)
{
	__u32 end = mpb->mpb_size / sizeof(end);
	__u32 *p = (__u32 *) mpb;
	__u32 sum = 0;

        while (end--) {
                sum += __le32_to_cpu(*p);
		p++;
	}

        return sum - __le32_to_cpu(mpb->check_sum);
}

static size_t sizeof_imsm_map(struct imsm_map *map)
{
	return sizeof(struct imsm_map) + sizeof(__u32) * (map->num_members - 1);
}

struct imsm_map *get_imsm_map(struct imsm_dev *dev, int second_map)
{
	struct imsm_map *map = &dev->vol.map[0];

	if (second_map && !dev->vol.migr_state)
		return NULL;
	else if (second_map) {
		void *ptr = map;

		return ptr + sizeof_imsm_map(map);
	} else
		return map;
		
}

/* return the size of the device.
 * migr_state increases the returned size if map[0] were to be duplicated
 */
static size_t sizeof_imsm_dev(struct imsm_dev *dev, int migr_state)
{
	size_t size = sizeof(*dev) - sizeof(struct imsm_map) +
		      sizeof_imsm_map(get_imsm_map(dev, 0));

	/* migrating means an additional map */
	if (dev->vol.migr_state)
		size += sizeof_imsm_map(get_imsm_map(dev, 1));
	else if (migr_state)
		size += sizeof_imsm_map(get_imsm_map(dev, 0));

	return size;
}

static struct imsm_dev *__get_imsm_dev(struct imsm_super *mpb, __u8 index)
{
	int offset;
	int i;
	void *_mpb = mpb;

	if (index >= mpb->num_raid_devs)
		return NULL;

	/* devices start after all disks */
	offset = ((void *) &mpb->disk[mpb->num_disks]) - _mpb;

	for (i = 0; i <= index; i++)
		if (i == index)
			return _mpb + offset;
		else
			offset += sizeof_imsm_dev(_mpb + offset, 0);

	return NULL;
}

static struct imsm_dev *get_imsm_dev(struct intel_super *super, __u8 index)
{
	if (index >= super->anchor->num_raid_devs)
		return NULL;
	return super->dev_tbl[index];
}

static __u32 get_imsm_ord_tbl_ent(struct imsm_dev *dev, int slot)
{
	struct imsm_map *map;

	if (dev->vol.migr_state)
		map = get_imsm_map(dev, 1);
	else
		map = get_imsm_map(dev, 0);

	/* top byte identifies disk under rebuild */
	return __le32_to_cpu(map->disk_ord_tbl[slot]);
}

#define ord_to_idx(ord) (((ord) << 8) >> 8)
static __u32 get_imsm_disk_idx(struct imsm_dev *dev, int slot)
{
	__u32 ord = get_imsm_ord_tbl_ent(dev, slot);

	return ord_to_idx(ord);
}

static void set_imsm_ord_tbl_ent(struct imsm_map *map, int slot, __u32 ord)
{
	map->disk_ord_tbl[slot] = __cpu_to_le32(ord);
}

static int get_imsm_raid_level(struct imsm_map *map)
{
	if (map->raid_level == 1) {
		if (map->num_members == 2)
			return 1;
		else
			return 10;
	}

	return map->raid_level;
}

static int cmp_extent(const void *av, const void *bv)
{
	const struct extent *a = av;
	const struct extent *b = bv;
	if (a->start < b->start)
		return -1;
	if (a->start > b->start)
		return 1;
	return 0;
}

static struct extent *get_extents(struct intel_super *super, struct dl *dl)
{
	/* find a list of used extents on the given physical device */
	struct extent *rv, *e;
	int i, j;
	int memberships = 0;
	__u32 reservation = MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);

		for (j = 0; j < map->num_members; j++) {
			__u32 index = get_imsm_disk_idx(dev, j);

			if (index == dl->index)
				memberships++;
		}
	}
	rv = malloc(sizeof(struct extent) * (memberships + 1));
	if (!rv)
		return NULL;
	e = rv;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);

		for (j = 0; j < map->num_members; j++) {
			__u32 index = get_imsm_disk_idx(dev, j);

			if (index == dl->index) {
				e->start = __le32_to_cpu(map->pba_of_lba0);
				e->size = __le32_to_cpu(map->blocks_per_member);
				e++;
			}
		}
	}
	qsort(rv, memberships, sizeof(*rv), cmp_extent);

	/* determine the start of the metadata 
	 * when no raid devices are defined use the default
	 * ...otherwise allow the metadata to truncate the value
	 * as is the case with older versions of imsm
	 */
	if (memberships) {
		struct extent *last = &rv[memberships - 1];
		__u32 remainder;

		remainder = __le32_to_cpu(dl->disk.total_blocks) - 
			    (last->start + last->size);
		/* round down to 1k block to satisfy precision of the kernel
		 * 'size' interface
		 */
		remainder &= ~1UL;
		/* make sure remainder is still sane */
		if (remainder < ROUND_UP(super->len, 512) >> 9)
			remainder = ROUND_UP(super->len, 512) >> 9;
		if (reservation > remainder)
			reservation = remainder;
	}
	e->start = __le32_to_cpu(dl->disk.total_blocks) - reservation;
	e->size = 0;
	return rv;
}

/* try to determine how much space is reserved for metadata from
 * the last get_extents() entry, otherwise fallback to the
 * default
 */
static __u32 imsm_reserved_sectors(struct intel_super *super, struct dl *dl)
{
	struct extent *e;
	int i;
	__u32 rv;

	/* for spares just return a minimal reservation which will grow
	 * once the spare is picked up by an array
	 */
	if (dl->index == -1)
		return MPB_SECTOR_CNT;

	e = get_extents(super, dl);
	if (!e)
		return MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;

	/* scroll to last entry */
	for (i = 0; e[i].size; i++)
		continue;

	rv = __le32_to_cpu(dl->disk.total_blocks) - e[i].start;

	free(e);

	return rv;
}

#ifndef MDASSEMBLE
static void print_imsm_dev(struct imsm_dev *dev, char *uuid, int disk_idx)
{
	__u64 sz;
	int slot;
	struct imsm_map *map = get_imsm_map(dev, 0);
	__u32 ord;

	printf("\n");
	printf("[%.16s]:\n", dev->volume);
	printf("           UUID : %s\n", uuid);
	printf("     RAID Level : %d\n", get_imsm_raid_level(map));
	printf("        Members : %d\n", map->num_members);
	for (slot = 0; slot < map->num_members; slot++)
		if (disk_idx== get_imsm_disk_idx(dev, slot))
			break;
	if (slot < map->num_members) {
		ord = get_imsm_ord_tbl_ent(dev, slot);
		printf("      This Slot : %d%s\n", slot,
		       ord & IMSM_ORD_REBUILD ? " (out-of-sync)" : "");
	} else
		printf("      This Slot : ?\n");
	sz = __le32_to_cpu(dev->size_high);
	sz <<= 32;
	sz += __le32_to_cpu(dev->size_low);
	printf("     Array Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
	sz = __le32_to_cpu(map->blocks_per_member);
	printf("   Per Dev Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
	printf("  Sector Offset : %u\n",
		__le32_to_cpu(map->pba_of_lba0));
	printf("    Num Stripes : %u\n",
		__le32_to_cpu(map->num_data_stripes));
	printf("     Chunk Size : %u KiB\n",
		__le16_to_cpu(map->blocks_per_strip) / 2);
	printf("       Reserved : %d\n", __le32_to_cpu(dev->reserved_blocks));
	printf("  Migrate State : %s", dev->vol.migr_state ? "migrating" : "idle");
	if (dev->vol.migr_state)
		printf(": %s", dev->vol.migr_type ? "rebuilding" : "initializing");
	printf("\n");
	printf("      Map State : %s", map_state_str[map->map_state]);
	if (dev->vol.migr_state) {
		struct imsm_map *map = get_imsm_map(dev, 1);
		printf(" <-- %s", map_state_str[map->map_state]);
	}
	printf("\n");
	printf("    Dirty State : %s\n", dev->vol.dirty ? "dirty" : "clean");
}

static void print_imsm_disk(struct imsm_super *mpb, int index, __u32 reserved)
{
	struct imsm_disk *disk = __get_imsm_disk(mpb, index);
	char str[MAX_RAID_SERIAL_LEN + 1];
	__u32 s;
	__u64 sz;

	if (index < 0)
		return;

	printf("\n");
	snprintf(str, MAX_RAID_SERIAL_LEN + 1, "%s", disk->serial);
	printf("  Disk%02d Serial : %s\n", index, str);
	s = disk->status;
	printf("          State :%s%s%s%s\n", s&SPARE_DISK ? " spare" : "",
					      s&CONFIGURED_DISK ? " active" : "",
					      s&FAILED_DISK ? " failed" : "",
					      s&USABLE_DISK ? " usable" : "");
	printf("             Id : %08x\n", __le32_to_cpu(disk->scsi_id));
	sz = __le32_to_cpu(disk->total_blocks) - reserved;
	printf("    Usable Size : %llu%s\n", (unsigned long long)sz,
	       human_size(sz * 512));
}

static void getinfo_super_imsm(struct supertype *st, struct mdinfo *info);

static void examine_super_imsm(struct supertype *st, char *homehost)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	char str[MAX_SIGNATURE_LENGTH];
	int i;
	struct mdinfo info;
	char nbuf[64];
	__u32 sum;
	__u32 reserved = imsm_reserved_sectors(super, super->disks);


	snprintf(str, MPB_SIG_LEN, "%s", mpb->sig);
	printf("          Magic : %s\n", str);
	snprintf(str, strlen(MPB_VERSION_RAID0), "%s", get_imsm_version(mpb));
	printf("        Version : %s\n", get_imsm_version(mpb));
	printf("         Family : %08x\n", __le32_to_cpu(mpb->family_num));
	printf("     Generation : %08x\n", __le32_to_cpu(mpb->generation_num));
	getinfo_super_imsm(st, &info);
	fname_from_uuid(st, &info, nbuf,'-');
	printf("           UUID : %s\n", nbuf + 5);
	sum = __le32_to_cpu(mpb->check_sum);
	printf("       Checksum : %08x %s\n", sum,
		__gen_imsm_checksum(mpb) == sum ? "correct" : "incorrect");
	printf("    MPB Sectors : %d\n", mpb_sectors(mpb));
	printf("          Disks : %d\n", mpb->num_disks);
	printf("   RAID Devices : %d\n", mpb->num_raid_devs);
	print_imsm_disk(mpb, super->disks->index, reserved);
	if (super->bbm_log) {
		struct bbm_log *log = super->bbm_log;

		printf("\n");
		printf("Bad Block Management Log:\n");
		printf("       Log Size : %d\n", __le32_to_cpu(mpb->bbm_log_size));
		printf("      Signature : %x\n", __le32_to_cpu(log->signature));
		printf("    Entry Count : %d\n", __le32_to_cpu(log->entry_count));
		printf("   Spare Blocks : %d\n",  __le32_to_cpu(log->reserved_spare_block_count));
		printf("    First Spare : %llx\n", __le64_to_cpu(log->first_spare_lba));
	}
	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct mdinfo info;
		struct imsm_dev *dev = __get_imsm_dev(mpb, i);

		super->current_vol = i;
		getinfo_super_imsm(st, &info);
		fname_from_uuid(st, &info, nbuf, '-');
		print_imsm_dev(dev, nbuf + 5, super->disks->index);
	}
	for (i = 0; i < mpb->num_disks; i++) {
		if (i == super->disks->index)
			continue;
		print_imsm_disk(mpb, i, reserved);
	}
}

static void brief_examine_super_imsm(struct supertype *st)
{
	/* We just write a generic IMSM ARRAY entry */
	struct mdinfo info;
	char nbuf[64];
	char nbuf1[64];
	struct intel_super *super = st->sb;
	int i;

	if (!super->anchor->num_raid_devs)
		return;

	getinfo_super_imsm(st, &info);
	fname_from_uuid(st, &info, nbuf,'-');
	printf("ARRAY metadata=imsm auto=md UUID=%s\n", nbuf + 5);
	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);

		super->current_vol = i;
		getinfo_super_imsm(st, &info);
		fname_from_uuid(st, &info, nbuf1,'-');
		printf("ARRAY /dev/md/%.16s container=%s\n"
		       "   member=%d auto=mdp UUID=%s\n",
		       dev->volume, nbuf + 5, i, nbuf1 + 5);
	}
}

static void detail_super_imsm(struct supertype *st, char *homehost)
{
	struct mdinfo info;
	char nbuf[64];

	getinfo_super_imsm(st, &info);
	fname_from_uuid(st, &info, nbuf,'-');
	printf("\n           UUID : %s\n", nbuf + 5);
}

static void brief_detail_super_imsm(struct supertype *st)
{
	struct mdinfo info;
	char nbuf[64];
	getinfo_super_imsm(st, &info);
	fname_from_uuid(st, &info, nbuf,'-');
	printf(" UUID=%s", nbuf + 5);
}
#endif

static int match_home_imsm(struct supertype *st, char *homehost)
{
	/* the imsm metadata format does not specify any host
	 * identification information.  We return -1 since we can never
	 * confirm nor deny whether a given array is "meant" for this
	 * host.  We rely on compare_super and the 'family_num' field to
	 * exclude member disks that do not belong, and we rely on
	 * mdadm.conf to specify the arrays that should be assembled.
	 * Auto-assembly may still pick up "foreign" arrays.
	 */

	return -1;
}

static void uuid_from_super_imsm(struct supertype *st, int uuid[4])
{
	/* The uuid returned here is used for:
	 *  uuid to put into bitmap file (Create, Grow)
	 *  uuid for backup header when saving critical section (Grow)
	 *  comparing uuids when re-adding a device into an array
	 *    In these cases the uuid required is that of the data-array,
	 *    not the device-set.
	 *  uuid to recognise same set when adding a missing device back
	 *    to an array.   This is a uuid for the device-set.
	 *  
	 * For each of these we can make do with a truncated
	 * or hashed uuid rather than the original, as long as
	 * everyone agrees.
	 * In each case the uuid required is that of the data-array,
	 * not the device-set.
	 */
	/* imsm does not track uuid's so we synthesis one using sha1 on
	 * - The signature (Which is constant for all imsm array, but no matter)
	 * - the family_num of the container
	 * - the index number of the volume
	 * - the 'serial' number of the volume.
	 * Hopefully these are all constant.
	 */
	struct intel_super *super = st->sb;

	char buf[20];
	struct sha1_ctx ctx;
	struct imsm_dev *dev = NULL;

	sha1_init_ctx(&ctx);
	sha1_process_bytes(super->anchor->sig, MPB_SIG_LEN, &ctx);
	sha1_process_bytes(&super->anchor->family_num, sizeof(__u32), &ctx);
	if (super->current_vol >= 0)
		dev = get_imsm_dev(super, super->current_vol);
	if (dev) {
		__u32 vol = super->current_vol;
		sha1_process_bytes(&vol, sizeof(vol), &ctx);
		sha1_process_bytes(dev->volume, MAX_RAID_SERIAL_LEN, &ctx);
	}
	sha1_finish_ctx(&ctx, buf);
	memcpy(uuid, buf, 4*4);
}

#if 0
static void
get_imsm_numerical_version(struct imsm_super *mpb, int *m, int *p)
{
	__u8 *v = get_imsm_version(mpb);
	__u8 *end = mpb->sig + MAX_SIGNATURE_LENGTH;
	char major[] = { 0, 0, 0 };
	char minor[] = { 0 ,0, 0 };
	char patch[] = { 0, 0, 0 };
	char *ver_parse[] = { major, minor, patch };
	int i, j;

	i = j = 0;
	while (*v != '\0' && v < end) {
		if (*v != '.' && j < 2)
			ver_parse[i][j++] = *v;
		else {
			i++;
			j = 0;
		}
		v++;
	}

	*m = strtol(minor, NULL, 0);
	*p = strtol(patch, NULL, 0);
}
#endif

static int imsm_level_to_layout(int level)
{
	switch (level) {
	case 0:
	case 1:
		return 0;
	case 5:
	case 6:
		return ALGORITHM_LEFT_ASYMMETRIC;
	case 10:
		return 0x102;
	}
	return -1;
}

static void getinfo_super_imsm_volume(struct supertype *st, struct mdinfo *info)
{
	struct intel_super *super = st->sb;
	struct imsm_dev *dev = get_imsm_dev(super, super->current_vol);
	struct imsm_map *map = get_imsm_map(dev, 0);

	info->container_member	  = super->current_vol;
	info->array.raid_disks    = map->num_members;
	info->array.level	  = get_imsm_raid_level(map);
	info->array.layout	  = imsm_level_to_layout(info->array.level);
	info->array.md_minor	  = -1;
	info->array.ctime	  = 0;
	info->array.utime	  = 0;
	info->array.chunk_size	  = __le16_to_cpu(map->blocks_per_strip) << 9;
	info->array.state	  = !dev->vol.dirty;

	info->disk.major = 0;
	info->disk.minor = 0;

	info->data_offset	  = __le32_to_cpu(map->pba_of_lba0);
	info->component_size	  = __le32_to_cpu(map->blocks_per_member);
	memset(info->uuid, 0, sizeof(info->uuid));

	if (map->map_state == IMSM_T_STATE_UNINITIALIZED || dev->vol.dirty)
		info->resync_start = 0;
	else if (dev->vol.migr_state)
		info->resync_start = __le32_to_cpu(dev->vol.curr_migr_unit);
	else
		info->resync_start = ~0ULL;

	strncpy(info->name, (char *) dev->volume, MAX_RAID_SERIAL_LEN);
	info->name[MAX_RAID_SERIAL_LEN] = 0;

	info->array.major_version = -1;
	info->array.minor_version = -2;
	sprintf(info->text_version, "/%s/%d",
		devnum2devname(st->container_dev),
		info->container_member);
	info->safe_mode_delay = 4000;  /* 4 secs like the Matrix driver */
	uuid_from_super_imsm(st, info->uuid);
}


static void getinfo_super_imsm(struct supertype *st, struct mdinfo *info)
{
	struct intel_super *super = st->sb;
	struct imsm_disk *disk;
	__u32 s;

	if (super->current_vol >= 0) {
		getinfo_super_imsm_volume(st, info);
		return;
	}

	/* Set raid_disks to zero so that Assemble will always pull in valid
	 * spares
	 */
	info->array.raid_disks    = 0;
	info->array.level         = LEVEL_CONTAINER;
	info->array.layout        = 0;
	info->array.md_minor      = -1;
	info->array.ctime         = 0; /* N/A for imsm */ 
	info->array.utime         = 0;
	info->array.chunk_size    = 0;

	info->disk.major = 0;
	info->disk.minor = 0;
	info->disk.raid_disk = -1;
	info->reshape_active = 0;
	info->array.major_version = -1;
	info->array.minor_version = -2;
	strcpy(info->text_version, "imsm");
	info->safe_mode_delay = 0;
	info->disk.number = -1;
	info->disk.state = 0;
	info->name[0] = 0;

	if (super->disks) {
		__u32 reserved = imsm_reserved_sectors(super, super->disks);

		disk = &super->disks->disk;
		info->data_offset = __le32_to_cpu(disk->total_blocks) - reserved;
		info->component_size = reserved;
		s = disk->status;
		info->disk.state  = s & CONFIGURED_DISK ? (1 << MD_DISK_ACTIVE) : 0;
		info->disk.state |= s & FAILED_DISK ? (1 << MD_DISK_FAULTY) : 0;
		info->disk.state |= s & SPARE_DISK ? 0 : (1 << MD_DISK_SYNC);
	}

	/* only call uuid_from_super_imsm when this disk is part of a populated container,
	 * ->compare_super may have updated the 'num_raid_devs' field for spares
	 */
	if (info->disk.state & (1 << MD_DISK_SYNC) || super->anchor->num_raid_devs)
		uuid_from_super_imsm(st, info->uuid);
	else
		memcpy(info->uuid, uuid_match_any, sizeof(int[4]));
}

static int update_super_imsm(struct supertype *st, struct mdinfo *info,
			     char *update, char *devname, int verbose,
			     int uuid_set, char *homehost)
{
	/* FIXME */

	/* For 'assemble' and 'force' we need to return non-zero if any
	 * change was made.  For others, the return value is ignored.
	 * Update options are:
	 *  force-one : This device looks a bit old but needs to be included,
	 *        update age info appropriately.
	 *  assemble: clear any 'faulty' flag to allow this device to
	 *		be assembled.
	 *  force-array: Array is degraded but being forced, mark it clean
	 *	   if that will be needed to assemble it.
	 *
	 *  newdev:  not used ????
	 *  grow:  Array has gained a new device - this is currently for
	 *		linear only
	 *  resync: mark as dirty so a resync will happen.
	 *  name:  update the name - preserving the homehost
	 *
	 * Following are not relevant for this imsm:
	 *  sparc2.2 : update from old dodgey metadata
	 *  super-minor: change the preferred_minor number
	 *  summaries:  update redundant counters.
	 *  uuid:  Change the uuid of the array to match watch is given
	 *  homehost:  update the recorded homehost
	 *  _reshape_progress: record new reshape_progress position.
	 */
	int rv = 0;
	//struct intel_super *super = st->sb;
	//struct imsm_super *mpb = super->mpb;

	if (strcmp(update, "grow") == 0) {
	}
	if (strcmp(update, "resync") == 0) {
		/* dev->vol.dirty = 1; */
	}

	/* IMSM has no concept of UUID or homehost */

	return rv;
}

static size_t disks_to_mpb_size(int disks)
{
	size_t size;

	size = sizeof(struct imsm_super);
	size += (disks - 1) * sizeof(struct imsm_disk);
	size += 2 * sizeof(struct imsm_dev);
	/* up to 2 maps per raid device (-2 for imsm_maps in imsm_dev */
	size += (4 - 2) * sizeof(struct imsm_map);
	/* 4 possible disk_ord_tbl's */
	size += 4 * (disks - 1) * sizeof(__u32);

	return size;
}

static __u64 avail_size_imsm(struct supertype *st, __u64 devsize)
{
	if (devsize < (MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS))
		return 0;

	return devsize - (MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS);
}

static int compare_super_imsm(struct supertype *st, struct supertype *tst)
{
	/*
	 * return:
	 *  0 same, or first was empty, and second was copied
	 *  1 second had wrong number
	 *  2 wrong uuid
	 *  3 wrong other info
	 */
	struct intel_super *first = st->sb;
	struct intel_super *sec = tst->sb;

        if (!first) {
                st->sb = tst->sb;
                tst->sb = NULL;
                return 0;
        }

	if (memcmp(first->anchor->sig, sec->anchor->sig, MAX_SIGNATURE_LENGTH) != 0)
		return 3;

	/* if an anchor does not have num_raid_devs set then it is a free
	 * floating spare
	 */
	if (first->anchor->num_raid_devs > 0 &&
	    sec->anchor->num_raid_devs > 0) {
		if (first->anchor->family_num != sec->anchor->family_num)
			return 3;
	}

	/* if 'first' is a spare promote it to a populated mpb with sec's
	 * family number
	 */
	if (first->anchor->num_raid_devs == 0 &&
	    sec->anchor->num_raid_devs > 0) {
		int i;

		/* we need to copy raid device info from sec if an allocation
		 * fails here we don't associate the spare
		 */
		for (i = 0; i < sec->anchor->num_raid_devs; i++) {
			first->dev_tbl[i] = malloc(sizeof(struct imsm_dev));
			if (!first->dev_tbl) {
				while (--i >= 0) {
					free(first->dev_tbl[i]);
					first->dev_tbl[i] = NULL;
				}
				fprintf(stderr, "imsm: failed to associate spare\n"); 
				return 3;
			}
			*first->dev_tbl[i] = *sec->dev_tbl[i];
		}

		first->anchor->num_raid_devs = sec->anchor->num_raid_devs;
		first->anchor->family_num = sec->anchor->family_num;
	}

	return 0;
}

static void fd2devname(int fd, char *name)
{
	struct stat st;
	char path[256];
	char dname[100];
	char *nm;
	int rv;

	name[0] = '\0';
	if (fstat(fd, &st) != 0)
		return;
	sprintf(path, "/sys/dev/block/%d:%d",
		major(st.st_rdev), minor(st.st_rdev));

	rv = readlink(path, dname, sizeof(dname));
	if (rv <= 0)
		return;
	
	dname[rv] = '\0';
	nm = strrchr(dname, '/');
	nm++;
	snprintf(name, MAX_RAID_SERIAL_LEN, "/dev/%s", nm);
}


extern int scsi_get_serial(int fd, void *buf, size_t buf_len);

static int imsm_read_serial(int fd, char *devname,
			    __u8 serial[MAX_RAID_SERIAL_LEN])
{
	unsigned char scsi_serial[255];
	int rv;
	int rsp_len;
	int len;
	char *c, *rsp_buf;

	memset(scsi_serial, 0, sizeof(scsi_serial));

	rv = scsi_get_serial(fd, scsi_serial, sizeof(scsi_serial));

	if (rv && check_env("IMSM_DEVNAME_AS_SERIAL")) {
		memset(serial, 0, MAX_RAID_SERIAL_LEN);
		fd2devname(fd, (char *) serial);
		return 0;
	}

	if (rv != 0) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to retrieve serial for %s\n",
				devname);
		return rv;
	}

	/* trim leading whitespace */
	rsp_len = scsi_serial[3];
	rsp_buf = (char *) &scsi_serial[4];
	c = rsp_buf;
	while (isspace(*c))
		c++;

	/* truncate len to the end of rsp_buf if necessary */
	if (c + MAX_RAID_SERIAL_LEN > rsp_buf + rsp_len)
		len = rsp_len - (c - rsp_buf);
	else
		len = MAX_RAID_SERIAL_LEN;

	/* initialize the buffer and copy rsp_buf characters */
	memset(serial, 0, MAX_RAID_SERIAL_LEN);
	memcpy(serial, c, len);

	/* trim trailing whitespace starting with the last character copied */
	c = (char *) &serial[len - 1];
	while (isspace(*c) || *c == '\0')
		*c-- = '\0';

	return 0;
}

static int serialcmp(__u8 *s1, __u8 *s2)
{
	return strncmp((char *) s1, (char *) s2, MAX_RAID_SERIAL_LEN);
}

static void serialcpy(__u8 *dest, __u8 *src)
{
	strncpy((char *) dest, (char *) src, MAX_RAID_SERIAL_LEN);
}

static int
load_imsm_disk(int fd, struct intel_super *super, char *devname, int keep_fd)
{
	struct dl *dl;
	struct stat stb;
	int rv;
	int i;
	int alloc = 1;
	__u8 serial[MAX_RAID_SERIAL_LEN];

	rv = imsm_read_serial(fd, devname, serial);

	if (rv != 0)
		return 2;

	/* check if this is a disk we have seen before.  it may be a spare in
	 * super->disks while the current anchor believes it is a raid member,
	 * check if we need to update dl->index
	 */
	for (dl = super->disks; dl; dl = dl->next)
		if (serialcmp(dl->serial, serial) == 0)
			break;

	if (!dl)
		dl = malloc(sizeof(*dl));
	else
		alloc = 0;

	if (!dl) {
		if (devname)
			fprintf(stderr,
				Name ": failed to allocate disk buffer for %s\n",
				devname);
		return 2;
	}

	if (alloc) {
		fstat(fd, &stb);
		dl->major = major(stb.st_rdev);
		dl->minor = minor(stb.st_rdev);
		dl->next = super->disks;
		dl->fd = keep_fd ? fd : -1;
		dl->devname = devname ? strdup(devname) : NULL;
		serialcpy(dl->serial, serial);
		dl->index = -2;
	} else if (keep_fd) {
		close(dl->fd);
		dl->fd = fd;
	}

	/* look up this disk's index in the current anchor */
	for (i = 0; i < super->anchor->num_disks; i++) {
		struct imsm_disk *disk_iter;

		disk_iter = __get_imsm_disk(super->anchor, i);

		if (serialcmp(disk_iter->serial, dl->serial) == 0) {
			dl->disk = *disk_iter;
			/* only set index on disks that are a member of a
			 * populated contianer, i.e. one with raid_devs
			 */
			if (dl->disk.status & FAILED_DISK)
				dl->index = -2;
			else if (dl->disk.status & SPARE_DISK)
				dl->index = -1;
			else
				dl->index = i;

			break;
		}
	}

	/* no match, maybe a stale failed drive */
	if (i == super->anchor->num_disks && dl->index >= 0) {
		dl->disk = *__get_imsm_disk(super->anchor, dl->index);
		if (dl->disk.status & FAILED_DISK)
			dl->index = -2;
	}

	if (alloc)
		super->disks = dl;

	return 0;
}

static void imsm_copy_dev(struct imsm_dev *dest, struct imsm_dev *src)
{
	memcpy(dest, src, sizeof_imsm_dev(src, 0));
}

#ifndef MDASSEMBLE
/* When migrating map0 contains the 'destination' state while map1
 * contains the current state.  When not migrating map0 contains the
 * current state.  This routine assumes that map[0].map_state is set to
 * the current array state before being called.
 *
 * Migration is indicated by one of the following states
 * 1/ Idle (migr_state=0 map0state=normal||unitialized||degraded||failed)
 * 2/ Initialize (migr_state=1 migr_type=MIGR_INIT map0state=normal
 *    map1state=unitialized)
 * 3/ Verify (Resync) (migr_state=1 migr_type=MIGR_REBUILD map0state=normal
 *    map1state=normal)
 * 4/ Rebuild (migr_state=1 migr_type=MIGR_REBUILD map0state=normal
 *    map1state=degraded)
 */
static void migrate(struct imsm_dev *dev, __u8 to_state, int rebuild_resync)
{
	struct imsm_map *dest;
	struct imsm_map *src = get_imsm_map(dev, 0);

	dev->vol.migr_state = 1;
	dev->vol.migr_type = rebuild_resync;
	dev->vol.curr_migr_unit = 0;
	dest = get_imsm_map(dev, 1);

	memcpy(dest, src, sizeof_imsm_map(src));
	src->map_state = to_state;
}

static void end_migration(struct imsm_dev *dev, __u8 map_state)
{
	struct imsm_map *map = get_imsm_map(dev, 0);

	dev->vol.migr_state = 0;
	dev->vol.curr_migr_unit = 0;
	map->map_state = map_state;
}
#endif

static int parse_raid_devices(struct intel_super *super)
{
	int i;
	struct imsm_dev *dev_new;
	size_t len, len_migr;
	size_t space_needed = 0;
	struct imsm_super *mpb = super->anchor;

	for (i = 0; i < super->anchor->num_raid_devs; i++) {
		struct imsm_dev *dev_iter = __get_imsm_dev(super->anchor, i);

		len = sizeof_imsm_dev(dev_iter, 0);
		len_migr = sizeof_imsm_dev(dev_iter, 1);
		if (len_migr > len)
			space_needed += len_migr - len;
		
		dev_new = malloc(len_migr);
		if (!dev_new)
			return 1;
		imsm_copy_dev(dev_new, dev_iter);
		super->dev_tbl[i] = dev_new;
	}

	/* ensure that super->buf is large enough when all raid devices
	 * are migrating
	 */
	if (__le32_to_cpu(mpb->mpb_size) + space_needed > super->len) {
		void *buf;

		len = ROUND_UP(__le32_to_cpu(mpb->mpb_size) + space_needed, 512);
		if (posix_memalign(&buf, 512, len) != 0)
			return 1;

		memcpy(buf, super->buf, len);
		free(super->buf);
		super->buf = buf;
		super->len = len;
	}
		
	return 0;
}

/* retrieve a pointer to the bbm log which starts after all raid devices */
struct bbm_log *__get_imsm_bbm_log(struct imsm_super *mpb)
{
	void *ptr = NULL;

	if (__le32_to_cpu(mpb->bbm_log_size)) {
		ptr = mpb;
		ptr += mpb->mpb_size - __le32_to_cpu(mpb->bbm_log_size);
	} 

	return ptr;
}

static void __free_imsm(struct intel_super *super, int free_disks);

/* load_imsm_mpb - read matrix metadata
 * allocates super->mpb to be freed by free_super
 */
static int load_imsm_mpb(int fd, struct intel_super *super, char *devname)
{
	unsigned long long dsize;
	unsigned long long sectors;
	struct stat;
	struct imsm_super *anchor;
	__u32 check_sum;
	int rc;

	get_dev_size(fd, NULL, &dsize);

	if (lseek64(fd, dsize - (512 * 2), SEEK_SET) < 0) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot seek to anchor block on %s: %s\n",
				devname, strerror(errno));
		return 1;
	}

	if (posix_memalign((void**)&anchor, 512, 512) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to allocate imsm anchor buffer"
				" on %s\n", devname);
		return 1;
	}
	if (read(fd, anchor, 512) != 512) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot read anchor block on %s: %s\n",
				devname, strerror(errno));
		free(anchor);
		return 1;
	}

	if (strncmp((char *) anchor->sig, MPB_SIGNATURE, MPB_SIG_LEN) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": no IMSM anchor on %s\n", devname);
		free(anchor);
		return 2;
	}

	__free_imsm(super, 0);
	super->len = ROUND_UP(anchor->mpb_size, 512);
	if (posix_memalign(&super->buf, 512, super->len) != 0) {
		if (devname)
			fprintf(stderr,
				Name ": unable to allocate %zu byte mpb buffer\n",
				super->len);
		free(anchor);
		return 2;
	}
	memcpy(super->buf, anchor, 512);

	sectors = mpb_sectors(anchor) - 1;
	free(anchor);
	if (!sectors) {
		rc = load_imsm_disk(fd, super, devname, 0);
		if (rc == 0)
			rc = parse_raid_devices(super);
		return rc;
	}

	/* read the extended mpb */
	if (lseek64(fd, dsize - (512 * (2 + sectors)), SEEK_SET) < 0) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot seek to extended mpb on %s: %s\n",
				devname, strerror(errno));
		return 1;
	}

	if (read(fd, super->buf + 512, super->len - 512) != super->len - 512) {
		if (devname)
			fprintf(stderr,
				Name ": Cannot read extended mpb on %s: %s\n",
				devname, strerror(errno));
		return 2;
	}

	check_sum = __gen_imsm_checksum(super->anchor);
	if (check_sum != __le32_to_cpu(super->anchor->check_sum)) {
		if (devname)
			fprintf(stderr,
				Name ": IMSM checksum %x != %x on %s\n",
				check_sum, __le32_to_cpu(super->anchor->check_sum),
				devname);
		return 2;
	}

	/* FIXME the BBM log is disk specific so we cannot use this global
	 * buffer for all disks.  Ok for now since we only look at the global
	 * bbm_log_size parameter to gate assembly
	 */
	super->bbm_log = __get_imsm_bbm_log(super->anchor);

	rc = load_imsm_disk(fd, super, devname, 0);
	if (rc == 0)
		rc = parse_raid_devices(super);

	return rc;
}

static void __free_imsm_disk(struct dl *d)
{
	if (d->fd >= 0)
		close(d->fd);
	if (d->devname)
		free(d->devname);
	free(d);

}
static void free_imsm_disks(struct intel_super *super)
{
	struct dl *d;

	while (super->disks) {
		d = super->disks;
		super->disks = d->next;
		__free_imsm_disk(d);
	}
	while (super->missing) {
		d = super->missing;
		super->missing = d->next;
		__free_imsm_disk(d);
	}

}

/* free all the pieces hanging off of a super pointer */
static void __free_imsm(struct intel_super *super, int free_disks)
{
	int i;

	if (super->buf) {
		free(super->buf);
		super->buf = NULL;
	}
	if (free_disks)
		free_imsm_disks(super);
	for (i = 0; i < IMSM_MAX_RAID_DEVS; i++)
		if (super->dev_tbl[i]) {
			free(super->dev_tbl[i]);
			super->dev_tbl[i] = NULL;
		}
}

static void free_imsm(struct intel_super *super)
{
	__free_imsm(super, 1);
	free(super);
}

static void free_super_imsm(struct supertype *st)
{
	struct intel_super *super = st->sb;

	if (!super)
		return;

	free_imsm(super);
	st->sb = NULL;
}

static struct intel_super *alloc_super(int creating_imsm)
{
	struct intel_super *super = malloc(sizeof(*super));

	if (super) {
		memset(super, 0, sizeof(*super));
		super->creating_imsm = creating_imsm;
		super->current_vol = -1;
	}

	return super;
}

#ifndef MDASSEMBLE
/* find_missing - helper routine for load_super_imsm_all that identifies
 * disks that have disappeared from the system.  This routine relies on
 * the mpb being uptodate, which it is at load time.
 */
static int find_missing(struct intel_super *super)
{
	int i;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	struct imsm_disk *disk;

	for (i = 0; i < mpb->num_disks; i++) {
		disk = __get_imsm_disk(mpb, i);
		for (dl = super->disks; dl; dl = dl->next)
			if (serialcmp(dl->disk.serial, disk->serial) == 0)
				break;
		if (dl)
			continue;
		/* ok we have a 'disk' without a live entry in
		 * super->disks
		 */
		if (disk->status & FAILED_DISK || !(disk->status & USABLE_DISK))
			continue; /* never mind, already marked */

		dl = malloc(sizeof(*dl));
		if (!dl)
			return 1;
		dl->major = 0;
		dl->minor = 0;
		dl->fd = -1;
		dl->devname = strdup("missing");
		dl->index = i;
		serialcpy(dl->serial, disk->serial);
		dl->disk = *disk;
		dl->next = super->missing;
		super->missing = dl;
	}

	return 0;
}

static int load_super_imsm_all(struct supertype *st, int fd, void **sbp,
			       char *devname, int keep_fd)
{
	struct mdinfo *sra;
	struct intel_super *super;
	struct mdinfo *sd, *best = NULL;
	__u32 bestgen = 0;
	__u32 gen;
	char nm[20];
	int dfd;
	int rv;

	/* check if this disk is a member of an active array */
	sra = sysfs_read(fd, 0, GET_LEVEL|GET_VERSION|GET_DEVS|GET_STATE);
	if (!sra)
		return 1;

	if (sra->array.major_version != -1 ||
	    sra->array.minor_version != -2 ||
	    strcmp(sra->text_version, "imsm") != 0)
		return 1;

	super = alloc_super(0);
	if (!super)
		return 1;

	/* find the most up to date disk in this array, skipping spares */
	for (sd = sra->devs; sd; sd = sd->next) {
		sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
		dfd = dev_open(nm, keep_fd ? O_RDWR : O_RDONLY);
		if (!dfd) {
			free_imsm(super);
			return 2;
		}
		rv = load_imsm_mpb(dfd, super, NULL);
		if (!keep_fd)
			close(dfd);
		if (rv == 0) {
			if (super->anchor->num_raid_devs == 0)
				gen = 0;
			else
				gen = __le32_to_cpu(super->anchor->generation_num);
			if (!best || gen > bestgen) {
				bestgen = gen;
				best = sd;
			}
		} else {
			free_imsm(super);
			return 2;
		}
	}

	if (!best) {
		free_imsm(super);
		return 1;
	}

	/* load the most up to date anchor */
	sprintf(nm, "%d:%d", best->disk.major, best->disk.minor);
	dfd = dev_open(nm, O_RDONLY);
	if (!dfd) {
		free_imsm(super);
		return 1;
	}
	rv = load_imsm_mpb(dfd, super, NULL);
	close(dfd);
	if (rv != 0) {
		free_imsm(super);
		return 2;
	}

	/* re-parse the disk list with the current anchor */
	for (sd = sra->devs ; sd ; sd = sd->next) {
		sprintf(nm, "%d:%d", sd->disk.major, sd->disk.minor);
		dfd = dev_open(nm, keep_fd? O_RDWR : O_RDONLY);
		if (!dfd) {
			free_imsm(super);
			return 2;
		}
		load_imsm_disk(dfd, super, NULL, keep_fd);
		if (!keep_fd)
			close(dfd);
	}


	if (find_missing(super) != 0) {
		free_imsm(super);
		return 2;
	}

	if (st->subarray[0]) {
		if (atoi(st->subarray) <= super->anchor->num_raid_devs)
			super->current_vol = atoi(st->subarray);
		else
			return 1;
	}

	*sbp = super;
	st->container_dev = fd2devnum(fd);
	if (st->ss == NULL) {
		st->ss = &super_imsm;
		st->minor_version = 0;
		st->max_devs = IMSM_MAX_DEVICES;
	}
	st->loaded_container = 1;

	return 0;
}
#endif

static int load_super_imsm(struct supertype *st, int fd, char *devname)
{
	struct intel_super *super;
	int rv;

#ifndef MDASSEMBLE
	if (load_super_imsm_all(st, fd, &st->sb, devname, 1) == 0)
		return 0;
#endif
	if (st->subarray[0])
		return 1; /* FIXME */

	super = alloc_super(0);
	if (!super) {
		fprintf(stderr,
			Name ": malloc of %zu failed.\n",
			sizeof(*super));
		return 1;
	}

	rv = load_imsm_mpb(fd, super, devname);

	if (rv) {
		if (devname)
			fprintf(stderr,
				Name ": Failed to load all information "
				"sections on %s\n", devname);
		free_imsm(super);
		return rv;
	}

	st->sb = super;
	if (st->ss == NULL) {
		st->ss = &super_imsm;
		st->minor_version = 0;
		st->max_devs = IMSM_MAX_DEVICES;
	}
	st->loaded_container = 0;

	return 0;
}

static __u16 info_to_blocks_per_strip(mdu_array_info_t *info)
{
	if (info->level == 1)
		return 128;
	return info->chunk_size >> 9;
}

static __u32 info_to_num_data_stripes(mdu_array_info_t *info)
{
	__u32 num_stripes;

	num_stripes = (info->size * 2) / info_to_blocks_per_strip(info);
	if (info->level == 1)
		num_stripes /= 2;

	return num_stripes;
}

static __u32 info_to_blocks_per_member(mdu_array_info_t *info)
{
	return (info->size * 2) & ~(info_to_blocks_per_strip(info) - 1);
}

static void imsm_update_version_info(struct intel_super *super)
{
	/* update the version and attributes */
	struct imsm_super *mpb = super->anchor;
	char *version;
	struct imsm_dev *dev;
	struct imsm_map *map;
	int i;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		dev = get_imsm_dev(super, i);
		map = get_imsm_map(dev, 0);
		if (__le32_to_cpu(dev->size_high) > 0)
			mpb->attributes |= MPB_ATTRIB_2TB;

		/* FIXME detect when an array spans a port multiplier */
		#if 0
		mpb->attributes |= MPB_ATTRIB_PM;
		#endif

		if (mpb->num_raid_devs > 1 ||
		    mpb->attributes != MPB_ATTRIB_CHECKSUM_VERIFY) {
			version = MPB_VERSION_ATTRIBS;
			switch (get_imsm_raid_level(map)) {
			case 0: mpb->attributes |= MPB_ATTRIB_RAID0; break;
			case 1: mpb->attributes |= MPB_ATTRIB_RAID1; break;
			case 10: mpb->attributes |= MPB_ATTRIB_RAID10; break;
			case 5: mpb->attributes |= MPB_ATTRIB_RAID5; break;
			}
		} else {
			if (map->num_members >= 5)
				version = MPB_VERSION_5OR6_DISK_ARRAY;
			else if (dev->status == DEV_CLONE_N_GO)
				version = MPB_VERSION_CNG;
			else if (get_imsm_raid_level(map) == 5)
				version = MPB_VERSION_RAID5;
			else if (map->num_members >= 3)
				version = MPB_VERSION_3OR4_DISK_ARRAY;
			else if (get_imsm_raid_level(map) == 1)
				version = MPB_VERSION_RAID1;
			else
				version = MPB_VERSION_RAID0;
		}
		strcpy(((char *) mpb->sig) + strlen(MPB_SIGNATURE), version);
	}
}

static int init_super_imsm_volume(struct supertype *st, mdu_array_info_t *info,
				  unsigned long long size, char *name,
				  char *homehost, int *uuid)
{
	/* We are creating a volume inside a pre-existing container.
	 * so st->sb is already set.
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct imsm_dev *dev;
	struct imsm_vol *vol;
	struct imsm_map *map;
	int idx = mpb->num_raid_devs;
	int i;
	unsigned long long array_blocks;
	__u32 offset = 0;
	size_t size_old, size_new;

	if (mpb->num_raid_devs >= 2) {
		fprintf(stderr, Name": This imsm-container already has the "
			"maximum of 2 volumes\n");
		return 0;
	}

	/* ensure the mpb is large enough for the new data */
	size_old = __le32_to_cpu(mpb->mpb_size);
	size_new = disks_to_mpb_size(info->nr_disks);
	if (size_new > size_old) {
		void *mpb_new;
		size_t size_round = ROUND_UP(size_new, 512);

		if (posix_memalign(&mpb_new, 512, size_round) != 0) {
			fprintf(stderr, Name": could not allocate new mpb\n");
			return 0;
		}
		memcpy(mpb_new, mpb, size_old);
		free(mpb);
		mpb = mpb_new;
		super->anchor = mpb_new;
		mpb->mpb_size = __cpu_to_le32(size_new);
		memset(mpb_new + size_old, 0, size_round - size_old);
	}
	super->current_vol = idx;
	/* when creating the first raid device in this container set num_disks
	 * to zero, i.e. delete this spare and add raid member devices in
	 * add_to_super_imsm_volume()
	 */
	if (super->current_vol == 0)
		mpb->num_disks = 0;
	sprintf(st->subarray, "%d", idx);
	dev = malloc(sizeof(*dev) + sizeof(__u32) * (info->raid_disks - 1));
	if (!dev) {
		fprintf(stderr, Name": could not allocate raid device\n");
		return 0;
	}
	strncpy((char *) dev->volume, name, MAX_RAID_SERIAL_LEN);
	array_blocks = calc_array_size(info->level, info->raid_disks,
				       info->layout, info->chunk_size,
				       info->size*2);
	dev->size_low = __cpu_to_le32((__u32) array_blocks);
	dev->size_high = __cpu_to_le32((__u32) (array_blocks >> 32));
	dev->status = __cpu_to_le32(0);
	dev->reserved_blocks = __cpu_to_le32(0);
	vol = &dev->vol;
	vol->migr_state = 0;
	vol->migr_type = MIGR_INIT;
	vol->dirty = 0;
	vol->curr_migr_unit = 0;
	for (i = 0; i < idx; i++) {
		struct imsm_dev *prev = get_imsm_dev(super, i);
		struct imsm_map *pmap = get_imsm_map(prev, 0);

		offset += __le32_to_cpu(pmap->blocks_per_member);
		offset += IMSM_RESERVED_SECTORS;
	}
	map = get_imsm_map(dev, 0);
	map->pba_of_lba0 = __cpu_to_le32(offset);
	map->blocks_per_member = __cpu_to_le32(info_to_blocks_per_member(info));
	map->blocks_per_strip = __cpu_to_le16(info_to_blocks_per_strip(info));
	map->num_data_stripes = __cpu_to_le32(info_to_num_data_stripes(info));
	map->map_state = info->level ? IMSM_T_STATE_UNINITIALIZED :
				       IMSM_T_STATE_NORMAL;

	if (info->level == 1 && info->raid_disks > 2) {
		fprintf(stderr, Name": imsm does not support more than 2 disks"
				"in a raid1 volume\n");
		return 0;
	}
	if (info->level == 10) {
		map->raid_level = 1;
		map->num_domains = info->raid_disks / 2;
	} else {
		map->raid_level = info->level;
		map->num_domains = !!map->raid_level;
	}

	map->num_members = info->raid_disks;
	for (i = 0; i < map->num_members; i++) {
		/* initialized in add_to_super */
		set_imsm_ord_tbl_ent(map, i, 0);
	}
	mpb->num_raid_devs++;
	super->dev_tbl[super->current_vol] = dev;

	imsm_update_version_info(super);

	return 1;
}

static int init_super_imsm(struct supertype *st, mdu_array_info_t *info,
			   unsigned long long size, char *name,
			   char *homehost, int *uuid)
{
	/* This is primarily called by Create when creating a new array.
	 * We will then get add_to_super called for each component, and then
	 * write_init_super called to write it out to each device.
	 * For IMSM, Create can create on fresh devices or on a pre-existing
	 * array.
	 * To create on a pre-existing array a different method will be called.
	 * This one is just for fresh drives.
	 */
	struct intel_super *super;
	struct imsm_super *mpb;
	size_t mpb_size;
	char *version;

	if (!info) {
		st->sb = NULL;
		return 0;
	}
	if (st->sb)
		return init_super_imsm_volume(st, info, size, name, homehost,
					      uuid);

	super = alloc_super(1);
	if (!super)
		return 0;
	mpb_size = disks_to_mpb_size(info->nr_disks);
	if (posix_memalign(&super->buf, 512, mpb_size) != 0) {
		free(super);
		return 0;
	}
	mpb = super->buf;
	memset(mpb, 0, mpb_size); 

	mpb->attributes = MPB_ATTRIB_CHECKSUM_VERIFY;

	version = (char *) mpb->sig;
	strcpy(version, MPB_SIGNATURE);
	version += strlen(MPB_SIGNATURE);
	strcpy(version, MPB_VERSION_RAID0);
	mpb->mpb_size = mpb_size;

	st->sb = super;
	return 1;
}

#ifndef MDASSEMBLE
static int add_to_super_imsm_volume(struct supertype *st, mdu_disk_info_t *dk,
				     int fd, char *devname)
{
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct dl *dl;
	struct imsm_dev *dev;
	struct imsm_map *map;

	dev = get_imsm_dev(super, super->current_vol);
	map = get_imsm_map(dev, 0);

	if (! (dk->state & (1<<MD_DISK_SYNC))) {
		fprintf(stderr, Name ": %s: Cannot add spare devices to IMSM volume\n",
			devname);
		return 1;
	}

	for (dl = super->disks; dl ; dl = dl->next)
		if (dl->major == dk->major &&
		    dl->minor == dk->minor)
			break;

	if (!dl) {
		fprintf(stderr, Name ": %s is not a member of the same container\n", devname);
		return 1;
	}

	/* add a pristine spare to the metadata */
	if (dl->index < 0) {
		dl->index = super->anchor->num_disks;
		super->anchor->num_disks++;
	}
	set_imsm_ord_tbl_ent(map, dk->number, dl->index);
	dl->disk.status = CONFIGURED_DISK | USABLE_DISK;

	/* if we are creating the first raid device update the family number */
	if (super->current_vol == 0) {
		__u32 sum;
		struct imsm_dev *_dev = __get_imsm_dev(mpb, 0);
		struct imsm_disk *_disk = __get_imsm_disk(mpb, dl->index);

		*_dev = *dev;
		*_disk = dl->disk;
		sum = __gen_imsm_checksum(mpb);
		mpb->family_num = __cpu_to_le32(sum);
	}

	return 0;
}

static int add_to_super_imsm(struct supertype *st, mdu_disk_info_t *dk,
			      int fd, char *devname)
{
	struct intel_super *super = st->sb;
	struct dl *dd;
	unsigned long long size;
	__u32 id;
	int rv;
	struct stat stb;

	if (super->current_vol >= 0)
		return add_to_super_imsm_volume(st, dk, fd, devname);

	fstat(fd, &stb);
	dd = malloc(sizeof(*dd));
	if (!dd) {
		fprintf(stderr,
			Name ": malloc failed %s:%d.\n", __func__, __LINE__);
		return 1;
	}
	memset(dd, 0, sizeof(*dd));
	dd->major = major(stb.st_rdev);
	dd->minor = minor(stb.st_rdev);
	dd->index = -1;
	dd->devname = devname ? strdup(devname) : NULL;
	dd->fd = fd;
	rv = imsm_read_serial(fd, devname, dd->serial);
	if (rv) {
		fprintf(stderr,
			Name ": failed to retrieve scsi serial, aborting\n");
		free(dd);
		abort();
	}

	get_dev_size(fd, NULL, &size);
	size /= 512;
	serialcpy(dd->disk.serial, dd->serial);
	dd->disk.total_blocks = __cpu_to_le32(size);
	dd->disk.status = USABLE_DISK | SPARE_DISK;
	if (sysfs_disk_to_scsi_id(fd, &id) == 0)
		dd->disk.scsi_id = __cpu_to_le32(id);
	else
		dd->disk.scsi_id = __cpu_to_le32(0);

	if (st->update_tail) {
		dd->next = super->add;
		super->add = dd;
	} else {
		dd->next = super->disks;
		super->disks = dd;
	}

	return 0;
}

static int store_imsm_mpb(int fd, struct intel_super *super);

/* spare records have their own family number and do not have any defined raid
 * devices
 */
static int write_super_imsm_spares(struct intel_super *super, int doclose)
{
	struct imsm_super mpb_save;
	struct imsm_super *mpb = super->anchor;
	__u32 sum;
	struct dl *d;

	mpb_save = *mpb;
	mpb->num_raid_devs = 0;
	mpb->num_disks = 1;
	mpb->mpb_size = sizeof(struct imsm_super);
	mpb->generation_num = __cpu_to_le32(1UL);

	for (d = super->disks; d; d = d->next) {
		if (d->index != -1)
			continue;

		mpb->disk[0] = d->disk;
		sum = __gen_imsm_checksum(mpb);
		mpb->family_num = __cpu_to_le32(sum);
		sum = __gen_imsm_checksum(mpb);
		mpb->check_sum = __cpu_to_le32(sum);

		if (store_imsm_mpb(d->fd, super)) {
			fprintf(stderr, "%s: failed for device %d:%d %s\n",
				__func__, d->major, d->minor, strerror(errno));
			*mpb = mpb_save;
			return 1;
		}
		if (doclose) {
			close(d->fd);
			d->fd = -1;
		}
	}

	*mpb = mpb_save;
	return 0;
}

static int write_super_imsm(struct intel_super *super, int doclose)
{
	struct imsm_super *mpb = super->anchor;
	struct dl *d;
	__u32 generation;
	__u32 sum;
	int spares = 0;
	int i;
	__u32 mpb_size = sizeof(struct imsm_super) - sizeof(struct imsm_disk);

	/* 'generation' is incremented everytime the metadata is written */
	generation = __le32_to_cpu(mpb->generation_num);
	generation++;
	mpb->generation_num = __cpu_to_le32(generation);

	mpb_size += sizeof(struct imsm_disk) * mpb->num_disks;
	for (d = super->disks; d; d = d->next) {
		if (d->index == -1)
			spares++;
		else
			mpb->disk[d->index] = d->disk;
	}
	for (d = super->missing; d; d = d->next)
		mpb->disk[d->index] = d->disk;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = __get_imsm_dev(mpb, i);

		imsm_copy_dev(dev, super->dev_tbl[i]);
		mpb_size += sizeof_imsm_dev(dev, 0);
	}
	mpb_size += __le32_to_cpu(mpb->bbm_log_size);
	mpb->mpb_size = __cpu_to_le32(mpb_size);

	/* recalculate checksum */
	sum = __gen_imsm_checksum(mpb);
	mpb->check_sum = __cpu_to_le32(sum);

	/* write the mpb for disks that compose raid devices */
	for (d = super->disks; d ; d = d->next) {
		if (d->index < 0)
			continue;
		if (store_imsm_mpb(d->fd, super))
			fprintf(stderr, "%s: failed for device %d:%d %s\n",
				__func__, d->major, d->minor, strerror(errno));
		if (doclose) {
			close(d->fd);
			d->fd = -1;
		}
	}

	if (spares)
		return write_super_imsm_spares(super, doclose);

	return 0;
}


static int create_array(struct supertype *st)
{
	size_t len;
	struct imsm_update_create_array *u;
	struct intel_super *super = st->sb;
	struct imsm_dev *dev = get_imsm_dev(super, super->current_vol);

	len = sizeof(*u) - sizeof(*dev) + sizeof_imsm_dev(dev, 0);
	u = malloc(len);
	if (!u) {
		fprintf(stderr, "%s: failed to allocate update buffer\n",
			__func__);
		return 1;
	}

	u->type = update_create_array;
	u->dev_idx = super->current_vol;
	imsm_copy_dev(&u->dev, dev);
	append_metadata_update(st, u, len);

	return 0;
}

static int _add_disk(struct supertype *st)
{
	struct intel_super *super = st->sb;
	size_t len;
	struct imsm_update_add_disk *u;

	if (!super->add)
		return 0;

	len = sizeof(*u);
	u = malloc(len);
	if (!u) {
		fprintf(stderr, "%s: failed to allocate update buffer\n",
			__func__);
		return 1;
	}

	u->type = update_add_disk;
	append_metadata_update(st, u, len);

	return 0;
}

static int write_init_super_imsm(struct supertype *st)
{
	if (st->update_tail) {
		/* queue the recently created array / added disk
		 * as a metadata update */
		struct intel_super *super = st->sb;
		struct dl *d;
		int rv;

		/* determine if we are creating a volume or adding a disk */
		if (super->current_vol < 0) {
			/* in the add disk case we are running in mdmon
			 * context, so don't close fd's
			 */
			return _add_disk(st);
		} else
			rv = create_array(st);

		for (d = super->disks; d ; d = d->next) {
			close(d->fd);
			d->fd = -1;
		}

		return rv;
	} else
		return write_super_imsm(st->sb, 1);
}
#endif

static int store_zero_imsm(struct supertype *st, int fd)
{
	unsigned long long dsize;
	void *buf;

	get_dev_size(fd, NULL, &dsize);

	/* first block is stored on second to last sector of the disk */
	if (lseek64(fd, dsize - (512 * 2), SEEK_SET) < 0)
		return 1;

	if (posix_memalign(&buf, 512, 512) != 0)
		return 1;

	memset(buf, 0, 512);
	if (write(fd, buf, 512) != 512)
		return 1;
	return 0;
}

static int imsm_bbm_log_size(struct imsm_super *mpb)
{
	return __le32_to_cpu(mpb->bbm_log_size);
}

#ifndef MDASSEMBLE
static int validate_geometry_imsm_container(struct supertype *st, int level,
					    int layout, int raiddisks, int chunk,
					    unsigned long long size, char *dev,
					    unsigned long long *freesize,
					    int verbose)
{
	int fd;
	unsigned long long ldsize;

	if (level != LEVEL_CONTAINER)
		return 0;
	if (!dev)
		return 1;

	fd = open(dev, O_RDONLY|O_EXCL, 0);
	if (fd < 0) {
		if (verbose)
			fprintf(stderr, Name ": imsm: Cannot open %s: %s\n",
				dev, strerror(errno));
		return 0;
	}
	if (!get_dev_size(fd, dev, &ldsize)) {
		close(fd);
		return 0;
	}
	close(fd);

	*freesize = avail_size_imsm(st, ldsize >> 9);

	return 1;
}

/* validate_geometry_imsm_volume - lifted from validate_geometry_ddf_bvd 
 * FIX ME add ahci details
 */
static int validate_geometry_imsm_volume(struct supertype *st, int level,
					 int layout, int raiddisks, int chunk,
					 unsigned long long size, char *dev,
					 unsigned long long *freesize,
					 int verbose)
{
	struct stat stb;
	struct intel_super *super = st->sb;
	struct dl *dl;
	unsigned long long pos = 0;
	unsigned long long maxsize;
	struct extent *e;
	int i;

	if (level == LEVEL_CONTAINER)
		return 0;

	if (level == 1 && raiddisks > 2) {
		if (verbose)
			fprintf(stderr, Name ": imsm does not support more "
				"than 2 in a raid1 configuration\n");
		return 0;
	}

	/* We must have the container info already read in. */
	if (!super)
		return 0;

	if (!dev) {
		/* General test:  make sure there is space for
		 * 'raiddisks' device extents of size 'size' at a given
		 * offset
		 */
		unsigned long long minsize = size;
		unsigned long long start_offset = ~0ULL;
		int dcnt = 0;
		if (minsize == 0)
			minsize = MPB_SECTOR_CNT + IMSM_RESERVED_SECTORS;
		for (dl = super->disks; dl ; dl = dl->next) {
			int found = 0;

			pos = 0;
			i = 0;
			e = get_extents(super, dl);
			if (!e) continue;
			do {
				unsigned long long esize;
				esize = e[i].start - pos;
				if (esize >= minsize)
					found = 1;
				if (found && start_offset == ~0ULL) {
					start_offset = pos;
					break;
				} else if (found && pos != start_offset) {
					found = 0;
					break;
				}
				pos = e[i].start + e[i].size;
				i++;
			} while (e[i-1].size);
			if (found)
				dcnt++;
			free(e);
		}
		if (dcnt < raiddisks) {
			if (verbose)
				fprintf(stderr, Name ": imsm: Not enough "
					"devices with space for this array "
					"(%d < %d)\n",
					dcnt, raiddisks);
			return 0;
		}
		return 1;
	}
	/* This device must be a member of the set */
	if (stat(dev, &stb) < 0)
		return 0;
	if ((S_IFMT & stb.st_mode) != S_IFBLK)
		return 0;
	for (dl = super->disks ; dl ; dl = dl->next) {
		if (dl->major == major(stb.st_rdev) &&
		    dl->minor == minor(stb.st_rdev))
			break;
	}
	if (!dl) {
		if (verbose)
			fprintf(stderr, Name ": %s is not in the "
				"same imsm set\n", dev);
		return 0;
	}
	e = get_extents(super, dl);
	maxsize = 0;
	i = 0;
	if (e) do {
		unsigned long long esize;
		esize = e[i].start - pos;
		if (esize >= maxsize)
			maxsize = esize;
		pos = e[i].start + e[i].size;
		i++;
	} while (e[i-1].size);
	*freesize = maxsize;

	return 1;
}

static int validate_geometry_imsm(struct supertype *st, int level, int layout,
				  int raiddisks, int chunk, unsigned long long size,
				  char *dev, unsigned long long *freesize,
				  int verbose)
{
	int fd, cfd;
	struct mdinfo *sra;

	/* if given unused devices create a container 
	 * if given given devices in a container create a member volume
	 */
	if (level == LEVEL_CONTAINER) {
		/* Must be a fresh device to add to a container */
		return validate_geometry_imsm_container(st, level, layout,
							raiddisks, chunk, size,
							dev, freesize,
							verbose);
	}
	
	if (!dev) {
		if (st->sb && freesize) {
			/* Should do auto-layout here */
			fprintf(stderr, Name ": IMSM does not support auto-layout yet\n");
			return 0;
		}
		return 1;
	}
	if (st->sb) {
		/* creating in a given container */
		return validate_geometry_imsm_volume(st, level, layout,
						     raiddisks, chunk, size,
						     dev, freesize, verbose);
	}

	/* limit creation to the following levels */
	if (!dev)
		switch (level) {
		case 0:
		case 1:
		case 10:
		case 5:
			break;
		default:
			return 1;
		}

	/* This device needs to be a device in an 'imsm' container */
	fd = open(dev, O_RDONLY|O_EXCL, 0);
	if (fd >= 0) {
		if (verbose)
			fprintf(stderr,
				Name ": Cannot create this array on device %s\n",
				dev);
		close(fd);
		return 0;
	}
	if (errno != EBUSY || (fd = open(dev, O_RDONLY, 0)) < 0) {
		if (verbose)
			fprintf(stderr, Name ": Cannot open %s: %s\n",
				dev, strerror(errno));
		return 0;
	}
	/* Well, it is in use by someone, maybe an 'imsm' container. */
	cfd = open_container(fd);
	if (cfd < 0) {
		close(fd);
		if (verbose)
			fprintf(stderr, Name ": Cannot use %s: It is busy\n",
				dev);
		return 0;
	}
	sra = sysfs_read(cfd, 0, GET_VERSION);
	close(fd);
	if (sra && sra->array.major_version == -1 &&
	    strcmp(sra->text_version, "imsm") == 0) {
		/* This is a member of a imsm container.  Load the container
		 * and try to create a volume
		 */
		struct intel_super *super;

		if (load_super_imsm_all(st, cfd, (void **) &super, NULL, 1) == 0) {
			st->sb = super;
			st->container_dev = fd2devnum(cfd);
			close(cfd);
			return validate_geometry_imsm_volume(st, level, layout,
							     raiddisks, chunk,
							     size, dev,
							     freesize, verbose);
		}
		close(cfd);
	} else /* may belong to another container */
		return 0;

	return 1;
}
#endif /* MDASSEMBLE */

static struct mdinfo *container_content_imsm(struct supertype *st)
{
	/* Given a container loaded by load_super_imsm_all,
	 * extract information about all the arrays into
	 * an mdinfo tree.
	 *
	 * For each imsm_dev create an mdinfo, fill it in,
	 *  then look for matching devices in super->disks
	 *  and create appropriate device mdinfo.
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	struct mdinfo *rest = NULL;
	int i;

	/* do not assemble arrays that might have bad blocks */
	if (imsm_bbm_log_size(super->anchor)) {
		fprintf(stderr, Name ": BBM log found in metadata. "
				"Cannot activate array(s).\n");
		return NULL;
	}

	for (i = 0; i < mpb->num_raid_devs; i++) {
		struct imsm_dev *dev = get_imsm_dev(super, i);
		struct imsm_map *map = get_imsm_map(dev, 0);
		struct mdinfo *this;
		int slot;

		this = malloc(sizeof(*this));
		memset(this, 0, sizeof(*this));
		this->next = rest;

		super->current_vol = i;
		getinfo_super_imsm_volume(st, this);
		for (slot = 0 ; slot <  map->num_members; slot++) {
			struct mdinfo *info_d;
			struct dl *d;
			int idx;
			int skip;
			__u32 s;
			__u32 ord;

			skip = 0;
			idx = get_imsm_disk_idx(dev, slot);
			ord = get_imsm_ord_tbl_ent(dev, slot); 
			for (d = super->disks; d ; d = d->next)
				if (d->index == idx)
                                        break;

			if (d == NULL)
				skip = 1;

			s = d ? d->disk.status : 0;
			if (s & FAILED_DISK)
				skip = 1;
			if (!(s & USABLE_DISK))
				skip = 1;
			if (ord & IMSM_ORD_REBUILD)
				skip = 1;

			/* 
			 * if we skip some disks the array will be assmebled degraded;
			 * reset resync start to avoid a dirty-degraded situation
			 *
			 * FIXME handle dirty degraded
			 */
			if (skip && !dev->vol.dirty)
				this->resync_start = ~0ULL;
			if (skip)
				continue;

			info_d = malloc(sizeof(*info_d));
			if (!info_d) {
				fprintf(stderr, Name ": failed to allocate disk"
					" for volume %s\n", (char *) dev->volume);
				free(this);
				this = rest;
				break;
			}
			memset(info_d, 0, sizeof(*info_d));
			info_d->next = this->devs;
			this->devs = info_d;

			info_d->disk.number = d->index;
			info_d->disk.major = d->major;
			info_d->disk.minor = d->minor;
			info_d->disk.raid_disk = slot;

			this->array.working_disks++;

			info_d->events = __le32_to_cpu(mpb->generation_num);
			info_d->data_offset = __le32_to_cpu(map->pba_of_lba0);
			info_d->component_size = __le32_to_cpu(map->blocks_per_member);
			if (d->devname)
				strcpy(info_d->name, d->devname);
		}
		rest = this;
	}

	return rest;
}


#ifndef MDASSEMBLE
static int imsm_open_new(struct supertype *c, struct active_array *a,
			 char *inst)
{
	struct intel_super *super = c->sb;
	struct imsm_super *mpb = super->anchor;
	
	if (atoi(inst) >= mpb->num_raid_devs) {
		fprintf(stderr, "%s: subarry index %d, out of range\n",
			__func__, atoi(inst));
		return -ENODEV;
	}

	dprintf("imsm: open_new %s\n", inst);
	a->info.container_member = atoi(inst);
	return 0;
}

static __u8 imsm_check_degraded(struct intel_super *super, struct imsm_dev *dev, int failed)
{
	struct imsm_map *map = get_imsm_map(dev, 0);

	if (!failed)
		return map->map_state == IMSM_T_STATE_UNINITIALIZED ? 
			IMSM_T_STATE_UNINITIALIZED : IMSM_T_STATE_NORMAL;

	switch (get_imsm_raid_level(map)) {
	case 0:
		return IMSM_T_STATE_FAILED;
		break;
	case 1:
		if (failed < map->num_members)
			return IMSM_T_STATE_DEGRADED;
		else
			return IMSM_T_STATE_FAILED;
		break;
	case 10:
	{
		/**
		 * check to see if any mirrors have failed, otherwise we
		 * are degraded.  Even numbered slots are mirrored on
		 * slot+1
		 */
		int i;
		/* gcc -Os complains that this is unused */
		int insync = insync;

		for (i = 0; i < map->num_members; i++) {
			__u32 ord = get_imsm_ord_tbl_ent(dev, i);
			int idx = ord_to_idx(ord);
			struct imsm_disk *disk;

			/* reset the potential in-sync count on even-numbered
			 * slots.  num_copies is always 2 for imsm raid10 
			 */
			if ((i & 1) == 0)
				insync = 2;

			disk = get_imsm_disk(super, idx);
			if (!disk || disk->status & FAILED_DISK ||
			    ord & IMSM_ORD_REBUILD)
				insync--;

			/* no in-sync disks left in this mirror the
			 * array has failed
			 */
			if (insync == 0)
				return IMSM_T_STATE_FAILED;
		}

		return IMSM_T_STATE_DEGRADED;
	}
	case 5:
		if (failed < 2)
			return IMSM_T_STATE_DEGRADED;
		else
			return IMSM_T_STATE_FAILED;
		break;
	default:
		break;
	}

	return map->map_state;
}

static int imsm_count_failed(struct intel_super *super, struct imsm_dev *dev)
{
	int i;
	int failed = 0;
	struct imsm_disk *disk;
	struct imsm_map *map = get_imsm_map(dev, 0);

	for (i = 0; i < map->num_members; i++) {
		__u32 ord = get_imsm_ord_tbl_ent(dev, i);
		int idx = ord_to_idx(ord);

		disk = get_imsm_disk(super, idx);
		if (!disk || disk->status & FAILED_DISK ||
		    ord & IMSM_ORD_REBUILD)
			failed++;
	}

	return failed;
}

static int is_resyncing(struct imsm_dev *dev)
{
	struct imsm_map *migr_map;

	if (!dev->vol.migr_state)
		return 0;

	if (dev->vol.migr_type == MIGR_INIT)
		return 1;

	migr_map = get_imsm_map(dev, 1);

	if (migr_map->map_state == IMSM_T_STATE_NORMAL)
		return 1;
	else
		return 0;
}

static int is_rebuilding(struct imsm_dev *dev)
{
	struct imsm_map *migr_map;

	if (!dev->vol.migr_state)
		return 0;

	if (dev->vol.migr_type != MIGR_REBUILD)
		return 0;

	migr_map = get_imsm_map(dev, 1);

	if (migr_map->map_state == IMSM_T_STATE_DEGRADED)
		return 1;
	else
		return 0;
}

static void mark_failure(struct imsm_disk *disk)
{
	if (disk->status & FAILED_DISK)
		return;
	disk->status |= FAILED_DISK;
	disk->scsi_id = __cpu_to_le32(~(__u32)0);
	memmove(&disk->serial[0], &disk->serial[1], MAX_RAID_SERIAL_LEN - 1);
}

/* Handle dirty -> clean transititions and resync.  Degraded and rebuild
 * states are handled in imsm_set_disk() with one exception, when a
 * resync is stopped due to a new failure this routine will set the
 * 'degraded' state for the array.
 */
static int imsm_set_array_state(struct active_array *a, int consistent)
{
	int inst = a->info.container_member;
	struct intel_super *super = a->container->sb;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	int failed = imsm_count_failed(super, dev);
	__u8 map_state = imsm_check_degraded(super, dev, failed);

	/* before we activate this array handle any missing disks */
	if (consistent == 2 && super->missing) {
		struct dl *dl;

		dprintf("imsm: mark missing\n");
		end_migration(dev, map_state);
		for (dl = super->missing; dl; dl = dl->next)
			mark_failure(&dl->disk);
		super->updates_pending++;
	}
		
	if (consistent == 2 &&
	    (!is_resync_complete(a) ||
	     map_state != IMSM_T_STATE_NORMAL ||
	     dev->vol.migr_state))
		consistent = 0;

	if (is_resync_complete(a)) {
		/* complete intialization / resync,
		 * recovery is completed in ->set_disk
		 */
		if (is_resyncing(dev)) {
			dprintf("imsm: mark resync done\n");
			end_migration(dev, map_state);
			super->updates_pending++;
		}
	} else if (!is_resyncing(dev) && !failed) {
		/* mark the start of the init process if nothing is failed */
		dprintf("imsm: mark resync start (%llu)\n", a->resync_start);
		if (map->map_state == IMSM_T_STATE_NORMAL)
			migrate(dev, IMSM_T_STATE_NORMAL, MIGR_REBUILD);
		else
			migrate(dev, IMSM_T_STATE_NORMAL, MIGR_INIT);
		super->updates_pending++;
	}

	/* check if we can update the migration checkpoint */
	if (dev->vol.migr_state &&
	    __le32_to_cpu(dev->vol.curr_migr_unit) != a->resync_start) {
		dprintf("imsm: checkpoint migration (%llu)\n", a->resync_start);
		dev->vol.curr_migr_unit = __cpu_to_le32(a->resync_start);
		super->updates_pending++;
	}

	/* mark dirty / clean */
	if (dev->vol.dirty != !consistent) {
		dprintf("imsm: mark '%s' (%llu)\n",
			consistent ? "clean" : "dirty", a->resync_start);
		if (consistent)
			dev->vol.dirty = 0;
		else
			dev->vol.dirty = 1;
		super->updates_pending++;
	}
	return consistent;
}

static void imsm_set_disk(struct active_array *a, int n, int state)
{
	int inst = a->info.container_member;
	struct intel_super *super = a->container->sb;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	struct imsm_disk *disk;
	int failed;
	__u32 ord;
	__u8 map_state;

	if (n > map->num_members)
		fprintf(stderr, "imsm: set_disk %d out of range 0..%d\n",
			n, map->num_members - 1);

	if (n < 0)
		return;

	dprintf("imsm: set_disk %d:%x\n", n, state);

	ord = get_imsm_ord_tbl_ent(dev, n);
	disk = get_imsm_disk(super, ord_to_idx(ord));

	/* check for new failures */
	if ((state & DS_FAULTY) && !(disk->status & FAILED_DISK)) {
		mark_failure(disk);
		super->updates_pending++;
	}

	/* check if in_sync */
	if (state & DS_INSYNC && ord & IMSM_ORD_REBUILD) {
		struct imsm_map *migr_map = get_imsm_map(dev, 1);

		set_imsm_ord_tbl_ent(migr_map, n, ord_to_idx(ord));
		super->updates_pending++;
	}

	failed = imsm_count_failed(super, dev);
	map_state = imsm_check_degraded(super, dev, failed);

	/* check if recovery complete, newly degraded, or failed */
	if (map_state == IMSM_T_STATE_NORMAL && is_rebuilding(dev)) {
		end_migration(dev, map_state);
		super->updates_pending++;
	} else if (map_state == IMSM_T_STATE_DEGRADED &&
		   map->map_state != map_state &&
		   !dev->vol.migr_state) {
		dprintf("imsm: mark degraded\n");
		map->map_state = map_state;
		super->updates_pending++;
	} else if (map_state == IMSM_T_STATE_FAILED &&
		   map->map_state != map_state) {
		dprintf("imsm: mark failed\n");
		end_migration(dev, map_state);
		super->updates_pending++;
	}
}

static int store_imsm_mpb(int fd, struct intel_super *super)
{
	struct imsm_super *mpb = super->anchor;
	__u32 mpb_size = __le32_to_cpu(mpb->mpb_size);
	unsigned long long dsize;
	unsigned long long sectors;

	get_dev_size(fd, NULL, &dsize);

	if (mpb_size > 512) {
		/* -1 to account for anchor */
		sectors = mpb_sectors(mpb) - 1;

		/* write the extended mpb to the sectors preceeding the anchor */
		if (lseek64(fd, dsize - (512 * (2 + sectors)), SEEK_SET) < 0)
			return 1;

		if (write(fd, super->buf + 512, 512 * sectors) != 512 * sectors)
			return 1;
	}

	/* first block is stored on second to last sector of the disk */
	if (lseek64(fd, dsize - (512 * 2), SEEK_SET) < 0)
		return 1;

	if (write(fd, super->buf, 512) != 512)
		return 1;

	return 0;
}

static void imsm_sync_metadata(struct supertype *container)
{
	struct intel_super *super = container->sb;

	if (!super->updates_pending)
		return;

	write_super_imsm(super, 0);

	super->updates_pending = 0;
}

static struct dl *imsm_readd(struct intel_super *super, int idx, struct active_array *a)
{
	struct imsm_dev *dev = get_imsm_dev(super, a->info.container_member);
	int i = get_imsm_disk_idx(dev, idx);
	struct dl *dl;

	for (dl = super->disks; dl; dl = dl->next)
		if (dl->index == i)
			break;

	if (dl && dl->disk.status & FAILED_DISK)
		dl = NULL;

	if (dl)
		dprintf("%s: found %x:%x\n", __func__, dl->major, dl->minor);

	return dl;
}

static struct dl *imsm_add_spare(struct intel_super *super, int slot, struct active_array *a)
{
	struct imsm_dev *dev = get_imsm_dev(super, a->info.container_member);
	int idx = get_imsm_disk_idx(dev, slot);
	struct imsm_map *map = get_imsm_map(dev, 0);
	unsigned long long esize;
	unsigned long long pos;
	struct mdinfo *d;
	struct extent *ex;
	int j;
	int found;
	__u32 array_start;
	struct dl *dl;

	for (dl = super->disks; dl; dl = dl->next) {
		/* If in this array, skip */
		for (d = a->info.devs ; d ; d = d->next)
			if (d->state_fd >= 0 &&
			    d->disk.major == dl->major &&
			    d->disk.minor == dl->minor) {
				dprintf("%x:%x already in array\n", dl->major, dl->minor);
				break;
			}
		if (d)
			continue;

		/* skip in use or failed drives */
		if (dl->disk.status & FAILED_DISK || idx == dl->index) {
			dprintf("%x:%x status ( %s%s)\n",
			dl->major, dl->minor,
			dl->disk.status & FAILED_DISK ? "failed " : "",
			idx == dl->index ? "in use " : "");
			continue;
		}

		/* Does this unused device have the requisite free space?
		 * We need a->info.component_size sectors
		 */
		ex = get_extents(super, dl);
		if (!ex) {
			dprintf("cannot get extents\n");
			continue;
		}
		found = 0;
		j = 0;
		pos = 0;
		array_start = __le32_to_cpu(map->pba_of_lba0);

		do {
			/* check that we can start at pba_of_lba0 with
			 * a->info.component_size of space
			 */
			esize = ex[j].start - pos;
			if (array_start >= pos &&
			    array_start + a->info.component_size < ex[j].start) {
				found = 1;
				break;
			}
			pos = ex[j].start + ex[j].size;
			j++;
			    
		} while (ex[j-1].size);

		free(ex);
		if (!found) {
			dprintf("%x:%x does not have %llu at %d\n",
				dl->major, dl->minor,
				a->info.component_size,
				__le32_to_cpu(map->pba_of_lba0));
			/* No room */
			continue;
		} else
			break;
	}

	return dl;
}

static struct mdinfo *imsm_activate_spare(struct active_array *a,
					  struct metadata_update **updates)
{
	/**
	 * Find a device with unused free space and use it to replace a
	 * failed/vacant region in an array.  We replace failed regions one a
	 * array at a time.  The result is that a new spare disk will be added
	 * to the first failed array and after the monitor has finished
	 * propagating failures the remainder will be consumed.
	 *
	 * FIXME add a capability for mdmon to request spares from another
	 * container.
	 */

	struct intel_super *super = a->container->sb;
	int inst = a->info.container_member;
	struct imsm_dev *dev = get_imsm_dev(super, inst);
	struct imsm_map *map = get_imsm_map(dev, 0);
	int failed = a->info.array.raid_disks;
	struct mdinfo *rv = NULL;
	struct mdinfo *d;
	struct mdinfo *di;
	struct metadata_update *mu;
	struct dl *dl;
	struct imsm_update_activate_spare *u;
	int num_spares = 0;
	int i;

	for (d = a->info.devs ; d ; d = d->next) {
		if ((d->curr_state & DS_FAULTY) &&
			d->state_fd >= 0)
			/* wait for Removal to happen */
			return NULL;
		if (d->state_fd >= 0)
			failed--;
	}

	dprintf("imsm: activate spare: inst=%d failed=%d (%d) level=%d\n",
		inst, failed, a->info.array.raid_disks, a->info.array.level);
	if (imsm_check_degraded(super, dev, failed) != IMSM_T_STATE_DEGRADED)
		return NULL;

	/* For each slot, if it is not working, find a spare */
	for (i = 0; i < a->info.array.raid_disks; i++) {
		for (d = a->info.devs ; d ; d = d->next)
			if (d->disk.raid_disk == i)
				break;
		dprintf("found %d: %p %x\n", i, d, d?d->curr_state:0);
		if (d && (d->state_fd >= 0))
			continue;

		/*
		 * OK, this device needs recovery.  Try to re-add the previous
		 * occupant of this slot, if this fails add a new spare
		 */
		dl = imsm_readd(super, i, a);
		if (!dl)
			dl = imsm_add_spare(super, i, a);
		if (!dl)
			continue;
 
		/* found a usable disk with enough space */
		di = malloc(sizeof(*di));
		if (!di)
			continue;
		memset(di, 0, sizeof(*di));

		/* dl->index will be -1 in the case we are activating a
		 * pristine spare.  imsm_process_update() will create a
		 * new index in this case.  Once a disk is found to be
		 * failed in all member arrays it is kicked from the
		 * metadata
		 */
		di->disk.number = dl->index;

		/* (ab)use di->devs to store a pointer to the device
		 * we chose
		 */
		di->devs = (struct mdinfo *) dl;

		di->disk.raid_disk = i;
		di->disk.major = dl->major;
		di->disk.minor = dl->minor;
		di->disk.state = 0;
		di->data_offset = __le32_to_cpu(map->pba_of_lba0);
		di->component_size = a->info.component_size;
		di->container_member = inst;
		di->next = rv;
		rv = di;
		num_spares++;
		dprintf("%x:%x to be %d at %llu\n", dl->major, dl->minor,
			i, di->data_offset);

		break;
	}

	if (!rv)
		/* No spares found */
		return rv;
	/* Now 'rv' has a list of devices to return.
	 * Create a metadata_update record to update the
	 * disk_ord_tbl for the array
	 */
	mu = malloc(sizeof(*mu));
	if (mu) {
		mu->buf = malloc(sizeof(struct imsm_update_activate_spare) * num_spares);
		if (mu->buf == NULL) {
			free(mu);
			mu = NULL;
		}
	}
	if (!mu) {
		while (rv) {
			struct mdinfo *n = rv->next;

			free(rv);
			rv = n;
		}
		return NULL;
	}
			
	mu->space = NULL;
	mu->len = sizeof(struct imsm_update_activate_spare) * num_spares;
	mu->next = *updates;
	u = (struct imsm_update_activate_spare *) mu->buf;

	for (di = rv ; di ; di = di->next) {
		u->type = update_activate_spare;
		u->dl = (struct dl *) di->devs;
		di->devs = NULL;
		u->slot = di->disk.raid_disk;
		u->array = inst;
		u->next = u + 1;
		u++;
	}
	(u-1)->next = NULL;
	*updates = mu;

	return rv;
}

static int disks_overlap(struct imsm_dev *d1, struct imsm_dev *d2)
{
	struct imsm_map *m1 = get_imsm_map(d1, 0);
	struct imsm_map *m2 = get_imsm_map(d2, 0);
	int i;
	int j;
	int idx;

	for (i = 0; i < m1->num_members; i++) {
		idx = get_imsm_disk_idx(d1, i);
		for (j = 0; j < m2->num_members; j++)
			if (idx == get_imsm_disk_idx(d2, j))
				return 1;
	}

	return 0;
}

static void imsm_delete(struct intel_super *super, struct dl **dlp, int index);

static void imsm_process_update(struct supertype *st,
			        struct metadata_update *update)
{
	/**
	 * crack open the metadata_update envelope to find the update record
	 * update can be one of:
	 * 	update_activate_spare - a spare device has replaced a failed
	 * 	device in an array, update the disk_ord_tbl.  If this disk is
	 * 	present in all member arrays then also clear the SPARE_DISK
	 * 	flag
	 */
	struct intel_super *super = st->sb;
	struct imsm_super *mpb;
	enum imsm_update_type type = *(enum imsm_update_type *) update->buf;

	/* update requires a larger buf but the allocation failed */
	if (super->next_len && !super->next_buf) {
		super->next_len = 0;
		return;
	}

	if (super->next_buf) {
		memcpy(super->next_buf, super->buf, super->len);
		free(super->buf);
		super->len = super->next_len;
		super->buf = super->next_buf;

		super->next_len = 0;
		super->next_buf = NULL;
	}

	mpb = super->anchor;

	switch (type) {
	case update_activate_spare: {
		struct imsm_update_activate_spare *u = (void *) update->buf; 
		struct imsm_dev *dev = get_imsm_dev(super, u->array);
		struct imsm_map *map = get_imsm_map(dev, 0);
		struct imsm_map *migr_map;
		struct active_array *a;
		struct imsm_disk *disk;
		__u8 to_state;
		struct dl *dl;
		unsigned int found;
		int failed;
		int victim = get_imsm_disk_idx(dev, u->slot);
		int i;

		for (dl = super->disks; dl; dl = dl->next)
			if (dl == u->dl)
				break;

		if (!dl) {
			fprintf(stderr, "error: imsm_activate_spare passed "
				"an unknown disk (index: %d)\n",
				u->dl->index);
			return;
		}

		super->updates_pending++;

		/* count failures (excluding rebuilds and the victim)
		 * to determine map[0] state
		 */
		failed = 0;
		for (i = 0; i < map->num_members; i++) {
			if (i == u->slot)
				continue;
			disk = get_imsm_disk(super, get_imsm_disk_idx(dev, i));
			if (!disk || disk->status & FAILED_DISK)
				failed++;
		}

		/* adding a pristine spare, assign a new index */
		if (dl->index < 0) {
			dl->index = super->anchor->num_disks;
			super->anchor->num_disks++;
		}
		disk = &dl->disk;
		disk->status |= CONFIGURED_DISK;
		disk->status &= ~SPARE_DISK;

		/* mark rebuild */
		to_state = imsm_check_degraded(super, dev, failed);
		map->map_state = IMSM_T_STATE_DEGRADED;
		migrate(dev, to_state, MIGR_REBUILD);
		migr_map = get_imsm_map(dev, 1);
		set_imsm_ord_tbl_ent(map, u->slot, dl->index);
		set_imsm_ord_tbl_ent(migr_map, u->slot, dl->index | IMSM_ORD_REBUILD);

		/* count arrays using the victim in the metadata */
		found = 0;
		for (a = st->arrays; a ; a = a->next) {
			dev = get_imsm_dev(super, a->info.container_member);
			for (i = 0; i < map->num_members; i++)
				if (victim == get_imsm_disk_idx(dev, i))
					found++;
		}

		/* delete the victim if it is no longer being
		 * utilized anywhere
		 */
		if (!found) {
			struct dl **dlp;

			/* We know that 'manager' isn't touching anything,
			 * so it is safe to delete
			 */
			for (dlp = &super->disks; *dlp; dlp = &(*dlp)->next)
				if ((*dlp)->index == victim)
					break;

			/* victim may be on the missing list */
			if (!*dlp)
				for (dlp = &super->missing; *dlp; dlp = &(*dlp)->next)
					if ((*dlp)->index == victim)
						break;
			imsm_delete(super, dlp, victim);
		}
		break;
	}
	case update_create_array: {
		/* someone wants to create a new array, we need to be aware of
		 * a few races/collisions:
		 * 1/ 'Create' called by two separate instances of mdadm
		 * 2/ 'Create' versus 'activate_spare': mdadm has chosen
		 *     devices that have since been assimilated via
		 *     activate_spare.
		 * In the event this update can not be carried out mdadm will
		 * (FIX ME) notice that its update did not take hold.
		 */
		struct imsm_update_create_array *u = (void *) update->buf;
		struct imsm_dev *dev;
		struct imsm_map *map, *new_map;
		unsigned long long start, end;
		unsigned long long new_start, new_end;
		int i;
		int overlap = 0;

		/* handle racing creates: first come first serve */
		if (u->dev_idx < mpb->num_raid_devs) {
			dprintf("%s: subarray %d already defined\n",
				__func__, u->dev_idx);
			return;
		}

		/* check update is next in sequence */
		if (u->dev_idx != mpb->num_raid_devs) {
			dprintf("%s: can not create array %d expected index %d\n",
				__func__, u->dev_idx, mpb->num_raid_devs);
			return;
		}

		new_map = get_imsm_map(&u->dev, 0);
		new_start = __le32_to_cpu(new_map->pba_of_lba0);
		new_end = new_start + __le32_to_cpu(new_map->blocks_per_member);

		/* handle activate_spare versus create race:
		 * check to make sure that overlapping arrays do not include
		 * overalpping disks
		 */
		for (i = 0; i < mpb->num_raid_devs; i++) {
			dev = get_imsm_dev(super, i);
			map = get_imsm_map(dev, 0);
			start = __le32_to_cpu(map->pba_of_lba0);
			end = start + __le32_to_cpu(map->blocks_per_member);
			if ((new_start >= start && new_start <= end) ||
			    (start >= new_start && start <= new_end))
				overlap = 1;
			if (overlap && disks_overlap(dev, &u->dev)) {
				dprintf("%s: arrays overlap\n", __func__);
				return;
			}
		}
		/* check num_members sanity */
		if (new_map->num_members > mpb->num_disks) {
			dprintf("%s: num_disks out of range\n", __func__);
			return;
		}

		/* check that prepare update was successful */
		if (!update->space) {
			dprintf("%s: prepare update failed\n", __func__);
			return;
		}

		super->updates_pending++;
		dev = update->space;
		map = get_imsm_map(dev, 0);
		update->space = NULL;
		imsm_copy_dev(dev, &u->dev);
		map = get_imsm_map(dev, 0);
		super->dev_tbl[u->dev_idx] = dev;
		mpb->num_raid_devs++;

		/* fix up flags */
		for (i = 0; i < map->num_members; i++) {
			struct imsm_disk *disk;

			disk = get_imsm_disk(super, get_imsm_disk_idx(dev, i));
			disk->status |= CONFIGURED_DISK;
			disk->status &= ~SPARE_DISK;
		}

		imsm_update_version_info(super);

		break;
	}
	case update_add_disk:

		/* we may be able to repair some arrays if disks are
		 * being added */
		if (super->add) {
			struct active_array *a;

			super->updates_pending++;
 			for (a = st->arrays; a; a = a->next)
				a->check_degraded = 1;
		}
		/* add some spares to the metadata */
		while (super->add) {
			struct dl *al;

			al = super->add;
			super->add = al->next;
			al->next = super->disks;
			super->disks = al;
			dprintf("%s: added %x:%x\n",
				__func__, al->major, al->minor);
		}

		break;
	}
}

static void imsm_prepare_update(struct supertype *st,
				struct metadata_update *update)
{
	/**
	 * Allocate space to hold new disk entries, raid-device entries or a new
	 * mpb if necessary.  The manager synchronously waits for updates to
	 * complete in the monitor, so new mpb buffers allocated here can be
	 * integrated by the monitor thread without worrying about live pointers
	 * in the manager thread.
	 */
	enum imsm_update_type type = *(enum imsm_update_type *) update->buf;
	struct intel_super *super = st->sb;
	struct imsm_super *mpb = super->anchor;
	size_t buf_len;
	size_t len = 0;

	switch (type) {
	case update_create_array: {
		struct imsm_update_create_array *u = (void *) update->buf;

		len = sizeof_imsm_dev(&u->dev, 1);
		update->space = malloc(len);
		break;
	default:
		break;
	}
	}

	/* check if we need a larger metadata buffer */
	if (super->next_buf)
		buf_len = super->next_len;
	else
		buf_len = super->len;

	if (__le32_to_cpu(mpb->mpb_size) + len > buf_len) {
		/* ok we need a larger buf than what is currently allocated
		 * if this allocation fails process_update will notice that
		 * ->next_len is set and ->next_buf is NULL
		 */
		buf_len = ROUND_UP(__le32_to_cpu(mpb->mpb_size) + len, 512);
		if (super->next_buf)
			free(super->next_buf);

		super->next_len = buf_len;
		if (posix_memalign(&super->next_buf, buf_len, 512) != 0)
			super->next_buf = NULL;
	}
}

/* must be called while manager is quiesced */
static void imsm_delete(struct intel_super *super, struct dl **dlp, int index)
{
	struct imsm_super *mpb = super->anchor;
	struct dl *iter;
	struct imsm_dev *dev;
	struct imsm_map *map;
	int i, j, num_members;
	__u32 ord;

	dprintf("%s: deleting device[%d] from imsm_super\n",
		__func__, index);

	/* shift all indexes down one */
	for (iter = super->disks; iter; iter = iter->next)
		if (iter->index > index)
			iter->index--;
	for (iter = super->missing; iter; iter = iter->next)
		if (iter->index > index)
			iter->index--;

	for (i = 0; i < mpb->num_raid_devs; i++) {
		dev = get_imsm_dev(super, i);
		map = get_imsm_map(dev, 0);
		num_members = map->num_members;
		for (j = 0; j < num_members; j++) {
			/* update ord entries being careful not to propagate
			 * ord-flags to the first map
			 */
			ord = get_imsm_ord_tbl_ent(dev, j);

			if (ord_to_idx(ord) <= index)
				continue;

			map = get_imsm_map(dev, 0);
			set_imsm_ord_tbl_ent(map, j, ord_to_idx(ord - 1));
			map = get_imsm_map(dev, 1);
			if (map)
				set_imsm_ord_tbl_ent(map, j, ord - 1);
		}
	}

	mpb->num_disks--;
	super->updates_pending++;
	if (*dlp) {
		struct dl *dl = *dlp;

		*dlp = (*dlp)->next;
		__free_imsm_disk(dl);
	}
}
#endif /* MDASSEMBLE */

struct superswitch super_imsm = {
#ifndef	MDASSEMBLE
	.examine_super	= examine_super_imsm,
	.brief_examine_super = brief_examine_super_imsm,
	.detail_super	= detail_super_imsm,
	.brief_detail_super = brief_detail_super_imsm,
	.write_init_super = write_init_super_imsm,
	.validate_geometry = validate_geometry_imsm,
	.add_to_super	= add_to_super_imsm,
#endif
	.match_home	= match_home_imsm,
	.uuid_from_super= uuid_from_super_imsm,
	.getinfo_super  = getinfo_super_imsm,
	.update_super	= update_super_imsm,

	.avail_size	= avail_size_imsm,

	.compare_super	= compare_super_imsm,

	.load_super	= load_super_imsm,
	.init_super	= init_super_imsm,
	.store_super	= store_zero_imsm,
	.free_super	= free_super_imsm,
	.match_metadata_desc = match_metadata_desc_imsm,
	.container_content = container_content_imsm,

	.external	= 1,

#ifndef MDASSEMBLE
/* for mdmon */
	.open_new	= imsm_open_new,
	.load_super	= load_super_imsm,
	.set_array_state= imsm_set_array_state,
	.set_disk	= imsm_set_disk,
	.sync_metadata	= imsm_sync_metadata,
	.activate_spare = imsm_activate_spare,
	.process_update = imsm_process_update,
	.prepare_update = imsm_prepare_update,
#endif /* MDASSEMBLE */
};