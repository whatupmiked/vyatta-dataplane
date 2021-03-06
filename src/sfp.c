/*
 * This module is derived from freebsd/sys/net/sfp.c
 * The changes made are
 *   - removal of i2c functions since the EEPROM info is passed in
 *     as a single buffer
 *   - conversion of printf to json_xxx APIs
 *   - checkpatch fixup
 */
/*-
 * Copyright (c) 2018-2019, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2014 Alexander V. Chernikov. All rights reserved.
 *
 * SPDX-License-Identifier: (LGPL-2.1-only AND BSD-2-Clause-FREEBSD)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "sff8436.h"
#include "sff8472.h"

#include <math.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json_writer.h>
#include <rte_dev_info.h>
#include <transceiver.h>

struct _nv {
	int v;
	const char *n;
};

/* Offset in the EEPROM data for all base queries. */
#define	SFF_8472_BASE_OFFSET 0
#define	SFF_8436_BASE_OFFSET 0

/* Offset in the EEPROM data for all diag data */
#define	SFF_8472_DIAG_OFFSET 256

static const char *find_value(struct _nv *x, int value);
static const char *find_zero_bit(struct _nv *x, int value, int sz);

/* SFF-8024 Rev. 4.1 Table 4-3: Connector Types */
static struct _nv conn[] = {
	{ 0x00, "Unknown" },
	{ 0x01, "SC" },
	{ 0x02, "Fibre Channel Style 1 copper" },
	{ 0x03, "Fibre Channel Style 2 copper" },
	{ 0x04, "BNC/TNC" },
	{ 0x05, "Fibre Channel coaxial" },
	{ 0x06, "FiberJack" },
	{ 0x07, "LC" },
	{ 0x08, "MT-RJ" },
	{ 0x09, "MU" },
	{ 0x0A, "SG" },
	{ 0x0B, "Optical pigtail" },
	{ 0x0C, "MPO Parallel Optic" },
	{ 0x20, "HSSDC II" },
	{ 0x21, "Copper pigtail" },
	{ 0x22, "RJ45" },
	{ 0x23, "No separable connector" },
	{ 0x24, "MXC 2x16" },
	{ 0, NULL }
};

/* SFF-8472 Rev. 11.4 table 3.5: Transceiver codes */
/* 10G Ethernet/IB compliance codes, byte 3 */
static struct _nv eth_10g[] = {
	{ 0x80, "10G Base-ER" },
	{ 0x40, "10G Base-LRM" },
	{ 0x20, "10G Base-LR" },
	{ 0x10, "10G Base-SR" },
	{ 0x08, "1X SX" },
	{ 0x04, "1X LX" },
	{ 0x02, "1X Copper Active" },
	{ 0x01, "1X Copper Passive" },
	{ 0, NULL }
};

/* Ethernet compliance codes, byte 6 */
static struct _nv eth_compat[] = {
	{ 0x80, "BASE-PX" },
	{ 0x40, "BASE-BX10" },
	{ 0x20, "100BASE-FX" },
	{ 0x10, "100BASE-LX/LX10" },
	{ 0x08, "1000BASE-T" },
	{ 0x04, "1000BASE-CX" },
	{ 0x02, "1000BASE-LX" },
	{ 0x01, "1000BASE-SX" },
	{ 0, NULL }
};

/* FC link length, byte 7 */
static struct _nv fc_len[] = {
	{ 0x80, "very long distance" },
	{ 0x40, "short distance" },
	{ 0x20, "intermediate distance" },
	{ 0x10, "long distance" },
	{ 0x08, "medium distance" },
	{ 0, NULL }
};

/* Channel/Cable technology, byte 7-8 */
static struct _nv cab_tech[] = {
	{ 0x0400, "Shortwave laser (SA)" },
	{ 0x0200, "Longwave laser (LC)" },
	{ 0x0100, "Electrical inter-enclosure (EL)" },
	{ 0x80, "Electrical intra-enclosure (EL)" },
	{ 0x40, "Shortwave laser (SN)" },
	{ 0x20, "Shortwave laser (SL)" },
	{ 0x10, "Longwave laser (LL)" },
	{ 0x08, "Active Cable" },
	{ 0x04, "Passive Cable" },
	{ 0, NULL }
};

/* FC Transmission media, byte 9 */
static struct _nv fc_media[] = {
	{ 0x80, "Twin Axial Pair" },
	{ 0x40, "Twisted Pair" },
	{ 0x20, "Miniature Coax" },
	{ 0x10, "Viao Coax" },
	{ 0x08, "Miltimode, 62.5um" },
	{ 0x04, "Multimode, 50um" },
	{ 0x02, "" },
	{ 0x01, "Single Mode" },
	{ 0, NULL }
};

/* FC Speed, byte 10 */
static struct _nv fc_speed[] = {
	{ 0x80, "1200 MBytes/sec" },
	{ 0x40, "800 MBytes/sec" },
	{ 0x20, "1600 MBytes/sec" },
	{ 0x10, "400 MBytes/sec" },
	{ 0x08, "3200 MBytes/sec" },
	{ 0x04, "200 MBytes/sec" },
	{ 0x01, "100 MBytes/sec" },
	{ 0, NULL }
};

/* SFF-8436 Rev. 4.8 table 33: Specification compliance  */

/* 10/40G Ethernet compliance codes, byte 128 + 3 */
static struct _nv eth_1040g[] = {
	{ 0x80, "Extended" },
	{ 0x40, "10GBASE-LRM" },
	{ 0x20, "10GBASE-LR" },
	{ 0x10, "10GBASE-SR" },
	{ 0x08, "40GBASE-CR4" },
	{ 0x04, "40GBASE-SR4" },
	{ 0x02, "40GBASE-LR4" },
	{ 0x01, "40G Active Cable" },
	{ 0, NULL }
};
#define	SFF_8636_EXT_COMPLIANCE	0x80

