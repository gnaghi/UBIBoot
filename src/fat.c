
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "serial.h"
#include "mmc.h"
#include "fat.h"

uint32_t lba_fat1;			/* sector of first FAT */
uint32_t lba_data;			/* sector of first cluster */
uint32_t root_cluster;		/* cluster where root dir starts */
uint8_t cluster_size;		/* sectors per cluster */

static int get_first_partition(unsigned int id, uint32_t *lba)
{
	struct mbr mbr;

	if (mmc_block_read(id, (uint32_t *) &mbr, 0, 1)) {
		/* Unable to read bootsector. */
		SERIAL_PUTI(0x00);
		return -1;
	}

	if (mbr.signature != 0xAA55) {
		/* No MBR detected. */
		SERIAL_PUTI(0x01);
		return -1;
	}

	if (mbr.partitions[0].status
				&& mbr.partitions[0].status != 0x80) {
		/* Unable to detect first physical partition. */
		SERIAL_PUTI(0x02);
		return -1;
	}

	*lba =  mbr.partitions[0].lba;
	return 0;
}

static int process_boot_sector(unsigned int id, uint32_t lba)
{
	uint32_t sector[FAT_BLOCK_SIZE >> 2];
	struct boot_sector *bs;
	struct volume_info *vinfo;

	if (mmc_block_read(id, sector, lba, 1)) {
		/* Unable to read from first partition. */
		SERIAL_PUTI(0x03);
		return -1;
	}

	bs = (void *)sector;
	lba_fat1 = lba + bs->reserved;
	lba_data = lba_fat1 + bs->fat32_length * bs->fats;
	root_cluster = bs->root_cluster;
	cluster_size = bs->cluster_size;

	vinfo = (void *) sector + sizeof(struct boot_sector);
	if (strncmp(vinfo->fs_type, "FAT32", 5)) {
		/* No FAT32 filesystem detected. */
		SERIAL_PUTI(0x05);
		return -1;
	}

	SERIAL_PUTS("MMC: FAT32 filesystem detected.\n");
	return 0;
}

static void *load_from_cluster(unsigned int id, uint32_t cluster, void *ld_addr)
{
	uint32_t sector[FAT_BLOCK_SIZE >> 2];
	uint32_t cached_fat_sector = -1;

	while (1) {
		uint32_t data_sector = lba_data + (cluster - 2) * cluster_size;
		uint32_t num_data_sectors = cluster_size;

		/* Figure out how many consecutive clusters we can load.
		 * Since every MMC command has a significant overhead, loading more
		 * data at once gives a big speed boost.
		 */
		while (1) {
			uint32_t fat_sector = lba_fat1 + cluster / (FAT_BLOCK_SIZE >> 2);

			/* Read FAT */
			if (fat_sector != cached_fat_sector) {
				if (mmc_block_read(id, sector, fat_sector, 1)) {
					/* Unable to read the FAT table. */
					SERIAL_PUTI(0x04);
					return NULL;
				}
				cached_fat_sector = fat_sector;
			}

			uint32_t prev_cluster = cluster;
			cluster = sector[cluster % (FAT_BLOCK_SIZE >> 2)] & 0x0fffffff;
			if (cluster == prev_cluster + 1)
				num_data_sectors += cluster_size;
			else
				break;
		}

		/* Read file data */
		if (mmc_block_read(id, ld_addr, data_sector, num_data_sectors)) {
			/* Unable to read from first partition. */
			SERIAL_PUTI(0x03);
			return NULL;
		}
		ld_addr += num_data_sectors * FAT_BLOCK_SIZE;

		if ((cluster >= 0x0ffffff0) || (cluster <= 1))
			break;
	}

	return ld_addr;
}

static struct dir_entry *find_file(
		struct dir_entry *first, struct dir_entry *end, const char *name)
{
	struct dir_entry *entry;

	for (entry = first; entry != end && entry->name[0]; entry++) {

		if (entry->attr & (ATTR_VOLUME | ATTR_DIR))
			continue;

		/*
		 * Entries starting with 0xE5 are deleted and should be ignored,
		 * but they won't match the name we're searching for anyway.
		 */

		if (!strncmp(entry->name, name, 8 + 3))
			return entry;
	}

	return NULL;
}

int mmc_load_kernel(unsigned int id, void *ld_addr, int alt)
{
	struct dir_entry *dir_start, *dir_end;
	uint32_t lba;
	int err, i;

	err = get_first_partition(id, &lba);
	if (err)
		return err;

	err = process_boot_sector(id, lba);
	if (err)
		return err;

	dir_start = NULL;
	err = 0;
	for (i = 0; i < 2; i++) {
		struct dir_entry *entry;

		if (!dir_start) {
			/* Load root directory. */
			dir_start = ld_addr;
			dir_end = load_from_cluster(id, root_cluster, dir_start);
			if (!dir_end)
				return -1;
		}

		if (i == !!alt) {
			/* try to find the regular kernel */
			entry = find_file(dir_start, dir_end, FAT_BOOTFILE_NAME);
		} else {
			/* try to find the alt kernel */
			entry = find_file(dir_start, dir_end, FAT_BOOTFILE_ALT_NAME);
		}

		if (entry) {
			SERIAL_PUTS("MMC: Loading kernel file...\n");
			if (load_from_cluster(
						id, entry->starthi << 16 | entry->start, ld_addr)) {
				return i == !alt;
			} else {
				err = -1;
				dir_start = NULL;
			}
		}
	}

	if (err) {
		return err;
	} else {
		/* Kernel file not found. */
		SERIAL_PUTI(0x07);
		return -1;
	}
}
