/* bnx2x_main.c: Broadcom Everest network driver.
 *
 * Copyright (c) 2007-2009 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * Maintained by: Eilon Greenstein <eilong@broadcom.com>
 * Written by: Eliezer Tamir
 * Based on code from Michael Chan's bnx2 driver
 * UDP CSUM errata workaround by Arik Gendelman
 * Slowpath and fastpath rework by Vladislav Zolotarov
 * Statistics and Link management by Yitchak Gertner
 *
 */

#include <linux/version.h>
#include <linux/module.h>
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
#include <linux/moduleparam.h>
#endif
#include <linux/kernel.h>
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
#include <linux/device.h>  /* for dev_info() */
#endif
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
#include <linux/dma-mapping.h>
#endif
#include <linux/bitops.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <asm/byteorder.h>
#include <linux/time.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <net/ip.h>
#if (LINUX_VERSION_CODE < 0x020600) /* ! BNX2X_UPSTREAM */
#include <net/ipv6.h>
#endif
#include <net/tcp.h>
#include <net/checksum.h>
#if (LINUX_VERSION_CODE > 0x020607) /* BNX2X_UPSTREAM */
#include <net/ip6_checksum.h>
#endif
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
#include <linux/workqueue.h>
#endif
#include <linux/crc32.h>
#if (LINUX_VERSION_CODE >= 0x02061b) && !defined(BNX2X_DRIVER_DISK) && !defined(__VMKLNX__) /* BNX2X_UPSTREAM */
#include <linux/crc32c.h>
#endif
#include <linux/prefetch.h>
#include <linux/zlib.h>
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
#include <linux/io.h>
#else
#include <asm/io.h>
#endif
#if defined(BNX2X_UPSTREAM) && !defined(BNX2X_USE_INIT_VALUES) /* BNX2X_UPSTREAM */
#include <linux/stringify.h>
#endif

#if (LINUX_VERSION_CODE < 0x020600) || \
    (defined(__VMKLNX__) && (VMWARE_ESX_DDK_VERSION >= 40000)) /* ! BNX2X_UPSTREAM */
#define __NO_TPA__		1
#endif

#if defined(CONFIG_XEN)
#define __NO_TPA__ 		1
#endif

#include "bnx2x.h"
#include "bnx2x_init.h"
#include "bnx2x_init_ops.h"
#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
#include "bnx2x_self_test.h"
#endif
#include "bnx2x_dump.h"

#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__) /* ! BNX2X_UPSTREAM */
#include "bnx2x_esx.h"
#endif

#define DRV_MODULE_VERSION	"1.52.12"
#define DRV_MODULE_RELDATE	"$DateTime: 2009/12/17 12:14:50 $"
#define BNX2X_BC_VER		0x040200

#if defined(BNX2X_UPSTREAM) && !defined(BNX2X_USE_INIT_VALUES) /* BNX2X_UPSTREAM */
#include <linux/firmware.h>
#include "bnx2x_fw_file_hdr.h"
/* FW files */
#define FW_FILE_VERSION					\
	__stringify(BCM_5710_FW_MAJOR_VERSION) "."	\
	__stringify(BCM_5710_FW_MINOR_VERSION) "."	\
	__stringify(BCM_5710_FW_REVISION_VERSION) "."	\
	__stringify(BCM_5710_FW_ENGINEERING_VERSION)
#define FW_FILE_NAME_E1		"bnx2x-e1-" FW_FILE_VERSION ".fw"
#define FW_FILE_NAME_E1H	"bnx2x-e1h-" FW_FILE_VERSION ".fw"
#else
void bnx2x_init_e1_firmware(struct bnx2x *bp);
void bnx2x_init_e1h_firmware(struct bnx2x *bp);
#endif

#define HAVE_PF_RING

#ifdef HAVE_PF_RING
#include "../../../../../kernel/linux/pf_ring.h"
#endif

/* Time in jiffies before concluding the transmitter is hung */
#if defined(__VMKLNX__) /* ! BNX2X_UPSTREAM */
/* On VMware ESX there is a possibility that that netdev watchdog thread
 * runs before the reset task if the machine is loaded.  If this occurs
 * too many times, these premature watchdog triggers will cause a PSOD
 * on a VMware ESX beta build */
#define TX_TIMEOUT		(20*HZ)
#else
#define TX_TIMEOUT		(5*HZ)
#endif

static char version[] __devinitdata =
	"Broadcom NetXtreme II 5771x 10Gigabit Ethernet Driver "
	DRV_MODULE_NAME " " DRV_MODULE_VERSION " (" DRV_MODULE_RELDATE ")\n";

MODULE_AUTHOR("Eliezer Tamir");
MODULE_DESCRIPTION("Broadcom NetXtreme II BCM57710/57711/57711E Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);
#if defined(BNX2X_UPSTREAM) && !defined(BNX2X_USE_INIT_VALUES) /* BNX2X_UPSTREAM */
MODULE_FIRMWARE(FW_FILE_NAME_E1);
MODULE_FIRMWARE(FW_FILE_NAME_E1H);
#endif
#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE >= 0x020600)
MODULE_INFO(cvs_version, "$Revision: #65 $");
#endif
#endif

static int multi_mode = 1;
module_param(multi_mode, int, 0);
#ifdef BNX2X_SAFC
MODULE_PARM_DESC(multi_mode, " Multi queue mode "
			     "(0 Disable; 1 Enable (default);"
			     " 2 VLAN PRI; 3 E1HOV PRI; 4 IP DSCP)");

static int pri_map;
module_param(pri_map, int, 0);
MODULE_PARM_DESC(pri_map, " Priority to HW queue mapping");

static int qs_per_cos;
module_param(qs_per_cos, int, 0);
MODULE_PARM_DESC(qs_per_cos, " Number of queues per HW queue");

static int cos_min_rate;
module_param(cos_min_rate, int, 0);
MODULE_PARM_DESC(cos_min_rate, " Weight for RR between HW queues");
#else /* BNX2X_UPSTREAM */
MODULE_PARM_DESC(multi_mode, " Multi queue mode "
			     "(0 Disable; 1 Enable (default))");
#endif /* not BNX2X_SAFC */

#ifdef CONFIG_64BIT
static int num_queues;
module_param(num_queues, int, 0);
MODULE_PARM_DESC(num_queues, " Number of queues for multi_mode=1"
				" (default is as a number of CPUs)");
#else
static int num_queues = 1;
module_param(num_queues, int, 0);
MODULE_PARM_DESC(num_queues, " Number of queues for multi_mode=1"
				" (default 1)");
#endif

#if defined(__NO_TPA__)
static int disable_tpa = 1;
#else /* BNX2X_UPSTREAM */
static int disable_tpa;
module_param(disable_tpa, int, 0);
MODULE_PARM_DESC(disable_tpa, " Disable the TPA (LRO) feature");
#endif

static int int_mode;
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
module_param(int_mode, int, 0);
MODULE_PARM_DESC(int_mode, " Force interrupt mode other then MSI-X "
				"(1 INT#x; 2 MSI)");
#endif

static int dropless_fc;
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
module_param(dropless_fc, int, 0);
MODULE_PARM_DESC(dropless_fc, " Pause on exhausted host ring");
#endif

static int poll;
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
module_param(poll, int, 0);
MODULE_PARM_DESC(poll, " Use polling (for debug)");
#endif

static int mrrs = -1;
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
module_param(mrrs, int, 0);
MODULE_PARM_DESC(mrrs, " Force Max Read Req Size (0..3) (for debug)");
#endif

static int debug;
#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, " Default debug msglevel");
#endif

static int load_count[3]; /* 0-common, 1-port0, 2-port1 */

static struct workqueue_struct *bnx2x_wq;

enum bnx2x_board_type {
	BCM57710 = 0,
	BCM57711 = 1,
	BCM57711E = 2,
};

/* indexed by board_type, above */
static struct {
	char *name;
} board_info[] __devinitdata = {
	{ "Broadcom NetXtreme II BCM57710 XGb" },
	{ "Broadcom NetXtreme II BCM57711 XGb" },
	{ "Broadcom NetXtreme II BCM57711E XGb" }
};

#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
#ifndef PCI_DEVICE_ID_NX2_57710
#define PCI_DEVICE_ID_NX2_57710		0x164e
#endif
#ifndef PCI_DEVICE_ID_NX2_57711
#define PCI_DEVICE_ID_NX2_57711		0x164f
#endif
#ifndef PCI_DEVICE_ID_NX2_57711E
#define PCI_DEVICE_ID_NX2_57711E	0x1650
#endif
#endif

static const struct pci_device_id bnx2x_pci_tbl[] = {
	{ PCI_VDEVICE(BROADCOM, PCI_DEVICE_ID_NX2_57710), BCM57710 },
	{ PCI_VDEVICE(BROADCOM, PCI_DEVICE_ID_NX2_57711), BCM57711 },
	{ PCI_VDEVICE(BROADCOM, PCI_DEVICE_ID_NX2_57711E), BCM57711E },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, bnx2x_pci_tbl);

/****************************************************************************
* General service functions
****************************************************************************/

/* used only at init
 * locking is done by mcp
 */
void bnx2x_reg_wr_ind(struct bnx2x *bp, u32 addr, u32 val)
{
	pci_write_config_dword(bp->pdev, PCICFG_GRC_ADDRESS, addr);
	pci_write_config_dword(bp->pdev, PCICFG_GRC_DATA, val);
	pci_write_config_dword(bp->pdev, PCICFG_GRC_ADDRESS,
			       PCICFG_VENDOR_ID_OFFSET);
}

static u32 bnx2x_reg_rd_ind(struct bnx2x *bp, u32 addr)
{
	u32 val;

	pci_write_config_dword(bp->pdev, PCICFG_GRC_ADDRESS, addr);
	pci_read_config_dword(bp->pdev, PCICFG_GRC_DATA, &val);
	pci_write_config_dword(bp->pdev, PCICFG_GRC_ADDRESS,
			       PCICFG_VENDOR_ID_OFFSET);

	return val;
}

static const u32 dmae_reg_go_c[] = {
	DMAE_REG_GO_C0, DMAE_REG_GO_C1, DMAE_REG_GO_C2, DMAE_REG_GO_C3,
	DMAE_REG_GO_C4, DMAE_REG_GO_C5, DMAE_REG_GO_C6, DMAE_REG_GO_C7,
	DMAE_REG_GO_C8, DMAE_REG_GO_C9, DMAE_REG_GO_C10, DMAE_REG_GO_C11,
	DMAE_REG_GO_C12, DMAE_REG_GO_C13, DMAE_REG_GO_C14, DMAE_REG_GO_C15
};

/* copy command into DMAE command memory and set DMAE command go */
static void bnx2x_post_dmae(struct bnx2x *bp, struct dmae_command *dmae,
			    int idx)
{
	u32 cmd_offset;
	int i;

	cmd_offset = (DMAE_REG_CMD_MEM + sizeof(struct dmae_command) * idx);
	for (i = 0; i < (sizeof(struct dmae_command)/4); i++) {
		REG_WR(bp, cmd_offset + i*4, *(((u32 *)dmae) + i));

		DP(BNX2X_MSG_OFF, "DMAE cmd[%d].%d (0x%08x) : 0x%08x\n",
		   idx, i, cmd_offset + i*4, *(((u32 *)dmae) + i));
	}
	REG_WR(bp, dmae_reg_go_c[idx], 1);
}

void bnx2x_write_dmae(struct bnx2x *bp, dma_addr_t dma_addr, u32 dst_addr,
		      u32 len32)
{
	struct dmae_command dmae;
	u32 *wb_comp = bnx2x_sp(bp, wb_comp);
	int cnt = 200;

	if (!bp->dmae_ready) {
		u32 *data = bnx2x_sp(bp, wb_data[0]);

		DP(BNX2X_MSG_OFF, "DMAE is not ready (dst_addr %08x  len32 %d)"
		   "  using indirect\n", dst_addr, len32);
		bnx2x_init_ind_wr(bp, dst_addr, data, len32);
		return;
	}

	memset(&dmae, 0, sizeof(struct dmae_command));

	dmae.opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
		       DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
		       DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		       DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		       DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		       (BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		       (BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
	dmae.src_addr_lo = U64_LO(dma_addr);
	dmae.src_addr_hi = U64_HI(dma_addr);
	dmae.dst_addr_lo = dst_addr >> 2;
	dmae.dst_addr_hi = 0;
	dmae.len = len32;
	dmae.comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, wb_comp));
	dmae.comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, wb_comp));
	dmae.comp_val = DMAE_COMP_VAL;

	DP(BNX2X_MSG_OFF, "DMAE: opcode 0x%08x\n"
	   DP_LEVEL "src_addr  [%x:%08x]  len [%d *4]  "
		    "dst_addr [%x:%08x (%08x)]\n"
	   DP_LEVEL "comp_addr [%x:%08x]  comp_val 0x%08x\n",
	   dmae.opcode, dmae.src_addr_hi, dmae.src_addr_lo,
	   dmae.len, dmae.dst_addr_hi, dmae.dst_addr_lo, dst_addr,
	   dmae.comp_addr_hi, dmae.comp_addr_lo, dmae.comp_val);
	DP(BNX2X_MSG_OFF, "data [0x%08x 0x%08x 0x%08x 0x%08x]\n",
	   bp->slowpath->wb_data[0], bp->slowpath->wb_data[1],
	   bp->slowpath->wb_data[2], bp->slowpath->wb_data[3]);

	mutex_lock(&bp->dmae_mutex);

	*wb_comp = 0;

	bnx2x_post_dmae(bp, &dmae, INIT_DMAE_C(bp));

	udelay(5);

	while (*wb_comp != DMAE_COMP_VAL) {
		DP(BNX2X_MSG_OFF, "wb_comp 0x%08x\n", *wb_comp);

		if (!cnt) {
			BNX2X_ERR("DMAE timeout!\n");
			break;
		}
		cnt--;
		/* adjust delay for emulation/FPGA */
		if (CHIP_REV_IS_SLOW(bp))
			msleep(100);
		else
			udelay(5);
	}

	mutex_unlock(&bp->dmae_mutex);
}

void bnx2x_read_dmae(struct bnx2x *bp, u32 src_addr, u32 len32)
{
	struct dmae_command dmae;
	u32 *wb_comp = bnx2x_sp(bp, wb_comp);
	int cnt = 200;

	if (!bp->dmae_ready) {
		u32 *data = bnx2x_sp(bp, wb_data[0]);
		int i;

		DP(BNX2X_MSG_OFF, "DMAE is not ready (src_addr %08x  len32 %d)"
		   "  using indirect\n", src_addr, len32);
		for (i = 0; i < len32; i++)
			data[i] = bnx2x_reg_rd_ind(bp, src_addr + i*4);
		return;
	}

	memset(&dmae, 0, sizeof(struct dmae_command));

	dmae.opcode = (DMAE_CMD_SRC_GRC | DMAE_CMD_DST_PCI |
		       DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
		       DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		       DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		       DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		       (BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		       (BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
	dmae.src_addr_lo = src_addr >> 2;
	dmae.src_addr_hi = 0;
	dmae.dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, wb_data));
	dmae.dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, wb_data));
	dmae.len = len32;
	dmae.comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, wb_comp));
	dmae.comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, wb_comp));
	dmae.comp_val = DMAE_COMP_VAL;

	DP(BNX2X_MSG_OFF, "DMAE: opcode 0x%08x\n"
	   DP_LEVEL "src_addr  [%x:%08x]  len [%d *4]  "
		    "dst_addr [%x:%08x (%08x)]\n"
	   DP_LEVEL "comp_addr [%x:%08x]  comp_val 0x%08x\n",
	   dmae.opcode, dmae.src_addr_hi, dmae.src_addr_lo,
	   dmae.len, dmae.dst_addr_hi, dmae.dst_addr_lo, src_addr,
	   dmae.comp_addr_hi, dmae.comp_addr_lo, dmae.comp_val);

	mutex_lock(&bp->dmae_mutex);

	memset(bnx2x_sp(bp, wb_data[0]), 0, sizeof(u32) * 4);
	*wb_comp = 0;

	bnx2x_post_dmae(bp, &dmae, INIT_DMAE_C(bp));

	udelay(5);

	while (*wb_comp != DMAE_COMP_VAL) {

		if (!cnt) {
			BNX2X_ERR("DMAE timeout!\n");
			break;
		}
		cnt--;
		/* adjust delay for emulation/FPGA */
		if (CHIP_REV_IS_SLOW(bp))
			msleep(100);
		else
			udelay(5);
	}
	DP(BNX2X_MSG_OFF, "data [0x%08x 0x%08x 0x%08x 0x%08x]\n",
	   bp->slowpath->wb_data[0], bp->slowpath->wb_data[1],
	   bp->slowpath->wb_data[2], bp->slowpath->wb_data[3]);

	mutex_unlock(&bp->dmae_mutex);
}

void bnx2x_write_dmae_phys_len(struct bnx2x *bp, dma_addr_t phys_addr,
			       u32 addr, u32 len)
{
	int dmae_wr_max = DMAE_LEN32_WR_MAX(bp);
	int offset = 0;

	while (len > dmae_wr_max) {
		bnx2x_write_dmae(bp, phys_addr + offset,
				 addr + offset, dmae_wr_max);
		offset += dmae_wr_max * 4;
		len -= dmae_wr_max;
	}

	bnx2x_write_dmae(bp, phys_addr + offset, addr + offset, len);
}

/* used only for slowpath so not inlined */
static void bnx2x_wb_wr(struct bnx2x *bp, int reg, u32 val_hi, u32 val_lo)
{
	u32 wb_write[2];

	wb_write[0] = val_hi;
	wb_write[1] = val_lo;
	REG_WR_DMAE(bp, reg, wb_write, 2);
}

#ifdef USE_WB_RD
static u64 bnx2x_wb_rd(struct bnx2x *bp, int reg)
{
	u32 wb_data[2];

	REG_RD_DMAE(bp, reg, wb_data, 2);

	return HILO_U64(wb_data[0], wb_data[1]);
}
#endif

static int bnx2x_mc_assert(struct bnx2x *bp)
{
	char last_idx;
	int i, rc = 0;
	u32 row0, row1, row2, row3;

	/* XSTORM */
	last_idx = REG_RD8(bp, BAR_XSTRORM_INTMEM +
			   XSTORM_ASSERT_LIST_INDEX_OFFSET);
	if (last_idx)
		BNX2X_ERR("XSTORM_ASSERT_LIST_INDEX 0x%x\n", last_idx);

	/* print the asserts */
	for (i = 0; i < STROM_ASSERT_ARRAY_SIZE; i++) {

		row0 = REG_RD(bp, BAR_XSTRORM_INTMEM +
			      XSTORM_ASSERT_LIST_OFFSET(i));
		row1 = REG_RD(bp, BAR_XSTRORM_INTMEM +
			      XSTORM_ASSERT_LIST_OFFSET(i) + 4);
		row2 = REG_RD(bp, BAR_XSTRORM_INTMEM +
			      XSTORM_ASSERT_LIST_OFFSET(i) + 8);
		row3 = REG_RD(bp, BAR_XSTRORM_INTMEM +
			      XSTORM_ASSERT_LIST_OFFSET(i) + 12);

		if (row0 != COMMON_ASM_INVALID_ASSERT_OPCODE) {
			BNX2X_ERR("XSTORM_ASSERT_INDEX 0x%x = 0x%08x"
				  " 0x%08x 0x%08x 0x%08x\n",
				  i, row3, row2, row1, row0);
			rc++;
		} else {
			break;
		}
	}

	/* TSTORM */
	last_idx = REG_RD8(bp, BAR_TSTRORM_INTMEM +
			   TSTORM_ASSERT_LIST_INDEX_OFFSET);
	if (last_idx)
		BNX2X_ERR("TSTORM_ASSERT_LIST_INDEX 0x%x\n", last_idx);

	/* print the asserts */
	for (i = 0; i < STROM_ASSERT_ARRAY_SIZE; i++) {

		row0 = REG_RD(bp, BAR_TSTRORM_INTMEM +
			      TSTORM_ASSERT_LIST_OFFSET(i));
		row1 = REG_RD(bp, BAR_TSTRORM_INTMEM +
			      TSTORM_ASSERT_LIST_OFFSET(i) + 4);
		row2 = REG_RD(bp, BAR_TSTRORM_INTMEM +
			      TSTORM_ASSERT_LIST_OFFSET(i) + 8);
		row3 = REG_RD(bp, BAR_TSTRORM_INTMEM +
			      TSTORM_ASSERT_LIST_OFFSET(i) + 12);

		if (row0 != COMMON_ASM_INVALID_ASSERT_OPCODE) {
			BNX2X_ERR("TSTORM_ASSERT_INDEX 0x%x = 0x%08x"
				  " 0x%08x 0x%08x 0x%08x\n",
				  i, row3, row2, row1, row0);
			rc++;
		} else {
			break;
		}
	}

	/* CSTORM */
	last_idx = REG_RD8(bp, BAR_CSTRORM_INTMEM +
			   CSTORM_ASSERT_LIST_INDEX_OFFSET);
	if (last_idx)
		BNX2X_ERR("CSTORM_ASSERT_LIST_INDEX 0x%x\n", last_idx);

	/* print the asserts */
	for (i = 0; i < STROM_ASSERT_ARRAY_SIZE; i++) {

		row0 = REG_RD(bp, BAR_CSTRORM_INTMEM +
			      CSTORM_ASSERT_LIST_OFFSET(i));
		row1 = REG_RD(bp, BAR_CSTRORM_INTMEM +
			      CSTORM_ASSERT_LIST_OFFSET(i) + 4);
		row2 = REG_RD(bp, BAR_CSTRORM_INTMEM +
			      CSTORM_ASSERT_LIST_OFFSET(i) + 8);
		row3 = REG_RD(bp, BAR_CSTRORM_INTMEM +
			      CSTORM_ASSERT_LIST_OFFSET(i) + 12);

		if (row0 != COMMON_ASM_INVALID_ASSERT_OPCODE) {
			BNX2X_ERR("CSTORM_ASSERT_INDEX 0x%x = 0x%08x"
				  " 0x%08x 0x%08x 0x%08x\n",
				  i, row3, row2, row1, row0);
			rc++;
		} else {
			break;
		}
	}

	/* USTORM */
	last_idx = REG_RD8(bp, BAR_USTRORM_INTMEM +
			   USTORM_ASSERT_LIST_INDEX_OFFSET);
	if (last_idx)
		BNX2X_ERR("USTORM_ASSERT_LIST_INDEX 0x%x\n", last_idx);

	/* print the asserts */
	for (i = 0; i < STROM_ASSERT_ARRAY_SIZE; i++) {

		row0 = REG_RD(bp, BAR_USTRORM_INTMEM +
			      USTORM_ASSERT_LIST_OFFSET(i));
		row1 = REG_RD(bp, BAR_USTRORM_INTMEM +
			      USTORM_ASSERT_LIST_OFFSET(i) + 4);
		row2 = REG_RD(bp, BAR_USTRORM_INTMEM +
			      USTORM_ASSERT_LIST_OFFSET(i) + 8);
		row3 = REG_RD(bp, BAR_USTRORM_INTMEM +
			      USTORM_ASSERT_LIST_OFFSET(i) + 12);

		if (row0 != COMMON_ASM_INVALID_ASSERT_OPCODE) {
			BNX2X_ERR("USTORM_ASSERT_INDEX 0x%x = 0x%08x"
				  " 0x%08x 0x%08x 0x%08x\n",
				  i, row3, row2, row1, row0);
			rc++;
		} else {
			break;
		}
	}

	return rc;
}

static void bnx2x_fw_dump(struct bnx2x *bp)
{
	u32 addr;
	u32 mark, offset;
	__be32 data[9];
	int word;

	if (BP_NOMCP(bp)) {
		BNX2X_ERR("NO MCP - can not dump\n");
		return;
	}

	addr = bp->common.shmem_base - 0x0800 + 4;
	mark = REG_RD(bp, addr);
	mark = MCP_REG_MCPR_SCRATCH + ((mark + 0x3) & ~0x3) - 0x08000000;
	printk(KERN_ERR PFX "begin fw dump (mark 0x%x)\n" KERN_ERR, mark);

	for (offset = mark; offset <= bp->common.shmem_base; offset += 0x8*4) {
		for (word = 0; word < 8; word++)
			data[word] = htonl(REG_RD(bp, offset + 4*word));
		data[8] = 0x0;
		printk(KERN_CONT "%s", (char *)data);
	}
	for (offset = addr + 4; offset <= mark; offset += 0x8*4) {
		for (word = 0; word < 8; word++)
			data[word] = htonl(REG_RD(bp, offset + 4*word));
		data[8] = 0x0;
		printk(KERN_CONT "%s", (char *)data);
	}
	printk("\n" KERN_ERR PFX "end of fw dump\n");
}

static void bnx2x_panic_dump(struct bnx2x *bp)
{
	int i;
	u16 j, start, end;

	bp->stats_state = STATS_STATE_DISABLED;
	DP(BNX2X_MSG_STATS, "stats_state - DISABLED\n");

	BNX2X_ERR("begin crash dump -----------------\n");

	/* Indices */
	/* Common */
	BNX2X_ERR("def_c_idx(0x%x)  def_u_idx(0x%x)  def_x_idx(0x%x)"
		  "  def_t_idx(0x%x)  def_att_idx(0x%x)  attn_state(0x%x)"
		  "  spq_prod_idx(0x%x)\n",
		  bp->def_c_idx, bp->def_u_idx, bp->def_x_idx, bp->def_t_idx,
		  bp->def_att_idx, bp->attn_state, bp->spq_prod_idx);

	/* Rx */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		BNX2X_ERR("fp%d: rx_bd_prod(0x%x)  rx_bd_cons(0x%x)"
			  "  *rx_bd_cons_sb(0x%x)  rx_comp_prod(0x%x)"
			  "  rx_comp_cons(0x%x)  *rx_cons_sb(0x%x)\n",
			  i, fp->rx_bd_prod, fp->rx_bd_cons,
			  le16_to_cpu(*fp->rx_bd_cons_sb), fp->rx_comp_prod,
			  fp->rx_comp_cons, le16_to_cpu(*fp->rx_cons_sb));
		BNX2X_ERR("     rx_sge_prod(0x%x)  last_max_sge(0x%x)"
			  "  fp_u_idx(0x%x) *sb_u_idx(0x%x)\n",
			  fp->rx_sge_prod, fp->last_max_sge,
			  le16_to_cpu(fp->fp_u_idx),
			  fp->status_blk->u_status_block.status_block_index);
	}

	/* Tx */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		BNX2X_ERR("fp%d: tx_pkt_prod(0x%x)  tx_pkt_cons(0x%x)"
			  "  tx_bd_prod(0x%x)  tx_bd_cons(0x%x)"
			  "  *tx_cons_sb(0x%x)\n",
			  i, fp->tx_pkt_prod, fp->tx_pkt_cons, fp->tx_bd_prod,
			  fp->tx_bd_cons, le16_to_cpu(*fp->tx_cons_sb));
		BNX2X_ERR("     fp_c_idx(0x%x)  *sb_c_idx(0x%x)"
			  "  tx_db_prod(0x%x)\n", le16_to_cpu(fp->fp_c_idx),
			  fp->status_blk->c_status_block.status_block_index,
			  fp->tx_db.data.prod);
	}

	/* Rings */
	/* Rx */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		start = RX_BD(le16_to_cpu(*fp->rx_cons_sb) - 10);
		end = RX_BD(le16_to_cpu(*fp->rx_cons_sb) + 503);
		for (j = start; j != end; j = RX_BD(j + 1)) {
			u32 *rx_bd = (u32 *)&fp->rx_desc_ring[j];
			struct sw_rx_bd *sw_bd = &fp->rx_buf_ring[j];

			BNX2X_ERR("fp%d: rx_bd[%x]=[%x:%x]  sw_bd=[%p]\n",
				  i, j, rx_bd[1], rx_bd[0], sw_bd->skb);
		}

		start = RX_SGE(fp->rx_sge_prod);
		end = RX_SGE(fp->last_max_sge);
		for (j = start; j != end; j = RX_SGE(j + 1)) {
			u32 *rx_sge = (u32 *)&fp->rx_sge_ring[j];
			struct sw_rx_page *sw_page = &fp->rx_page_ring[j];

			BNX2X_ERR("fp%d: rx_sge[%x]=[%x:%x]  sw_page=[%p]\n",
				  i, j, rx_sge[1], rx_sge[0], sw_page->page);
		}

		start = RCQ_BD(fp->rx_comp_cons - 10);
		end = RCQ_BD(fp->rx_comp_cons + 503);
		for (j = start; j != end; j = RCQ_BD(j + 1)) {
			u32 *cqe = (u32 *)&fp->rx_comp_ring[j];

			BNX2X_ERR("fp%d: cqe[%x]=[%x:%x:%x:%x]\n",
				  i, j, cqe[0], cqe[1], cqe[2], cqe[3]);
		}
	}

	/* Tx */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		start = TX_BD(le16_to_cpu(*fp->tx_cons_sb) - 10);
		end = TX_BD(le16_to_cpu(*fp->tx_cons_sb) + 245);
		for (j = start; j != end; j = TX_BD(j + 1)) {
			struct sw_tx_bd *sw_bd = &fp->tx_buf_ring[j];

			BNX2X_ERR("fp%d: packet[%x]=[%p,%x]\n",
				  i, j, sw_bd->skb, sw_bd->first_bd);
		}

		start = TX_BD(fp->tx_bd_cons - 10);
		end = TX_BD(fp->tx_bd_cons + 254);
		for (j = start; j != end; j = TX_BD(j + 1)) {
			u32 *tx_bd = (u32 *)&fp->tx_desc_ring[j];

			BNX2X_ERR("fp%d: tx_bd[%x]=[%x:%x:%x:%x]\n",
				  i, j, tx_bd[0], tx_bd[1], tx_bd[2], tx_bd[3]);
		}
	}

	bnx2x_fw_dump(bp);
#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
	bp->msglevel |= NETIF_MSG_PROBE;
	BNX2X_ERR("Idle check (1st round) ----------\n");
	bnx2x_idle_chk(bp);
	BNX2X_ERR("Idle check (2nd round) ----------\n");
	bnx2x_idle_chk(bp);
#endif
	bnx2x_mc_assert(bp);
	BNX2X_ERR("end crash dump -----------------\n");
}

static void bnx2x_int_enable(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	u32 addr = port ? HC_REG_CONFIG_1 : HC_REG_CONFIG_0;
	u32 val = REG_RD(bp, addr);
	int msix = (bp->flags & USING_MSIX_FLAG) ? 1 : 0;
	int msi = (bp->flags & USING_MSI_FLAG) ? 1 : 0;

	if (msix) {
		val &= ~(HC_CONFIG_0_REG_SINGLE_ISR_EN_0 |
			 HC_CONFIG_0_REG_INT_LINE_EN_0);
		val |= (HC_CONFIG_0_REG_MSI_MSIX_INT_EN_0 |
			HC_CONFIG_0_REG_ATTN_BIT_EN_0);
	} else if (msi) {
		val &= ~HC_CONFIG_0_REG_INT_LINE_EN_0;
		val |= (HC_CONFIG_0_REG_SINGLE_ISR_EN_0 |
			HC_CONFIG_0_REG_MSI_MSIX_INT_EN_0 |
			HC_CONFIG_0_REG_ATTN_BIT_EN_0);
	} else {
		val |= (HC_CONFIG_0_REG_SINGLE_ISR_EN_0 |
			HC_CONFIG_0_REG_MSI_MSIX_INT_EN_0 |
			HC_CONFIG_0_REG_INT_LINE_EN_0 |
			HC_CONFIG_0_REG_ATTN_BIT_EN_0);

		DP(NETIF_MSG_INTR, "write %x to HC %d (addr 0x%x)\n",
		   val, port, addr);

		REG_WR(bp, addr, val);

		val &= ~HC_CONFIG_0_REG_MSI_MSIX_INT_EN_0;
	}

	DP(NETIF_MSG_INTR, "write %x to HC %d (addr 0x%x)  mode %s\n",
	   val, port, addr, (msix ? "MSI-X" : (msi ? "MSI" : "INTx")));

	REG_WR(bp, addr, val);
	/*
	 * Ensure that HC_CONFIG is written before leading/trailing edge config
	 */
	mmiowb();
	barrier();

	if (CHIP_IS_E1H(bp)) {
		/* init leading/trailing edge */
		if (IS_E1HMF(bp)) {
			val = (0xee0f | (1 << (BP_E1HVN(bp) + 4)));
			if (bp->port.pmf)
				/* enable nig and gpio3 attention */
				val |= 0x1100;
		} else
			val = 0xffff;

		REG_WR(bp, HC_REG_TRAILING_EDGE_0 + port*8, val);
		REG_WR(bp, HC_REG_LEADING_EDGE_0 + port*8, val);
	}

	/* Make sure that interrupts are indeed enabled from here on */
	mmiowb();
}

static void bnx2x_int_disable(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	u32 addr = port ? HC_REG_CONFIG_1 : HC_REG_CONFIG_0;
	u32 val = REG_RD(bp, addr);

	val &= ~(HC_CONFIG_0_REG_SINGLE_ISR_EN_0 |
		 HC_CONFIG_0_REG_MSI_MSIX_INT_EN_0 |
		 HC_CONFIG_0_REG_INT_LINE_EN_0 |
		 HC_CONFIG_0_REG_ATTN_BIT_EN_0);

	DP(NETIF_MSG_INTR, "write %x to HC %d (addr 0x%x)\n",
	   val, port, addr);

	/* flush all outstanding writes */
	mmiowb();

	REG_WR(bp, addr, val);
	if (REG_RD(bp, addr) != val)
		BNX2X_ERR("BUG! proper val not read from IGU!\n");
}

static void bnx2x_int_disable_sync(struct bnx2x *bp, int disable_hw)
{
#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	int msix = (bp->flags & USING_MSIX_FLAG) ? 1 : 0;
	int i, offset;
#endif
#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	int j;
#endif

	/* disable interrupt handling */
	atomic_inc(&bp->intr_sem);
	smp_wmb(); /* Ensure that bp->intr_sem update is SMP-safe */

	if (disable_hw)
		/* prevent the HW from sending interrupts */
		bnx2x_int_disable(bp);

	/* make sure all ISRs are done */
#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	if (msix) {
		synchronize_irq(bp->msix_table[0].vector);
		offset = 1;
#ifdef BCM_CNIC
		offset++;
#endif
		for_each_queue(bp, i)
			synchronize_irq(bp->msix_table[i + offset].vector);
	} else
#endif
		synchronize_irq(bp->pdev->irq);

	/* make sure sp_task is not running */
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
	cancel_delayed_work(&bp->sp_task);
	flush_workqueue(bnx2x_wq);
#else
	j = 0;
	smp_mb(); /* sp_running is set by sp_task */
	while (bp->sp_running) {
		if (!(++j % 1000))
			BNX2X_ERR("sp_running (j %d)\n", j);
		msleep(1);
		smp_mb();
	}
#endif
}

/* fast path */

/*
 * General service functions
 */

static inline void bnx2x_ack_sb(struct bnx2x *bp, u8 sb_id,
				u8 storm, u16 index, u8 op, u8 update)
{
	u32 hc_addr = (HC_REG_COMMAND_REG + BP_PORT(bp)*32 +
		       COMMAND_REG_INT_ACK);
	struct igu_ack_register igu_ack;

	igu_ack.status_block_index = index;
	igu_ack.sb_id_and_flags =
			((sb_id << IGU_ACK_REGISTER_STATUS_BLOCK_ID_SHIFT) |
			 (storm << IGU_ACK_REGISTER_STORM_ID_SHIFT) |
			 (update << IGU_ACK_REGISTER_UPDATE_INDEX_SHIFT) |
			 (op << IGU_ACK_REGISTER_INTERRUPT_MODE_SHIFT));

#if (LINUX_VERSION_CODE < 0x020600) /* ! BNX2X_UPSTREAM */
	/* x86's writel() in 2.4.x does not have barrier(). */
	barrier();
#endif
	DP(BNX2X_MSG_OFF, "write 0x%08x to HC addr 0x%x\n",
	   (*(u32 *)&igu_ack), hc_addr);
	REG_WR(bp, hc_addr, (*(u32 *)&igu_ack));

	/* Make sure that ACK is written */
	mmiowb();
	barrier();
}

static inline void bnx2x_update_fpsb_idx(struct bnx2x_fastpath *fp)
{
	struct host_status_block *fpsb = fp->status_blk;

	barrier(); /* status block is written to by the chip */
	fp->fp_c_idx = fpsb->c_status_block.status_block_index;
	fp->fp_u_idx = fpsb->u_status_block.status_block_index;
}

static u16 bnx2x_ack_int(struct bnx2x *bp)
{
	u32 hc_addr = (HC_REG_COMMAND_REG + BP_PORT(bp)*32 +
		       COMMAND_REG_SIMD_MASK);
	u32 result = REG_RD(bp, hc_addr);

	DP(BNX2X_MSG_OFF, "read 0x%08x from HC addr 0x%x\n",
	   result, hc_addr);

	return result;
}


/*
 * fast path service functions
 */

static inline int bnx2x_has_tx_work_unload(struct bnx2x_fastpath *fp)
{
	/* Tell compiler that consumer and producer can change */
	barrier();
	return (fp->tx_pkt_prod != fp->tx_pkt_cons);
}

/* free skb in the packet ring at pos idx
 * return idx of last bd freed
 */
static u16 bnx2x_free_tx_pkt(struct bnx2x *bp, struct bnx2x_fastpath *fp,
			     u16 idx)
{
	struct sw_tx_bd *tx_buf = &fp->tx_buf_ring[idx];
	struct eth_tx_start_bd *tx_start_bd;
	struct eth_tx_bd *tx_data_bd;
	struct sk_buff *skb = tx_buf->skb;
	u16 bd_idx = TX_BD(tx_buf->first_bd), new_cons;
	int nbd;

	/* prefetch skb end pointer to speedup dev_kfree_skb() */
	prefetch(&skb->end);

	DP(BNX2X_MSG_OFF, "pkt_idx %d  buff @(%p)->skb %p\n",
	   idx, tx_buf, skb);

	/* unmap first bd */
	DP(BNX2X_MSG_OFF, "free bd_idx %d\n", bd_idx);
	tx_start_bd = &fp->tx_desc_ring[bd_idx].start_bd;
	pci_unmap_single(bp->pdev, BD_UNMAP_ADDR(tx_start_bd),
			 BD_UNMAP_LEN(tx_start_bd), PCI_DMA_TODEVICE);

	nbd = le16_to_cpu(tx_start_bd->nbd) - 1;
#ifdef BNX2X_STOP_ON_ERROR
	if ((nbd - 1) > (MAX_SKB_FRAGS + 2)) {
		BNX2X_ERR("BAD nbd!\n");
		bnx2x_panic();
	}
#endif
	new_cons = nbd + tx_buf->first_bd;

	/* Get the next bd */
	bd_idx = TX_BD(NEXT_TX_IDX(bd_idx));

	/* Skip a parse bd... */
	--nbd;
	bd_idx = TX_BD(NEXT_TX_IDX(bd_idx));

	/* ...and the TSO split header bd since they have no mapping */
	if (tx_buf->flags & BNX2X_TSO_SPLIT_BD) {
		--nbd;
		bd_idx = TX_BD(NEXT_TX_IDX(bd_idx));
	}

	/* now free frags */
	while (nbd > 0) {

		DP(BNX2X_MSG_OFF, "free frag bd_idx %d\n", bd_idx);
		tx_data_bd = &fp->tx_desc_ring[bd_idx].reg_bd;
		pci_unmap_page(bp->pdev, BD_UNMAP_ADDR(tx_data_bd),
			       BD_UNMAP_LEN(tx_data_bd), PCI_DMA_TODEVICE);
		if (--nbd)
			bd_idx = TX_BD(NEXT_TX_IDX(bd_idx));
	}

	/* release skb */
	WARN_ON(!skb);
	dev_kfree_skb(skb);
	tx_buf->first_bd = 0;
	tx_buf->skb = NULL;

	return new_cons;
}

static inline u16 bnx2x_tx_avail(struct bnx2x_fastpath *fp)
{
	s16 used;
	u16 prod;
	u16 cons;

	barrier(); /* Tell compiler that prod and cons can change */
	prod = fp->tx_bd_prod;
	cons = fp->tx_bd_cons;

	/* NUM_TX_RINGS = number of "next-page" entries
	   It will be used as a threshold */
	used = SUB_S16(prod, cons) + (s16)NUM_TX_RINGS;

#ifdef BNX2X_STOP_ON_ERROR
	WARN_ON(used < 0);
	WARN_ON(used > fp->bp->tx_ring_size);
	WARN_ON((fp->bp->tx_ring_size - used) > MAX_TX_AVAIL);
#endif

	return (s16)(fp->bp->tx_ring_size) - used;
}

static inline int bnx2x_has_tx_work(struct bnx2x_fastpath *fp)
{
	u16 hw_cons;

	/* Tell compiler that status block fields can change */
	barrier();
	hw_cons = le16_to_cpu(*fp->tx_cons_sb);
	return hw_cons != fp->tx_pkt_cons;
}

static int bnx2x_tx_int(struct bnx2x_fastpath *fp)
{
	struct bnx2x *bp = fp->bp;
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	struct netdev_queue *txq;
#endif
	u16 hw_cons, sw_cons, bd_cons = fp->tx_bd_cons;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return -1;
#endif

#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	txq = netdev_get_tx_queue(bp->dev, fp->index);
#endif
	hw_cons = le16_to_cpu(*fp->tx_cons_sb);
	sw_cons = fp->tx_pkt_cons;

	while (sw_cons != hw_cons) {
		u16 pkt_cons;

		pkt_cons = TX_BD(sw_cons);

		/* prefetch(bp->tx_buf_ring[pkt_cons].skb); */

		DP(NETIF_MSG_TX_DONE, "hw_cons %u  sw_cons %u  pkt_cons %u\n",
		   hw_cons, sw_cons, pkt_cons);

/*		if (NEXT_TX_IDX(sw_cons) != hw_cons) {
			rmb();
			prefetch(fp->tx_buf_ring[NEXT_TX_IDX(sw_cons)].skb);
		}
*/
		bd_cons = bnx2x_free_tx_pkt(bp, fp, pkt_cons);
		sw_cons++;
	}

	fp->tx_pkt_cons = sw_cons;
	fp->tx_bd_cons = bd_cons;

	/* TBD need a thresh? */
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	if (unlikely(netif_tx_queue_stopped(txq))) {

		/*
		 * Need to make the tx_bd_cons update visible to start_xmit()
		 * before checking for netif_tx_queue_stopped().  Without the
		 * memory barrier, there is a small possibility that
		 * start_xmit() will miss it and cause the queue to be stopped
		 * forever.
		 */
		smp_mb();

		if ((netif_tx_queue_stopped(txq)) &&
		    (bp->state == BNX2X_STATE_OPEN) &&
		    (bnx2x_tx_avail(fp) >= MAX_SKB_FRAGS + 3))
			netif_tx_wake_queue(txq);
	}
#else
	if (unlikely(netif_queue_stopped(bp->dev))) {

		/* See above */
		smp_mb();

		if (netif_queue_stopped(bp->dev) &&
		    (bp->state == BNX2X_STATE_OPEN) &&
		    (bnx2x_tx_avail(fp) >= MAX_SKB_FRAGS + 3))
			netif_wake_queue(bp->dev);
	}
#endif
	return 0;
}

#ifdef BCM_CNIC
static void bnx2x_cnic_cfc_comp(struct bnx2x *bp, int cid);
#endif

static void bnx2x_sp_event(struct bnx2x_fastpath *fp,
			   union eth_rx_cqe *rr_cqe)
{
	struct bnx2x *bp = fp->bp;
	int cid = SW_CID(rr_cqe->ramrod_cqe.conn_and_cmd_data);
	int command = CQE_CMD(rr_cqe->ramrod_cqe.conn_and_cmd_data);

	DP(BNX2X_MSG_SP,
	   "fp %d  cid %d  got ramrod #%d  state is %x  type is %d\n",
	   fp->index, cid, command, bp->state,
	   rr_cqe->ramrod_cqe.ramrod_type);

	bp->spq_left++;

	if (fp->index) {
		switch (command | fp->state) {
		case (RAMROD_CMD_ID_ETH_CLIENT_SETUP |
						BNX2X_FP_STATE_OPENING):
			DP(NETIF_MSG_IFUP, "got MULTI[%d] setup ramrod\n",
			   cid);
			fp->state = BNX2X_FP_STATE_OPEN;
			break;

		case (RAMROD_CMD_ID_ETH_HALT | BNX2X_FP_STATE_HALTING):
			DP(NETIF_MSG_IFDOWN, "got MULTI[%d] halt ramrod\n",
			   cid);
			fp->state = BNX2X_FP_STATE_HALTED;
			break;

		default:
			BNX2X_ERR("unexpected MC reply (%d)  "
				  "fp[%d] state is %x\n",
				  command, fp->index, fp->state);
			break;
		}
		mb(); /* force bnx2x_wait_ramrod() to see the change */
		return;
	}

	switch (command | bp->state) {
	case (RAMROD_CMD_ID_ETH_PORT_SETUP | BNX2X_STATE_OPENING_WAIT4_PORT):
		DP(NETIF_MSG_IFUP, "got setup ramrod\n");
		bp->state = BNX2X_STATE_OPEN;
		break;

	case (RAMROD_CMD_ID_ETH_HALT | BNX2X_STATE_CLOSING_WAIT4_HALT):
		DP(NETIF_MSG_IFDOWN, "got halt ramrod\n");
		bp->state = BNX2X_STATE_CLOSING_WAIT4_DELETE;
		fp->state = BNX2X_FP_STATE_HALTED;
		break;

	case (RAMROD_CMD_ID_ETH_CFC_DEL | BNX2X_STATE_CLOSING_WAIT4_HALT):
		DP(NETIF_MSG_IFDOWN, "got delete ramrod for MULTI[%d]\n", cid);
		bnx2x_fp(bp, cid, state) = BNX2X_FP_STATE_CLOSED;
		break;

#ifdef BCM_CNIC
	case (RAMROD_CMD_ID_ETH_CFC_DEL | BNX2X_STATE_OPEN):
		DP(NETIF_MSG_IFDOWN, "got delete ramrod for CID %d\n", cid);
		bnx2x_cnic_cfc_comp(bp, cid);
		break;
#endif

	case (RAMROD_CMD_ID_ETH_SET_MAC | BNX2X_STATE_OPEN):
	case (RAMROD_CMD_ID_ETH_SET_MAC | BNX2X_STATE_DIAG):
		DP(NETIF_MSG_IFUP, "got set mac ramrod\n");
		bp->set_mac_pending--;
		smp_wmb();
		break;

	case (RAMROD_CMD_ID_ETH_SET_MAC | BNX2X_STATE_CLOSING_WAIT4_HALT):
		DP(NETIF_MSG_IFDOWN, "got (un)set mac ramrod\n");
		bp->set_mac_pending--;
		smp_wmb();
		break;

	default:
		BNX2X_ERR("unexpected MC reply (%d)  bp->state is %x\n",
			  command, bp->state);
		break;
	}
	mb(); /* force bnx2x_wait_ramrod() to see the change */
}

static inline void bnx2x_free_rx_sge(struct bnx2x *bp,
				     struct bnx2x_fastpath *fp, u16 index)
{
	struct sw_rx_page *sw_buf = &fp->rx_page_ring[index];
	struct page *page = sw_buf->page;
	struct eth_rx_sge *sge = &fp->rx_sge_ring[index];

	/* Skip "next page" elements */
	if (!page)
		return;

	pci_unmap_page(bp->pdev, pci_unmap_addr(sw_buf, mapping),
		       SGE_PAGE_SIZE*PAGES_PER_SGE, PCI_DMA_FROMDEVICE);
	__free_pages(page, PAGES_PER_SGE_SHIFT);

	sw_buf->page = NULL;
	sge->addr_hi = 0;
	sge->addr_lo = 0;
}

static inline void bnx2x_free_rx_sge_range(struct bnx2x *bp,
					   struct bnx2x_fastpath *fp, int last)
{
	int i;

	for (i = 0; i < last; i++)
		bnx2x_free_rx_sge(bp, fp, i);
}

static inline int bnx2x_alloc_rx_sge(struct bnx2x *bp,
				     struct bnx2x_fastpath *fp, u16 index)
{
	struct page *page = alloc_pages(GFP_ATOMIC, PAGES_PER_SGE_SHIFT);
	struct sw_rx_page *sw_buf = &fp->rx_page_ring[index];
	struct eth_rx_sge *sge = &fp->rx_sge_ring[index];
	dma_addr_t mapping;

	if (unlikely(page == NULL))
		return -ENOMEM;

	mapping = pci_map_page(bp->pdev, page, 0, SGE_PAGE_SIZE*PAGES_PER_SGE,
			       PCI_DMA_FROMDEVICE);
#if (LINUX_VERSION_CODE >= 0x02061b) /* BNX2X_UPSTREAM */
	if (unlikely(dma_mapping_error(&bp->pdev->dev, mapping))) {
#else
	if (unlikely(dma_mapping_error(mapping))) {
#endif
		__free_pages(page, PAGES_PER_SGE_SHIFT);
		return -ENOMEM;
	}

	sw_buf->page = page;
	pci_unmap_addr_set(sw_buf, mapping, mapping);

	sge->addr_hi = cpu_to_le32(U64_HI(mapping));
	sge->addr_lo = cpu_to_le32(U64_LO(mapping));

	return 0;
}

static inline int bnx2x_alloc_rx_skb(struct bnx2x *bp,
				     struct bnx2x_fastpath *fp, u16 index)
{
	struct sk_buff *skb;
	struct sw_rx_bd *rx_buf = &fp->rx_buf_ring[index];
	struct eth_rx_bd *rx_bd = &fp->rx_desc_ring[index];
	dma_addr_t mapping;

	skb = netdev_alloc_skb(bp->dev, bp->rx_buf_size);
	if (unlikely(skb == NULL))
		return -ENOMEM;

	mapping = pci_map_single(bp->pdev, skb->data, bp->rx_buf_size,
				 PCI_DMA_FROMDEVICE);
#if (LINUX_VERSION_CODE >= 0x02061b) /* BNX2X_UPSTREAM */
	if (unlikely(dma_mapping_error(&bp->pdev->dev, mapping))) {
#else
	if (unlikely(dma_mapping_error(mapping))) {
#endif
		dev_kfree_skb(skb);
		return -ENOMEM;
	}

	rx_buf->skb = skb;
	pci_unmap_addr_set(rx_buf, mapping, mapping);

	rx_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
	rx_bd->addr_lo = cpu_to_le32(U64_LO(mapping));

	return 0;
}

/* note that we are not allocating a new skb,
 * we are just moving one from cons to prod
 * we are not creating a new mapping,
 * so there is no need to check for dma_mapping_error().
 */
static void bnx2x_reuse_rx_skb(struct bnx2x_fastpath *fp,
			       struct sk_buff *skb, u16 cons, u16 prod)
{
	struct bnx2x *bp = fp->bp;
	struct sw_rx_bd *cons_rx_buf = &fp->rx_buf_ring[cons];
	struct sw_rx_bd *prod_rx_buf = &fp->rx_buf_ring[prod];
	struct eth_rx_bd *cons_bd = &fp->rx_desc_ring[cons];
	struct eth_rx_bd *prod_bd = &fp->rx_desc_ring[prod];

	pci_dma_sync_single_for_device(bp->pdev,
				       pci_unmap_addr(cons_rx_buf, mapping),
				       RX_COPY_THRESH, PCI_DMA_FROMDEVICE);

	prod_rx_buf->skb = cons_rx_buf->skb;
	pci_unmap_addr_set(prod_rx_buf, mapping,
			   pci_unmap_addr(cons_rx_buf, mapping));
	*prod_bd = *cons_bd;
}

#if !defined(__NO_TPA__) /* BNX2X_UPSTREAM */
static inline void bnx2x_update_last_max_sge(struct bnx2x_fastpath *fp,
					     u16 idx)
{
	u16 last_max = fp->last_max_sge;

	if (SUB_S16(idx, last_max) > 0)
		fp->last_max_sge = idx;
}

static void bnx2x_clear_sge_mask_next_elems(struct bnx2x_fastpath *fp)
{
	int i, j;

	for (i = 1; i <= NUM_RX_SGE_PAGES; i++) {
		int idx = RX_SGE_CNT * i - 1;

		for (j = 0; j < 2; j++) {
			SGE_MASK_CLEAR_BIT(fp, idx);
			idx--;
		}
	}
}

static void bnx2x_update_sge_prod(struct bnx2x_fastpath *fp,
				  struct eth_fast_path_rx_cqe *fp_cqe)
{
	struct bnx2x *bp = fp->bp;
	u16 sge_len = SGE_PAGE_ALIGN(le16_to_cpu(fp_cqe->pkt_len) -
				     le16_to_cpu(fp_cqe->len_on_bd)) >>
		      SGE_PAGE_SHIFT;
	u16 last_max, last_elem, first_elem;
	u16 delta = 0;
	u16 i;

	if (!sge_len)
		return;

	/* First mark all used pages */
	for (i = 0; i < sge_len; i++)
		SGE_MASK_CLEAR_BIT(fp, RX_SGE(le16_to_cpu(fp_cqe->sgl[i])));

	DP(NETIF_MSG_RX_STATUS, "fp_cqe->sgl[%d] = %d\n",
	   sge_len - 1, le16_to_cpu(fp_cqe->sgl[sge_len - 1]));

	/* Here we assume that the last SGE index is the biggest */
	prefetch((void *)(fp->sge_mask));
	bnx2x_update_last_max_sge(fp, le16_to_cpu(fp_cqe->sgl[sge_len - 1]));

	last_max = RX_SGE(fp->last_max_sge);
	last_elem = last_max >> RX_SGE_MASK_ELEM_SHIFT;
	first_elem = RX_SGE(fp->rx_sge_prod) >> RX_SGE_MASK_ELEM_SHIFT;

	/* If ring is not full */
	if (last_elem + 1 != first_elem)
		last_elem++;

	/* Now update the prod */
	for (i = first_elem; i != last_elem; i = NEXT_SGE_MASK_ELEM(i)) {
		if (likely(fp->sge_mask[i]))
			break;

		fp->sge_mask[i] = RX_SGE_MASK_ELEM_ONE_MASK;
		delta += RX_SGE_MASK_ELEM_SZ;
	}

	if (delta > 0) {
		fp->rx_sge_prod += delta;
		/* clear page-end entries */
		bnx2x_clear_sge_mask_next_elems(fp);
	}

	DP(NETIF_MSG_RX_STATUS,
	   "fp->last_max_sge = %d  fp->rx_sge_prod = %d\n",
	   fp->last_max_sge, fp->rx_sge_prod);
}

static inline void bnx2x_init_sge_ring_bit_mask(struct bnx2x_fastpath *fp)
{
	/* Set the mask to all 1-s: it's faster to compare to 0 than to 0xf-s */
	memset(fp->sge_mask, 0xff,
	       (NUM_RX_SGE >> RX_SGE_MASK_ELEM_SHIFT)*sizeof(u64));

	/* Clear the two last indices in the page to 1:
	   these are the indices that correspond to the "next" element,
	   hence will never be indicated and should be removed from
	   the calculations. */
	bnx2x_clear_sge_mask_next_elems(fp);
}

static void bnx2x_tpa_start(struct bnx2x_fastpath *fp, u16 queue,
			    struct sk_buff *skb, u16 cons, u16 prod)
{
	struct bnx2x *bp = fp->bp;
	struct sw_rx_bd *cons_rx_buf = &fp->rx_buf_ring[cons];
	struct sw_rx_bd *prod_rx_buf = &fp->rx_buf_ring[prod];
	struct eth_rx_bd *prod_bd = &fp->rx_desc_ring[prod];
	dma_addr_t mapping;

	/* move empty skb from pool to prod and map it */
	prod_rx_buf->skb = fp->tpa_pool[queue].skb;
	mapping = pci_map_single(bp->pdev, fp->tpa_pool[queue].skb->data,
				 bp->rx_buf_size, PCI_DMA_FROMDEVICE);
	pci_unmap_addr_set(prod_rx_buf, mapping, mapping);

	/* move partial skb from cons to pool (don't unmap yet) */
	fp->tpa_pool[queue] = *cons_rx_buf;

	/* mark bin state as start - print error if current state != stop */
	if (fp->tpa_state[queue] != BNX2X_TPA_STOP)
		BNX2X_ERR("start of bin not in stop [%d]\n", queue);

	fp->tpa_state[queue] = BNX2X_TPA_START;

	/* point prod_bd to new skb */
	prod_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
	prod_bd->addr_lo = cpu_to_le32(U64_LO(mapping));

#ifdef BNX2X_STOP_ON_ERROR
	fp->tpa_queue_used |= (1 << queue);
#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
#ifdef _ASM_GENERIC_INT_L64_H
	DP(NETIF_MSG_RX_STATUS, "fp->tpa_queue_used = 0x%lx\n",
#else
	DP(NETIF_MSG_RX_STATUS, "fp->tpa_queue_used = 0x%llx\n",
#endif
#else
#if defined(__powerpc64__) || defined(_ASM_IA64_TYPES_H)
	DP(NETIF_MSG_RX_STATUS, "fp->tpa_queue_used = 0x%lx\n",
#else
	DP(NETIF_MSG_RX_STATUS, "fp->tpa_queue_used = 0x%llx\n",
#endif
#endif
	   fp->tpa_queue_used);
#endif
}

static int bnx2x_fill_frag_skb(struct bnx2x *bp, struct bnx2x_fastpath *fp,
			       struct sk_buff *skb,
			       struct eth_fast_path_rx_cqe *fp_cqe,
			       u16 cqe_idx)
{
	struct sw_rx_page *rx_pg, old_rx_pg;
	u16 len_on_bd = le16_to_cpu(fp_cqe->len_on_bd);
	u32 i, frag_len, frag_size, pages;
	int err;
	int j;

	frag_size = le16_to_cpu(fp_cqe->pkt_len) - len_on_bd;
	pages = SGE_PAGE_ALIGN(frag_size) >> SGE_PAGE_SHIFT;

	/* This is needed in order to enable forwarding support */
	if (frag_size)
		skb_shinfo(skb)->gso_size = min((u32)SGE_PAGE_SIZE,
					       max(frag_size, (u32)len_on_bd));

#ifdef BNX2X_STOP_ON_ERROR
	if (pages > min_t(u32, 8, MAX_SKB_FRAGS)*SGE_PAGE_SIZE*PAGES_PER_SGE) {
		BNX2X_ERR("SGL length is too long: %d. CQE index is %d\n",
			  pages, cqe_idx);
		BNX2X_ERR("fp_cqe->pkt_len = %d  fp_cqe->len_on_bd = %d\n",
			  fp_cqe->pkt_len, len_on_bd);
		bnx2x_panic();
		return -EINVAL;
	}
#endif

	/* Run through the SGL and compose the fragmented skb */
	for (i = 0, j = 0; i < pages; i += PAGES_PER_SGE, j++) {
		u16 sge_idx = RX_SGE(le16_to_cpu(fp_cqe->sgl[j]));

		/* FW gives the indices of the SGE as if the ring is an array
		   (meaning that "next" element will consume 2 indices) */
		frag_len = min(frag_size, (u32)(SGE_PAGE_SIZE*PAGES_PER_SGE));
		rx_pg = &fp->rx_page_ring[sge_idx];
		old_rx_pg = *rx_pg;

		/* If we fail to allocate a substitute page, we simply stop
		   where we are and drop the whole packet */
		err = bnx2x_alloc_rx_sge(bp, fp, sge_idx);
		if (unlikely(err)) {
			fp->eth_q_stats.rx_skb_alloc_failed++;
			return err;
		}

		/* Unmap the page as we r going to pass it to the stack */
		pci_unmap_page(bp->pdev, pci_unmap_addr(&old_rx_pg, mapping),
			      SGE_PAGE_SIZE*PAGES_PER_SGE, PCI_DMA_FROMDEVICE);

		/* Add one frag and update the appropriate fields in the skb */
		skb_fill_page_desc(skb, j, old_rx_pg.page, 0, frag_len);

		skb->data_len += frag_len;
		skb->truesize += frag_len;
		skb->len += frag_len;

		frag_size -= frag_len;
	}

	return 0;
}

static void bnx2x_tpa_stop(struct bnx2x *bp, struct bnx2x_fastpath *fp,
			   u16 queue, int pad, int len, union eth_rx_cqe *cqe,
			   u16 cqe_idx)
{
	struct sw_rx_bd *rx_buf = &fp->tpa_pool[queue];
	struct sk_buff *skb = rx_buf->skb;
	/* alloc new skb */
	struct sk_buff *new_skb = netdev_alloc_skb(bp->dev, bp->rx_buf_size);

	/* Unmap skb in the pool anyway, as we are going to change
	   pool entry status to BNX2X_TPA_STOP even if new skb allocation
	   fails. */
	pci_unmap_single(bp->pdev, pci_unmap_addr(rx_buf, mapping),
			 bp->rx_buf_size, PCI_DMA_FROMDEVICE);

	if (likely(new_skb)) {
		/* fix ip xsum and give it to the stack */
		/* (no need to map the new skb) */
#ifdef BCM_VLAN
		int is_vlan_cqe =
			(le16_to_cpu(cqe->fast_path_cqe.pars_flags.flags) &
			 PARSING_FLAGS_VLAN);
		int is_not_hwaccel_vlan_cqe =
			(is_vlan_cqe && (!(bp->flags & HW_VLAN_RX_FLAG)));
#endif

		prefetch(skb);
		prefetch(((char *)(skb)) + 128);

#ifdef BNX2X_STOP_ON_ERROR
		if (pad + len > bp->rx_buf_size) {
			BNX2X_ERR("skb_put is about to fail...  "
				  "pad %d  len %d  rx_buf_size %d\n",
				  pad, len, bp->rx_buf_size);
			bnx2x_panic();
			return;
		}
#endif

		skb_reserve(skb, pad);
		skb_put(skb, len);

		skb->protocol = eth_type_trans(skb, bp->dev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;

#ifdef HAVE_PF_RING
		{
		  int debug = 0;
		  struct pfring_hooks *hook = (struct pfring_hooks*)bp->dev->pfring_ptr;
		  
		  if(hook && (hook->magic == PF_RING)) {
		    /* Wow: PF_RING is alive & kickin' ! */
		    int rc;
			  
		    if(debug)
		      printk(KERN_INFO "[PF_RING] alive [%s][len=%d][queue=%d/%d]\n",
			     bp->dev->name, skb->len,
			     fp->index  /* queue */, BNX2X_NUM_QUEUES(bp));
			  
		    if(*hook->transparent_mode != standard_linux_path) {
		      rc = hook->ring_handler(skb, 1, 1, queue, BNX2X_NUM_QUEUES(bp));
		      if(rc == 1 /* Packet handled by PF_RING */) {
			if(*hook->transparent_mode == driver2pf_ring_non_transparent) {
			  /* PF_RING has already freed the memory */
			  goto next_pkt;
			}
		      }
		    } else {
		      if(debug) printk(KERN_INFO "[PF_RING] present but transparent_mode=0\n");
		    }
		  } else {
		      if(debug) printk(KERN_INFO "[PF_RING] not present on %s [hook=%p]\n",
				       bp->dev->name, hook);
		  }
		}
#endif

		{
			struct iphdr *iph;

			iph = (struct iphdr *)skb->data;
#ifdef BCM_VLAN
			/* If there is no Rx VLAN offloading -
			   take VLAN tag into an account */
			if (unlikely(is_not_hwaccel_vlan_cqe))
				iph = (struct iphdr *)((u8 *)iph + VLAN_HLEN);
#endif
			iph->check = 0;
			iph->check = ip_fast_csum((u8 *)iph, iph->ihl);
		}

		if (!bnx2x_fill_frag_skb(bp, fp, skb,
					 &cqe->fast_path_cqe, cqe_idx)) {
#ifdef BCM_VLAN
			if ((bp->vlgrp != NULL) && is_vlan_cqe &&
			    (!is_not_hwaccel_vlan_cqe))
				vlan_gro_receive(&fp->napi, bp->vlgrp,
						 le16_to_cpu(cqe->fast_path_cqe.
							     vlan_tag), skb); 
			else
#endif
				napi_gro_receive(&fp->napi, skb);
		} else {
			DP(NETIF_MSG_RX_STATUS, "Failed to allocate new pages"
			   " - dropping packet!\n");
			dev_kfree_skb(skb);
		}

#ifdef HAVE_PF_RING
	next_pkt:
#endif

#if (LINUX_VERSION_CODE < 0x02061b) /* ! BNX2X_UPSTREAM */
		bp->dev->last_rx = jiffies;
#endif

		/* put new skb in bin */
		fp->tpa_pool[queue].skb = new_skb;

	} else {
		/* else drop the packet and keep the buffer in the bin */
		DP(NETIF_MSG_RX_STATUS,
		   "Failed to allocate new skb - dropping packet!\n");
		fp->eth_q_stats.rx_skb_alloc_failed++;
	}

	fp->tpa_state[queue] = BNX2X_TPA_STOP;
}
#endif

static inline void bnx2x_update_rx_prod(struct bnx2x *bp,
					struct bnx2x_fastpath *fp,
					u16 bd_prod, u16 rx_comp_prod,
					u16 rx_sge_prod)
{
	struct ustorm_eth_rx_producers rx_prods = {0};
	int i;

	/* Update producers */
	rx_prods.bd_prod = bd_prod;
	rx_prods.cqe_prod = rx_comp_prod;
	rx_prods.sge_prod = rx_sge_prod;

	/*
	 * Make sure that the BD and SGE data is updated before updating the
	 * producers since FW might read the BD/SGE right after the producer
	 * is updated.
	 * This is only applicable for weak-ordered memory model archs such
	 * as IA-64. The following barrier is also mandatory since FW will
	 * assumes BDs must have buffers.
	 */
	wmb();

	for (i = 0; i < sizeof(struct ustorm_eth_rx_producers)/4; i++)
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_RX_PRODS_OFFSET(BP_PORT(bp), fp->cl_id) + i*4,
		       ((u32 *)&rx_prods)[i]);

	mmiowb(); /* keep prod updates ordered */

	DP(NETIF_MSG_RX_STATUS,
	   "queue[%d]:  wrote  bd_prod %u  cqe_prod %u  sge_prod %u\n",
	   fp->index, bd_prod, rx_comp_prod, rx_sge_prod);
}

static int bnx2x_rx_int(struct bnx2x_fastpath *fp, int budget)
{
	struct bnx2x *bp = fp->bp;
	u16 bd_cons, bd_prod, bd_prod_fw, comp_ring_cons;
	u16 hw_comp_cons, sw_comp_cons, sw_comp_prod;
	int rx_pkt = 0;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return 0;
#endif

	/* CQ "next element" is of the size of the regular element,
	   that's why it's ok here */
	hw_comp_cons = le16_to_cpu(*fp->rx_cons_sb);
	if ((hw_comp_cons & MAX_RCQ_DESC_CNT) == MAX_RCQ_DESC_CNT)
		hw_comp_cons++;

	bd_cons = fp->rx_bd_cons;
	bd_prod = fp->rx_bd_prod;
	bd_prod_fw = bd_prod;
	sw_comp_cons = fp->rx_comp_cons;
	sw_comp_prod = fp->rx_comp_prod;

	/* Memory barrier necessary as speculative reads of the rx
	 * buffer can be ahead of the index in the status block
	 */
	rmb();

	DP(NETIF_MSG_RX_STATUS,
	   "queue[%d]:  hw_comp_cons %u  sw_comp_cons %u\n",
	   fp->index, hw_comp_cons, sw_comp_cons);

	while (sw_comp_cons != hw_comp_cons) {
		struct sw_rx_bd *rx_buf = NULL;
		struct sk_buff *skb;
		union eth_rx_cqe *cqe;
		u8 cqe_fp_flags;
		u16 len, pad;

		comp_ring_cons = RCQ_BD(sw_comp_cons);
		bd_prod = RX_BD(bd_prod);
		bd_cons = RX_BD(bd_cons);

		/* Prefetch the page containing the BD descriptor
		   at producer's index. It will be needed when new skb is
		   allocated */
		prefetch((void *)(PAGE_ALIGN((unsigned long)
					     (&fp->rx_desc_ring[bd_prod])) -
				  PAGE_SIZE + 1));

		cqe = &fp->rx_comp_ring[comp_ring_cons];
		cqe_fp_flags = cqe->fast_path_cqe.type_error_flags;

		DP(NETIF_MSG_RX_STATUS, "CQE type %x  err %x  status %x"
		   "  queue %x  vlan %x  len %u\n", CQE_TYPE(cqe_fp_flags),
		   cqe_fp_flags, cqe->fast_path_cqe.status_flags,
		   le32_to_cpu(cqe->fast_path_cqe.rss_hash_result),
		   le16_to_cpu(cqe->fast_path_cqe.vlan_tag),
		   le16_to_cpu(cqe->fast_path_cqe.pkt_len));

		/* is this a slowpath msg? */
		if (unlikely(CQE_TYPE(cqe_fp_flags))) {
			bnx2x_sp_event(fp, cqe);
			goto next_cqe;

		/* this is an rx packet */
		} else {
			rx_buf = &fp->rx_buf_ring[bd_cons];
			skb = rx_buf->skb;
			prefetch(skb);
			prefetch((u8 *)skb + 256);
			len = le16_to_cpu(cqe->fast_path_cqe.pkt_len);
			pad = cqe->fast_path_cqe.placement_offset;

#if !defined(__NO_TPA__) /* BNX2X_UPSTREAM */
			/* If CQE is marked both TPA_START and TPA_END
			   it is a non-TPA CQE */
			if ((!fp->disable_tpa) &&
			    (TPA_TYPE(cqe_fp_flags) !=
					(TPA_TYPE_START | TPA_TYPE_END))) {
				u16 queue = cqe->fast_path_cqe.queue_index;

				if (TPA_TYPE(cqe_fp_flags) == TPA_TYPE_START) {
					DP(NETIF_MSG_RX_STATUS,
					   "calling tpa_start on queue %d\n",
					   queue);

					bnx2x_tpa_start(fp, queue, skb,
							bd_cons, bd_prod);
					goto next_rx;
				}

				if (TPA_TYPE(cqe_fp_flags) == TPA_TYPE_END) {
					DP(NETIF_MSG_RX_STATUS,
					   "calling tpa_stop on queue %d\n",
					   queue);

					if (!BNX2X_RX_SUM_FIX(cqe))
						BNX2X_ERR("STOP on none TCP "
							  "data\n");

					/* This is a size of the linear data
					   on this skb */
					len = le16_to_cpu(cqe->fast_path_cqe.
								len_on_bd);
					bnx2x_tpa_stop(bp, fp, queue, pad,
						    len, cqe, comp_ring_cons);
#ifdef BNX2X_STOP_ON_ERROR
					if (bp->panic)
						return 0;
#endif

					bnx2x_update_sge_prod(fp,
							&cqe->fast_path_cqe);
					goto next_cqe;
				}
			}
#endif

			pci_dma_sync_single_for_device(bp->pdev,
					pci_unmap_addr(rx_buf, mapping),
						       pad + RX_COPY_THRESH,
						       PCI_DMA_FROMDEVICE);
			prefetch(skb);
			prefetch(((char *)(skb)) + 128);

			/* is this an error packet? */
			if (unlikely(cqe_fp_flags & ETH_RX_ERROR_FALGS)) {
				DP(NETIF_MSG_RX_ERR,
				   "ERROR  flags %x  rx packet %u\n",
				   cqe_fp_flags, sw_comp_cons);
				fp->eth_q_stats.rx_err_discard_pkt++;
				goto reuse_rx;
			}

			/* Since we don't have a jumbo ring
			 * copy small packets if mtu > 1500
			 */
			if ((bp->dev->mtu > ETH_MAX_PACKET_SIZE) &&
			    (len <= RX_COPY_THRESH)) {
				struct sk_buff *new_skb;

				new_skb = netdev_alloc_skb(bp->dev,
							   len + pad);
				if (new_skb == NULL) {
					DP(NETIF_MSG_RX_ERR,
					   "ERROR  packet dropped "
					   "because of alloc failure\n");
					fp->eth_q_stats.rx_skb_alloc_failed++;
					goto reuse_rx;
				}

				/* aligned copy */
				skb_copy_from_linear_data_offset(skb, pad,
						    new_skb->data + pad, len);
				skb_reserve(new_skb, pad);
				skb_put(new_skb, len);

				bnx2x_reuse_rx_skb(fp, skb, bd_cons, bd_prod);

				skb = new_skb;

			} else
			if (likely(bnx2x_alloc_rx_skb(bp, fp, bd_prod) == 0)) {
				pci_unmap_single(bp->pdev,
					pci_unmap_addr(rx_buf, mapping),
						 bp->rx_buf_size,
						 PCI_DMA_FROMDEVICE);
				skb_reserve(skb, pad);
				skb_put(skb, len);

			} else {
				DP(NETIF_MSG_RX_ERR,
				   "ERROR  packet dropped because "
				   "of alloc failure\n");
				fp->eth_q_stats.rx_skb_alloc_failed++;
reuse_rx:
				bnx2x_reuse_rx_skb(fp, skb, bd_cons, bd_prod);
				goto next_rx;
			}

			skb->protocol = eth_type_trans(skb, bp->dev);

			skb->ip_summed = CHECKSUM_NONE;
			if (bp->rx_csum) {
				if (likely(BNX2X_RX_CSUM_OK(cqe)))
					skb->ip_summed = CHECKSUM_UNNECESSARY;
				else
					fp->eth_q_stats.hw_csum_err++;
			}
		}

		skb_record_rx_queue(skb, fp->index);

#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__) /* ! BNX2X_UPSTREAM */
		vmknetddi_queueops_set_skb_queueid(skb,
				VMKNETDDI_QUEUEOPS_MK_RX_QUEUEID(fp->index));
#endif
#ifdef BCM_VLAN
		if ((bp->vlgrp != NULL) && (bp->flags & HW_VLAN_RX_FLAG) &&
		    (le16_to_cpu(cqe->fast_path_cqe.pars_flags.flags) &
		     PARSING_FLAGS_VLAN))
			vlan_gro_receive(&fp->napi, bp->vlgrp,
				le16_to_cpu(cqe->fast_path_cqe.vlan_tag), skb); 
		else
#endif

#ifdef HAVE_PF_RING
		{

		  int debug = 0;
		  struct pfring_hooks *hook = (struct pfring_hooks*)bp->dev->pfring_ptr;
		  
		  if(hook && (hook->magic == PF_RING)) {
		    /* Wow: PF_RING is alive & kickin' ! */
		    int rc, queue = fp->index;
			  
		    if(debug)
		      printk(KERN_INFO "[2] [PF_RING] alive [%s][len=%d][queue=%d/%d]\n",
			     bp->dev->name, skb->len,
			     queue, BNX2X_NUM_QUEUES(bp));
			  
		    if(*hook->transparent_mode != standard_linux_path) {
		      rc = hook->ring_handler(skb, 1, 1, queue, BNX2X_NUM_QUEUES(bp));
		      if(rc == 1 /* Packet handled by PF_RING */) {
			if(*hook->transparent_mode == driver2pf_ring_non_transparent) {
			  /* PF_RING has already freed the memory */
			  goto next_pkt2;
			}
		      }
		    } else {
		      if(debug) printk(KERN_INFO "[2] [PF_RING] present but transparent_mode=0\n");
		    }
		  } else {
		      if(debug) printk(KERN_INFO "[2] [PF_RING] not present on %s [hook=%p]\n",
				       bp->dev->name, hook);
		  }
		}
#endif

			napi_gro_receive(&fp->napi, skb);
		 

#ifdef HAVE_PF_RING
	next_pkt2:
#endif

#if (LINUX_VERSION_CODE < 0x02061b) /* ! BNX2X_UPSTREAM */
		bp->dev->last_rx = jiffies;
#endif

next_rx:
		rx_buf->skb = NULL;

		bd_cons = NEXT_RX_IDX(bd_cons);
		bd_prod = NEXT_RX_IDX(bd_prod);
		bd_prod_fw = NEXT_RX_IDX(bd_prod_fw);
		rx_pkt++;
next_cqe:
		sw_comp_prod = NEXT_RCQ_IDX(sw_comp_prod);
		sw_comp_cons = NEXT_RCQ_IDX(sw_comp_cons);

		if (rx_pkt == budget)
			break;
	} /* while */

	fp->rx_bd_cons = bd_cons;
	fp->rx_bd_prod = bd_prod_fw;
	fp->rx_comp_cons = sw_comp_cons;
	fp->rx_comp_prod = sw_comp_prod;

	/* Update producers */
	bnx2x_update_rx_prod(bp, fp, bd_prod_fw, sw_comp_prod,
			     fp->rx_sge_prod);

	fp->rx_pkt += rx_pkt;
	fp->rx_calls++;

	return rx_pkt;
}

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE < 0x020613) && (VMWARE_ESX_DDK_VERSION < 4000)
static irqreturn_t bnx2x_msix_fp_int(int irq, void *fp_cookie,
				     struct pt_regs *regs)
#else /* BNX2X_UPSTREAM */
static irqreturn_t bnx2x_msix_fp_int(int irq, void *fp_cookie)
#endif
{
	struct bnx2x_fastpath *fp = fp_cookie;
	struct bnx2x *bp = fp->bp;

	/* Return here if interrupt is disabled */
	if (unlikely(atomic_read(&bp->intr_sem) != 0)) {
		DP(NETIF_MSG_INTR, "called but intr_sem not 0, returning\n");
		return IRQ_HANDLED;
	}

	DP(BNX2X_MSG_FP, "got an MSI-X interrupt on IDX:SB [%d:%d]\n",
	   fp->index, fp->sb_id);
	bnx2x_ack_sb(bp, fp->sb_id, USTORM_ID, 0, IGU_INT_DISABLE, 0);

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return IRQ_HANDLED;
#endif

	/* Handle Rx and Tx according to MSI-X vector */
	prefetch(fp->rx_cons_sb);
	prefetch(fp->tx_cons_sb);
	prefetch(&fp->status_blk->u_status_block.status_block_index);
	prefetch(&fp->status_blk->c_status_block.status_block_index);
#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
	napi_schedule(&bnx2x_fp(bp, fp->index, napi));
#else
	napi_schedule(&bnx2x_fp(bp, fp->index, dummy_netdev));
#endif

	return IRQ_HANDLED;
}
#endif

#if (LINUX_VERSION_CODE < 0x020613) && (VMWARE_ESX_DDK_VERSION < 40000)
static irqreturn_t bnx2x_interrupt(int irq, void *dev_instance,
				   struct pt_regs *regs)
#else /* BNX2X_UPSTREAM */
static irqreturn_t bnx2x_interrupt(int irq, void *dev_instance)
#endif
{
	struct bnx2x *bp = netdev_priv(dev_instance);
	u16 status = bnx2x_ack_int(bp);
	u16 mask;
	int i;

	/* Return here if interrupt is shared and it's not for us */
	if (unlikely(status == 0)) {
		DP(NETIF_MSG_INTR, "not our interrupt!\n");
		return IRQ_NONE;
	}
	DP(NETIF_MSG_INTR, "got an interrupt  status 0x%x\n", status);

	/* Return here if interrupt is disabled */
	if (unlikely(atomic_read(&bp->intr_sem) != 0)) {
		DP(NETIF_MSG_INTR, "called but intr_sem not 0, returning\n");
		return IRQ_HANDLED;
	}

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return IRQ_HANDLED;
#endif

	for (i = 0; i < BNX2X_NUM_QUEUES(bp); i++) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		mask = 0x2 << fp->sb_id;
		if (status & mask) {
			/* Handle Rx and Tx according to SB id */
			prefetch(fp->rx_cons_sb);
			prefetch(&fp->status_blk->u_status_block.
						status_block_index);
			prefetch(fp->tx_cons_sb);
			prefetch(&fp->status_blk->c_status_block.
						status_block_index);
#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
			napi_schedule(&bnx2x_fp(bp, fp->index, napi));
#else
			napi_schedule(&bnx2x_fp(bp, fp->index, dummy_netdev));
#endif
			status &= ~mask;
		}
	}

#ifdef BCM_CNIC
	mask = 0x2 << CNIC_SB_ID(bp);
	if (status & mask) {
		struct cnic_ops *c_ops = NULL;

		rcu_read_lock();
		c_ops = rcu_dereference(bp->cnic_ops);
		if (c_ops)
			c_ops->cnic_handler(bp->cnic_data, NULL);
		rcu_read_unlock();

		status &= ~mask;
	}
#endif

	if (unlikely(status & 0x1)) {
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
		queue_delayed_work(bnx2x_wq, &bp->sp_task, 0);
#else
		queue_work(bnx2x_wq, &bp->sp_task);
#endif

		status &= ~0x1;
		if (!status)
			return IRQ_HANDLED;
	}

	if (unlikely(status))
		DP(NETIF_MSG_INTR, "got an unknown interrupt! (status 0x%x)\n",
		   status);

	return IRQ_HANDLED;
}

/* end of fast path */

static void bnx2x_stats_handle(struct bnx2x *bp, enum bnx2x_stats_event event);

/* Link */

/*
 * General service functions
 */

static int bnx2x_acquire_hw_lock(struct bnx2x *bp, u32 resource)
{
	u32 lock_status;
	u32 resource_bit = (1 << resource);
	int func = BP_FUNC(bp);
	u32 hw_lock_control_reg;
	int cnt;

	/* Validating that the resource is within range */
	if (resource > HW_LOCK_MAX_RESOURCE_VALUE) {
		DP(NETIF_MSG_HW,
		   "resource(0x%x) > HW_LOCK_MAX_RESOURCE_VALUE(0x%x)\n",
		   resource, HW_LOCK_MAX_RESOURCE_VALUE);
		return -EINVAL;
	}

	if (func <= 5) {
		hw_lock_control_reg = (MISC_REG_DRIVER_CONTROL_1 + func*8);
	} else {
		hw_lock_control_reg =
				(MISC_REG_DRIVER_CONTROL_7 + (func - 6)*8);
	}

	/* Validating that the resource is not already taken */
	lock_status = REG_RD(bp, hw_lock_control_reg);
	if (lock_status & resource_bit) {
		DP(NETIF_MSG_HW, "lock_status 0x%x  resource_bit 0x%x\n",
		   lock_status, resource_bit);
		return -EEXIST;
	}

	/* Try for 5 second every 5ms */
	for (cnt = 0; cnt < 1000; cnt++) {
		/* Try to acquire the lock */
		REG_WR(bp, hw_lock_control_reg + 4, resource_bit);
		lock_status = REG_RD(bp, hw_lock_control_reg);
		if (lock_status & resource_bit)
			return 0;

		msleep(5);
	}
	DP(NETIF_MSG_HW, "Timeout\n");
	return -EAGAIN;
}

static int bnx2x_release_hw_lock(struct bnx2x *bp, u32 resource)
{
	u32 lock_status;
	u32 resource_bit = (1 << resource);
	int func = BP_FUNC(bp);
	u32 hw_lock_control_reg;

	/* Validating that the resource is within range */
	if (resource > HW_LOCK_MAX_RESOURCE_VALUE) {
		DP(NETIF_MSG_HW,
		   "resource(0x%x) > HW_LOCK_MAX_RESOURCE_VALUE(0x%x)\n",
		   resource, HW_LOCK_MAX_RESOURCE_VALUE);
		return -EINVAL;
	}

	if (func <= 5) {
		hw_lock_control_reg = (MISC_REG_DRIVER_CONTROL_1 + func*8);
	} else {
		hw_lock_control_reg =
				(MISC_REG_DRIVER_CONTROL_7 + (func - 6)*8);
	}

	/* Validating that the resource is currently taken */
	lock_status = REG_RD(bp, hw_lock_control_reg);
	if (!(lock_status & resource_bit)) {
		DP(NETIF_MSG_HW, "lock_status 0x%x  resource_bit 0x%x\n",
		   lock_status, resource_bit);
		return -EFAULT;
	}

	REG_WR(bp, hw_lock_control_reg, resource_bit);
	return 0;
}

/* HW Lock for shared dual port PHYs */
static void bnx2x_acquire_phy_lock(struct bnx2x *bp)
{
	mutex_lock(&bp->port.phy_mutex);

	if (bp->port.need_hw_lock)
		bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_MDIO);
}

static void bnx2x_release_phy_lock(struct bnx2x *bp)
{
	if (bp->port.need_hw_lock)
		bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_MDIO);

	mutex_unlock(&bp->port.phy_mutex);
}

int bnx2x_get_gpio(struct bnx2x *bp, int gpio_num, u8 port)
{
	/* The GPIO should be swapped if swap register is set and active */
	int gpio_port = (REG_RD(bp, NIG_REG_PORT_SWAP) &&
			 REG_RD(bp, NIG_REG_STRAP_OVERRIDE)) ^ port;
	int gpio_shift = gpio_num +
			(gpio_port ? MISC_REGISTERS_GPIO_PORT_SHIFT : 0);
	u32 gpio_mask = (1 << gpio_shift);
	u32 gpio_reg;
	int value;

	if (gpio_num > MISC_REGISTERS_GPIO_3) {
		BNX2X_ERR("Invalid GPIO %d\n", gpio_num);
		return -EINVAL;
	}

	/* read GPIO value */
	gpio_reg = REG_RD(bp, MISC_REG_GPIO);

	/* get the requested pin value */
	if ((gpio_reg & gpio_mask) == gpio_mask)
		value = 1;
	else
		value = 0;

	DP(NETIF_MSG_LINK, "pin %d  value 0x%x\n", gpio_num, value);

	return value;
}

int bnx2x_set_gpio(struct bnx2x *bp, int gpio_num, u32 mode, u8 port)
{
	/* The GPIO should be swapped if swap register is set and active */
	int gpio_port = (REG_RD(bp, NIG_REG_PORT_SWAP) &&
			 REG_RD(bp, NIG_REG_STRAP_OVERRIDE)) ^ port;
	int gpio_shift = gpio_num +
			(gpio_port ? MISC_REGISTERS_GPIO_PORT_SHIFT : 0);
	u32 gpio_mask = (1 << gpio_shift);
	u32 gpio_reg;

	if (gpio_num > MISC_REGISTERS_GPIO_3) {
		BNX2X_ERR("Invalid GPIO %d\n", gpio_num);
		return -EINVAL;
	}

	bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_GPIO);
	/* read GPIO and mask except the float bits */
	gpio_reg = (REG_RD(bp, MISC_REG_GPIO) & MISC_REGISTERS_GPIO_FLOAT);

	switch (mode) {
	case MISC_REGISTERS_GPIO_OUTPUT_LOW:
		DP(NETIF_MSG_LINK, "Set GPIO %d (shift %d) -> output low\n",
		   gpio_num, gpio_shift);
		/* clear FLOAT and set CLR */
		gpio_reg &= ~(gpio_mask << MISC_REGISTERS_GPIO_FLOAT_POS);
		gpio_reg |=  (gpio_mask << MISC_REGISTERS_GPIO_CLR_POS);
		break;

	case MISC_REGISTERS_GPIO_OUTPUT_HIGH:
		DP(NETIF_MSG_LINK, "Set GPIO %d (shift %d) -> output high\n",
		   gpio_num, gpio_shift);
		/* clear FLOAT and set SET */
		gpio_reg &= ~(gpio_mask << MISC_REGISTERS_GPIO_FLOAT_POS);
		gpio_reg |=  (gpio_mask << MISC_REGISTERS_GPIO_SET_POS);
		break;

	case MISC_REGISTERS_GPIO_INPUT_HI_Z:
		DP(NETIF_MSG_LINK, "Set GPIO %d (shift %d) -> input\n",
		   gpio_num, gpio_shift);
		/* set FLOAT */
		gpio_reg |= (gpio_mask << MISC_REGISTERS_GPIO_FLOAT_POS);
		break;

	default:
		break;
	}

	REG_WR(bp, MISC_REG_GPIO, gpio_reg);
	bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_GPIO);

	return 0;
}

int bnx2x_set_gpio_int(struct bnx2x *bp, int gpio_num, u32 mode, u8 port)
{
	/* The GPIO should be swapped if swap register is set and active */
	int gpio_port = (REG_RD(bp, NIG_REG_PORT_SWAP) &&
			 REG_RD(bp, NIG_REG_STRAP_OVERRIDE)) ^ port;
	int gpio_shift = gpio_num +
			(gpio_port ? MISC_REGISTERS_GPIO_PORT_SHIFT : 0);
	u32 gpio_mask = (1 << gpio_shift);
	u32 gpio_reg;

	if (gpio_num > MISC_REGISTERS_GPIO_3) {
		BNX2X_ERR("Invalid GPIO %d\n", gpio_num);
		return -EINVAL;
	}

	bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_GPIO);
	/* read GPIO int */
	gpio_reg = REG_RD(bp, MISC_REG_GPIO_INT);

	switch (mode) {
	case MISC_REGISTERS_GPIO_INT_OUTPUT_CLR:
		DP(NETIF_MSG_LINK, "Clear GPIO INT %d (shift %d) -> "
				   "output low\n", gpio_num, gpio_shift);
		/* clear SET and set CLR */
		gpio_reg &= ~(gpio_mask << MISC_REGISTERS_GPIO_INT_SET_POS);
		gpio_reg |=  (gpio_mask << MISC_REGISTERS_GPIO_INT_CLR_POS);
		break;

	case MISC_REGISTERS_GPIO_INT_OUTPUT_SET:
		DP(NETIF_MSG_LINK, "Set GPIO INT %d (shift %d) -> "
				   "output high\n", gpio_num, gpio_shift);
		/* clear CLR and set SET */
		gpio_reg &= ~(gpio_mask << MISC_REGISTERS_GPIO_INT_CLR_POS);
		gpio_reg |=  (gpio_mask << MISC_REGISTERS_GPIO_INT_SET_POS);
		break;

	default:
		break;
	}

	REG_WR(bp, MISC_REG_GPIO_INT, gpio_reg);
	bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_GPIO);

	return 0;
}

static int bnx2x_set_spio(struct bnx2x *bp, int spio_num, u32 mode)
{
	u32 spio_mask = (1 << spio_num);
	u32 spio_reg;

	if ((spio_num < MISC_REGISTERS_SPIO_4) ||
	    (spio_num > MISC_REGISTERS_SPIO_7)) {
		BNX2X_ERR("Invalid SPIO %d\n", spio_num);
		return -EINVAL;
	}

	bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_SPIO);
	/* read SPIO and mask except the float bits */
	spio_reg = (REG_RD(bp, MISC_REG_SPIO) & MISC_REGISTERS_SPIO_FLOAT);

	switch (mode) {
	case MISC_REGISTERS_SPIO_OUTPUT_LOW:
		DP(NETIF_MSG_LINK, "Set SPIO %d -> output low\n", spio_num);
		/* clear FLOAT and set CLR */
		spio_reg &= ~(spio_mask << MISC_REGISTERS_SPIO_FLOAT_POS);
		spio_reg |=  (spio_mask << MISC_REGISTERS_SPIO_CLR_POS);
		break;

	case MISC_REGISTERS_SPIO_OUTPUT_HIGH:
		DP(NETIF_MSG_LINK, "Set SPIO %d -> output high\n", spio_num);
		/* clear FLOAT and set SET */
		spio_reg &= ~(spio_mask << MISC_REGISTERS_SPIO_FLOAT_POS);
		spio_reg |=  (spio_mask << MISC_REGISTERS_SPIO_SET_POS);
		break;

	case MISC_REGISTERS_SPIO_INPUT_HI_Z:
		DP(NETIF_MSG_LINK, "Set SPIO %d -> input\n", spio_num);
		/* set FLOAT */
		spio_reg |= (spio_mask << MISC_REGISTERS_SPIO_FLOAT_POS);
		break;

	default:
		break;
	}

	REG_WR(bp, MISC_REG_SPIO, spio_reg);
	bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_SPIO);

	return 0;
}

static void bnx2x_calc_fc_adv(struct bnx2x *bp)
{
	switch (bp->link_vars.ieee_fc &
		MDIO_COMBO_IEEE0_AUTO_NEG_ADV_PAUSE_MASK) {
	case MDIO_COMBO_IEEE0_AUTO_NEG_ADV_PAUSE_NONE:
		bp->port.advertising &= ~(ADVERTISED_Asym_Pause |
					  ADVERTISED_Pause);
		break;

	case MDIO_COMBO_IEEE0_AUTO_NEG_ADV_PAUSE_BOTH:
		bp->port.advertising |= (ADVERTISED_Asym_Pause |
					 ADVERTISED_Pause);
		break;

	case MDIO_COMBO_IEEE0_AUTO_NEG_ADV_PAUSE_ASYMMETRIC:
		bp->port.advertising |= ADVERTISED_Asym_Pause;
		break;

	default:
		bp->port.advertising &= ~(ADVERTISED_Asym_Pause |
					  ADVERTISED_Pause);
		break;
	}
}

static void bnx2x_link_report(struct bnx2x *bp)
{
	if (bp->flags & MF_FUNC_DIS) {
		netif_carrier_off(bp->dev);
		printk(KERN_ERR PFX "%s NIC Link is Down\n", bp->dev->name);
		return;
	}

	if (bp->link_vars.link_up) {
		u16 line_speed;

		if (bp->state == BNX2X_STATE_OPEN)
			netif_carrier_on(bp->dev);
		printk(KERN_INFO PFX "%s NIC Link is Up, ", bp->dev->name);

		line_speed = bp->link_vars.line_speed;
		if (IS_E1HMF(bp)) {
			u16 vn_max_rate;

			vn_max_rate =
				((bp->mf_config & FUNC_MF_CFG_MAX_BW_MASK) >>
				 FUNC_MF_CFG_MAX_BW_SHIFT) * 100;
			if (vn_max_rate < line_speed)
				line_speed = vn_max_rate;
		}
		printk("%d Mbps ", line_speed);

		if (bp->link_vars.duplex == DUPLEX_FULL)
			printk("full duplex");
		else
			printk("half duplex");

		if (bp->link_vars.flow_ctrl != BNX2X_FLOW_CTRL_NONE) {
			if (bp->link_vars.flow_ctrl & BNX2X_FLOW_CTRL_RX) {
				printk(", receive ");
				if (bp->link_vars.flow_ctrl &
				    BNX2X_FLOW_CTRL_TX)
					printk("& transmit ");
			} else {
				printk(", transmit ");
			}
			printk("flow control ON");
		}
		printk("\n");

	} else { /* link_down */
		netif_carrier_off(bp->dev);
		printk(KERN_ERR PFX "%s NIC Link is Down\n", bp->dev->name);
	}
}

static u8 bnx2x_initial_phy_init(struct bnx2x *bp, int load_mode)
{
	if (!BP_NOMCP(bp)) {
		u8 rc;

		/* Initialize link parameters structure variables */
		/* It is recommended to turn off RX FC for jumbo frames
		   for better performance */
		if (bp->dev->mtu > 5000)
			bp->link_params.req_fc_auto_adv = BNX2X_FLOW_CTRL_TX;
		else
			bp->link_params.req_fc_auto_adv = BNX2X_FLOW_CTRL_BOTH;

		bnx2x_acquire_phy_lock(bp);

		if (load_mode == LOAD_DIAG)
			bp->link_params.loopback_mode = LOOPBACK_XGXS_10;

		rc = bnx2x_phy_init(&bp->link_params, &bp->link_vars);

		bnx2x_release_phy_lock(bp);

		bnx2x_calc_fc_adv(bp);

		if (CHIP_REV_IS_SLOW(bp) && bp->link_vars.link_up) {
			bnx2x_stats_handle(bp, STATS_EVENT_LINK_UP);
			bnx2x_link_report(bp);
		}

		return rc;
	}
	BNX2X_ERR("Bootcode is missing - can not initialize link\n");
	return -EINVAL;
}

static void bnx2x_link_set(struct bnx2x *bp)
{
	if (!BP_NOMCP(bp)) {
		bnx2x_acquire_phy_lock(bp);
		bnx2x_phy_init(&bp->link_params, &bp->link_vars);
		bnx2x_release_phy_lock(bp);

		bnx2x_calc_fc_adv(bp);
	} else
		BNX2X_ERR("Bootcode is missing - can not set link\n");
}

static void bnx2x__link_reset(struct bnx2x *bp)
{
	if (!BP_NOMCP(bp)) {
		bnx2x_acquire_phy_lock(bp);
		bnx2x_link_reset(&bp->link_params, &bp->link_vars, 1);
		bnx2x_release_phy_lock(bp);
	} else
		BNX2X_ERR("Bootcode is missing - can not reset link\n");
}

static u8 bnx2x_link_test(struct bnx2x *bp)
{
	u8 rc = 0;

	if (!BP_NOMCP(bp)) {
		bnx2x_acquire_phy_lock(bp);
		rc = bnx2x_test_link(&bp->link_params, &bp->link_vars);
		bnx2x_release_phy_lock(bp);
	} else
		BNX2X_ERR("Bootcode is missing - can not test link\n");

	return rc;
}

static void bnx2x_init_port_minmax(struct bnx2x *bp)
{
	u32 r_param = bp->link_vars.line_speed / 8;
	u32 fair_periodic_timeout_usec;
	u32 t_fair;

	memset(&(bp->cmng.rs_vars), 0,
	       sizeof(struct rate_shaping_vars_per_port));
	memset(&(bp->cmng.fair_vars), 0, sizeof(struct fairness_vars_per_port));

	/* 100 usec in SDM ticks = 25 since each tick is 4 usec */
	bp->cmng.rs_vars.rs_periodic_timeout = RS_PERIODIC_TIMEOUT_USEC / 4;

	/* this is the threshold below which no timer arming will occur
	   1.25 coefficient is for the threshold to be a little bigger
	   than the real time, to compensate for timer in-accuracy */
	bp->cmng.rs_vars.rs_threshold =
				(RS_PERIODIC_TIMEOUT_USEC * r_param * 5) / 4;

	/* resolution of fairness timer */
	fair_periodic_timeout_usec = QM_ARB_BYTES / r_param;
	/* for 10G it is 1000usec. for 1G it is 10000usec. */
	t_fair = T_FAIR_COEF / bp->link_vars.line_speed;

	/* this is the threshold below which we won't arm the timer anymore */
	bp->cmng.fair_vars.fair_threshold = QM_ARB_BYTES;

	/* we multiply by 1e3/8 to get bytes/msec.
	   We don't want the credits to pass a credit
	   of the t_fair*FAIR_MEM (algorithm resolution) */
	bp->cmng.fair_vars.upper_bound = r_param * t_fair * FAIR_MEM;
	/* since each tick is 4 usec */
	bp->cmng.fair_vars.fairness_timeout = fair_periodic_timeout_usec / 4;
}

/* Calculates the sum of vn_min_rates.
   It's needed for further normalizing of the min_rates.
   Returns:
     sum of vn_min_rates.
       or
     0 - if all the min_rates are 0.
     In the later case fainess algorithm should be deactivated.
     If not all min_rates are zero then those that are zeroes will be set to 1.
 */
static void bnx2x_calc_vn_weight_sum(struct bnx2x *bp)
{
	int all_zero = 1;
	int port = BP_PORT(bp);
	int vn;

	bp->vn_weight_sum = 0;
	for (vn = VN_0; vn < E1HVN_MAX; vn++) {
		int func = 2*vn + port;
		u32 vn_cfg = SHMEM_RD(bp, mf_cfg.func_mf_config[func].config);
		u32 vn_min_rate = ((vn_cfg & FUNC_MF_CFG_MIN_BW_MASK) >>
				   FUNC_MF_CFG_MIN_BW_SHIFT) * 100;

		/* Skip hidden vns */
		if (vn_cfg & FUNC_MF_CFG_FUNC_HIDE)
			continue;

		/* If min rate is zero - set it to 1 */
		if (!vn_min_rate)
			vn_min_rate = DEF_MIN_RATE;
		else
			all_zero = 0;

		bp->vn_weight_sum += vn_min_rate;
	}

	/* ... only if all min rates are zeros - disable fairness */
	if (all_zero) {
		bp->cmng.flags.cmng_enables &=
					~CMNG_FLAGS_PER_PORT_FAIRNESS_VN;
		DP(NETIF_MSG_IFUP, "All MIN values are zeroes"
		   "  fairness will be disabled\n");
	} else
		bp->cmng.flags.cmng_enables |=
					CMNG_FLAGS_PER_PORT_FAIRNESS_VN;
}

static void bnx2x_init_vn_minmax(struct bnx2x *bp, int func)
{
	struct rate_shaping_vars_per_vn m_rs_vn;
	struct fairness_vars_per_vn m_fair_vn;
	u32 vn_cfg = SHMEM_RD(bp, mf_cfg.func_mf_config[func].config);
	u16 vn_min_rate, vn_max_rate;
	int i;

	/* If function is hidden - set min and max to zeroes */
	if (vn_cfg & FUNC_MF_CFG_FUNC_HIDE) {
		vn_min_rate = 0;
		vn_max_rate = 0;

	} else {
		vn_min_rate = ((vn_cfg & FUNC_MF_CFG_MIN_BW_MASK) >>
				FUNC_MF_CFG_MIN_BW_SHIFT) * 100;
		/* If min rate is zero - set it to 1 */
		if (!vn_min_rate)
			vn_min_rate = DEF_MIN_RATE;
		vn_max_rate = ((vn_cfg & FUNC_MF_CFG_MAX_BW_MASK) >>
				FUNC_MF_CFG_MAX_BW_SHIFT) * 100;
	}
	DP(NETIF_MSG_IFUP,
	   "func %d: vn_min_rate %d  vn_max_rate %d  vn_weight_sum %d\n",
	   func, vn_min_rate, vn_max_rate, bp->vn_weight_sum);

	memset(&m_rs_vn, 0, sizeof(struct rate_shaping_vars_per_vn));
	memset(&m_fair_vn, 0, sizeof(struct fairness_vars_per_vn));

	/* global vn counter - maximal Mbps for this vn */
	m_rs_vn.vn_counter.rate = vn_max_rate;

	/* quota - number of bytes transmitted in this period */
	m_rs_vn.vn_counter.quota =
				(vn_max_rate * RS_PERIODIC_TIMEOUT_USEC) / 8;

	if (bp->vn_weight_sum) {
		/* credit for each period of the fairness algorithm:
		   number of bytes in T_FAIR (the vn share the port rate).
		   vn_weight_sum should not be larger than 10000, thus
		   T_FAIR_COEF / (8 * vn_weight_sum) will always be greater
		   than zero */
		m_fair_vn.vn_credit_delta =
			max_t(u32, (vn_min_rate * (T_FAIR_COEF /
						   (8 * bp->vn_weight_sum))),
			      (bp->cmng.fair_vars.fair_threshold * 2));
		DP(NETIF_MSG_IFUP, "m_fair_vn.vn_credit_delta %d\n",
		   m_fair_vn.vn_credit_delta);
	}

	/* Store it to internal memory */
	for (i = 0; i < sizeof(struct rate_shaping_vars_per_vn)/4; i++)
		REG_WR(bp, BAR_XSTRORM_INTMEM +
		       XSTORM_RATE_SHAPING_PER_VN_VARS_OFFSET(func) + i * 4,
		       ((u32 *)(&m_rs_vn))[i]);

	for (i = 0; i < sizeof(struct fairness_vars_per_vn)/4; i++)
		REG_WR(bp, BAR_XSTRORM_INTMEM +
		       XSTORM_FAIRNESS_PER_VN_VARS_OFFSET(func) + i * 4,
		       ((u32 *)(&m_fair_vn))[i]);
}

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
static void bnx2x_init_cos_credit(struct bnx2x *bp)
{
	struct fairness_vars_per_vn m_fair_vn;
	int func = BP_FUNC(bp);
	int i, cos;

	memset(&m_fair_vn, 0, sizeof(struct fairness_vars_per_vn));

	for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
		if (bp->cos_min_rate[cos]) {
			m_fair_vn.cos_credit_delta[cos] =
			max_t(u32, (bp->cos_min_rate[cos] *
				    (T_FAIR_COEF / (8 * bp->cos_weight_sum))),
			      (bp->cmng.fair_vars.fair_threshold * 2));
		}
		DP(NETIF_MSG_IFUP, "cos_credit_delta[%d] = %d\n",
		   cos, m_fair_vn.cos_credit_delta[cos]);
	}

	for (i = 0; i < sizeof(struct fairness_vars_per_vn)/4; i++)
		REG_WR(bp, BAR_XSTRORM_INTMEM +
		       XSTORM_FAIRNESS_PER_VN_VARS_OFFSET(func) + i * 4,
		       ((u32 *)(&m_fair_vn))[i]);
}
#endif

/* This function is called upon link interrupt */
static void bnx2x_link_attn(struct bnx2x *bp)
{
	u32 prev_link_status = bp->link_vars.link_status;
	/* Make sure that we are synced with the current statistics */
	bnx2x_stats_handle(bp, STATS_EVENT_STOP);
	
	bnx2x_link_update(&bp->link_params, &bp->link_vars);

	if (bp->link_vars.link_up) {

		/* dropless flow control */
		if (CHIP_IS_E1H(bp) && bp->dropless_fc) {
			int port = BP_PORT(bp);
			u32 pause_enabled = 0;

#ifdef BNX2X_SAFC
			if ((bp->link_vars.flow_ctrl & BNX2X_FLOW_CTRL_TX) ||
			    (bp->flags & SAFC_TX_FLAG))
#else /* BNX2X_UPSTREAM */
			if (bp->link_vars.flow_ctrl & BNX2X_FLOW_CTRL_TX)
#endif
				pause_enabled = 1;

			REG_WR(bp, BAR_USTRORM_INTMEM +
			       USTORM_ETH_PAUSE_ENABLED_OFFSET(port),
			       pause_enabled);
		}

		if (bp->link_vars.mac_type == MAC_TYPE_BMAC) {
			struct host_port_stats *pstats;

			pstats = bnx2x_sp(bp, port_stats);
			/* reset old bmac stats */
			memset(&(pstats->mac_stx[0]), 0,
			       sizeof(struct mac_stx));
		}
		if (bp->state == BNX2X_STATE_OPEN)
			bnx2x_stats_handle(bp, STATS_EVENT_LINK_UP);
	}

	/* indicate link status only if link status actually changed */
	if (prev_link_status != bp->link_vars.link_status)
		bnx2x_link_report(bp);

	if (IS_E1HMF(bp)) {
		int port = BP_PORT(bp);
		int func;
		int vn;

		/* Set the attention towards other drivers on the same port */
		for (vn = VN_0; vn < E1HVN_MAX; vn++) {
			if (vn == BP_E1HVN(bp))
				continue;

			func = ((vn << 1) | port);
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_0 +
			       (LINK_SYNC_ATTENTION_BIT_FUNC_0 + func)*4, 1);
		}

		if (bp->link_vars.link_up) {
			int i;

			/* Init rate shaping and fairness contexts */
			bnx2x_init_port_minmax(bp);

			for (vn = VN_0; vn < E1HVN_MAX; vn++)
				bnx2x_init_vn_minmax(bp, 2*vn + port);

			/* Store it to internal memory */
			for (i = 0;
			     i < sizeof(struct cmng_struct_per_port) / 4; i++)
				REG_WR(bp, BAR_XSTRORM_INTMEM +
				  XSTORM_CMNG_PER_PORT_VARS_OFFSET(port) + i*4,
				       ((u32 *)(&bp->cmng))[i]);
		}
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	} else {
		if (bp->link_vars.link_up && bp->cos_weight_sum) {
			int port = BP_PORT(bp);
			int i;

			/* Init fairness context */
			bnx2x_init_port_minmax(bp);

			bnx2x_init_cos_credit(bp);

			/* Store it to internal memory */
			for (i = 0;
			     i < sizeof(struct cmng_struct_per_port) / 4; i++)
				REG_WR(bp, BAR_XSTRORM_INTMEM +
				  XSTORM_CMNG_PER_PORT_VARS_OFFSET(port) + i*4,
				       ((u32 *)(&bp->cmng))[i]);
		}
#endif
	}
}

static void bnx2x__link_status_update(struct bnx2x *bp)
{
	if ((bp->state != BNX2X_STATE_OPEN) || (bp->flags & MF_FUNC_DIS))
		return;

	bnx2x_link_status_update(&bp->link_params, &bp->link_vars);

	if (bp->link_vars.link_up)
		bnx2x_stats_handle(bp, STATS_EVENT_LINK_UP);
	else
		bnx2x_stats_handle(bp, STATS_EVENT_STOP);

	bnx2x_calc_vn_weight_sum(bp);

	/* indicate link status */
	bnx2x_link_report(bp);
}

static void bnx2x_pmf_update(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	u32 val;

	bp->port.pmf = 1;
	DP(NETIF_MSG_LINK, "pmf %d\n", bp->port.pmf);

	/* enable nig attention */
	val = (0xff0f | (1 << (BP_E1HVN(bp) + 4)));
	REG_WR(bp, HC_REG_TRAILING_EDGE_0 + port*8, val);
	REG_WR(bp, HC_REG_LEADING_EDGE_0 + port*8, val);

	bnx2x_stats_handle(bp, STATS_EVENT_PMF);
}

/* end of Link */

/* slow path */

/*
 * General service functions
 */

/* send the MCP a request, block until there is a reply */
u32 bnx2x_fw_command(struct bnx2x *bp, u32 command)
{
	int func = BP_FUNC(bp);
	u32 seq = ++bp->fw_seq;
	u32 rc = 0;
	u32 cnt = 1;
	u8 delay = CHIP_REV_IS_SLOW(bp) ? 100 : 10;

	mutex_lock(&bp->fw_mb_mutex);
	SHMEM_WR(bp, func_mb[func].drv_mb_header, (command | seq));
	DP(BNX2X_MSG_MCP, "wrote command (%x) to FW MB\n", (command | seq));

	do {
		/* let the FW do it's magic ... */
		msleep(delay);

		rc = SHMEM_RD(bp, func_mb[func].fw_mb_header);

		/* Give the FW up to 5 second (500*10ms) */
	} while ((seq != (rc & FW_MSG_SEQ_NUMBER_MASK)) && (cnt++ < 500));

	DP(BNX2X_MSG_MCP, "[after %d ms] read (%x) seq is (%x) from FW MB\n",
	   cnt*delay, rc, seq);

	/* is this a reply to our command? */
	if (seq == (rc & FW_MSG_SEQ_NUMBER_MASK))
		rc &= FW_MSG_CODE_MASK;
	else {
		/* FW BUG! */
		BNX2X_ERR("FW failed to respond!\n");
		bnx2x_fw_dump(bp);
		rc = 0;
	}
	mutex_unlock(&bp->fw_mb_mutex);

	return rc;
}

static void bnx2x_set_eth_mac_addr_e1h(struct bnx2x *bp, int set);
static void bnx2x_set_rx_mode(struct net_device *dev);

static void bnx2x_e1h_disable(struct bnx2x *bp)
{
	int port = BP_PORT(bp);

	netif_tx_disable(bp->dev);
#if (LINUX_VERSION_CODE < 0x02061f) /* ! BNX2X_UPSTREAM */
	/* In kernels starting from 2.6.31 netdev layer does this */
	bp->dev->trans_start = jiffies;	/* prevent tx timeout */
#endif

	REG_WR(bp, NIG_REG_LLH0_FUNC_EN + port*8, 0);

	netif_carrier_off(bp->dev);
}

static void bnx2x_e1h_enable(struct bnx2x *bp)
{
	int port = BP_PORT(bp);

	REG_WR(bp, NIG_REG_LLH0_FUNC_EN + port*8, 1);

	/* Tx queue should be only reenabled */
	netif_tx_wake_all_queues(bp->dev);

	/*
	 * Should not call netif_carrier_on since it will be called if the link
	 * is up when checking for link state
	 */
}

static void bnx2x_update_min_max(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int vn, i;

	/* Init rate shaping and fairness contexts */
	bnx2x_init_port_minmax(bp);

	bnx2x_calc_vn_weight_sum(bp);

	for (vn = VN_0; vn < E1HVN_MAX; vn++)
		bnx2x_init_vn_minmax(bp, 2*vn + port);

	if (bp->port.pmf) {
		int func;

		/* Set the attention towards other drivers on the same port */
		for (vn = VN_0; vn < E1HVN_MAX; vn++) {
			if (vn == BP_E1HVN(bp))
				continue;

			func = ((vn << 1) | port);
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_0 +
			       (LINK_SYNC_ATTENTION_BIT_FUNC_0 + func)*4, 1);
		}

		/* Store it to internal memory */
		for (i = 0; i < sizeof(struct cmng_struct_per_port) / 4; i++)
			REG_WR(bp, BAR_XSTRORM_INTMEM +
			       XSTORM_CMNG_PER_PORT_VARS_OFFSET(port) + i*4,
			       ((u32 *)(&bp->cmng))[i]);
	}
}

static void bnx2x_dcc_event(struct bnx2x *bp, u32 dcc_event)
{
	DP(BNX2X_MSG_MCP, "dcc_event 0x%x\n", dcc_event);

	if (dcc_event & DRV_STATUS_DCC_DISABLE_ENABLE_PF) {

		/*
		 * This is the only place besides the function initialization
		 * where the bp->flags can change so it is done without any
		 * locks
		 */
		if (bp->mf_config & FUNC_MF_CFG_FUNC_DISABLED) {
			DP(NETIF_MSG_IFDOWN, "mf_cfg function disabled\n");
			bp->flags |= MF_FUNC_DIS;

			bnx2x_e1h_disable(bp);
		} else {
			DP(NETIF_MSG_IFUP, "mf_cfg function enabled\n");
			bp->flags &= ~MF_FUNC_DIS;

			bnx2x_e1h_enable(bp);
		}
		dcc_event &= ~DRV_STATUS_DCC_DISABLE_ENABLE_PF;
	}
	if (dcc_event & DRV_STATUS_DCC_BANDWIDTH_ALLOCATION) {

		bnx2x_update_min_max(bp);
		dcc_event &= ~DRV_STATUS_DCC_BANDWIDTH_ALLOCATION;
	}

	/* Report results to MCP */
	if (dcc_event)
		bnx2x_fw_command(bp, DRV_MSG_CODE_DCC_FAILURE);
	else
		bnx2x_fw_command(bp, DRV_MSG_CODE_DCC_OK);
}

/* must be called under the spq lock */
static inline struct eth_spe *bnx2x_sp_get_next(struct bnx2x *bp)
{
	struct eth_spe *next_spe = bp->spq_prod_bd;

	if (bp->spq_prod_bd == bp->spq_last_bd) {
		bp->spq_prod_bd = bp->spq;
		bp->spq_prod_idx = 0;
		DP(NETIF_MSG_TIMER, "end of spq\n");
	} else {
		bp->spq_prod_bd++;
		bp->spq_prod_idx++;
	}
	return next_spe;
}

/* must be called under the spq lock */
static inline void bnx2x_sp_prod_update(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);

	/* Make sure that BD data is updated before writing the producer */
	wmb();

	REG_WR(bp, BAR_XSTRORM_INTMEM + XSTORM_SPQ_PROD_OFFSET(func),
	       bp->spq_prod_idx);
	mmiowb();
}

/* the slow path queue is odd since completions arrive on the fastpath ring */
int bnx2x_sp_post(struct bnx2x *bp, int command, int cid,
		  u32 data_hi, u32 data_lo, int common)
{
	struct eth_spe *spe;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return -EIO;
#endif

	spin_lock_bh(&bp->spq_lock);

	if (!bp->spq_left) {
		BNX2X_ERR("BUG! SPQ ring full!\n");
		spin_unlock_bh(&bp->spq_lock);
		bnx2x_panic();
		return -EBUSY;
	}

	spe = bnx2x_sp_get_next(bp);

	/* CID needs port number to be encoded int it */
	spe->hdr.conn_and_cmd_data =
			cpu_to_le32((command << SPE_HDR_CMD_ID_SHIFT) |
				    HW_CID(bp, cid));
	spe->hdr.type = cpu_to_le16(ETH_CONNECTION_TYPE);
	if (common)
		spe->hdr.type |=
			cpu_to_le16((1 << SPE_HDR_COMMON_RAMROD_SHIFT));
	spe->data.mac_config_addr.hi = cpu_to_le32(data_hi);
	spe->data.mac_config_addr.lo = cpu_to_le32(data_lo);

	bp->spq_left--;

	DP(BNX2X_MSG_SP/*NETIF_MSG_TIMER*/,
	   "SPQE[%x] (%x:%x)  command %d  hw_cid %x  data (%x:%x)  left %x\n",
	   bp->spq_prod_idx, (u32)U64_HI(bp->spq_mapping),
	   (u32)(U64_LO(bp->spq_mapping) +
	   (void *)bp->spq_prod_bd - (void *)bp->spq), command,
	   HW_CID(bp, cid), data_hi, data_lo, bp->spq_left);

	bnx2x_sp_prod_update(bp);
	spin_unlock_bh(&bp->spq_lock);
	return 0;
}

/* acquire split MCP access lock register */
static int bnx2x_acquire_alr(struct bnx2x *bp)
{
	u32 i, j, val;
	int rc = 0;

	might_sleep();
	i = 100;
	for (j = 0; j < i*10; j++) {
		val = (1UL << 31);
		REG_WR(bp, GRCBASE_MCP + 0x9c, val);
		val = REG_RD(bp, GRCBASE_MCP + 0x9c);
		if (val & (1L << 31))
			break;

		msleep(5);
	}
	if (!(val & (1L << 31))) {
		BNX2X_ERR("Cannot acquire MCP access lock register\n");
		rc = -EBUSY;
	}

	return rc;
}

/* release split MCP access lock register */
static void bnx2x_release_alr(struct bnx2x *bp)
{
	u32 val = 0;

	REG_WR(bp, GRCBASE_MCP + 0x9c, val);
}

static inline u16 bnx2x_update_dsb_idx(struct bnx2x *bp)
{
	struct host_def_status_block *def_sb = bp->def_status_blk;
	u16 rc = 0;

	barrier(); /* status block is written to by the chip */
	if (bp->def_att_idx != def_sb->atten_status_block.attn_bits_index) {
		bp->def_att_idx = def_sb->atten_status_block.attn_bits_index;
		rc |= 1;
	}
	if (bp->def_c_idx != def_sb->c_def_status_block.status_block_index) {
		bp->def_c_idx = def_sb->c_def_status_block.status_block_index;
		rc |= 2;
	}
	if (bp->def_u_idx != def_sb->u_def_status_block.status_block_index) {
		bp->def_u_idx = def_sb->u_def_status_block.status_block_index;
		rc |= 4;
	}
	if (bp->def_x_idx != def_sb->x_def_status_block.status_block_index) {
		bp->def_x_idx = def_sb->x_def_status_block.status_block_index;
		rc |= 8;
	}
	if (bp->def_t_idx != def_sb->t_def_status_block.status_block_index) {
		bp->def_t_idx = def_sb->t_def_status_block.status_block_index;
		rc |= 16;
	}
	return rc;
}

/*
 * slow path service functions
 */

static void bnx2x_attn_int_asserted(struct bnx2x *bp, u32 asserted)
{
	int port = BP_PORT(bp);
	u32 hc_addr = (HC_REG_COMMAND_REG + port*32 +
		       COMMAND_REG_ATTN_BITS_SET);
	u32 aeu_addr = port ? MISC_REG_AEU_MASK_ATTN_FUNC_1 :
			      MISC_REG_AEU_MASK_ATTN_FUNC_0;
	u32 nig_int_mask_addr = port ? NIG_REG_MASK_INTERRUPT_PORT1 :
				       NIG_REG_MASK_INTERRUPT_PORT0;
	u32 aeu_mask;
	u32 nig_mask = 0;

	if (bp->attn_state & asserted)
		BNX2X_ERR("IGU ERROR\n");

	bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_PORT0_ATT_MASK + port);
	aeu_mask = REG_RD(bp, aeu_addr);

	DP(NETIF_MSG_HW, "aeu_mask %x  newly asserted %x\n",
	   aeu_mask, asserted);
	aeu_mask &= ~(asserted & 0xff);
	DP(NETIF_MSG_HW, "new mask %x\n", aeu_mask);

	REG_WR(bp, aeu_addr, aeu_mask);
	bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_PORT0_ATT_MASK + port);

	DP(NETIF_MSG_HW, "attn_state %x\n", bp->attn_state);
	bp->attn_state |= asserted;
	DP(NETIF_MSG_HW, "new state %x\n", bp->attn_state);

	if (asserted & ATTN_HARD_WIRED_MASK) {
		if (asserted & ATTN_NIG_FOR_FUNC) {

			bnx2x_acquire_phy_lock(bp);

			/* save nig interrupt mask */
			nig_mask = REG_RD(bp, nig_int_mask_addr);
			REG_WR(bp, nig_int_mask_addr, 0);

			bnx2x_link_attn(bp);

			/* handle unicore attn? */
		}
		if (asserted & ATTN_SW_TIMER_4_FUNC)
			DP(NETIF_MSG_HW, "ATTN_SW_TIMER_4_FUNC!\n");

		if (asserted & GPIO_2_FUNC)
			DP(NETIF_MSG_HW, "GPIO_2_FUNC!\n");

		if (asserted & GPIO_3_FUNC)
			DP(NETIF_MSG_HW, "GPIO_3_FUNC!\n");

		if (asserted & GPIO_4_FUNC)
			DP(NETIF_MSG_HW, "GPIO_4_FUNC!\n");

		if (port == 0) {
			if (asserted & ATTN_GENERAL_ATTN_1) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_1!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_1, 0x0);
			}
			if (asserted & ATTN_GENERAL_ATTN_2) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_2!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_2, 0x0);
			}
			if (asserted & ATTN_GENERAL_ATTN_3) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_3!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_3, 0x0);
			}
		} else {
			if (asserted & ATTN_GENERAL_ATTN_4) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_4!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_4, 0x0);
			}
			if (asserted & ATTN_GENERAL_ATTN_5) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_5!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_5, 0x0);
			}
			if (asserted & ATTN_GENERAL_ATTN_6) {
				DP(NETIF_MSG_HW, "ATTN_GENERAL_ATTN_6!\n");
				REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_6, 0x0);
			}
		}

	} /* if hardwired */

	DP(NETIF_MSG_HW, "about to mask 0x%08x at HC addr 0x%x\n",
	   asserted, hc_addr);
	REG_WR(bp, hc_addr, asserted);

	/* now set back the mask */
	if (asserted & ATTN_NIG_FOR_FUNC) {
		REG_WR(bp, nig_int_mask_addr, nig_mask);
		bnx2x_release_phy_lock(bp);
	}
}

static inline void bnx2x_fan_failure(struct bnx2x *bp)
{
	int port = BP_PORT(bp);

	/* mark the failure */
	bp->link_params.ext_phy_config &= ~PORT_HW_CFG_XGXS_EXT_PHY_TYPE_MASK;
	bp->link_params.ext_phy_config |= PORT_HW_CFG_XGXS_EXT_PHY_TYPE_FAILURE;
	SHMEM_WR(bp, dev_info.port_hw_config[port].external_phy_config,
		 bp->link_params.ext_phy_config);

	/* log the failure */
	printk(KERN_ERR PFX "Fan Failure on Network Controller %s has caused"
	       " the driver to shutdown the card to prevent permanent"
	       " damage.  Please contact OEM Support for assistance\n",
	       bp->dev->name);
}

static inline void bnx2x_attn_int_deasserted0(struct bnx2x *bp, u32 attn)
{
	int port = BP_PORT(bp);
	int reg_offset;
	u32 val, swap_val, swap_override;

	reg_offset = (port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0 :
			     MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0);

	if (attn & AEU_INPUTS_ATTN_BITS_SPIO5) {

		val = REG_RD(bp, reg_offset);
		val &= ~AEU_INPUTS_ATTN_BITS_SPIO5;
		REG_WR(bp, reg_offset, val);

		BNX2X_ERR("SPIO5 hw attention\n");

		/* Fan failure attention */
		switch (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config)) {
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101:
			/* Low power mode is controlled by GPIO 2 */
			bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_2,
				       MISC_REGISTERS_GPIO_OUTPUT_LOW, port);
			/* The PHY reset is controlled by GPIO 1 */
			bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_1,
				       MISC_REGISTERS_GPIO_OUTPUT_LOW, port);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727:
			/* The PHY reset is controlled by GPIO 1 */
			/* fake the port number to cancel the swap done in
			   set_gpio() */
			swap_val = REG_RD(bp, NIG_REG_PORT_SWAP);
			swap_override = REG_RD(bp, NIG_REG_STRAP_OVERRIDE);
			port = (swap_val && swap_override) ^ 1;
			bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_1,
				       MISC_REGISTERS_GPIO_OUTPUT_LOW, port);
			break;

		default:
			break;
		}
		bnx2x_fan_failure(bp);
	}

	if (attn & (AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_0 |
		    AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_1)) {
		bnx2x_acquire_phy_lock(bp);
		bnx2x_handle_module_detect_int(&bp->link_params);
		bnx2x_release_phy_lock(bp);
	}

	if (attn & HW_INTERRUT_ASSERT_SET_0) {

		val = REG_RD(bp, reg_offset);
		val &= ~(attn & HW_INTERRUT_ASSERT_SET_0);
		REG_WR(bp, reg_offset, val);

		BNX2X_ERR("FATAL HW block attention set0 0x%x\n",
			  (u32)(attn & HW_INTERRUT_ASSERT_SET_0));
		bnx2x_panic();
	}
}

static inline void bnx2x_attn_int_deasserted1(struct bnx2x *bp, u32 attn)
{
	u32 val;

	if (attn & AEU_INPUTS_ATTN_BITS_DOORBELLQ_HW_INTERRUPT) {

		val = REG_RD(bp, DORQ_REG_DORQ_INT_STS_CLR);
		BNX2X_ERR("DB hw attention 0x%x\n", val);
		/* DORQ discard attention */
		if (val & 0x2)
			BNX2X_ERR("FATAL error from DORQ\n");
	}

	if (attn & HW_INTERRUT_ASSERT_SET_1) {

		int port = BP_PORT(bp);
		int reg_offset;

		reg_offset = (port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_1 :
				     MISC_REG_AEU_ENABLE1_FUNC_0_OUT_1);

		val = REG_RD(bp, reg_offset);
		val &= ~(attn & HW_INTERRUT_ASSERT_SET_1);
		REG_WR(bp, reg_offset, val);

		BNX2X_ERR("FATAL HW block attention set1 0x%x\n",
			  (u32)(attn & HW_INTERRUT_ASSERT_SET_1));
		bnx2x_panic();
	}
}

static inline void bnx2x_attn_int_deasserted2(struct bnx2x *bp, u32 attn)
{
	u32 val;

	if (attn & AEU_INPUTS_ATTN_BITS_CFC_HW_INTERRUPT) {

		val = REG_RD(bp, CFC_REG_CFC_INT_STS_CLR);
		BNX2X_ERR("CFC hw attention 0x%x\n", val);
		/* CFC error attention */
		if (val & 0x2)
			BNX2X_ERR("FATAL error from CFC\n");
	}

	if (attn & AEU_INPUTS_ATTN_BITS_PXP_HW_INTERRUPT) {

		val = REG_RD(bp, PXP_REG_PXP_INT_STS_CLR_0);
		BNX2X_ERR("PXP hw attention 0x%x\n", val);
		/* RQ_USDMDP_FIFO_OVERFLOW */
		if (val & 0x18000)
			BNX2X_ERR("FATAL error from PXP\n");
	}

	if (attn & HW_INTERRUT_ASSERT_SET_2) {

		int port = BP_PORT(bp);
		int reg_offset;

		reg_offset = (port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_2 :
				     MISC_REG_AEU_ENABLE1_FUNC_0_OUT_2);

		val = REG_RD(bp, reg_offset);
		val &= ~(attn & HW_INTERRUT_ASSERT_SET_2);
		REG_WR(bp, reg_offset, val);

		BNX2X_ERR("FATAL HW block attention set2 0x%x\n",
			  (u32)(attn & HW_INTERRUT_ASSERT_SET_2));
		bnx2x_panic();
	}
}

static inline void bnx2x_attn_int_deasserted3(struct bnx2x *bp, u32 attn)
{
	u32 val;

	if (attn & EVEREST_GEN_ATTN_IN_USE_MASK) {

		if (attn & BNX2X_PMF_LINK_ASSERT) {
			int func = BP_FUNC(bp);

			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_12 + func*4, 0);
			bp->mf_config = SHMEM_RD(bp,
					   mf_cfg.func_mf_config[func].config);
			val = SHMEM_RD(bp, func_mb[func].drv_status);
			if (val & DRV_STATUS_DCC_EVENT_MASK)
				bnx2x_dcc_event(bp,
					    (val & DRV_STATUS_DCC_EVENT_MASK));
			bnx2x__link_status_update(bp);
			if ((bp->port.pmf == 0) && (val & DRV_STATUS_PMF))
				bnx2x_pmf_update(bp);

		} else if (attn & BNX2X_MC_ASSERT_BITS) {

			BNX2X_ERR("MC assert!\n");
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_10, 0);
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_9, 0);
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_8, 0);
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_7, 0);
			bnx2x_panic();

		} else if (attn & BNX2X_MCP_ASSERT) {

			BNX2X_ERR("MCP assert!\n");
			REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_11, 0);
			bnx2x_fw_dump(bp);

		} else
			BNX2X_ERR("Unknown HW assert! (attn 0x%x)\n", attn);
	}

	if (attn & EVEREST_LATCHED_ATTN_IN_USE_MASK) {
		BNX2X_ERR("LATCHED attention 0x%08x (masked)\n", attn);
		if (attn & BNX2X_GRC_TIMEOUT) {
			val = CHIP_IS_E1H(bp) ?
				REG_RD(bp, MISC_REG_GRC_TIMEOUT_ATTN) : 0;
			BNX2X_ERR("GRC time-out 0x%08x\n", val);
		}
		if (attn & BNX2X_GRC_RSV) {
			val = CHIP_IS_E1H(bp) ?
				REG_RD(bp, MISC_REG_GRC_RSV_ATTN) : 0;
			BNX2X_ERR("GRC reserved 0x%08x\n", val);
		}
		REG_WR(bp, MISC_REG_AEU_CLR_LATCH_SIGNAL, 0x7ff);
	}
}

static void bnx2x_attn_int_deasserted(struct bnx2x *bp, u32 deasserted)
{
	struct attn_route attn;
	struct attn_route group_mask;
	int port = BP_PORT(bp);
	int index;
	u32 reg_addr;
	u32 val;
	u32 aeu_mask;

	/* need to take HW lock because MCP or other port might also
	   try to handle this event */
	bnx2x_acquire_alr(bp);

	attn.sig[0] = REG_RD(bp, MISC_REG_AEU_AFTER_INVERT_1_FUNC_0 + port*4);
	attn.sig[1] = REG_RD(bp, MISC_REG_AEU_AFTER_INVERT_2_FUNC_0 + port*4);
	attn.sig[2] = REG_RD(bp, MISC_REG_AEU_AFTER_INVERT_3_FUNC_0 + port*4);
	attn.sig[3] = REG_RD(bp, MISC_REG_AEU_AFTER_INVERT_4_FUNC_0 + port*4);
	DP(NETIF_MSG_HW, "attn: %08x %08x %08x %08x\n",
	   attn.sig[0], attn.sig[1], attn.sig[2], attn.sig[3]);

	for (index = 0; index < MAX_DYNAMIC_ATTN_GRPS; index++) {
		if (deasserted & (1 << index)) {
			group_mask = bp->attn_group[index];

			DP(NETIF_MSG_HW, "group[%d]: %08x %08x %08x %08x\n",
			   index, group_mask.sig[0], group_mask.sig[1],
			   group_mask.sig[2], group_mask.sig[3]);

			bnx2x_attn_int_deasserted3(bp,
					attn.sig[3] & group_mask.sig[3]);
			bnx2x_attn_int_deasserted1(bp,
					attn.sig[1] & group_mask.sig[1]);
			bnx2x_attn_int_deasserted2(bp,
					attn.sig[2] & group_mask.sig[2]);
			bnx2x_attn_int_deasserted0(bp,
					attn.sig[0] & group_mask.sig[0]);

			if ((attn.sig[0] & group_mask.sig[0] &
						HW_PRTY_ASSERT_SET_0) ||
			    (attn.sig[1] & group_mask.sig[1] &
						HW_PRTY_ASSERT_SET_1) ||
			    (attn.sig[2] & group_mask.sig[2] &
						HW_PRTY_ASSERT_SET_2))
				BNX2X_ERR("FATAL HW block parity attention\n");
		}
	}

	bnx2x_release_alr(bp);

	reg_addr = (HC_REG_COMMAND_REG + port*32 + COMMAND_REG_ATTN_BITS_CLR);

	val = ~deasserted;
	DP(NETIF_MSG_HW, "about to mask 0x%08x at HC addr 0x%x\n",
	   val, reg_addr);
	REG_WR(bp, reg_addr, val);

	if (~bp->attn_state & deasserted)
		BNX2X_ERR("IGU ERROR\n");

	reg_addr = port ? MISC_REG_AEU_MASK_ATTN_FUNC_1 :
			  MISC_REG_AEU_MASK_ATTN_FUNC_0;

	bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_PORT0_ATT_MASK + port);
	aeu_mask = REG_RD(bp, reg_addr);

	DP(NETIF_MSG_HW, "aeu_mask %x  newly deasserted %x\n",
	   aeu_mask, deasserted);
	aeu_mask |= (deasserted & 0xff);
	DP(NETIF_MSG_HW, "new mask %x\n", aeu_mask);

	REG_WR(bp, reg_addr, aeu_mask);
	bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_PORT0_ATT_MASK + port);

	DP(NETIF_MSG_HW, "attn_state %x\n", bp->attn_state);
	bp->attn_state &= ~deasserted;
	DP(NETIF_MSG_HW, "new state %x\n", bp->attn_state);
}

static void bnx2x_attn_int(struct bnx2x *bp)
{
	/* read local copy of bits */
	u32 attn_bits = le32_to_cpu(bp->def_status_blk->atten_status_block.
								attn_bits);
	u32 attn_ack = le32_to_cpu(bp->def_status_blk->atten_status_block.
								attn_bits_ack);
	u32 attn_state = bp->attn_state;

	/* look for changed bits */
	u32 asserted   =  attn_bits & ~attn_ack & ~attn_state;
	u32 deasserted = ~attn_bits &  attn_ack &  attn_state;

	DP(NETIF_MSG_HW,
	   "attn_bits %x  attn_ack %x  asserted %x  deasserted %x\n",
	   attn_bits, attn_ack, asserted, deasserted);

	if (~(attn_bits ^ attn_ack) & (attn_bits ^ attn_state))
		BNX2X_ERR("BAD attention state\n");

	/* handle bits that were raised */
	if (asserted)
		bnx2x_attn_int_asserted(bp, asserted);

	if (deasserted)
		bnx2x_attn_int_deasserted(bp, deasserted);
}

#if defined(INIT_DELAYED_WORK_DEFERRABLE) || defined(INIT_WORK_NAR) || (VMWARE_ESX_DDK_VERSION >= 40000) /* BNX2X_UPSTREAM */
static void bnx2x_sp_task(struct work_struct *work)
{
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	struct bnx2x *bp = container_of(work, struct bnx2x, sp_task.work);
#else
	struct bnx2x *bp = container_of(work, struct bnx2x, sp_task);
#endif
#else
static void bnx2x_sp_task(void *data)
{
	struct bnx2x *bp = (struct bnx2x *)data;
#endif
	u16 status;

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	bp->sp_running = 1;
	smp_mb(); /* make sure close can see this */
#endif

	/* Return here if interrupt is disabled */
	if (unlikely(atomic_read(&bp->intr_sem) != 0)) {
		DP(NETIF_MSG_INTR, "called but intr_sem not 0, returning\n");
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
		return;
#else
		goto sp_task_exit;
#endif
	}

	status = bnx2x_update_dsb_idx(bp);
/*	if (status == 0)				     */
/*		BNX2X_ERR("spurious slowpath interrupt!\n"); */

	DP(NETIF_MSG_INTR, "got a slowpath interrupt (status 0x%x)\n", status);

	/* HW attentions */
	if (status & 0x1) {
		bnx2x_attn_int(bp);
		status &= ~0x1;
	}

	/* CStorm events: STAT_QUERY */
	if (status & 0x2) {
		DP(BNX2X_MSG_SP, "CStorm events: STAT_QUERY\n");
		status &= ~0x2;
	}

	if (unlikely(status))
		DP(NETIF_MSG_INTR, "got an unknown interrupt! (status 0x%x)\n",
		   status);

	bnx2x_ack_sb(bp, DEF_SB_ID, ATTENTION_ID, le16_to_cpu(bp->def_att_idx),
		     IGU_INT_NOP, 1);
	bnx2x_ack_sb(bp, DEF_SB_ID, USTORM_ID, le16_to_cpu(bp->def_u_idx),
		     IGU_INT_NOP, 1);
	bnx2x_ack_sb(bp, DEF_SB_ID, CSTORM_ID, le16_to_cpu(bp->def_c_idx),
		     IGU_INT_NOP, 1);
	bnx2x_ack_sb(bp, DEF_SB_ID, XSTORM_ID, le16_to_cpu(bp->def_x_idx),
		     IGU_INT_NOP, 1);
	bnx2x_ack_sb(bp, DEF_SB_ID, TSTORM_ID, le16_to_cpu(bp->def_t_idx),
		     IGU_INT_ENABLE, 1);

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
sp_task_exit:
	bp->sp_running = 0;
	smp_mb(); /* make sure close can see this */
#endif
}

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE < 0x020613) && (VMWARE_ESX_DDK_VERSION < 40000)
static irqreturn_t bnx2x_msix_sp_int(int irq, void *dev_instance,
				     struct pt_regs *regs)
#else /* BNX2X_UPSTREAM */
static irqreturn_t bnx2x_msix_sp_int(int irq, void *dev_instance)
#endif
{
	struct net_device *dev = dev_instance;
	struct bnx2x *bp = netdev_priv(dev);

	/* Return here if interrupt is disabled */
	if (unlikely(atomic_read(&bp->intr_sem) != 0)) {
		DP(NETIF_MSG_INTR, "called but intr_sem not 0, returning\n");
		return IRQ_HANDLED;
	}

	bnx2x_ack_sb(bp, DEF_SB_ID, TSTORM_ID, 0, IGU_INT_DISABLE, 0);

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return IRQ_HANDLED;
#endif

#ifdef BCM_CNIC
	{
		struct cnic_ops *c_ops;

		rcu_read_lock();
		c_ops = rcu_dereference(bp->cnic_ops);
		if (c_ops)
			c_ops->cnic_handler(bp->cnic_data, NULL);
		rcu_read_unlock();
	}
#endif
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	queue_delayed_work(bnx2x_wq, &bp->sp_task, 0);
#else
	queue_work(bnx2x_wq, &bp->sp_task);
#endif

	return IRQ_HANDLED;
}
#endif

/* end of slow path */

/* Statistics */

/****************************************************************************
* Macros
****************************************************************************/

/* sum[hi:lo] += add[hi:lo] */
#define ADD_64(s_hi, a_hi, s_lo, a_lo) \
	do { \
		s_lo += a_lo; \
		s_hi += a_hi + ((s_lo < a_lo) ? 1 : 0); \
	} while (0)

/* difference = minuend - subtrahend */
#define DIFF_64(d_hi, m_hi, s_hi, d_lo, m_lo, s_lo) \
	do { \
		if (m_lo < s_lo) { \
			/* underflow */ \
			d_hi = m_hi - s_hi; \
			if (d_hi > 0) { \
				/* we can 'loan' 1 */ \
				d_hi--; \
				d_lo = m_lo + (UINT_MAX - s_lo) + 1; \
			} else { \
				/* m_hi <= s_hi */ \
				d_hi = 0; \
				d_lo = 0; \
			} \
		} else { \
			/* m_lo >= s_lo */ \
			if (m_hi < s_hi) { \
				d_hi = 0; \
				d_lo = 0; \
			} else { \
				/* m_hi >= s_hi */ \
				d_hi = m_hi - s_hi; \
				d_lo = m_lo - s_lo; \
			} \
		} \
	} while (0)

#define UPDATE_STAT64(s, t) \
	do { \
		DIFF_64(diff.hi, new->s##_hi, pstats->mac_stx[0].t##_hi, \
			diff.lo, new->s##_lo, pstats->mac_stx[0].t##_lo); \
		pstats->mac_stx[0].t##_hi = new->s##_hi; \
		pstats->mac_stx[0].t##_lo = new->s##_lo; \
		ADD_64(pstats->mac_stx[1].t##_hi, diff.hi, \
		       pstats->mac_stx[1].t##_lo, diff.lo); \
	} while (0)

#define UPDATE_STAT64_NIG(s, t) \
	do { \
		DIFF_64(diff.hi, new->s##_hi, old->s##_hi, \
			diff.lo, new->s##_lo, old->s##_lo); \
		ADD_64(estats->t##_hi, diff.hi, \
		       estats->t##_lo, diff.lo); \
	} while (0)

/* sum[hi:lo] += add */
#define ADD_EXTEND_64(s_hi, s_lo, a) \
	do { \
		s_lo += a; \
		s_hi += (s_lo < a) ? 1 : 0; \
	} while (0)

#define UPDATE_EXTEND_STAT(s) \
	do { \
		ADD_EXTEND_64(pstats->mac_stx[1].s##_hi, \
			      pstats->mac_stx[1].s##_lo, \
			      new->s); \
	} while (0)

#define UPDATE_EXTEND_TSTAT(s, t) \
	do { \
		diff = le32_to_cpu(tclient->s) - le32_to_cpu(old_tclient->s); \
		old_tclient->s = tclient->s; \
		ADD_EXTEND_64(qstats->t##_hi, qstats->t##_lo, diff); \
	} while (0)

#define UPDATE_EXTEND_USTAT(s, t) \
	do { \
		diff = le32_to_cpu(uclient->s) - le32_to_cpu(old_uclient->s); \
		old_uclient->s = uclient->s; \
		ADD_EXTEND_64(qstats->t##_hi, qstats->t##_lo, diff); \
	} while (0)

#define UPDATE_EXTEND_XSTAT(s, t) \
	do { \
		diff = le32_to_cpu(xclient->s) - le32_to_cpu(old_xclient->s); \
		old_xclient->s = xclient->s; \
		ADD_EXTEND_64(qstats->t##_hi, qstats->t##_lo, diff); \
	} while (0)

/* minuend -= subtrahend */
#define SUB_64(m_hi, s_hi, m_lo, s_lo) \
	do { \
		DIFF_64(m_hi, m_hi, s_hi, m_lo, m_lo, s_lo); \
	} while (0)

/* minuend[hi:lo] -= subtrahend */
#define SUB_EXTEND_64(m_hi, m_lo, s) \
	do { \
		SUB_64(m_hi, 0, m_lo, s); \
	} while (0)

#define SUB_EXTEND_USTAT(s, t) \
	do { \
		diff = le32_to_cpu(uclient->s) - le32_to_cpu(old_uclient->s); \
		SUB_EXTEND_64(qstats->t##_hi, qstats->t##_lo, diff); \
	} while (0)

/*
 * General service functions
 */

static inline long bnx2x_hilo(u32 *hiref)
{
	u32 lo = *(hiref + 1);
#if (BITS_PER_LONG == 64)
	u32 hi = *hiref;

	return HILO_U64(hi, lo);
#else
	return lo;
#endif
}

/*
 * Init service functions
 */

static void bnx2x_storm_stats_post(struct bnx2x *bp)
{
	if (!bp->stats_pending) {
		struct eth_query_ramrod_data ramrod_data = {0};
		int i, rc;

		ramrod_data.drv_counter = bp->stats_counter++;
		ramrod_data.collect_port = bp->port.pmf ? 1 : 0;
		for_each_queue(bp, i)
			ramrod_data.ctr_id_vector |= (1 << bp->fp[i].cl_id);

		rc = bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_STAT_QUERY, 0,
				   ((u32 *)&ramrod_data)[1],
				   ((u32 *)&ramrod_data)[0], 0);
		if (rc == 0) {
			/* stats ramrod has it's own slot on the spq */
			bp->spq_left++;
			bp->stats_pending = 1;
		}
	}
}

static void bnx2x_hw_stats_post(struct bnx2x *bp)
{
	struct dmae_command *dmae = &bp->stats_dmae;
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	*stats_comp = DMAE_COMP_VAL;
	if (CHIP_REV_IS_SLOW(bp))
		return;

	/* loader */
	if (bp->executer_idx) {
		int loader_idx = PMF_DMAE_C(bp);

		memset(dmae, 0, sizeof(struct dmae_command));

		dmae->opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
				DMAE_CMD_C_DST_GRC | DMAE_CMD_C_ENABLE |
				DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
				DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
				DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
				(BP_PORT(bp) ? DMAE_CMD_PORT_1 :
					       DMAE_CMD_PORT_0) |
				(BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
		dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, dmae[0]));
		dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, dmae[0]));
		dmae->dst_addr_lo = (DMAE_REG_CMD_MEM +
				     sizeof(struct dmae_command) *
				     (loader_idx + 1)) >> 2;
		dmae->dst_addr_hi = 0;
		dmae->len = sizeof(struct dmae_command) >> 2;
		if (CHIP_IS_E1(bp))
			dmae->len--;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx + 1] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;

		*stats_comp = 0;
		bnx2x_post_dmae(bp, dmae, loader_idx);

	} else if (bp->func_stx) {
		*stats_comp = 0;
		bnx2x_post_dmae(bp, dmae, INIT_DMAE_C(bp));
	}
}

static int bnx2x_stats_comp(struct bnx2x *bp)
{
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);
	int cnt = 10;

	might_sleep();
	while (*stats_comp != DMAE_COMP_VAL) {
		if (!cnt) {
			BNX2X_ERR("timeout waiting for stats finished\n");
			break;
		}
		cnt--;
		msleep(1);
	}
	return 1;
}

/*
 * Statistics service functions
 */

static void bnx2x_stats_pmf_update(struct bnx2x *bp)
{
	struct dmae_command *dmae;
	u32 opcode;
	int loader_idx = PMF_DMAE_C(bp);
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	/* sanity */
	if (!IS_E1HMF(bp) || !bp->port.pmf || !bp->port.port_stx) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	bp->executer_idx = 0;

	opcode = (DMAE_CMD_SRC_GRC | DMAE_CMD_DST_PCI |
		  DMAE_CMD_C_ENABLE |
		  DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		  DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		  DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		  (BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		  (BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));

	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = (opcode | DMAE_CMD_C_DST_GRC);
	dmae->src_addr_lo = bp->port.port_stx >> 2;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, port_stats));
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, port_stats));
	dmae->len = DMAE_LEN32_RD_MAX;
	dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
	dmae->comp_addr_hi = 0;
	dmae->comp_val = 1;

	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = (opcode | DMAE_CMD_C_DST_PCI);
	dmae->src_addr_lo = (bp->port.port_stx >> 2) + DMAE_LEN32_RD_MAX;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, port_stats) +
				   DMAE_LEN32_RD_MAX * 4);
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, port_stats) +
				   DMAE_LEN32_RD_MAX * 4);
	dmae->len = (sizeof(struct host_port_stats) >> 2) - DMAE_LEN32_RD_MAX;
	dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_val = DMAE_COMP_VAL;

	*stats_comp = 0;
	bnx2x_hw_stats_post(bp);
	bnx2x_stats_comp(bp);
}

static void bnx2x_port_stats_init(struct bnx2x *bp)
{
	struct dmae_command *dmae;
	int port = BP_PORT(bp);
	int vn = BP_E1HVN(bp);
	u32 opcode;
	int loader_idx = PMF_DMAE_C(bp);
	u32 mac_addr;
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	/* sanity */
	if (!bp->link_vars.link_up || !bp->port.pmf) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	bp->executer_idx = 0;

	/* MCP */
	opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
		  DMAE_CMD_C_DST_GRC | DMAE_CMD_C_ENABLE |
		  DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		  DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		  DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		  (port ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		  (vn << DMAE_CMD_E1HVN_SHIFT));

	if (bp->port.port_stx) {

		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, port_stats));
		dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, port_stats));
		dmae->dst_addr_lo = bp->port.port_stx >> 2;
		dmae->dst_addr_hi = 0;
		dmae->len = sizeof(struct host_port_stats) >> 2;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;
	}

	if (bp->func_stx) {

		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, func_stats));
		dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, func_stats));
		dmae->dst_addr_lo = bp->func_stx >> 2;
		dmae->dst_addr_hi = 0;
		dmae->len = sizeof(struct host_func_stats) >> 2;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;
	}

	/* MAC */
	opcode = (DMAE_CMD_SRC_GRC | DMAE_CMD_DST_PCI |
		  DMAE_CMD_C_DST_GRC | DMAE_CMD_C_ENABLE |
		  DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		  DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		  DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		  (port ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		  (vn << DMAE_CMD_E1HVN_SHIFT));

	if (bp->link_vars.mac_type == MAC_TYPE_BMAC) {

		mac_addr = (port ? NIG_REG_INGRESS_BMAC1_MEM :
				   NIG_REG_INGRESS_BMAC0_MEM);

		/* BIGMAC_REGISTER_TX_STAT_GTPKT ..
		   BIGMAC_REGISTER_TX_STAT_GTBYT */
		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = (mac_addr +
				     BIGMAC_REGISTER_TX_STAT_GTPKT) >> 2;
		dmae->src_addr_hi = 0;
		dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, mac_stats));
		dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, mac_stats));
		dmae->len = (8 + BIGMAC_REGISTER_TX_STAT_GTBYT -
			     BIGMAC_REGISTER_TX_STAT_GTPKT) >> 2;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;

		/* BIGMAC_REGISTER_RX_STAT_GR64 ..
		   BIGMAC_REGISTER_RX_STAT_GRIPJ */
		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = (mac_addr +
				     BIGMAC_REGISTER_RX_STAT_GR64) >> 2;
		dmae->src_addr_hi = 0;
		dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, mac_stats) +
				offsetof(struct bmac_stats, rx_stat_gr64_lo));
		dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, mac_stats) +
				offsetof(struct bmac_stats, rx_stat_gr64_lo));
		dmae->len = (8 + BIGMAC_REGISTER_RX_STAT_GRIPJ -
			     BIGMAC_REGISTER_RX_STAT_GR64) >> 2;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;

	} else if (bp->link_vars.mac_type == MAC_TYPE_EMAC) {

		mac_addr = (port ? GRCBASE_EMAC1 : GRCBASE_EMAC0);

		/* EMAC_REG_EMAC_RX_STAT_AC (EMAC_REG_EMAC_RX_STAT_AC_COUNT)*/
		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = (mac_addr +
				     EMAC_REG_EMAC_RX_STAT_AC) >> 2;
		dmae->src_addr_hi = 0;
		dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, mac_stats));
		dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, mac_stats));
		dmae->len = EMAC_REG_EMAC_RX_STAT_AC_COUNT;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;

		/* EMAC_REG_EMAC_RX_STAT_AC_28 */
		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = (mac_addr +
				     EMAC_REG_EMAC_RX_STAT_AC_28) >> 2;
		dmae->src_addr_hi = 0;
		dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, mac_stats) +
		     offsetof(struct emac_stats, rx_stat_falsecarriererrors));
		dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, mac_stats) +
		     offsetof(struct emac_stats, rx_stat_falsecarriererrors));
		dmae->len = 1;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;

		/* EMAC_REG_EMAC_TX_STAT_AC (EMAC_REG_EMAC_TX_STAT_AC_COUNT)*/
		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = opcode;
		dmae->src_addr_lo = (mac_addr +
				     EMAC_REG_EMAC_TX_STAT_AC) >> 2;
		dmae->src_addr_hi = 0;
		dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, mac_stats) +
			offsetof(struct emac_stats, tx_stat_ifhcoutoctets));
		dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, mac_stats) +
			offsetof(struct emac_stats, tx_stat_ifhcoutoctets));
		dmae->len = EMAC_REG_EMAC_TX_STAT_AC_COUNT;
		dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
		dmae->comp_addr_hi = 0;
		dmae->comp_val = 1;
	}

	/* NIG */
	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = opcode;
	dmae->src_addr_lo = (port ? NIG_REG_STAT1_BRB_DISCARD :
				    NIG_REG_STAT0_BRB_DISCARD) >> 2;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, nig_stats));
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, nig_stats));
	dmae->len = (sizeof(struct nig_stats) - 4*sizeof(u32)) >> 2;
	dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
	dmae->comp_addr_hi = 0;
	dmae->comp_val = 1;

	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = opcode;
	dmae->src_addr_lo = (port ? NIG_REG_STAT1_EGRESS_MAC_PKT0 :
				    NIG_REG_STAT0_EGRESS_MAC_PKT0) >> 2;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, nig_stats) +
			offsetof(struct nig_stats, egress_mac_pkt0_lo));
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, nig_stats) +
			offsetof(struct nig_stats, egress_mac_pkt0_lo));
	dmae->len = (2*sizeof(u32)) >> 2;
	dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
	dmae->comp_addr_hi = 0;
	dmae->comp_val = 1;

	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = (DMAE_CMD_SRC_GRC | DMAE_CMD_DST_PCI |
			DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
			DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
			DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
			DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
			(port ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
			(vn << DMAE_CMD_E1HVN_SHIFT));
	dmae->src_addr_lo = (port ? NIG_REG_STAT1_EGRESS_MAC_PKT1 :
				    NIG_REG_STAT0_EGRESS_MAC_PKT1) >> 2;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, nig_stats) +
			offsetof(struct nig_stats, egress_mac_pkt1_lo));
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, nig_stats) +
			offsetof(struct nig_stats, egress_mac_pkt1_lo));
	dmae->len = (2*sizeof(u32)) >> 2;
	dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_val = DMAE_COMP_VAL;

	*stats_comp = 0;
}

static void bnx2x_func_stats_init(struct bnx2x *bp)
{
	struct dmae_command *dmae = &bp->stats_dmae;
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	/* sanity */
	if (!bp->func_stx) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	bp->executer_idx = 0;
	memset(dmae, 0, sizeof(struct dmae_command));

	dmae->opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
			DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
			DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
			DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
			DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
			(BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
			(BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
	dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, func_stats));
	dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, func_stats));
	dmae->dst_addr_lo = bp->func_stx >> 2;
	dmae->dst_addr_hi = 0;
	dmae->len = sizeof(struct host_func_stats) >> 2;
	dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_val = DMAE_COMP_VAL;

	*stats_comp = 0;
}

static void bnx2x_stats_start(struct bnx2x *bp)
{
	if (bp->port.pmf)
		bnx2x_port_stats_init(bp);

	else if (bp->func_stx)
		bnx2x_func_stats_init(bp);

	bnx2x_hw_stats_post(bp);
	bnx2x_storm_stats_post(bp);
}

static void bnx2x_stats_pmf_start(struct bnx2x *bp)
{
	bnx2x_stats_comp(bp);
	bnx2x_stats_pmf_update(bp);
	bnx2x_stats_start(bp);
}

static void bnx2x_stats_restart(struct bnx2x *bp)
{
	bnx2x_stats_comp(bp);
	bnx2x_stats_start(bp);
}

static void bnx2x_bmac_stats_update(struct bnx2x *bp)
{
	struct bmac_stats *new = bnx2x_sp(bp, mac_stats.bmac_stats);
	struct host_port_stats *pstats = bnx2x_sp(bp, port_stats);
	struct bnx2x_eth_stats *estats = &bp->eth_stats;
	struct {
		u32 lo;
		u32 hi;
	} diff;

	UPDATE_STAT64(rx_stat_grerb, rx_stat_ifhcinbadoctets);
	UPDATE_STAT64(rx_stat_grfcs, rx_stat_dot3statsfcserrors);
	UPDATE_STAT64(rx_stat_grund, rx_stat_etherstatsundersizepkts);
	UPDATE_STAT64(rx_stat_grovr, rx_stat_dot3statsframestoolong);
	UPDATE_STAT64(rx_stat_grfrg, rx_stat_etherstatsfragments);
	UPDATE_STAT64(rx_stat_grjbr, rx_stat_etherstatsjabbers);
	UPDATE_STAT64(rx_stat_grxcf, rx_stat_maccontrolframesreceived);
	UPDATE_STAT64(rx_stat_grxpf, rx_stat_xoffstateentered);
	UPDATE_STAT64(rx_stat_grxpf, rx_stat_bmac_xpf);
	UPDATE_STAT64(tx_stat_gtxpf, tx_stat_outxoffsent);
	UPDATE_STAT64(tx_stat_gtxpf, tx_stat_flowcontroldone);
	UPDATE_STAT64(tx_stat_gt64, tx_stat_etherstatspkts64octets);
	UPDATE_STAT64(tx_stat_gt127,
				tx_stat_etherstatspkts65octetsto127octets);
	UPDATE_STAT64(tx_stat_gt255,
				tx_stat_etherstatspkts128octetsto255octets);
	UPDATE_STAT64(tx_stat_gt511,
				tx_stat_etherstatspkts256octetsto511octets);
	UPDATE_STAT64(tx_stat_gt1023,
				tx_stat_etherstatspkts512octetsto1023octets);
	UPDATE_STAT64(tx_stat_gt1518,
				tx_stat_etherstatspkts1024octetsto1522octets);
	UPDATE_STAT64(tx_stat_gt2047, tx_stat_bmac_2047);
	UPDATE_STAT64(tx_stat_gt4095, tx_stat_bmac_4095);
	UPDATE_STAT64(tx_stat_gt9216, tx_stat_bmac_9216);
	UPDATE_STAT64(tx_stat_gt16383, tx_stat_bmac_16383);
	UPDATE_STAT64(tx_stat_gterr,
				tx_stat_dot3statsinternalmactransmiterrors);
	UPDATE_STAT64(tx_stat_gtufl, tx_stat_bmac_ufl);

	estats->pause_frames_received_hi =
				pstats->mac_stx[1].rx_stat_bmac_xpf_hi;
	estats->pause_frames_received_lo =
				pstats->mac_stx[1].rx_stat_bmac_xpf_lo;

	estats->pause_frames_sent_hi =
				pstats->mac_stx[1].tx_stat_outxoffsent_hi;
	estats->pause_frames_sent_lo =
				pstats->mac_stx[1].tx_stat_outxoffsent_lo;
}

static void bnx2x_emac_stats_update(struct bnx2x *bp)
{
	struct emac_stats *new = bnx2x_sp(bp, mac_stats.emac_stats);
	struct host_port_stats *pstats = bnx2x_sp(bp, port_stats);
	struct bnx2x_eth_stats *estats = &bp->eth_stats;

	UPDATE_EXTEND_STAT(rx_stat_ifhcinbadoctets);
	UPDATE_EXTEND_STAT(tx_stat_ifhcoutbadoctets);
	UPDATE_EXTEND_STAT(rx_stat_dot3statsfcserrors);
	UPDATE_EXTEND_STAT(rx_stat_dot3statsalignmenterrors);
	UPDATE_EXTEND_STAT(rx_stat_dot3statscarriersenseerrors);
	UPDATE_EXTEND_STAT(rx_stat_falsecarriererrors);
	UPDATE_EXTEND_STAT(rx_stat_etherstatsundersizepkts);
	UPDATE_EXTEND_STAT(rx_stat_dot3statsframestoolong);
	UPDATE_EXTEND_STAT(rx_stat_etherstatsfragments);
	UPDATE_EXTEND_STAT(rx_stat_etherstatsjabbers);
	UPDATE_EXTEND_STAT(rx_stat_maccontrolframesreceived);
	UPDATE_EXTEND_STAT(rx_stat_xoffstateentered);
	UPDATE_EXTEND_STAT(rx_stat_xonpauseframesreceived);
	UPDATE_EXTEND_STAT(rx_stat_xoffpauseframesreceived);
	UPDATE_EXTEND_STAT(tx_stat_outxonsent);
	UPDATE_EXTEND_STAT(tx_stat_outxoffsent);
	UPDATE_EXTEND_STAT(tx_stat_flowcontroldone);
	UPDATE_EXTEND_STAT(tx_stat_etherstatscollisions);
	UPDATE_EXTEND_STAT(tx_stat_dot3statssinglecollisionframes);
	UPDATE_EXTEND_STAT(tx_stat_dot3statsmultiplecollisionframes);
	UPDATE_EXTEND_STAT(tx_stat_dot3statsdeferredtransmissions);
	UPDATE_EXTEND_STAT(tx_stat_dot3statsexcessivecollisions);
	UPDATE_EXTEND_STAT(tx_stat_dot3statslatecollisions);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts64octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts65octetsto127octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts128octetsto255octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts256octetsto511octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts512octetsto1023octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspkts1024octetsto1522octets);
	UPDATE_EXTEND_STAT(tx_stat_etherstatspktsover1522octets);
	UPDATE_EXTEND_STAT(tx_stat_dot3statsinternalmactransmiterrors);

	estats->pause_frames_received_hi =
			pstats->mac_stx[1].rx_stat_xonpauseframesreceived_hi;
	estats->pause_frames_received_lo =
			pstats->mac_stx[1].rx_stat_xonpauseframesreceived_lo;
	ADD_64(estats->pause_frames_received_hi,
	       pstats->mac_stx[1].rx_stat_xoffpauseframesreceived_hi,
	       estats->pause_frames_received_lo,
	       pstats->mac_stx[1].rx_stat_xoffpauseframesreceived_lo);

	estats->pause_frames_sent_hi =
			pstats->mac_stx[1].tx_stat_outxonsent_hi;
	estats->pause_frames_sent_lo =
			pstats->mac_stx[1].tx_stat_outxonsent_lo;
	ADD_64(estats->pause_frames_sent_hi,
	       pstats->mac_stx[1].tx_stat_outxoffsent_hi,
	       estats->pause_frames_sent_lo,
	       pstats->mac_stx[1].tx_stat_outxoffsent_lo);
}

static int bnx2x_hw_stats_update(struct bnx2x *bp)
{
	struct nig_stats *new = bnx2x_sp(bp, nig_stats);
	struct nig_stats *old = &(bp->port.old_nig_stats);
	struct host_port_stats *pstats = bnx2x_sp(bp, port_stats);
	struct bnx2x_eth_stats *estats = &bp->eth_stats;
	struct {
		u32 lo;
		u32 hi;
	} diff;

	if (bp->link_vars.mac_type == MAC_TYPE_BMAC)
		bnx2x_bmac_stats_update(bp);

	else if (bp->link_vars.mac_type == MAC_TYPE_EMAC)
		bnx2x_emac_stats_update(bp);

	else { /* unreached */
		BNX2X_ERR("stats updated by DMAE but no MAC active\n");
		return -1;
	}

	ADD_EXTEND_64(pstats->brb_drop_hi, pstats->brb_drop_lo,
		      new->brb_discard - old->brb_discard);
	ADD_EXTEND_64(estats->brb_truncate_hi, estats->brb_truncate_lo,
		      new->brb_truncate - old->brb_truncate);

	UPDATE_STAT64_NIG(egress_mac_pkt0,
					etherstatspkts1024octetsto1522octets);
	UPDATE_STAT64_NIG(egress_mac_pkt1, etherstatspktsover1522octets);

	memcpy(old, new, sizeof(struct nig_stats));

	memcpy(&(estats->rx_stat_ifhcinbadoctets_hi), &(pstats->mac_stx[1]),
	       sizeof(struct mac_stx));
	estats->brb_drop_hi = pstats->brb_drop_hi;
	estats->brb_drop_lo = pstats->brb_drop_lo;

	pstats->host_port_stats_start = ++pstats->host_port_stats_end;

	if (!BP_NOMCP(bp)) {
		u32 nig_timer_max =
			SHMEM_RD(bp, port_mb[BP_PORT(bp)].stat_nig_timer);
		if (nig_timer_max != estats->nig_timer_max) {
			estats->nig_timer_max = nig_timer_max;
			BNX2X_ERR("NIG timer max (%u)\n",
				  estats->nig_timer_max);
		}
	}

	return 0;
}

static int bnx2x_storm_stats_update(struct bnx2x *bp)
{
	struct eth_stats_query *stats = bnx2x_sp(bp, fw_stats);
	struct tstorm_per_port_stats *tport =
					&stats->tstorm_common.port_statistics;
	struct host_func_stats *fstats = bnx2x_sp(bp, func_stats);
	struct bnx2x_eth_stats *estats = &bp->eth_stats;
	int i;

	memcpy(&(fstats->total_bytes_received_hi),
	       &(bnx2x_sp(bp, func_stats_base)->total_bytes_received_hi),
	       sizeof(struct host_func_stats) - 2*sizeof(u32));
	estats->error_bytes_received_hi = 0;
	estats->error_bytes_received_lo = 0;
	estats->etherstatsoverrsizepkts_hi = 0;
	estats->etherstatsoverrsizepkts_lo = 0;
	estats->no_buff_discard_hi = 0;
	estats->no_buff_discard_lo = 0;

	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];
		int cl_id = fp->cl_id;
		struct tstorm_per_client_stats *tclient =
				&stats->tstorm_common.client_statistics[cl_id];
		struct tstorm_per_client_stats *old_tclient = &fp->old_tclient;
		struct ustorm_per_client_stats *uclient =
				&stats->ustorm_common.client_statistics[cl_id];
		struct ustorm_per_client_stats *old_uclient = &fp->old_uclient;
		struct xstorm_per_client_stats *xclient =
				&stats->xstorm_common.client_statistics[cl_id];
		struct xstorm_per_client_stats *old_xclient = &fp->old_xclient;
		struct bnx2x_eth_q_stats *qstats = &fp->eth_q_stats;
		u32 diff;

		/* are storm stats valid? */
		if ((u16)(le16_to_cpu(xclient->stats_counter) + 1) !=
							bp->stats_counter) {
			DP(BNX2X_MSG_STATS, "[%d] stats not updated by xstorm"
			   "  xstorm counter (0x%x) != stats_counter (0x%x)\n",
			   i, xclient->stats_counter, bp->stats_counter);
			return -1;
		}
		if ((u16)(le16_to_cpu(tclient->stats_counter) + 1) !=
							bp->stats_counter) {
			DP(BNX2X_MSG_STATS, "[%d] stats not updated by tstorm"
			   "  tstorm counter (0x%x) != stats_counter (0x%x)\n",
			   i, tclient->stats_counter, bp->stats_counter);
			return -2;
		}
		if ((u16)(le16_to_cpu(uclient->stats_counter) + 1) !=
							bp->stats_counter) {
			DP(BNX2X_MSG_STATS, "[%d] stats not updated by ustorm"
			   "  ustorm counter (0x%x) != stats_counter (0x%x)\n",
			   i, uclient->stats_counter, bp->stats_counter);
			return -4;
		}

		qstats->total_bytes_received_hi =
			le32_to_cpu(tclient->rcv_broadcast_bytes.hi);
		qstats->total_bytes_received_lo =
			le32_to_cpu(tclient->rcv_broadcast_bytes.lo);

		ADD_64(qstats->total_bytes_received_hi,
		       le32_to_cpu(tclient->rcv_multicast_bytes.hi),
		       qstats->total_bytes_received_lo,
		       le32_to_cpu(tclient->rcv_multicast_bytes.lo));

		ADD_64(qstats->total_bytes_received_hi,
		       le32_to_cpu(tclient->rcv_unicast_bytes.hi),
		       qstats->total_bytes_received_lo,
		       le32_to_cpu(tclient->rcv_unicast_bytes.lo));

		SUB_64(qstats->total_bytes_received_hi,
		       le32_to_cpu(uclient->bcast_no_buff_bytes.hi),
		       qstats->total_bytes_received_lo,
		       le32_to_cpu(uclient->bcast_no_buff_bytes.lo));

		SUB_64(qstats->total_bytes_received_hi,
		       le32_to_cpu(uclient->mcast_no_buff_bytes.hi),
		       qstats->total_bytes_received_lo,
		       le32_to_cpu(uclient->mcast_no_buff_bytes.lo));

		SUB_64(qstats->total_bytes_received_hi,
		       le32_to_cpu(uclient->ucast_no_buff_bytes.hi),
		       qstats->total_bytes_received_lo,
		       le32_to_cpu(uclient->ucast_no_buff_bytes.lo));

		qstats->valid_bytes_received_hi =
					qstats->total_bytes_received_hi;
		qstats->valid_bytes_received_lo =
					qstats->total_bytes_received_lo;

		qstats->error_bytes_received_hi =
				le32_to_cpu(tclient->rcv_error_bytes.hi);
		qstats->error_bytes_received_lo =
				le32_to_cpu(tclient->rcv_error_bytes.lo);

		ADD_64(qstats->total_bytes_received_hi,
		       qstats->error_bytes_received_hi,
		       qstats->total_bytes_received_lo,
		       qstats->error_bytes_received_lo);

		UPDATE_EXTEND_TSTAT(rcv_unicast_pkts,
					total_unicast_packets_received);
		UPDATE_EXTEND_TSTAT(rcv_multicast_pkts,
					total_multicast_packets_received);
		UPDATE_EXTEND_TSTAT(rcv_broadcast_pkts,
					total_broadcast_packets_received);
		UPDATE_EXTEND_TSTAT(packets_too_big_discard,
					etherstatsoverrsizepkts);
		UPDATE_EXTEND_TSTAT(no_buff_discard, no_buff_discard);

		SUB_EXTEND_USTAT(ucast_no_buff_pkts,
					total_unicast_packets_received);
		SUB_EXTEND_USTAT(mcast_no_buff_pkts,
					total_multicast_packets_received);
		SUB_EXTEND_USTAT(bcast_no_buff_pkts,
					total_broadcast_packets_received);
		UPDATE_EXTEND_USTAT(ucast_no_buff_pkts, no_buff_discard);
		UPDATE_EXTEND_USTAT(mcast_no_buff_pkts, no_buff_discard);
		UPDATE_EXTEND_USTAT(bcast_no_buff_pkts, no_buff_discard);

		qstats->total_bytes_transmitted_hi =
				le32_to_cpu(xclient->unicast_bytes_sent.hi);
		qstats->total_bytes_transmitted_lo =
				le32_to_cpu(xclient->unicast_bytes_sent.lo);

		ADD_64(qstats->total_bytes_transmitted_hi,
		       le32_to_cpu(xclient->multicast_bytes_sent.hi),
		       qstats->total_bytes_transmitted_lo,
		       le32_to_cpu(xclient->multicast_bytes_sent.lo));

		ADD_64(qstats->total_bytes_transmitted_hi,
		       le32_to_cpu(xclient->broadcast_bytes_sent.hi),
		       qstats->total_bytes_transmitted_lo,
		       le32_to_cpu(xclient->broadcast_bytes_sent.lo));

		UPDATE_EXTEND_XSTAT(unicast_pkts_sent,
					total_unicast_packets_transmitted);
		UPDATE_EXTEND_XSTAT(multicast_pkts_sent,
					total_multicast_packets_transmitted);
		UPDATE_EXTEND_XSTAT(broadcast_pkts_sent,
					total_broadcast_packets_transmitted);

		old_tclient->checksum_discard = tclient->checksum_discard;
		old_tclient->ttl0_discard = tclient->ttl0_discard;

		ADD_64(fstats->total_bytes_received_hi,
		       qstats->total_bytes_received_hi,
		       fstats->total_bytes_received_lo,
		       qstats->total_bytes_received_lo);
		ADD_64(fstats->total_bytes_transmitted_hi,
		       qstats->total_bytes_transmitted_hi,
		       fstats->total_bytes_transmitted_lo,
		       qstats->total_bytes_transmitted_lo);
		ADD_64(fstats->total_unicast_packets_received_hi,
		       qstats->total_unicast_packets_received_hi,
		       fstats->total_unicast_packets_received_lo,
		       qstats->total_unicast_packets_received_lo);
		ADD_64(fstats->total_multicast_packets_received_hi,
		       qstats->total_multicast_packets_received_hi,
		       fstats->total_multicast_packets_received_lo,
		       qstats->total_multicast_packets_received_lo);
		ADD_64(fstats->total_broadcast_packets_received_hi,
		       qstats->total_broadcast_packets_received_hi,
		       fstats->total_broadcast_packets_received_lo,
		       qstats->total_broadcast_packets_received_lo);
		ADD_64(fstats->total_unicast_packets_transmitted_hi,
		       qstats->total_unicast_packets_transmitted_hi,
		       fstats->total_unicast_packets_transmitted_lo,
		       qstats->total_unicast_packets_transmitted_lo);
		ADD_64(fstats->total_multicast_packets_transmitted_hi,
		       qstats->total_multicast_packets_transmitted_hi,
		       fstats->total_multicast_packets_transmitted_lo,
		       qstats->total_multicast_packets_transmitted_lo);
		ADD_64(fstats->total_broadcast_packets_transmitted_hi,
		       qstats->total_broadcast_packets_transmitted_hi,
		       fstats->total_broadcast_packets_transmitted_lo,
		       qstats->total_broadcast_packets_transmitted_lo);
		ADD_64(fstats->valid_bytes_received_hi,
		       qstats->valid_bytes_received_hi,
		       fstats->valid_bytes_received_lo,
		       qstats->valid_bytes_received_lo);

		ADD_64(estats->error_bytes_received_hi,
		       qstats->error_bytes_received_hi,
		       estats->error_bytes_received_lo,
		       qstats->error_bytes_received_lo);
		ADD_64(estats->etherstatsoverrsizepkts_hi,
		       qstats->etherstatsoverrsizepkts_hi,
		       estats->etherstatsoverrsizepkts_lo,
		       qstats->etherstatsoverrsizepkts_lo);
		ADD_64(estats->no_buff_discard_hi, qstats->no_buff_discard_hi,
		       estats->no_buff_discard_lo, qstats->no_buff_discard_lo);
	}

	ADD_64(fstats->total_bytes_received_hi,
	       estats->rx_stat_ifhcinbadoctets_hi,
	       fstats->total_bytes_received_lo,
	       estats->rx_stat_ifhcinbadoctets_lo);

	memcpy(estats, &(fstats->total_bytes_received_hi),
	       sizeof(struct host_func_stats) - 2*sizeof(u32));

	ADD_64(estats->etherstatsoverrsizepkts_hi,
	       estats->rx_stat_dot3statsframestoolong_hi,
	       estats->etherstatsoverrsizepkts_lo,
	       estats->rx_stat_dot3statsframestoolong_lo);
	ADD_64(estats->error_bytes_received_hi,
	       estats->rx_stat_ifhcinbadoctets_hi,
	       estats->error_bytes_received_lo,
	       estats->rx_stat_ifhcinbadoctets_lo);

	if (bp->port.pmf) {
		estats->mac_filter_discard =
				le32_to_cpu(tport->mac_filter_discard);
		estats->xxoverflow_discard =
				le32_to_cpu(tport->xxoverflow_discard);
		estats->brb_truncate_discard =
				le32_to_cpu(tport->brb_truncate_discard);
		estats->mac_discard = le32_to_cpu(tport->mac_discard);
	}

	fstats->host_func_stats_start = ++fstats->host_func_stats_end;

	bp->stats_pending = 0;

	return 0;
}

static void bnx2x_net_stats_update(struct bnx2x *bp)
{
	struct bnx2x_eth_stats *estats = &bp->eth_stats;
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
	struct net_device_stats *nstats = &bp->dev->stats;
#else
	struct net_device_stats *nstats = &bp->net_stats;
#endif
	int i;

	nstats->rx_packets =
		bnx2x_hilo(&estats->total_unicast_packets_received_hi) +
		bnx2x_hilo(&estats->total_multicast_packets_received_hi) +
		bnx2x_hilo(&estats->total_broadcast_packets_received_hi);

	nstats->tx_packets =
		bnx2x_hilo(&estats->total_unicast_packets_transmitted_hi) +
		bnx2x_hilo(&estats->total_multicast_packets_transmitted_hi) +
		bnx2x_hilo(&estats->total_broadcast_packets_transmitted_hi);

	nstats->rx_bytes = bnx2x_hilo(&estats->total_bytes_received_hi);

	nstats->tx_bytes = bnx2x_hilo(&estats->total_bytes_transmitted_hi);

	nstats->rx_dropped = estats->mac_discard;
	for_each_queue(bp, i)
		nstats->rx_dropped +=
			le32_to_cpu(bp->fp[i].old_tclient.checksum_discard);

	nstats->tx_dropped = 0;

	nstats->multicast =
		bnx2x_hilo(&estats->total_multicast_packets_received_hi);

	nstats->collisions =
		bnx2x_hilo(&estats->tx_stat_etherstatscollisions_hi);

	nstats->rx_length_errors =
		bnx2x_hilo(&estats->rx_stat_etherstatsundersizepkts_hi) +
		bnx2x_hilo(&estats->etherstatsoverrsizepkts_hi);
	nstats->rx_over_errors = bnx2x_hilo(&estats->brb_drop_hi) +
				 bnx2x_hilo(&estats->brb_truncate_hi);
	nstats->rx_crc_errors =
		bnx2x_hilo(&estats->rx_stat_dot3statsfcserrors_hi);
	nstats->rx_frame_errors =
		bnx2x_hilo(&estats->rx_stat_dot3statsalignmenterrors_hi);
	nstats->rx_fifo_errors = bnx2x_hilo(&estats->no_buff_discard_hi);
	nstats->rx_missed_errors = estats->xxoverflow_discard;

	nstats->rx_errors = nstats->rx_length_errors +
			    nstats->rx_over_errors +
			    nstats->rx_crc_errors +
			    nstats->rx_frame_errors +
			    nstats->rx_fifo_errors +
			    nstats->rx_missed_errors;

	nstats->tx_aborted_errors =
		bnx2x_hilo(&estats->tx_stat_dot3statslatecollisions_hi) +
		bnx2x_hilo(&estats->tx_stat_dot3statsexcessivecollisions_hi);
	nstats->tx_carrier_errors =
		bnx2x_hilo(&estats->rx_stat_dot3statscarriersenseerrors_hi);
	nstats->tx_fifo_errors = 0;
	nstats->tx_heartbeat_errors = 0;
	nstats->tx_window_errors = 0;

	nstats->tx_errors = nstats->tx_aborted_errors +
			    nstats->tx_carrier_errors +
	    bnx2x_hilo(&estats->tx_stat_dot3statsinternalmactransmiterrors_hi);
}

static void bnx2x_drv_stats_update(struct bnx2x *bp)
{
	struct bnx2x_eth_stats *estats = &bp->eth_stats;
	int i;

	estats->driver_xoff = 0;
	estats->rx_err_discard_pkt = 0;
	estats->rx_skb_alloc_failed = 0;
	estats->hw_csum_err = 0;
	for_each_queue(bp, i) {
		struct bnx2x_eth_q_stats *qstats = &bp->fp[i].eth_q_stats;

		estats->rx_err_discard_pkt += qstats->rx_err_discard_pkt;
		estats->rx_skb_alloc_failed += qstats->rx_skb_alloc_failed;
		estats->hw_csum_err += qstats->hw_csum_err;
	}
	for_each_queue(bp, i) {
		struct bnx2x_eth_q_stats *qstats = &bp->fp[i].eth_q_stats;

		estats->driver_xoff += qstats->driver_xoff;
	}
}

static void bnx2x_stats_update(struct bnx2x *bp)
{
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	if (*stats_comp != DMAE_COMP_VAL)
		return;

	if (bp->port.pmf)
		bnx2x_hw_stats_update(bp);

	if (bnx2x_storm_stats_update(bp) && (bp->stats_pending++ == 3)) {
		BNX2X_ERR("storm stats were not updated for 3 times\n");
		bnx2x_panic();
		return;
	}

	bnx2x_net_stats_update(bp);
	bnx2x_drv_stats_update(bp);

	if (bp->msglevel & NETIF_MSG_TIMER) {
		struct bnx2x_eth_stats *estats = &bp->eth_stats;
		int i;

		printk(KERN_DEBUG "%s: brb drops %u  brb truncate %u\n",
		       bp->dev->name,
		       estats->brb_drop_lo, estats->brb_truncate_lo);

		for_each_queue(bp, i) {
			struct bnx2x_fastpath *fp = &bp->fp[i];
			struct bnx2x_eth_q_stats *qstats = &fp->eth_q_stats;

			printk(KERN_DEBUG "%s: rx usage(%4u)  *rx_cons_sb(%u)"
					  "  rx pkt(%lu)  rx calls(%lu %lu)\n",
			       fp->name, (le16_to_cpu(*fp->rx_cons_sb) -
			       fp->rx_comp_cons),
			       le16_to_cpu(*fp->rx_cons_sb),
			       bnx2x_hilo(&qstats->
					  total_unicast_packets_received_hi),
			       fp->rx_calls, fp->rx_pkt);
		}

		for_each_queue(bp, i) {
			struct bnx2x_fastpath *fp = &bp->fp[i];
			struct bnx2x_eth_q_stats *qstats = &fp->eth_q_stats;
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
			struct netdev_queue *txq = netdev_get_tx_queue(bp->dev,	i);
#endif

			printk(KERN_DEBUG "%s: tx avail(%4u)  *tx_cons_sb(%u)"
					  "  tx pkt(%lu) tx calls (%lu)"
					  "  %s (Xoff events %u)\n",
			       fp->name, bnx2x_tx_avail(fp),
			       le16_to_cpu(*fp->tx_cons_sb),
			       bnx2x_hilo(&qstats->
					  total_unicast_packets_transmitted_hi),
			       fp->tx_pkt,
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
			       (netif_tx_queue_stopped(txq) ? "Xoff" : "Xon"),
#else
			       (netif_queue_stopped(bp->dev) ? "Xoff" : "Xon"),
#endif
			       qstats->driver_xoff);
		}
	}

	bnx2x_hw_stats_post(bp);
	bnx2x_storm_stats_post(bp);
}

static void bnx2x_port_stats_stop(struct bnx2x *bp)
{
	struct dmae_command *dmae;
	u32 opcode;
	int loader_idx = PMF_DMAE_C(bp);
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	bp->executer_idx = 0;

	opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
		  DMAE_CMD_C_ENABLE |
		  DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
		  DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
		  DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
		  (BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
		  (BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));

	if (bp->port.port_stx) {

		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		if (bp->func_stx)
			dmae->opcode = (opcode | DMAE_CMD_C_DST_GRC);
		else
			dmae->opcode = (opcode | DMAE_CMD_C_DST_PCI);
		dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, port_stats));
		dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, port_stats));
		dmae->dst_addr_lo = bp->port.port_stx >> 2;
		dmae->dst_addr_hi = 0;
		dmae->len = sizeof(struct host_port_stats) >> 2;
		if (bp->func_stx) {
			dmae->comp_addr_lo = dmae_reg_go_c[loader_idx] >> 2;
			dmae->comp_addr_hi = 0;
			dmae->comp_val = 1;
		} else {
			dmae->comp_addr_lo =
				U64_LO(bnx2x_sp_mapping(bp, stats_comp));
			dmae->comp_addr_hi =
				U64_HI(bnx2x_sp_mapping(bp, stats_comp));
			dmae->comp_val = DMAE_COMP_VAL;

			*stats_comp = 0;
		}
	}

	if (bp->func_stx) {

		dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
		dmae->opcode = (opcode | DMAE_CMD_C_DST_PCI);
		dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, func_stats));
		dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, func_stats));
		dmae->dst_addr_lo = bp->func_stx >> 2;
		dmae->dst_addr_hi = 0;
		dmae->len = sizeof(struct host_func_stats) >> 2;
		dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
		dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
		dmae->comp_val = DMAE_COMP_VAL;

		*stats_comp = 0;
	}
}

static void bnx2x_stats_stop(struct bnx2x *bp)
{
	int update = 0;

	bnx2x_stats_comp(bp);

	if (bp->port.pmf)
		update = (bnx2x_hw_stats_update(bp) == 0);

	update |= (bnx2x_storm_stats_update(bp) == 0);

	if (update) {
		bnx2x_net_stats_update(bp);

		if (bp->port.pmf)
			bnx2x_port_stats_stop(bp);

		bnx2x_hw_stats_post(bp);
		bnx2x_stats_comp(bp);
	}
}

static void bnx2x_stats_do_nothing(struct bnx2x *bp)
{
}

static const struct {
	void (*action)(struct bnx2x *bp);
	enum bnx2x_stats_state next_state;
} bnx2x_stats_stm[STATS_STATE_MAX][STATS_EVENT_MAX] = {
/* state	event	*/
{
/* DISABLED	PMF	*/ {bnx2x_stats_pmf_update, STATS_STATE_DISABLED},
/*		LINK_UP	*/ {bnx2x_stats_start,      STATS_STATE_ENABLED},
/*		UPDATE	*/ {bnx2x_stats_do_nothing, STATS_STATE_DISABLED},
/*		STOP	*/ {bnx2x_stats_do_nothing, STATS_STATE_DISABLED}
},
{
/* ENABLED	PMF	*/ {bnx2x_stats_pmf_start,  STATS_STATE_ENABLED},
/*		LINK_UP	*/ {bnx2x_stats_restart,    STATS_STATE_ENABLED},
/*		UPDATE	*/ {bnx2x_stats_update,     STATS_STATE_ENABLED},
/*		STOP	*/ {bnx2x_stats_stop,       STATS_STATE_DISABLED}
}
};

static void bnx2x_stats_handle(struct bnx2x *bp, enum bnx2x_stats_event event)
{
	enum bnx2x_stats_state state = bp->stats_state;

	if (unlikely(bp->panic))
		return;

	bnx2x_stats_stm[state][event].action(bp);
	bp->stats_state = bnx2x_stats_stm[state][event].next_state;

	/* Make sure the state has been "changed" */
	smp_wmb();

	if ((event != STATS_EVENT_UPDATE) || (bp->msglevel & NETIF_MSG_TIMER))
		DP(BNX2X_MSG_STATS, "state %d -> event %d -> state %d\n",
		   state, event, bp->stats_state);
}

static void bnx2x_port_stats_base_init(struct bnx2x *bp)
{
	struct dmae_command *dmae;
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	/* sanity */
	if (!bp->port.pmf || !bp->port.port_stx) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	bp->executer_idx = 0;

	dmae = bnx2x_sp(bp, dmae[bp->executer_idx++]);
	dmae->opcode = (DMAE_CMD_SRC_PCI | DMAE_CMD_DST_GRC |
			DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
			DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
			DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
			DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
			(BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
			(BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
	dmae->src_addr_lo = U64_LO(bnx2x_sp_mapping(bp, port_stats));
	dmae->src_addr_hi = U64_HI(bnx2x_sp_mapping(bp, port_stats));
	dmae->dst_addr_lo = bp->port.port_stx >> 2;
	dmae->dst_addr_hi = 0;
	dmae->len = sizeof(struct host_port_stats) >> 2;
	dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_val = DMAE_COMP_VAL;

	*stats_comp = 0;
	bnx2x_hw_stats_post(bp);
	bnx2x_stats_comp(bp);
}

static void bnx2x_func_stats_base_init(struct bnx2x *bp)
{
	int vn, vn_max = IS_E1HMF(bp) ? E1HVN_MAX : E1VN_MAX;
	int port = BP_PORT(bp);
	int func;
	u32 func_stx;

	/* sanity */
	if (!bp->port.pmf || !bp->func_stx) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	/* save our func_stx */
	func_stx = bp->func_stx;

	for (vn = VN_0; vn < vn_max; vn++) {
		func = 2*vn + port;

		bp->func_stx = SHMEM_RD(bp, func_mb[func].fw_mb_param);
		bnx2x_func_stats_init(bp);
		bnx2x_hw_stats_post(bp);
		bnx2x_stats_comp(bp);
	}

	/* restore our func_stx */
	bp->func_stx = func_stx;
}

static void bnx2x_func_stats_base_update(struct bnx2x *bp)
{
	struct dmae_command *dmae = &bp->stats_dmae;
	u32 *stats_comp = bnx2x_sp(bp, stats_comp);

	/* sanity */
	if (!bp->func_stx) {
		BNX2X_ERR("BUG!\n");
		return;
	}

	bp->executer_idx = 0;
	memset(dmae, 0, sizeof(struct dmae_command));

	dmae->opcode = (DMAE_CMD_SRC_GRC | DMAE_CMD_DST_PCI |
			DMAE_CMD_C_DST_PCI | DMAE_CMD_C_ENABLE |
			DMAE_CMD_SRC_RESET | DMAE_CMD_DST_RESET |
#ifdef __BIG_ENDIAN
			DMAE_CMD_ENDIANITY_B_DW_SWAP |
#else
			DMAE_CMD_ENDIANITY_DW_SWAP |
#endif
			(BP_PORT(bp) ? DMAE_CMD_PORT_1 : DMAE_CMD_PORT_0) |
			(BP_E1HVN(bp) << DMAE_CMD_E1HVN_SHIFT));
	dmae->src_addr_lo = bp->func_stx >> 2;
	dmae->src_addr_hi = 0;
	dmae->dst_addr_lo = U64_LO(bnx2x_sp_mapping(bp, func_stats_base));
	dmae->dst_addr_hi = U64_HI(bnx2x_sp_mapping(bp, func_stats_base));
	dmae->len = sizeof(struct host_func_stats) >> 2;
	dmae->comp_addr_lo = U64_LO(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_addr_hi = U64_HI(bnx2x_sp_mapping(bp, stats_comp));
	dmae->comp_val = DMAE_COMP_VAL;

	*stats_comp = 0;
	bnx2x_hw_stats_post(bp);
	bnx2x_stats_comp(bp);
}

static void bnx2x_stats_init(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	int i;

	bp->stats_pending = 0;
	bp->executer_idx = 0;
	bp->stats_counter = 0;

	/* port and func stats for management */
	if (!BP_NOMCP(bp)) {
		bp->port.port_stx = SHMEM_RD(bp, port_mb[port].port_stx);
		bp->func_stx = SHMEM_RD(bp, func_mb[func].fw_mb_param);

	} else {
		bp->port.port_stx = 0;
		bp->func_stx = 0;
	}
	DP(BNX2X_MSG_STATS, "port_stx 0x%x  func_stx 0x%x\n",
	   bp->port.port_stx, bp->func_stx);

	/* port stats */
	memset(&(bp->port.old_nig_stats), 0, sizeof(struct nig_stats));
	bp->port.old_nig_stats.brb_discard =
			REG_RD(bp, NIG_REG_STAT0_BRB_DISCARD + port*0x38);
	bp->port.old_nig_stats.brb_truncate =
			REG_RD(bp, NIG_REG_STAT0_BRB_TRUNCATE + port*0x38);
	REG_RD_DMAE(bp, NIG_REG_STAT0_EGRESS_MAC_PKT0 + port*0x50,
		    &(bp->port.old_nig_stats.egress_mac_pkt0_lo), 2);
	REG_RD_DMAE(bp, NIG_REG_STAT0_EGRESS_MAC_PKT1 + port*0x50,
		    &(bp->port.old_nig_stats.egress_mac_pkt1_lo), 2);

	/* function stats */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		memset(&fp->old_tclient, 0,
		       sizeof(struct tstorm_per_client_stats));
		memset(&fp->old_uclient, 0,
		       sizeof(struct ustorm_per_client_stats));
		memset(&fp->old_xclient, 0,
		       sizeof(struct xstorm_per_client_stats));
		memset(&fp->eth_q_stats, 0, sizeof(struct bnx2x_eth_q_stats));
	}

#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
	memset(&bp->dev->stats, 0, sizeof(struct net_device_stats));
#else
	memset(&bp->net_stats, 0, sizeof(struct net_device_stats));
#endif
	memset(&bp->eth_stats, 0, sizeof(struct bnx2x_eth_stats));

	bp->stats_state = STATS_STATE_DISABLED;

	if (bp->port.pmf) {
		if (bp->port.port_stx)
			bnx2x_port_stats_base_init(bp);

		if (bp->func_stx)
			bnx2x_func_stats_base_init(bp);

	} else if (bp->func_stx)
		bnx2x_func_stats_base_update(bp);
}

static void bnx2x_timer(unsigned long data)
{
	struct bnx2x *bp = (struct bnx2x *) data;

	if (!netif_running(bp->dev))
		return;

	if (atomic_read(&bp->intr_sem) != 0)
		goto timer_restart;

	if (poll) {
		struct bnx2x_fastpath *fp = &bp->fp[0];
		int rc;

		bnx2x_tx_int(fp);
		rc = bnx2x_rx_int(fp, 1000);
	}

	if (!BP_NOMCP(bp)) {
		int func = BP_FUNC(bp);
		u32 drv_pulse;
		u32 mcp_pulse;

		++bp->fw_drv_pulse_wr_seq;
		bp->fw_drv_pulse_wr_seq &= DRV_PULSE_SEQ_MASK;
		/* TBD - add SYSTEM_TIME */
		drv_pulse = bp->fw_drv_pulse_wr_seq;
		SHMEM_WR(bp, func_mb[func].drv_pulse_mb, drv_pulse);

		mcp_pulse = (SHMEM_RD(bp, func_mb[func].mcp_pulse_mb) &
			     MCP_PULSE_SEQ_MASK);
		/* The delta between driver pulse and mcp response
		 * should be 1 (before mcp response) or 0 (after mcp response)
		 */
		if ((drv_pulse != mcp_pulse) &&
		    (drv_pulse != ((mcp_pulse + 1) & MCP_PULSE_SEQ_MASK))) {
			/* someone lost a heartbeat... */
			BNX2X_ERR("drv_pulse (0x%x) != mcp_pulse (0x%x)\n",
				  drv_pulse, mcp_pulse);
		}
	}

	if (bp->state == BNX2X_STATE_OPEN)
		bnx2x_stats_handle(bp, STATS_EVENT_UPDATE);

timer_restart:
	mod_timer(&bp->timer, jiffies + bp->current_interval);
}

/* end of Statistics */

/* nic init */

/*
 * nic init service functions
 */

static void bnx2x_zero_sb(struct bnx2x *bp, int sb_id)
{
	int port = BP_PORT(bp);

	/* "CSTORM" */
	bnx2x_init_fill(bp, CSEM_REG_FAST_MEMORY +
			CSTORM_SB_HOST_STATUS_BLOCK_U_OFFSET(port, sb_id), 0,
			CSTORM_SB_STATUS_BLOCK_U_SIZE / 4);
	bnx2x_init_fill(bp, CSEM_REG_FAST_MEMORY +
			CSTORM_SB_HOST_STATUS_BLOCK_C_OFFSET(port, sb_id), 0,
			CSTORM_SB_STATUS_BLOCK_C_SIZE / 4);
}

static void bnx2x_init_sb(struct bnx2x *bp, struct host_status_block *sb,
			  dma_addr_t mapping, int sb_id)
{
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	int index;
	u64 section;

	/* USTORM */
	section = ((u64)mapping) + offsetof(struct host_status_block,
					    u_status_block);
	sb->u_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       CSTORM_SB_HOST_SB_ADDR_U_OFFSET(port, sb_id), U64_LO(section));
	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       ((CSTORM_SB_HOST_SB_ADDR_U_OFFSET(port, sb_id)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_CSTRORM_INTMEM + FP_USB_FUNC_OFF +
		CSTORM_SB_HOST_STATUS_BLOCK_U_OFFSET(port, sb_id), func);

	for (index = 0; index < HC_USTORM_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_SB_HC_DISABLE_U_OFFSET(port, sb_id, index), 1);

	/* CSTORM */
	section = ((u64)mapping) + offsetof(struct host_status_block,
					    c_status_block);
	sb->c_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       CSTORM_SB_HOST_SB_ADDR_C_OFFSET(port, sb_id), U64_LO(section));
	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       ((CSTORM_SB_HOST_SB_ADDR_C_OFFSET(port, sb_id)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_CSTRORM_INTMEM + FP_CSB_FUNC_OFF +
		CSTORM_SB_HOST_STATUS_BLOCK_C_OFFSET(port, sb_id), func);

	for (index = 0; index < HC_CSTORM_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_SB_HC_DISABLE_C_OFFSET(port, sb_id, index), 1);

	bnx2x_ack_sb(bp, sb_id, CSTORM_ID, 0, IGU_INT_ENABLE, 0);
}

static void bnx2x_zero_def_sb(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);

	bnx2x_init_fill(bp, TSEM_REG_FAST_MEMORY +
			TSTORM_DEF_SB_HOST_STATUS_BLOCK_OFFSET(func), 0,
			sizeof(struct tstorm_def_status_block)/4);
	bnx2x_init_fill(bp, CSEM_REG_FAST_MEMORY +
			CSTORM_DEF_SB_HOST_STATUS_BLOCK_U_OFFSET(func), 0,
			sizeof(struct cstorm_def_status_block_u)/4);
	bnx2x_init_fill(bp, CSEM_REG_FAST_MEMORY +
			CSTORM_DEF_SB_HOST_STATUS_BLOCK_C_OFFSET(func), 0,
			sizeof(struct cstorm_def_status_block_c)/4);
	bnx2x_init_fill(bp, XSEM_REG_FAST_MEMORY +
			XSTORM_DEF_SB_HOST_STATUS_BLOCK_OFFSET(func), 0,
			sizeof(struct xstorm_def_status_block)/4);
}

static void bnx2x_init_def_sb(struct bnx2x *bp,
			      struct host_def_status_block *def_sb,
			      dma_addr_t mapping, int sb_id)
{
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	int index, val, reg_offset;
	u64 section;

	/* ATTN */
	section = ((u64)mapping) + offsetof(struct host_def_status_block,
					    atten_status_block);
	def_sb->atten_status_block.status_block_id = sb_id;

	bp->attn_state = 0;

	reg_offset = (port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0 :
			     MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0);

	for (index = 0; index < MAX_DYNAMIC_ATTN_GRPS; index++) {
		bp->attn_group[index].sig[0] = REG_RD(bp,
						     reg_offset + 0x10*index);
		bp->attn_group[index].sig[1] = REG_RD(bp,
					       reg_offset + 0x4 + 0x10*index);
		bp->attn_group[index].sig[2] = REG_RD(bp,
					       reg_offset + 0x8 + 0x10*index);
		bp->attn_group[index].sig[3] = REG_RD(bp,
					       reg_offset + 0xc + 0x10*index);
	}

	reg_offset = (port ? HC_REG_ATTN_MSG1_ADDR_L :
			     HC_REG_ATTN_MSG0_ADDR_L);

	REG_WR(bp, reg_offset, U64_LO(section));
	REG_WR(bp, reg_offset + 4, U64_HI(section));

	reg_offset = (port ? HC_REG_ATTN_NUM_P1 : HC_REG_ATTN_NUM_P0);

	val = REG_RD(bp, reg_offset);
	val |= sb_id;
	REG_WR(bp, reg_offset, val);

	/* USTORM */
	section = ((u64)mapping) + offsetof(struct host_def_status_block,
					    u_def_status_block);
	def_sb->u_def_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       CSTORM_DEF_SB_HOST_SB_ADDR_U_OFFSET(func), U64_LO(section));
	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       ((CSTORM_DEF_SB_HOST_SB_ADDR_U_OFFSET(func)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_CSTRORM_INTMEM + DEF_USB_FUNC_OFF +
		CSTORM_DEF_SB_HOST_STATUS_BLOCK_U_OFFSET(func), func);

	for (index = 0; index < HC_USTORM_DEF_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_DEF_SB_HC_DISABLE_U_OFFSET(func, index), 1);

	/* CSTORM */
	section = ((u64)mapping) + offsetof(struct host_def_status_block,
					    c_def_status_block);
	def_sb->c_def_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       CSTORM_DEF_SB_HOST_SB_ADDR_C_OFFSET(func), U64_LO(section));
	REG_WR(bp, BAR_CSTRORM_INTMEM +
	       ((CSTORM_DEF_SB_HOST_SB_ADDR_C_OFFSET(func)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_CSTRORM_INTMEM + DEF_CSB_FUNC_OFF +
		CSTORM_DEF_SB_HOST_STATUS_BLOCK_C_OFFSET(func), func);

	for (index = 0; index < HC_CSTORM_DEF_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_DEF_SB_HC_DISABLE_C_OFFSET(func, index), 1);

	/* TSTORM */
	section = ((u64)mapping) + offsetof(struct host_def_status_block,
					    t_def_status_block);
	def_sb->t_def_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_TSTRORM_INTMEM +
	       TSTORM_DEF_SB_HOST_SB_ADDR_OFFSET(func), U64_LO(section));
	REG_WR(bp, BAR_TSTRORM_INTMEM +
	       ((TSTORM_DEF_SB_HOST_SB_ADDR_OFFSET(func)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_TSTRORM_INTMEM + DEF_TSB_FUNC_OFF +
		TSTORM_DEF_SB_HOST_STATUS_BLOCK_OFFSET(func), func);

	for (index = 0; index < HC_TSTORM_DEF_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_TSTRORM_INTMEM +
			 TSTORM_DEF_SB_HC_DISABLE_OFFSET(func, index), 1);

	/* XSTORM */
	section = ((u64)mapping) + offsetof(struct host_def_status_block,
					    x_def_status_block);
	def_sb->x_def_status_block.status_block_id = sb_id;

	REG_WR(bp, BAR_XSTRORM_INTMEM +
	       XSTORM_DEF_SB_HOST_SB_ADDR_OFFSET(func), U64_LO(section));
	REG_WR(bp, BAR_XSTRORM_INTMEM +
	       ((XSTORM_DEF_SB_HOST_SB_ADDR_OFFSET(func)) + 4),
	       U64_HI(section));
	REG_WR8(bp, BAR_XSTRORM_INTMEM + DEF_XSB_FUNC_OFF +
		XSTORM_DEF_SB_HOST_STATUS_BLOCK_OFFSET(func), func);

	for (index = 0; index < HC_XSTORM_DEF_SB_NUM_INDICES; index++)
		REG_WR16(bp, BAR_XSTRORM_INTMEM +
			 XSTORM_DEF_SB_HC_DISABLE_OFFSET(func, index), 1);

	bp->stats_pending = 0;
	bp->set_mac_pending = 0;

	bnx2x_ack_sb(bp, sb_id, CSTORM_ID, 0, IGU_INT_ENABLE, 0);
}

static void bnx2x_update_coalesce(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int i;

	for_each_queue(bp, i) {
		int sb_id = bp->fp[i].sb_id;

		/* HC_INDEX_U_ETH_RX_CQ_CONS */
		REG_WR8(bp, BAR_CSTRORM_INTMEM +
			CSTORM_SB_HC_TIMEOUT_U_OFFSET(port, sb_id,
						      U_SB_ETH_RX_CQ_INDEX),
			bp->rx_ticks/(4 * BNX2X_BTR));
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_SB_HC_DISABLE_U_OFFSET(port, sb_id,
						       U_SB_ETH_RX_CQ_INDEX),
			 (bp->rx_ticks/(4 * BNX2X_BTR)) ? 0 : 1);

		/* HC_INDEX_C_ETH_TX_CQ_CONS */
		REG_WR8(bp, BAR_CSTRORM_INTMEM +
			CSTORM_SB_HC_TIMEOUT_C_OFFSET(port, sb_id,
						      C_SB_ETH_TX_CQ_INDEX),
			bp->tx_ticks/(4 * BNX2X_BTR));
		REG_WR16(bp, BAR_CSTRORM_INTMEM +
			 CSTORM_SB_HC_DISABLE_C_OFFSET(port, sb_id,
						       C_SB_ETH_TX_CQ_INDEX),
			 (bp->tx_ticks/(4 * BNX2X_BTR)) ? 0 : 1);
	}
}

static inline void bnx2x_free_tpa_pool(struct bnx2x *bp,
				       struct bnx2x_fastpath *fp, int last)
{
	int i;

	for (i = 0; i < last; i++) {
		struct sw_rx_bd *rx_buf = &(fp->tpa_pool[i]);
		struct sk_buff *skb = rx_buf->skb;

		if (skb == NULL) {
			DP(NETIF_MSG_IFDOWN, "tpa bin %d empty on free\n", i);
			continue;
		}

		if (fp->tpa_state[i] == BNX2X_TPA_START)
			pci_unmap_single(bp->pdev,
					 pci_unmap_addr(rx_buf, mapping),
					 bp->rx_buf_size, PCI_DMA_FROMDEVICE);

		dev_kfree_skb(skb);
		rx_buf->skb = NULL;
	}
}

static void bnx2x_init_rx_rings(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);
	int max_agg_queues = CHIP_IS_E1(bp) ? ETH_MAX_AGGREGATION_QUEUES_E1 :
					      ETH_MAX_AGGREGATION_QUEUES_E1H;
	u16 ring_prod, cqe_ring_prod;
	int i, j;

	bp->rx_buf_size = bp->dev->mtu + ETH_OVREHEAD + BNX2X_RX_ALIGN;
	DP(NETIF_MSG_IFUP,
	   "mtu %d  rx_buf_size %d\n", bp->dev->mtu, bp->rx_buf_size);

	if (bp->flags & TPA_ENABLE_FLAG) {

		for_each_queue(bp, j) {
			struct bnx2x_fastpath *fp = &bp->fp[j];

			for (i = 0; i < max_agg_queues; i++) {
				fp->tpa_pool[i].skb =
				   netdev_alloc_skb(bp->dev, bp->rx_buf_size);
				if (!fp->tpa_pool[i].skb) {
					BNX2X_ERR("Failed to allocate TPA "
						  "skb pool for queue[%d] - "
						  "disabling TPA on this "
						  "queue!\n", j);
					bnx2x_free_tpa_pool(bp, fp, i);
					fp->disable_tpa = 1;
					break;
				}
				pci_unmap_addr_set((struct sw_rx_bd *)
							&bp->fp->tpa_pool[i],
						   mapping, 0);
				fp->tpa_state[i] = BNX2X_TPA_STOP;
			}
		}
	}

	for_each_queue(bp, j) {
		struct bnx2x_fastpath *fp = &bp->fp[j];

		fp->rx_bd_cons = 0;
		fp->rx_cons_sb = BNX2X_RX_SB_INDEX;
		fp->rx_bd_cons_sb = BNX2X_RX_SB_BD_INDEX;

#if defined(__NO_TPA__)
		fp->disable_tpa = 1;
#else /* BNX2X_UPSTREAM */
		/* "next page" elements initialization */
		/* SGE ring */
		for (i = 1; i <= NUM_RX_SGE_PAGES; i++) {
			struct eth_rx_sge *sge;

			sge = &fp->rx_sge_ring[RX_SGE_CNT * i - 2];
			sge->addr_hi =
				cpu_to_le32(U64_HI(fp->rx_sge_mapping +
					BCM_PAGE_SIZE*(i % NUM_RX_SGE_PAGES)));
			sge->addr_lo =
				cpu_to_le32(U64_LO(fp->rx_sge_mapping +
					BCM_PAGE_SIZE*(i % NUM_RX_SGE_PAGES)));
		}

		bnx2x_init_sge_ring_bit_mask(fp);
#endif

		/* RX BD ring */
		for (i = 1; i <= NUM_RX_RINGS; i++) {
			struct eth_rx_bd *rx_bd;

			rx_bd = &fp->rx_desc_ring[RX_DESC_CNT * i - 2];
			rx_bd->addr_hi =
				cpu_to_le32(U64_HI(fp->rx_desc_mapping +
					    BCM_PAGE_SIZE*(i % NUM_RX_RINGS)));
			rx_bd->addr_lo =
				cpu_to_le32(U64_LO(fp->rx_desc_mapping +
					    BCM_PAGE_SIZE*(i % NUM_RX_RINGS)));
		}

		/* CQ ring */
		for (i = 1; i <= NUM_RCQ_RINGS; i++) {
			struct eth_rx_cqe_next_page *nextpg;

			nextpg = (struct eth_rx_cqe_next_page *)
				&fp->rx_comp_ring[RCQ_DESC_CNT * i - 1];
			nextpg->addr_hi =
				cpu_to_le32(U64_HI(fp->rx_comp_mapping +
					   BCM_PAGE_SIZE*(i % NUM_RCQ_RINGS)));
			nextpg->addr_lo =
				cpu_to_le32(U64_LO(fp->rx_comp_mapping +
					   BCM_PAGE_SIZE*(i % NUM_RCQ_RINGS)));
		}

#if !defined(__NO_TPA__) /* BNX2X_UPSTREAM */
		/* Allocate SGEs and initialize the ring elements */
		for (i = 0, ring_prod = 0;
		     i < MAX_RX_SGE_CNT*NUM_RX_SGE_PAGES; i++) {

			if (bnx2x_alloc_rx_sge(bp, fp, ring_prod) < 0) {
				BNX2X_ERR("was only able to allocate "
					  "%d rx sges\n", i);
				BNX2X_ERR("disabling TPA for queue[%d]\n", j);
				/* Cleanup already allocated elements */
				bnx2x_free_rx_sge_range(bp, fp, ring_prod);
				bnx2x_free_tpa_pool(bp, fp, max_agg_queues);
				fp->disable_tpa = 1;
				ring_prod = 0;
				break;
			}
			ring_prod = NEXT_SGE_IDX(ring_prod);
		}
		fp->rx_sge_prod = ring_prod;
#endif

		/* Allocate BDs and initialize BD ring */
		fp->rx_comp_cons = 0;
		cqe_ring_prod = ring_prod = 0;
		for (i = 0; i < bp->rx_ring_size; i++) {
			if (bnx2x_alloc_rx_skb(bp, fp, ring_prod) < 0) {
				BNX2X_ERR("was only able to allocate "
					  "%d rx skbs on queue[%d]\n", i, j);
				fp->eth_q_stats.rx_skb_alloc_failed++;
				break;
			}
			ring_prod = NEXT_RX_IDX(ring_prod);
			cqe_ring_prod = NEXT_RCQ_IDX(cqe_ring_prod);
			WARN_ON(ring_prod <= i);
		}

		fp->rx_bd_prod = ring_prod;
		/* must not have more available CQEs than BDs */
		fp->rx_comp_prod = min_t(u16, NUM_RCQ_RINGS*RCQ_DESC_CNT,
					 cqe_ring_prod);
		fp->rx_pkt = fp->rx_calls = 0;

		/* Warning!
		 * this will generate an interrupt (to the TSTORM)
		 * must only be done after chip is initialized
		 */
		bnx2x_update_rx_prod(bp, fp, ring_prod, fp->rx_comp_prod,
				     fp->rx_sge_prod);
		if (j != 0)
			continue;

		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_MEM_WORKAROUND_ADDRESS_OFFSET(func),
		       U64_LO(fp->rx_comp_mapping));
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_MEM_WORKAROUND_ADDRESS_OFFSET(func) + 4,
		       U64_HI(fp->rx_comp_mapping));
	}
}

static void bnx2x_init_tx_ring(struct bnx2x *bp)
{
	int i, j;

	for_each_queue(bp, j) {
		struct bnx2x_fastpath *fp = &bp->fp[j];

		for (i = 1; i <= NUM_TX_RINGS; i++) {
			struct eth_tx_next_bd *tx_next_bd =
				&fp->tx_desc_ring[TX_DESC_CNT * i - 1].next_bd;

			tx_next_bd->addr_hi =
				cpu_to_le32(U64_HI(fp->tx_desc_mapping +
					    BCM_PAGE_SIZE*(i % NUM_TX_RINGS)));
			tx_next_bd->addr_lo =
				cpu_to_le32(U64_LO(fp->tx_desc_mapping +
					    BCM_PAGE_SIZE*(i % NUM_TX_RINGS)));
		}

		fp->tx_db.data.header.header = DOORBELL_HDR_DB_TYPE;
		fp->tx_db.data.zero_fill1 = 0;
		fp->tx_db.data.prod = 0;

		fp->tx_pkt_prod = 0;
		fp->tx_pkt_cons = 0;
		fp->tx_bd_prod = 0;
		fp->tx_bd_cons = 0;
		fp->tx_cons_sb = BNX2X_TX_SB_INDEX;
		fp->tx_pkt = 0;
	}
}

static void bnx2x_init_sp_ring(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);

	spin_lock_init(&bp->spq_lock);

	bp->spq_left = MAX_SPQ_PENDING;
	bp->spq_prod_idx = 0;
	bp->dsb_sp_prod = BNX2X_SP_DSB_INDEX;
	bp->spq_prod_bd = bp->spq;
	bp->spq_last_bd = bp->spq_prod_bd + MAX_SP_DESC_CNT;

	REG_WR(bp, XSEM_REG_FAST_MEMORY + XSTORM_SPQ_PAGE_BASE_OFFSET(func),
	       U64_LO(bp->spq_mapping));
	REG_WR(bp,
	       XSEM_REG_FAST_MEMORY + XSTORM_SPQ_PAGE_BASE_OFFSET(func) + 4,
	       U64_HI(bp->spq_mapping));

	REG_WR(bp, XSEM_REG_FAST_MEMORY + XSTORM_SPQ_PROD_OFFSET(func),
	       bp->spq_prod_idx);
}

static void bnx2x_init_context(struct bnx2x *bp)
{
	int i;

	/* Rx */
	for_each_queue(bp, i) {
		struct eth_context *context = bnx2x_sp(bp, context[i].eth);
		struct bnx2x_fastpath *fp = &bp->fp[i];
		u8 cl_id = fp->cl_id;

		context->ustorm_st_context.common.sb_index_numbers =
						BNX2X_RX_SB_INDEX_NUM;
		context->ustorm_st_context.common.clientId = cl_id;
		context->ustorm_st_context.common.status_block_id = fp->sb_id;
		context->ustorm_st_context.common.flags =
			(USTORM_ETH_ST_CONTEXT_CONFIG_ENABLE_MC_ALIGNMENT |
			 USTORM_ETH_ST_CONTEXT_CONFIG_ENABLE_STATISTICS);
		context->ustorm_st_context.common.statistics_counter_id =
						cl_id;
		context->ustorm_st_context.common.mc_alignment_log_size =
						BNX2X_RX_ALIGN_SHIFT;
		context->ustorm_st_context.common.bd_buff_size =
						bp->rx_buf_size;
		context->ustorm_st_context.common.bd_page_base_hi =
						U64_HI(fp->rx_desc_mapping);
		context->ustorm_st_context.common.bd_page_base_lo =
						U64_LO(fp->rx_desc_mapping);
		if (!fp->disable_tpa) {
			context->ustorm_st_context.common.flags |=
				USTORM_ETH_ST_CONTEXT_CONFIG_ENABLE_TPA;
			context->ustorm_st_context.common.sge_buff_size =
				(u16)min_t(u32, SGE_PAGE_SIZE*PAGES_PER_SGE,
					   0xffff);
			context->ustorm_st_context.common.sge_page_base_hi =
						U64_HI(fp->rx_sge_mapping);
			context->ustorm_st_context.common.sge_page_base_lo =
						U64_LO(fp->rx_sge_mapping);

			context->ustorm_st_context.common.max_sges_for_packet =
				SGE_PAGE_ALIGN(bp->dev->mtu) >> SGE_PAGE_SHIFT;
			context->ustorm_st_context.common.max_sges_for_packet =
				((context->ustorm_st_context.common.
				  max_sges_for_packet + PAGES_PER_SGE - 1) &
				 (~(PAGES_PER_SGE - 1))) >> PAGES_PER_SGE_SHIFT;
		}

		context->ustorm_ag_context.cdu_usage =
			CDU_RSRVD_VALUE_TYPE_A(HW_CID(bp, i),
					       CDU_REGION_NUMBER_UCM_AG,
					       ETH_CONNECTION_TYPE);

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
		switch (bp->multi_mode) {
		case ETH_RSS_MODE_DISABLED:
		case ETH_RSS_MODE_REGULAR:
			break;

		case ETH_RSS_MODE_VLAN_PRI:
		case ETH_RSS_MODE_E1HOV_PRI:
		case ETH_RSS_MODE_IP_DSCP:
			context->xstorm_st_context.safc_group_en = 1;
			context->xstorm_st_context.safc_group_num = fp->cos;
			break;

		default:
			break;
		}
#endif
		context->xstorm_ag_context.cdu_reserved =
			CDU_RSRVD_VALUE_TYPE_A(HW_CID(bp, i),
					       CDU_REGION_NUMBER_XCM_AG,
					       ETH_CONNECTION_TYPE);
	}

	/* Tx */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];
		struct eth_context *context =
			bnx2x_sp(bp, context[i].eth);

		context->cstorm_st_context.sb_index_number =
						C_SB_ETH_TX_CQ_INDEX;
		context->cstorm_st_context.status_block_id = fp->sb_id;

		context->xstorm_st_context.tx_bd_page_base_hi =
						U64_HI(fp->tx_desc_mapping);
		context->xstorm_st_context.tx_bd_page_base_lo =
						U64_LO(fp->tx_desc_mapping);
		context->xstorm_st_context.statistics_data = (fp->cl_id |
				XSTORM_ETH_ST_CONTEXT_STATISTICS_ENABLE);
	}
}

static void bnx2x_init_ind_table(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);
#ifdef BNX2X_SAFC
	int i, cos;
	u8 val;
#else /* BNX2X_UPSTREAM */
	int i;
#endif

	if (bp->multi_mode == ETH_RSS_MODE_DISABLED)
		return;

	DP(NETIF_MSG_IFUP,
	   "Initializing indirection table  multi_mode %d\n", bp->multi_mode);
#ifdef BNX2X_SAFC
	for (i = 0; i < TSTORM_INDIRECTION_TABLE_SIZE; i++) {
		cos = bp->pri_map[i / BNX2X_MAX_ENTRIES_PER_PRI];
		val = bp->fp->cl_id + bp->cos_map[cos] +
		      (i % bp->qs_per_cos[cos]);
		REG_WR8(bp, BAR_TSTRORM_INTMEM +
			TSTORM_INDIRECTION_TABLE_OFFSET(func) + i, val);
	}
#else /* BNX2X_UPSTREAM */
	for (i = 0; i < TSTORM_INDIRECTION_TABLE_SIZE; i++)
		REG_WR8(bp, BAR_TSTRORM_INTMEM +
			TSTORM_INDIRECTION_TABLE_OFFSET(func) + i,
			bp->fp->cl_id + (i % bp->num_queues));
#endif
}

static void bnx2x_set_client_config(struct bnx2x *bp)
{
	struct tstorm_eth_client_config tstorm_client = {0};
	int port = BP_PORT(bp);
	int i;

	tstorm_client.mtu = bp->dev->mtu;
	tstorm_client.config_flags =
				(TSTORM_ETH_CLIENT_CONFIG_STATSITICS_ENABLE |
				 TSTORM_ETH_CLIENT_CONFIG_E1HOV_REM_ENABLE);
#ifdef BCM_VLAN
	if (bp->rx_mode && bp->vlgrp && (bp->flags & HW_VLAN_RX_FLAG)) {
		tstorm_client.config_flags |=
				TSTORM_ETH_CLIENT_CONFIG_VLAN_REM_ENABLE;
		DP(NETIF_MSG_IFUP, "vlan removal enabled\n");
	}
#endif

	for_each_queue(bp, i) {
		tstorm_client.statistics_counter_id = bp->fp[i].cl_id;

		REG_WR(bp, BAR_TSTRORM_INTMEM +
		       TSTORM_CLIENT_CONFIG_OFFSET(port, bp->fp[i].cl_id),
		       ((u32 *)&tstorm_client)[0]);
		REG_WR(bp, BAR_TSTRORM_INTMEM +
		       TSTORM_CLIENT_CONFIG_OFFSET(port, bp->fp[i].cl_id) + 4,
		       ((u32 *)&tstorm_client)[1]);
	}

	DP(BNX2X_MSG_OFF, "tstorm_client: 0x%08x 0x%08x\n",
	   ((u32 *)&tstorm_client)[0], ((u32 *)&tstorm_client)[1]);
}

void bnx2x_set_storm_rx_mode(struct bnx2x *bp)
{
	struct tstorm_eth_mac_filter_config tstorm_mac_filter = {0};
	int mode = bp->rx_mode;
	int mask = bp->rx_mode_cl_mask;
	int func = BP_FUNC(bp);
	int port = BP_PORT(bp);
	int i;
	/* All but management unicast packets should pass to the host as well */
	u32 llh_mask =
		NIG_LLH0_BRB1_DRV_MASK_REG_LLH0_BRB1_DRV_MASK_BRCST |
		NIG_LLH0_BRB1_DRV_MASK_REG_LLH0_BRB1_DRV_MASK_MLCST |
		NIG_LLH0_BRB1_DRV_MASK_REG_LLH0_BRB1_DRV_MASK_VLAN |
		NIG_LLH0_BRB1_DRV_MASK_REG_LLH0_BRB1_DRV_MASK_NO_VLAN;

	DP(NETIF_MSG_IFUP, "rx mode %d  mask 0x%x\n", mode, mask);

	switch (mode) {
	case BNX2X_RX_MODE_NONE: /* no Rx */
		tstorm_mac_filter.ucast_drop_all = mask;
		tstorm_mac_filter.mcast_drop_all = mask;
		tstorm_mac_filter.bcast_drop_all = mask;
		break;

	case BNX2X_RX_MODE_NORMAL:
		tstorm_mac_filter.bcast_accept_all = mask;
		break;

	case BNX2X_RX_MODE_ALLMULTI:
		tstorm_mac_filter.mcast_accept_all = mask;
		tstorm_mac_filter.bcast_accept_all = mask;
		break;

	case BNX2X_RX_MODE_PROMISC:
		tstorm_mac_filter.ucast_accept_all = mask;
		tstorm_mac_filter.mcast_accept_all = mask;
		tstorm_mac_filter.bcast_accept_all = mask;
		/* pass management unicast packets as well */
		llh_mask |= NIG_LLH0_BRB1_DRV_MASK_REG_LLH0_BRB1_DRV_MASK_UNCST;
		break;

	default:
		BNX2X_ERR("BAD rx mode (%d)\n", mode);
		break;
	}

	REG_WR(bp,
	       (port ? NIG_REG_LLH1_BRB1_DRV_MASK : NIG_REG_LLH0_BRB1_DRV_MASK),
	       llh_mask);

	for (i = 0; i < sizeof(struct tstorm_eth_mac_filter_config)/4; i++) {
		REG_WR(bp, BAR_TSTRORM_INTMEM +
		       TSTORM_MAC_FILTER_CONFIG_OFFSET(func) + i * 4,
		       ((u32 *)&tstorm_mac_filter)[i]);

/*		DP(NETIF_MSG_IFUP, "tstorm_mac_filter[%d]: 0x%08x\n", i,
		   ((u32 *)&tstorm_mac_filter)[i]); */
	}

	if (mode != BNX2X_RX_MODE_NONE)
		bnx2x_set_client_config(bp);
}

static void bnx2x_init_internal_common(struct bnx2x *bp)
{
	int i;

	/* Zero this manually as its initialization is
	   currently missing in the initTool */
	for (i = 0; i < (USTORM_AGG_DATA_SIZE >> 2); i++)
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_AGG_DATA_OFFSET + i * 4, 0);
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	if (bp->flags & SAFC_TX_FLAG) {
		REG_WR(bp, BAR_TSTRORM_INTMEM +
		       TSTORM_COMMON_SAFC_WORKAROUND_ENABLE_OFFSET, 1);
		REG_WR(bp, BAR_TSTRORM_INTMEM +
		       TSTORM_COMMON_SAFC_WORKAROUND_TIMEOUT_10USEC_OFFSET, 10);
		REG_WR(bp, MISC_REG_SW_TIMER_VAL + 0xc, 10);
		REG_WR(bp, MISC_REG_SW_TIMER_RELOAD_VAL_4, 10);
	}
#endif
}

static void bnx2x_init_internal_port(struct bnx2x *bp)
{
	int port = BP_PORT(bp);

	REG_WR(bp,
	       BAR_CSTRORM_INTMEM + CSTORM_HC_BTR_U_OFFSET(port), BNX2X_BTR);
	REG_WR(bp,
	       BAR_CSTRORM_INTMEM + CSTORM_HC_BTR_C_OFFSET(port), BNX2X_BTR);
	REG_WR(bp, BAR_TSTRORM_INTMEM + TSTORM_HC_BTR_OFFSET(port), BNX2X_BTR);
	REG_WR(bp, BAR_XSTRORM_INTMEM + XSTORM_HC_BTR_OFFSET(port), BNX2X_BTR);
}

static void bnx2x_init_internal_func(struct bnx2x *bp)
{
	struct tstorm_eth_function_common_config tstorm_config = {0};
	struct stats_indication_flags stats_flags = {0};
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	int i, j;
	u32 offset;
	u16 max_agg_size;

	if (is_multi(bp)) {
#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__)
		/* enable queue 0 as the default queue */
		tstorm_config.config_flags =
			TSTORM_ETH_FUNCTION_COMMON_CONFIG_DEFAULT_ENABLE;
#else /* BNX2X_UPSTREAM */
		tstorm_config.config_flags = MULTI_FLAGS(bp);
		tstorm_config.rss_result_mask = MULTI_MASK;
#endif
	}

	/* Enable TPA if needed */
	if (bp->flags & TPA_ENABLE_FLAG)
		tstorm_config.config_flags |=
			TSTORM_ETH_FUNCTION_COMMON_CONFIG_ENABLE_TPA;

	if (IS_E1HMF(bp))
		tstorm_config.config_flags |=
				TSTORM_ETH_FUNCTION_COMMON_CONFIG_E1HOV_IN_CAM;

	tstorm_config.leading_client_id = BP_L_ID(bp);

	REG_WR(bp, BAR_TSTRORM_INTMEM +
	       TSTORM_FUNCTION_COMMON_CONFIG_OFFSET(func),
	       (*(u32 *)&tstorm_config));

	bp->rx_mode = BNX2X_RX_MODE_NONE; /* no rx until link is up */
	bp->rx_mode_cl_mask = (1 << BP_L_ID(bp));
	bnx2x_set_storm_rx_mode(bp);

	for_each_queue(bp, i) {
		u8 cl_id = bp->fp[i].cl_id;

		/* reset xstorm per client statistics */
		offset = BAR_XSTRORM_INTMEM +
			 XSTORM_PER_COUNTER_ID_STATS_OFFSET(port, cl_id);
		for (j = 0;
		     j < sizeof(struct xstorm_per_client_stats) / 4; j++)
			REG_WR(bp, offset + j*4, 0);

		/* reset tstorm per client statistics */
		offset = BAR_TSTRORM_INTMEM +
			 TSTORM_PER_COUNTER_ID_STATS_OFFSET(port, cl_id);
		for (j = 0;
		     j < sizeof(struct tstorm_per_client_stats) / 4; j++)
			REG_WR(bp, offset + j*4, 0);

		/* reset ustorm per client statistics */
		offset = BAR_USTRORM_INTMEM +
			 USTORM_PER_COUNTER_ID_STATS_OFFSET(port, cl_id);
		for (j = 0;
		     j < sizeof(struct ustorm_per_client_stats) / 4; j++)
			REG_WR(bp, offset + j*4, 0);
	}

	/* Init statistics related context */
	stats_flags.collect_eth = 1;

	REG_WR(bp, BAR_XSTRORM_INTMEM + XSTORM_STATS_FLAGS_OFFSET(func),
	       ((u32 *)&stats_flags)[0]);
	REG_WR(bp, BAR_XSTRORM_INTMEM + XSTORM_STATS_FLAGS_OFFSET(func) + 4,
	       ((u32 *)&stats_flags)[1]);

	REG_WR(bp, BAR_TSTRORM_INTMEM + TSTORM_STATS_FLAGS_OFFSET(func),
	       ((u32 *)&stats_flags)[0]);
	REG_WR(bp, BAR_TSTRORM_INTMEM + TSTORM_STATS_FLAGS_OFFSET(func) + 4,
	       ((u32 *)&stats_flags)[1]);

	REG_WR(bp, BAR_USTRORM_INTMEM + USTORM_STATS_FLAGS_OFFSET(func),
	       ((u32 *)&stats_flags)[0]);
	REG_WR(bp, BAR_USTRORM_INTMEM + USTORM_STATS_FLAGS_OFFSET(func) + 4,
	       ((u32 *)&stats_flags)[1]);

	REG_WR(bp, BAR_CSTRORM_INTMEM + CSTORM_STATS_FLAGS_OFFSET(func),
	       ((u32 *)&stats_flags)[0]);
	REG_WR(bp, BAR_CSTRORM_INTMEM + CSTORM_STATS_FLAGS_OFFSET(func) + 4,
	       ((u32 *)&stats_flags)[1]);

	REG_WR(bp, BAR_XSTRORM_INTMEM +
	       XSTORM_ETH_STATS_QUERY_ADDR_OFFSET(func),
	       U64_LO(bnx2x_sp_mapping(bp, fw_stats)));
	REG_WR(bp, BAR_XSTRORM_INTMEM +
	       XSTORM_ETH_STATS_QUERY_ADDR_OFFSET(func) + 4,
	       U64_HI(bnx2x_sp_mapping(bp, fw_stats)));

	REG_WR(bp, BAR_TSTRORM_INTMEM +
	       TSTORM_ETH_STATS_QUERY_ADDR_OFFSET(func),
	       U64_LO(bnx2x_sp_mapping(bp, fw_stats)));
	REG_WR(bp, BAR_TSTRORM_INTMEM +
	       TSTORM_ETH_STATS_QUERY_ADDR_OFFSET(func) + 4,
	       U64_HI(bnx2x_sp_mapping(bp, fw_stats)));

	REG_WR(bp, BAR_USTRORM_INTMEM +
	       USTORM_ETH_STATS_QUERY_ADDR_OFFSET(func),
	       U64_LO(bnx2x_sp_mapping(bp, fw_stats)));
	REG_WR(bp, BAR_USTRORM_INTMEM +
	       USTORM_ETH_STATS_QUERY_ADDR_OFFSET(func) + 4,
	       U64_HI(bnx2x_sp_mapping(bp, fw_stats)));

	if (CHIP_IS_E1H(bp)) {
		REG_WR8(bp, BAR_XSTRORM_INTMEM + XSTORM_FUNCTION_MODE_OFFSET,
			IS_E1HMF(bp));
		REG_WR8(bp, BAR_TSTRORM_INTMEM + TSTORM_FUNCTION_MODE_OFFSET,
			IS_E1HMF(bp));
		REG_WR8(bp, BAR_CSTRORM_INTMEM + CSTORM_FUNCTION_MODE_OFFSET,
			IS_E1HMF(bp));
		REG_WR8(bp, BAR_USTRORM_INTMEM + USTORM_FUNCTION_MODE_OFFSET,
			IS_E1HMF(bp));

		REG_WR16(bp, BAR_XSTRORM_INTMEM + XSTORM_E1HOV_OFFSET(func),
			 bp->e1hov);
	}

	/* Init CQ ring mapping and aggregation size, the FW limit is 8 frags */
	max_agg_size = min_t(u32, (min_t(u32, 8, MAX_SKB_FRAGS) *
				   SGE_PAGE_SIZE * PAGES_PER_SGE), 0xffff);
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_CQE_PAGE_BASE_OFFSET(port, fp->cl_id),
		       U64_LO(fp->rx_comp_mapping));
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_CQE_PAGE_BASE_OFFSET(port, fp->cl_id) + 4,
		       U64_HI(fp->rx_comp_mapping));

		/* Next page */
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_CQE_PAGE_NEXT_OFFSET(port, fp->cl_id),
		       U64_LO(fp->rx_comp_mapping + BCM_PAGE_SIZE));
		REG_WR(bp, BAR_USTRORM_INTMEM +
		       USTORM_CQE_PAGE_NEXT_OFFSET(port, fp->cl_id) + 4,
		       U64_HI(fp->rx_comp_mapping + BCM_PAGE_SIZE));

		REG_WR16(bp, BAR_USTRORM_INTMEM +
			 USTORM_MAX_AGG_SIZE_OFFSET(port, fp->cl_id),
			 max_agg_size);
	}

	/* dropless flow control */
	if (CHIP_IS_E1H(bp)) {
		struct ustorm_eth_rx_pause_data_e1h rx_pause = {0};

		rx_pause.bd_thr_low = 250;
		rx_pause.cqe_thr_low = 250;
		rx_pause.cos = 1;
		rx_pause.sge_thr_low = 0;
		rx_pause.bd_thr_high = 350;
		rx_pause.cqe_thr_high = 350;
		rx_pause.sge_thr_high = 0;

		for_each_queue(bp, i) {
			struct bnx2x_fastpath *fp = &bp->fp[i];

			if (!fp->disable_tpa) {
				rx_pause.sge_thr_low = 150;
				rx_pause.sge_thr_high = 250;
			}

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
			if (bp->flags & SAFC_TX_FLAG) {
				rx_pause.cos = 0;
				for (j = 0; j < BNX2X_MAX_PRIORITY; j++)
					if (bp->pri_map[j] == fp->cos)
						rx_pause.cos |= (1 << j);
			}
#endif

			offset = BAR_USTRORM_INTMEM +
				 USTORM_ETH_RING_PAUSE_DATA_OFFSET(port,
								   fp->cl_id);
			for (j = 0;
			     j < sizeof(struct ustorm_eth_rx_pause_data_e1h)/4;
			     j++)
				REG_WR(bp, offset + j*4,
				       ((u32 *)&rx_pause)[j]);
		}
	}

	memset(&(bp->cmng), 0, sizeof(struct cmng_struct_per_port));

	/* Init rate shaping and fairness contexts */
	if (IS_E1HMF(bp)) {
		int vn;

		/* During init there is no active link
		   Until link is up, set link rate to 10Gbps */
		bp->link_vars.line_speed = SPEED_10000;
		bnx2x_init_port_minmax(bp);

		if (!BP_NOMCP(bp))
			bp->mf_config =
			      SHMEM_RD(bp, mf_cfg.func_mf_config[func].config);
		bnx2x_calc_vn_weight_sum(bp);

		for (vn = VN_0; vn < E1HVN_MAX; vn++)
			bnx2x_init_vn_minmax(bp, 2*vn + port);

		/* Enable rate shaping and fairness */
		bp->cmng.flags.cmng_enables |=
					CMNG_FLAGS_PER_PORT_RATE_SHAPING_VN;

	} else {
#ifdef BNX2X_SAFC
		if (bp->cos_weight_sum) {
			/* During init there is no active link
			   Until link is up, set link rate to 10Gbps */
			bp->link_vars.line_speed = SPEED_10000;
			bnx2x_init_port_minmax(bp);

			bnx2x_init_cos_credit(bp);

			/* Enable fairness */
			bp->cmng.flags.cmng_enables =
					CMNG_FLAGS_PER_PORT_FAIRNESS_COS;
		} else
			DP(NETIF_MSG_IFUP, "All MIN values are zeroes"
			   "  fairness will be disabled\n");
#else /* BNX2X_UPSTREAM */
		/* rate shaping and fairness are disabled */
		DP(NETIF_MSG_IFUP,
		   "single function mode  minmax will be disabled\n");
#endif
	}

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	switch (bp->multi_mode) {
	case ETH_RSS_MODE_DISABLED:
	case ETH_RSS_MODE_REGULAR:
		break;

	case ETH_RSS_MODE_VLAN_PRI:
	case ETH_RSS_MODE_E1HOV_PRI:
	case ETH_RSS_MODE_IP_DSCP:
		bp->cmng.safc_vars.safc_timeout_usec = 200;
		for (i = 0; i < BNX2X_MAX_PRIORITY; i++)
			bp->cmng.safc_vars.cos_to_pause_mask[i] =
							(1 << bp->pri_map[i]);
		break;

	default:
		break;
	}
#endif

	/* Store cmng structures to internal memory */
	if (bp->port.pmf)
		for (i = 0; i < sizeof(struct cmng_struct_per_port) / 4; i++)
			REG_WR(bp, BAR_XSTRORM_INTMEM +
			       XSTORM_CMNG_PER_PORT_VARS_OFFSET(port) + i * 4,
			       ((u32 *)(&bp->cmng))[i]);
}

static void bnx2x_init_internal(struct bnx2x *bp, u32 load_code)
{
	switch (load_code) {
	case FW_MSG_CODE_DRV_LOAD_COMMON:
		bnx2x_init_internal_common(bp);
		/* no break */

	case FW_MSG_CODE_DRV_LOAD_PORT:
		bnx2x_init_internal_port(bp);
		/* no break */

	case FW_MSG_CODE_DRV_LOAD_FUNCTION:
		bnx2x_init_internal_func(bp);
		break;

	default:
		BNX2X_ERR("Unknown load_code (0x%x) from MCP\n", load_code);
		break;
	}
}

static void bnx2x_nic_init(struct bnx2x *bp, u32 load_code)
{
	int i;
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	int cos, num_queues;
#endif

	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		fp->bp = bp;
		fp->state = BNX2X_FP_STATE_CLOSED;
		fp->index = i;
		fp->cl_id = BP_L_ID(bp) + i;
#ifdef BCM_CNIC
		fp->sb_id = fp->cl_id + 1;
#else
		fp->sb_id = fp->cl_id;
#endif
#ifdef BNX2X_SAFC
		num_queues = 0;
		for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
			if (i < (num_queues + bp->qs_per_cos[cos])) {
				fp->cos = cos;
				break;
			} else
				num_queues += bp->qs_per_cos[cos];
		}
		DP(NETIF_MSG_IFUP, "queue[%d]:  bnx2x_init_sb(%p,%p)  "
				   "cl_id %d  sb %d  cos %d\n",
		   i, bp, fp->status_blk, fp->cl_id, fp->sb_id, fp->cos);
#else /* BNX2X_UPSTREAM */
		DP(NETIF_MSG_IFUP,
		   "queue[%d]:  bnx2x_init_sb(%p,%p)  cl_id %d  sb %d\n",
		   i, bp, fp->status_blk, fp->cl_id, fp->sb_id);
#endif
		bnx2x_init_sb(bp, fp->status_blk, fp->status_blk_mapping,
			      fp->sb_id);
		bnx2x_update_fpsb_idx(fp);
	}

	/* ensure status block indices were read */
	rmb();

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
		bp->cos_map[cos] = 0;
		for_each_queue(bp, i) {
			/* take the 1st queue that belongs to this cos */
			if (bp->fp[i].cos == cos) {
				bp->cos_map[cos] = i;
				break;
			}
		}
	}
#endif

	bnx2x_init_def_sb(bp, bp->def_status_blk, bp->def_status_blk_mapping,
			  DEF_SB_ID);
	bnx2x_update_dsb_idx(bp);
	bnx2x_update_coalesce(bp);
	bnx2x_init_rx_rings(bp);
	bnx2x_init_tx_ring(bp);
	bnx2x_init_sp_ring(bp);
	bnx2x_init_context(bp);
	bnx2x_init_internal(bp, load_code);
	bnx2x_init_ind_table(bp);
	bnx2x_stats_init(bp);

	/* At this point, we are ready for interrupts */
	atomic_set(&bp->intr_sem, 0);

	/* flush all before enabling interrupts */
	mb();
	mmiowb();

	bnx2x_int_enable(bp);

	/* Check for SPIO5 */
	bnx2x_attn_int_deasserted0(bp,
		REG_RD(bp, MISC_REG_AEU_AFTER_INVERT_1_FUNC_0 + BP_PORT(bp)*4) &
				   AEU_INPUTS_ATTN_BITS_SPIO5);
}

/* end of nic init */

/*
 * gzip service functions
 */

static int bnx2x_gunzip_init(struct bnx2x *bp)
{
	bp->gunzip_buf = pci_alloc_consistent(bp->pdev, FW_BUF_SIZE,
					      &bp->gunzip_mapping);
	if (bp->gunzip_buf  == NULL)
		goto gunzip_nomem1;

	bp->strm = kmalloc(sizeof(*bp->strm), GFP_KERNEL);
	if (bp->strm  == NULL)
		goto gunzip_nomem2;

	bp->strm->workspace = kmalloc(zlib_inflate_workspacesize(),
				      GFP_KERNEL);
	if (bp->strm->workspace == NULL)
		goto gunzip_nomem3;

	return 0;

gunzip_nomem3:
	kfree(bp->strm);
	bp->strm = NULL;

gunzip_nomem2:
	pci_free_consistent(bp->pdev, FW_BUF_SIZE, bp->gunzip_buf,
			    bp->gunzip_mapping);
	bp->gunzip_buf = NULL;

gunzip_nomem1:
	printk(KERN_ERR PFX "%s: Cannot allocate firmware buffer for"
	       " un-compression\n", bp->dev->name);
	return -ENOMEM;
}

static void bnx2x_gunzip_end(struct bnx2x *bp)
{
	kfree(bp->strm->workspace);

	kfree(bp->strm);
	bp->strm = NULL;

	if (bp->gunzip_buf) {
		pci_free_consistent(bp->pdev, FW_BUF_SIZE, bp->gunzip_buf,
				    bp->gunzip_mapping);
		bp->gunzip_buf = NULL;
	}
}

static int bnx2x_gunzip(struct bnx2x *bp, const u8 *zbuf, int len)
{
	int n, rc;

	/* check gzip header */
	if ((zbuf[0] != 0x1f) || (zbuf[1] != 0x8b) || (zbuf[2] != Z_DEFLATED)) {
		BNX2X_ERR("Bad gzip header\n");
		return -EINVAL;
	}

	n = 10;

#define FNAME				0x8

	if (zbuf[3] & FNAME)
		while ((zbuf[n++] != 0) && (n < len));

	bp->strm->next_in = (typeof(bp->strm->next_in))zbuf + n;
	bp->strm->avail_in = len - n;
	bp->strm->next_out = bp->gunzip_buf;
	bp->strm->avail_out = FW_BUF_SIZE;

	rc = zlib_inflateInit2(bp->strm, -MAX_WBITS);
	if (rc != Z_OK)
		return rc;

	rc = zlib_inflate(bp->strm, Z_FINISH);
	if ((rc != Z_OK) && (rc != Z_STREAM_END))
		printk(KERN_ERR PFX "%s: Firmware decompression error: %s\n",
		       bp->dev->name, bp->strm->msg);

	bp->gunzip_outlen = (FW_BUF_SIZE - bp->strm->avail_out);
	if (bp->gunzip_outlen & 0x3)
		printk(KERN_ERR PFX "%s: Firmware decompression error:"
				    " gunzip_outlen (%d) not aligned\n",
		       bp->dev->name, bp->gunzip_outlen);
	bp->gunzip_outlen >>= 2;

	zlib_inflateEnd(bp->strm);

	if (rc == Z_STREAM_END)
		return 0;

	return rc;
}

/* nic load/unload */

/*
 * General service functions
 */

/* send a NIG loopback debug packet */
static void bnx2x_lb_pckt(struct bnx2x *bp)
{
	u32 wb_write[3];

	/* Ethernet source and destination addresses */
	wb_write[0] = 0x55555555;
	wb_write[1] = 0x55555555;
	wb_write[2] = 0x20;		/* SOP */
	REG_WR_DMAE(bp, NIG_REG_DEBUG_PACKET_LB, wb_write, 3);

	/* NON-IP protocol */
	wb_write[0] = 0x09000000;
	wb_write[1] = 0x55555555;
	wb_write[2] = 0x10;		/* EOP, eop_bvalid = 0 */
	REG_WR_DMAE(bp, NIG_REG_DEBUG_PACKET_LB, wb_write, 3);
}

/* some of the internal memories
 * are not directly readable from the driver
 * to test them we send debug packets
 */
static int bnx2x_int_mem_test(struct bnx2x *bp)
{
	int factor;
	int count, i;
	u32 val = 0;

	if (CHIP_REV_IS_FPGA(bp))
		factor = 120;
	else if (CHIP_REV_IS_EMUL(bp))
		factor = 200;
	else
		factor = 1;

	DP(NETIF_MSG_HW, "start part1\n");

	/* Disable inputs of parser neighbor blocks */
	REG_WR(bp, TSDM_REG_ENABLE_IN1, 0x0);
	REG_WR(bp, TCM_REG_PRS_IFEN, 0x0);
	REG_WR(bp, CFC_REG_DEBUG0, 0x1);
	REG_WR(bp, NIG_REG_PRS_REQ_IN_EN, 0x0);

	/*  Write 0 to parser credits for CFC search request */
	REG_WR(bp, PRS_REG_CFC_SEARCH_INITIAL_CREDIT, 0x0);

	/* send Ethernet packet */
	bnx2x_lb_pckt(bp);

	/* TODO do i reset NIG statistic? */
	/* Wait until NIG register shows 1 packet of size 0x10 */
	count = 1000 * factor;
	while (count) {

		bnx2x_read_dmae(bp, NIG_REG_STAT2_BRB_OCTET, 2);
		val = *bnx2x_sp(bp, wb_data[0]);
		if (val == 0x10)
			break;

		msleep(10);
		count--;
	}
	if (val != 0x10) {
		BNX2X_ERR("NIG timeout  val = 0x%x\n", val);
		return -1;
	}

	/* Wait until PRS register shows 1 packet */
	count = 1000 * factor;
	while (count) {
		val = REG_RD(bp, PRS_REG_NUM_OF_PACKETS);
		if (val == 1)
			break;

		msleep(10);
		count--;
	}
	if (val != 0x1) {
		BNX2X_ERR("PRS timeout val = 0x%x\n", val);
		return -2;
	}

	/* Reset and init BRB, PRS */
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR, 0x03);
	msleep(50);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET, 0x03);
	msleep(50);
	bnx2x_init_block(bp, BRB1_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, PRS_BLOCK, COMMON_STAGE);

	DP(NETIF_MSG_HW, "part2\n");

	/* Disable inputs of parser neighbor blocks */
	REG_WR(bp, TSDM_REG_ENABLE_IN1, 0x0);
	REG_WR(bp, TCM_REG_PRS_IFEN, 0x0);
	REG_WR(bp, CFC_REG_DEBUG0, 0x1);
	REG_WR(bp, NIG_REG_PRS_REQ_IN_EN, 0x0);

	/* Write 0 to parser credits for CFC search request */
	REG_WR(bp, PRS_REG_CFC_SEARCH_INITIAL_CREDIT, 0x0);

	/* send 10 Ethernet packets */
	for (i = 0; i < 10; i++)
		bnx2x_lb_pckt(bp);

	/* Wait until NIG register shows 10 + 1
	   packets of size 11*0x10 = 0xb0 */
	count = 1000 * factor;
	while (count) {

		bnx2x_read_dmae(bp, NIG_REG_STAT2_BRB_OCTET, 2);
		val = *bnx2x_sp(bp, wb_data[0]);
		if (val == 0xb0)
			break;

		msleep(10);
		count--;
	}
	if (val != 0xb0) {
		BNX2X_ERR("NIG timeout  val = 0x%x\n", val);
		return -3;
	}

	/* Wait until PRS register shows 2 packets */
	val = REG_RD(bp, PRS_REG_NUM_OF_PACKETS);
	if (val != 2)
		BNX2X_ERR("PRS timeout  val = 0x%x\n", val);

	/* Write 1 to parser credits for CFC search request */
	REG_WR(bp, PRS_REG_CFC_SEARCH_INITIAL_CREDIT, 0x1);

	/* Wait until PRS register shows 3 packets */
	msleep(10 * factor);
	/* Wait until NIG register shows 1 packet of size 0x10 */
	val = REG_RD(bp, PRS_REG_NUM_OF_PACKETS);
	if (val != 3)
		BNX2X_ERR("PRS timeout  val = 0x%x\n", val);

	/* clear NIG EOP FIFO */
	for (i = 0; i < 11; i++)
		REG_RD(bp, NIG_REG_INGRESS_EOP_LB_FIFO);
	val = REG_RD(bp, NIG_REG_INGRESS_EOP_LB_EMPTY);
	if (val != 1) {
		BNX2X_ERR("clear of NIG failed\n");
		return -4;
	}

	/* Reset and init BRB, PRS, NIG */
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR, 0x03);
	msleep(50);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET, 0x03);
	msleep(50);
	bnx2x_init_block(bp, BRB1_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, PRS_BLOCK, COMMON_STAGE);
#ifndef BCM_CNIC
	/* set NIC mode */
	REG_WR(bp, PRS_REG_NIC_MODE, 1);
#endif

	/* Enable inputs of parser neighbor blocks */
	REG_WR(bp, TSDM_REG_ENABLE_IN1, 0x7fffffff);
	REG_WR(bp, TCM_REG_PRS_IFEN, 0x1);
	REG_WR(bp, CFC_REG_DEBUG0, 0x0);
	REG_WR(bp, NIG_REG_PRS_REQ_IN_EN, 0x1);

	DP(NETIF_MSG_HW, "done\n");

	return 0; /* OK */
}

static void enable_blocks_attention(struct bnx2x *bp)
{
	REG_WR(bp, PXP_REG_PXP_INT_MASK_0, 0);
	REG_WR(bp, PXP_REG_PXP_INT_MASK_1, 0);
	REG_WR(bp, DORQ_REG_DORQ_INT_MASK, 0);
	REG_WR(bp, CFC_REG_CFC_INT_MASK, 0);
	REG_WR(bp, QM_REG_QM_INT_MASK, 0);
	REG_WR(bp, TM_REG_TM_INT_MASK, 0);
	REG_WR(bp, XSDM_REG_XSDM_INT_MASK_0, 0);
	REG_WR(bp, XSDM_REG_XSDM_INT_MASK_1, 0);
	REG_WR(bp, XCM_REG_XCM_INT_MASK, 0);
/*	REG_WR(bp, XSEM_REG_XSEM_INT_MASK_0, 0); */
/*	REG_WR(bp, XSEM_REG_XSEM_INT_MASK_1, 0); */
	REG_WR(bp, USDM_REG_USDM_INT_MASK_0, 0);
	REG_WR(bp, USDM_REG_USDM_INT_MASK_1, 0);
	REG_WR(bp, UCM_REG_UCM_INT_MASK, 0);
/*	REG_WR(bp, USEM_REG_USEM_INT_MASK_0, 0); */
/*	REG_WR(bp, USEM_REG_USEM_INT_MASK_1, 0); */
	REG_WR(bp, GRCBASE_UPB + PB_REG_PB_INT_MASK, 0);
	REG_WR(bp, CSDM_REG_CSDM_INT_MASK_0, 0);
	REG_WR(bp, CSDM_REG_CSDM_INT_MASK_1, 0);
	REG_WR(bp, CCM_REG_CCM_INT_MASK, 0);
/*	REG_WR(bp, CSEM_REG_CSEM_INT_MASK_0, 0); */
/*	REG_WR(bp, CSEM_REG_CSEM_INT_MASK_1, 0); */
	if (CHIP_REV_IS_FPGA(bp))
		REG_WR(bp, PXP2_REG_PXP2_INT_MASK_0, 0x580000);
	else
		REG_WR(bp, PXP2_REG_PXP2_INT_MASK_0, 0x480000);
	REG_WR(bp, TSDM_REG_TSDM_INT_MASK_0, 0);
	REG_WR(bp, TSDM_REG_TSDM_INT_MASK_1, 0);
	REG_WR(bp, TCM_REG_TCM_INT_MASK, 0);
/*	REG_WR(bp, TSEM_REG_TSEM_INT_MASK_0, 0); */
/*	REG_WR(bp, TSEM_REG_TSEM_INT_MASK_1, 0); */
	REG_WR(bp, CDU_REG_CDU_INT_MASK, 0);
	REG_WR(bp, DMAE_REG_DMAE_INT_MASK, 0);
/*	REG_WR(bp, MISC_REG_MISC_INT_MASK, 0); */
	REG_WR(bp, PBF_REG_PBF_INT_MASK, 0X18);		/* bit 3,4 masked */
}

#ifdef BNX2X_EXTRA_DEBUG /* ! BNX2X_UPSTREAM */
static void enable_blocks_parity(struct bnx2x *bp)
{
	REG_WR(bp, PXP_REG_PXP_PRTY_MASK, 0x80000);	/* bit 19 masked */
	REG_WR(bp, PXP2_REG_PXP2_PRTY_MASK_0, 0xfff40020);/* bit 5,18,20-31 */
	REG_WR(bp, PXP2_REG_PXP2_PRTY_MASK_1, 0x20);	/* bit 5 */
	REG_WR(bp, QM_REG_QM_PRTY_MASK, 0x0);
	REG_WR(bp, DORQ_REG_DORQ_PRTY_MASK, 0x0);
	REG_WR(bp, GRCBASE_UPB + PB_REG_PB_PRTY_MASK, 0x0);
	REG_WR(bp, GRCBASE_XPB + PB_REG_PB_PRTY_MASK, 0x0);
	REG_WR(bp, SRC_REG_SRC_PRTY_MASK, 0x4);		/* bit 2 */
	REG_WR(bp, CDU_REG_CDU_PRTY_MASK, 0x0);
	REG_WR(bp, CFC_REG_CFC_PRTY_MASK, 0x0);
	REG_WR(bp, HC_REG_HC_PRTY_MASK, 0x0);
	REG_WR(bp, MISC_REG_MISC_PRTY_MASK, 0x0);
	REG_WR(bp, DBG_REG_DBG_PRTY_MASK, 0x0);
	REG_WR(bp, DMAE_REG_DMAE_PRTY_MASK, 0x0);
	REG_WR(bp, BRB1_REG_BRB1_PRTY_MASK, 0x0);
	REG_WR(bp, PRS_REG_PRS_PRTY_MASK, (1<<6));	/* bit 6 */
	REG_WR(bp, TSDM_REG_TSDM_PRTY_MASK, 0x8);	/* bit 3 */
	REG_WR(bp, CSDM_REG_CSDM_PRTY_MASK, 0x8);	/* bit 3 */
	REG_WR(bp, USDM_REG_USDM_PRTY_MASK, 0x28);	/* bit 3,5 */
	REG_WR(bp, XSDM_REG_XSDM_PRTY_MASK, 0x8);	/* bit 3 */
	REG_WR(bp, TSEM_REG_TSEM_PRTY_MASK_0, 0x0);
	REG_WR(bp, TSEM_REG_TSEM_PRTY_MASK_1, 0x0);
	REG_WR(bp, USEM_REG_USEM_PRTY_MASK_0, 0x0);
	REG_WR(bp, USEM_REG_USEM_PRTY_MASK_1, 0x0);
	REG_WR(bp, CSEM_REG_CSEM_PRTY_MASK_0, 0x0);
	REG_WR(bp, CSEM_REG_CSEM_PRTY_MASK_1, 0x0);
	REG_WR(bp, XSEM_REG_XSEM_PRTY_MASK_0, 0x0);
	REG_WR(bp, XSEM_REG_XSEM_PRTY_MASK_1, 0x0);
}
#endif

static void bnx2x_reset_common(struct bnx2x *bp)
{
	/* reset_common */
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR,
	       0xd3ffff7f);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_2_CLEAR, 0x1403);
}

static void bnx2x_init_pxp(struct bnx2x *bp)
{
	u16 devctl;
	int r_order, w_order;

	pci_read_config_word(bp->pdev,
			     bp->pcie_cap + PCI_EXP_DEVCTL, &devctl);
	DP(NETIF_MSG_HW, "read 0x%x from devctl\n", devctl);
	w_order = ((devctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5);
	if (bp->mrrs == -1)
		r_order = ((devctl & PCI_EXP_DEVCTL_READRQ) >> 12);
	else {
		DP(NETIF_MSG_HW, "force read order to %d\n", bp->mrrs);
		r_order = bp->mrrs;
	}

	bnx2x_init_pxp_arb(bp, r_order, w_order);
}

static void bnx2x_setup_fan_failure_detection(struct bnx2x *bp)
{
	int is_required;
	u32 val;
	int port;

	if (BP_NOMCP(bp))
		return;

	is_required = 0;
	val = SHMEM_RD(bp, dev_info.shared_hw_config.config2) &
	      SHARED_HW_CFG_FAN_FAILURE_MASK;

	if (val == SHARED_HW_CFG_FAN_FAILURE_ENABLED)
		is_required = 1;

	/*
	 * The fan failure mechanism is usually related to the PHY type since
	 * the power consumption of the board is affected by the PHY. Currently,
	 * fan is required for most designs with SFX7101, BCM8727 and BCM8481.
	 */
	else if (val == SHARED_HW_CFG_FAN_FAILURE_PHY_TYPE)
		for (port = PORT_0; port < PORT_MAX; port++) {
			u32 phy_type =
				SHMEM_RD(bp, dev_info.port_hw_config[port].
					 external_phy_config) &
				PORT_HW_CFG_XGXS_EXT_PHY_TYPE_MASK;
			is_required |=
				((phy_type ==
				  PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101) ||
				 (phy_type ==
				  PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727) ||
				 (phy_type ==
				  PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8481));
		}

	DP(NETIF_MSG_HW, "fan detection setting: %d\n", is_required);

	if (is_required == 0)
		return;

	/* Fan failure is indicated by SPIO 5 */
	bnx2x_set_spio(bp, MISC_REGISTERS_SPIO_5,
		       MISC_REGISTERS_SPIO_INPUT_HI_Z);

	/* set to active low mode */
	val = REG_RD(bp, MISC_REG_SPIO_INT);
	val |= ((1 << MISC_REGISTERS_SPIO_5) <<
					MISC_REGISTERS_SPIO_INT_OLD_SET_POS);
	REG_WR(bp, MISC_REG_SPIO_INT, val);

	/* enable interrupt to signal the IGU */
	val = REG_RD(bp, MISC_REG_SPIO_EVENT_EN);
	val |= (1 << MISC_REGISTERS_SPIO_5);
	REG_WR(bp, MISC_REG_SPIO_EVENT_EN, val);
}

static int bnx2x_init_common(struct bnx2x *bp)
{
	u32 val, i;
#ifdef BCM_CNIC
	u32 wb_write[2];
#endif

	DP(BNX2X_MSG_MCP, "starting common init  func %d\n", BP_FUNC(bp));

	bnx2x_reset_common(bp);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET, 0xffffffff);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_2_SET, 0xfffc);

	bnx2x_init_block(bp, MISC_BLOCK, COMMON_STAGE);
	if (CHIP_IS_E1H(bp))
		REG_WR(bp, MISC_REG_E1HMF_MODE, IS_E1HMF(bp));

	REG_WR(bp, MISC_REG_LCPLL_CTRL_REG_2, 0x100);
	msleep(30);
	REG_WR(bp, MISC_REG_LCPLL_CTRL_REG_2, 0x0);

	bnx2x_init_block(bp, PXP_BLOCK, COMMON_STAGE);
	if (CHIP_IS_E1(bp)) {
		/* enable HW interrupt from PXP on USDM overflow
		   bit 16 on INT_MASK_0 */
		REG_WR(bp, PXP_REG_PXP_INT_MASK_0, 0);
	}

	bnx2x_init_block(bp, PXP2_BLOCK, COMMON_STAGE);
	bnx2x_init_pxp(bp);

#ifdef __BIG_ENDIAN
	REG_WR(bp, PXP2_REG_RQ_QM_ENDIAN_M, 1);
	REG_WR(bp, PXP2_REG_RQ_TM_ENDIAN_M, 1);
	REG_WR(bp, PXP2_REG_RQ_SRC_ENDIAN_M, 1);
	REG_WR(bp, PXP2_REG_RQ_CDU_ENDIAN_M, 1);
	REG_WR(bp, PXP2_REG_RQ_DBG_ENDIAN_M, 1);
	/* make sure this value is 0 */
	REG_WR(bp, PXP2_REG_RQ_HC_ENDIAN_M, 0);

/*	REG_WR(bp, PXP2_REG_RD_PBF_SWAP_MODE, 1); */
	REG_WR(bp, PXP2_REG_RD_QM_SWAP_MODE, 1);
	REG_WR(bp, PXP2_REG_RD_TM_SWAP_MODE, 1);
	REG_WR(bp, PXP2_REG_RD_SRC_SWAP_MODE, 1);
	REG_WR(bp, PXP2_REG_RD_CDURD_SWAP_MODE, 1);
#endif

	REG_WR(bp, PXP2_REG_RQ_CDU_P_SIZE, 2);
#ifdef BCM_CNIC
	REG_WR(bp, PXP2_REG_RQ_TM_P_SIZE, 5);
	REG_WR(bp, PXP2_REG_RQ_QM_P_SIZE, 5);
	REG_WR(bp, PXP2_REG_RQ_SRC_P_SIZE, 5);
#endif

	if (CHIP_REV_IS_FPGA(bp) && CHIP_IS_E1H(bp))
		REG_WR(bp, PXP2_REG_PGL_TAGS_LIMIT, 0x1);

	/* let the HW do it's magic ... */
	msleep(100);
	/* finish PXP init */
	val = REG_RD(bp, PXP2_REG_RQ_CFG_DONE);
	if (val != 1) {
		BNX2X_ERR("PXP2 CFG failed\n");
		return -EBUSY;
	}
	val = REG_RD(bp, PXP2_REG_RD_INIT_DONE);
	if (val != 1) {
		BNX2X_ERR("PXP2 RD_INIT failed\n");
		return -EBUSY;
	}

	REG_WR(bp, PXP2_REG_RQ_DISABLE_INPUTS, 0);
	REG_WR(bp, PXP2_REG_RD_DISABLE_INPUTS, 0);

	bnx2x_init_block(bp, DMAE_BLOCK, COMMON_STAGE);

	/* clean the DMAE memory */
	bp->dmae_ready = 1;
	bnx2x_init_fill(bp, TSEM_REG_PRAM, 0, 8);

	bnx2x_init_block(bp, TCM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, UCM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, CCM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, XCM_BLOCK, COMMON_STAGE);

	bnx2x_read_dmae(bp, XSEM_REG_PASSIVE_BUFFER, 3);
	bnx2x_read_dmae(bp, CSEM_REG_PASSIVE_BUFFER, 3);
	bnx2x_read_dmae(bp, TSEM_REG_PASSIVE_BUFFER, 3);
	bnx2x_read_dmae(bp, USEM_REG_PASSIVE_BUFFER, 3);

	bnx2x_init_block(bp, QM_BLOCK, COMMON_STAGE);
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	switch (bp->multi_mode) {
	case ETH_RSS_MODE_DISABLED:
	case ETH_RSS_MODE_REGULAR:
		break;

	case ETH_RSS_MODE_VLAN_PRI:
	case ETH_RSS_MODE_E1HOV_PRI:
	case ETH_RSS_MODE_IP_DSCP:
		REG_WR(bp, QM_REG_BYTECRDINITVAL, 0x5dc0);
		break;

	default:
		break;
	}
#endif
#ifdef BCM_CNIC
	wb_write[0] = 0;
	wb_write[1] = 0;
	for (i = 0; i < 64; i++) {
		REG_WR(bp, QM_REG_BASEADDR + i*4, 1024 * 4 * (i%16));
		bnx2x_init_ind_wr(bp, QM_REG_PTRTBL + i*8, wb_write, 2);

		if (CHIP_IS_E1H(bp)) {
			REG_WR(bp, QM_REG_BASEADDR_EXT_A + i*4, 1024*4*(i%16));
			bnx2x_init_ind_wr(bp, QM_REG_PTRTBL_EXT_A + i*8,
					  wb_write, 2);
		}
	}
#endif
	/* soft reset pulse */
	REG_WR(bp, QM_REG_SOFT_RESET, 1);
	REG_WR(bp, QM_REG_SOFT_RESET, 0);

#ifdef BCM_CNIC
	bnx2x_init_block(bp, TIMERS_BLOCK, COMMON_STAGE);
#endif

	bnx2x_init_block(bp, DQ_BLOCK, COMMON_STAGE);
	REG_WR(bp, DORQ_REG_DPM_CID_OFST, BCM_PAGE_SHIFT);
	if (!CHIP_REV_IS_SLOW(bp)) {
		/* enable hw interrupt from doorbell Q */
		REG_WR(bp, DORQ_REG_DORQ_INT_MASK, 0);
	}

	bnx2x_init_block(bp, BRB1_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, PRS_BLOCK, COMMON_STAGE);
	REG_WR(bp, PRS_REG_A_PRSU_20, 0xf);
#ifndef BCM_CNIC
	/* set NIC mode */
	REG_WR(bp, PRS_REG_NIC_MODE, 1);
#endif
#ifdef BNX2X_SAFC
	if (CHIP_IS_E1H(bp)) {
		val = (IS_E1HMF(bp) ||
		       (bp->multi_mode == ETH_RSS_MODE_E1HOV_PRI)) ? 1 : 0;
		REG_WR(bp, PRS_REG_E1HOV_MODE, val);
	}
#else /* BNX2X_UPSTREAM */
	if (CHIP_IS_E1H(bp))
		REG_WR(bp, PRS_REG_E1HOV_MODE, IS_E1HMF(bp));
#endif

	bnx2x_init_block(bp, TSDM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, CSDM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, USDM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, XSDM_BLOCK, COMMON_STAGE);

	bnx2x_init_fill(bp, TSEM_REG_FAST_MEMORY, 0, STORM_INTMEM_SIZE(bp));
	bnx2x_init_fill(bp, USEM_REG_FAST_MEMORY, 0, STORM_INTMEM_SIZE(bp));
	bnx2x_init_fill(bp, CSEM_REG_FAST_MEMORY, 0, STORM_INTMEM_SIZE(bp));
	bnx2x_init_fill(bp, XSEM_REG_FAST_MEMORY, 0, STORM_INTMEM_SIZE(bp));

	bnx2x_init_block(bp, TSEM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, USEM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, CSEM_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, XSEM_BLOCK, COMMON_STAGE);

	/* sync semi rtc */
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR,
	       0x80000000);
	REG_WR(bp, GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET,
	       0x80000000);

	bnx2x_init_block(bp, UPB_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, XPB_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, PBF_BLOCK, COMMON_STAGE);

	REG_WR(bp, SRC_REG_SOFT_RST, 1);
	for (i = SRC_REG_KEYRSS0_0; i <= SRC_REG_KEYRSS1_9; i += 4) {
		REG_WR(bp, i, 0xc0cac01a);
		/* TODO: replace with something meaningful */
	}
	bnx2x_init_block(bp, SRCH_BLOCK, COMMON_STAGE);
#ifdef BCM_CNIC
	REG_WR(bp, SRC_REG_KEYSEARCH_0, 0x63285672);
	REG_WR(bp, SRC_REG_KEYSEARCH_1, 0x24b8f2cc);
	REG_WR(bp, SRC_REG_KEYSEARCH_2, 0x223aef9b);
	REG_WR(bp, SRC_REG_KEYSEARCH_3, 0x26001e3a);
	REG_WR(bp, SRC_REG_KEYSEARCH_4, 0x7ae91116);
	REG_WR(bp, SRC_REG_KEYSEARCH_5, 0x5ce5230b);
	REG_WR(bp, SRC_REG_KEYSEARCH_6, 0x298d8adf);
	REG_WR(bp, SRC_REG_KEYSEARCH_7, 0x6eb0ff09);
	REG_WR(bp, SRC_REG_KEYSEARCH_8, 0x1830f82f);
	REG_WR(bp, SRC_REG_KEYSEARCH_9, 0x01e46be7);
#endif
	REG_WR(bp, SRC_REG_SOFT_RST, 0);

	if (sizeof(union cdu_context) != 1024)
		/* we currently assume that a context is 1024 bytes */
		printk(KERN_ALERT PFX "please adjust the size of"
		       " cdu_context(%ld)\n", (long)sizeof(union cdu_context));

	bnx2x_init_block(bp, CDU_BLOCK, COMMON_STAGE);
	val = (4 << 24) + (0 << 12) + 1024;
	REG_WR(bp, CDU_REG_CDU_GLOBAL_PARAMS, val);

	bnx2x_init_block(bp, CFC_BLOCK, COMMON_STAGE);
	REG_WR(bp, CFC_REG_INIT_REG, 0x7FF);
	/* enable context validation interrupt from CFC */
	REG_WR(bp, CFC_REG_CFC_INT_MASK, 0);

	/* set the thresholds to prevent CFC/CDU race */
	REG_WR(bp, CFC_REG_DEBUG0, 0x20020000);

	bnx2x_init_block(bp, HC_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, MISC_AEU_BLOCK, COMMON_STAGE);

	bnx2x_init_block(bp, PXPCS_BLOCK, COMMON_STAGE);
	/* Reset PCIE errors for debug */
	REG_WR(bp, 0x2814, 0xffffffff);
	REG_WR(bp, 0x3820, 0xffffffff);

	bnx2x_init_block(bp, EMAC0_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, EMAC1_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, DBU_BLOCK, COMMON_STAGE);
	bnx2x_init_block(bp, DBG_BLOCK, COMMON_STAGE);

	bnx2x_init_block(bp, NIG_BLOCK, COMMON_STAGE);
	if (CHIP_IS_E1H(bp)) {
		REG_WR(bp, NIG_REG_LLH_MF_MODE, IS_E1HMF(bp));
		REG_WR(bp, NIG_REG_LLH_E1HOV_MODE, IS_E1HMF(bp));
	}

	if (CHIP_REV_IS_SLOW(bp))
		msleep(200);

	/* finish CFC init */
	val = reg_poll(bp, CFC_REG_LL_INIT_DONE, 1, 100, 10);
	if (val != 1) {
		BNX2X_ERR("CFC LL_INIT failed\n");
		return -EBUSY;
	}
	val = reg_poll(bp, CFC_REG_AC_INIT_DONE, 1, 100, 10);
	if (val != 1) {
		BNX2X_ERR("CFC AC_INIT failed\n");
		return -EBUSY;
	}
	val = reg_poll(bp, CFC_REG_CAM_INIT_DONE, 1, 100, 10);
	if (val != 1) {
		BNX2X_ERR("CFC CAM_INIT failed\n");
		return -EBUSY;
	}
	REG_WR(bp, CFC_REG_DEBUG0, 0);

	/* read NIG statistic
	   to see if this is our first up since powerup */
	bnx2x_read_dmae(bp, NIG_REG_STAT2_BRB_OCTET, 2);
	val = *bnx2x_sp(bp, wb_data[0]);

	/* do internal memory self test */
	if ((CHIP_IS_E1(bp)) && (val == 0) && bnx2x_int_mem_test(bp)) {
		BNX2X_ERR("internal mem self test failed\n");
		return -EBUSY;
	}

	switch (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config)) {
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8072:
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8073:
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8726:
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727:
		bp->port.need_hw_lock = 1;
		break;

	default:
		break;
	}

	bnx2x_setup_fan_failure_detection(bp);

	/* clear PXP2 attentions */
	REG_RD(bp, PXP2_REG_PXP2_INT_STS_CLR_0);

	enable_blocks_attention(bp);
#ifdef BNX2X_EXTRA_DEBUG /* ! BNX2X_UPSTREAM */
	enable_blocks_parity(bp);
#endif

	if (!BP_NOMCP(bp)) {
		bnx2x_acquire_phy_lock(bp);
		bnx2x_common_init_phy(bp, bp->common.shmem_base);
		bnx2x_release_phy_lock(bp);
	} else
		BNX2X_ERR("Bootcode is missing - can not initialize link\n");

	return 0;
}

static int bnx2x_init_port(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int init_stage = port ? PORT1_STAGE : PORT0_STAGE;
	u32 low, high;
	u32 val;
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	int i;
#endif

	DP(BNX2X_MSG_MCP, "starting port init  port %d\n", port);

	REG_WR(bp, NIG_REG_MASK_INTERRUPT_PORT0 + port*4, 0);

	bnx2x_init_block(bp, PXP_BLOCK, init_stage);
	bnx2x_init_block(bp, PXP2_BLOCK, init_stage);

	bnx2x_init_block(bp, TCM_BLOCK, init_stage);
	bnx2x_init_block(bp, UCM_BLOCK, init_stage);
	bnx2x_init_block(bp, CCM_BLOCK, init_stage);
	bnx2x_init_block(bp, XCM_BLOCK, init_stage);

#ifdef BCM_CNIC
	REG_WR(bp, QM_REG_CONNNUM_0 + port*4, 1024/16 - 1);

	bnx2x_init_block(bp, TIMERS_BLOCK, init_stage);
	REG_WR(bp, TM_REG_LIN0_SCAN_TIME + port*4, 20);
	REG_WR(bp, TM_REG_LIN0_MAX_ACTIVE_CID + port*4, 31);
#endif

	bnx2x_init_block(bp, DQ_BLOCK, init_stage);

	bnx2x_init_block(bp, BRB1_BLOCK, init_stage);
	if (CHIP_REV_IS_SLOW(bp) && !CHIP_IS_E1H(bp)) {
		/* no pause for emulation and FPGA */
		low = 0;
		high = 513;
	} else {
		if (IS_E1HMF(bp))
			low = ((bp->flags & ONE_PORT_FLAG) ? 160 : 246);
		else if (bp->dev->mtu > 4096) {
			if (bp->flags & ONE_PORT_FLAG)
				low = 160;
			else {
				val = bp->dev->mtu;
				/* (24*1024 + val*4)/256 */
				low = 96 + (val/64) + ((val % 64) ? 1 : 0);
			}
		} else
			low = ((bp->flags & ONE_PORT_FLAG) ? 80 : 160);
		high = low + 56;	/* 14*1024/256 */
	}
	REG_WR(bp, BRB1_REG_PAUSE_LOW_THRESHOLD_0 + port*4, low);
	REG_WR(bp, BRB1_REG_PAUSE_HIGH_THRESHOLD_0 + port*4, high);

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	if (bp->flags & SAFC_TX_FLAG) {
		REG_WR(bp, BRB1_REG_HIGH_LLFC_LOW_THRESHOLD_0 + port*4, 0xa0);
		REG_WR(bp, BRB1_REG_HIGH_LLFC_HIGH_THRESHOLD_0 + port*4, 0xd8);
		REG_WR(bp, BRB1_REG_LOW_LLFC_LOW_THRESHOLD_0 + port*4, 0xa0);
		REG_WR(bp, BRB1_REG_LOW_LLFC_HIGH_THRESHOLD_0 + port*4, 0xd8);
	}
#endif

	bnx2x_init_block(bp, PRS_BLOCK, init_stage);

	bnx2x_init_block(bp, TSDM_BLOCK, init_stage);
	bnx2x_init_block(bp, CSDM_BLOCK, init_stage);
	bnx2x_init_block(bp, USDM_BLOCK, init_stage);
	bnx2x_init_block(bp, XSDM_BLOCK, init_stage);

	bnx2x_init_block(bp, TSEM_BLOCK, init_stage);
	bnx2x_init_block(bp, USEM_BLOCK, init_stage);
	bnx2x_init_block(bp, CSEM_BLOCK, init_stage);
	bnx2x_init_block(bp, XSEM_BLOCK, init_stage);

	bnx2x_init_block(bp, UPB_BLOCK, init_stage);
	bnx2x_init_block(bp, XPB_BLOCK, init_stage);

	bnx2x_init_block(bp, PBF_BLOCK, init_stage);

	/* configure PBF to work without PAUSE mtu 9000 */
	REG_WR(bp, PBF_REG_P0_PAUSE_ENABLE + port*4, 0);

	/* update threshold */
	REG_WR(bp, PBF_REG_P0_ARB_THRSH + port*4, (9040/16));
	/* update init credit */
	REG_WR(bp, PBF_REG_P0_INIT_CRD + port*4, (9040/16) + 553 - 22);

	/* probe changes */
	REG_WR(bp, PBF_REG_INIT_P0 + port*4, 1);
	msleep(5);
	REG_WR(bp, PBF_REG_INIT_P0 + port*4, 0);

#ifdef BCM_CNIC
	bnx2x_init_block(bp, SRCH_BLOCK, init_stage);
#endif
	bnx2x_init_block(bp, CDU_BLOCK, init_stage);
	bnx2x_init_block(bp, CFC_BLOCK, init_stage);

	if (CHIP_IS_E1(bp)) {
		REG_WR(bp, HC_REG_LEADING_EDGE_0 + port*8, 0);
		REG_WR(bp, HC_REG_TRAILING_EDGE_0 + port*8, 0);
	}
	bnx2x_init_block(bp, HC_BLOCK, init_stage);

	bnx2x_init_block(bp, MISC_AEU_BLOCK, init_stage);
	/* init aeu_mask_attn_func_0/1:
	 *  - SF mode: bits 3-7 are masked. only bits 0-2 are in use
	 *  - MF mode: bit 3 is masked. bits 0-2 are in use as in SF
	 *             bits 4-7 are used for "per vn group attention" */
	REG_WR(bp, MISC_REG_AEU_MASK_ATTN_FUNC_0 + port*4,
	       (IS_E1HMF(bp) ? 0xF7 : 0x7));

	bnx2x_init_block(bp, PXPCS_BLOCK, init_stage);
	bnx2x_init_block(bp, EMAC0_BLOCK, init_stage);
	bnx2x_init_block(bp, EMAC1_BLOCK, init_stage);
	bnx2x_init_block(bp, DBU_BLOCK, init_stage);
	bnx2x_init_block(bp, DBG_BLOCK, init_stage);

	bnx2x_init_block(bp, NIG_BLOCK, init_stage);

	REG_WR(bp, NIG_REG_XGXS_SERDES0_MODE_SEL + port*4, 1);

	if (CHIP_IS_E1H(bp)) {
		/* 0x2 disable e1hov, 0x1 enable */
		REG_WR(bp, NIG_REG_LLH0_BRB1_DRV_MASK_MF + port*4,
		       (IS_E1HMF(bp) ? 0x1 : 0x2));

#ifdef BNX2X_SAFC
		if (bp->flags & SAFC_TX_FLAG) {
			high = 0;
			for (i = 0; i < BNX2X_MAX_PRIORITY; i++)
				if (bp->pri_map[i] == 1)
					high |= (1 << i);
			REG_WR(bp,
			       NIG_REG_LLFC_HIGH_PRIORITY_CLASSES_0 + port*4,
			       high);
			low = 0;
			for (i = 0; i < BNX2X_MAX_PRIORITY; i++)
				if (bp->pri_map[i] == 0)
					low |= (1 << i);
			REG_WR(bp,
			       NIG_REG_LLFC_LOW_PRIORITY_CLASSES_0 + port*4,
			       low);

			REG_WR(bp, NIG_REG_PAUSE_ENABLE_0 + port*4, 0);
			REG_WR(bp, NIG_REG_LLFC_ENABLE_0 + port*4, 1);
			REG_WR(bp, NIG_REG_LLFC_OUT_EN_0 + port*4, 1);
		} else {
#else /* BNX2X_UPSTREAM */
		{
#endif
			REG_WR(bp, NIG_REG_LLFC_ENABLE_0 + port*4, 0);
			REG_WR(bp, NIG_REG_LLFC_OUT_EN_0 + port*4, 0);
			REG_WR(bp, NIG_REG_PAUSE_ENABLE_0 + port*4, 1);
		}
	}

	bnx2x_init_block(bp, MCP_BLOCK, init_stage);
	bnx2x_init_block(bp, DMAE_BLOCK, init_stage);

	switch (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config)) {
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8726:
		{
		u32 swap_val, swap_override, aeu_gpio_mask, offset;

		bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_3,
			       MISC_REGISTERS_GPIO_INPUT_HI_Z, port);

		/* The GPIO should be swapped if the swap register is
		   set and active */
		swap_val = REG_RD(bp, NIG_REG_PORT_SWAP);
		swap_override = REG_RD(bp, NIG_REG_STRAP_OVERRIDE);

		/* Select function upon port-swap configuration */
		if (port == 0) {
			offset = MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0;
			aeu_gpio_mask = (swap_val && swap_override) ?
				AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_1 :
				AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_0;
		} else {
			offset = MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0;
			aeu_gpio_mask = (swap_val && swap_override) ?
				AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_0 :
				AEU_INPUTS_ATTN_BITS_GPIO3_FUNCTION_1;
		}
		val = REG_RD(bp, offset);
		/* add GPIO3 to group */
		val |= aeu_gpio_mask;
		REG_WR(bp, offset, val);
		}
		break;

	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101:
	case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727:
		/* add SPIO 5 to group 0 */
		{
		u32 reg_addr = (port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0 :
				       MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0);
		val = REG_RD(bp, reg_addr);
		val |= AEU_INPUTS_ATTN_BITS_SPIO5;
		REG_WR(bp, reg_addr, val);
		}
		break;

	default:
		break;
	}

	bnx2x__link_reset(bp);

	return 0;
}

#define ILT_PER_FUNC		(768/2)
#define FUNC_ILT_BASE(func)	(func * ILT_PER_FUNC)
/* the phys address is shifted right 12 bits and has an added
   1=valid bit added to the 53rd bit
   then since this is a wide register(TM)
   we split it into two 32 bit writes
 */
#define ONCHIP_ADDR1(x)		((u32)(((u64)x >> 12) & 0xFFFFFFFF))
#define ONCHIP_ADDR2(x)		((u32)((1 << 20) | ((u64)x >> 44)))
#define PXP_ONE_ILT(x)		(((x) << 10) | x)
#define PXP_ILT_RANGE(f, l)	(((l) << 10) | f)

#ifdef BCM_CNIC
#define CNIC_ILT_LINES		127
#define CNIC_CTX_PER_ILT	16
#else
#define CNIC_ILT_LINES		0
#endif

static void bnx2x_ilt_wr(struct bnx2x *bp, u32 index, dma_addr_t addr)
{
	int reg;

	if (CHIP_IS_E1H(bp))
		reg = PXP2_REG_RQ_ONCHIP_AT_B0 + index*8;
	else /* E1 */
		reg = PXP2_REG_RQ_ONCHIP_AT + index*8;

	bnx2x_wb_wr(bp, reg, ONCHIP_ADDR1(addr), ONCHIP_ADDR2(addr));
}

static int bnx2x_init_func(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	u32 addr, val;
	int i;

	DP(BNX2X_MSG_MCP, "starting func init  func %d\n", func);

	/* set MSI reconfigure capability */
	addr = (port ? HC_REG_CONFIG_1 : HC_REG_CONFIG_0);
	val = REG_RD(bp, addr);
	val |= HC_CONFIG_0_REG_MSI_ATTN_EN_0;
	REG_WR(bp, addr, val);

	i = FUNC_ILT_BASE(func);

	bnx2x_ilt_wr(bp, i, bnx2x_sp_mapping(bp, context));
	if (CHIP_IS_E1H(bp)) {
		REG_WR(bp, PXP2_REG_RQ_CDU_FIRST_ILT, i);
		REG_WR(bp, PXP2_REG_RQ_CDU_LAST_ILT, i + CNIC_ILT_LINES);
	} else /* E1 */
		REG_WR(bp, PXP2_REG_PSWRQ_CDU0_L2P + func*4,
		       PXP_ILT_RANGE(i, i + CNIC_ILT_LINES));

#ifdef BCM_CNIC
	i += 1 + CNIC_ILT_LINES;
	bnx2x_ilt_wr(bp, i, bp->timers_mapping);
	if (CHIP_IS_E1(bp))
		REG_WR(bp, PXP2_REG_PSWRQ_TM0_L2P + func*4, PXP_ONE_ILT(i));
	else {
		REG_WR(bp, PXP2_REG_RQ_TM_FIRST_ILT, i);
		REG_WR(bp, PXP2_REG_RQ_TM_LAST_ILT, i);
	}

	i++;
	bnx2x_ilt_wr(bp, i, bp->qm_mapping);
	if (CHIP_IS_E1(bp))
		REG_WR(bp, PXP2_REG_PSWRQ_QM0_L2P + func*4, PXP_ONE_ILT(i));
	else {
		REG_WR(bp, PXP2_REG_RQ_QM_FIRST_ILT, i);
		REG_WR(bp, PXP2_REG_RQ_QM_LAST_ILT, i);
	}

	i++;
	bnx2x_ilt_wr(bp, i, bp->t1_mapping);
	if (CHIP_IS_E1(bp))
		REG_WR(bp, PXP2_REG_PSWRQ_SRC0_L2P + func*4, PXP_ONE_ILT(i));
	else {
		REG_WR(bp, PXP2_REG_RQ_SRC_FIRST_ILT, i);
		REG_WR(bp, PXP2_REG_RQ_SRC_LAST_ILT, i);
	}

	/* tell the searcher where the T2 table is */
	REG_WR(bp, SRC_REG_COUNTFREE0 + port*4, 16*1024/64);

	bnx2x_wb_wr(bp, SRC_REG_FIRSTFREE0 + port*16,
		    U64_LO(bp->t2_mapping), U64_HI(bp->t2_mapping));

	bnx2x_wb_wr(bp, SRC_REG_LASTFREE0 + port*16,
		    U64_LO((u64)bp->t2_mapping + 16*1024 - 64),
		    U64_HI((u64)bp->t2_mapping + 16*1024 - 64));

	REG_WR(bp, SRC_REG_NUMBER_HASH_BITS0 + port*4, 10);
#endif

	if (CHIP_IS_E1H(bp)) {
		bnx2x_init_block(bp, MISC_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, TCM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, UCM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, CCM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, XCM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, TSEM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, USEM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, CSEM_BLOCK, FUNC0_STAGE + func);
		bnx2x_init_block(bp, XSEM_BLOCK, FUNC0_STAGE + func);

		REG_WR(bp, NIG_REG_LLH0_FUNC_EN + port*8, 1);
		REG_WR(bp, NIG_REG_LLH0_FUNC_VLAN_ID + port*8, bp->e1hov);
	}

	/* HC init per function */
	if (CHIP_IS_E1H(bp)) {
		REG_WR(bp, MISC_REG_AEU_GENERAL_ATTN_12 + func*4, 0);

		REG_WR(bp, HC_REG_LEADING_EDGE_0 + port*8, 0);
		REG_WR(bp, HC_REG_TRAILING_EDGE_0 + port*8, 0);
	}
	bnx2x_init_block(bp, HC_BLOCK, FUNC0_STAGE + func);

	/* Reset PCIE errors for debug */
	REG_WR(bp, 0x2114, 0xffffffff);
	REG_WR(bp, 0x2120, 0xffffffff);

	return 0;
}

static int bnx2x_init_hw(struct bnx2x *bp, u32 load_code)
{
	int i, rc = 0;

	DP(BNX2X_MSG_MCP, "function %d  load_code %x\n",
	   BP_FUNC(bp), load_code);

	bp->dmae_ready = 0;
	mutex_init(&bp->dmae_mutex);
	rc = bnx2x_gunzip_init(bp);
	if (rc)
		return rc;

	switch (load_code) {
	case FW_MSG_CODE_DRV_LOAD_COMMON:
		rc = bnx2x_init_common(bp);
		if (rc)
			goto init_hw_err;
		/* no break */

	case FW_MSG_CODE_DRV_LOAD_PORT:
		bp->dmae_ready = 1;
		rc = bnx2x_init_port(bp);
		if (rc)
			goto init_hw_err;
		/* no break */

	case FW_MSG_CODE_DRV_LOAD_FUNCTION:
		bp->dmae_ready = 1;
		rc = bnx2x_init_func(bp);
		if (rc)
			goto init_hw_err;
		break;

	default:
		BNX2X_ERR("Unknown load_code (0x%x) from MCP\n", load_code);
		break;
	}

	if (!BP_NOMCP(bp)) {
		int func = BP_FUNC(bp);

		bp->fw_drv_pulse_wr_seq =
				(SHMEM_RD(bp, func_mb[func].drv_pulse_mb) &
				 DRV_PULSE_SEQ_MASK);
		DP(BNX2X_MSG_MCP, "drv_pulse 0x%x\n", bp->fw_drv_pulse_wr_seq);
	}

	/* this needs to be done before gunzip end */
	bnx2x_zero_def_sb(bp);
	for_each_queue(bp, i)
		bnx2x_zero_sb(bp, BP_L_ID(bp) + i);
#ifdef BCM_CNIC
	bnx2x_zero_sb(bp, BP_L_ID(bp) + i);
#endif

init_hw_err:
	bnx2x_gunzip_end(bp);

	return rc;
}

static void bnx2x_free_mem(struct bnx2x *bp)
{

#define BNX2X_PCI_FREE(x, y, size) \
	do { \
		if (x) { \
			pci_free_consistent(bp->pdev, size, x, y); \
			x = NULL; \
			y = 0; \
		} \
	} while (0)

#define BNX2X_FREE(x) \
	do { \
		if (x) { \
			vfree(x); \
			x = NULL; \
		} \
	} while (0)

	int i;

	/* fastpath */
	/* Common */
	for_each_queue(bp, i) {

		/* status blocks */
		BNX2X_PCI_FREE(bnx2x_fp(bp, i, status_blk),
			       bnx2x_fp(bp, i, status_blk_mapping),
			       sizeof(struct host_status_block));
	}
	/* Rx */
	for_each_queue(bp, i) {

		/* fastpath rx rings: rx_buf rx_desc rx_comp */
		BNX2X_FREE(bnx2x_fp(bp, i, rx_buf_ring));
		BNX2X_PCI_FREE(bnx2x_fp(bp, i, rx_desc_ring),
			       bnx2x_fp(bp, i, rx_desc_mapping),
			       sizeof(struct eth_rx_bd) * NUM_RX_BD);

		BNX2X_PCI_FREE(bnx2x_fp(bp, i, rx_comp_ring),
			       bnx2x_fp(bp, i, rx_comp_mapping),
			       sizeof(struct eth_fast_path_rx_cqe) *
			       NUM_RCQ_BD);

		/* SGE ring */
		BNX2X_FREE(bnx2x_fp(bp, i, rx_page_ring));
		BNX2X_PCI_FREE(bnx2x_fp(bp, i, rx_sge_ring),
			       bnx2x_fp(bp, i, rx_sge_mapping),
			       BCM_PAGE_SIZE * NUM_RX_SGE_PAGES);
	}
	/* Tx */
	for_each_queue(bp, i) {

		/* fastpath tx rings: tx_buf tx_desc */
		BNX2X_FREE(bnx2x_fp(bp, i, tx_buf_ring));
		BNX2X_PCI_FREE(bnx2x_fp(bp, i, tx_desc_ring),
			       bnx2x_fp(bp, i, tx_desc_mapping),
			       sizeof(union eth_tx_bd_types) * NUM_TX_BD);
	}
	/* end of fastpath */

	BNX2X_PCI_FREE(bp->def_status_blk, bp->def_status_blk_mapping,
		       sizeof(struct host_def_status_block));

	BNX2X_PCI_FREE(bp->slowpath, bp->slowpath_mapping,
		       sizeof(struct bnx2x_slowpath));

#ifdef BCM_CNIC
	BNX2X_PCI_FREE(bp->t1, bp->t1_mapping, 64*1024);
	BNX2X_PCI_FREE(bp->t2, bp->t2_mapping, 16*1024);
	BNX2X_PCI_FREE(bp->timers, bp->timers_mapping, 8*1024);
	BNX2X_PCI_FREE(bp->qm, bp->qm_mapping, 128*1024);
	BNX2X_PCI_FREE(bp->cnic_sb, bp->cnic_sb_mapping,
		       sizeof(struct host_status_block));
#endif
	BNX2X_PCI_FREE(bp->spq, bp->spq_mapping, BCM_PAGE_SIZE);

#undef BNX2X_PCI_FREE
#undef BNX2X_KFREE
}

static int bnx2x_alloc_mem(struct bnx2x *bp)
{

#define BNX2X_PCI_ALLOC(x, y, size) \
	do { \
		x = pci_alloc_consistent(bp->pdev, size, y); \
		if (x == NULL) \
			goto alloc_mem_err; \
		memset(x, 0, size); \
	} while (0)

#define BNX2X_ALLOC(x, size) \
	do { \
		x = vmalloc(size); \
		if (x == NULL) \
			goto alloc_mem_err; \
		memset(x, 0, size); \
	} while (0)

	int i;

	/* fastpath */
	/* Common */
	for_each_queue(bp, i) {
		bnx2x_fp(bp, i, bp) = bp;

		/* status blocks */
		BNX2X_PCI_ALLOC(bnx2x_fp(bp, i, status_blk),
				&bnx2x_fp(bp, i, status_blk_mapping),
				sizeof(struct host_status_block));
	}
	/* Rx */
	for_each_queue(bp, i) {

		/* fastpath rx rings: rx_buf rx_desc rx_comp */
		BNX2X_ALLOC(bnx2x_fp(bp, i, rx_buf_ring),
				sizeof(struct sw_rx_bd) * NUM_RX_BD);
		BNX2X_PCI_ALLOC(bnx2x_fp(bp, i, rx_desc_ring),
				&bnx2x_fp(bp, i, rx_desc_mapping),
				sizeof(struct eth_rx_bd) * NUM_RX_BD);

		BNX2X_PCI_ALLOC(bnx2x_fp(bp, i, rx_comp_ring),
				&bnx2x_fp(bp, i, rx_comp_mapping),
				sizeof(struct eth_fast_path_rx_cqe) *
				NUM_RCQ_BD);

		/* SGE ring */
		BNX2X_ALLOC(bnx2x_fp(bp, i, rx_page_ring),
				sizeof(struct sw_rx_page) * NUM_RX_SGE);
		BNX2X_PCI_ALLOC(bnx2x_fp(bp, i, rx_sge_ring),
				&bnx2x_fp(bp, i, rx_sge_mapping),
				BCM_PAGE_SIZE * NUM_RX_SGE_PAGES);
	}
	/* Tx */
	for_each_queue(bp, i) {

		/* fastpath tx rings: tx_buf tx_desc */
		BNX2X_ALLOC(bnx2x_fp(bp, i, tx_buf_ring),
				sizeof(struct sw_tx_bd) * NUM_TX_BD);
		BNX2X_PCI_ALLOC(bnx2x_fp(bp, i, tx_desc_ring),
				&bnx2x_fp(bp, i, tx_desc_mapping),
				sizeof(union eth_tx_bd_types) * NUM_TX_BD);
	}
	/* end of fastpath */

	BNX2X_PCI_ALLOC(bp->def_status_blk, &bp->def_status_blk_mapping,
			sizeof(struct host_def_status_block));

	BNX2X_PCI_ALLOC(bp->slowpath, &bp->slowpath_mapping,
			sizeof(struct bnx2x_slowpath));

#ifdef BCM_CNIC
	BNX2X_PCI_ALLOC(bp->t1, &bp->t1_mapping, 64*1024);

	/* allocate searcher T2 table
	   we allocate 1/4 of alloc num for T2
	  (which is not entered into the ILT) */
	BNX2X_PCI_ALLOC(bp->t2, &bp->t2_mapping, 16*1024);

	/* Initialize T2 (for 1024 connections) */
	for (i = 0; i < 16*1024; i += 64)
		*(u64 *)((char *)bp->t2 + i + 56) = bp->t2_mapping + i + 64;

	/* Timer block array (8*MAX_CONN) phys uncached for now 1024 conns */
	BNX2X_PCI_ALLOC(bp->timers, &bp->timers_mapping, 8*1024);

	/* QM queues (128*MAX_CONN) */
	BNX2X_PCI_ALLOC(bp->qm, &bp->qm_mapping, 128*1024);

	BNX2X_PCI_ALLOC(bp->cnic_sb, &bp->cnic_sb_mapping,
			sizeof(struct host_status_block));
#endif

	/* Slow path ring */
	BNX2X_PCI_ALLOC(bp->spq, &bp->spq_mapping, BCM_PAGE_SIZE);

	return 0;

alloc_mem_err:
	bnx2x_free_mem(bp);
	return -ENOMEM;

#undef BNX2X_PCI_ALLOC
#undef BNX2X_ALLOC
}

static void bnx2x_free_tx_skbs(struct bnx2x *bp)
{
	int i;

	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		u16 bd_cons = fp->tx_bd_cons;
		u16 sw_prod = fp->tx_pkt_prod;
		u16 sw_cons = fp->tx_pkt_cons;

		while (sw_cons != sw_prod) {
			bd_cons = bnx2x_free_tx_pkt(bp, fp, TX_BD(sw_cons));
			sw_cons++;
		}
	}
}

static void bnx2x_free_rx_skbs(struct bnx2x *bp)
{
	int i, j;

	for_each_queue(bp, j) {
		struct bnx2x_fastpath *fp = &bp->fp[j];

		for (i = 0; i < NUM_RX_BD; i++) {
			struct sw_rx_bd *rx_buf = &fp->rx_buf_ring[i];
			struct sk_buff *skb = rx_buf->skb;

			if (skb == NULL)
				continue;

			pci_unmap_single(bp->pdev,
					 pci_unmap_addr(rx_buf, mapping),
					 bp->rx_buf_size, PCI_DMA_FROMDEVICE);

			rx_buf->skb = NULL;
			dev_kfree_skb(skb);
		}
		if (!fp->disable_tpa)
			bnx2x_free_tpa_pool(bp, fp, CHIP_IS_E1(bp) ?
					    ETH_MAX_AGGREGATION_QUEUES_E1 :
					    ETH_MAX_AGGREGATION_QUEUES_E1H);
	}
}

static void bnx2x_free_skbs(struct bnx2x *bp)
{
	bnx2x_free_tx_skbs(bp);
	bnx2x_free_rx_skbs(bp);
}

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
static void bnx2x_free_msix_irqs(struct bnx2x *bp)
{
	int i, offset = 1;

	free_irq(bp->msix_table[0].vector, bp->dev);
	DP(NETIF_MSG_IFDOWN, "released sp irq (%d)\n",
	   bp->msix_table[0].vector);

#ifdef BCM_CNIC
	offset++;
#endif
	for_each_queue(bp, i) {
		DP(NETIF_MSG_IFDOWN, "about to release fp #%d->%d irq  "
		   "state %x\n", i, bp->msix_table[i + offset].vector,
		   bnx2x_fp(bp, i, state));

		free_irq(bp->msix_table[i + offset].vector, &bp->fp[i]);
	}
}
#endif

static void bnx2x_free_irq(struct bnx2x *bp)
{
#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	if (bp->flags & USING_MSIX_FLAG) {
		bnx2x_free_msix_irqs(bp);
		pci_disable_msix(bp->pdev);
		bp->flags &= ~USING_MSIX_FLAG;

	} else if (bp->flags & USING_MSI_FLAG) {
		free_irq(bp->pdev->irq, bp->dev);
		pci_disable_msi(bp->pdev);
		bp->flags &= ~USING_MSI_FLAG;

	} else
#endif
		free_irq(bp->pdev->irq, bp->dev);
}

static int bnx2x_enable_msix(struct bnx2x *bp)
{
#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	int i, rc, offset = 1;
	int igu_vec = 0;

	bp->msix_table[0].entry = igu_vec;
	DP(NETIF_MSG_IFUP, "msix_table[0].entry = %d (slowpath)\n", igu_vec);

#ifdef BCM_CNIC
	igu_vec = BP_L_ID(bp) + offset;
	bp->msix_table[1].entry = igu_vec;
	DP(NETIF_MSG_IFUP, "msix_table[1].entry = %d (CNIC)\n", igu_vec);
	offset++;
#endif
	for_each_queue(bp, i) {
		igu_vec = BP_L_ID(bp) + offset + i;
		bp->msix_table[i + offset].entry = igu_vec;
		DP(NETIF_MSG_IFUP, "msix_table[%d].entry = %d "
		   "(fastpath #%u)\n", i + offset, igu_vec, i);
	}

	rc = pci_enable_msix(bp->pdev, &bp->msix_table[0],
			     BNX2X_NUM_QUEUES(bp) + offset);

	/* 
	 * reconfigure number of tx/rx queues according to available
	 * MSI-X vectors
	 */
	if (rc >= BNX2X_MIN_MSIX_VEC_CNT) {
		/* vectors available for FP */
		int fp_vec = rc - BNX2X_MSIX_VEC_FP_START;

		DP(NETIF_MSG_IFUP,
		   "Trying to use less MSI-X vectors: %d\n", rc);

		rc = pci_enable_msix(bp->pdev, &bp->msix_table[0], rc);

		if (rc) {
			DP(NETIF_MSG_IFUP,
			   "MSI-X is not attainable  rc %d\n", rc);
			return rc;
		}

		bp->num_queues = min(bp->num_queues, fp_vec);

		DP(NETIF_MSG_IFUP, "New queue configuration set: %d\n",
				  bp->num_queues);
	} else if (rc) {
		DP(NETIF_MSG_IFUP, "MSI-X is not attainable  rc %d\n", rc);
		return rc;
	}

	bp->flags |= USING_MSIX_FLAG;

	return 0;
#else
	return -1;
#endif
}

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
static int bnx2x_req_msix_irqs(struct bnx2x *bp)
{
	int i, rc, offset = 1;

	rc = request_irq(bp->msix_table[0].vector, bnx2x_msix_sp_int, 0,
			 bp->dev->name, bp->dev);
	if (rc) {
		BNX2X_ERR("request sp irq failed\n");
		return -EBUSY;
	}

#ifdef BCM_CNIC
	offset++;
#endif
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];
		snprintf(fp->name, sizeof(fp->name), "%s-fp-%d",
			 bp->dev->name, i);

		rc = request_irq(bp->msix_table[i + offset].vector,
				 bnx2x_msix_fp_int, 0, fp->name, fp);
		if (rc) {
			BNX2X_ERR("request fp #%d irq failed  rc %d\n", i, rc);
			bnx2x_free_msix_irqs(bp);
			return -EBUSY;
		}

		fp->state = BNX2X_FP_STATE_IRQ;
	}

	i = BNX2X_NUM_QUEUES(bp);
	printk(KERN_INFO PFX "%s: using MSI-X  IRQs: sp %d  fp[%d] %d"
	       " ... fp[%d] %d\n",
	       bp->dev->name, bp->msix_table[0].vector,
	       0, bp->msix_table[offset].vector,
	       i - 1, bp->msix_table[offset + i - 1].vector);

	return 0;
}

static int bnx2x_enable_msi(struct bnx2x *bp)
{
	int rc;

	rc = pci_enable_msi(bp->pdev);
	if (rc) {
		DP(NETIF_MSG_IFUP, "MSI is not attainable\n");
		return -1;
	}
	bp->flags |= USING_MSI_FLAG;

	return 0;
}
#endif

static int bnx2x_req_irq(struct bnx2x *bp)
{
	unsigned long flags;
	int rc;

	if (bp->flags & USING_MSI_FLAG)
		flags = 0;
	else
		flags = IRQF_SHARED;

	rc = request_irq(bp->pdev->irq, bnx2x_interrupt, flags,
			 bp->dev->name, bp->dev);
	if (!rc)
		bnx2x_fp(bp, 0, state) = BNX2X_FP_STATE_IRQ;

	return rc;
}

static void bnx2x_napi_enable(struct bnx2x *bp)
{
	int i;

	for_each_queue(bp, i)
#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
		napi_enable(&bnx2x_fp(bp, i, napi));
#else
		netif_poll_enable(&bnx2x_fp(bp, i, dummy_netdev));
#endif
}

static void bnx2x_napi_disable(struct bnx2x *bp)
{
	int i;

	for_each_queue(bp, i)
#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
		napi_disable(&bnx2x_fp(bp, i, napi));
#else
		netif_poll_disable(&bnx2x_fp(bp, i, dummy_netdev));
#endif
}

static void bnx2x_netif_start(struct bnx2x *bp)
{
	int intr_sem;

	intr_sem = atomic_dec_and_test(&bp->intr_sem);
	smp_wmb(); /* Ensure that bp->intr_sem update is SMP-safe */

	if (intr_sem) {
		if (netif_running(bp->dev)) {
			bnx2x_napi_enable(bp);
			bnx2x_int_enable(bp);
			if (bp->state == BNX2X_STATE_OPEN)
				netif_tx_wake_all_queues(bp->dev);
		}
	}
}

static void bnx2x_netif_stop(struct bnx2x *bp, int disable_hw)
{
	bnx2x_int_disable_sync(bp, disable_hw);
#if defined(BNX2X_NEW_NAPI) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
	bnx2x_napi_disable(bp);
#else
	if (netif_running(bp->dev))
		bnx2x_napi_disable(bp);
#endif
	netif_tx_disable(bp->dev);
#if (LINUX_VERSION_CODE < 0x02061f) /* ! BNX2X_UPSTREAM */
	/* In kernels starting from 2.6.31 netdev layer does this */
	bp->dev->trans_start = jiffies;	/* prevent tx timeout */
#endif
}

/*
 * Init service functions
 */

/**
 * Sets a MAC in a CAM for a few L2 Clients for E1 chip
 *
 * @param bp driver descriptor
 * @param set set or clear an entry (1 or 0)
 * @param mac pointer to a buffer containing a MAC
 * @param cl_bit_vec bit vector of clients to register a MAC for
 * @param cam_offset offset in a CAM to use
 * @param with_bcast set broadcast MAC as well
 */
static void bnx2x_set_mac_addr_e1_gen(struct bnx2x *bp, int set, u8 *mac,
				      u32 cl_bit_vec, u8 cam_offset,
				      u8 with_bcast)
{
	struct mac_configuration_cmd *config = bnx2x_sp(bp, mac_config);
	int port = BP_PORT(bp);

	/* CAM allocation
	 * unicasts 0-31:port0 32-63:port1
	 * multicast 64-127:port0 128-191:port1
	 */
	config->hdr.length = 1 + (with_bcast ? 1 : 0);
	config->hdr.offset = cam_offset;
	config->hdr.client_id = 0xff;
	config->hdr.reserved1 = 0;

	/* primary MAC */
	config->config_table[0].cam_entry.msb_mac_addr =
					swab16(*(u16 *)&mac[0]);
	config->config_table[0].cam_entry.middle_mac_addr =
					swab16(*(u16 *)&mac[2]);
	config->config_table[0].cam_entry.lsb_mac_addr =
					swab16(*(u16 *)&mac[4]);
	config->config_table[0].cam_entry.flags = cpu_to_le16(port);
	if (set)
		config->config_table[0].target_table_entry.flags = 0;
	else
		CAM_INVALIDATE(config->config_table[0]);
	config->config_table[0].target_table_entry.clients_bit_vector =
						cpu_to_le32(cl_bit_vec);
	config->config_table[0].target_table_entry.vlan_id = 0;

	DP(NETIF_MSG_IFUP, "%s MAC (%04x:%04x:%04x)\n",
	   (set ? "setting" : "clearing"),
	   config->config_table[0].cam_entry.msb_mac_addr,
	   config->config_table[0].cam_entry.middle_mac_addr,
	   config->config_table[0].cam_entry.lsb_mac_addr);

	/* broadcast */
	if (with_bcast) {
		config->config_table[1].cam_entry.msb_mac_addr =
			cpu_to_le16(0xffff);
		config->config_table[1].cam_entry.middle_mac_addr =
			cpu_to_le16(0xffff);
		config->config_table[1].cam_entry.lsb_mac_addr =
			cpu_to_le16(0xffff);
		config->config_table[1].cam_entry.flags = cpu_to_le16(port);
		if (set)
			config->config_table[1].target_table_entry.flags =
					TSTORM_CAM_TARGET_TABLE_ENTRY_BROADCAST;
		else
			CAM_INVALIDATE(config->config_table[1]);
		config->config_table[1].target_table_entry.clients_bit_vector =
							cpu_to_le32(cl_bit_vec);
		config->config_table[1].target_table_entry.vlan_id = 0;
	}

	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_SET_MAC, 0,
		      U64_HI(bnx2x_sp_mapping(bp, mac_config)),
		      U64_LO(bnx2x_sp_mapping(bp, mac_config)), 0);
}

/**
 * Sets a MAC in a CAM for a few L2 Clients for E1H chip
 *
 * @param bp driver descriptor
 * @param set set or clear an entry (1 or 0)
 * @param mac pointer to a buffer containing a MAC
 * @param cl_bit_vec bit vector of clients to register a MAC for
 * @param cam_offset offset in a CAM to use
 */
static void bnx2x_set_mac_addr_e1h_gen(struct bnx2x *bp, int set, u8 *mac,
				       u32 cl_bit_vec, u8 cam_offset)
{
	struct mac_configuration_cmd_e1h *config =
		(struct mac_configuration_cmd_e1h *)bnx2x_sp(bp, mac_config);

	config->hdr.length = 1;
	config->hdr.offset = cam_offset;
	config->hdr.client_id = 0xff;
	config->hdr.reserved1 = 0;

	/* primary MAC */
	config->config_table[0].msb_mac_addr =
					swab16(*(u16 *)&mac[0]);
	config->config_table[0].middle_mac_addr =
					swab16(*(u16 *)&mac[2]);
	config->config_table[0].lsb_mac_addr =
					swab16(*(u16 *)&mac[4]);
	config->config_table[0].clients_bit_vector =
					cpu_to_le32(cl_bit_vec);
	config->config_table[0].vlan_id = 0;
	config->config_table[0].e1hov_id = cpu_to_le16(bp->e1hov);
	if (set)
		config->config_table[0].flags = BP_PORT(bp);
	else
		config->config_table[0].flags =
				MAC_CONFIGURATION_ENTRY_E1H_ACTION_TYPE;

	DP(NETIF_MSG_IFUP, "%s MAC (%04x:%04x:%04x)  E1HOV %d  CLID mask %d\n",
	   (set ? "setting" : "clearing"),
	   config->config_table[0].msb_mac_addr,
	   config->config_table[0].middle_mac_addr,
	   config->config_table[0].lsb_mac_addr, bp->e1hov, cl_bit_vec);

	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_SET_MAC, 0,
		      U64_HI(bnx2x_sp_mapping(bp, mac_config)),
		      U64_LO(bnx2x_sp_mapping(bp, mac_config)), 0);
}

static int bnx2x_wait_ramrod(struct bnx2x *bp, int state, int idx,
			     int *state_p, int poll)
{
	/* can take a while if any port is running */
	int cnt = 5000;

	DP(NETIF_MSG_IFUP, "%s for state to become %x on IDX [%d]\n",
	   poll ? "polling" : "waiting", state, idx);

	might_sleep();
	while (cnt--) {
		if (poll) {
			bnx2x_rx_int(bp->fp, 10);
			/* if index is different from 0
			 * the reply for some commands will
			 * be on the non default queue
			 */
			if (idx)
				bnx2x_rx_int(&bp->fp[idx], 10);
		}

		mb(); /* state is changed by bnx2x_sp_event() */
		if (*state_p == state) {
#ifdef BNX2X_STOP_ON_ERROR
			DP(NETIF_MSG_IFUP, "exit  (cnt %d)\n", 5000 - cnt);
#endif
			return 0;
		}

		msleep(1);

		if (bp->panic)
			return -EIO;
	}

	/* timeout! */
	BNX2X_ERR("timeout %s for state %x on IDX [%d]\n",
		  poll ? "polling" : "waiting", state, idx);
#ifdef BNX2X_STOP_ON_ERROR
	bnx2x_panic();
#endif

	return -EBUSY;
}

static void bnx2x_set_eth_mac_addr_e1h(struct bnx2x *bp, int set)
{
	bp->set_mac_pending++;
	smp_wmb();

	bnx2x_set_mac_addr_e1h_gen(bp, set, bp->dev->dev_addr,
				   (1 << bp->fp->cl_id), BP_FUNC(bp));

	/* Wait for a completion */
	bnx2x_wait_ramrod(bp, 0, 0, &bp->set_mac_pending, set ? 0 : 1);
}

static void bnx2x_set_eth_mac_addr_e1(struct bnx2x *bp, int set)
{
	bp->set_mac_pending++;
	smp_wmb();

	bnx2x_set_mac_addr_e1_gen(bp, set, bp->dev->dev_addr,
				  (1 << bp->fp->cl_id), (BP_PORT(bp) ? 32 : 0),
				  1);

	/* Wait for a completion */
	bnx2x_wait_ramrod(bp, 0, 0, &bp->set_mac_pending, set ? 0 : 1);
}

#ifdef BCM_CNIC
/**
 * Set iSCSI MAC(s) at the next enties in the CAM after the ETH
 * MAC(s). This function will wait until the ramdord completion
 * returns.
 *
 * @param bp driver handle
 * @param set set or clear the CAM entry
 *
 * @return 0 if cussess, -ENODEV if ramrod doesn't return.
 */
int bnx2x_set_iscsi_eth_mac_addr(struct bnx2x *bp, int set)
{
	u32 cl_bit_vec = (1 << BCM_ISCSI_ETH_CL_ID);

	bp->set_mac_pending++;
	smp_wmb();

	/* Send a SET_MAC ramrod */
	if (CHIP_IS_E1(bp))
		bnx2x_set_mac_addr_e1_gen(bp, set, bp->iscsi_mac,
				  cl_bit_vec, (BP_PORT(bp) ? 32 : 0) + 2,
				  1);
	else
		/* CAM allocation for E1H
		* unicasts: by func number
		* multicast: 20+FUNC*20, 20 each
		*/
		bnx2x_set_mac_addr_e1h_gen(bp, set, bp->iscsi_mac,
				   cl_bit_vec, E1H_FUNC_MAX + BP_FUNC(bp));

	/* Wait for a completion when setting */
	bnx2x_wait_ramrod(bp, 0, 0, &bp->set_mac_pending, set ? 0 : 1);

	return 0;
}

EXPORT_SYMBOL(bnx2x_set_iscsi_eth_mac_addr);
#endif

static int bnx2x_setup_leading(struct bnx2x *bp)
{
	int rc;

	/* reset IGU state */
	bnx2x_ack_sb(bp, bp->fp[0].sb_id, CSTORM_ID, 0, IGU_INT_ENABLE, 0);

	/* SETUP ramrod */
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_PORT_SETUP, 0, 0, 0, 0);

	/* Wait for completion */
	rc = bnx2x_wait_ramrod(bp, BNX2X_STATE_OPEN, 0, &(bp->state), 0);

	return rc;
}

static int bnx2x_setup_multi(struct bnx2x *bp, int index)
{
	struct bnx2x_fastpath *fp = &bp->fp[index];

	/* reset IGU state */
	bnx2x_ack_sb(bp, fp->sb_id, CSTORM_ID, 0, IGU_INT_ENABLE, 0);

	/* SETUP ramrod */
	fp->state = BNX2X_FP_STATE_OPENING;
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_CLIENT_SETUP, index, 0,
		      fp->cl_id, 0);

	/* Wait for completion */
	return bnx2x_wait_ramrod(bp, BNX2X_FP_STATE_OPEN, index,
				 &(fp->state), 0);
}

#if defined(BNX2X_NEW_NAPI) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
static int bnx2x_poll(struct napi_struct *napi, int budget);
#else
static int bnx2x_poll(struct net_device *dev, int *budget);
#endif

static void bnx2x_set_num_queues_msix(struct bnx2x *bp)
{
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	int i;
#endif

	switch (bp->multi_mode) {
	case ETH_RSS_MODE_DISABLED:
		bp->num_queues = 1;
		break;

	case ETH_RSS_MODE_REGULAR:
		if (num_queues)
			bp->num_queues = min_t(u32, num_queues,
						  BNX2X_MAX_QUEUES(bp));
		else
			bp->num_queues = min_t(u32, num_online_cpus(),
						  BNX2X_MAX_QUEUES(bp));
		break;

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	case ETH_RSS_MODE_VLAN_PRI:
	case ETH_RSS_MODE_E1HOV_PRI:
	case ETH_RSS_MODE_IP_DSCP:
		bp->num_queues = 0;
		for (i = 0; i < BNX2X_MAX_COS; i++)
			bp->num_queues += bp->qs_per_cos[i];
		break;
#endif

	default:
		bp->num_queues = 1;
		break;
	}
}

static int bnx2x_set_num_queues(struct bnx2x *bp)
{
	int rc = 0;

	switch (int_mode) {
	case INT_MODE_INTx:
	case INT_MODE_MSI:
		bp->num_queues = 1;
		DP(NETIF_MSG_IFUP, "set number of queues to 1\n");
		break;
	default:
		/* Set number of queues according to bp->multi_mode value */
		bnx2x_set_num_queues_msix(bp);

		DP(NETIF_MSG_IFUP, "set number of queues to %d\n",
		   bp->num_queues);

		/* if we can't use MSI-X we only need one fp,
		 * so try to enable MSI-X with the requested number of fp's
		 * and fallback to MSI or legacy INTx with one fp
		 */
		rc = bnx2x_enable_msix(bp);
		if (rc)
			/* failed to enable MSI-X */
			bp->num_queues = 1;
		break;
	}
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	bp->dev->real_num_tx_queues = bp->num_queues;
#endif
	return rc;
}

#ifdef BCM_CNIC
static int bnx2x_cnic_notify(struct bnx2x *bp, int cmd);
static void bnx2x_setup_cnic_irq_info(struct bnx2x *bp);
#endif

/* must be called with rtnl_lock */
static int bnx2x_nic_load(struct bnx2x *bp, int load_mode)
{
	u32 load_code;
	int i, rc;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return -EPERM;
#endif

	bp->state = BNX2X_STATE_OPENING_WAIT4_LOAD;

	rc = bnx2x_set_num_queues(bp);

	if (bnx2x_alloc_mem(bp))
		return -ENOMEM;

	for_each_queue(bp, i)
		bnx2x_fp(bp, i, disable_tpa) =
					((bp->flags & TPA_ENABLE_FLAG) == 0);

#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
	for_each_queue(bp, i)
#ifdef __VMKLNX__
		if (!bnx2x_fp(bp, i, napi).worldlet)
#endif /* __VMKLNX__ */
		netif_napi_add(bp->dev, &bnx2x_fp(bp, i, napi),
			       bnx2x_poll, 128);
#else /* ! BNX2X_UPSTREAM */
	/* initialize net_device for each rx queue */
	for_each_queue(bp, i) {
		struct net_device *dummy_netdev = &bnx2x_fp(bp, i, dummy_netdev);
		dummy_netdev->priv = &bp->fp[i];
#if defined(USE_NAPI_GRO)
		netif_napi_add(dummy_netdev, &bnx2x_fp(bp, i, napi),
			       bnx2x_poll, 128);
#else
		dummy_netdev->poll = bnx2x_poll;
		dummy_netdev->weight = 128;
		set_bit(__LINK_STATE_START, &dummy_netdev->state);
#endif
	}

#endif

	bnx2x_napi_enable(bp);

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	if (bp->flags & USING_MSIX_FLAG) {
		rc = bnx2x_req_msix_irqs(bp);
		if (rc) {
			pci_disable_msix(bp->pdev);
			goto load_error1;
		}
	} else {
		/* Fall to INTx if failed to enable MSI-X due to lack of
		   memory (in bnx2x_set_num_queues()) */
		if ((rc != -ENOMEM) && (int_mode != INT_MODE_INTx))
			bnx2x_enable_msi(bp);
#else
	{
#endif
		bnx2x_ack_int(bp);
		rc = bnx2x_req_irq(bp);
		if (rc) {
			BNX2X_ERR("IRQ request failed  rc %d, aborting\n", rc);
#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
			if (bp->flags & USING_MSI_FLAG)
				pci_disable_msi(bp->pdev);
#endif
			goto load_error1;
		}
		if (bp->flags & USING_MSI_FLAG) {
			bp->dev->irq = bp->pdev->irq;
			printk(KERN_INFO PFX "%s: using MSI  IRQ %d\n",
			       bp->dev->name, bp->pdev->irq);
		}
	}

	/* Send LOAD_REQUEST command to MCP
	   Returns the type of LOAD command:
	   if it is the first port to be initialized
	   common blocks should be initialized, otherwise - not
	*/
	if (!BP_NOMCP(bp)) {
		load_code = bnx2x_fw_command(bp, DRV_MSG_CODE_LOAD_REQ);
		if (!load_code) {
			BNX2X_ERR("MCP response failure, aborting\n");
			rc = -EBUSY;
			goto load_error2;
		}
		if (load_code == FW_MSG_CODE_DRV_LOAD_REFUSED) {
			rc = -EBUSY; /* other port in diagnostic mode */
			goto load_error2;
		}

	} else {
		int port = BP_PORT(bp);

		DP(NETIF_MSG_IFUP, "NO MCP - load counts      %d, %d, %d\n",
		   load_count[0], load_count[1], load_count[2]);
		load_count[0]++;
		load_count[1 + port]++;
		DP(NETIF_MSG_IFUP, "NO MCP - new load counts  %d, %d, %d\n",
		   load_count[0], load_count[1], load_count[2]);
		if (load_count[0] == 1)
			load_code = FW_MSG_CODE_DRV_LOAD_COMMON;
		else if (load_count[1 + port] == 1)
			load_code = FW_MSG_CODE_DRV_LOAD_PORT;
		else
			load_code = FW_MSG_CODE_DRV_LOAD_FUNCTION;
	}

	if ((load_code == FW_MSG_CODE_DRV_LOAD_COMMON) ||
	    (load_code == FW_MSG_CODE_DRV_LOAD_PORT))
		bp->port.pmf = 1;
	else
		bp->port.pmf = 0;
	DP(NETIF_MSG_LINK, "pmf %d\n", bp->port.pmf);

	/* Initialize HW */
	rc = bnx2x_init_hw(bp, load_code);
	if (rc) {
		BNX2X_ERR("HW init failed, aborting\n");
		bnx2x_fw_command(bp, DRV_MSG_CODE_LOAD_DONE);
		bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_REQ_WOL_MCP);
		bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_DONE);
		goto load_error2;
	}

	/* Setup NIC internals and enable interrupts */
	bnx2x_nic_init(bp, load_code);

	if ((load_code == FW_MSG_CODE_DRV_LOAD_COMMON) &&
	    (bp->common.shmem2_base))
		SHMEM2_WR(bp, dcc_support,
			  (SHMEM_DCC_SUPPORT_DISABLE_ENABLE_PF_TLV |
			   SHMEM_DCC_SUPPORT_BANDWIDTH_ALLOCATION_TLV));

	/* Send LOAD_DONE command to MCP */
	if (!BP_NOMCP(bp)) {
		load_code = bnx2x_fw_command(bp, DRV_MSG_CODE_LOAD_DONE);
		if (!load_code) {
			BNX2X_ERR("MCP response failure, aborting\n");
			rc = -EBUSY;
			goto load_error3;
		}
	}

	bp->state = BNX2X_STATE_OPENING_WAIT4_PORT;

	rc = bnx2x_setup_leading(bp);
	if (rc) {
		BNX2X_ERR("Setup leading failed!\n");
#ifndef BNX2X_STOP_ON_ERROR
		goto load_error3;
#else
		bp->panic = 1;
		return -EBUSY;
#endif
	}

	if (CHIP_IS_E1H(bp))
		if (bp->mf_config & FUNC_MF_CFG_FUNC_DISABLED) {
			DP(NETIF_MSG_IFUP, "mf_cfg function disabled\n");
			bp->flags |= MF_FUNC_DIS;
		}

	if (bp->state == BNX2X_STATE_OPEN) {
#ifdef BCM_CNIC
		/* Enable Timer scan */
		REG_WR(bp, TM_REG_EN_LINEAR0_TIMER + BP_PORT(bp)*4, 1);
#endif
		for_each_nondefault_queue(bp, i) {
			rc = bnx2x_setup_multi(bp, i);
			if (rc)
#ifdef BCM_CNIC
				goto load_error4;
#else
				goto load_error3;
#endif
		}

		if (CHIP_IS_E1(bp))
			bnx2x_set_eth_mac_addr_e1(bp, 1);
		else
			bnx2x_set_eth_mac_addr_e1h(bp, 1);
#ifdef BCM_CNIC
		/* Set iSCSI L2 MAC */
		mutex_lock(&bp->cnic_mutex);
		if (bp->cnic_eth_dev.drv_state & CNIC_DRV_STATE_REGD) {
			bnx2x_set_iscsi_eth_mac_addr(bp, 1);
			bp->cnic_flags |= BNX2X_CNIC_FLAG_MAC_SET;
			bnx2x_init_sb(bp, bp->cnic_sb, bp->cnic_sb_mapping,
				      CNIC_SB_ID(bp));
		}
		mutex_unlock(&bp->cnic_mutex);
#endif
	}

	if (bp->port.pmf)
		bnx2x_initial_phy_init(bp, load_mode);

	/* Start fast path */
	switch (load_mode) {
	case LOAD_NORMAL:
		if (bp->state == BNX2X_STATE_OPEN) {
			/* Tx queue should be only reenabled */
			netif_tx_wake_all_queues(bp->dev);
		}
		/* Initialize the receive filter. */
		bnx2x_set_rx_mode(bp->dev);
		break;

	case LOAD_OPEN:
		netif_tx_start_all_queues(bp->dev);
		if (bp->state != BNX2X_STATE_OPEN)
			netif_tx_disable(bp->dev);
		/* Initialize the receive filter. */
		bnx2x_set_rx_mode(bp->dev);
		break;

	case LOAD_DIAG:
		/* Initialize the receive filter. */
		bnx2x_set_rx_mode(bp->dev);
		bp->state = BNX2X_STATE_DIAG;
		break;

	default:
		break;
	}

	if (!bp->port.pmf)
		bnx2x__link_status_update(bp);

	/* start the timer */
	mod_timer(&bp->timer, jiffies + bp->current_interval);

#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__) /* ! BNX2X_UPSTREAM */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		fp->netq_flags &= ~(BNX2X_NETQ_RX_QUEUE_ALLOCATED |
				    BNX2X_NETQ_TX_QUEUE_ALLOCATED);
	}
	bp->n_rx_queues_allocated = 0;
	bp->n_tx_queues_allocated = 0;
	if (bp->netq_enabled) {
		/* workaround for packets duplicated issue when NetQ enabled */
		bp->rx_mode = BNX2X_RX_MODE_NORMAL;
		bnx2x_set_storm_rx_mode(bp);
	}
#endif
#ifdef BCM_CNIC
	bnx2x_setup_cnic_irq_info(bp);
	if (bp->state == BNX2X_STATE_OPEN)
		bnx2x_cnic_notify(bp, CNIC_CTL_START_CMD);
#endif

	return 0;

#ifdef BCM_CNIC
load_error4:
	/* Disable Timer scan */
	REG_WR(bp, TM_REG_EN_LINEAR0_TIMER + BP_PORT(bp)*4, 0);
#endif
load_error3:
	bnx2x_int_disable_sync(bp, 1);
	if (!BP_NOMCP(bp)) {
		bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_REQ_WOL_MCP);
		bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_DONE);
	}
	bp->port.pmf = 0;
	/* Free SKBs, SGEs, TPA pool and driver internals */
	bnx2x_free_skbs(bp);
	for_each_queue(bp, i)
		bnx2x_free_rx_sge_range(bp, bp->fp + i, NUM_RX_SGE);
load_error2:
	/* Release IRQs */
	bnx2x_free_irq(bp);
load_error1:
	bnx2x_napi_disable(bp);
#if (defined(BNX2X_NEW_NAPI) && (LINUX_VERSION_CODE >= 0x02061b)) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
	for_each_queue(bp, i)
		netif_napi_del(&bnx2x_fp(bp, i, napi));
#endif
	bnx2x_free_mem(bp);

	return rc;
}

static int bnx2x_stop_multi(struct bnx2x *bp, int index)
{
	struct bnx2x_fastpath *fp = &bp->fp[index];
	int rc;

	/* halt the connection */
	fp->state = BNX2X_FP_STATE_HALTING;
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_HALT, index, 0, fp->cl_id, 0);

	/* Wait for completion */
	rc = bnx2x_wait_ramrod(bp, BNX2X_FP_STATE_HALTED, index,
			       &(fp->state), 1);
	if (rc) /* timeout */
		return rc;

	/* delete cfc entry */
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_CFC_DEL, index, 0, 0, 1);

	/* Wait for completion */
	rc = bnx2x_wait_ramrod(bp, BNX2X_FP_STATE_CLOSED, index,
			       &(fp->state), 1);
	return rc;
}

static int bnx2x_stop_leading(struct bnx2x *bp)
{
	__le16 dsb_sp_prod_idx;
	/* if the other port is handling traffic,
	   this can take a lot of time */
	int cnt = 500;
	int rc;

	might_sleep();

	/* Send HALT ramrod */
	bp->fp[0].state = BNX2X_FP_STATE_HALTING;
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_HALT, 0, 0, bp->fp->cl_id, 0);

	/* Wait for completion */
	rc = bnx2x_wait_ramrod(bp, BNX2X_FP_STATE_HALTED, 0,
			       &(bp->fp[0].state), 1);
	if (rc) /* timeout */
		return rc;

	dsb_sp_prod_idx = *bp->dsb_sp_prod;

	/* Send PORT_DELETE ramrod */
	bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_PORT_DEL, 0, 0, 0, 1);

	/* Wait for completion to arrive on default status block
	   we are going to reset the chip anyway
	   so there is not much to do if this times out
	 */
	while (dsb_sp_prod_idx == *bp->dsb_sp_prod) {
		if (!cnt) {
			DP(NETIF_MSG_IFDOWN, "timeout waiting for port del "
			   "dsb_sp_prod 0x%x != dsb_sp_prod_idx 0x%x\n",
			   *bp->dsb_sp_prod, dsb_sp_prod_idx);
#ifdef BNX2X_STOP_ON_ERROR
			bnx2x_panic();
#endif
			rc = -EBUSY;
			break;
		}
		cnt--;
		msleep(1);
		rmb(); /* Refresh the dsb_sp_prod */
	}
	bp->state = BNX2X_STATE_CLOSING_WAIT4_UNLOAD;
	bp->fp[0].state = BNX2X_FP_STATE_CLOSED;

	return rc;
}

static void bnx2x_reset_func(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int func = BP_FUNC(bp);
	int base, i;

	/* Configure IGU */
	REG_WR(bp, HC_REG_LEADING_EDGE_0 + port*8, 0);
	REG_WR(bp, HC_REG_TRAILING_EDGE_0 + port*8, 0);

#ifdef BCM_CNIC
	/* Disable Timer scan */
	REG_WR(bp, TM_REG_EN_LINEAR0_TIMER + port*4, 0);
	/*
	 * Wait for at least 10ms and up to 2 second for the timers scan to
	 * complete
	 */
	for (i = 0; i < 200; i++) {
		msleep(10);
		if (!REG_RD(bp, TM_REG_LIN0_SCAN_ON + port*4))
			break;
	}
#endif
	/* Clear ILT */
	base = FUNC_ILT_BASE(func);
	for (i = base; i < base + ILT_PER_FUNC; i++)
		bnx2x_ilt_wr(bp, i, 0);
}

static void bnx2x_reset_port(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	u32 val;

	REG_WR(bp, NIG_REG_MASK_INTERRUPT_PORT0 + port*4, 0);

	/* Do not rcv packets to BRB */
	REG_WR(bp, NIG_REG_LLH0_BRB1_DRV_MASK + port*4, 0x0);
	/* Do not direct rcv packets that are not for MCP to the BRB */
	REG_WR(bp, (port ? NIG_REG_LLH1_BRB1_NOT_MCP :
			   NIG_REG_LLH0_BRB1_NOT_MCP), 0x0);

	/* Configure AEU */
	REG_WR(bp, MISC_REG_AEU_MASK_ATTN_FUNC_0 + port*4, 0);

	msleep(100);
	/* Check for BRB port occupancy */
	val = REG_RD(bp, BRB1_REG_PORT_NUM_OCC_BLOCKS_0 + port*4);
	if (val)
		DP(NETIF_MSG_IFDOWN,
		   "BRB1 is not empty  %d blocks are occupied\n", val);

	/* TODO: Close Doorbell port? */
}

static void bnx2x_reset_chip(struct bnx2x *bp, u32 reset_code)
{
	DP(BNX2X_MSG_MCP, "function %d  reset_code %x\n",
	   BP_FUNC(bp), reset_code);

	switch (reset_code) {
	case FW_MSG_CODE_DRV_UNLOAD_COMMON:
		bnx2x_reset_port(bp);
		bnx2x_reset_func(bp);
		bnx2x_reset_common(bp);
		break;

	case FW_MSG_CODE_DRV_UNLOAD_PORT:
		bnx2x_reset_port(bp);
		bnx2x_reset_func(bp);
		break;

	case FW_MSG_CODE_DRV_UNLOAD_FUNCTION:
		bnx2x_reset_func(bp);
		break;

	default:
		BNX2X_ERR("Unknown reset_code (0x%x) from MCP\n", reset_code);
		break;
	}
}

/* must be called with rtnl_lock */
static int bnx2x_nic_unload(struct bnx2x *bp, int unload_mode)
{
	int port = BP_PORT(bp);
	u32 reset_code = 0;
	int i, cnt, rc;

#ifdef BCM_CNIC
	bnx2x_cnic_notify(bp, CNIC_CTL_STOP_CMD);
#endif
	bp->state = BNX2X_STATE_CLOSING_WAIT4_HALT;

	/* Set "drop all" */
	bp->rx_mode = BNX2X_RX_MODE_NONE;
	bnx2x_set_storm_rx_mode(bp);

	/* Disable HW interrupts, NAPI and Tx */
	bnx2x_netif_stop(bp, 1);

	del_timer_sync(&bp->timer);
	SHMEM_WR(bp, func_mb[BP_FUNC(bp)].drv_pulse_mb,
		 (DRV_PULSE_ALWAYS_ALIVE | bp->fw_drv_pulse_wr_seq));
	bnx2x_stats_handle(bp, STATS_EVENT_STOP);

	/* Release IRQs */
	bnx2x_free_irq(bp);

	/* Wait until tx fastpath tasks complete */
	for_each_queue(bp, i) {
		struct bnx2x_fastpath *fp = &bp->fp[i];

		cnt = 1000;
		while (bnx2x_has_tx_work_unload(fp)) {

			bnx2x_tx_int(fp);
			if (!cnt) {
				BNX2X_ERR("timeout waiting for queue[%d]\n",
					  i);
#ifdef BNX2X_STOP_ON_ERROR
				bnx2x_panic();
				return -EBUSY;
#else
				break;
#endif
			}
			cnt--;
			msleep(1);
		}
	}
	/* Give HW time to discard old tx messages */
	msleep(1);

	if (CHIP_IS_E1(bp)) {
		struct mac_configuration_cmd *config =
						bnx2x_sp(bp, mcast_config);

		bnx2x_set_eth_mac_addr_e1(bp, 0);

		for (i = 0; i < config->hdr.length; i++)
			CAM_INVALIDATE(config->config_table[i]);

		config->hdr.length = i;
		if (CHIP_REV_IS_SLOW(bp))
			config->hdr.offset = BNX2X_MAX_EMUL_MULTI*(1 + port);
		else
			config->hdr.offset = BNX2X_MAX_MULTICAST*(1 + port);
		config->hdr.client_id = bp->fp->cl_id;
		config->hdr.reserved1 = 0;

		bp->set_mac_pending++;
		smp_wmb();

		bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_SET_MAC, 0,
			      U64_HI(bnx2x_sp_mapping(bp, mcast_config)),
			      U64_LO(bnx2x_sp_mapping(bp, mcast_config)), 0);

	} else { /* E1H */
		REG_WR(bp, NIG_REG_LLH0_FUNC_EN + port*8, 0);

		bnx2x_set_eth_mac_addr_e1h(bp, 0);

		for (i = 0; i < MC_HASH_SIZE; i++)
			REG_WR(bp, MC_HASH_OFFSET(bp, i), 0);

		REG_WR(bp, MISC_REG_E1HMF_MODE, 0);
	}
#ifdef BCM_CNIC
	/* Clear iSCSI L2 MAC */
	mutex_lock(&bp->cnic_mutex);
	if (bp->cnic_flags & BNX2X_CNIC_FLAG_MAC_SET) {
		bnx2x_set_iscsi_eth_mac_addr(bp, 0);
		bp->cnic_flags &= ~BNX2X_CNIC_FLAG_MAC_SET;
	}
	mutex_unlock(&bp->cnic_mutex);
#endif

	if (unload_mode == UNLOAD_NORMAL)
		reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS;

	else if (bp->flags & NO_WOL_FLAG)
		reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_MCP;

	else if (bp->wol) {
		u32 emac_base = port ? GRCBASE_EMAC1 : GRCBASE_EMAC0;
		u8 *mac_addr = bp->dev->dev_addr;
		u32 val;
		/* The mac address is written to entries 1-4 to
		   preserve entry 0 which is used by the PMF */
		u8 entry = (BP_E1HVN(bp) + 1)*8;

		val = (mac_addr[0] << 8) | mac_addr[1];
		EMAC_WR(bp, EMAC_REG_EMAC_MAC_MATCH + entry, val);

		val = (mac_addr[2] << 24) | (mac_addr[3] << 16) |
		      (mac_addr[4] << 8) | mac_addr[5];
		EMAC_WR(bp, EMAC_REG_EMAC_MAC_MATCH + entry + 4, val);

		reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_EN;

	} else
		reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS;

	/* Close multi and leading connections
	   Completions for ramrods are collected in a synchronous way */
	for_each_nondefault_queue(bp, i)
		if (bnx2x_stop_multi(bp, i))
			goto unload_error;

	rc = bnx2x_stop_leading(bp);
	if (rc) {
		BNX2X_ERR("Stop leading failed!\n");
#ifdef BNX2X_STOP_ON_ERROR
		return -EBUSY;
#else
		goto unload_error;
#endif
	}

unload_error:
	if (!BP_NOMCP(bp))
		reset_code = bnx2x_fw_command(bp, reset_code);
	else {
		DP(NETIF_MSG_IFDOWN, "NO MCP - load counts      %d, %d, %d\n",
		   load_count[0], load_count[1], load_count[2]);
		load_count[0]--;
		load_count[1 + port]--;
		DP(NETIF_MSG_IFDOWN, "NO MCP - new load counts  %d, %d, %d\n",
		   load_count[0], load_count[1], load_count[2]);
		if (load_count[0] == 0)
			reset_code = FW_MSG_CODE_DRV_UNLOAD_COMMON;
		else if (load_count[1 + port] == 0)
			reset_code = FW_MSG_CODE_DRV_UNLOAD_PORT;
		else
			reset_code = FW_MSG_CODE_DRV_UNLOAD_FUNCTION;
	}

	if ((reset_code == FW_MSG_CODE_DRV_UNLOAD_COMMON) ||
	    (reset_code == FW_MSG_CODE_DRV_UNLOAD_PORT))
		bnx2x__link_reset(bp);

	/* Reset the chip */
	bnx2x_reset_chip(bp, reset_code);

	/* Report UNLOAD_DONE to MCP */
	if (!BP_NOMCP(bp))
		bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_DONE);

	bp->port.pmf = 0;

	/* Free SKBs, SGEs, TPA pool and driver internals */
	bnx2x_free_skbs(bp);
	for_each_queue(bp, i)
		bnx2x_free_rx_sge_range(bp, bp->fp + i, NUM_RX_SGE);
#if (defined(BNX2X_NEW_NAPI) && (LINUX_VERSION_CODE >= 0x02061b)) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
	for_each_queue(bp, i)
		netif_napi_del(&bnx2x_fp(bp, i, napi));
#endif
	bnx2x_free_mem(bp);

	bp->state = BNX2X_STATE_CLOSED;

	netif_carrier_off(bp->dev);

	return 0;
}

#if defined(INIT_DELAYED_WORK_DEFERRABLE) || defined(INIT_WORK_NAR) || (VMWARE_ESX_DDK_VERSION >= 40000) /* BNX2X_UPSTREAM */
static void bnx2x_reset_task(struct work_struct *work)
{
	struct bnx2x *bp = container_of(work, struct bnx2x, reset_task);
#else
static void bnx2x_reset_task(void *data)
{
	struct bnx2x *bp = (struct bnx2x *)data;
#endif

#ifdef BNX2X_STOP_ON_ERROR
	BNX2X_ERR("reset task called but STOP_ON_ERROR defined"
		  " so reset not done to allow debug dump,\n"
	 KERN_ERR " you will need to reboot when done\n");
	return;
#endif

	rtnl_lock();

	if (!netif_running(bp->dev))
		goto reset_task_exit;

	bnx2x_nic_unload(bp, UNLOAD_NORMAL);
	bnx2x_nic_load(bp, LOAD_NORMAL);

reset_task_exit:
	rtnl_unlock();
}

/* end of nic load/unload */

/* ethtool_ops */

/*
 * Init service functions
 */

static inline u32 bnx2x_get_pretend_reg(struct bnx2x *bp, int func)
{
	switch (func) {
	case 0: return PXP2_REG_PGL_PRETEND_FUNC_F0;
	case 1:	return PXP2_REG_PGL_PRETEND_FUNC_F1;
	case 2:	return PXP2_REG_PGL_PRETEND_FUNC_F2;
	case 3:	return PXP2_REG_PGL_PRETEND_FUNC_F3;
	case 4:	return PXP2_REG_PGL_PRETEND_FUNC_F4;
	case 5:	return PXP2_REG_PGL_PRETEND_FUNC_F5;
	case 6:	return PXP2_REG_PGL_PRETEND_FUNC_F6;
	case 7:	return PXP2_REG_PGL_PRETEND_FUNC_F7;
	default:
		BNX2X_ERR("Unsupported function index: %d\n", func);
		return (u32)(-1);
	}
}

static void bnx2x_undi_int_disable_e1h(struct bnx2x *bp, int orig_func)
{
	u32 reg = bnx2x_get_pretend_reg(bp, orig_func), new_val;

	/* Flush all outstanding writes */
	mmiowb();

	/* Pretend to be function 0 */
	REG_WR(bp, reg, 0);
	/* Flush the GRC transaction (in the chip) */
	new_val = REG_RD(bp, reg);
	if (new_val != 0) {
		BNX2X_ERR("Hmmm... Pretend register wasn't updated: (0,%d)!\n",
			  new_val);
		BUG();
	}

	/* From now we are in the "like-E1" mode */
	bnx2x_int_disable(bp);

	/* Flush all outstanding writes */
	mmiowb();

	/* Restore the original funtion settings */
	REG_WR(bp, reg, orig_func);
	new_val = REG_RD(bp, reg);
	if (new_val != orig_func) {
		BNX2X_ERR("Hmmm... Pretend register wasn't updated: (%d,%d)!\n",
			  orig_func, new_val);
		BUG();
	}
}

static inline void bnx2x_undi_int_disable(struct bnx2x *bp, int func)
{
	if (CHIP_IS_E1H(bp))
		bnx2x_undi_int_disable_e1h(bp, func);
	else
		bnx2x_int_disable(bp);
}

static void __devinit bnx2x_undi_unload(struct bnx2x *bp)
{
	u32 val;

	/* Check if there is any driver already loaded */
	val = REG_RD(bp, MISC_REG_UNPREPARED);
	if (val == 0x1) {
		/* Check if it is the UNDI driver
		 * UNDI driver initializes CID offset for normal bell to 0x7
		 */
		bnx2x_acquire_hw_lock(bp, HW_LOCK_RESOURCE_UNDI);
		val = REG_RD(bp, DORQ_REG_NORM_CID_OFST);
		if (val == 0x7) {
			u32 reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS;
			/* save our func */
			int func = BP_FUNC(bp);
			u32 swap_en;
			u32 swap_val;

			/* clear the UNDI indication */
			REG_WR(bp, DORQ_REG_NORM_CID_OFST, 0);

			BNX2X_DEV_INFO("UNDI is active! reset device\n");

			/* try unload UNDI on port 0 */
			bp->func = 0;
			bp->fw_seq =
			       (SHMEM_RD(bp, func_mb[bp->func].drv_mb_header) &
				DRV_MSG_SEQ_NUMBER_MASK);
			reset_code = bnx2x_fw_command(bp, reset_code);

			/* if UNDI is loaded on the other port */
			if (reset_code != FW_MSG_CODE_DRV_UNLOAD_COMMON) {

				/* send "DONE" for previous unload */
				bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_DONE);

				/* unload UNDI on port 1 */
				bp->func = 1;
				bp->fw_seq =
			       (SHMEM_RD(bp, func_mb[bp->func].drv_mb_header) &
					DRV_MSG_SEQ_NUMBER_MASK);
				reset_code = DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS;

				bnx2x_fw_command(bp, reset_code);
			}

			/* now it's safe to release the lock */
			bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_UNDI);

			bnx2x_undi_int_disable(bp, func);

			/* close input traffic and wait for it */
			/* Do not rcv packets to BRB */
			REG_WR(bp,
			      (BP_PORT(bp) ? NIG_REG_LLH1_BRB1_DRV_MASK :
					     NIG_REG_LLH0_BRB1_DRV_MASK), 0x0);
			/* Do not direct rcv packets that are not for MCP to
			 * the BRB */
			REG_WR(bp,
			       (BP_PORT(bp) ? NIG_REG_LLH1_BRB1_NOT_MCP :
					      NIG_REG_LLH0_BRB1_NOT_MCP), 0x0);
			/* clear AEU */
			REG_WR(bp,
			     (BP_PORT(bp) ? MISC_REG_AEU_MASK_ATTN_FUNC_1 :
					    MISC_REG_AEU_MASK_ATTN_FUNC_0), 0);
			msleep(10);

			/* save NIG port swap info */
			swap_val = REG_RD(bp, NIG_REG_PORT_SWAP);
			swap_en = REG_RD(bp, NIG_REG_STRAP_OVERRIDE);
			/* reset device */
			REG_WR(bp,
			       GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR,
			       0xd3ffffff);
			REG_WR(bp,
			       GRCBASE_MISC + MISC_REGISTERS_RESET_REG_2_CLEAR,
			       0x1403);
			/* take the NIG out of reset and restore swap values */
			REG_WR(bp,
			       GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET,
			       MISC_REGISTERS_RESET_REG_1_RST_NIG);
			REG_WR(bp, NIG_REG_PORT_SWAP, swap_val);
			REG_WR(bp, NIG_REG_STRAP_OVERRIDE, swap_en);

			/* send unload done to the MCP */
			bnx2x_fw_command(bp, DRV_MSG_CODE_UNLOAD_DONE);

			/* restore our func and fw_seq */
			bp->func = func;
			bp->fw_seq =
			       (SHMEM_RD(bp, func_mb[bp->func].drv_mb_header) &
				DRV_MSG_SEQ_NUMBER_MASK);

		} else
			bnx2x_release_hw_lock(bp, HW_LOCK_RESOURCE_UNDI);
	}
}

static void __devinit bnx2x_get_common_hwinfo(struct bnx2x *bp)
{
	u32 val, val2, val3, val4, id;
	u16 pmc;

	/* Get the chip revision id and number. */
	/* chip num:16-31, rev:12-15, metal:4-11, bond_id:0-3 */
	val = REG_RD(bp, MISC_REG_CHIP_NUM);
	id = ((val & 0xffff) << 16);
	val = REG_RD(bp, MISC_REG_CHIP_REV);
	id |= ((val & 0xf) << 12);
	val = REG_RD(bp, MISC_REG_CHIP_METAL);
	id |= ((val & 0xff) << 4);
	val = REG_RD(bp, MISC_REG_BOND_ID);
	id |= (val & 0xf);
	bp->common.chip_id = id;
	bp->link_params.chip_id = bp->common.chip_id;
	BNX2X_DEV_INFO("chip ID is 0x%x\n", id);

	val = (REG_RD(bp, 0x2874) & 0x55);
	if ((bp->common.chip_id & 0x1) ||
	    (CHIP_IS_E1(bp) && val) || (CHIP_IS_E1H(bp) && (val == 0x55))) {
		bp->flags |= ONE_PORT_FLAG;
		BNX2X_DEV_INFO("single port device\n");
	}

	val = REG_RD(bp, MCP_REG_MCPR_NVM_CFG4);
	bp->common.flash_size = (NVRAM_1MB_SIZE <<
				 (val & MCPR_NVM_CFG4_FLASH_SIZE));
	BNX2X_DEV_INFO("flash_size 0x%x (%d)\n",
		       bp->common.flash_size, bp->common.flash_size);

	bp->common.shmem_base = REG_RD(bp, MISC_REG_SHARED_MEM_ADDR);
	bp->common.shmem2_base = REG_RD(bp, MISC_REG_GENERIC_CR_0);
	bp->link_params.shmem_base = bp->common.shmem_base;
	BNX2X_DEV_INFO("shmem offset 0x%x  shmem2 offset 0x%x\n",
		       bp->common.shmem_base, bp->common.shmem2_base);

	if (!bp->common.shmem_base ||
	    (bp->common.shmem_base < 0xA0000) ||
	    (bp->common.shmem_base >= 0xC0000)) {
		BNX2X_DEV_INFO("MCP not active\n");
		bp->flags |= NO_MCP_FLAG;
		return;
	}

	val = SHMEM_RD(bp, validity_map[BP_PORT(bp)]);
	if ((val & (SHR_MEM_VALIDITY_DEV_INFO | SHR_MEM_VALIDITY_MB))
		!= (SHR_MEM_VALIDITY_DEV_INFO | SHR_MEM_VALIDITY_MB))
		BNX2X_ERROR("BAD MCP validity signature\n");

	bp->common.hw_config = SHMEM_RD(bp, dev_info.shared_hw_config.config);
	BNX2X_DEV_INFO("hw_config 0x%08x\n", bp->common.hw_config);

	bp->link_params.hw_led_mode = ((bp->common.hw_config &
					SHARED_HW_CFG_LED_MODE_MASK) >>
				       SHARED_HW_CFG_LED_MODE_SHIFT);

	bp->link_params.feature_config_flags = 0;
	val = SHMEM_RD(bp, dev_info.shared_feature_config.config);
	if (val & SHARED_FEAT_CFG_OVERRIDE_PREEMPHASIS_CFG_ENABLED)
		bp->link_params.feature_config_flags |=
				FEATURE_CONFIG_OVERRIDE_PREEMPHASIS_ENABLED;
	else
		bp->link_params.feature_config_flags &=
				~FEATURE_CONFIG_OVERRIDE_PREEMPHASIS_ENABLED;

	val = SHMEM_RD(bp, dev_info.bc_rev) >> 8;
	bp->common.bc_ver = val;
	BNX2X_DEV_INFO("bc_ver %X\n", val);
	if (val < BNX2X_BC_VER) {
		/* for now only warn
		 * later we might need to enforce this */
		BNX2X_ERROR("This driver needs bc_ver %X but found %X, "
			    "please upgrade BC\n", BNX2X_BC_VER, val);
	}
	bp->link_params.feature_config_flags |=
				(val >= REQ_BC_VER_4_VRFY_OPT_MDL) ?
				FEATURE_CONFIG_BC_SUPPORTS_OPT_MDL_VRFY : 0;

	if (BP_E1HVN(bp) == 0) {
		pci_read_config_word(bp->pdev, bp->pm_cap + PCI_PM_PMC, &pmc);
		bp->flags |= (pmc & PCI_PM_CAP_PME_D3cold) ? 0 : NO_WOL_FLAG;
	} else {
		/* no WOL capability for E1HVN != 0 */
		bp->flags |= NO_WOL_FLAG;
	}
	BNX2X_DEV_INFO("%sWoL capable\n",
		       (bp->flags & NO_WOL_FLAG) ? "not " : "");

	val = SHMEM_RD(bp, dev_info.shared_hw_config.part_num);
	val2 = SHMEM_RD(bp, dev_info.shared_hw_config.part_num[4]);
	val3 = SHMEM_RD(bp, dev_info.shared_hw_config.part_num[8]);
	val4 = SHMEM_RD(bp, dev_info.shared_hw_config.part_num[12]);

	printk(KERN_INFO PFX "part number %X-%X-%X-%X\n",
	       val, val2, val3, val4);
}

static void __devinit bnx2x_link_settings_supported(struct bnx2x *bp,
						    u32 switch_cfg)
{
	int port = BP_PORT(bp);
	u32 ext_phy_type;

	switch (switch_cfg) {
	case SWITCH_CFG_1G:
		BNX2X_DEV_INFO("switch_cfg 0x%x (1G)\n", switch_cfg);

		ext_phy_type =
			SERDES_EXT_PHY_TYPE(bp->link_params.ext_phy_config);
		switch (ext_phy_type) {
		case PORT_HW_CFG_SERDES_EXT_PHY_TYPE_DIRECT:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (Direct)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10baseT_Half |
					       SUPPORTED_10baseT_Full |
					       SUPPORTED_100baseT_Half |
					       SUPPORTED_100baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_2500baseX_Full |
					       SUPPORTED_TP |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_SERDES_EXT_PHY_TYPE_BCM5482:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (5482)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10baseT_Half |
					       SUPPORTED_10baseT_Full |
					       SUPPORTED_100baseT_Half |
					       SUPPORTED_100baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_TP |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		default:
			BNX2X_ERROR("NVRAM config error. "
				    "BAD SerDes ext_phy_config 0x%x\n",
				    bp->link_params.ext_phy_config);
			return;
		}

		bp->port.phy_addr = REG_RD(bp, NIG_REG_SERDES0_CTRL_PHY_ADDR +
					   port*0x10);
		BNX2X_DEV_INFO("phy_addr 0x%x\n", bp->port.phy_addr);
		break;

	case SWITCH_CFG_10G:
		BNX2X_DEV_INFO("switch_cfg 0x%x (10G)\n", switch_cfg);

		ext_phy_type =
			XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);
		switch (ext_phy_type) {
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_DIRECT:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (Direct)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10baseT_Half |
					       SUPPORTED_10baseT_Full |
					       SUPPORTED_100baseT_Half |
					       SUPPORTED_100baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_2500baseX_Full |
					       SUPPORTED_10000baseT_Full |
					       SUPPORTED_TP |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8072:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8072)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8073:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8073)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_2500baseX_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8705:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8705)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8706:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8706)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8726:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8726)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_Autoneg |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (8727)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_Autoneg |
					       SUPPORTED_FIBRE |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (SFX7101)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10000baseT_Full |
					       SUPPORTED_TP |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8481:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM84823:
			BNX2X_DEV_INFO("ext_phy_type 0x%x (BCM8481)\n",
				       ext_phy_type);

			bp->port.supported |= (SUPPORTED_10baseT_Half |
					       SUPPORTED_10baseT_Full |
					       SUPPORTED_100baseT_Half |
					       SUPPORTED_100baseT_Full |
					       SUPPORTED_1000baseT_Full |
					       SUPPORTED_10000baseT_Full |
					       SUPPORTED_TP |
					       SUPPORTED_Autoneg |
					       SUPPORTED_Pause |
					       SUPPORTED_Asym_Pause);
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_FAILURE:
			BNX2X_ERROR("XGXS PHY Failure detected 0x%x\n",
				    bp->link_params.ext_phy_config);
			break;

		default:
			BNX2X_ERROR("NVRAM config error. "
				    "BAD XGXS ext_phy_config 0x%x\n",
				    bp->link_params.ext_phy_config);
			return;
		}

		bp->port.phy_addr = REG_RD(bp, NIG_REG_XGXS0_CTRL_PHY_ADDR +
					   port*0x18);
		BNX2X_DEV_INFO("phy_addr 0x%x\n", bp->port.phy_addr);

		break;

	default:
		BNX2X_ERROR("BAD switch_cfg link_config 0x%x\n",
			    bp->port.link_config);
		return;
	}
	bp->link_params.phy_addr = bp->port.phy_addr;

	/* mask what we support according to speed_cap_mask */
	if (!(bp->link_params.speed_cap_mask &
				PORT_HW_CFG_SPEED_CAPABILITY_D0_10M_HALF))
		bp->port.supported &= ~SUPPORTED_10baseT_Half;

	if (!(bp->link_params.speed_cap_mask &
				PORT_HW_CFG_SPEED_CAPABILITY_D0_10M_FULL))
		bp->port.supported &= ~SUPPORTED_10baseT_Full;

	if (!(bp->link_params.speed_cap_mask &
				PORT_HW_CFG_SPEED_CAPABILITY_D0_100M_HALF))
		bp->port.supported &= ~SUPPORTED_100baseT_Half;

	if (!(bp->link_params.speed_cap_mask &
				PORT_HW_CFG_SPEED_CAPABILITY_D0_100M_FULL))
		bp->port.supported &= ~SUPPORTED_100baseT_Full;

	if (!(bp->link_params.speed_cap_mask &
					PORT_HW_CFG_SPEED_CAPABILITY_D0_1G))
		bp->port.supported &= ~(SUPPORTED_1000baseT_Half |
					SUPPORTED_1000baseT_Full);

	if (!(bp->link_params.speed_cap_mask &
					PORT_HW_CFG_SPEED_CAPABILITY_D0_2_5G))
		bp->port.supported &= ~SUPPORTED_2500baseX_Full;

	if (!(bp->link_params.speed_cap_mask &
					PORT_HW_CFG_SPEED_CAPABILITY_D0_10G))
		bp->port.supported &= ~SUPPORTED_10000baseT_Full;

	BNX2X_DEV_INFO("supported 0x%x\n", bp->port.supported);
}

static void __devinit bnx2x_link_settings_requested(struct bnx2x *bp)
{
	bp->link_params.req_duplex = DUPLEX_FULL;

	switch (bp->port.link_config & PORT_FEATURE_LINK_SPEED_MASK) {
	case PORT_FEATURE_LINK_SPEED_AUTO:
		if (bp->port.supported & SUPPORTED_Autoneg) {
			bp->link_params.req_line_speed = SPEED_AUTO_NEG;
			bp->port.advertising = bp->port.supported;
		} else {
			u32 ext_phy_type =
			    XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);

			if ((ext_phy_type ==
			     PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8705) ||
			    (ext_phy_type ==
			     PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8706)) {
				/* force 10G, no AN */
				bp->link_params.req_line_speed = SPEED_10000;
				bp->port.advertising =
						(ADVERTISED_10000baseT_Full |
						 ADVERTISED_FIBRE);
				break;
			}
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  autoneg not supported\n",
				    bp->port.link_config);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_10M_FULL:
		if (bp->port.supported & SUPPORTED_10baseT_Full) {
			bp->link_params.req_line_speed = SPEED_10;
			bp->port.advertising = (ADVERTISED_10baseT_Full |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_10M_HALF:
		if (bp->port.supported & SUPPORTED_10baseT_Half) {
			bp->link_params.req_line_speed = SPEED_10;
			bp->link_params.req_duplex = DUPLEX_HALF;
			bp->port.advertising = (ADVERTISED_10baseT_Half |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_100M_FULL:
		if (bp->port.supported & SUPPORTED_100baseT_Full) {
			bp->link_params.req_line_speed = SPEED_100;
			bp->port.advertising = (ADVERTISED_100baseT_Full |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_100M_HALF:
		if (bp->port.supported & SUPPORTED_100baseT_Half) {
			bp->link_params.req_line_speed = SPEED_100;
			bp->link_params.req_duplex = DUPLEX_HALF;
			bp->port.advertising = (ADVERTISED_100baseT_Half |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_1G:
		if (bp->port.supported & SUPPORTED_1000baseT_Full) {
			bp->link_params.req_line_speed = SPEED_1000;
			bp->port.advertising = (ADVERTISED_1000baseT_Full |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_2_5G:
		if (bp->port.supported & SUPPORTED_2500baseX_Full) {
			bp->link_params.req_line_speed = SPEED_2500;
			bp->port.advertising = (ADVERTISED_2500baseX_Full |
						ADVERTISED_TP);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	case PORT_FEATURE_LINK_SPEED_10G_CX4:
	case PORT_FEATURE_LINK_SPEED_10G_KX4:
	case PORT_FEATURE_LINK_SPEED_10G_KR:
		if (bp->port.supported & SUPPORTED_10000baseT_Full) {
			bp->link_params.req_line_speed = SPEED_10000;
			bp->port.advertising = (ADVERTISED_10000baseT_Full |
						ADVERTISED_FIBRE);
		} else {
			BNX2X_ERROR("NVRAM config error. "
				    "Invalid link_config 0x%x"
				    "  speed_cap_mask 0x%x\n",
				    bp->port.link_config,
				    bp->link_params.speed_cap_mask);
			return;
		}
		break;

	default:
		BNX2X_ERROR("NVRAM config error. "
			    "BAD link speed link_config 0x%x\n",
			    bp->port.link_config);
		bp->link_params.req_line_speed = SPEED_AUTO_NEG;
		bp->port.advertising = bp->port.supported;
		break;
	}

	bp->link_params.req_flow_ctrl = (bp->port.link_config &
					 PORT_FEATURE_FLOW_CONTROL_MASK);
	if ((bp->link_params.req_flow_ctrl == BNX2X_FLOW_CTRL_AUTO) &&
	    !(bp->port.supported & SUPPORTED_Autoneg))
		bp->link_params.req_flow_ctrl = BNX2X_FLOW_CTRL_NONE;

	BNX2X_DEV_INFO("req_line_speed %d  req_duplex %d  req_flow_ctrl 0x%x"
		       "  advertising 0x%x\n",
		       bp->link_params.req_line_speed,
		       bp->link_params.req_duplex,
		       bp->link_params.req_flow_ctrl, bp->port.advertising);
}

static inline void bnx2x_set_mac_buf(u8 *mac_buf, u32 mac_lo, u16 mac_hi)
{
	mac_hi = cpu_to_be16(mac_hi);
	mac_lo = cpu_to_be32(mac_lo);
	memcpy(mac_buf, &mac_hi, sizeof(mac_hi));
	memcpy(mac_buf + sizeof(mac_hi), &mac_lo, sizeof(mac_lo));
}

static void __devinit bnx2x_get_port_hwinfo(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	u32 val, val2;
	u32 config;
	u16 i;
	u32 ext_phy_type;

	bp->link_params.bp = bp;
	bp->link_params.port = port;

	bp->link_params.lane_config =
		SHMEM_RD(bp, dev_info.port_hw_config[port].lane_config);
	bp->link_params.ext_phy_config =
		SHMEM_RD(bp,
			 dev_info.port_hw_config[port].external_phy_config);
	/* BCM8727_NOC => BCM8727 no over current */
	if (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config) ==
	    PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727_NOC) {
		bp->link_params.ext_phy_config &=
					~PORT_HW_CFG_XGXS_EXT_PHY_TYPE_MASK;
		bp->link_params.ext_phy_config |=
					PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727;
		bp->link_params.feature_config_flags |=
					FEATURE_CONFIG_BCM8727_NOC;
	}

	bp->link_params.speed_cap_mask =
		SHMEM_RD(bp,
			 dev_info.port_hw_config[port].speed_capability_mask);

	bp->port.link_config =
		SHMEM_RD(bp, dev_info.port_feature_config[port].link_config);

	/* Get the 4 lanes xgxs config rx and tx */
	for (i = 0; i < 2; i++) {
		val = SHMEM_RD(bp,
			   dev_info.port_hw_config[port].xgxs_config_rx[i<<1]);
		bp->link_params.xgxs_config_rx[i << 1] = ((val>>16) & 0xffff);
		bp->link_params.xgxs_config_rx[(i << 1) + 1] = (val & 0xffff);

		val = SHMEM_RD(bp,
			   dev_info.port_hw_config[port].xgxs_config_tx[i<<1]);
		bp->link_params.xgxs_config_tx[i << 1] = ((val>>16) & 0xffff);
		bp->link_params.xgxs_config_tx[(i << 1) + 1] = (val & 0xffff);
	}

	/* If the device is capable of WoL, set the default state according
	 * to the HW
	 */
	config = SHMEM_RD(bp, dev_info.port_feature_config[port].config);
	bp->wol = (!(bp->flags & NO_WOL_FLAG) &&
		   (config & PORT_FEATURE_WOL_ENABLED));

	BNX2X_DEV_INFO("lane_config 0x%08x  ext_phy_config 0x%08x"
		       "  speed_cap_mask 0x%08x  link_config 0x%08x\n",
		       bp->link_params.lane_config,
		       bp->link_params.ext_phy_config,
		       bp->link_params.speed_cap_mask, bp->port.link_config);

	bp->link_params.switch_cfg = (bp->port.link_config &
				      PORT_FEATURE_CONNECTED_SWITCH_MASK);
	bnx2x_link_settings_supported(bp, bp->link_params.switch_cfg);

	bnx2x_link_settings_requested(bp);

	/*
	 * If connected directly, work with the internal PHY, otherwise, work
	 * with the external PHY
	 */
	ext_phy_type = XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);
	if (ext_phy_type == PORT_HW_CFG_XGXS_EXT_PHY_TYPE_DIRECT)
		bp->mdio.prtad = bp->link_params.phy_addr;

	else if ((ext_phy_type != PORT_HW_CFG_XGXS_EXT_PHY_TYPE_FAILURE) &&
		 (ext_phy_type != PORT_HW_CFG_XGXS_EXT_PHY_TYPE_NOT_CONN))
		bp->mdio.prtad =
			XGXS_EXT_PHY_ADDR(bp->link_params.ext_phy_config);

	val2 = SHMEM_RD(bp, dev_info.port_hw_config[port].mac_upper);
	val = SHMEM_RD(bp, dev_info.port_hw_config[port].mac_lower);
	bnx2x_set_mac_buf(bp->dev->dev_addr, val, val2);
	memcpy(bp->link_params.mac_addr, bp->dev->dev_addr, ETH_ALEN);
#ifdef ETHTOOL_GPERMADDR /* BNX2X_UPSTREAM */
	memcpy(bp->dev->perm_addr, bp->dev->dev_addr, ETH_ALEN);
#endif

#ifdef BCM_CNIC
	val2 = SHMEM_RD(bp, dev_info.port_hw_config[port].iscsi_mac_upper);
	val = SHMEM_RD(bp, dev_info.port_hw_config[port].iscsi_mac_lower);
	bnx2x_set_mac_buf(bp->iscsi_mac, val, val2);
#endif
}

static int __devinit bnx2x_get_hwinfo(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);
	u32 val, val2;
	int rc = 0;

	bnx2x_get_common_hwinfo(bp);

	bp->e1hov = 0;
	bp->e1hmf = 0;
	if (CHIP_IS_E1H(bp) && !BP_NOMCP(bp)) {
		bp->mf_config =
			SHMEM_RD(bp, mf_cfg.func_mf_config[func].config);

		val = (SHMEM_RD(bp, mf_cfg.func_mf_config[FUNC_0].e1hov_tag) &
		       FUNC_MF_CFG_E1HOV_TAG_MASK);
		if (val != FUNC_MF_CFG_E1HOV_TAG_DEFAULT)
			bp->e1hmf = 1;
		BNX2X_DEV_INFO("%s function mode\n",
			       IS_E1HMF(bp) ? "multi" : "single");

		if (IS_E1HMF(bp)) {
			val = (SHMEM_RD(bp,
					mf_cfg.func_mf_config[func].e1hov_tag) &
			       FUNC_MF_CFG_E1HOV_TAG_MASK);
			if (val != FUNC_MF_CFG_E1HOV_TAG_DEFAULT) {
				bp->e1hov = val;
				BNX2X_DEV_INFO("E1HOV for func %d is %d "
					       "(0x%04x)\n",
					       func, bp->e1hov, bp->e1hov);
			} else {
				BNX2X_ERROR("No valid E1HOV for func %d,"
					    "  aborting\n", func);
				rc = -EPERM;
			}
		} else {
			if (BP_E1HVN(bp)) {
				BNX2X_ERROR("VN %d in single function mode,"
					    "  aborting\n", BP_E1HVN(bp));
				rc = -EPERM;
			}
		}
	}

	if (!BP_NOMCP(bp)) {
		bnx2x_get_port_hwinfo(bp);

		bp->fw_seq = (SHMEM_RD(bp, func_mb[func].drv_mb_header) &
			      DRV_MSG_SEQ_NUMBER_MASK);
		BNX2X_DEV_INFO("fw_seq 0x%08x\n", bp->fw_seq);
	}

	if (IS_E1HMF(bp)) {
		val2 = SHMEM_RD(bp, mf_cfg.func_mf_config[func].mac_upper);
		val = SHMEM_RD(bp,  mf_cfg.func_mf_config[func].mac_lower);
		if ((val2 != FUNC_MF_CFG_UPPERMAC_DEFAULT) &&
		    (val != FUNC_MF_CFG_LOWERMAC_DEFAULT)) {
			bp->dev->dev_addr[0] = (u8)(val2 >> 8 & 0xff);
			bp->dev->dev_addr[1] = (u8)(val2 & 0xff);
			bp->dev->dev_addr[2] = (u8)(val >> 24 & 0xff);
			bp->dev->dev_addr[3] = (u8)(val >> 16 & 0xff);
			bp->dev->dev_addr[4] = (u8)(val >> 8  & 0xff);
			bp->dev->dev_addr[5] = (u8)(val & 0xff);
			memcpy(bp->link_params.mac_addr, bp->dev->dev_addr,
			       ETH_ALEN);
#ifdef ETHTOOL_GPERMADDR /* BNX2X_UPSTREAM */
			memcpy(bp->dev->perm_addr, bp->dev->dev_addr,
			       ETH_ALEN);
#endif
		}

		return rc;
	}

	if (BP_NOMCP(bp)) {
		/* only supposed to happen on emulation/FPGA */
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
		BNX2X_ERROR("warning: random MAC workaround active\n");
		random_ether_addr(bp->dev->dev_addr);
#else
		BNX2X_ERROR("warning: constant MAC workaround active\n");
		bp->dev->dev_addr[0] = 0;
		bp->dev->dev_addr[1] = 0x50;
		bp->dev->dev_addr[2] = 0xc2;
		bp->dev->dev_addr[3] = 0x2c;
		bp->dev->dev_addr[4] = (func + 1) * 0x10;
		bp->dev->dev_addr[5] = 0x00;
		memcpy(bp->link_params.mac_addr, bp->dev->dev_addr, ETH_ALEN);
#endif
#ifdef ETHTOOL_GPERMADDR /* BNX2X_UPSTREAM */
		memcpy(bp->dev->perm_addr, bp->dev->dev_addr, ETH_ALEN);
#endif
	}

	return rc;
}

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
static u16 bnx2x_select_queue(struct net_device *dev, struct sk_buff *skb);

static void __devinit bnx2x_init_safc(struct bnx2x *bp)
{
	int i, cos, max_cos, max_queues;

	bp->flags &= ~SAFC_TX_FLAG;

	for (i = 0; i < BNX2X_MAX_PRIORITY; i++)
		bp->pri_map[i] = 0;

	max_cos = BNX2X_MAX_COS;
	bp->cos_weight_sum = 0;
	for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
		bp->qs_per_cos[cos] = 0;
		bp->cos_min_rate[cos] = 0;
	}

	switch (bp->multi_mode) {
	case ETH_RSS_MODE_DISABLED:
		bp->qs_per_cos[0] = 1;
		break;

	case ETH_RSS_MODE_REGULAR:
		bp->qs_per_cos[0] = min_t(u32, num_online_cpus(),
					  BNX2X_MAX_QUEUES(bp));
		break;

	case ETH_RSS_MODE_VLAN_PRI:
	case ETH_RSS_MODE_E1HOV_PRI:
		if (CHIP_IS_E1H(bp)) {
			bp->flags |= SAFC_TX_FLAG;
			max_cos = BNX2X_MAX_TX_COS;
		}

		/* fallthrough */

	case ETH_RSS_MODE_IP_DSCP:
		/* COS 0 must have at least 1 queue */
		if (BNX2X_COS_QUEUES(0) == 0) {
			printk(KERN_ERR PFX
			       "Illegal number of queues per COS 0  "
			       "defaulting to 1\n");
			qs_per_cos |= 0x1;
		}

		for (i = 0; i < BNX2X_MAX_PRIORITY; i++) {
			cos = ((pri_map & (0xf << i*4)) >> i*4);
			if (cos < max_cos) {
				bp->pri_map[i] = cos;
				if (BNX2X_COS_QUEUES(cos) == 0) {
					printk(KERN_ERR PFX
					       "Illegal number of queues per "
					       "COS %d  defaulting to 1\n",
					       cos);
					qs_per_cos |= (0x1 << cos*8);
				}
			} else {
				printk(KERN_ERR PFX
				       "Illegal COS (%d) for priority %d  "
				       "defaulting to 0\n", cos, i);
				pri_map &= ~(0xf << i*4);
				bp->pri_map[i] = 0;
			}
		}

		max_queues = BNX2X_MAX_QUEUES(bp);
		for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
			i = BNX2X_COS_QUEUES(cos);
			if (i <= max_queues) {
				bp->qs_per_cos[cos] = i;
				max_queues -= i;
			} else {
				bp->qs_per_cos[cos] = max_queues;
				max_queues = 0;
			}
			if (bp->qs_per_cos[cos] == 0)
				for (i = 0; i < BNX2X_MAX_PRIORITY; i++)
					if (bp->pri_map[i] == cos) {
						printk(KERN_ERR PFX
						       "Illegal COS (%d) for "
						       "priority %d  "
						       "defaulting to 0\n",
						       cos, i);
						pri_map &= ~(0xf << i*4);
						bp->pri_map[i] = 0;
					}
		}

#if (LINUX_VERSION_CODE < 0x02061e) /* ! BNX2X_UPSTREAM */
		bp->dev->select_queue = bnx2x_select_queue;
#endif

		if (cos_min_rate == 0)
			break;

		for (cos = 0; cos < BNX2X_MAX_COS; cos++) {
			i = BNX2X_COS_RATE(cos);
			if (i > 100) {
				printk(KERN_ERR PFX
				       "Illegal rate (%d) for COS %d  "
				       "defaulting to 100\n", i, cos);
				i = 100;
				cos_min_rate |= (i << cos*8);
			}
			i *= 100;
			if (bp->qs_per_cos[cos]) {
				if (i == 0) {
					printk(KERN_ERR PFX
					       "Illegal rate for COS %d  "
					       "defaulting to 1\n", cos);
					i = DEF_MIN_RATE;
					cos_min_rate |= (i << cos*8);
				}
			} else {
				if (i) {
					printk(KERN_ERR PFX
					       "Illegal rate (%d) for COS %d "
					       "with no queues  "
					       "defaulting to 0\n", i, cos);
					i = 0;
					cos_min_rate &= ~(0xff << cos*8);
				}
			}
			bp->cos_min_rate[cos] = i;
			bp->cos_weight_sum += i;
		}
		break;

	default:
		bp->multi_mode = ETH_RSS_MODE_DISABLED;
		bp->qs_per_cos[0] = 1;
		break;
	}
}
#endif

static inline u16 bnx2x_vpd_lrdt_size(u8 *lrdt)
{
	return (u16)lrdt[1] + ((u16)lrdt[2] << 8);
}

static inline u8 bnx2x_vpd_srdt_size(u8 *srdt)
{
	return (*srdt) & BNX2X_VPD_SRDT_LEN_MASK;
}

static int __devinit bnx2x_vpd_find_tag(u8 *data, unsigned int start,
				      unsigned int len, u8 tagid)
{
	int i;

	for (i = start; i < len; ) {
		u8 val = data[i];

		if (val & BNX2X_VPD_LRDT) {
			/* Don't return success of the tag isn't complete */
			if (i + BNX2X_VPD_LRDT_TAG_SIZE > len)
				break;

			if (val == tagid)
				return i;

			i += BNX2X_VPD_LRDT_TAG_SIZE +
			     bnx2x_vpd_lrdt_size(&data[i]);
		} else {
			u8 tag = val & ~BNX2X_VPD_SRDT_LEN_MASK;

			if (tag == tagid)
				return i;

			if (tag == BNX2X_VPD_SRDT_SIN_END)
				break;

			i += BNX2X_VPD_SRDT_TAG_SIZE +
			     bnx2x_vpd_srdt_size(&data[i]);
		}
	}

	return -1;
}

static inline u8 bnx2x_vpd_info_field_size(u8 *info_field)
{
	return info_field[2];
}

static int __devinit bnx2x_vpd_rodata_find_if_keyword(u8 *rodata,
						    unsigned int start,
						    unsigned int rosize,
						    char *kw)
{
	int i;

	for (i = start; i + BNX2X_VPD_INFO_FLD_HDR_SIZE <= start + rosize;) {
		if (rodata[i + 0] == kw[0] &&
		    rodata[i + 1] == kw[1])
			return i;

		i += BNX2X_VPD_INFO_FLD_HDR_SIZE +
		     bnx2x_vpd_info_field_size(&rodata[i]);
	}

	return -1;
}

static void __devinit bnx2x_read_fwinfo(struct bnx2x *bp) {
	int cnt, i, block_end, rodi;
	char vpd_data[BNX2X_VPD_LEN+1];
	char str_id_reg[VENDOR_ID_LEN+1];
	char str_id_cap[VENDOR_ID_LEN+1];
	u8 len;

	cnt = pci_read_vpd(bp->pdev, 0, BNX2X_VPD_LEN, vpd_data);
	memset(bp->fw_ver, 0, sizeof(bp->fw_ver));

	if (cnt < BNX2X_VPD_LEN)
		goto out_not_found;

	i = bnx2x_vpd_find_tag(vpd_data, 0, BNX2X_VPD_LEN,
			     BNX2X_VPD_LRDT_RO_DATA);
	if (i < 0)
		goto out_not_found;
	

	block_end = i + BNX2X_VPD_LRDT_TAG_SIZE +
		    bnx2x_vpd_lrdt_size(&vpd_data[i]);

	i += BNX2X_VPD_LRDT_TAG_SIZE;

	if (block_end > BNX2X_VPD_LEN)
		goto out_not_found;
	
	rodi = bnx2x_vpd_rodata_find_if_keyword(vpd_data, i, block_end,
				   BNX2X_VPD_RO_KEYWORD_MANUFACTURERNO);
	if (rodi < 0 ) 
		goto out_not_found;

	len = bnx2x_vpd_info_field_size(&vpd_data[rodi]);
	
	if (len != VENDOR_ID_LEN) 
		goto out_not_found;

	rodi += BNX2X_VPD_INFO_FLD_HDR_SIZE;

	/* vendor specific info */
	snprintf(str_id_reg, VENDOR_ID_LEN + 1, "%04x", PCI_VENDOR_ID_DELL);
	snprintf(str_id_cap, VENDOR_ID_LEN + 1, "%04X", PCI_VENDOR_ID_DELL);
	if (!strncmp(str_id_reg, &vpd_data[rodi], VENDOR_ID_LEN) ||
	    !strncmp(str_id_cap, &vpd_data[rodi], VENDOR_ID_LEN)) {

		rodi = bnx2x_vpd_rodata_find_if_keyword(vpd_data, i, block_end,
						   BNX2X_VPD_RO_KEYWORD_VENDOR0);
		if (rodi >= 0) {
			
			len = bnx2x_vpd_info_field_size(&vpd_data[rodi]);

			rodi += BNX2X_VPD_INFO_FLD_HDR_SIZE;

			if (len < 32 && (len + rodi) <= BNX2X_VPD_LEN) {
				memcpy(bp->fw_ver, &vpd_data[rodi], len);
				bp->fw_ver[len] = ' ';
			}
		}
		return;
	}
out_not_found:
	return;
}

static int __devinit bnx2x_init_bp(struct bnx2x *bp)
{
	int func = BP_FUNC(bp);
	int timer_interval;
	int rc;

	/* Disable interrupt handling until HW is initialized */
	atomic_set(&bp->intr_sem, 1);
	smp_wmb(); /* Ensure that bp->intr_sem update is SMP-safe */

	mutex_init(&bp->port.phy_mutex);
	mutex_init(&bp->fw_mb_mutex);
#ifdef BCM_CNIC
	mutex_init(&bp->cnic_mutex);
#endif

#if defined(INIT_DELAYED_WORK_DEFERRABLE) || defined(INIT_WORK_NAR) || (VMWARE_ESX_DDK_VERSION >= 40000) /* BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	INIT_DELAYED_WORK(&bp->sp_task, bnx2x_sp_task);
#else
	INIT_WORK(&bp->sp_task, bnx2x_sp_task);
#endif
	INIT_WORK(&bp->reset_task, bnx2x_reset_task);
#else
	INIT_WORK(&bp->sp_task, bnx2x_sp_task, bp);
	INIT_WORK(&bp->reset_task, bnx2x_reset_task, bp);
#endif
#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	bp->sp_running = 0;
#endif

	rc = bnx2x_get_hwinfo(bp);

	bnx2x_read_fwinfo(bp);
	/* need to reset chip if undi was active */
	if (!BP_NOMCP(bp))
		bnx2x_undi_unload(bp);

	if (CHIP_REV_IS_FPGA(bp))
		printk(KERN_ERR PFX "FPGA detected\n");

	if (BP_NOMCP(bp) && (func == 0))
		printk(KERN_ERR PFX
		       "MCP disabled, must load devices in order!\n");

	/* Set multi queue mode and priority mapping */
	if ((multi_mode != ETH_RSS_MODE_DISABLED) &&
	    ((int_mode == INT_MODE_INTx) || (int_mode == INT_MODE_MSI))) {
		printk(KERN_ERR PFX
		      "Multi disabled since int_mode requested is not MSI-X\n");
		multi_mode = ETH_RSS_MODE_DISABLED;
	}
	bp->multi_mode = multi_mode;

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	bnx2x_init_safc(bp);
#endif

#ifdef NETIF_F_GRO /* BNX2X_UPSTREAM */
	bp->dev->features |= NETIF_F_GRO;
#endif

	/* Set TPA flags */
	if (disable_tpa) {
		bp->flags &= ~TPA_ENABLE_FLAG;
#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
		bp->dev->features &= ~NETIF_F_LRO;
#endif
	} else {
		bp->flags |= TPA_ENABLE_FLAG;
#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
		bp->dev->features |= NETIF_F_LRO;
#endif
	}

	if (CHIP_IS_E1(bp))
		bp->dropless_fc = 0;
	else
		bp->dropless_fc = dropless_fc;

	bp->mrrs = mrrs;

#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
	/* MCP workaround */
	if (BP_NOMCP(bp)) {
		/* [0x24c0]table size:
		   PCI_REG_GRC_PTR(func)->pci_msix_control = 0x10; */
		REG_WR(bp, 0x24c0, 0x10);
		/* [0x24c4]table pointer:
		   PCI_REG_GRC_PTR(func)->pci_msix_tbl_off_bir =
						0x440000 + 0x2000*func; */
		REG_WR(bp, 0x24c4, 0x440000 + 0x2000*func);
		/* [0x24c8]PBA pointer:
		   PCI_REG_GRC_PTR(func)->pci_msix_pba_off_bir =
						0x441800 + 0x2000*func; */
		REG_WR(bp, 0x24c8, 0x441800 + 0x2000*func);
	}
#endif
	bp->tx_ring_size = MAX_TX_AVAIL;
	bp->rx_ring_size = MAX_RX_AVAIL;

	bp->rx_csum = 1;

	/* make sure that the numbers are in the right granularity */
	bp->tx_ticks = (50 / (4 * BNX2X_BTR)) * (4 * BNX2X_BTR);
	bp->rx_ticks = (25 / (4 * BNX2X_BTR)) * (4 * BNX2X_BTR);

	timer_interval = (CHIP_REV_IS_SLOW(bp) ? 5*HZ : HZ);
	bp->current_interval = (poll ? poll : timer_interval);

	init_timer(&bp->timer);
	bp->timer.expires = jiffies + bp->current_interval;
	bp->timer.data = (unsigned long) bp;
	bp->timer.function = bnx2x_timer;

	return rc;
}

/*
 * ethtool service functions
 */

/* All ethtool functions called with rtnl_lock */

static int bnx2x_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct bnx2x *bp = netdev_priv(dev);

	cmd->supported = bp->port.supported;
	cmd->advertising = bp->port.advertising;

	if ((bp->state == BNX2X_STATE_OPEN) &&
	    !(bp->flags & MF_FUNC_DIS) &&
	    (bp->link_vars.link_up)) {
		cmd->speed = bp->link_vars.line_speed;
		cmd->duplex = bp->link_vars.duplex;
		if (IS_E1HMF(bp)) {
			u16 vn_max_rate;

			vn_max_rate =
				((bp->mf_config & FUNC_MF_CFG_MAX_BW_MASK) >>
				FUNC_MF_CFG_MAX_BW_SHIFT) * 100;
			if (vn_max_rate < cmd->speed)
				cmd->speed = vn_max_rate;
		}
	} else {
		cmd->speed = -1;
		cmd->duplex = -1;
	}

	if (bp->link_params.switch_cfg == SWITCH_CFG_10G) {
		u32 ext_phy_type =
			XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);

		switch (ext_phy_type) {
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_DIRECT:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8072:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8073:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8705:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8706:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8726:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8727:
			cmd->port = PORT_FIBRE;
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM8481:
		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_BCM84823:
			cmd->port = PORT_TP;
			break;

		case PORT_HW_CFG_XGXS_EXT_PHY_TYPE_FAILURE:
			BNX2X_ERR("XGXS PHY Failure detected 0x%x\n",
				  bp->link_params.ext_phy_config);
			break;

		default:
			DP(NETIF_MSG_LINK, "BAD XGXS ext_phy_config 0x%x\n",
			   bp->link_params.ext_phy_config);
			break;
		}
	} else
		cmd->port = PORT_TP;

	cmd->phy_address = bp->mdio.prtad;
	cmd->transceiver = XCVR_INTERNAL;

	if (bp->link_params.req_line_speed == SPEED_AUTO_NEG)
		cmd->autoneg = AUTONEG_ENABLE;
	else
		cmd->autoneg = AUTONEG_DISABLE;

	cmd->maxtxpkt = 0;
	cmd->maxrxpkt = 0;

	DP(NETIF_MSG_LINK, "ethtool_cmd: cmd %d\n"
	   DP_LEVEL "  supported 0x%x  advertising 0x%x  speed %d\n"
	   DP_LEVEL "  duplex %d  port %d  phy_address %d  transceiver %d\n"
	   DP_LEVEL "  autoneg %d  maxtxpkt %d  maxrxpkt %d\n",
	   cmd->cmd, cmd->supported, cmd->advertising, cmd->speed,
	   cmd->duplex, cmd->port, cmd->phy_address, cmd->transceiver,
	   cmd->autoneg, cmd->maxtxpkt, cmd->maxrxpkt);

	return 0;
}

static int bnx2x_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct bnx2x *bp = netdev_priv(dev);
	u32 advertising;

	if (IS_E1HMF(bp))
		return 0;

	DP(NETIF_MSG_LINK, "ethtool_cmd: cmd %d\n"
	   DP_LEVEL "  supported 0x%x  advertising 0x%x  speed %d\n"
	   DP_LEVEL "  duplex %d  port %d  phy_address %d  transceiver %d\n"
	   DP_LEVEL "  autoneg %d  maxtxpkt %d  maxrxpkt %d\n",
	   cmd->cmd, cmd->supported, cmd->advertising, cmd->speed,
	   cmd->duplex, cmd->port, cmd->phy_address, cmd->transceiver,
	   cmd->autoneg, cmd->maxtxpkt, cmd->maxrxpkt);

	if (cmd->autoneg == AUTONEG_ENABLE) {
		if (!(bp->port.supported & SUPPORTED_Autoneg)) {
			DP(NETIF_MSG_LINK, "Autoneg not supported\n");
			return -EINVAL;
		}

		/* advertise the requested speed and duplex if supported */
		cmd->advertising &= bp->port.supported;

		bp->link_params.req_line_speed = SPEED_AUTO_NEG;
		bp->link_params.req_duplex = DUPLEX_FULL;
		bp->port.advertising |= (ADVERTISED_Autoneg |
					 cmd->advertising);

	} else { /* forced speed */
		/* advertise the requested speed and duplex if supported */
		switch (cmd->speed) {
		case SPEED_10:
			if (cmd->duplex == DUPLEX_FULL) {
				if (!(bp->port.supported &
				      SUPPORTED_10baseT_Full)) {
					DP(NETIF_MSG_LINK,
					   "10M full not supported\n");
					return -EINVAL;
				}

				advertising = (ADVERTISED_10baseT_Full |
					       ADVERTISED_TP);
			} else {
				if (!(bp->port.supported &
				      SUPPORTED_10baseT_Half)) {
					DP(NETIF_MSG_LINK,
					   "10M half not supported\n");
					return -EINVAL;
				}

				advertising = (ADVERTISED_10baseT_Half |
					       ADVERTISED_TP);
			}
			break;

		case SPEED_100:
			if (cmd->duplex == DUPLEX_FULL) {
				if (!(bp->port.supported &
						SUPPORTED_100baseT_Full)) {
					DP(NETIF_MSG_LINK,
					   "100M full not supported\n");
					return -EINVAL;
				}

				advertising = (ADVERTISED_100baseT_Full |
					       ADVERTISED_TP);
			} else {
				if (!(bp->port.supported &
						SUPPORTED_100baseT_Half)) {
					DP(NETIF_MSG_LINK,
					   "100M half not supported\n");
					return -EINVAL;
				}

				advertising = (ADVERTISED_100baseT_Half |
					       ADVERTISED_TP);
			}
			break;

		case SPEED_1000:
			if (cmd->duplex != DUPLEX_FULL) {
				DP(NETIF_MSG_LINK, "1G half not supported\n");
				return -EINVAL;
			}

			if (!(bp->port.supported & SUPPORTED_1000baseT_Full)) {
				DP(NETIF_MSG_LINK, "1G full not supported\n");
				return -EINVAL;
			}

			advertising = (ADVERTISED_1000baseT_Full |
				       ADVERTISED_TP);
			break;

		case SPEED_2500:
			if (cmd->duplex != DUPLEX_FULL) {
				DP(NETIF_MSG_LINK,
				   "2.5G half not supported\n");
				return -EINVAL;
			}

			if (!(bp->port.supported & SUPPORTED_2500baseX_Full)) {
				DP(NETIF_MSG_LINK,
				   "2.5G full not supported\n");
				return -EINVAL;
			}

			advertising = (ADVERTISED_2500baseX_Full |
				       ADVERTISED_TP);
			break;

		case SPEED_10000:
			if (cmd->duplex != DUPLEX_FULL) {
				DP(NETIF_MSG_LINK, "10G half not supported\n");
				return -EINVAL;
			}

			if (!(bp->port.supported & SUPPORTED_10000baseT_Full)) {
				DP(NETIF_MSG_LINK, "10G full not supported\n");
				return -EINVAL;
			}

			advertising = (ADVERTISED_10000baseT_Full |
				       ADVERTISED_FIBRE);
			break;

		default:
			DP(NETIF_MSG_LINK, "Unsupported speed\n");
			return -EINVAL;
		}

		bp->link_params.req_line_speed = cmd->speed;
		bp->link_params.req_duplex = cmd->duplex;
		bp->port.advertising = advertising;
	}

	DP(NETIF_MSG_LINK, "req_line_speed %d\n"
	   DP_LEVEL "  req_duplex %d  advertising 0x%x\n",
	   bp->link_params.req_line_speed, bp->link_params.req_duplex,
	   bp->port.advertising);

	if (netif_running(dev)) {
		bnx2x_stats_handle(bp, STATS_EVENT_STOP);
		bnx2x_link_set(bp);
	}

	return 0;
}

#define IS_E1_ONLINE(info)	(((info) & RI_E1_ONLINE) == RI_E1_ONLINE)
#define IS_E1H_ONLINE(info)	(((info) & RI_E1H_ONLINE) == RI_E1H_ONLINE)

static int bnx2x_get_regs_len(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	int regdump_len = 0;
	int i;

	if (CHIP_IS_E1(bp)) {
		for (i = 0; i < REGS_COUNT; i++)
			if (IS_E1_ONLINE(reg_addrs[i].info))
				regdump_len += reg_addrs[i].size;

		for (i = 0; i < WREGS_COUNT_E1; i++)
			if (IS_E1_ONLINE(wreg_addrs_e1[i].info))
				regdump_len += wreg_addrs_e1[i].size *
					(1 + wreg_addrs_e1[i].read_regs_count);

	} else { /* E1H */
		for (i = 0; i < REGS_COUNT; i++)
			if (IS_E1H_ONLINE(reg_addrs[i].info))
				regdump_len += reg_addrs[i].size;

		for (i = 0; i < WREGS_COUNT_E1H; i++)
			if (IS_E1H_ONLINE(wreg_addrs_e1h[i].info))
				regdump_len += wreg_addrs_e1h[i].size *
					(1 + wreg_addrs_e1h[i].read_regs_count);
	}
	regdump_len *= 4;
	regdump_len += sizeof(struct dump_hdr);

	return regdump_len;
}

static void bnx2x_get_regs(struct net_device *dev,
			   struct ethtool_regs *regs, void *_p)
{
	u32 *p = _p, i, j;
	struct bnx2x *bp = netdev_priv(dev);
	struct dump_hdr dump_hdr = {0};

	regs->version = 0;
	memset(p, 0, regs->len);

	if (!netif_running(bp->dev))
		return;

	dump_hdr.hdr_size = (sizeof(struct dump_hdr) / 4) - 1;
	dump_hdr.dump_sign = dump_sign_all;
	dump_hdr.xstorm_waitp = REG_RD(bp, XSTORM_WAITP_ADDR);
	dump_hdr.tstorm_waitp = REG_RD(bp, TSTORM_WAITP_ADDR);
	dump_hdr.ustorm_waitp = REG_RD(bp, USTORM_WAITP_ADDR);
	dump_hdr.cstorm_waitp = REG_RD(bp, CSTORM_WAITP_ADDR);
	dump_hdr.info = CHIP_IS_E1(bp) ? RI_E1_ONLINE : RI_E1H_ONLINE;

	memcpy(p, &dump_hdr, sizeof(struct dump_hdr));
	p += dump_hdr.hdr_size + 1;

	if (CHIP_IS_E1(bp)) {
		for (i = 0; i < REGS_COUNT; i++)
			if (IS_E1_ONLINE(reg_addrs[i].info))
				for (j = 0; j < reg_addrs[i].size; j++)
					*p++ = REG_RD(bp,
						      reg_addrs[i].addr + j*4);

	} else { /* E1H */
		for (i = 0; i < REGS_COUNT; i++)
			if (IS_E1H_ONLINE(reg_addrs[i].info))
				for (j = 0; j < reg_addrs[i].size; j++)
					*p++ = REG_RD(bp,
						      reg_addrs[i].addr + j*4);
	}
}

#define PHY_FW_VER_LEN			10

static void bnx2x_get_drvinfo(struct net_device *dev,
			      struct ethtool_drvinfo *info)
{
	struct bnx2x *bp = netdev_priv(dev);
	u8 phy_fw_ver[PHY_FW_VER_LEN];

	strcpy(info->driver, DRV_MODULE_NAME);
	strcpy(info->version, DRV_MODULE_VERSION);

	phy_fw_ver[0] = '\0';
	if (bp->port.pmf) {
		bnx2x_acquire_phy_lock(bp);
		bnx2x_get_ext_phy_fw_version(&bp->link_params,
					     (bp->state != BNX2X_STATE_CLOSED),
					     phy_fw_ver, PHY_FW_VER_LEN);
		bnx2x_release_phy_lock(bp);
	}

	strncpy(info->fw_version, bp->fw_ver, 32);
	snprintf(info->fw_version + strlen(bp->fw_ver), 32 - strlen(bp->fw_ver),
		 "bc %d.%d.%d%s%s",
		 (bp->common.bc_ver & 0xff0000) >> 16,
		 (bp->common.bc_ver & 0xff00) >> 8,
		 (bp->common.bc_ver & 0xff),
		 ((phy_fw_ver[0] != '\0') ? " phy " : ""), phy_fw_ver);
	strcpy(info->bus_info, pci_name(bp->pdev));
	info->n_stats = BNX2X_NUM_STATS;
	info->testinfo_len = BNX2X_NUM_TESTS;
	info->eedump_len = bp->common.flash_size;
	info->regdump_len = bnx2x_get_regs_len(dev);
}

static void bnx2x_get_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (bp->flags & NO_WOL_FLAG) {
		wol->supported = 0;
		wol->wolopts = 0;
	} else {
		wol->supported = WAKE_MAGIC;
		if (bp->wol)
			wol->wolopts = WAKE_MAGIC;
		else
			wol->wolopts = 0;
	}
	memset(&wol->sopass, 0, sizeof(wol->sopass));
}

static int bnx2x_set_wol(struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (wol->wolopts & ~WAKE_MAGIC)
		return -EINVAL;

	if (wol->wolopts & WAKE_MAGIC) {
		if (bp->flags & NO_WOL_FLAG)
			return -EINVAL;

		bp->wol = 1;
	} else
		bp->wol = 0;

	return 0;
}

static u32 bnx2x_get_msglevel(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	return bp->msglevel;
}

static void bnx2x_set_msglevel(struct net_device *dev, u32 level)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (capable(CAP_NET_ADMIN))
		bp->msglevel = level;
}

static int bnx2x_nway_reset(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (!bp->port.pmf)
		return 0;

	if (netif_running(dev)) {
		bnx2x_stats_handle(bp, STATS_EVENT_STOP);
		bnx2x_link_set(bp);
	}

	return 0;
}

static u32 bnx2x_get_link(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (bp->flags & MF_FUNC_DIS)
		return 0;

	return bp->link_vars.link_up;
}

#if (LINUX_VERSION_CODE >= 0x020418) /* BNX2X_UPSTREAM */
static int bnx2x_get_eeprom_len(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	return bp->common.flash_size;
}
#endif

static int bnx2x_acquire_nvram_lock(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int count, i;
	u32 val = 0;

	/* adjust timeout for emulation/FPGA */
	count = NVRAM_TIMEOUT_COUNT;
	if (CHIP_REV_IS_SLOW(bp))
		count *= 100;

	/* request access to nvram interface */
	REG_WR(bp, MCP_REG_MCPR_NVM_SW_ARB,
	       (MCPR_NVM_SW_ARB_ARB_REQ_SET1 << port));

	for (i = 0; i < count*10; i++) {
		val = REG_RD(bp, MCP_REG_MCPR_NVM_SW_ARB);
		if (val & (MCPR_NVM_SW_ARB_ARB_ARB1 << port))
			break;

		udelay(5);
	}

	if (!(val & (MCPR_NVM_SW_ARB_ARB_ARB1 << port))) {
		DP(BNX2X_MSG_NVM, "cannot get access to nvram interface\n");
		return -EBUSY;
	}

	return 0;
}

static int bnx2x_release_nvram_lock(struct bnx2x *bp)
{
	int port = BP_PORT(bp);
	int count, i;
	u32 val = 0;

	/* adjust timeout for emulation/FPGA */
	count = NVRAM_TIMEOUT_COUNT;
	if (CHIP_REV_IS_SLOW(bp))
		count *= 100;

	/* relinquish nvram interface */
	REG_WR(bp, MCP_REG_MCPR_NVM_SW_ARB,
	       (MCPR_NVM_SW_ARB_ARB_REQ_CLR1 << port));

	for (i = 0; i < count*10; i++) {
		val = REG_RD(bp, MCP_REG_MCPR_NVM_SW_ARB);
		if (!(val & (MCPR_NVM_SW_ARB_ARB_ARB1 << port)))
			break;

		udelay(5);
	}

	if (val & (MCPR_NVM_SW_ARB_ARB_ARB1 << port)) {
		DP(BNX2X_MSG_NVM, "cannot free access to nvram interface\n");
		return -EBUSY;
	}

	return 0;
}

static void bnx2x_enable_nvram_access(struct bnx2x *bp)
{
	u32 val;

	val = REG_RD(bp, MCP_REG_MCPR_NVM_ACCESS_ENABLE);

	/* enable both bits, even on read */
	REG_WR(bp, MCP_REG_MCPR_NVM_ACCESS_ENABLE,
	       (val | MCPR_NVM_ACCESS_ENABLE_EN |
		      MCPR_NVM_ACCESS_ENABLE_WR_EN));
}

static void bnx2x_disable_nvram_access(struct bnx2x *bp)
{
	u32 val;

	val = REG_RD(bp, MCP_REG_MCPR_NVM_ACCESS_ENABLE);

	/* disable both bits, even after read */
	REG_WR(bp, MCP_REG_MCPR_NVM_ACCESS_ENABLE,
	       (val & ~(MCPR_NVM_ACCESS_ENABLE_EN |
			MCPR_NVM_ACCESS_ENABLE_WR_EN)));
}

static int bnx2x_nvram_read_dword(struct bnx2x *bp, u32 offset, __be32 *ret_val,
				  u32 cmd_flags)
{
	int count, i, rc;
	u32 val;

	/* build the command word */
	cmd_flags |= MCPR_NVM_COMMAND_DOIT;

	/* need to clear DONE bit separately */
	REG_WR(bp, MCP_REG_MCPR_NVM_COMMAND, MCPR_NVM_COMMAND_DONE);

	/* address of the NVRAM to read from */
	REG_WR(bp, MCP_REG_MCPR_NVM_ADDR,
	       (offset & MCPR_NVM_ADDR_NVM_ADDR_VALUE));

	/* issue a read command */
	REG_WR(bp, MCP_REG_MCPR_NVM_COMMAND, cmd_flags);

	/* adjust timeout for emulation/FPGA */
	count = NVRAM_TIMEOUT_COUNT;
	if (CHIP_REV_IS_SLOW(bp))
		count *= 100;

	/* wait for completion */
	*ret_val = 0;
	rc = -EBUSY;
	for (i = 0; i < count; i++) {
		udelay(5);
		val = REG_RD(bp, MCP_REG_MCPR_NVM_COMMAND);

		if (val & MCPR_NVM_COMMAND_DONE) {
			val = REG_RD(bp, MCP_REG_MCPR_NVM_READ);
			/* we read nvram data in cpu order
			 * but ethtool sees it as an array of bytes
			 * converting to big-endian will do the work */
			*ret_val = cpu_to_be32(val);
			rc = 0;
			break;
		}
	}

	return rc;
}

static int bnx2x_nvram_read(struct bnx2x *bp, u32 offset, u8 *ret_buf,
			    int buf_size)
{
	int rc;
	u32 cmd_flags;
	__be32 val;

	if ((offset & 0x03) || (buf_size & 0x03) || (buf_size == 0)) {
		DP(BNX2X_MSG_NVM,
		   "Invalid parameter: offset 0x%x  buf_size 0x%x\n",
		   offset, buf_size);
		return -EINVAL;
	}

	if (offset + buf_size > bp->common.flash_size) {
		DP(BNX2X_MSG_NVM, "Invalid parameter: offset (0x%x) +"
				  " buf_size (0x%x) > flash_size (0x%x)\n",
		   offset, buf_size, bp->common.flash_size);
		return -EINVAL;
	}

	/* request access to nvram interface */
	rc = bnx2x_acquire_nvram_lock(bp);
	if (rc)
		return rc;

	/* enable access to nvram interface */
	bnx2x_enable_nvram_access(bp);

	/* read the first word(s) */
	cmd_flags = MCPR_NVM_COMMAND_FIRST;
	while ((buf_size > sizeof(u32)) && (rc == 0)) {
		rc = bnx2x_nvram_read_dword(bp, offset, &val, cmd_flags);
		memcpy(ret_buf, &val, 4);

		/* advance to the next dword */
		offset += sizeof(u32);
		ret_buf += sizeof(u32);
		buf_size -= sizeof(u32);
		cmd_flags = 0;
	}

	if (rc == 0) {
		cmd_flags |= MCPR_NVM_COMMAND_LAST;
		rc = bnx2x_nvram_read_dword(bp, offset, &val, cmd_flags);
		memcpy(ret_buf, &val, 4);
	}

	/* disable access to nvram interface */
	bnx2x_disable_nvram_access(bp);
	bnx2x_release_nvram_lock(bp);

	return rc;
}

static int bnx2x_get_eeprom(struct net_device *dev,
			    struct ethtool_eeprom *eeprom, u8 *eebuf)
{
	struct bnx2x *bp = netdev_priv(dev);
	int rc;

	if (!netif_running(dev))
		return -EAGAIN;

	DP(BNX2X_MSG_NVM, "ethtool_eeprom: cmd %d\n"
	   DP_LEVEL "  magic 0x%x  offset 0x%x (%d)  len 0x%x (%d)\n",
	   eeprom->cmd, eeprom->magic, eeprom->offset, eeprom->offset,
	   eeprom->len, eeprom->len);

	/* parameters already validated in ethtool_get_eeprom */

	rc = bnx2x_nvram_read(bp, eeprom->offset, eebuf, eeprom->len);

	return rc;
}

static int bnx2x_nvram_write_dword(struct bnx2x *bp, u32 offset, u32 val,
				   u32 cmd_flags)
{
	int count, i, rc;

	/* build the command word */
	cmd_flags |= MCPR_NVM_COMMAND_DOIT | MCPR_NVM_COMMAND_WR;

	/* need to clear DONE bit separately */
	REG_WR(bp, MCP_REG_MCPR_NVM_COMMAND, MCPR_NVM_COMMAND_DONE);

	/* write the data */
	REG_WR(bp, MCP_REG_MCPR_NVM_WRITE, val);

	/* address of the NVRAM to write to */
	REG_WR(bp, MCP_REG_MCPR_NVM_ADDR,
	       (offset & MCPR_NVM_ADDR_NVM_ADDR_VALUE));

	/* issue the write command */
	REG_WR(bp, MCP_REG_MCPR_NVM_COMMAND, cmd_flags);

	/* adjust timeout for emulation/FPGA */
	count = NVRAM_TIMEOUT_COUNT;
	if (CHIP_REV_IS_SLOW(bp))
		count *= 100;

	/* wait for completion */
	rc = -EBUSY;
	for (i = 0; i < count; i++) {
		udelay(5);
		val = REG_RD(bp, MCP_REG_MCPR_NVM_COMMAND);
		if (val & MCPR_NVM_COMMAND_DONE) {
			rc = 0;
			break;
		}
	}

	return rc;
}

#define BYTE_OFFSET(offset)		(8 * (offset & 0x03))

static int bnx2x_nvram_write1(struct bnx2x *bp, u32 offset, u8 *data_buf,
			      int buf_size)
{
	int rc;
	u32 cmd_flags;
	u32 align_offset;
	__be32 val;

	if (offset + buf_size > bp->common.flash_size) {
		DP(BNX2X_MSG_NVM, "Invalid parameter: offset (0x%x) +"
				  " buf_size (0x%x) > flash_size (0x%x)\n",
		   offset, buf_size, bp->common.flash_size);
		return -EINVAL;
	}

	/* request access to nvram interface */
	rc = bnx2x_acquire_nvram_lock(bp);
	if (rc)
		return rc;

	/* enable access to nvram interface */
	bnx2x_enable_nvram_access(bp);

	cmd_flags = (MCPR_NVM_COMMAND_FIRST | MCPR_NVM_COMMAND_LAST);
	align_offset = (offset & ~0x03);
	rc = bnx2x_nvram_read_dword(bp, align_offset, &val, cmd_flags);

	if (rc == 0) {
		val &= ~(0xff << BYTE_OFFSET(offset));
		val |= (*data_buf << BYTE_OFFSET(offset));

		/* nvram data is returned as an array of bytes
		 * convert it back to cpu order */
		val = be32_to_cpu(val);

		rc = bnx2x_nvram_write_dword(bp, align_offset, val,
					     cmd_flags);
	}

	/* disable access to nvram interface */
	bnx2x_disable_nvram_access(bp);
	bnx2x_release_nvram_lock(bp);

	return rc;
}

static int bnx2x_nvram_write(struct bnx2x *bp, u32 offset, u8 *data_buf,
			     int buf_size)
{
	int rc;
	u32 cmd_flags;
	u32 val;
	u32 written_so_far;

	if (buf_size == 1)	/* ethtool */
		return bnx2x_nvram_write1(bp, offset, data_buf, buf_size);

	if ((offset & 0x03) || (buf_size & 0x03) || (buf_size == 0)) {
		DP(BNX2X_MSG_NVM,
		   "Invalid parameter: offset 0x%x  buf_size 0x%x\n",
		   offset, buf_size);
		return -EINVAL;
	}

	if (offset + buf_size > bp->common.flash_size) {
		DP(BNX2X_MSG_NVM, "Invalid parameter: offset (0x%x) +"
				  " buf_size (0x%x) > flash_size (0x%x)\n",
		   offset, buf_size, bp->common.flash_size);
		return -EINVAL;
	}

	/* request access to nvram interface */
	rc = bnx2x_acquire_nvram_lock(bp);
	if (rc)
		return rc;

	/* enable access to nvram interface */
	bnx2x_enable_nvram_access(bp);

	written_so_far = 0;
	cmd_flags = MCPR_NVM_COMMAND_FIRST;
	while ((written_so_far < buf_size) && (rc == 0)) {
		if (written_so_far == (buf_size - sizeof(u32)))
			cmd_flags |= MCPR_NVM_COMMAND_LAST;
		else if (((offset + 4) % NVRAM_PAGE_SIZE) == 0)
			cmd_flags |= MCPR_NVM_COMMAND_LAST;
		else if ((offset % NVRAM_PAGE_SIZE) == 0)
			cmd_flags |= MCPR_NVM_COMMAND_FIRST;

		memcpy(&val, data_buf, 4);

		rc = bnx2x_nvram_write_dword(bp, offset, val, cmd_flags);

		/* advance to the next dword */
		offset += sizeof(u32);
		data_buf += sizeof(u32);
		written_so_far += sizeof(u32);
		cmd_flags = 0;
	}

	/* disable access to nvram interface */
	bnx2x_disable_nvram_access(bp);
	bnx2x_release_nvram_lock(bp);

	return rc;
}

static int bnx2x_set_eeprom(struct net_device *dev,
			    struct ethtool_eeprom *eeprom, u8 *eebuf)
{
	struct bnx2x *bp = netdev_priv(dev);
	int port = BP_PORT(bp);
	int rc = 0;

	if (!netif_running(dev))
		return -EAGAIN;

	DP(BNX2X_MSG_NVM, "ethtool_eeprom: cmd %d\n"
	   DP_LEVEL "  magic 0x%x  offset 0x%x (%d)  len 0x%x (%d)\n",
	   eeprom->cmd, eeprom->magic, eeprom->offset, eeprom->offset,
	   eeprom->len, eeprom->len);

	/* parameters already validated in ethtool_set_eeprom */

	/* PHY eeprom can be accessed only by the PMF */
	if ((eeprom->magic >= 0x50485900) && (eeprom->magic <= 0x504859FF) &&
	    !bp->port.pmf)
		return -EINVAL;

	if (eeprom->magic == 0x50485950) {
		/* 'PHYP' (0x50485950): prepare phy for FW upgrade */
		bnx2x_stats_handle(bp, STATS_EVENT_STOP);

		bnx2x_acquire_phy_lock(bp);
		rc |= bnx2x_link_reset(&bp->link_params,
				       &bp->link_vars, 0);
		if (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config) ==
					PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101)
			bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_0,
				       MISC_REGISTERS_GPIO_HIGH, port);
		bnx2x_release_phy_lock(bp);
		bnx2x_link_report(bp);

	} else if (eeprom->magic == 0x50485952) {
		/* 'PHYR' (0x50485952): re-init link after FW upgrade */
		if (bp->state == BNX2X_STATE_OPEN) {
			bnx2x_acquire_phy_lock(bp);
			rc |= bnx2x_link_reset(&bp->link_params,
					       &bp->link_vars, 1);

			rc |= bnx2x_phy_init(&bp->link_params,
					     &bp->link_vars);
			bnx2x_release_phy_lock(bp);
			bnx2x_calc_fc_adv(bp);
		}
	} else if (eeprom->magic == 0x53985943) {
		/* 'PHYC' (0x53985943): PHY FW upgrade completed */
		if (XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config) ==
				       PORT_HW_CFG_XGXS_EXT_PHY_TYPE_SFX7101) {
			u8 ext_phy_addr =
			     XGXS_EXT_PHY_ADDR(bp->link_params.ext_phy_config);

			/* DSP Remove Download Mode */
			bnx2x_set_gpio(bp, MISC_REGISTERS_GPIO_0,
				       MISC_REGISTERS_GPIO_LOW, port);

			bnx2x_acquire_phy_lock(bp);

			bnx2x_sfx7101_sp_sw_reset(bp, port, ext_phy_addr);

			/* wait 0.5 sec to allow it to run */
			msleep(500);
			bnx2x_ext_phy_hw_reset(bp, port);
			msleep(500);
			bnx2x_release_phy_lock(bp);
		}
	} else
		rc = bnx2x_nvram_write(bp, eeprom->offset, eebuf, eeprom->len);

	return rc;
}

static int bnx2x_get_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *coal)
{
	struct bnx2x *bp = netdev_priv(dev);

	memset(coal, 0, sizeof(struct ethtool_coalesce));

	coal->rx_coalesce_usecs = bp->rx_ticks;
	coal->tx_coalesce_usecs = bp->tx_ticks;

	return 0;
}

static int bnx2x_set_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *coal)
{
	struct bnx2x *bp = netdev_priv(dev);

	bp->rx_ticks = (u16)coal->rx_coalesce_usecs;
	if (bp->rx_ticks > BNX2X_MAX_COALESCE_TOUT)
		bp->rx_ticks = BNX2X_MAX_COALESCE_TOUT;

	bp->tx_ticks = (u16)coal->tx_coalesce_usecs;
	if (bp->tx_ticks > BNX2X_MAX_COALESCE_TOUT)
		bp->tx_ticks = BNX2X_MAX_COALESCE_TOUT;

	if (netif_running(dev))
		bnx2x_update_coalesce(bp);

	return 0;
}

static void bnx2x_get_ringparam(struct net_device *dev,
				struct ethtool_ringparam *ering)
{
	struct bnx2x *bp = netdev_priv(dev);

	ering->rx_max_pending = MAX_RX_AVAIL;
	ering->rx_mini_max_pending = 0;
	ering->rx_jumbo_max_pending = 0;

	ering->rx_pending = bp->rx_ring_size;
	ering->rx_mini_pending = 0;
	ering->rx_jumbo_pending = 0;

	ering->tx_max_pending = MAX_TX_AVAIL;
	ering->tx_pending = bp->tx_ring_size;
}

static int bnx2x_set_ringparam(struct net_device *dev,
			       struct ethtool_ringparam *ering)
{
	struct bnx2x *bp = netdev_priv(dev);
	int rc = 0;

	if ((ering->rx_pending > MAX_RX_AVAIL) ||
	    (ering->tx_pending > MAX_TX_AVAIL) ||
	    (ering->tx_pending <= MAX_SKB_FRAGS + 4))
		return -EINVAL;

	bp->rx_ring_size = ering->rx_pending;
	bp->tx_ring_size = ering->tx_pending;

	if (netif_running(dev)) {
		bnx2x_nic_unload(bp, UNLOAD_NORMAL);
		rc = bnx2x_nic_load(bp, LOAD_NORMAL);
	}

	return rc;
}

static void bnx2x_get_pauseparam(struct net_device *dev,
				 struct ethtool_pauseparam *epause)
{
	struct bnx2x *bp = netdev_priv(dev);

	epause->autoneg = (bp->link_params.req_flow_ctrl ==
			   BNX2X_FLOW_CTRL_AUTO) &&
			  (bp->link_params.req_line_speed == SPEED_AUTO_NEG);

	epause->rx_pause = ((bp->link_vars.flow_ctrl & BNX2X_FLOW_CTRL_RX) ==
			    BNX2X_FLOW_CTRL_RX);
	epause->tx_pause = ((bp->link_vars.flow_ctrl & BNX2X_FLOW_CTRL_TX) ==
			    BNX2X_FLOW_CTRL_TX);

	DP(NETIF_MSG_LINK, "ethtool_pauseparam: cmd %d\n"
	   DP_LEVEL "  autoneg %d  rx_pause %d  tx_pause %d\n",
	   epause->cmd, epause->autoneg, epause->rx_pause, epause->tx_pause);
}

static int bnx2x_set_pauseparam(struct net_device *dev,
				struct ethtool_pauseparam *epause)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (IS_E1HMF(bp))
		return 0;

	DP(NETIF_MSG_LINK, "ethtool_pauseparam: cmd %d\n"
	   DP_LEVEL "  autoneg %d  rx_pause %d  tx_pause %d\n",
	   epause->cmd, epause->autoneg, epause->rx_pause, epause->tx_pause);

	bp->link_params.req_flow_ctrl = BNX2X_FLOW_CTRL_AUTO;

	if (epause->rx_pause)
		bp->link_params.req_flow_ctrl |= BNX2X_FLOW_CTRL_RX;

	if (epause->tx_pause)
		bp->link_params.req_flow_ctrl |= BNX2X_FLOW_CTRL_TX;

	if (bp->link_params.req_flow_ctrl == BNX2X_FLOW_CTRL_AUTO)
		bp->link_params.req_flow_ctrl = BNX2X_FLOW_CTRL_NONE;

	if (epause->autoneg) {
		if (!(bp->port.supported & SUPPORTED_Autoneg)) {
			DP(NETIF_MSG_LINK, "autoneg not supported\n");
			return -EINVAL;
		}

		if (bp->link_params.req_line_speed == SPEED_AUTO_NEG)
			bp->link_params.req_flow_ctrl = BNX2X_FLOW_CTRL_AUTO;
	}

	DP(NETIF_MSG_LINK,
	   "req_flow_ctrl 0x%x\n", bp->link_params.req_flow_ctrl);

	if (netif_running(dev)) {
		bnx2x_stats_handle(bp, STATS_EVENT_STOP);
		bnx2x_link_set(bp);
	}

	return 0;
}

#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
static int bnx2x_set_flags(struct net_device *dev, u32 data)
{
	struct bnx2x *bp = netdev_priv(dev);
	int changed = 0;
	int rc = 0;

	/* TPA requires Rx CSUM offloading */
	if ((data & ETH_FLAG_LRO) && bp->rx_csum) {
#ifndef __NO_TPA__
		if (!(dev->features & NETIF_F_LRO)) {
			dev->features |= NETIF_F_LRO;
			bp->flags |= TPA_ENABLE_FLAG;
			changed = 1;
		}
#else
		rc = -EINVAL;
#endif
	} else if (dev->features & NETIF_F_LRO) {
		dev->features &= ~NETIF_F_LRO;
		bp->flags &= ~TPA_ENABLE_FLAG;
		changed = 1;
	}

	if (changed && netif_running(dev)) {
		bnx2x_nic_unload(bp, UNLOAD_NORMAL);
		rc = bnx2x_nic_load(bp, LOAD_NORMAL);
	}

	return rc;
}
#endif

static u32 bnx2x_get_rx_csum(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	return bp->rx_csum;
}

static int bnx2x_set_rx_csum(struct net_device *dev, u32 data)
{
	struct bnx2x *bp = netdev_priv(dev);
	int rc = 0;

	bp->rx_csum = data;

	/* Disable TPA, when Rx CSUM is disabled. Otherwise all
	   TPA'ed packets will be discarded due to wrong TCP CSUM */
	if (!data) {
#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
		u32 flags = ethtool_op_get_flags(dev);

		rc = bnx2x_set_flags(dev, (flags & ~ETH_FLAG_LRO));
#else
		bp->flags &= ~TPA_ENABLE_FLAG;
		if (netif_running(dev)) {
			bnx2x_nic_unload(bp, UNLOAD_NORMAL);
			rc = bnx2x_nic_load(bp, LOAD_NORMAL);
		}
#endif
	}

	return rc;
}

#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
static int bnx2x_set_tso(struct net_device *dev, u32 data)
{
	if (data) {
		dev->features |= (NETIF_F_TSO | NETIF_F_TSO_ECN);
#ifdef NETIF_F_TSO6 /* BNX2X_UPSTREAM */
		dev->features |= NETIF_F_TSO6;
#endif
	} else {
		dev->features &= ~(NETIF_F_TSO | NETIF_F_TSO_ECN);
#ifdef NETIF_F_TSO6 /* BNX2X_UPSTREAM */
		dev->features &= ~NETIF_F_TSO6;
#endif
	}

	return 0;
}
#endif

static const struct {
	char string[ETH_GSTRING_LEN];
} bnx2x_tests_str_arr[BNX2X_NUM_TESTS] = {
	{ "register_test (offline)" },
	{ "memory_test (offline)" },
	{ "loopback_test (offline)" },
	{ "nvram_test (online)" },
	{ "interrupt_test (online)" },
	{ "link_test (online)" },
	{ "idle check (online)" }
};

static int bnx2x_test_registers(struct bnx2x *bp)
{
	int idx, i, rc = -ENODEV;
	u32 wr_val = 0;
	int port = BP_PORT(bp);
	static const struct {
		u32 offset0;
		u32 offset1;
		u32 mask;
	} reg_tbl[] = {
/* 0 */		{ BRB1_REG_PAUSE_LOW_THRESHOLD_0,      4, 0x000003ff },
		{ DORQ_REG_DB_ADDR0,                   4, 0xffffffff },
		{ HC_REG_AGG_INT_0,                    4, 0x000003ff },
		{ PBF_REG_MAC_IF0_ENABLE,              4, 0x00000001 },
		{ PBF_REG_P0_INIT_CRD,                 4, 0x000007ff },
		{ PRS_REG_CID_PORT_0,                  4, 0x00ffffff },
		{ PXP2_REG_PSWRQ_CDU0_L2P,             4, 0x000fffff },
		{ PXP2_REG_RQ_CDU0_EFIRST_MEM_ADDR,    8, 0x0003ffff },
		{ PXP2_REG_PSWRQ_TM0_L2P,              4, 0x000fffff },
		{ PXP2_REG_RQ_USDM0_EFIRST_MEM_ADDR,   8, 0x0003ffff },
/* 10 */	{ PXP2_REG_PSWRQ_TSDM0_L2P,            4, 0x000fffff },
		{ QM_REG_CONNNUM_0,                    4, 0x000fffff },
		{ TM_REG_LIN0_MAX_ACTIVE_CID,          4, 0x0003ffff },
		{ SRC_REG_KEYRSS0_0,                  40, 0xffffffff },
		{ SRC_REG_KEYRSS0_7,                  40, 0xffffffff },
		{ XCM_REG_WU_DA_SET_TMR_CNT_FLG_CMD00, 4, 0x00000001 },
		{ XCM_REG_WU_DA_CNT_CMD00,             4, 0x00000003 },
		{ XCM_REG_GLB_DEL_ACK_MAX_CNT_0,       4, 0x000000ff },
		{ NIG_REG_LLH0_T_BIT,                  4, 0x00000001 },
		{ NIG_REG_EMAC0_IN_EN,                 4, 0x00000001 },
/* 20 */	{ NIG_REG_BMAC0_IN_EN,                 4, 0x00000001 },
		{ NIG_REG_XCM0_OUT_EN,                 4, 0x00000001 },
		{ NIG_REG_BRB0_OUT_EN,                 4, 0x00000001 },
		{ NIG_REG_LLH0_XCM_MASK,               4, 0x00000007 },
		{ NIG_REG_LLH0_ACPI_PAT_6_LEN,        68, 0x000000ff },
		{ NIG_REG_LLH0_ACPI_PAT_0_CRC,        68, 0xffffffff },
		{ NIG_REG_LLH0_DEST_MAC_0_0,         160, 0xffffffff },
		{ NIG_REG_LLH0_DEST_IP_0_1,          160, 0xffffffff },
		{ NIG_REG_LLH0_IPV4_IPV6_0,          160, 0x00000001 },
		{ NIG_REG_LLH0_DEST_UDP_0,           160, 0x0000ffff },
/* 30 */	{ NIG_REG_LLH0_DEST_TCP_0,           160, 0x0000ffff },
		{ NIG_REG_LLH0_VLAN_ID_0,            160, 0x00000fff },
		{ NIG_REG_XGXS_SERDES0_MODE_SEL,       4, 0x00000001 },
		{ NIG_REG_LED_CONTROL_OVERRIDE_TRAFFIC_P0, 4, 0x00000001 },
		{ NIG_REG_STATUS_INTERRUPT_PORT0,      4, 0x07ffffff },
		{ NIG_REG_XGXS0_CTRL_EXTREMOTEMDIOST, 24, 0x00000001 },
		{ NIG_REG_SERDES0_CTRL_PHY_ADDR,      16, 0x0000001f },

		{ 0xffffffff, 0, 0x00000000 }
	};

	if (!netif_running(bp->dev))
		return rc;

	/* Repeat the test twice:
	   First by writing 0x00000000, second by writing 0xffffffff */
	for (idx = 0; idx < 2; idx++) {

		switch (idx) {
		case 0:
			wr_val = 0;
			break;
		case 1:
			wr_val = 0xffffffff;
			break;
		}

		for (i = 0; reg_tbl[i].offset0 != 0xffffffff; i++) {
			u32 offset, mask, save_val, val;

			offset = reg_tbl[i].offset0 + port*reg_tbl[i].offset1;
			mask = reg_tbl[i].mask;

			save_val = REG_RD(bp, offset);

			REG_WR(bp, offset, (wr_val & mask));
			val = REG_RD(bp, offset);

			/* Restore the original register's value */
			REG_WR(bp, offset, save_val);

			/* verify value is as expected */
			if ((val & mask) != (wr_val & mask)) {
				DP(NETIF_MSG_PROBE,
				   "offset 0x%x: val 0x%x != 0x%x mask 0x%x\n",
				   offset, val, wr_val, mask);
				goto test_reg_exit;
			}
		}
	}

	rc = 0;

test_reg_exit:
	return rc;
}

static int bnx2x_test_memory(struct bnx2x *bp)
{
	int i, j, rc = -ENODEV;
	u32 val;
	static const struct {
		u32 offset;
		int size;
	} mem_tbl[] = {
		{ CCM_REG_XX_DESCR_TABLE,   CCM_REG_XX_DESCR_TABLE_SIZE },
		{ CFC_REG_ACTIVITY_COUNTER, CFC_REG_ACTIVITY_COUNTER_SIZE },
		{ CFC_REG_LINK_LIST,        CFC_REG_LINK_LIST_SIZE },
		{ DMAE_REG_CMD_MEM,         DMAE_REG_CMD_MEM_SIZE },
		{ TCM_REG_XX_DESCR_TABLE,   TCM_REG_XX_DESCR_TABLE_SIZE },
		{ UCM_REG_XX_DESCR_TABLE,   UCM_REG_XX_DESCR_TABLE_SIZE },
		{ XCM_REG_XX_DESCR_TABLE,   XCM_REG_XX_DESCR_TABLE_SIZE },

		{ 0xffffffff, 0 }
	};
	static const struct {
		char *name;
		u32 offset;
		u32 e1_mask;
		u32 e1h_mask;
	} prty_tbl[] = {
		{ "CCM_PRTY_STS",  CCM_REG_CCM_PRTY_STS,   0x3ffc0, 0 },
		{ "CFC_PRTY_STS",  CFC_REG_CFC_PRTY_STS,   0x2,     0x2 },
		{ "DMAE_PRTY_STS", DMAE_REG_DMAE_PRTY_STS, 0,       0 },
		{ "TCM_PRTY_STS",  TCM_REG_TCM_PRTY_STS,   0x3ffc0, 0 },
		{ "UCM_PRTY_STS",  UCM_REG_UCM_PRTY_STS,   0x3ffc0, 0 },
		{ "XCM_PRTY_STS",  XCM_REG_XCM_PRTY_STS,   0x3ffc1, 0 },

		{ NULL, 0xffffffff, 0, 0 }
	};

	if (!netif_running(bp->dev))
		return rc;

	/* Go through all the memories */
	for (i = 0; mem_tbl[i].offset != 0xffffffff; i++)
		for (j = 0; j < mem_tbl[i].size; j++)
			REG_RD(bp, mem_tbl[i].offset + j*4);

	/* Check the parity status */
	for (i = 0; prty_tbl[i].offset != 0xffffffff; i++) {
		val = REG_RD(bp, prty_tbl[i].offset);
		if ((CHIP_IS_E1(bp) && (val & ~(prty_tbl[i].e1_mask))) ||
		    (CHIP_IS_E1H(bp) && (val & ~(prty_tbl[i].e1h_mask)))) {
			DP(NETIF_MSG_HW,
			   "%s is 0x%x\n", prty_tbl[i].name, val);
			goto test_mem_exit;
		}
	}

	rc = 0;

test_mem_exit:
	return rc;
}

static void bnx2x_wait_for_link(struct bnx2x *bp, u8 link_up)
{
	int cnt = 1000;

	if (link_up)
		while (bnx2x_link_test(bp) && cnt--)
			msleep(10);
}

static int bnx2x_run_loopback(struct bnx2x *bp, int loopback_mode, u8 link_up)
{
	unsigned int pkt_size, num_pkts, i;
	struct sk_buff *skb;
	unsigned char *packet;
	struct bnx2x_fastpath *fp_rx = &bp->fp[0];
	struct bnx2x_fastpath *fp_tx = &bp->fp[0];
	u16 tx_start_idx, tx_idx;
	u16 rx_start_idx, rx_idx;
	u16 pkt_prod, bd_prod;
	struct sw_tx_bd *tx_buf;
	struct eth_tx_start_bd *tx_start_bd;
	struct eth_tx_parse_bd *pbd = NULL;
	dma_addr_t mapping;
	union eth_rx_cqe *cqe;
	u8 cqe_fp_flags;
	struct sw_rx_bd *rx_buf;
	u16 len;
	int rc = -ENODEV;

	/* check the loopback mode */
	switch (loopback_mode) {
	case BNX2X_PHY_LOOPBACK:
		if (bp->link_params.loopback_mode != LOOPBACK_XGXS_10)
			return -EINVAL;
		break;
	case BNX2X_MAC_LOOPBACK:
		bp->link_params.loopback_mode = LOOPBACK_BMAC;
		bnx2x_phy_init(&bp->link_params, &bp->link_vars);
		break;
	default:
		return -EINVAL;
	}

	/* prepare the loopback packet */
	pkt_size = (((bp->dev->mtu < ETH_MAX_PACKET_SIZE) ?
		     bp->dev->mtu : ETH_MAX_PACKET_SIZE) + ETH_HLEN);
	skb = netdev_alloc_skb(bp->dev, bp->rx_buf_size);
	if (!skb) {
		rc = -ENOMEM;
		goto test_loopback_exit;
	}
	packet = skb_put(skb, pkt_size);
	memcpy(packet, bp->dev->dev_addr, ETH_ALEN);
	memset(packet + ETH_ALEN, 0, ETH_ALEN);
	memset(packet + 2*ETH_ALEN, 0x77, (ETH_HLEN - 2*ETH_ALEN));
	for (i = ETH_HLEN; i < pkt_size; i++)
		packet[i] = (unsigned char) (i & 0xff);

	/* send the loopback packet */
	num_pkts = 0;
	tx_start_idx = le16_to_cpu(*fp_tx->tx_cons_sb);
	rx_start_idx = le16_to_cpu(*fp_rx->rx_cons_sb);

	pkt_prod = fp_tx->tx_pkt_prod++;
	tx_buf = &fp_tx->tx_buf_ring[TX_BD(pkt_prod)];
	tx_buf->first_bd = fp_tx->tx_bd_prod;
	tx_buf->skb = skb;
	tx_buf->flags = 0;

	bd_prod = TX_BD(fp_tx->tx_bd_prod);
	tx_start_bd = &fp_tx->tx_desc_ring[bd_prod].start_bd;
	mapping = pci_map_single(bp->pdev, skb->data,
				 skb_headlen(skb), PCI_DMA_TODEVICE);
	tx_start_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
	tx_start_bd->addr_lo = cpu_to_le32(U64_LO(mapping));
	tx_start_bd->nbd = cpu_to_le16(2); /* start + pbd */
	tx_start_bd->nbytes = cpu_to_le16(skb_headlen(skb));
	tx_start_bd->vlan = cpu_to_le16(pkt_prod);
	tx_start_bd->bd_flags.as_bitfield = ETH_TX_BD_FLAGS_START_BD;
	tx_start_bd->general_data = ((UNICAST_ADDRESS <<
				ETH_TX_START_BD_ETH_ADDR_TYPE_SHIFT) | 1);

	/* turn on parsing and get a BD */
	bd_prod = TX_BD(NEXT_TX_IDX(bd_prod));
	pbd = &fp_tx->tx_desc_ring[bd_prod].parse_bd;

	memset(pbd, 0, sizeof(struct eth_tx_parse_bd));

	wmb();

	fp_tx->tx_db.data.prod += 2;
	barrier();
	DOORBELL(bp, fp_tx->index, fp_tx->tx_db.raw);

	mmiowb();

	num_pkts++;
	fp_tx->tx_bd_prod += 2; /* start + pbd */
#if (LINUX_VERSION_CODE < 0x02061f) /* ! BNX2X_UPSTREAM */
	/* In kernels starting from 2.6.31 netdev layer does this */
	bp->dev->trans_start = jiffies;
#endif

	udelay(100);

	tx_idx = le16_to_cpu(*fp_tx->tx_cons_sb);
	if (tx_idx != tx_start_idx + num_pkts)
		goto test_loopback_exit;

	rx_idx = le16_to_cpu(*fp_rx->rx_cons_sb);
	if (rx_idx != rx_start_idx + num_pkts)
		goto test_loopback_exit;

	cqe = &fp_rx->rx_comp_ring[RCQ_BD(fp_rx->rx_comp_cons)];
	cqe_fp_flags = cqe->fast_path_cqe.type_error_flags;
	if (CQE_TYPE(cqe_fp_flags) || (cqe_fp_flags & ETH_RX_ERROR_FALGS))
		goto test_loopback_rx_exit;

	len = le16_to_cpu(cqe->fast_path_cqe.pkt_len);
	if (len != pkt_size)
		goto test_loopback_rx_exit;

	rx_buf = &fp_rx->rx_buf_ring[RX_BD(fp_rx->rx_bd_cons)];
	skb = rx_buf->skb;
	skb_reserve(skb, cqe->fast_path_cqe.placement_offset);
	for (i = ETH_HLEN; i < pkt_size; i++)
		if (*(skb->data + i) != (unsigned char) (i & 0xff))
			goto test_loopback_rx_exit;

	rc = 0;

test_loopback_rx_exit:
#if (LINUX_VERSION_CODE < 0x02061b) /* ! BNX2X_UPSTREAM */
	bp->dev->last_rx = jiffies;
#endif

	fp_rx->rx_bd_cons = NEXT_RX_IDX(fp_rx->rx_bd_cons);
	fp_rx->rx_bd_prod = NEXT_RX_IDX(fp_rx->rx_bd_prod);
	fp_rx->rx_comp_cons = NEXT_RCQ_IDX(fp_rx->rx_comp_cons);
	fp_rx->rx_comp_prod = NEXT_RCQ_IDX(fp_rx->rx_comp_prod);

	/* Update producers */
	bnx2x_update_rx_prod(bp, fp_rx, fp_rx->rx_bd_prod, fp_rx->rx_comp_prod,
			     fp_rx->rx_sge_prod);

test_loopback_exit:
	bp->link_params.loopback_mode = LOOPBACK_NONE;

	return rc;
}

static int bnx2x_test_loopback(struct bnx2x *bp, u8 link_up)
{
	int rc = 0, res;

	if (BP_NOMCP(bp))
		return rc;

	if (!netif_running(bp->dev))
		return BNX2X_LOOPBACK_FAILED;

	bnx2x_netif_stop(bp, 1);
	bnx2x_acquire_phy_lock(bp);

	res = bnx2x_run_loopback(bp, BNX2X_PHY_LOOPBACK, link_up);
	if (res) {
		DP(NETIF_MSG_PROBE, "  PHY loopback failed  (res %d)\n", res);
		rc |= BNX2X_PHY_LOOPBACK_FAILED;
	}

	res = bnx2x_run_loopback(bp, BNX2X_MAC_LOOPBACK, link_up);
	if (res) {
		DP(NETIF_MSG_PROBE, "  MAC loopback failed  (res %d)\n", res);
		rc |= BNX2X_MAC_LOOPBACK_FAILED;
	}

	bnx2x_release_phy_lock(bp);
	bnx2x_netif_start(bp);

	return rc;
}

#define CRC32_RESIDUAL			0xdebb20e3

static int bnx2x_test_nvram(struct bnx2x *bp)
{
	static const struct {
		int offset;
		int size;
	} nvram_tbl[] = {
		{     0,  0x14 }, /* bootstrap */
		{  0x14,  0xec }, /* dir */
		{ 0x100, 0x350 }, /* manuf_info */
		{ 0x450,  0xf0 }, /* feature_info */
		{ 0x640,  0x64 }, /* upgrade_key_info */
		{ 0x6a4,  0x64 },
		{ 0x708,  0x70 }, /* manuf_key_info */
		{ 0x778,  0x70 },
		{     0,     0 }
	};
	__be32 buf[0x350 / 4];
	u8 *data = (u8 *)buf;
	int i, rc;
	u32 magic, crc;

	if (BP_NOMCP(bp))
		return 0;

	rc = bnx2x_nvram_read(bp, 0, data, 4);
	if (rc) {
		DP(NETIF_MSG_PROBE, "magic value read (rc %d)\n", rc);
		goto test_nvram_exit;
	}

	magic = be32_to_cpu(buf[0]);
	if (magic != 0x669955aa) {
		DP(NETIF_MSG_PROBE, "magic value (0x%08x)\n", magic);
		rc = -ENODEV;
		goto test_nvram_exit;
	}

	for (i = 0; nvram_tbl[i].size; i++) {

		rc = bnx2x_nvram_read(bp, nvram_tbl[i].offset, data,
				      nvram_tbl[i].size);
		if (rc) {
			DP(NETIF_MSG_PROBE,
			   "nvram_tbl[%d] read data (rc %d)\n", i, rc);
			goto test_nvram_exit;
		}

		crc = ether_crc_le(nvram_tbl[i].size, data);
		if (crc != CRC32_RESIDUAL) {
			DP(NETIF_MSG_PROBE,
			   "nvram_tbl[%d] crc value (0x%08x)\n", i, crc);
			rc = -ENODEV;
			goto test_nvram_exit;
		}
	}

test_nvram_exit:
	return rc;
}

static int bnx2x_test_intr(struct bnx2x *bp)
{
	struct mac_configuration_cmd *config = bnx2x_sp(bp, mac_config);
	int i, rc;

	if (!netif_running(bp->dev))
		return -ENODEV;

	config->hdr.length = 0;
	if (CHIP_IS_E1(bp))
		/* use last unicast entries */
		config->hdr.offset = (BP_PORT(bp) ? 63 : 31);
	else
		config->hdr.offset = BP_FUNC(bp);
	config->hdr.client_id = bp->fp->cl_id;
	config->hdr.reserved1 = 0;

	bp->set_mac_pending++;
	smp_wmb();
	rc = bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_SET_MAC, 0,
			   U64_HI(bnx2x_sp_mapping(bp, mac_config)),
			   U64_LO(bnx2x_sp_mapping(bp, mac_config)), 0);
	if (rc == 0) {
		for (i = 0; i < 10; i++) {
			if (!bp->set_mac_pending)
				break;
			smp_rmb();
			msleep_interruptible(10);
		}
		if (i == 10)
			rc = -ENODEV;
	}

	return rc;
}

static void bnx2x_self_test(struct net_device *dev,
			    struct ethtool_test *etest, u64 *buf)
{
	struct bnx2x *bp = netdev_priv(dev);

	memset(buf, 0, sizeof(u64) * BNX2X_NUM_TESTS);

	if (!netif_running(dev))
		return;

	/* offline tests are not supported in MF mode */
	if (IS_E1HMF(bp))
		etest->flags &= ~ETH_TEST_FL_OFFLINE;

	if (etest->flags & ETH_TEST_FL_OFFLINE) {
		int port = BP_PORT(bp);
		u32 val;
		u8 link_up;

		/* save current value of input enable for TX port IF */
		val = REG_RD(bp, NIG_REG_EGRESS_UMP0_IN_EN + port*4);
		/* disable input for TX port IF */
		REG_WR(bp, NIG_REG_EGRESS_UMP0_IN_EN + port*4, 0);

		link_up = (bnx2x_link_test(bp) == 0);
		bnx2x_nic_unload(bp, UNLOAD_NORMAL);
		bnx2x_nic_load(bp, LOAD_DIAG);
		/* wait until link state is restored */
		bnx2x_wait_for_link(bp, link_up);

		if (bnx2x_test_registers(bp) != 0) {
			buf[0] = 1;
			etest->flags |= ETH_TEST_FL_FAILED;
		}
		if (bnx2x_test_memory(bp) != 0) {
			buf[1] = 1;
			etest->flags |= ETH_TEST_FL_FAILED;
		}
		buf[2] = bnx2x_test_loopback(bp, link_up);
		if (buf[2] != 0)
			etest->flags |= ETH_TEST_FL_FAILED;

		bnx2x_nic_unload(bp, UNLOAD_NORMAL);

		/* restore input for TX port IF */
		REG_WR(bp, NIG_REG_EGRESS_UMP0_IN_EN + port*4, val);

		bnx2x_nic_load(bp, LOAD_NORMAL);
		/* wait until link state is restored */
		bnx2x_wait_for_link(bp, link_up);
	}
	if (bnx2x_test_nvram(bp) != 0) {
		buf[3] = 1;
		etest->flags |= ETH_TEST_FL_FAILED;
	}
	if (bnx2x_test_intr(bp) != 0) {
		buf[4] = 1;
		etest->flags |= ETH_TEST_FL_FAILED;
	}
	if (bp->port.pmf)
		if (bnx2x_link_test(bp) != 0) {
			buf[5] = 1;
			etest->flags |= ETH_TEST_FL_FAILED;
		}
#ifndef BNX2X_UPSTREAM /* ! BNX2X_UPSTREAM */
	/* run the idle check twice */
	bnx2x_idle_chk(bp);
	buf[6] = bnx2x_idle_chk(bp);
	if (buf[6] != 0)
		etest->flags |= ETH_TEST_FL_FAILED;
#endif

#ifdef BNX2X_EXTRA_DEBUG
	bnx2x_panic_dump(bp);
#endif
}

static const struct {
	long offset;
	int size;
	u8 string[ETH_GSTRING_LEN];
} bnx2x_q_stats_arr[BNX2X_NUM_Q_STATS] = {
/* 1 */	{ Q_STATS_OFFSET32(total_bytes_received_hi), 8, "[%d]: rx_bytes" },
	{ Q_STATS_OFFSET32(error_bytes_received_hi),
						8, "[%d]: rx_error_bytes" },
	{ Q_STATS_OFFSET32(total_unicast_packets_received_hi),
						8, "[%d]: rx_ucast_packets" },
	{ Q_STATS_OFFSET32(total_multicast_packets_received_hi),
						8, "[%d]: rx_mcast_packets" },
	{ Q_STATS_OFFSET32(total_broadcast_packets_received_hi),
						8, "[%d]: rx_bcast_packets" },
	{ Q_STATS_OFFSET32(no_buff_discard_hi),	8, "[%d]: rx_discards" },
	{ Q_STATS_OFFSET32(rx_err_discard_pkt),
					 4, "[%d]: rx_phy_ip_err_discards"},
	{ Q_STATS_OFFSET32(rx_skb_alloc_failed),
					 4, "[%d]: rx_skb_alloc_discard" },
	{ Q_STATS_OFFSET32(hw_csum_err), 4, "[%d]: rx_csum_offload_errors" },

/* 10 */{ Q_STATS_OFFSET32(total_bytes_transmitted_hi),	8, "[%d]: tx_bytes" },
	{ Q_STATS_OFFSET32(total_unicast_packets_transmitted_hi),
						8, "[%d]: tx_ucast_packets" },
	{ Q_STATS_OFFSET32(total_multicast_packets_transmitted_hi),
						8, "[%d]: tx_mcast_packets" },
	{ Q_STATS_OFFSET32(total_broadcast_packets_transmitted_hi),
						8, "[%d]: tx_bcast_packets" }
};

static const struct {
	long offset;
	int size;
	u32 flags;
#define STATS_FLAGS_PORT		1
#define STATS_FLAGS_FUNC		2
#define STATS_FLAGS_BOTH		(STATS_FLAGS_FUNC | STATS_FLAGS_PORT)
	u8 string[ETH_GSTRING_LEN];
} bnx2x_stats_arr[BNX2X_NUM_STATS] = {
/* 1 */	{ STATS_OFFSET32(total_bytes_received_hi),
				8, STATS_FLAGS_BOTH, "rx_bytes" },
	{ STATS_OFFSET32(error_bytes_received_hi),
				8, STATS_FLAGS_BOTH, "rx_error_bytes" },
	{ STATS_OFFSET32(total_unicast_packets_received_hi),
				8, STATS_FLAGS_BOTH, "rx_ucast_packets" },
	{ STATS_OFFSET32(total_multicast_packets_received_hi),
				8, STATS_FLAGS_BOTH, "rx_mcast_packets" },
	{ STATS_OFFSET32(total_broadcast_packets_received_hi),
				8, STATS_FLAGS_BOTH, "rx_bcast_packets" },
	{ STATS_OFFSET32(rx_stat_dot3statsfcserrors_hi),
				8, STATS_FLAGS_PORT, "rx_crc_errors" },
	{ STATS_OFFSET32(rx_stat_dot3statsalignmenterrors_hi),
				8, STATS_FLAGS_PORT, "rx_align_errors" },
	{ STATS_OFFSET32(rx_stat_etherstatsundersizepkts_hi),
				8, STATS_FLAGS_PORT, "rx_undersize_packets" },
	{ STATS_OFFSET32(etherstatsoverrsizepkts_hi),
				8, STATS_FLAGS_PORT, "rx_oversize_packets" },
/* 10 */{ STATS_OFFSET32(rx_stat_etherstatsfragments_hi),
				8, STATS_FLAGS_PORT, "rx_fragments" },
	{ STATS_OFFSET32(rx_stat_etherstatsjabbers_hi),
				8, STATS_FLAGS_PORT, "rx_jabbers" },
	{ STATS_OFFSET32(no_buff_discard_hi),
				8, STATS_FLAGS_BOTH, "rx_discards" },
	{ STATS_OFFSET32(mac_filter_discard),
				4, STATS_FLAGS_PORT, "rx_filtered_packets" },
	{ STATS_OFFSET32(xxoverflow_discard),
				4, STATS_FLAGS_PORT, "rx_fw_discards" },
	{ STATS_OFFSET32(brb_drop_hi),
				8, STATS_FLAGS_PORT, "rx_brb_discard" },
	{ STATS_OFFSET32(brb_truncate_hi),
				8, STATS_FLAGS_PORT, "rx_brb_truncate" },
	{ STATS_OFFSET32(pause_frames_received_hi),
				8, STATS_FLAGS_PORT, "rx_pause_frames" },
	{ STATS_OFFSET32(rx_stat_maccontrolframesreceived_hi),
				8, STATS_FLAGS_PORT, "rx_mac_ctrl_frames" },
	{ STATS_OFFSET32(nig_timer_max),
			4, STATS_FLAGS_PORT, "rx_constant_pause_events" },
/* 20 */{ STATS_OFFSET32(rx_err_discard_pkt),
				4, STATS_FLAGS_BOTH, "rx_phy_ip_err_discards"},
	{ STATS_OFFSET32(rx_skb_alloc_failed),
				4, STATS_FLAGS_BOTH, "rx_skb_alloc_discard" },
	{ STATS_OFFSET32(hw_csum_err),
				4, STATS_FLAGS_BOTH, "rx_csum_offload_errors" },

	{ STATS_OFFSET32(total_bytes_transmitted_hi),
				8, STATS_FLAGS_BOTH, "tx_bytes" },
	{ STATS_OFFSET32(tx_stat_ifhcoutbadoctets_hi),
				8, STATS_FLAGS_PORT, "tx_error_bytes" },
	{ STATS_OFFSET32(total_unicast_packets_transmitted_hi),
				8, STATS_FLAGS_BOTH, "tx_ucast_packets" },
	{ STATS_OFFSET32(total_multicast_packets_transmitted_hi),
				8, STATS_FLAGS_BOTH, "tx_mcast_packets" },
	{ STATS_OFFSET32(total_broadcast_packets_transmitted_hi),
				8, STATS_FLAGS_BOTH, "tx_bcast_packets" },
	{ STATS_OFFSET32(tx_stat_dot3statsinternalmactransmiterrors_hi),
				8, STATS_FLAGS_PORT, "tx_mac_errors" },
	{ STATS_OFFSET32(rx_stat_dot3statscarriersenseerrors_hi),
				8, STATS_FLAGS_PORT, "tx_carrier_errors" },
/* 30 */{ STATS_OFFSET32(tx_stat_dot3statssinglecollisionframes_hi),
				8, STATS_FLAGS_PORT, "tx_single_collisions" },
	{ STATS_OFFSET32(tx_stat_dot3statsmultiplecollisionframes_hi),
				8, STATS_FLAGS_PORT, "tx_multi_collisions" },
	{ STATS_OFFSET32(tx_stat_dot3statsdeferredtransmissions_hi),
				8, STATS_FLAGS_PORT, "tx_deferred" },
	{ STATS_OFFSET32(tx_stat_dot3statsexcessivecollisions_hi),
				8, STATS_FLAGS_PORT, "tx_excess_collisions" },
	{ STATS_OFFSET32(tx_stat_dot3statslatecollisions_hi),
				8, STATS_FLAGS_PORT, "tx_late_collisions" },
	{ STATS_OFFSET32(tx_stat_etherstatscollisions_hi),
				8, STATS_FLAGS_PORT, "tx_total_collisions" },
	{ STATS_OFFSET32(tx_stat_etherstatspkts64octets_hi),
				8, STATS_FLAGS_PORT, "tx_64_byte_packets" },
	{ STATS_OFFSET32(tx_stat_etherstatspkts65octetsto127octets_hi),
			8, STATS_FLAGS_PORT, "tx_65_to_127_byte_packets" },
	{ STATS_OFFSET32(tx_stat_etherstatspkts128octetsto255octets_hi),
			8, STATS_FLAGS_PORT, "tx_128_to_255_byte_packets" },
	{ STATS_OFFSET32(tx_stat_etherstatspkts256octetsto511octets_hi),
			8, STATS_FLAGS_PORT, "tx_256_to_511_byte_packets" },
/* 40 */{ STATS_OFFSET32(tx_stat_etherstatspkts512octetsto1023octets_hi),
			8, STATS_FLAGS_PORT, "tx_512_to_1023_byte_packets" },
	{ STATS_OFFSET32(etherstatspkts1024octetsto1522octets_hi),
			8, STATS_FLAGS_PORT, "tx_1024_to_1522_byte_packets" },
	{ STATS_OFFSET32(etherstatspktsover1522octets_hi),
			8, STATS_FLAGS_PORT, "tx_1523_to_9022_byte_packets" },
	{ STATS_OFFSET32(pause_frames_sent_hi),
				8, STATS_FLAGS_PORT, "tx_pause_frames" }
};

#define IS_PORT_STAT(i) \
	((bnx2x_stats_arr[i].flags & STATS_FLAGS_BOTH) == STATS_FLAGS_PORT)
#define IS_FUNC_STAT(i)		(bnx2x_stats_arr[i].flags & STATS_FLAGS_FUNC)
#define IS_E1HMF_MODE_STAT(bp) \
			(IS_E1HMF(bp) && !(bp->msglevel & BNX2X_MSG_STATS))

#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
static int bnx2x_get_sset_count(struct net_device *dev, int stringset)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i, num_stats;

	switch (stringset) {
	case ETH_SS_STATS:
		if (is_multi(bp)) {
			num_stats = BNX2X_NUM_Q_STATS * bp->num_queues;
			if (!IS_E1HMF_MODE_STAT(bp))
				num_stats += BNX2X_NUM_STATS;
		} else {
			if (IS_E1HMF_MODE_STAT(bp)) {
				num_stats = 0;
				for (i = 0; i < BNX2X_NUM_STATS; i++)
					if (IS_FUNC_STAT(i))
						num_stats++;
			} else
				num_stats = BNX2X_NUM_STATS;
		}
		return num_stats;

	case ETH_SS_TEST:
		return BNX2X_NUM_TESTS;

	default:
		return -EINVAL;
	}
}
#else
static int bnx2x_self_test_count(struct net_device *dev)
{
	return BNX2X_NUM_TESTS;
}

static int bnx2x_get_stats_count(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i, num_stats;

	if (is_multi(bp)) {
		num_stats = BNX2X_NUM_Q_STATS * bp->num_queues;
		if (!IS_E1HMF_MODE_STAT(bp))
			num_stats += BNX2X_NUM_STATS;
	} else {
		if (IS_E1HMF_MODE_STAT(bp)) {
			num_stats = 0;
			for (i = 0; i < BNX2X_NUM_STATS; i++)
				if (IS_FUNC_STAT(i))
					num_stats++;
		} else
			num_stats = BNX2X_NUM_STATS;
	}

	return num_stats;
}

#endif

static void bnx2x_get_strings(struct net_device *dev, u32 stringset, u8 *buf)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i, j, k;

	switch (stringset) {
	case ETH_SS_STATS:
		if (is_multi(bp)) {
			k = 0;
			for_each_queue(bp, i) {
				for (j = 0; j < BNX2X_NUM_Q_STATS; j++)
					sprintf(buf + (k + j)*ETH_GSTRING_LEN,
						bnx2x_q_stats_arr[j].string, i);
				k += BNX2X_NUM_Q_STATS;
			}
			if (IS_E1HMF_MODE_STAT(bp))
				break;
			for (j = 0; j < BNX2X_NUM_STATS; j++)
				strcpy(buf + (k + j)*ETH_GSTRING_LEN,
				       bnx2x_stats_arr[j].string);
		} else {
			for (i = 0, j = 0; i < BNX2X_NUM_STATS; i++) {
				if (IS_E1HMF_MODE_STAT(bp) && IS_PORT_STAT(i))
					continue;
				strcpy(buf + j*ETH_GSTRING_LEN,
				       bnx2x_stats_arr[i].string);
				j++;
			}
		}
		break;

	case ETH_SS_TEST:
		memcpy(buf, bnx2x_tests_str_arr, sizeof(bnx2x_tests_str_arr));
		break;
	}
}

static void bnx2x_get_ethtool_stats(struct net_device *dev,
				    struct ethtool_stats *stats, u64 *buf)
{
	struct bnx2x *bp = netdev_priv(dev);
	u32 *hw_stats, *offset;
	int i, j, k;

	if (is_multi(bp)) {
		k = 0;
		for_each_queue(bp, i) {
			hw_stats = (u32 *)&bp->fp[i].eth_q_stats;
			for (j = 0; j < BNX2X_NUM_Q_STATS; j++) {
				if (bnx2x_q_stats_arr[j].size == 0) {
					/* skip this counter */
					buf[k + j] = 0;
					continue;
				}
				offset = (hw_stats +
					  bnx2x_q_stats_arr[j].offset);
				if (bnx2x_q_stats_arr[j].size == 4) {
					/* 4-byte counter */
					buf[k + j] = (u64) *offset;
					continue;
				}
				/* 8-byte counter */
				buf[k + j] = HILO_U64(*offset, *(offset + 1));
			}
			k += BNX2X_NUM_Q_STATS;
		}
		if (IS_E1HMF_MODE_STAT(bp))
			return;
		hw_stats = (u32 *)&bp->eth_stats;
		for (j = 0; j < BNX2X_NUM_STATS; j++) {
			if (bnx2x_stats_arr[j].size == 0) {
				/* skip this counter */
				buf[k + j] = 0;
				continue;
			}
			offset = (hw_stats + bnx2x_stats_arr[j].offset);
			if (bnx2x_stats_arr[j].size == 4) {
				/* 4-byte counter */
				buf[k + j] = (u64) *offset;
				continue;
			}
			/* 8-byte counter */
			buf[k + j] = HILO_U64(*offset, *(offset + 1));
		}
	} else {
		hw_stats = (u32 *)&bp->eth_stats;
		for (i = 0, j = 0; i < BNX2X_NUM_STATS; i++) {
			if (IS_E1HMF_MODE_STAT(bp) && IS_PORT_STAT(i))
				continue;
			if (bnx2x_stats_arr[i].size == 0) {
				/* skip this counter */
				buf[j] = 0;
				j++;
				continue;
			}
			offset = (hw_stats + bnx2x_stats_arr[i].offset);
			if (bnx2x_stats_arr[i].size == 4) {
				/* 4-byte counter */
				buf[j] = (u64) *offset;
				j++;
				continue;
			}
			/* 8-byte counter */
			buf[j] = HILO_U64(*offset, *(offset + 1));
			j++;
		}
	}
}

static int bnx2x_phys_id(struct net_device *dev, u32 data)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i;

	if (!netif_running(dev))
		return 0;

	if (!bp->port.pmf)
		return 0;

	if (data == 0)
		data = 2;

	for (i = 0; i < (data * 2); i++) {
		if ((i % 2) == 0)
			bnx2x_set_led(&bp->link_params, LED_MODE_OPER,
				      SPEED_1000);
		else
			bnx2x_set_led(&bp->link_params, LED_MODE_OFF, 0);

		msleep_interruptible(500);
		if (signal_pending(current))
			break;
	}

	if (bp->link_vars.link_up)
		bnx2x_set_led(&bp->link_params, LED_MODE_OPER,
			      bp->link_vars.line_speed);

	return 0;
}

#if (LINUX_VERSION_CODE >= 0x020613) /* BNX2X_UPSTREAM */
static const struct ethtool_ops bnx2x_ethtool_ops = {
#else
static struct ethtool_ops bnx2x_ethtool_ops = {
#endif
	.get_settings		= bnx2x_get_settings,
	.set_settings		= bnx2x_set_settings,
	.get_drvinfo		= bnx2x_get_drvinfo,
	.get_regs_len		= bnx2x_get_regs_len,
	.get_regs		= bnx2x_get_regs,
	.get_wol		= bnx2x_get_wol,
	.set_wol		= bnx2x_set_wol,
	.get_msglevel		= bnx2x_get_msglevel,
	.set_msglevel		= bnx2x_set_msglevel,
	.nway_reset		= bnx2x_nway_reset,
	.get_link		= bnx2x_get_link,
#if (LINUX_VERSION_CODE >= 0x020418) /* BNX2X_UPSTREAM */
	.get_eeprom_len		= bnx2x_get_eeprom_len,
#endif
	.get_eeprom		= bnx2x_get_eeprom,
	.set_eeprom		= bnx2x_set_eeprom,
	.get_coalesce		= bnx2x_get_coalesce,
	.set_coalesce		= bnx2x_set_coalesce,
	.get_ringparam		= bnx2x_get_ringparam,
	.set_ringparam		= bnx2x_set_ringparam,
	.get_pauseparam		= bnx2x_get_pauseparam,
	.set_pauseparam		= bnx2x_set_pauseparam,
	.get_rx_csum		= bnx2x_get_rx_csum,
	.set_rx_csum		= bnx2x_set_rx_csum,
	.get_tx_csum		= ethtool_op_get_tx_csum,
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
	.set_tx_csum		= ethtool_op_set_tx_hw_csum,
#else
	.set_tx_csum		= bnx2x_set_tx_hw_csum,
#endif
#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
	.set_flags		= bnx2x_set_flags,
	.get_flags		= ethtool_op_get_flags,
#endif
	.get_sg			= ethtool_op_get_sg,
	.set_sg			= ethtool_op_set_sg,
#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
	.get_tso		= ethtool_op_get_tso,
	.set_tso		= bnx2x_set_tso,
#endif
	.self_test		= bnx2x_self_test,
#if (LINUX_VERSION_CODE >= 0x020618) /* BNX2X_UPSTREAM */
	.get_sset_count		= bnx2x_get_sset_count,
#else
	.get_stats_count	= bnx2x_get_stats_count,
	.self_test_count	= bnx2x_self_test_count,
#endif
	.get_strings		= bnx2x_get_strings,
	.phys_id		= bnx2x_phys_id,
	.get_ethtool_stats	= bnx2x_get_ethtool_stats,
#ifdef ETHTOOL_GPERMADDR /* ! BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE < 0x020617)
	.get_perm_addr		= ethtool_op_get_perm_addr
#endif
#endif
};

/* end of ethtool_ops */

/****************************************************************************
* General service functions
****************************************************************************/

static int bnx2x_set_power_state(struct bnx2x *bp, pci_power_t state)
{
	u16 pmcsr;

	pci_read_config_word(bp->pdev, bp->pm_cap + PCI_PM_CTRL, &pmcsr);

	switch (state) {
	case PCI_D0:
		pci_write_config_word(bp->pdev, bp->pm_cap + PCI_PM_CTRL,
				      ((pmcsr & ~PCI_PM_CTRL_STATE_MASK) |
				       PCI_PM_CTRL_PME_STATUS));

		if (pmcsr & PCI_PM_CTRL_STATE_MASK)
			/* delay required during transition out of D3hot */
			msleep(20);
		break;

	case PCI_D3hot:
		pmcsr &= ~PCI_PM_CTRL_STATE_MASK;
		pmcsr |= 3;

		if (bp->wol)
			pmcsr |= PCI_PM_CTRL_PME_ENABLE;

		pci_write_config_word(bp->pdev, bp->pm_cap + PCI_PM_CTRL,
				      pmcsr);

		/* No more memory access after this point until
		* device is brought back to D0.
		*/
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static inline int bnx2x_has_rx_work(struct bnx2x_fastpath *fp)
{
	u16 rx_cons_sb;

	/* Tell compiler that status block fields can change */
	barrier();
	rx_cons_sb = le16_to_cpu(*fp->rx_cons_sb);
	if ((rx_cons_sb & MAX_RCQ_DESC_CNT) == MAX_RCQ_DESC_CNT)
		rx_cons_sb++;
	return (fp->rx_comp_cons != rx_cons_sb);
}

#if !defined(BNX2X_NEW_NAPI) && defined(USE_NAPI_GRO) /* ! BNX2X_UPSTREAM */
int  __bnx2x_poll(struct net_device *dev, int *budget) {
	struct bnx2x_fastpath *fp = dev->priv;
	int done;
	int todo = min(*budget, dev->quota);
	done =  bnx2x_poll(&fp->napi, todo);
	*budget -= done;
	dev->quota -= done;
	return (done >= todo) ? 1 : 0;
}
#endif
/*
 * net_device service functions
 */

#if defined(BNX2X_NEW_NAPI) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
static int bnx2x_poll(struct napi_struct *napi, int budget)
#else
static int bnx2x_poll(struct net_device *dev, int *budget)
#endif
{
	int work_done = 0;
#if defined(BNX2X_NEW_NAPI) || defined(USE_NAPI_GRO)/* BNX2X_UPSTREAM */
	struct bnx2x_fastpath *fp = container_of(napi, struct bnx2x_fastpath,
						 napi);
	struct bnx2x *bp = fp->bp;
#else /* ! BNX2X_UPSTREAM */
	struct bnx2x_fastpath *fp = dev->priv;
	struct bnx2x *bp = fp->bp;
	int orig_budget = min(*budget, dev->quota);
#endif

	while (1) {
#ifdef BNX2X_STOP_ON_ERROR
		if (unlikely(bp->panic)) {
			napi_complete(napi);
			return 0;
		}
#endif

#ifndef BNX2X_MULTI_QUEUE /* ! BNX2X_UPSTREAM */
		/* There is only one Tx queue on kernels 2.6.26 and below */
		if (fp->index == 0)
#endif
		if (bnx2x_has_tx_work(fp))
			bnx2x_tx_int(fp);

		if (bnx2x_has_rx_work(fp)) {
#if defined(BNX2X_NEW_NAPI) || defined(USE_NAPI_GRO)/* BNX2X_UPSTREAM */
			work_done += bnx2x_rx_int(fp, budget - work_done);

			/* must not complete if we consumed full budget */
			if (work_done >= budget)
				break;
#else
			work_done = bnx2x_rx_int(fp, orig_budget);

			*budget -= work_done;
			dev->quota -= work_done;
			orig_budget = min(*budget, dev->quota);
			if (orig_budget <= 0)
				break;
#endif
		}

		/* Fall out from the NAPI loop if needed */
		if (!(bnx2x_has_rx_work(fp) || bnx2x_has_tx_work(fp))) {
			bnx2x_update_fpsb_idx(fp);
		/* bnx2x_has_rx_work() reads the status block, thus we need
		 * to ensure that status block indices have been actually read
		 * (bnx2x_update_fpsb_idx) prior to this check
		 * (bnx2x_has_rx_work) so that we won't write the "newer"
		 * value of the status block to IGU (if there was a DMA right
		 * after bnx2x_has_rx_work and if there is no rmb, the memory
		 * reading (bnx2x_update_fpsb_idx) may be postponed to right
		 * before bnx2x_ack_sb). In this case there will never be
		 * another interrupt until there is another update of the
		 * status block, while there is still unhandled work.
		 */
			rmb();

			if (!(bnx2x_has_rx_work(fp) || bnx2x_has_tx_work(fp))) {
				napi_complete(napi);
				/* Re-enable interrupts */
				bnx2x_ack_sb(bp, fp->sb_id, CSTORM_ID,
					     le16_to_cpu(fp->fp_c_idx),
					     IGU_INT_NOP, 1);
				bnx2x_ack_sb(bp, fp->sb_id, USTORM_ID,
					     le16_to_cpu(fp->fp_u_idx),
					     IGU_INT_ENABLE, 1);
#ifndef BNX2X_NEW_NAPI
				return 0;
#else	/* BNX2X_UPSTREAM */
				break;
#endif
			}
		}
	}

#ifdef BNX2X_NEW_NAPI /* BNX2X_UPSTREAM */
	return work_done;
#else
	return 1;
#endif
}

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
static struct net_device_stats *bnx2x_get_stats(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	return &bp->net_stats;
}
#endif

#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
/* we split the first BD into headers and data BDs
 * to ease the pain of our fellow microcode engineers
 * we use one mapping for both BDs
 * So far this has only been observed to happen
 * in Other Operating Systems(TM)
 */
static noinline u16 bnx2x_tx_split(struct bnx2x *bp,
				   struct bnx2x_fastpath *fp,
				   struct sw_tx_bd *tx_buf,
				   struct eth_tx_start_bd **tx_bd, u16 hlen,
				   u16 bd_prod, int nbd)
{
	struct eth_tx_start_bd *h_tx_bd = *tx_bd;
	struct eth_tx_bd *d_tx_bd;
	dma_addr_t mapping;
	int old_len = le16_to_cpu(h_tx_bd->nbytes);

	/* first fix first BD */
	h_tx_bd->nbd = cpu_to_le16(nbd);
	h_tx_bd->nbytes = cpu_to_le16(hlen);

	DP(NETIF_MSG_TX_QUEUED,	"TSO split header size is %d "
	   "(%x:%x) nbd %d\n", h_tx_bd->nbytes, h_tx_bd->addr_hi,
	   h_tx_bd->addr_lo, h_tx_bd->nbd);

	/* now get a new data BD
	 * (after the pbd) and fill it */
	bd_prod = TX_BD(NEXT_TX_IDX(bd_prod));
	d_tx_bd = &fp->tx_desc_ring[bd_prod].reg_bd;

	mapping = HILO_U64(le32_to_cpu(h_tx_bd->addr_hi),
			   le32_to_cpu(h_tx_bd->addr_lo)) + hlen;

	d_tx_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
	d_tx_bd->addr_lo = cpu_to_le32(U64_LO(mapping));
	d_tx_bd->nbytes = cpu_to_le16(old_len - hlen);

	/* this marks the BD as one that has no individual mapping */
	tx_buf->flags |= BNX2X_TSO_SPLIT_BD;

	DP(NETIF_MSG_TX_QUEUED,
	   "TSO split data size is %d (%x:%x)\n",
	   d_tx_bd->nbytes, d_tx_bd->addr_hi, d_tx_bd->addr_lo);

	/* update tx_bd */
	*tx_bd = (struct eth_tx_start_bd *)d_tx_bd;

	return bd_prod;
}
#endif

static inline u16 bnx2x_csum_fix(unsigned char *t_header, u16 csum, s8 fix)
{
	if (fix > 0)
		csum = (u16) ~csum_fold(csum_sub(csum,
				csum_partial(t_header - fix, fix, 0)));

	else if (fix < 0)
		csum = (u16) ~csum_fold(csum_add(csum,
				csum_partial(t_header, -fix, 0)));

	return swab16(csum);
}

static inline u32 bnx2x_xmit_type(struct bnx2x *bp, struct sk_buff *skb)
{
	u32 rc;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		rc = XMIT_PLAIN;

	else {
		if (skb->protocol == htons(ETH_P_IPV6)) {
			rc = XMIT_CSUM_V6;
			if (ipv6_hdr(skb)->nexthdr == IPPROTO_TCP)
				rc |= XMIT_CSUM_TCP;

		} else {
			rc = XMIT_CSUM_V4;
			if (ip_hdr(skb)->protocol == IPPROTO_TCP)
				rc |= XMIT_CSUM_TCP;
		}
	}

#ifdef NETIF_F_GSO /* BNX2X_UPSTREAM */
	if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV4)
		rc |= (XMIT_GSO_V4 | XMIT_CSUM_V4 | XMIT_CSUM_TCP);

#ifdef NETIF_F_TSO6 /* BNX2X_UPSTREAM */
	else if (skb_shinfo(skb)->gso_type & SKB_GSO_TCPV6)
		rc |= (XMIT_GSO_V6 | XMIT_CSUM_TCP | XMIT_CSUM_V6);
#endif
#elif defined(NETIF_F_TSO) /* ! BNX2X_UPSTREAM */
	if ((skb_shinfo(skb)->gso_size) &&
	    (skb->len > (bp->dev->mtu + ETH_HLEN)))
		rc |= (XMIT_GSO_V4 | XMIT_CSUM_V4 | XMIT_CSUM_TCP);
#endif

	return rc;
}

#if (MAX_SKB_FRAGS >= MAX_FETCH_BD - 3)
/* check if packet requires linearization (packet is too fragmented)
   no need to check fragmentation if page size > 8K (there will be no
   violation to FW restrictions) */
static int bnx2x_pkt_req_lin(struct bnx2x *bp, struct sk_buff *skb,
			     u32 xmit_type)
{
	int to_copy = 0;
	int hlen = 0;
	int first_bd_sz = 0;

	/* 3 = 1 (for linear data BD) + 2 (for PBD and last BD) */
	if (skb_shinfo(skb)->nr_frags >= (MAX_FETCH_BD - 3)) {

		if (xmit_type & XMIT_GSO) {
#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
			unsigned short lso_mss = skb_shinfo(skb)->gso_size;
			/* Check if LSO packet needs to be copied:
			   3 = 1 (for headers BD) + 2 (for PBD and last BD) */
			int wnd_size = MAX_FETCH_BD - 3;
			/* Number of windows to check */
			int num_wnds = skb_shinfo(skb)->nr_frags - wnd_size;
			int wnd_idx = 0;
			int frag_idx = 0;
			u32 wnd_sum = 0;

			/* Headers length */
			hlen = (int)(skb_transport_header(skb) - skb->data) +
				tcp_hdrlen(skb);

			/* Amount of data (w/o headers) on linear part of SKB*/
			first_bd_sz = skb_headlen(skb) - hlen;

			wnd_sum  = first_bd_sz;

			/* Calculate the first sum - it's special */
			for (frag_idx = 0; frag_idx < wnd_size - 1; frag_idx++)
				wnd_sum +=
					skb_shinfo(skb)->frags[frag_idx].size;

			/* If there was data on linear skb data - check it */
			if (first_bd_sz > 0) {
				if (unlikely(wnd_sum < lso_mss)) {
					to_copy = 1;
					goto exit_lbl;
				}

				wnd_sum -= first_bd_sz;
			}

			/* Others are easier: run through the frag list and
			   check all windows */
			for (wnd_idx = 0; wnd_idx <= num_wnds; wnd_idx++) {
				wnd_sum +=
			  skb_shinfo(skb)->frags[wnd_idx + wnd_size - 1].size;

				if (unlikely(wnd_sum < lso_mss)) {
					to_copy = 1;
					break;
				}
				wnd_sum -=
					skb_shinfo(skb)->frags[wnd_idx].size;
			}
#endif
		} else {
			/* in non-LSO too fragmented packet should always
			   be linearized */
			to_copy = 1;
		}
	}

#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
exit_lbl:
#endif
	if (unlikely(to_copy))
		DP(NETIF_MSG_TX_QUEUED,
		   "Linearization IS REQUIRED for %s packet. "
		   "num_frags %d  hlen %d  first_bd_sz %d\n",
		   (xmit_type & XMIT_GSO) ? "LSO" : "non-LSO",
		   skb_shinfo(skb)->nr_frags, hlen, first_bd_sz);

	return to_copy;
}
#endif

#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
static u16 bnx2x_select_queue(struct net_device *dev, struct sk_buff *skb)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i, fp_index = 0;

	/* Determine which tx ring we will be placed on */
	switch (bp->multi_mode) {
	case ETH_RSS_MODE_VLAN_PRI:
	case ETH_RSS_MODE_E1HOV_PRI:
#ifdef BCM_VLAN
		if ((bp->vlgrp != NULL) && vlan_tx_tag_present(skb)) {
			i = ((vlan_tx_tag_get(skb) >> 13) & 0x7);
			fp_index = bp->cos_map[bp->pri_map[i]];
		}
#endif
		break;

	case ETH_RSS_MODE_IP_DSCP:
		if (skb->protocol == htons(ETH_P_IP)) {
			i = ((ip_hdr(skb)->tos >> 2) & 0x7);
			fp_index = bp->cos_map[bp->pri_map[i]];
		}
		break;
	default:
		fp_index = skb_tx_hash(dev, skb);
	}

	return fp_index;
}
#endif
/* called with netif_tx_lock
 * bnx2x_tx_int() runs without netif_tx_lock unless it needs to call
 * netif_wake_queue()
 */
static netdev_tx_t bnx2x_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	struct bnx2x_fastpath *fp;
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	struct netdev_queue *txq;
#endif
	struct sw_tx_bd *tx_buf;
	struct eth_tx_start_bd *tx_start_bd;
	struct eth_tx_bd *tx_data_bd, *total_pkt_bd = NULL;
	struct eth_tx_parse_bd *pbd = NULL;
	u16 pkt_prod, bd_prod;
	int nbd, fp_index;
	dma_addr_t mapping;
	u32 xmit_type = bnx2x_xmit_type(bp, skb);
	int i;
	u8 hlen = 0;
	__le16 pkt_size = 0;  
	struct ethhdr *eth;
	u8 mac_type = UNICAST_ADDRESS;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return NETDEV_TX_BUSY;
#endif

#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__)
	VMK_ASSERT(skb->queue_mapping < BNX2X_NUM_QUEUES(bp));
	fp_index = skb->queue_mapping;
	txq = netdev_get_tx_queue(dev, fp_index);
#else /* BNX2X_UPSTREAM */
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	fp_index = skb_get_queue_mapping(skb);
	txq = netdev_get_tx_queue(dev, fp_index);
#else
	fp_index = 0;
#endif
#endif

	fp = &bp->fp[fp_index];

	if (unlikely(bnx2x_tx_avail(fp) < (skb_shinfo(skb)->nr_frags + 3))) {
		fp->eth_q_stats.driver_xoff++;
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
		netif_tx_stop_queue(txq);
#else
		netif_stop_queue(dev);
#endif
		BNX2X_ERR("BUG! Tx ring full when queue awake!\n");
		return NETDEV_TX_BUSY;
	}

#ifdef NETIF_F_GSO /* BNX2X_UPSTREAM */
	DP(NETIF_MSG_TX_QUEUED, "SKB: summed %x  protocol %x  protocol(%x,%x)"
	   "  gso type %x  xmit_type %x\n",
	   skb->ip_summed, skb->protocol, ipv6_hdr(skb)->nexthdr,
	   ip_hdr(skb)->protocol, skb_shinfo(skb)->gso_type, xmit_type);
#endif

	eth = (struct ethhdr*)skb->data;

	/* set flag according to packet type (UNICAST_ADDRESS is default)*/
	if (unlikely(is_multicast_ether_addr(eth->h_dest))) {
		if (is_broadcast_ether_addr(eth->h_dest))
			mac_type = BROADCAST_ADDRESS;
		else
			mac_type = MULTICAST_ADDRESS;
	}

#if (MAX_SKB_FRAGS >= MAX_FETCH_BD - 3)
	/* First, check if we need to linearize the skb (due to FW
	   restrictions). No need to check fragmentation if page size > 8K
	   (there will be no violation to FW restrictions) */
	if (bnx2x_pkt_req_lin(bp, skb, xmit_type)) {
		/* Statistics of linearization */
		bp->lin_cnt++;
#if (LINUX_VERSION_CODE > 0x020611) || defined(SLE_VERSION_CODE) /* BNX2X_UPSTREAM */
		if (skb_linearize(skb) != 0) {
#else
		if (skb_linearize(skb, GFP_ATOMIC) != 0) {
#endif
			DP(NETIF_MSG_TX_QUEUED, "SKB linearization failed - "
			   "silently dropping this SKB\n");
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
	}
#endif

	/*
	Please read carefully. First we use one BD which we mark as start,
	then we have a parsing info BD (used for TSO or xsum),
	and only then we have the rest of the TSO BDs.
	(don't forget to mark the last one as last,
	and to unmap only AFTER you write to the BD ...)
	And above all, all pdb sizes are in words - NOT DWORDS!
	*/

	pkt_prod = fp->tx_pkt_prod++;
	bd_prod = TX_BD(fp->tx_bd_prod);

	/* get a tx_buf and first BD */
	tx_buf = &fp->tx_buf_ring[TX_BD(pkt_prod)];
	tx_start_bd = &fp->tx_desc_ring[bd_prod].start_bd;

	tx_start_bd->bd_flags.as_bitfield = ETH_TX_BD_FLAGS_START_BD;
	tx_start_bd->general_data =  (mac_type <<
					ETH_TX_START_BD_ETH_ADDR_TYPE_SHIFT);
	/* header nbd */
	tx_start_bd->general_data |= (1 << ETH_TX_START_BD_HDR_NBDS_SHIFT);

	/* remember the first BD of the packet */
	tx_buf->first_bd = fp->tx_bd_prod;
	tx_buf->skb = skb;
	tx_buf->flags = 0;

	DP(NETIF_MSG_TX_QUEUED,
	   "sending pkt %u @%p  next_idx %u  bd %u @%p\n",
	   pkt_prod, tx_buf, fp->tx_pkt_prod, bd_prod, tx_start_bd);

#ifdef BCM_VLAN
	if ((bp->vlgrp != NULL) && vlan_tx_tag_present(skb) &&
	    (bp->flags & HW_VLAN_TX_FLAG)) {
		tx_start_bd->vlan = cpu_to_le16(vlan_tx_tag_get(skb));
		tx_start_bd->bd_flags.as_bitfield |= ETH_TX_BD_FLAGS_VLAN_TAG;
	} else
#endif
		tx_start_bd->vlan = cpu_to_le16(pkt_prod);

	/* turn on parsing and get a BD */
	bd_prod = TX_BD(NEXT_TX_IDX(bd_prod));
	pbd = &fp->tx_desc_ring[bd_prod].parse_bd;

	memset(pbd, 0, sizeof(struct eth_tx_parse_bd));

	if (xmit_type & XMIT_CSUM) {
		hlen = (skb_network_header(skb) - skb->data) / 2;

		/* for now NS flag is not used in Linux */
		pbd->global_data =
			(hlen | ((skb->protocol == cpu_to_be16(ETH_P_8021Q)) <<
				 ETH_TX_PARSE_BD_LLC_SNAP_EN_SHIFT));

		pbd->ip_hlen = (skb_transport_header(skb) -
				skb_network_header(skb)) / 2;

		hlen += pbd->ip_hlen + tcp_hdrlen(skb) / 2;

		pbd->total_hlen = cpu_to_le16(hlen);
		hlen = hlen*2;

		tx_start_bd->bd_flags.as_bitfield |= ETH_TX_BD_FLAGS_L4_CSUM;

		if (xmit_type & XMIT_CSUM_V4)
			tx_start_bd->bd_flags.as_bitfield |=
						ETH_TX_BD_FLAGS_IP_CSUM;
		else
			tx_start_bd->bd_flags.as_bitfield |=
						ETH_TX_BD_FLAGS_IPV6;

		if (xmit_type & XMIT_CSUM_TCP) {
			pbd->tcp_pseudo_csum = swab16(tcp_hdr(skb)->check);

		} else {
			s8 fix = SKB_CS_OFF(skb); /* signed! */

			pbd->global_data |= ETH_TX_PARSE_BD_UDP_CS_FLG;

			DP(NETIF_MSG_TX_QUEUED,
			   "hlen %d  fix %d  csum before fix %x\n",
			   le16_to_cpu(pbd->total_hlen), fix, SKB_CS(skb));

			/* HW bug: fixup the CSUM */
			pbd->tcp_pseudo_csum =
				bnx2x_csum_fix(skb_transport_header(skb),
					       SKB_CS(skb), fix);

			DP(NETIF_MSG_TX_QUEUED, "csum after fix %x\n",
			   pbd->tcp_pseudo_csum);
		}
	}

	mapping = pci_map_single(bp->pdev, skb->data,
				 skb_headlen(skb), PCI_DMA_TODEVICE);

	tx_start_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
	tx_start_bd->addr_lo = cpu_to_le32(U64_LO(mapping));
	nbd = skb_shinfo(skb)->nr_frags + 2; /* start_bd + pbd + frags */
	tx_start_bd->nbd = cpu_to_le16(nbd);
	tx_start_bd->nbytes = cpu_to_le16(skb_headlen(skb));
	pkt_size = tx_start_bd->nbytes;

	DP(NETIF_MSG_TX_QUEUED, "first bd @%p  addr (%x:%x)  nbd %d"
	   "  nbytes %d  flags %x  vlan %x\n",
	   tx_start_bd, tx_start_bd->addr_hi, tx_start_bd->addr_lo,
	   le16_to_cpu(tx_start_bd->nbd), le16_to_cpu(tx_start_bd->nbytes),
	   tx_start_bd->bd_flags.as_bitfield, le16_to_cpu(tx_start_bd->vlan));

#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
	if (xmit_type & XMIT_GSO) {

		DP(NETIF_MSG_TX_QUEUED,
		   "TSO packet len %d  hlen %d  total len %d  tso size %d\n",
		   skb->len, hlen, skb_headlen(skb),
		   skb_shinfo(skb)->gso_size);

		tx_start_bd->bd_flags.as_bitfield |= ETH_TX_BD_FLAGS_SW_LSO;

		if (unlikely(skb_headlen(skb) > hlen))
			bd_prod = bnx2x_tx_split(bp, fp, tx_buf, &tx_start_bd,
						 hlen, bd_prod, ++nbd);

		pbd->lso_mss = cpu_to_le16(skb_shinfo(skb)->gso_size);
		pbd->tcp_send_seq = swab32(tcp_hdr(skb)->seq);
		pbd->tcp_flags = pbd_tcp_flags(skb);

		if (xmit_type & XMIT_GSO_V4) {
			pbd->ip_id = swab16(ip_hdr(skb)->id);
			pbd->tcp_pseudo_csum =
				swab16(~csum_tcpudp_magic(ip_hdr(skb)->saddr,
							  ip_hdr(skb)->daddr,
							  0, IPPROTO_TCP, 0));

		} else
			pbd->tcp_pseudo_csum =
				swab16(~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
							&ipv6_hdr(skb)->daddr,
							0, IPPROTO_TCP, 0));

		pbd->global_data |= ETH_TX_PARSE_BD_PSEUDO_CS_WITHOUT_LEN;
	}
#endif
	tx_data_bd = (struct eth_tx_bd *)tx_start_bd;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		bd_prod = TX_BD(NEXT_TX_IDX(bd_prod));
		tx_data_bd = &fp->tx_desc_ring[bd_prod].reg_bd;
		if (total_pkt_bd == NULL)
			total_pkt_bd = &fp->tx_desc_ring[bd_prod].reg_bd;

		mapping = pci_map_page(bp->pdev, frag->page, frag->page_offset,
				       frag->size, PCI_DMA_TODEVICE);

		tx_data_bd->addr_hi = cpu_to_le32(U64_HI(mapping));
		tx_data_bd->addr_lo = cpu_to_le32(U64_LO(mapping));
		tx_data_bd->nbytes = cpu_to_le16(frag->size);
		le16_add_cpu(&pkt_size, frag->size);

		DP(NETIF_MSG_TX_QUEUED,
		   "frag %d  bd @%p  addr (%x:%x)  nbytes %d\n",
		   i, tx_data_bd, tx_data_bd->addr_hi, tx_data_bd->addr_lo,
		   le16_to_cpu(tx_data_bd->nbytes));
	}

	DP(NETIF_MSG_TX_QUEUED, "last bd @%p\n", tx_data_bd);

	bd_prod = TX_BD(NEXT_TX_IDX(bd_prod));

	/* now send a tx doorbell, counting the next BD
	 * if the packet contains or ends with it
	 */
	if (TX_BD_POFF(bd_prod) < nbd)
		nbd++;

	if (total_pkt_bd != NULL)
		total_pkt_bd->total_pkt_bytes = pkt_size;

	if (pbd)
		DP(NETIF_MSG_TX_QUEUED,
		   "PBD @%p  ip_data %x  ip_hlen %u  ip_id %u  lso_mss %u"
		   "  tcp_flags %x  xsum %x  seq %u  hlen %u\n",
		   pbd, pbd->global_data, pbd->ip_hlen, pbd->ip_id,
		   pbd->lso_mss, pbd->tcp_flags, pbd->tcp_pseudo_csum,
		   pbd->tcp_send_seq, le16_to_cpu(pbd->total_hlen));

	DP(NETIF_MSG_TX_QUEUED, "doorbell: nbd %d  bd %u\n", nbd, bd_prod);

	/*
	 * Make sure that the BD data is updated before updating the producer
	 * since FW might read the BD right after the producer is updated.
	 * This is only applicable for weak-ordered memory model archs such
	 * as IA-64. The following barrier is also mandatory since FW will
	 * assumes packets must have BDs.
	 */
	wmb();

	fp->tx_db.data.prod += nbd;
	barrier();
	DOORBELL(bp, fp->index, fp->tx_db.raw);

	mmiowb();

	fp->tx_bd_prod += nbd;
#if (LINUX_VERSION_CODE < 0x02061f) /* ! BNX2X_UPSTREAM */
	/* In kernels starting from 2.6.31 netdev layer does this */
	dev->trans_start = jiffies;
#endif

	if (unlikely(bnx2x_tx_avail(fp) < MAX_SKB_FRAGS + 3)) {
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
		netif_tx_stop_queue(txq);
#else
		netif_stop_queue(dev);
#endif
		/* We want bnx2x_tx_int to "see" the updated tx_bd_prod
		   if we put Tx into XOFF state. */
		smp_mb();
		fp->eth_q_stats.driver_xoff++;
		if (bnx2x_tx_avail(fp) >= MAX_SKB_FRAGS + 3)
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
			netif_tx_wake_queue(txq);
#else
			netif_wake_queue(dev);
#endif
	}
	fp->tx_pkt++;

	return NETDEV_TX_OK;
}

/* called with rtnl_lock */
int bnx2x_open(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	netif_carrier_off(dev);

	bnx2x_set_power_state(bp, PCI_D0);

	return bnx2x_nic_load(bp, LOAD_OPEN);
}

/* called with rtnl_lock */
int bnx2x_close(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	/* Unload the driver, release IRQs */
	bnx2x_nic_unload(bp, UNLOAD_CLOSE);
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	if (atomic_read(&bp->pdev->enable_cnt) == 1)
#endif
		if (!CHIP_REV_IS_SLOW(bp))
			bnx2x_set_power_state(bp, PCI_D3hot);

	return 0;
}

/* called with netif_tx_lock from dev_mcast.c */
static void bnx2x_set_rx_mode(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	u32 rx_mode = BNX2X_RX_MODE_NORMAL;
	int port = BP_PORT(bp);

	if (bp->state != BNX2X_STATE_OPEN) {
		DP(NETIF_MSG_IFUP, "state is %x, returning\n", bp->state);
		return;
	}

	DP(NETIF_MSG_IFUP, "dev->flags = %x\n", dev->flags);

	if (dev->flags & IFF_PROMISC)
		rx_mode = BNX2X_RX_MODE_PROMISC;

	else if ((dev->flags & IFF_ALLMULTI) ||
		 ((dev->mc_count > BNX2X_MAX_MULTICAST) && CHIP_IS_E1(bp)))
		rx_mode = BNX2X_RX_MODE_ALLMULTI;

	else { /* some multicasts */
		if (CHIP_IS_E1(bp)) {
			int i, old, offset;
			struct dev_mc_list *mclist;
			struct mac_configuration_cmd *config =
						bnx2x_sp(bp, mcast_config);

			for (i = 0, mclist = dev->mc_list;
			     mclist && (i < dev->mc_count);
			     i++, mclist = mclist->next) {

				config->config_table[i].
					cam_entry.msb_mac_addr =
					swab16(*(u16 *)&mclist->dmi_addr[0]);
				config->config_table[i].
					cam_entry.middle_mac_addr =
					swab16(*(u16 *)&mclist->dmi_addr[2]);
				config->config_table[i].
					cam_entry.lsb_mac_addr =
					swab16(*(u16 *)&mclist->dmi_addr[4]);
				config->config_table[i].cam_entry.flags =
							cpu_to_le16(port);
				config->config_table[i].
					target_table_entry.flags = 0;
				config->config_table[i].target_table_entry.
					clients_bit_vector =
						cpu_to_le32(1 << BP_L_ID(bp));
				config->config_table[i].
					target_table_entry.vlan_id = 0;

				DP(NETIF_MSG_IFUP,
				   "setting MCAST[%d] (%04x:%04x:%04x)\n", i,
				   config->config_table[i].
						cam_entry.msb_mac_addr,
				   config->config_table[i].
						cam_entry.middle_mac_addr,
				   config->config_table[i].
						cam_entry.lsb_mac_addr);
			}
			old = config->hdr.length;
			if (old > i) {
				for (; i < old; i++) {
					if (CAM_IS_INVALID(config->
							   config_table[i])) {
						/* already invalidated */
						break;
					}
					/* invalidate */
					CAM_INVALIDATE(config->
						       config_table[i]);
				}
			}

			if (CHIP_REV_IS_SLOW(bp))
				offset = BNX2X_MAX_EMUL_MULTI*(1 + port);
			else
				offset = BNX2X_MAX_MULTICAST*(1 + port);

			config->hdr.length = i;
			config->hdr.offset = offset;
			config->hdr.client_id = bp->fp->cl_id;
			config->hdr.reserved1 = 0;

			bp->set_mac_pending++;
			smp_wmb();

			bnx2x_sp_post(bp, RAMROD_CMD_ID_ETH_SET_MAC, 0,
				   U64_HI(bnx2x_sp_mapping(bp, mcast_config)),
				   U64_LO(bnx2x_sp_mapping(bp, mcast_config)),
				      0);
		} else { /* E1H */
			/* Accept one or more multicasts */
			struct dev_mc_list *mclist;
			u32 mc_filter[MC_HASH_SIZE];
			u32 crc, bit, regidx;
			int i;

			memset(mc_filter, 0, 4 * MC_HASH_SIZE);

			for (i = 0, mclist = dev->mc_list;
			     mclist && (i < dev->mc_count);
			     i++, mclist = mclist->next) {

#if (LINUX_VERSION_CODE >= 0x02061b) /* BNX2X_UPSTREAM */
				DP(NETIF_MSG_IFUP, "Adding mcast MAC: %pM\n",
				   mclist->dmi_addr);
#else
				DP(NETIF_MSG_IFUP, "Adding mcast MAC: "
				   "%02x:%02x:%02x:%02x:%02x:%02x\n",
				   mclist->dmi_addr[0], mclist->dmi_addr[1],
				   mclist->dmi_addr[2], mclist->dmi_addr[3],
				   mclist->dmi_addr[4], mclist->dmi_addr[5]);
#endif

				crc = crc32c_le(0, mclist->dmi_addr, ETH_ALEN);
				bit = (crc >> 24) & 0xff;
				regidx = bit >> 5;
				bit &= 0x1f;
				mc_filter[regidx] |= (1 << bit);
			}

			for (i = 0; i < MC_HASH_SIZE; i++)
				REG_WR(bp, MC_HASH_OFFSET(bp, i),
				       mc_filter[i]);
		}
	}

	bp->rx_mode = rx_mode;
	bnx2x_set_storm_rx_mode(bp);
}

/* called with rtnl_lock */
static int bnx2x_change_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;
	struct bnx2x *bp = netdev_priv(dev);

	if (!is_valid_ether_addr((u8 *)(addr->sa_data)))
		return -EINVAL;

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	if (netif_running(dev)) {
		if (CHIP_IS_E1(bp))
			bnx2x_set_eth_mac_addr_e1(bp, 1);
		else
			bnx2x_set_eth_mac_addr_e1h(bp, 1);
	}

	return 0;
}

/* called with rtnl_lock */
static int bnx2x_mdio_read(struct net_device *netdev, int prtad,
			   int devad, u16 addr)
{
	struct bnx2x *bp = netdev_priv(netdev);
	u16 value;
	int rc;
	u32 phy_type = XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);

	DP(NETIF_MSG_LINK, "mdio_read: prtad 0x%x, devad 0x%x, addr 0x%x\n",
	   prtad, devad, addr);

	if (prtad != bp->mdio.prtad) {
		DP(NETIF_MSG_LINK, "prtad missmatch (cmd:0x%x != bp:0x%x)\n",
		   prtad, bp->mdio.prtad);
		return -EINVAL;
	}

	/* The HW expects different devad if CL22 is used */
	devad = (devad == MDIO_DEVAD_NONE) ? DEFAULT_PHY_DEV_ADDR : devad;

	bnx2x_acquire_phy_lock(bp);
	rc = bnx2x_cl45_read(bp, BP_PORT(bp), phy_type, prtad,
			     devad, addr, &value);
	bnx2x_release_phy_lock(bp);
	DP(NETIF_MSG_LINK, "mdio_read_val 0x%x rc = 0x%x\n", value, rc);

	if (!rc)
		rc = value;
	return rc;
}

/* called with rtnl_lock */
static int bnx2x_mdio_write(struct net_device *netdev, int prtad, int devad,
			    u16 addr, u16 value)
{
	struct bnx2x *bp = netdev_priv(netdev);
	u32 ext_phy_type = XGXS_EXT_PHY_TYPE(bp->link_params.ext_phy_config);
	int rc;

	DP(NETIF_MSG_LINK, "mdio_write: prtad 0x%x, devad 0x%x, addr 0x%x,"
			   " value 0x%x\n", prtad, devad, addr, value);

	if (prtad != bp->mdio.prtad) {
		DP(NETIF_MSG_LINK, "prtad missmatch (cmd:0x%x != bp:0x%x)\n",
		   prtad, bp->mdio.prtad);
		return -EINVAL;
	}

	/* The HW expects different devad if CL22 is used */
	devad = (devad == MDIO_DEVAD_NONE) ? DEFAULT_PHY_DEV_ADDR : devad;

	bnx2x_acquire_phy_lock(bp);
	rc = bnx2x_cl45_write(bp, BP_PORT(bp), ext_phy_type, prtad,
			      devad, addr, value);
	bnx2x_release_phy_lock(bp);
	return rc;
}

/* called with rtnl_lock */
static int bnx2x_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct bnx2x *bp = netdev_priv(dev);
	struct mii_ioctl_data *mdio = if_mii(ifr);

	DP(NETIF_MSG_LINK, "ioctl: phy id 0x%x, reg 0x%x, val_in 0x%x\n",
	   mdio->phy_id, mdio->reg_num, mdio->val_in);

	if (!netif_running(dev))
		return -EAGAIN;

	switch(cmd) {
#if defined(VMWARE_ESX_DDK_VERSION) && (VMWARE_ESX_DDK_VERSION >= 40000) /* ! BNX2X_UPSTREAM */
#define SIOBNX2CIM     0x89F0
	case SIOBNX2CIM:
		return bnx2x_ioctl_cim(dev, ifr);
#endif
	default:
		return mdio_mii_ioctl(&bp->mdio, mdio, cmd);
	}
}

/* called with rtnl_lock */
static int bnx2x_change_mtu(struct net_device *dev, int new_mtu)
{
	struct bnx2x *bp = netdev_priv(dev);
	int rc = 0;

	if ((new_mtu > ETH_MAX_JUMBO_PACKET_SIZE) ||
	    ((new_mtu + ETH_HLEN) < ETH_MIN_PACKET_SIZE))
		return -EINVAL;

	/* This does not race with packet allocation
	 * because the actual alloc size is
	 * only updated as part of load
	 */
	dev->mtu = new_mtu;

	if (netif_running(dev)) {
		bnx2x_nic_unload(bp, UNLOAD_NORMAL);
		rc = bnx2x_nic_load(bp, LOAD_NORMAL);
	}

	return rc;
}

static void bnx2x_tx_timeout(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

#ifdef BNX2X_STOP_ON_ERROR
	if (!bp->panic)
		bnx2x_panic();
#endif
	/* This allows the netif to be shutdown gracefully before resetting */
	schedule_work(&bp->reset_task);
}

#ifdef BCM_VLAN
/* called with rtnl_lock */
static void bnx2x_vlan_rx_register(struct net_device *dev,
				   struct vlan_group *vlgrp)
{
	struct bnx2x *bp = netdev_priv(dev);

	bp->vlgrp = vlgrp;

	/* Set flags according to the required capabilities */
	bp->flags &= ~(HW_VLAN_RX_FLAG | HW_VLAN_TX_FLAG);

	if (dev->features & NETIF_F_HW_VLAN_TX)
		bp->flags |= HW_VLAN_TX_FLAG;

	if (dev->features & NETIF_F_HW_VLAN_RX)
		bp->flags |= HW_VLAN_RX_FLAG;

	if (netif_running(dev))
		bnx2x_set_client_config(bp);
}

#if (LINUX_VERSION_CODE < 0x020616) /* ! BNX2X_UPSTREAM */
static void bnx2x_vlan_rx_kill_vid(struct net_device *dev, uint16_t vid)
{
	struct bnx2x *bp = netdev_priv(dev);

	if (bp->vlgrp)
		vlan_group_set_device(bp->vlgrp, vid, NULL);
}
#endif
#endif

#if defined(HAVE_POLL_CONTROLLER) || defined(CONFIG_NET_POLL_CONTROLLER)
static void poll_bnx2x(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);

	disable_irq(bp->pdev->irq);
#if (LINUX_VERSION_CODE < 0x020613) && (VMWARE_ESX_DDK_VERSION < 40000)
	bnx2x_interrupt(bp->pdev->irq, dev, NULL);
#else /* BNX2X_UPSTREAM */
	bnx2x_interrupt(bp->pdev->irq, dev);
#endif
	enable_irq(bp->pdev->irq);
}
#endif

#if (LINUX_VERSION_CODE >= 0x02061e) /* BNX2X_UPSTREAM */
static const struct net_device_ops bnx2x_netdev_ops = {
	.ndo_open		= bnx2x_open,
	.ndo_stop		= bnx2x_close,
	.ndo_start_xmit		= bnx2x_start_xmit,
#ifdef BNX2X_SAFC /* ! BNX2X_UPSTREAM */
	.ndo_select_queue	= bnx2x_select_queue,
#endif
	.ndo_set_multicast_list	= bnx2x_set_rx_mode,
	.ndo_set_mac_address	= bnx2x_change_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_do_ioctl		= bnx2x_ioctl,
	.ndo_change_mtu		= bnx2x_change_mtu,
	.ndo_tx_timeout		= bnx2x_tx_timeout,
#ifdef BCM_VLAN
	.ndo_vlan_rx_register	= bnx2x_vlan_rx_register,
#endif
#if defined(HAVE_POLL_CONTROLLER) || defined(CONFIG_NET_POLL_CONTROLLER)
	.ndo_poll_controller	= poll_bnx2x,
#endif
};
#endif

static int __devinit bnx2x_init_dev(struct pci_dev *pdev,
				    struct net_device *dev)
{
	struct bnx2x *bp;
	int rc;

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	SET_MODULE_OWNER(dev);
#endif
#if (LINUX_VERSION_CODE >= 0x020419) /* BNX2X_UPSTREAM */
	SET_NETDEV_DEV(dev, &pdev->dev);
#endif
	bp = netdev_priv(dev);

	bp->dev = dev;
	bp->pdev = pdev;
	bp->flags = 0;
	bp->func = PCI_FUNC(pdev->devfn);

	rc = pci_enable_device(pdev);
	if (rc) {
		printk(KERN_ERR PFX "Cannot enable PCI device, aborting\n");
		goto err_out;
	}

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		printk(KERN_ERR PFX "Cannot find PCI device base address,"
		       " aborting\n");
		rc = -ENODEV;
		goto err_out_disable;
	}

	if (!(pci_resource_flags(pdev, 2) & IORESOURCE_MEM)) {
		printk(KERN_ERR PFX "Cannot find second PCI device"
		       " base address, aborting\n");
		rc = -ENODEV;
		goto err_out_disable;
	}

#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	if (atomic_read(&pdev->enable_cnt) == 1) {
#endif
		rc = pci_request_regions(pdev, DRV_MODULE_NAME);
		if (rc) {
			printk(KERN_ERR PFX "Cannot obtain PCI resources,"
			       " aborting\n");
			goto err_out_disable;
		}

		pci_set_master(pdev);
#if (LINUX_VERSION_CODE >= 0x02060b) /* BNX2X_UPSTREAM */
		pci_save_state(pdev);
#else
		pci_save_state(pdev, bp->pci_state);
#endif
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	}
#endif

	bp->pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	if (bp->pm_cap == 0) {
		printk(KERN_ERR PFX "Cannot find power management"
		       " capability, aborting\n");
		rc = -EIO;
		goto err_out_release;
	}

	bp->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	if (bp->pcie_cap == 0) {
		printk(KERN_ERR PFX "Cannot find PCI Express capability,"
		       " aborting\n");
		rc = -EIO;
		goto err_out_release;
	}

	if (pci_set_dma_mask(pdev, DMA_BIT_MASK(64)) == 0) {
		bp->flags |= USING_DAC_FLAG;
		if (pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)) != 0) {
			printk(KERN_ERR PFX "pci_set_consistent_dma_mask"
			       " failed, aborting\n");
			rc = -EIO;
			goto err_out_release;
		}

	} else if (pci_set_dma_mask(pdev, DMA_BIT_MASK(32)) != 0) {
		printk(KERN_ERR PFX "System does not support DMA,"
		       " aborting\n");
		rc = -EIO;
		goto err_out_release;
	}

	dev->mem_start = pci_resource_start(pdev, 0);
	dev->base_addr = dev->mem_start;
	dev->mem_end = pci_resource_end(pdev, 0);

	dev->irq = pdev->irq;

#if (LINUX_VERSION_CODE >= 0x02061c) /* BNX2X_UPSTREAM */
	bp->regview = pci_ioremap_bar(pdev, 0);
#else
	bp->regview = ioremap_nocache(dev->base_addr,
				      pci_resource_len(pdev, 0));
#endif
	if (!bp->regview) {
		printk(KERN_ERR PFX "Cannot map register space, aborting\n");
		rc = -ENOMEM;
		goto err_out_release;
	}

	bp->doorbells = ioremap_nocache(pci_resource_start(pdev, 2),
					min_t(u64, BNX2X_DB_SIZE,
					      pci_resource_len(pdev, 2)));
	if (!bp->doorbells) {
		printk(KERN_ERR PFX "Cannot map doorbell space, aborting\n");
		rc = -ENOMEM;
		goto err_out_unmap;
	}

	bnx2x_set_power_state(bp, PCI_D0);

	/* clean indirect addresses */
	pci_write_config_dword(bp->pdev, PCICFG_GRC_ADDRESS,
			       PCICFG_VENDOR_ID_OFFSET);
	REG_WR(bp, PXP2_REG_PGL_ADDR_88_F0 + BP_PORT(bp)*16, 0);
	REG_WR(bp, PXP2_REG_PGL_ADDR_8C_F0 + BP_PORT(bp)*16, 0);
	REG_WR(bp, PXP2_REG_PGL_ADDR_90_F0 + BP_PORT(bp)*16, 0);
	REG_WR(bp, PXP2_REG_PGL_ADDR_94_F0 + BP_PORT(bp)*16, 0);

#if (LINUX_VERSION_CODE < 0x02061e) /* ! BNX2X_UPSTREAM */
#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	dev->get_stats = bnx2x_get_stats;
#endif
	dev->hard_start_xmit = bnx2x_start_xmit;
#endif
	dev->watchdog_timeo = TX_TIMEOUT;

#if (LINUX_VERSION_CODE >= 0x02061e) /* BNX2X_UPSTREAM */
	dev->netdev_ops = &bnx2x_netdev_ops;
#endif
	dev->ethtool_ops = &bnx2x_ethtool_ops;
#if (LINUX_VERSION_CODE < 0x02061e) /* ! BNX2X_UPSTREAM */
	dev->open = bnx2x_open;
	dev->stop = bnx2x_close;
	dev->set_multicast_list = bnx2x_set_rx_mode;
	dev->set_mac_address = bnx2x_change_mac_addr;
	dev->do_ioctl = bnx2x_ioctl;
	dev->change_mtu = bnx2x_change_mtu;
	dev->tx_timeout = bnx2x_tx_timeout;
#ifdef BCM_VLAN
	dev->vlan_rx_register = bnx2x_vlan_rx_register;
#if (LINUX_VERSION_CODE < 0x020616) /* ! BNX2X_UPSTREAM */
	dev->vlan_rx_kill_vid = bnx2x_vlan_rx_kill_vid;
#endif
#endif
#if defined(HAVE_POLL_CONTROLLER) || defined(CONFIG_NET_POLL_CONTROLLER)
	dev->poll_controller = poll_bnx2x;
#endif
#endif
	dev->features |= NETIF_F_SG;
	dev->features |= NETIF_F_HW_CSUM;
	if (bp->flags & USING_DAC_FLAG)
		dev->features |= NETIF_F_HIGHDMA;
#ifdef NETIF_F_TSO /* BNX2X_UPSTREAM */
	dev->features |= (NETIF_F_TSO | NETIF_F_TSO_ECN);
#endif
#ifdef NETIF_F_TSO6 /* BNX2X_UPSTREAM */
	dev->features |= NETIF_F_TSO6;
#endif
#ifdef BCM_VLAN
	dev->features |= (NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX);
	bp->flags |= (HW_VLAN_RX_FLAG | HW_VLAN_TX_FLAG);

#if (LINUX_VERSION_CODE >= 0x02061a) /* BNX2X_UPSTREAM */
	dev->vlan_features |= NETIF_F_SG;
	dev->vlan_features |= NETIF_F_HW_CSUM;
	if (bp->flags & USING_DAC_FLAG)
		dev->vlan_features |= NETIF_F_HIGHDMA;
	dev->vlan_features |= (NETIF_F_TSO | NETIF_F_TSO_ECN);
	dev->vlan_features |= NETIF_F_TSO6;
#endif
#endif

	/* get_port_hwinfo() will set prtad and mmds properly */
	bp->mdio.prtad = MDIO_PRTAD_NONE;
	bp->mdio.mmds = 0;
	bp->mdio.mode_support = MDIO_SUPPORTS_C45 | MDIO_EMULATE_C22;
	bp->mdio.dev = dev;
	bp->mdio.mdio_read = bnx2x_mdio_read;
	bp->mdio.mdio_write = bnx2x_mdio_write;

	return 0;

err_out_unmap:
	if (bp->regview) {
		iounmap(bp->regview);
		bp->regview = NULL;
	}
	if (bp->doorbells) {
		iounmap(bp->doorbells);
		bp->doorbells = NULL;
	}

err_out_release:
#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	if (atomic_read(&pdev->enable_cnt) == 1)
#endif
		pci_release_regions(pdev);

err_out_disable:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

err_out:
	return rc;
}

static void __devinit bnx2x_get_pcie_width_speed(struct bnx2x *bp,
						 int *width, int *speed)
{
	u32 val = REG_RD(bp, PCICFG_OFFSET + PCICFG_LINK_CONTROL);

	*width = (val & PCICFG_LINK_WIDTH) >> PCICFG_LINK_WIDTH_SHIFT;

	/* return value of 1=2.5GHz 2=5GHz */
	*speed = (val & PCICFG_LINK_SPEED) >> PCICFG_LINK_SPEED_SHIFT;
}

#if defined(BNX2X_UPSTREAM) && !defined(BNX2X_USE_INIT_VALUES) /* BNX2X_UPSTREAM */
static int __devinit bnx2x_check_firmware(struct bnx2x *bp)
{
	const struct firmware *firmware = bp->firmware;
	struct bnx2x_fw_file_hdr *fw_hdr;
	struct bnx2x_fw_file_section *sections;
	u32 offset, len, num_ops;
	u16 *ops_offsets;
	int i;
	const u8 *fw_ver;

	if (firmware->size < sizeof(struct bnx2x_fw_file_hdr))
		return -EINVAL;

	fw_hdr = (struct bnx2x_fw_file_hdr *)firmware->data;
	sections = (struct bnx2x_fw_file_section *)fw_hdr;

	/* Make sure none of the offsets and sizes make us read beyond
	 * the end of the firmware data */
	for (i = 0; i < sizeof(*fw_hdr) / sizeof(*sections); i++) {
		offset = be32_to_cpu(sections[i].offset);
		len = be32_to_cpu(sections[i].len);
		if (offset + len > firmware->size) {
			printk(KERN_ERR PFX "Section %d length is out of "
					    "bounds\n", i);
			return -EINVAL;
		}
	}

	/* Likewise for the init_ops offsets */
	offset = be32_to_cpu(fw_hdr->init_ops_offsets.offset);
	ops_offsets = (u16 *)(firmware->data + offset);
	num_ops = be32_to_cpu(fw_hdr->init_ops.len) / sizeof(struct raw_op);

	for (i = 0; i < be32_to_cpu(fw_hdr->init_ops_offsets.len) / 2; i++) {
		if (be16_to_cpu(ops_offsets[i]) > num_ops) {
			printk(KERN_ERR PFX "Section offset %d is out of "
					    "bounds\n", i);
			return -EINVAL;
		}
	}

	/* Check FW version */
	offset = be32_to_cpu(fw_hdr->fw_version.offset);
	fw_ver = firmware->data + offset;
	if ((fw_ver[0] != BCM_5710_FW_MAJOR_VERSION) ||
	    (fw_ver[1] != BCM_5710_FW_MINOR_VERSION) ||
	    (fw_ver[2] != BCM_5710_FW_REVISION_VERSION) ||
	    (fw_ver[3] != BCM_5710_FW_ENGINEERING_VERSION)) {
		printk(KERN_ERR PFX "Bad FW version:%d.%d.%d.%d."
				    " Should be %d.%d.%d.%d\n",
		       fw_ver[0], fw_ver[1], fw_ver[2],
		       fw_ver[3], BCM_5710_FW_MAJOR_VERSION,
		       BCM_5710_FW_MINOR_VERSION,
		       BCM_5710_FW_REVISION_VERSION,
		       BCM_5710_FW_ENGINEERING_VERSION);
		return -EINVAL;
	}

	return 0;
}

static inline void be32_to_cpu_n(const u8 *_source, u8 *_target, u32 n)
{
	const __be32 *source = (const __be32 *)_source;
	u32 *target = (u32 *)_target;
	u32 i;

	for (i = 0; i < n/4; i++)
		target[i] = be32_to_cpu(source[i]);
}

/*
   Ops array is stored in the following format:
   {op(8bit), offset(24bit, big endian), data(32bit, big endian)}
 */
static inline void bnx2x_prep_ops(const u8 *_source, u8 *_target, u32 n)
{
	const __be32 *source = (const __be32 *)_source;
	struct raw_op *target = (struct raw_op *)_target;
	u32 i, j, tmp;

	for (i = 0, j = 0; i < n/8; i++, j += 2) {
		tmp = be32_to_cpu(source[j]);
		target[i].op = (tmp >> 24) & 0xff;
		target[i].offset = tmp & 0xffffff;
		target[i].raw_data = be32_to_cpu(source[j + 1]);
	}
}

static inline void be16_to_cpu_n(const u8 *_source, u8 *_target, u32 n)
{
	const __be16 *source = (const __be16 *)_source;
	u16 *target = (u16 *)_target;
	u32 i;

	for (i = 0; i < n/2; i++)
		target[i] = be16_to_cpu(source[i]);
}

#define BNX2X_ALLOC_AND_SET(arr, lbl, func) \
	do { \
		u32 len = be32_to_cpu(fw_hdr->arr.len); \
		bp->arr = kmalloc(len, GFP_KERNEL); \
		if (!bp->arr) { \
			printk(KERN_ERR PFX "Failed to allocate %d bytes " \
					    "for "#arr"\n", len); \
			goto lbl; \
		} \
		func(bp->firmware->data + be32_to_cpu(fw_hdr->arr.offset), \
		     (u8 *)bp->arr, len); \
	} while (0)

static int __devinit bnx2x_init_firmware(struct bnx2x *bp, struct device *dev)
{
	const char *fw_file_name;
	struct bnx2x_fw_file_hdr *fw_hdr;
	int rc;

	if (CHIP_IS_E1(bp))
		fw_file_name = FW_FILE_NAME_E1;
	else if (CHIP_IS_E1H(bp))
		fw_file_name = FW_FILE_NAME_E1H;
	else {
		printk(KERN_ERR PFX "Unsupported chip revision\n");
		return -EINVAL;
	}

	printk(KERN_INFO PFX "Loading %s\n", fw_file_name);

	rc = request_firmware(&bp->firmware, fw_file_name, dev);
	if (rc) {
		printk(KERN_ERR PFX "Can't load firmware file %s\n",
		       fw_file_name);
		goto request_firmware_exit;
	}

	rc = bnx2x_check_firmware(bp);
	if (rc) {
		printk(KERN_ERR PFX "Corrupt firmware file %s\n", fw_file_name);
		goto request_firmware_exit;
	}

	fw_hdr = (struct bnx2x_fw_file_hdr *)bp->firmware->data;

	/* Initialize the pointers to the init arrays */
	/* Blob */
	BNX2X_ALLOC_AND_SET(init_data, request_firmware_exit, be32_to_cpu_n);

	/* Opcodes */
	BNX2X_ALLOC_AND_SET(init_ops, init_ops_alloc_err, bnx2x_prep_ops);

	/* Offsets */
	BNX2X_ALLOC_AND_SET(init_ops_offsets, init_offsets_alloc_err,
			    be16_to_cpu_n);

	/* STORMs firmware */
	INIT_TSEM_INT_TABLE_DATA(bp) = bp->firmware->data +
			be32_to_cpu(fw_hdr->tsem_int_table_data.offset);
	INIT_TSEM_PRAM_DATA(bp)      = bp->firmware->data +
			be32_to_cpu(fw_hdr->tsem_pram_data.offset);
	INIT_USEM_INT_TABLE_DATA(bp) = bp->firmware->data +
			be32_to_cpu(fw_hdr->usem_int_table_data.offset);
	INIT_USEM_PRAM_DATA(bp)      = bp->firmware->data +
			be32_to_cpu(fw_hdr->usem_pram_data.offset);
	INIT_XSEM_INT_TABLE_DATA(bp) = bp->firmware->data +
			be32_to_cpu(fw_hdr->xsem_int_table_data.offset);
	INIT_XSEM_PRAM_DATA(bp)      = bp->firmware->data +
			be32_to_cpu(fw_hdr->xsem_pram_data.offset);
	INIT_CSEM_INT_TABLE_DATA(bp) = bp->firmware->data +
			be32_to_cpu(fw_hdr->csem_int_table_data.offset);
	INIT_CSEM_PRAM_DATA(bp)      = bp->firmware->data +
			be32_to_cpu(fw_hdr->csem_pram_data.offset);

	return 0;

init_offsets_alloc_err:
	kfree(bp->init_ops);
init_ops_alloc_err:
	kfree(bp->init_data);
request_firmware_exit:
	release_firmware(bp->firmware);

	return rc;
}

#else

static int __devinit bnx2x_init_firmware(struct bnx2x *bp, struct device *dev)
{
	if (CHIP_IS_E1(bp))
		bnx2x_init_e1_firmware(bp);

	else if (CHIP_IS_E1H(bp))
		bnx2x_init_e1h_firmware(bp);

	else {
		printk(KERN_ERR PFX "Unsupported chip revision\n");
		return -EINVAL;
	}

	return 0;
}
#endif

static int __devinit bnx2x_init_one(struct pci_dev *pdev,
				    const struct pci_device_id *ent)
{
	struct net_device *dev = NULL;
	struct bnx2x *bp;
	int pcie_width, pcie_speed;
	int rc;

	/* dev zeroed in init_etherdev */
#ifdef BNX2X_MULTI_QUEUE /* BNX2X_UPSTREAM */
	dev = alloc_etherdev_mq(sizeof(*bp), MAX_CONTEXT);
#else
	dev = alloc_etherdev(sizeof(*bp));
#endif
	if (!dev) {
		printk(KERN_ERR PFX "Cannot allocate net device\n");
		return -ENOMEM;
	}

	bp = netdev_priv(dev);
	bp->msglevel = debug;

	pci_set_drvdata(pdev, dev);

	rc = bnx2x_init_dev(pdev, dev);
	if (rc < 0) {
		free_netdev(dev);
		return rc;
	}

	rc = bnx2x_init_bp(bp);
	if (rc)
		goto init_one_exit;

	/* Set init arrays */
	rc = bnx2x_init_firmware(bp, &pdev->dev);
	if (rc) {
		printk(KERN_ERR PFX "Error loading firmware\n");
		goto init_one_exit;
	}

	rc = register_netdev(dev);
	if (rc) {
		dev_err(&pdev->dev, "Cannot register net device\n");
		goto init_one_exit;
	}

	bnx2x_get_pcie_width_speed(bp, &pcie_width, &pcie_speed);
	printk(KERN_INFO "%s: %s (%c%d) PCI-E x%d %s found at mem %lx,"
	       " IRQ %d, ", dev->name, board_info[ent->driver_data].name,
	       (CHIP_REV(bp) >> 12) + 'A', (CHIP_METAL(bp) >> 4),
	       pcie_width, (pcie_speed == 2) ? "5GHz (Gen2)" : "2.5GHz",
	       dev->base_addr, bp->pdev->irq);
#if (LINUX_VERSION_CODE >= 0x02061b) /* BNX2X_UPSTREAM */
	printk(KERN_CONT "node addr %pM\n", dev->dev_addr);
#else
	printk(KERN_CONT "node addr ");
	{
		int i;

		for (i = 0; i < ETH_ALEN; i++)
			printk(KERN_CONT "%2.2x", dev->dev_addr[i]);
	}
	printk(KERN_CONT "\n");
#endif
#if defined(__VMKLNX__) && defined(__VMKNETDDI_QUEUEOPS__) /* ! BNX2X_UPSTREAM */
	if(multi_mode != 0) {
		VMKNETDDI_REGISTER_QUEUEOPS(dev, bnx2x_netqueue_ops);
		printk(KERN_INFO "%s: VMware NetQueue Ops is registered\n",
		       dev->name);
	}
#endif

	return 0;

init_one_exit:
	if (bp->regview)
		iounmap(bp->regview);

	if (bp->doorbells)
		iounmap(bp->doorbells);

	free_netdev(dev);

#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	if (atomic_read(&pdev->enable_cnt) == 1)
#endif
		pci_release_regions(pdev);

	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	return rc;
}

static void __devexit bnx2x_remove_one(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp;

	if (!dev) {
		printk(KERN_ERR PFX "BAD net device from bnx2x_init_one\n");
		return;
	}
	bp = netdev_priv(dev);

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	flush_scheduled_work();
#endif
	unregister_netdev(dev);

#if defined(BNX2X_UPSTREAM) && !defined(BNX2X_USE_INIT_VALUES) /* BNX2X_UPSTREAM */
	kfree(bp->init_ops_offsets);
	kfree(bp->init_ops);
	kfree(bp->init_data);
	release_firmware(bp->firmware);
#endif

	if (bp->regview)
		iounmap(bp->regview);

	if (bp->doorbells)
		iounmap(bp->doorbells);

	free_netdev(dev);

#if (LINUX_VERSION_CODE >= 0x020614) /* BNX2X_UPSTREAM */
	if (atomic_read(&pdev->enable_cnt) == 1)
#endif
		pci_release_regions(pdev);

	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
}

static int bnx2x_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp;

	if (!dev) {
		printk(KERN_ERR PFX "BAD net device from bnx2x_init_one\n");
		return -ENODEV;
	}
	bp = netdev_priv(dev);

	rtnl_lock();

#if (LINUX_VERSION_CODE >= 0x02060b) /* BNX2X_UPSTREAM */
	pci_save_state(pdev);
#else
	pci_save_state(pdev, bp->pci_state);
#endif

	if (!netif_running(dev)) {
		rtnl_unlock();
		return 0;
	}

#if (LINUX_VERSION_CODE < 0x020618) /* ! BNX2X_UPSTREAM */
	flush_scheduled_work();
#endif
	netif_device_detach(dev);

	bnx2x_nic_unload(bp, UNLOAD_CLOSE);

	bnx2x_set_power_state(bp, pci_choose_state(pdev, state));

	rtnl_unlock();

	return 0;
}

static int bnx2x_resume(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp;
	int rc;

	if (!dev) {
		printk(KERN_ERR PFX "BAD net device from bnx2x_init_one\n");
		return -ENODEV;
	}
	bp = netdev_priv(dev);

	rtnl_lock();

#if (LINUX_VERSION_CODE >= 0x02060b) /* BNX2X_UPSTREAM */
	pci_restore_state(pdev);
#else
	pci_restore_state(pdev, bp->pci_state);
#endif

	if (!netif_running(dev)) {
		rtnl_unlock();
		return 0;
	}

	bnx2x_set_power_state(bp, PCI_D0);
	netif_device_attach(dev);

	rc = bnx2x_nic_load(bp, LOAD_OPEN);

	rtnl_unlock();

	return rc;
}

#if (LINUX_VERSION_CODE >= 0x02060b) && !defined(__VMKLNX__) /* BNX2X_UPSTREAM */
static int bnx2x_eeh_nic_unload(struct bnx2x *bp)
{
	int i;

	bp->state = BNX2X_STATE_ERROR;

	bp->rx_mode = BNX2X_RX_MODE_NONE;

	bnx2x_netif_stop(bp, 0);

	del_timer_sync(&bp->timer);
	bp->stats_state = STATS_STATE_DISABLED;
	DP(BNX2X_MSG_STATS, "stats_state - DISABLED\n");

	/* Release IRQs */
	bnx2x_free_irq(bp);

	if (CHIP_IS_E1(bp)) {
		struct mac_configuration_cmd *config =
						bnx2x_sp(bp, mcast_config);

		for (i = 0; i < config->hdr.length; i++)
			CAM_INVALIDATE(config->config_table[i]);
	}

	/* Free SKBs, SGEs, TPA pool and driver internals */
	bnx2x_free_skbs(bp);
	for_each_queue(bp, i)
		bnx2x_free_rx_sge_range(bp, bp->fp + i, NUM_RX_SGE);
#if (defined(BNX2X_NEW_NAPI) && (LINUX_VERSION_CODE >= 0x02061b)) || defined(USE_NAPI_GRO) /* BNX2X_UPSTREAM */
	for_each_queue(bp, i)
		netif_napi_del(&bnx2x_fp(bp, i, napi));
#endif
	bnx2x_free_mem(bp);

	bp->state = BNX2X_STATE_CLOSED;

	netif_carrier_off(bp->dev);

	return 0;
}

static void bnx2x_eeh_recover(struct bnx2x *bp)
{
	u32 val;

	mutex_init(&bp->port.phy_mutex);

	bp->common.shmem_base = REG_RD(bp, MISC_REG_SHARED_MEM_ADDR);
	bp->link_params.shmem_base = bp->common.shmem_base;
	BNX2X_DEV_INFO("shmem offset is 0x%x\n", bp->common.shmem_base);

	if (!bp->common.shmem_base ||
	    (bp->common.shmem_base < 0xA0000) ||
	    (bp->common.shmem_base >= 0xC0000)) {
		BNX2X_DEV_INFO("MCP not active\n");
		bp->flags |= NO_MCP_FLAG;
		return;
	}

	val = SHMEM_RD(bp, validity_map[BP_PORT(bp)]);
	if ((val & (SHR_MEM_VALIDITY_DEV_INFO | SHR_MEM_VALIDITY_MB))
		!= (SHR_MEM_VALIDITY_DEV_INFO | SHR_MEM_VALIDITY_MB))
		BNX2X_ERR("BAD MCP validity signature\n");

	if (!BP_NOMCP(bp)) {
		bp->fw_seq = (SHMEM_RD(bp, func_mb[BP_FUNC(bp)].drv_mb_header)
			      & DRV_MSG_SEQ_NUMBER_MASK);
		BNX2X_DEV_INFO("fw_seq 0x%08x\n", bp->fw_seq);
	}
}

/**
 * bnx2x_io_error_detected - called when PCI error is detected
 * @pdev: Pointer to PCI device
 * @state: The current pci connection state
 *
 * This function is called after a PCI bus error affecting
 * this device has been detected.
 */
static pci_ers_result_t bnx2x_io_error_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp = netdev_priv(dev);

	rtnl_lock();

	netif_device_detach(dev);

	if (state == pci_channel_io_perm_failure) {
		rtnl_unlock();
		return PCI_ERS_RESULT_DISCONNECT;
	}

	if (netif_running(dev))
		bnx2x_eeh_nic_unload(bp);

	pci_disable_device(pdev);

	rtnl_unlock();

	/* Request a slot reset */
	return PCI_ERS_RESULT_NEED_RESET;
}

/**
 * bnx2x_io_slot_reset - called after the PCI bus has been reset
 * @pdev: Pointer to PCI device
 *
 * Restart the card from scratch, as if from a cold-boot.
 */
static pci_ers_result_t bnx2x_io_slot_reset(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp = netdev_priv(dev);

	rtnl_lock();

	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev,
			"Cannot re-enable PCI device after reset\n");
		rtnl_unlock();
		return PCI_ERS_RESULT_DISCONNECT;
	}

	pci_set_master(pdev);
	pci_restore_state(pdev);

	if (netif_running(dev))
		bnx2x_set_power_state(bp, PCI_D0);

	rtnl_unlock();

	return PCI_ERS_RESULT_RECOVERED;
}

/**
 * bnx2x_io_resume - called when traffic can start flowing again
 * @pdev: Pointer to PCI device
 *
 * This callback is called when the error recovery driver tells us that
 * its OK to resume normal operation.
 */
static void bnx2x_io_resume(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct bnx2x *bp = netdev_priv(dev);

	rtnl_lock();

	bnx2x_eeh_recover(bp);

	if (netif_running(dev))
		bnx2x_nic_load(bp, LOAD_NORMAL);

	netif_device_attach(dev);

	rtnl_unlock();
}

static struct pci_error_handlers bnx2x_err_handler = {
	.error_detected = bnx2x_io_error_detected,
	.slot_reset     = bnx2x_io_slot_reset,
	.resume         = bnx2x_io_resume,
};
#endif

static struct pci_driver bnx2x_pci_driver = {
	.name        = DRV_MODULE_NAME,
	.id_table    = bnx2x_pci_tbl,
	.probe       = bnx2x_init_one,
	.remove      = __devexit_p(bnx2x_remove_one),
	.suspend     = bnx2x_suspend,
	.resume      = bnx2x_resume,
#if (LINUX_VERSION_CODE >= 0x02060b) && !defined(__VMKLNX__) /* BNX2X_UPSTREAM */
	.err_handler = &bnx2x_err_handler,
#endif
};

static int __init bnx2x_init(void)
{
	int ret;

	printk(KERN_INFO "%s", version);

	bnx2x_wq = create_singlethread_workqueue("bnx2x");
	if (bnx2x_wq == NULL) {
		printk(KERN_ERR PFX "Cannot create workqueue\n");
		return -ENOMEM;
	}

#if (LINUX_VERSION_CODE >= 0x020600) /* BNX2X_UPSTREAM */
	ret = pci_register_driver(&bnx2x_pci_driver);
#else
	ret = pci_module_init(&bnx2x_pci_driver);
#endif
#if (LINUX_VERSION_CODE >= 0x02060a) /* BNX2X_UPSTREAM */
	if (ret) {
		printk(KERN_ERR PFX "Cannot register driver\n");
		destroy_workqueue(bnx2x_wq);
	}
#endif
	return ret;
}

static void __exit bnx2x_cleanup(void)
{
	pci_unregister_driver(&bnx2x_pci_driver);

	destroy_workqueue(bnx2x_wq);
}

module_init(bnx2x_init);
module_exit(bnx2x_cleanup);

#ifdef BCM_CNIC

/* count denotes the number of new completions we have seen */
static void bnx2x_cnic_sp_post(struct bnx2x *bp, int count)
{
	struct eth_spe *spe;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return;
#endif

	spin_lock_bh(&bp->spq_lock);
	bp->cnic_spq_pending -= count;

	for (; bp->cnic_spq_pending < bp->cnic_eth_dev.max_kwqe_pending;
	     bp->cnic_spq_pending++) {

		if (!bp->cnic_kwq_pending)
			break;

		spe = bnx2x_sp_get_next(bp);
		*spe = *bp->cnic_kwq_cons;

		bp->cnic_kwq_pending--;

		DP(NETIF_MSG_TIMER, "pending on SPQ %d, on KWQ %d count %d\n",
		   bp->cnic_spq_pending, bp->cnic_kwq_pending, count);

		if (bp->cnic_kwq_cons == bp->cnic_kwq_last)
			bp->cnic_kwq_cons = bp->cnic_kwq;
		else
			bp->cnic_kwq_cons++;
	}
	bnx2x_sp_prod_update(bp);
	spin_unlock_bh(&bp->spq_lock);
}

static int bnx2x_cnic_sp_queue(struct net_device *dev,
			       struct kwqe_16 *kwqes[], u32 count)
{
	struct bnx2x *bp = netdev_priv(dev);
	int i;

#ifdef BNX2X_STOP_ON_ERROR
	if (unlikely(bp->panic))
		return -EIO;
#endif

	spin_lock_bh(&bp->spq_lock);

	for (i = 0; i < count; i++) {
		struct eth_spe *spe = (struct eth_spe *)kwqes[i];

		if (bp->cnic_kwq_pending == MAX_SP_DESC_CNT)
			break;

		*bp->cnic_kwq_prod = *spe;

		bp->cnic_kwq_pending++;

		DP(NETIF_MSG_TIMER, "L5 SPQE %x %x %x:%x pos %d\n",
		   spe->hdr.conn_and_cmd_data, spe->hdr.type,
		   spe->data.mac_config_addr.hi,
		   spe->data.mac_config_addr.lo,
		   bp->cnic_kwq_pending);

		if (bp->cnic_kwq_prod == bp->cnic_kwq_last)
			bp->cnic_kwq_prod = bp->cnic_kwq;
		else
			bp->cnic_kwq_prod++;
	}

	spin_unlock_bh(&bp->spq_lock);

	if (bp->cnic_spq_pending < bp->cnic_eth_dev.max_kwqe_pending)
		bnx2x_cnic_sp_post(bp, 0);

	return i;
}

static int bnx2x_cnic_ctl_send(struct bnx2x *bp, struct cnic_ctl_info *ctl)
{
	struct cnic_ops *c_ops;
	int rc = 0;

	mutex_lock(&bp->cnic_mutex);
	c_ops = bp->cnic_ops;
	if (c_ops)
		rc = c_ops->cnic_ctl(bp->cnic_data, ctl);
	mutex_unlock(&bp->cnic_mutex);

	return rc;
}

static int bnx2x_cnic_ctl_send_bh(struct bnx2x *bp, struct cnic_ctl_info *ctl)
{
	struct cnic_ops *c_ops;
	int rc = 0;

	rcu_read_lock();
	c_ops = rcu_dereference(bp->cnic_ops);
	if (c_ops)
		rc = c_ops->cnic_ctl(bp->cnic_data, ctl);
	rcu_read_unlock();

	return rc;
}

/*
 * for commands that have no data
 */
static int bnx2x_cnic_notify(struct bnx2x *bp, int cmd)
{
	struct cnic_ctl_info ctl = {0};

	ctl.cmd = cmd;

	return bnx2x_cnic_ctl_send(bp, &ctl);
}

static void bnx2x_cnic_cfc_comp(struct bnx2x *bp, int cid)
{
	struct cnic_ctl_info ctl;

	/* first we tell CNIC and only then we count this as a completion */
	ctl.cmd = CNIC_CTL_COMPLETION_CMD;
	ctl.data.comp.cid = cid;

	bnx2x_cnic_ctl_send_bh(bp, &ctl);
	bnx2x_cnic_sp_post(bp, 1);
}

static int bnx2x_drv_ctl(struct net_device *dev, struct drv_ctl_info *ctl)
{
	struct bnx2x *bp = netdev_priv(dev);
	int rc = 0;

	switch (ctl->cmd) {
	case DRV_CTL_CTXTBL_WR_CMD: {
		u32 index = ctl->data.io.offset;
		dma_addr_t addr = ctl->data.io.dma_addr;

		bnx2x_ilt_wr(bp, index, addr);
		break;
	}

	case DRV_CTL_COMPLETION_CMD: {
		int count = ctl->data.comp.comp_count;

		bnx2x_cnic_sp_post(bp, count);
		break;
	}

	/* rtnl_lock is held.  */
	case DRV_CTL_START_L2_CMD: {
		u32 cli = ctl->data.ring.client_id;

		bp->rx_mode_cl_mask |= (1 << cli);
		bnx2x_set_storm_rx_mode(bp);
		break;
	}

	/* rtnl_lock is held.  */
	case DRV_CTL_STOP_L2_CMD: {
		u32 cli = ctl->data.ring.client_id;

		bp->rx_mode_cl_mask &= ~(1 << cli);
		bnx2x_set_storm_rx_mode(bp);
		break;
	}

	default:
		BNX2X_ERR("unknown command %x\n", ctl->cmd);
		rc = -EINVAL;
	}

	return rc;
}

static void bnx2x_setup_cnic_irq_info(struct bnx2x *bp)
{
	struct cnic_eth_dev *cp = &bp->cnic_eth_dev;

#ifdef CONFIG_PCI_MSI /* BNX2X_UPSTREAM */
	if (bp->flags & USING_MSIX_FLAG) {
		cp->drv_state |= CNIC_DRV_STATE_USING_MSIX;
		cp->irq_arr[0].irq_flags |= CNIC_IRQ_FL_MSIX;
		cp->irq_arr[0].vector = bp->msix_table[1].vector;
	} else {
#else
	{
#endif
		cp->drv_state &= ~CNIC_DRV_STATE_USING_MSIX;
		cp->irq_arr[0].irq_flags &= ~CNIC_IRQ_FL_MSIX;
	}
	cp->irq_arr[0].status_blk = bp->cnic_sb;
	cp->irq_arr[0].status_blk_num = CNIC_SB_ID(bp);
	cp->irq_arr[1].status_blk = bp->def_status_blk;
	cp->irq_arr[1].status_blk_num = DEF_SB_ID;

	cp->num_irq = 2;
}

static int bnx2x_register_cnic(struct net_device *dev, struct cnic_ops *ops,
			       void *data)
{
	struct bnx2x *bp = netdev_priv(dev);
	struct cnic_eth_dev *cp = &bp->cnic_eth_dev;

	if (ops == NULL)
		return -EINVAL;

	if (atomic_read(&bp->intr_sem) != 0)
		return -EBUSY;

	bp->cnic_kwq = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bp->cnic_kwq)
		return -ENOMEM;

	bp->cnic_kwq_cons = bp->cnic_kwq;
	bp->cnic_kwq_prod = bp->cnic_kwq;
	bp->cnic_kwq_last = bp->cnic_kwq + MAX_SP_DESC_CNT;

	bp->cnic_spq_pending = 0;
	bp->cnic_kwq_pending = 0;

	bp->cnic_data = data;

	cp->num_irq = 0;
	cp->drv_state = CNIC_DRV_STATE_REGD;

	bnx2x_init_sb(bp, bp->cnic_sb, bp->cnic_sb_mapping, CNIC_SB_ID(bp));

	bnx2x_setup_cnic_irq_info(bp);
	bnx2x_set_iscsi_eth_mac_addr(bp, 1);
	bp->cnic_flags |= BNX2X_CNIC_FLAG_MAC_SET;
	rcu_assign_pointer(bp->cnic_ops, ops);

	return 0;
}

static int bnx2x_unregister_cnic(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	struct cnic_eth_dev *cp = &bp->cnic_eth_dev;

	mutex_lock(&bp->cnic_mutex);
	if (bp->cnic_flags & BNX2X_CNIC_FLAG_MAC_SET) {
		bp->cnic_flags &= ~BNX2X_CNIC_FLAG_MAC_SET;
		bnx2x_set_iscsi_eth_mac_addr(bp, 0);
	}
	cp->drv_state = 0;
	rcu_assign_pointer(bp->cnic_ops, NULL);
	mutex_unlock(&bp->cnic_mutex);
	synchronize_rcu();
	kfree(bp->cnic_kwq);
	bp->cnic_kwq = NULL;

	return 0;
}

struct cnic_eth_dev *bnx2x_cnic_probe(struct net_device *dev)
{
	struct bnx2x *bp = netdev_priv(dev);
	struct cnic_eth_dev *cp = &bp->cnic_eth_dev;

	cp->drv_owner = THIS_MODULE;
	cp->chip_id = CHIP_ID(bp);
	cp->pdev = bp->pdev;
	cp->io_base = bp->regview;
#if defined(RHEL_RELEASE_CODE) /* BNX2X_UPSTREAM */
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(5,5) /* BNX2X_UPSTREAM */
 	cp->io_base2 = bp->doorbells;
#endif
#endif
	cp->max_kwqe_pending = 8;
	cp->ctx_blk_size = CNIC_CTX_PER_ILT * sizeof(union cdu_context);
	cp->ctx_tbl_offset = FUNC_ILT_BASE(BP_FUNC(bp)) + 1;
	cp->ctx_tbl_len = CNIC_ILT_LINES;
	cp->starting_cid = BCM_CNIC_CID_START;
	cp->drv_submit_kwqes_16 = bnx2x_cnic_sp_queue;
	cp->drv_ctl = bnx2x_drv_ctl;
	cp->drv_register_cnic = bnx2x_register_cnic;
	cp->drv_unregister_cnic = bnx2x_unregister_cnic;

	return cp;
}
EXPORT_SYMBOL(bnx2x_cnic_probe);

#endif /* BCM_CNIC */