/* SFF-8024 Rev. 4.2 table 4-4: Extended Specification Compliance */
static struct _nv eth_extended_comp[] = {
	{ 0xFF, "Reserved" },
	{ 0x21, "100G PAM4 BiDi" },
	{ 0x20, "100G SWDM4" },
	{ 0x1F, "40G SWDM4" },
	{ 0x1E, "2.5GBASE-T" },
	{ 0x1D, "5GBASE-T" },
	{ 0x1C, "10GBASE-T Short Reach" },
	{ 0x1B, "100G 1550nm WDM" },
	{ 0x1A, "100GE-DWDM2" },
	{ 0x19, "100G ACC or 25GAUI C2M ACC" },
	{ 0x18, "100G AOC or 25GAUI C2M AOC" },
	{ 0x17, "100G CLR4" },
	{ 0x16, "10GBASE-T with SFI electrical interface" },
	{ 0x15, "G959.1 profile P1L1-2D2" },
	{ 0x14, "G959.1 profile P1S1-2D2" },
	{ 0x13, "G959.1 profile P1I1-2D1" },
	{ 0x12, "40G PSM4 Parallel SMF" },
	{ 0x11, "4 x 10GBASE-SR" },
	{ 0x10, "40GBASE-ER4" },
	{ 0x0F, "Reserved" },
	{ 0x0E, "Reserved" },
	{ 0x0D, "25GBASE-CR CA-N" },
	{ 0x0C, "25GBASE-CR CA-S" },
	{ 0x0B, "100GBASE-CR4 or 25GBASE-CR CA-L" },
	{ 0x0A, "Reserved" },
	{ 0x09, "Obsolete" },
	{ 0x08, "100G ACC (Active Copper Cable) or 25GAUI C2M ACC" },
	{ 0x07, "100G PSM4 Parallel SMF" },
	{ 0x06, "100G CWDM4" },
	{ 0x05, "100GBASE-SR10" },
	{ 0x04, "100GBASE-ER4 or 25GBASE-ER" },
	{ 0x03, "100GBASE-LR4 or 25GBASE-LR" },
	{ 0x02, "100GBASE-SR4 or 25GBASE-SR" },
	{ 0x01, "100G AOC (Active Optical Cable) or 25GAUI C2M AOC" },
	{ 0x00, "Unspecified" }
};

/* SFF-8636 Rev. 2.9 table 6.3: Revision compliance */
static struct _nv rev_compl[] = {
	{ 0x1, "SFF-8436 rev <=4.8" },
	{ 0x2, "SFF-8436 rev <=4.8" },
	{ 0x3, "SFF-8636 rev <=1.3" },
	{ 0x4, "SFF-8636 rev <=1.4" },
	{ 0x5, "SFF-8636 rev <=1.5" },
	{ 0x6, "SFF-8636 rev <=2.0" },
	{ 0x7, "SFF-8636 rev <=2.7" },
	{ 0x8, "SFF-8636 rev >=2.8" },
	{ 0x0, "Unspecified" }
};

/* SFF-8472 table 3.6: Encoding codes */
static struct _nv encoding[] = {
	{ 0x1, "8B/10B" },
	{ 0x2, "4B/5B" },
	{ 0x3, "NRZ" },
	{ 0x4, "Manchester" },
	{ 0x5, "SONET scrambled" },
	{ 0x6, "64B/66B" },
	{ 0x0, "Unspecified" }
};

/* SFF-8636 Rev. 2.9 points to SFF-8024 table 4.2: Encoding values */
static struct _nv qsfp_encoding[] = {
	{ 0x1, "8B/10B" },
	{ 0x2, "4B/5B" },
	{ 0x3, "NRZ" },
	{ 0x4, "SONET scrambled" },
	{ 0x5, "64B/66B" },
	{ 0x6, "Manchester" },
	{ 0x7, "256B/257B" },
	{ 0x8, "PAM4" },
	{ 0x0, "Unspecified" }
};


/* SFF-8472 table 3.12: Compliane */
static struct _nv sff_8472_compl[] = {
	{ 0x1, "SFF_8472 rev 9.3" },
	{ 0x2, "SFF_8472 rev 9.5" },
	{ 0x3, "SFF_8472 rev 10.2" },
	{ 0x4, "SFF_8472 rev 10.4" },
	{ 0x5, "SFF_8472 rev 11.0" },
	{ 0x0, "Undefined" }
};

/* SFF-8472 table 3.3: Extended Identifier values */
static struct _nv ext_id[] = {
	{ 0x1, "MOD_DEF 1" },
	{ 0x2, "MOD_DEF 2" },
	{ 0x3, "MOD_DEF 3" },
	{ 0x4, "" },
	{ 0x5, "MOD_DEF 5" },
	{ 0x6, "MOD_DEF 6" },
	{ 0x7, "MOD_DEF 7" },
	{ 0x0, "Undefined" }
};

static struct _nv ext_8436_id[] = {
	{ 0x1, "Power Class 2(2.0 W max)" },
	{ 0x2, "Power Class 3(2.5 W max)" },
	{ 0x3, "Power Class 4(3.5 W max)" },
	{ 0x0, "Power Class 1(1.5 W max)" }
};

static struct _nv alarm_flags[] = {
	{ 0x0f, "temp_high_alarm" },
	{ 0x0e, "temp_low_alarm" },
	{ 0x0d, "vcc_high_alarm" },
	{ 0x0c, "vcc_low_alarm" },
	{ 0x0b, "tx_bias_high_alarm" },
	{ 0x0a, "tx_bias_low_alarm" },
	{ 0x09, "tx_power_high_alarm" },
	{ 0x08, "tx_power_low_alarm" },
	{ 0x07, "rx_power_high_alarm" },
	{ 0x06, "rx_power_low_alarm" },
	{ 0x00,  NULL }
};

static struct _nv warning_flags[] = {
	{ 0x0f, "temp_high_warn" },
	{ 0x0e, "temp_low_warn" },
	{ 0x0d, "vcc_high_warn" },
	{ 0x0c, "vcc_low_warn" },
	{ 0x0b, "tx_bias_high_warn" },
	{ 0x0a, "tx_bias_low_warn" },
	{ 0x09, "tx_power_high_warn" },
	{ 0x08, "tx_power_low_warn" },
	{ 0x07, "rx_power_high_warn" },
	{ 0x06, "rx_power_low_warn" },
	{ 0x00,  NULL }
};


static struct _nv rx_pwr_aw_chan_upper_flags[] = {
	{ 0x4, "rx_power_low_warn"  },
	{ 0x5, "rx_power_high_warn" },
	{ 0x6, "rx_power_low_alarm" },
	{ 0x7, "rx_power_high_alarm" },
	{ 0x8, NULL  }
};

static struct _nv rx_pwr_aw_chan_lower_flags[] = {
	{ 0x0, "rx_power_low_warn"  },
	{ 0x1, "rx_power_high_warn" },
	{ 0x2, "rx_power_low_alarm" },
	{ 0x3, "rx_power_high_alarm" },
	{ 0x8, NULL  }
};

static struct _nv tx_pwr_aw_chan_upper_flags[] = {
	{ 0x4, "tx_power_low_warn"  },
	{ 0x5, "tx_power_high_warn" },
	{ 0x6, "tx_power_low_alarm" },
	{ 0x7, "tx_power_high_alarm" },
	{ 0x8, NULL  }
};

