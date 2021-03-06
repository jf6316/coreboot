/*
 * This file is part of the coreboot project.
 *
 * Copyright 2017 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <device/mmio.h>
#include <bootstate.h>
#include <commonlib/region.h>
#include <console/console.h>
#include <fmap.h>
#include <intelblocks/cse.h>
#include <intelblocks/p2sb.h>
#include <intelblocks/pcr.h>
#include <soc/heci.h>
#include <soc/iomap.h>
#include <soc/pcr_ids.h>
#include <soc/pci_devs.h>
#include <device/pci_ops.h>
#include <stdint.h>

#define PCI_ME_HFSTS1	0x40
#define PCI_ME_HFSTS2	0x48
#define PCI_ME_HFSTS3	0x60
#define PCI_ME_HFSTS4	0x64
#define PCI_ME_HFSTS5	0x68
#define PCI_ME_HFSTS6	0x6c

#define MKHI_GROUP_ID_MCA			0x0a
#define READ_FILE				0x02
#define   READ_FILE_FLAG_DEFAULT		(1 << 0)
#define   READ_FILE_FLAG_HASH			(1 << 1)
#define   READ_FILE_FLAG_EMULATED		(1 << 2)
#define   READ_FILE_FLAG_HW			(1 << 3)

#define MKHI_GROUP_ID_GEN			0xff
#define GET_FW_VERSION				0x02

#define MCA_MAX_FILE_PATH_SIZE			64

#define FUSE_LOCK_FILE				"/fpf/intel/SocCfgLock"

/* Status values are made in such a way erase is not needed */
static enum fuse_flash_state {
	FUSE_FLASH_FUSED = 0xfc,
	FUSE_FLASH_UNFUSED = 0xfe,
	FUSE_FLASH_UNKNOWN = 0xff,
} g_fuse_state;

#define FPF_STATUS_FMAP				"FPF_STATUS"

union mkhi_header {
	uint32_t data;
	struct {
		uint32_t group_id: 8;
		uint32_t command: 7;
		uint32_t is_response: 1;
		uint32_t reserved: 8;
		uint32_t result: 8;
	} __packed fields;
};

/*
 * Read file from CSE internal filesystem.
 * size is maximum length of provided buffer buff, which is updated with actual
 * size of the file read. flags indicate whether real file or fuse is used.
 * Returns 1 on success and 0 otherwise.
 */
static int read_cse_file(const char *path, void *buff, size_t *size,
						size_t offset, uint32_t flags)
{
	int res;
	size_t reply_size;

	struct mca_command {
		union mkhi_header mkhi_hdr;
		char file_name[MCA_MAX_FILE_PATH_SIZE];
		uint32_t offset;
		uint32_t data_size;
		uint8_t flags;
	} __packed msg;

	struct mca_response {
		union mkhi_header mkhi_hdr;
		uint32_t data_size;
		uint8_t buffer[128];
	} __packed rmsg;

	if (sizeof(rmsg.buffer) < *size) {
		printk(BIOS_ERR, "internal buffer is too small\n");
		return 0;
	}

	if (strnlen(path, sizeof(msg.file_name)) >= sizeof(msg.file_name)) {
		printk(BIOS_ERR, "path too big for msg.file_name buffer\n");
		return 0;
	}
	strncpy(msg.file_name, path, sizeof(msg.file_name));
	msg.mkhi_hdr.fields.group_id = MKHI_GROUP_ID_MCA;
	msg.mkhi_hdr.fields.command = READ_FILE;
	msg.flags = flags;
	msg.data_size = *size;
	msg.offset = offset;

	res = heci_send(&msg, sizeof(msg), BIOS_HOST_ADDR, HECI_MKHI_ADDR);

	if (!res) {
		printk(BIOS_ERR, "failed to send HECI message\n");
		return 0;
	}

	reply_size = sizeof(rmsg);
	res = heci_receive(&rmsg, &reply_size);

	if (!res) {
		printk(BIOS_ERR, "failed to receive HECI reply\n");
		return 0;
	}

	if (rmsg.data_size > *size) {
		printk(BIOS_ERR, "reply is too large\n");
		return 0;
	}

	memcpy(buff, rmsg.buffer, rmsg.data_size);
	*size = rmsg.data_size;

	return 1;
}

static enum fuse_flash_state load_cached_fpf(struct region_device *rdev)
{
	enum fuse_flash_state state;
	uint8_t buff;

	state = FUSE_FLASH_UNKNOWN;

	if (rdev_readat(rdev, &buff, 0, sizeof(buff)) >= 0) {
		state = read8(&buff);
		return state;
	}

	printk(BIOS_WARNING, "failed to load cached FPF value\n");

	return state;
}

static
int save_fpf_state(enum fuse_flash_state state, struct region_device *rdev)
{
	uint8_t buff;

	write8(&buff, (uint8_t) state);
	return rdev_writeat(rdev, &buff, 0, sizeof(buff));
}