static struct _nv tx_pwr_aw_chan_lower_flags[] = {
	{ 0x0, "tx_power_low_warn"  },
	{ 0x1, "tx_power_high_warn" },
	{ 0x2, "tx_power_low_alarm" },
	{ 0x3, "tx_power_high_alarm" },
	{ 0x8, NULL  }
};


static struct _nv tx_bias_aw_chan_upper_flags[] = {
	{ 0x4, "tx_bias_low_warn"  },
	{ 0x5, "tx_bias_high_warn" },
	{ 0x6, "tx_bias_low_alarm" },
	{ 0x7, "tx_bias_high_alarm" },
	{ 0x8, NULL  }
};

static struct _nv tx_bias_aw_chan_lower_flags[] = {
	{ 0x0, "tx_bias_low_warn"  },
	{ 0x1, "tx_bias_high_warn" },
	{ 0x2, "tx_bias_low_alarm" },
	{ 0x3, "tx_bias_high_alarm" },
	{ 0x8, NULL  }
};


static struct _nv temp_alarm_warn_flags[] = {
	{ 0x7, "temp_high_alarm" },
	{ 0x6, "temp_low_alarm" },
	{ 0x5, "temp_high_warn" },
	{ 0x4, "temp_low_warn"  },
	{ 0x00,  NULL }
};

static struct _nv voltage_alarm_warn_flags[] = {
	{ 0x7, "vcc_high_alarm" },
	{ 0x6, "vcc_low_alarm" },
	{ 0x5, "vcc_high_warn" },
	{ 0x4, "vcc_low_warn"  },
	{ 0x00,  NULL }
};

/*
 * Retrieves a section of eeprom data for parsing & display
 */
static int
get_eeprom_data(const struct rte_dev_eeprom_info *eeprom_info,
		uint8_t base, uint32_t off, uint8_t len, uint8_t *buf)
{
	switch (base) {
	case SFF_8472_BASE:
		off += SFF_8472_BASE_OFFSET;
		if ((off >= eeprom_info->length) ||
		    (len >= eeprom_info->length - SFF_8472_BASE_OFFSET))
			return -EINVAL;


		memcpy(buf, &((const char *)eeprom_info->data)[off], len);
		return 0;

	case SFF_8472_DIAG:
		off += SFF_8472_DIAG_OFFSET;
		if ((off >= eeprom_info->length) ||
		    (len >= eeprom_info->length - SFF_8472_DIAG_OFFSET))
			return -EINVAL;


		memcpy(buf, &((const char *)eeprom_info->data)[off], len);
		return 0;

	case SFF_8436_BASE:
		off += SFF_8436_BASE_OFFSET;
		if ((off >= eeprom_info->length) ||
		    (len >= eeprom_info->length - SFF_8436_BASE_OFFSET))
			return -EINVAL;


		memcpy(buf, &((const char *)eeprom_info->data)[off], len);
		return 0;


	default:
		return -EINVAL;
	}
}

/*
 * Temporarily unused. Will be used with a separate vplsh command
 * to dump raw EEPROM data
 */
#if 0
static void
dump_eeprom_data(const struct rte_dev_eeprom_info *eeprom_info,
		 uint8_t base, uint8_t off, uint8_t len)
{
	int last;

	switch (base) {
	case SFF_8436_BASE:
		if ((off >= eeprom_info->length) ||
		    (len >= eeprom_info->length))
			return;

		printf("\t");
		last = off + len;
		for (int i = off; i < last; i++)
			printf("%hhx ", ((const char *)&eeprom_info->data)[i]);
		printf("\n");
	}
}
#endif

static const char *
find_value(struct _nv *x, int value)
{
	for (; x->n != NULL; x++)
		if (x->v == value)
			return x->n;
	return NULL;
}

static const char *
find_zero_bit(struct _nv *x, int value, int sz)
{
	int v, m;
	const char *s;

	for (v = 1, m = 1 << (8 * sz); v < m; v *= 2) {
		if ((value & v) == 0)
			continue;
		s = find_value(x, value & v);
		if (s != NULL)
			return s;
	}

	return NULL;
}

static void
convert_sff_identifier(json_writer_t *wr, uint8_t value)
{
	const char *x;

	x = NULL;
	if (value <= SFF_8024_ID_LAST)
		x = sff_8024_id[value];
	else {
		if (value > 0x80)
			x = "Vendor specific";
		else
			x = "Reserved";
	}

	jsonw_string_field(wr, "identifier", x);
}

static void
convert_sff_ext_identifier(json_writer_t *wr, uint8_t value)
{
	const char *x;

	if (value > 0x07)
		x = "Unallocated";
	else
		x = find_value(ext_id, value);
	jsonw_string_field(wr, "ext_identifier", x);
}

static void
convert_sff_8436_ext_identifier(json_writer_t *wr, uint8_t value)
{
	const char *x;

	x = find_value(ext_8436_id, (value & 0xc0) >> 6);
	jsonw_string_field(wr, "ext_identifier", x);
}

static void
convert_sff_connector(json_writer_t *wr, uint8_t value)
{
	const char *x;

	x = find_value(conn, value);
	if (x == NULL) {
		if (value >= 0x0D && value <= 0x1F)
			x = "Unallocated";
		else if (value >= 0x24 && value <= 0x7F)
			x = "Unallocated";
		else
			x = "Vendor specific";
	}

	jsonw_string_field(wr, "connector", x);
}

static void
convert_sff_8436_rev_compliance(json_writer_t *wr, uint8_t value)
{
	const char *x;

	if (value > 0x08)
		x = "Unallocated";
	else
		x = find_value(rev_compl, value);

	jsonw_string_field(wr, "8472_compl", x);
}

static void
convert_sff_encoding(json_writer_t *wr, uint8_t value)
{
	const char *x;

	if (value > 0x06)
		x = "Unallocated";
	else
		x = find_value(encoding, value);

	jsonw_string_field(wr, "encoding", x);
}

static void
convert_sff_8436_encoding(json_writer_t *wr, uint8_t value)
{
	const char *x;

	if (value > 0x06)
		x = "Unallocated";
	else
		x = find_value(qsfp_encoding, value);

	jsonw_string_field(wr, "encoding", x);
}

static void
convert_sff_8472_compl(json_writer_t *wr, uint8_t value)
{
	const char *x;

	if (value > 0x05)
		x = "Unallocated";
	else
		x = find_value(sff_8472_compl, value);

	jsonw_string_field(wr, "8472_compl", x);
}

static void
print_sfp_identifier(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_ID, 1, &data))
		return;

	convert_sff_identifier(wr, data);
}

static void
print_sfp_ext_identifier(const struct rte_dev_eeprom_info *eeprom_info,
			 json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_EXT_ID, 1,
			    &data))
		return;

	convert_sff_ext_identifier(wr, data);
}

static void
print_sfp_connector(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_CONNECTOR, 1,
			    &data))
		return;

	convert_sff_connector(wr, data);
}

static void
print_qsfp_identifier(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_ID, 1, &data))
		return;

	convert_sff_identifier(wr, data);
}

static void
print_qsfp_ext_identifier(const struct rte_dev_eeprom_info *eeprom_info,
			 json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_EXT_ID, 1,
			    &data))
		return;

	convert_sff_8436_ext_identifier(wr, data);
}

static void
print_qsfp_connector(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	uint8_t data;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_CONNECTOR, 1,
			    &data))
		return;

	convert_sff_connector(wr, data);
}

static void
print_sfp_transceiver_descr(const struct rte_dev_eeprom_info *eeprom_info,
			    json_writer_t *wr)
{
	uint8_t xbuf[12];
	const char *tech_class, *tech_len, *tech_tech, *tech_media, *tech_speed;

	tech_class = NULL;
	tech_len = NULL;
	tech_tech = NULL;
	tech_media = NULL;
	tech_speed = NULL;

	/* Read bytes 3-10 at once */
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_TRANS_START, 8,
			    &xbuf[3]))
		return;

	/* Check 10G ethernet first */
	tech_class = find_zero_bit(eth_10g, xbuf[3], 1);
	if (tech_class == NULL) {
		/* No match. Try 1G */
		tech_class = find_zero_bit(eth_compat, xbuf[6], 1);
	}

	tech_len = find_zero_bit(fc_len, xbuf[7], 1);
	tech_tech = find_zero_bit(cab_tech, xbuf[7] << 8 | xbuf[8], 2);
	tech_media = find_zero_bit(fc_media, xbuf[9], 1);
	tech_speed = find_zero_bit(fc_speed, xbuf[10], 1);

	/* transceiver compliance codes - bytes 3-10 */
	if (tech_class)
		jsonw_string_field(wr, "class", tech_class);
	if (tech_len)
		jsonw_string_field(wr, "length", tech_len);
	if (tech_tech)
		jsonw_string_field(wr, "tech", tech_tech);
	if (tech_media)
		jsonw_string_field(wr, "media", tech_media);
	if (tech_speed)
		jsonw_string_field(wr, "speed", tech_speed);
}

static void
print_sfp_transceiver_class(const struct rte_dev_eeprom_info *eeprom_info,
			    json_writer_t *wr)
{
	const char *tech_class;
	uint8_t code;

	/* Use extended compliance code if it's valid */
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_TRANS, 1,
			    &code))
		return;

	if (code != 0)
		tech_class = find_value(eth_extended_comp, code);
	else {
		/* Next, check 10G Ethernet/IB CCs */
		get_eeprom_data(eeprom_info, SFF_8472_BASE,
				SFF_8472_TRANS_START, 1, &code);
		tech_class = find_zero_bit(eth_10g, code, 1);
		if (tech_class == NULL) {
			/* No match. Try Ethernet 1G */
			get_eeprom_data(eeprom_info, SFF_8472_BASE,
					SFF_8472_TRANS_START + 3,
					1, &code);
			tech_class = find_zero_bit(eth_compat, code, 1);
		}
	}

	if (tech_class == NULL)
		tech_class = "Unknown";

	/* extended compliance code - byte 36 */
	jsonw_string_field(wr, "xcvr_class", tech_class);
}

static void
print_qsfp_transceiver_class(const struct rte_dev_eeprom_info *eeprom_info,
			     json_writer_t *wr)
{
	const char *tech_class;
	uint8_t code;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
			    SFF_8436_CODE_E1040100G, 1, &code))
		return;

	/* Check for extended specification compliance */
	if (code & SFF_8636_EXT_COMPLIANCE) {
		get_eeprom_data(eeprom_info, SFF_8436_BASE,
				SFF_8436_OPTIONS_START, 1, &code);
		tech_class = find_value(eth_extended_comp, code);
	} else
		/* Check 10/40G Ethernet class only */
		tech_class = find_zero_bit(eth_1040g, code, 1);

	if (tech_class == NULL)
		tech_class = "Unknown";

	jsonw_string_field(wr, "xcvr_class", tech_class);
}

static bool
is_valid_char(const char c)
{
	return !((c < 0x20) || (c > 0x7e));
}

/*
 * Print SFF-8472/SFF-8436 string to supplied buffer.
 * All (vendor-specific) strings are padded right with '0x20'.
 */
static void
convert_sff_name(json_writer_t *wr, const char *field_name, char *xbuf,
		 uint8_t len)
{
	int i;
	char *p = &xbuf[0];

	for (i = 0; i < len; i++) {
		if (!is_valid_char(*p)) {
			jsonw_string_field(wr, field_name, "");
			return;
		}
		if (*p == 0x20)
			break;
		p++;
	}
	*p = '\0';
	jsonw_string_field(wr, field_name, xbuf);
}

static void
convert_sff_vendor_oui(json_writer_t *wr, char *xbuf)
{
	char buf[9];

	snprintf(buf, sizeof(buf),
		 "%02hhx.%02hhx.%02hhx", xbuf[0], xbuf[1], xbuf[2]);
	jsonw_string_field(wr, "vendor_oui", buf);
}

static void
convert_sff_date(json_writer_t *wr, char *xbuf)
{
	char buf[20];
	int i;

	for (i = 0; i < 6; i++) {
		if (!is_valid_char(xbuf[i]))
			return;
	}
	snprintf(buf, 20, "20%c%c-%c%c-%c%c", xbuf[0], xbuf[1],
		 xbuf[2], xbuf[3], xbuf[4], xbuf[5]);
	buf[10] = '\0';
	jsonw_string_field(wr, "date", buf);
}

static void
print_sfp_vendor_name(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_VENDOR_START,
			    16, (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_name", xbuf, 16);
}

static void
print_sfp_vendor_pn(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_PN_START, 16,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_pn", xbuf, 16);
}

static void
print_sfp_vendor_oui(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	char xbuf[4];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE,
			    SFF_8472_VENDOR_OUI_START, 3, (uint8_t *)xbuf))
		return;

	convert_sff_vendor_oui(wr, xbuf);
}