static void fpf_blown(void *unused)
{
	uint8_t fuse;
	struct region_device rdev;
	size_t sz = sizeof(fuse);
	bool rdev_valid = false;

	if (fmap_locate_area_as_rdev_rw(FPF_STATUS_FMAP, &rdev) == 0) {
		rdev_valid = true;
		g_fuse_state = load_cached_fpf(&rdev);
		if (g_fuse_state != FUSE_FLASH_UNKNOWN)
			return;
	}

	if (!read_cse_file(FUSE_LOCK_FILE, &fuse, &sz, 0, READ_FILE_FLAG_HW))
		return;

	g_fuse_state = fuse == 1 ? FUSE_FLASH_FUSED : FUSE_FLASH_UNFUSED;

	if (rdev_valid && (save_fpf_state(g_fuse_state, &rdev) < 0))
		printk(BIOS_CRIT, "failed to save FPF state\n");
}

static uint32_t dump_status(int index, int reg_addr)
{
	uint32_t reg = pci_read_config32(PCH_DEV_CSE, reg_addr);

	printk(BIOS_DEBUG, "CSE FWSTS%d: 0x%08x\n", index, reg);

	return reg;
}

static void dump_cse_version(void *unused)
{
	int res;
	size_t reply_size;

	struct fw_version_cmd {
		union mkhi_header mkhi_hdr;
	} __packed msg;

	struct version {
		uint16_t minor;
		uint16_t major;
		uint16_t build;
		uint16_t hotfix;
	} __packed;

	struct fw_version_response {
		union mkhi_header mkhi_hdr;
		struct version code;
		struct version nftp;
		struct version fitc;
	} __packed rsp;

	/*
	 * Print ME version only if UART debugging is enabled. Else, it takes
	 * ~0.6 second to talk to ME and get this information.
	 */
	if (!CONFIG(CONSOLE_SERIAL))
		return;

	msg.mkhi_hdr.fields.group_id = MKHI_GROUP_ID_GEN;
	msg.mkhi_hdr.fields.command = GET_FW_VERSION;

	res = heci_send(&msg, sizeof(msg), BIOS_HOST_ADDR, HECI_MKHI_ADDR);

	if (!res) {
		printk(BIOS_ERR, "Failed to send HECI message.\n");
		return;
	}

	reply_size = sizeof(rsp);
	res = heci_receive(&rsp, &reply_size);

	if (!res) {
		printk(BIOS_ERR, "Failed to receive HECI reply.\n");
		return;
	}

	if (rsp.mkhi_hdr.fields.result != 0) {
		printk(BIOS_ERR, "Failed to get ME version.\n");
		return;
	}

	printk(BIOS_DEBUG, "ME: Version: %d.%d.%d.%d\n", rsp.code.major,
	       rsp.code.minor, rsp.code.hotfix, rsp.code.build);
}

static void dump_cse_state(void)
{
	uint32_t fwsts1;

	fwsts1 = dump_status(1, PCI_ME_HFSTS1);
	dump_status(2, PCI_ME_HFSTS2);
	dump_status(3, PCI_ME_HFSTS3);
	dump_status(4, PCI_ME_HFSTS4);
	dump_status(5, PCI_ME_HFSTS5);
	dump_status(6, PCI_ME_HFSTS6);

	/* Minimal decoding is done here in order to call out most important
	   pieces. Manufacturing mode needs to be locked down prior to shipping
	   the product so it's called out explicitly. */
	printk(BIOS_DEBUG, "ME: Manufacturing Mode      : %s\n",
		(fwsts1 & (1 << 0x4)) ? "YES" : "NO");

	printk(BIOS_DEBUG, "ME: FPF status              : ");
	switch (g_fuse_state) {
	case FUSE_FLASH_UNFUSED:
		printk(BIOS_DEBUG, "unfused");
		break;
	case FUSE_FLASH_FUSED:
		printk(BIOS_DEBUG, "fused");
		break;
	default:
	case FUSE_FLASH_UNKNOWN:
		printk(BIOS_DEBUG, "unknown");
	}
	printk(BIOS_DEBUG, "\n");
}

#define PCR_PSFX_T0_SHDW_PCIEN		0x1C
#define PCR_PSFX_T0_SHDW_PCIEN_FUNDIS	(1 << 8)

static void disable_heci1(void)
{
	pcr_or32(PID_PSF3, PSF3_BASE_ADDRESS + PCR_PSFX_T0_SHDW_PCIEN,
		 PCR_PSFX_T0_SHDW_PCIEN_FUNDIS);
}

void heci_cse_lockdown(void)
{
	dump_cse_state();

	/*
	 * It is safe to disable HECI1 now since we won't be talking to the ME
	 * anymore.
	 */
	disable_heci1();
}

BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_ENTRY, fpf_blown, NULL);
BOOT_STATE_INIT_ENTRY(BS_DEV_INIT, BS_ON_EXIT, dump_cse_version, NULL);