static void
print_sfp_vendor_sn(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_SN_START, 16,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_sn", xbuf, 16);
}

static void
print_sfp_vendor_rev(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr)
{
	char xbuf[5];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_REV_START, 4,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_rev", xbuf, 4);
}

static void
print_sfp_vendor_date(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	char xbuf[7];

	memset(xbuf, 0, sizeof(xbuf));
	/* Date code, see Table 3.8 for description */
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_DATE_START, 6,
			    (uint8_t *)xbuf))
		return;

	convert_sff_date(wr, xbuf);
}

static void
print_qsfp_vendor_name(const struct rte_dev_eeprom_info *eeprom_info,
		       json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_VENDOR_START,
			    16, (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_name", xbuf, 16);
}

static void
print_qsfp_vendor_pn(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_PN_START, 16,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_pn", xbuf, 16);
}

static void
print_qsfp_vendor_oui(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	char xbuf[4];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
			    SFF_8436_VENDOR_OUI_START, 3, (uint8_t *)xbuf))
		return;

	convert_sff_vendor_oui(wr, xbuf);
}

static void
print_qsfp_vendor_sn(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	char xbuf[17];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_SN_START, 16,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_sn", xbuf, 16);
}

static void
print_qsfp_vendor_rev(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr)
{
	char xbuf[5];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_REV_START, 2,
			    (uint8_t *)xbuf))
		return;

	convert_sff_name(wr, "vendor_rev", xbuf, 2);
}

static void
print_qsfp_vendor_date(const struct rte_dev_eeprom_info *eeprom_info,
		       json_writer_t *wr)
{
	char xbuf[6];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_DATE_START, 6,
			    (uint8_t *)xbuf))
		return;

	convert_sff_date(wr, xbuf);
}

static void
print_sfp_vendor(const struct rte_eth_dev_module_info *module_info,
		 const struct rte_dev_eeprom_info *eeprom_info,
		 json_writer_t *wr)
{
	if (module_info->type == RTE_ETH_MODULE_SFF_8436) {
		print_qsfp_vendor_name(eeprom_info, wr);
		print_qsfp_vendor_pn(eeprom_info, wr);
		print_qsfp_vendor_sn(eeprom_info, wr);
		print_qsfp_vendor_date(eeprom_info, wr);
	} else if (module_info->type == RTE_ETH_MODULE_SFF_8472 ||
		   module_info->type == RTE_ETH_MODULE_SFF_8079) {
		print_sfp_vendor_name(eeprom_info, wr);
		print_sfp_vendor_pn(eeprom_info, wr);
		print_sfp_vendor_oui(eeprom_info, wr);
		print_sfp_vendor_sn(eeprom_info, wr);
		print_sfp_vendor_rev(eeprom_info, wr);
		print_sfp_vendor_date(eeprom_info, wr);
	}
}

static void
print_qsfp_vendor(const struct rte_dev_eeprom_info *eeprom_info,
		 json_writer_t *wr)
{
	print_qsfp_vendor_name(eeprom_info, wr);
	print_qsfp_vendor_pn(eeprom_info, wr);
	print_qsfp_vendor_oui(eeprom_info, wr);
	print_qsfp_vendor_sn(eeprom_info, wr);
	print_qsfp_vendor_rev(eeprom_info, wr);
	print_qsfp_vendor_date(eeprom_info, wr);
}

/*
 * Converts internal templerature (SFF-8472, SFF-8436)
 * 16-bit unsigned value to human-readable representation:
 *
 * Internally measured Module temperature are represented
 * as a 16-bit signed twos complement value in increments of
 * 1/256 degrees Celsius, yielding a total range of –128C to +128C
 * that is considered valid between –40 and +125C.
 *
 */
static void
convert_sff_temp(json_writer_t *wr, const char *field_name,
		 uint8_t *xbuf)
{
	int16_t temp;
	double d;

	temp = (xbuf[0] > 0x7f) ? xbuf[0] - (0xff + 1) : xbuf[0];

	d = (double)temp + (double)xbuf[1] / 256;

	jsonw_float_field(wr, field_name, d);
}

/*
 * Retrieves supplied voltage (SFF-8472, SFF-8436).
 * 16-bit usigned value, treated as range 0..+6.55 Volts
 */
static void
convert_sff_voltage(json_writer_t *wr, const char *field_name,
		    uint8_t *xbuf)
{
	double d;

	d = (double)((xbuf[0] << 8) | xbuf[1]);
	jsonw_float_field(wr, field_name, d / 10000);
}

/*
 * Retrieves power in mW (SFF-8472).
 * 16-bit unsigned value, treated as a range of 0 - 6.5535 mW
 */
static void
convert_sff_power(json_writer_t *wr, const char *field_name,
		  uint8_t *xbuf)
{
	double mW;

	mW = (xbuf[0] << 8) + xbuf[1];

	jsonw_float_field(wr, field_name, mW / 10000);
}

static void
convert_sff_bias(json_writer_t *wr, const char *field_name,
		 uint8_t *xbuf)
{
	double mA;

	mA = (xbuf[0] << 8) + xbuf[1];

	jsonw_float_field(wr, field_name, mA / 500);
}

static void
print_sfp_temp(const struct rte_dev_eeprom_info *eeprom_info,
	       json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_TEMP, 2, xbuf);
	convert_sff_temp(wr, "temperature_C", xbuf);
}

static void
print_sfp_voltage(const struct rte_dev_eeprom_info *eeprom_info,
		  json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_VCC, 2, xbuf);
	convert_sff_voltage(wr, "voltage_V", xbuf);
}

static void
print_sfp_br(const struct rte_dev_eeprom_info *eeprom_info,
	      json_writer_t *wr)
{
	uint8_t xbuf;
	uint32_t rate;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_BITRATE, 1, &xbuf);
	rate = xbuf * 100;
	if (xbuf == 0xFF) {
		get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_BITRATE,
				1, &xbuf);
		rate = xbuf * 250;
	}

	jsonw_uint_field(wr, "nominal_bit_rate_mbps", rate);
}

static void
print_sfp_diag_type(const struct rte_dev_eeprom_info *eeprom_info,
	      json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_DIAG_TYPE, 1,
			&xbuf);

	jsonw_uint_field(wr, "diag_type", xbuf);
}

static void
print_sfp_len(const struct rte_dev_eeprom_info *eeprom_info,
	      uint8_t offset, const char *field,
	      json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, offset, 1,
			&xbuf);

	jsonw_uint_field(wr, field, xbuf);
}

static void
print_sfp_encoding(const struct rte_dev_eeprom_info *eeprom_info,
		   json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_ENCODING, 1,
			&xbuf);

	convert_sff_encoding(wr, xbuf);
}

static void
print_sfp_8472_compl(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_COMPLIANCE, 1,
			&xbuf);

	convert_sff_8472_compl(wr, xbuf);
}

static void
print_qsfp_len(const struct rte_dev_eeprom_info *eeprom_info,
	      uint8_t offset, const char *field,
	      json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8436_BASE, offset, 1,
			&xbuf);

	jsonw_uint_field(wr, field, xbuf);
}

static void
print_qsfp_encoding(const struct rte_dev_eeprom_info *eeprom_info,
		   json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_ENCODING, 1,
			&xbuf);

	convert_sff_8436_encoding(wr, xbuf);
}

static void
print_qsfp_temp(const struct rte_dev_eeprom_info *eeprom_info,
		json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_TEMP, 2, xbuf);
	convert_sff_temp(wr, "temperature_C", xbuf);
}

static void
print_qsfp_voltage(const struct rte_dev_eeprom_info *eeprom_info,
		   json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_VCC, 2, xbuf);
	convert_sff_voltage(wr, "voltage_V", xbuf);
}

static void
print_sfp_rx_power(const struct rte_dev_eeprom_info *eeprom_info,
		   json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_RX_POWER, 2, xbuf);
	convert_sff_power(wr, "rx_power_mW", xbuf);
}

static void
print_sfp_tx_power(const struct rte_dev_eeprom_info *eeprom_info,
		   json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_TX_POWER, 2, xbuf);
	convert_sff_power(wr, "tx_power_mW", xbuf);
}

static void
print_sfp_laser_bias(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_TX_BIAS, 2, xbuf);
	convert_sff_bias(wr, "laser_bias", xbuf);
}

static void
print_qsfp_rx_power(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr, int chan)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8436_BASE,
			SFF_8436_RX_CH1_MSB + (chan * 2), 2, xbuf);
	convert_sff_power(wr, "rx_power_mW", xbuf);
}

static void
print_qsfp_tx_power(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr, int chan)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8436_BASE,
			SFF_8436_TX_CH1_MSB + (chan * 2), 2, xbuf);
	convert_sff_power(wr, "tx_power_mW", xbuf);
}

static void
print_qsfp_laser_bias(const struct rte_dev_eeprom_info *eeprom_info,
		    json_writer_t *wr, int chan)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	get_eeprom_data(eeprom_info, SFF_8436_BASE,
			SFF_8436_TX_BIAS_CH1_MSB + (chan * 2), 2, xbuf);
	convert_sff_power(wr, "laser_bias", xbuf);
}

static void
print_qsfp_rev_compliance(const struct rte_dev_eeprom_info *eeprom_info,
			  json_writer_t *wr)
{
	uint8_t xbuf;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_STATUS, 1, &xbuf);
	convert_sff_8436_rev_compliance(wr, xbuf);
}

static void
print_qsfp_br(const struct rte_dev_eeprom_info *eeprom_info,
	      json_writer_t *wr)
{
	uint8_t xbuf;
	uint32_t rate;

	xbuf = 0;
	get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8436_BITRATE, 1, &xbuf);
	rate = xbuf * 100;
	if (xbuf == 0xFF) {
		get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF_8636_BITRATE,
				1, &xbuf);
		rate = xbuf * 250;
	}

	jsonw_uint_field(wr, "nominal_bit_rate_mbps", rate);
}

static void
print_qsfp_temp_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TEMP_HIGH_ALARM, 2, xbuf))
		convert_sff_temp(wr, "high_temp_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TEMP_LOW_ALARM, 2, xbuf))
		convert_sff_temp(wr, "low_temp_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TEMP_HIGH_WARN, 2, xbuf))
		convert_sff_temp(wr, "high_temp_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TEMP_LOW_WARN, 2, xbuf))
		convert_sff_temp(wr, "low_temp_warn_thresh", xbuf);
}

static void
print_qsfp_voltage_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			 json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			    SFF_8636_VOLTAGE_HIGH_ALARM, 2, xbuf))
		convert_sff_voltage(wr, "high_voltage_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_VOLTAGE_LOW_ALARM, 2, xbuf))
		convert_sff_voltage(wr, "low_voltage_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_VOLTAGE_HIGH_WARN, 2, xbuf))
		convert_sff_voltage(wr, "high_voltage_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_VOLTAGE_LOW_WARN, 2, xbuf))
		convert_sff_voltage(wr, "low_voltage_warn_thresh", xbuf);
}

static void
print_qsfp_bias_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_BIAS_HIGH_ALARM, 2, xbuf))
		convert_sff_bias(wr, "high_bias_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_BIAS_LOW_ALARM, 2, xbuf))
		convert_sff_bias(wr, "low_bias_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_BIAS_HIGH_WARN, 2, xbuf))
		convert_sff_bias(wr, "high_bias_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_BIAS_LOW_WARN, 2, xbuf))
		convert_sff_bias(wr, "low_bias_warn_thresh", xbuf);
}

static void
print_qsfp_tx_power_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			  json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_POWER_HIGH_ALARM, 2, xbuf))
		convert_sff_power(wr, "high_tx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_POWER_LOW_ALARM, 2, xbuf))
		convert_sff_power(wr, "low_tx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_POWER_HIGH_WARN, 2, xbuf))
		convert_sff_power(wr, "high_tx_power_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_TX_POWER_LOW_WARN, 2, xbuf))
		convert_sff_power(wr, "low_tx_power_warn_thresh", xbuf);
}

static void
print_qsfp_rx_power_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			  json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_RX_POWER_HIGH_ALARM, 2, xbuf))
		convert_sff_power(wr, "high_rx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_RX_POWER_LOW_ALARM, 2, xbuf))
		convert_sff_power(wr, "low_rx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_RX_POWER_HIGH_WARN, 2, xbuf))
		convert_sff_power(wr, "high_rx_power_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8436_BASE,
			     SFF_8636_RX_POWER_LOW_WARN, 2, xbuf))
		convert_sff_power(wr, "low_rx_power_warn_thresh", xbuf);
}

static void
print_qsfp_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	print_qsfp_temp_thresholds(eeprom_info, wr);
	print_qsfp_voltage_thresholds(eeprom_info, wr);
	print_qsfp_bias_thresholds(eeprom_info, wr);
	print_qsfp_tx_power_thresholds(eeprom_info, wr);
	print_qsfp_rx_power_thresholds(eeprom_info, wr);
}

static void
print_temp_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TEMP_HIGH_ALM, 2, xbuf))
		convert_sff_temp(wr, "high_temp_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TEMP_LOW_ALM, 2, xbuf))
		convert_sff_temp(wr, "low_temp_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TEMP_HIGH_WARN, 2, xbuf))
		convert_sff_temp(wr, "high_temp_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TEMP_LOW_WARN, 2, xbuf))
		convert_sff_temp(wr, "low_temp_warn_thresh", xbuf);
}

static void
print_voltage_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			 json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			    SFF_8472_VOLTAGE_HIGH_ALM, 2, xbuf))
		convert_sff_voltage(wr, "high_voltage_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_VOLTAGE_LOW_ALM, 2, xbuf))
		convert_sff_voltage(wr, "low_voltage_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_VOLTAGE_HIGH_WARN, 2, xbuf))
		convert_sff_voltage(wr, "high_voltage_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_VOLTAGE_LOW_WARN, 2, xbuf))
		convert_sff_voltage(wr, "low_voltage_warn_thresh", xbuf);
}

static void
print_bias_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
		uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_BIAS_HIGH_ALM, 2, xbuf))
		convert_sff_bias(wr, "high_bias_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_BIAS_LOW_ALM, 2, xbuf))
		convert_sff_bias(wr, "low_bias_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_BIAS_HIGH_WARN, 2, xbuf))
		convert_sff_bias(wr, "high_bias_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_BIAS_LOW_WARN, 2, xbuf))
		convert_sff_bias(wr, "low_bias_warn_thresh", xbuf);
}

static void
print_tx_power_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			  json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TX_POWER_HIGH_ALM, 2, xbuf))
		convert_sff_power(wr, "high_tx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TX_POWER_LOW_ALM, 2, xbuf))
		convert_sff_power(wr, "low_tx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TX_POWER_HIGH_WARN, 2, xbuf))
		convert_sff_power(wr, "high_tx_power_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_TX_POWER_LOW_WARN, 2, xbuf))
		convert_sff_power(wr, "low_tx_power_warn_thresh", xbuf);
}

static void
print_rx_power_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
			  json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_RX_POWER_HIGH_ALM, 2, xbuf))
		convert_sff_power(wr, "high_rx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_RX_POWER_LOW_ALM, 2, xbuf))
		convert_sff_power(wr, "low_rx_power_alarm_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_RX_POWER_HIGH_WARN, 2, xbuf))
		convert_sff_power(wr, "high_rx_power_warn_thresh", xbuf);
	if (!get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			     SFF_8472_RX_POWER_LOW_WARN, 2, xbuf))
		convert_sff_power(wr, "low_rx_power_warn_thresh", xbuf);
}

static void
print_sfp_thresholds(const struct rte_dev_eeprom_info *eeprom_info,
		     json_writer_t *wr)
{
	print_temp_thresholds(eeprom_info, wr);
	print_voltage_thresholds(eeprom_info, wr);
	print_bias_thresholds(eeprom_info, wr);
	print_tx_power_thresholds(eeprom_info, wr);
	print_rx_power_thresholds(eeprom_info, wr);
}

static void
convert_aw_flags(json_writer_t *wr, struct _nv *x, uint8_t *xbuf)
{
	uint16_t flags;

	flags = (uint16_t)((xbuf[0] << 8) | xbuf[1]);
	for (; x->n != NULL; x++)
		jsonw_bool_field(wr, x->n, flags & (1 << x->v));
}

static void
print_sfp_alarm_flags(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_DIAG, SFF_8472_ALARM_FLAGS,
			    2, xbuf))
		return;
	convert_aw_flags(wr, alarm_flags, xbuf);
}

static void
print_sfp_warning_flags(const struct rte_dev_eeprom_info *eeprom_info,
			json_writer_t *wr)
{
	uint8_t xbuf[2];

	memset(xbuf, 0, sizeof(xbuf));
	if (get_eeprom_data(eeprom_info, SFF_8472_DIAG,
			    SFF_8472_WARNING_FLAGS, 2, xbuf))
		return;
	convert_aw_flags(wr, warning_flags, xbuf);
}

static void
convert_qsfp_aw_flags(json_writer_t *wr, struct _nv  *x,
		  uint8_t flags)
{

	for (; x->n != NULL; x++)
		jsonw_bool_field(wr, x->n, flags & (1 << x->v));
}

static void
print_qsfp_temp_aw_flags(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf = 0;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF8436_TEMP_AW_OFFSET,
			    1, &xbuf))
		return;

	convert_qsfp_aw_flags(wr, temp_alarm_warn_flags, xbuf);
}

static void
print_qsfp_voltage_aw_flags(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf = 0;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE, SFF8436_VCC_AW_OFFSET,
			    1, &xbuf))
		return;

	convert_qsfp_aw_flags(wr, voltage_alarm_warn_flags, xbuf);
}

static void
print_qsfp_aw_flags(const struct rte_dev_eeprom_info *eeprom_info,
		      json_writer_t *wr)
{
	uint8_t xbuf_tx_bias_12 = 0;
	uint8_t xbuf_tx_bias_34 = 0;
	uint8_t xbuf_tx_pow_12 = 0;
	uint8_t xbuf_tx_pow_34 = 0;
	uint8_t xbuf_rx_pow_12 = 0;
	uint8_t xbuf_rx_pow_34 = 0;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
				SFF8436_TX_BIAS_12_AW_OFFSET,
				1, &xbuf_tx_bias_12))
		return;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
				SFF8436_TX_BIAS_34_AW_OFFSET,
				1, &xbuf_tx_bias_34))
		return;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
				SFF8436_TX_PWR_12_AW_OFFSET,
				1, &xbuf_tx_pow_12))
		return;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
				SFF8436_TX_PWR_34_AW_OFFSET,
				1, &xbuf_tx_pow_34))
		return;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
			SFF8436_RX_PWR_12_AW_OFFSET,
			1, &xbuf_rx_pow_12))
		return;

	if (get_eeprom_data(eeprom_info, SFF_8436_BASE,
			SFF8436_RX_PWR_34_AW_OFFSET,
			1, &xbuf_rx_pow_34))
		return;

	jsonw_name(wr, "alarm_warning");
	jsonw_start_array(wr);

	jsonw_start_object(wr);
	convert_qsfp_aw_flags(wr, tx_bias_aw_chan_upper_flags, xbuf_tx_bias_12);
	convert_qsfp_aw_flags(wr, tx_pwr_aw_chan_upper_flags, xbuf_tx_pow_12);
	convert_qsfp_aw_flags(wr, rx_pwr_aw_chan_upper_flags, xbuf_rx_pow_12);
	jsonw_end_object(wr);

	jsonw_start_object(wr);
	convert_qsfp_aw_flags(wr, tx_bias_aw_chan_lower_flags, xbuf_tx_bias_12);
	convert_qsfp_aw_flags(wr, tx_pwr_aw_chan_lower_flags, xbuf_tx_pow_12);
	convert_qsfp_aw_flags(wr, rx_pwr_aw_chan_lower_flags, xbuf_rx_pow_12);
	jsonw_end_object(wr);

	jsonw_start_object(wr);
	convert_qsfp_aw_flags(wr, tx_bias_aw_chan_upper_flags, xbuf_tx_bias_34);
	convert_qsfp_aw_flags(wr, tx_pwr_aw_chan_upper_flags, xbuf_tx_pow_34);
	convert_qsfp_aw_flags(wr, rx_pwr_aw_chan_upper_flags, xbuf_rx_pow_34);
	jsonw_end_object(wr);

	jsonw_start_object(wr);
	convert_qsfp_aw_flags(wr, tx_bias_aw_chan_lower_flags, xbuf_tx_bias_34);
	convert_qsfp_aw_flags(wr, tx_pwr_aw_chan_lower_flags, xbuf_tx_pow_34);
	convert_qsfp_aw_flags(wr, rx_pwr_aw_chan_lower_flags, xbuf_rx_pow_34);
	jsonw_end_object(wr);

	jsonw_end_array(wr);
}

static void
print_sfp_status(const struct rte_eth_dev_module_info *module_info,
		 const struct rte_dev_eeprom_info *eeprom_info,
		 json_writer_t *wr)
{
	uint8_t diag_type, flags;
	int do_diag = 0;

	/* Read diagnostic monitoring type */
	if (get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_DIAG_TYPE,
			    1, &diag_type))
		return;

	/*
	 * Read monitoring data IFF it is supplied AND is
	 * internally calibrated
	 */
	flags = SFF_8472_DDM_DONE | SFF_8472_DDM_INTERNAL;
	if ((diag_type & flags) == flags)
		do_diag = 1;

	/* Transceiver type */
	print_sfp_identifier(eeprom_info, wr);
	print_sfp_ext_identifier(eeprom_info, wr);
	print_sfp_transceiver_class(eeprom_info, wr);
	print_sfp_connector(eeprom_info, wr);
	print_sfp_vendor(module_info, eeprom_info, wr);
	print_sfp_transceiver_descr(eeprom_info, wr);
	print_sfp_br(eeprom_info, wr);
	print_sfp_diag_type(eeprom_info, wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_OM4, "copper_len", wr);
	print_sfp_encoding(eeprom_info, wr);
	print_sfp_8472_compl(eeprom_info, wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_SMF, "smf_100", wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_SMF_KM, "smf_km", wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_625UM, "smf_om1", wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_50UM, "smf_om2", wr);
	print_sfp_len(eeprom_info, SFF_8472_LEN_OM3, "smf_om3", wr);
	/*
	 * Request current measurements iff they are provided:
	 */
	if (do_diag != 0) {
		print_sfp_temp(eeprom_info, wr);
		print_sfp_voltage(eeprom_info, wr);
		print_sfp_rx_power(eeprom_info, wr);
		print_sfp_tx_power(eeprom_info, wr);
		print_sfp_laser_bias(eeprom_info, wr);
	}
	print_sfp_thresholds(eeprom_info, wr);
	print_sfp_alarm_flags(eeprom_info, wr);
	print_sfp_warning_flags(eeprom_info, wr);
}

static void
print_qsfp_status(const struct rte_dev_eeprom_info *eeprom_info,
		  json_writer_t *wr)
{
	/* Transceiver type */
	print_qsfp_identifier(eeprom_info, wr);
	print_qsfp_ext_identifier(eeprom_info, wr);
	print_qsfp_transceiver_class(eeprom_info, wr);
	print_qsfp_connector(eeprom_info, wr);
	print_qsfp_vendor(eeprom_info, wr);
	print_qsfp_encoding(eeprom_info, wr);
	print_qsfp_rev_compliance(eeprom_info, wr);
	print_qsfp_br(eeprom_info, wr);

	print_qsfp_len(eeprom_info, SFF_8436_LEN_SMF_KM, "smf_km", wr);
	print_qsfp_len(eeprom_info, SFF_8436_LEN_OM1, "smf_om1", wr);
	print_qsfp_len(eeprom_info, SFF_8436_LEN_OM2, "smf_om2", wr);
	print_qsfp_len(eeprom_info, SFF_8436_LEN_OM3, "smf_om3", wr);


	/*
	 * The standards in this area are not clear when the
	 * additional measurements are present or not. Use a valid
	 * temperature reading as an indicator for the presence of
	 * voltage and TX/RX power measurements.
	 */
	print_qsfp_temp(eeprom_info, wr);
	print_qsfp_voltage(eeprom_info, wr);
	jsonw_name(wr, "measured_values");
	jsonw_start_array(wr);
	for (int i = 0; i < 4; i++) {
		jsonw_start_object(wr);
		print_qsfp_rx_power(eeprom_info, wr, i);
		print_qsfp_tx_power(eeprom_info, wr, i);
		print_qsfp_laser_bias(eeprom_info, wr, i);
		jsonw_end_object(wr);
	}
	jsonw_end_array(wr);

	print_qsfp_temp_aw_flags(eeprom_info, wr);
	print_qsfp_voltage_aw_flags(eeprom_info, wr);

	print_qsfp_aw_flags(eeprom_info, wr);

	print_qsfp_thresholds(eeprom_info, wr);
}


void
sfp_status(const struct rte_eth_dev_module_info *module_info,
	   const struct rte_dev_eeprom_info *eeprom_info,
	   json_writer_t *wr)
{
	uint8_t id_byte;

	/*
	 * Try to read byte 0:
	 * Both SFF-8472 and SFF-8436 use it as
	 * 'identification byte'.
	 * Stop reading status on zero as value -
	 * this might happen in case of empty transceiver slot.
	 */
	id_byte = 0;
	get_eeprom_data(eeprom_info, SFF_8472_BASE, SFF_8472_ID, 1,
			&id_byte);
	if (id_byte == 0)
		return;

	switch (id_byte) {
	case SFF_8024_ID_QSFP:
	case SFF_8024_ID_QSFPPLUS:
	case SFF_8024_ID_QSFP28:
		print_qsfp_status(eeprom_info, wr);
		break;
	default:
		print_sfp_status(module_info, eeprom_info, wr);
	}
}

