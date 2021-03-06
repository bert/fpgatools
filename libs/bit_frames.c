//
// Author: Wolfgang Spraul
//
// This is free and unencumbered software released into the public domain.
// For details see the UNLICENSE file at the root of the source tree.
//

#include "model.h"
#include "bit.h"
#include "parts.h"
#include "control.h"

#define HCLK_BYTES 2

static uint8_t* get_first_minor(struct fpga_bits* bits, int row, int major)
{
	int i, num_frames;

	num_frames = 0;
	for (i = 0; i < major; i++)
		num_frames += get_major_minors(XC6SLX9, i);
	return &bits->d[(row*FRAMES_PER_ROW + num_frames)*FRAME_SIZE];
}

static int get_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_get_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

static void set_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_set_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

static void clear_bit(struct fpga_bits* bits,
	int row, int major, int minor, int bit_i)
{
	return frame_clear_bit(get_first_minor(bits, row, major)
		+ minor*FRAME_SIZE, bit_i);
}

struct bit_pos
{
	int row;
	int major;
	int minor;
	int bit_i;
};

static int get_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	return get_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static void set_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	set_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static void clear_bitp(struct fpga_bits* bits, struct bit_pos* pos)
{
	clear_bit(bits, pos->row, pos->major, pos->minor, pos->bit_i);
}

static struct bit_pos s_default_bits[] = {
	{ 0, 0, 3, 66 },
	{ 0, 1, 23, 1034 },
	{ 0, 1, 23, 1035 },
	{ 0, 1, 23, 1039 },
	{ 2, 0, 3, 66 }};

struct sw_yxpos
{
	int y;
	int x;
	swidx_t idx;
};

#define MAX_YX_SWITCHES 1024

struct extract_state
{
	struct fpga_model* model;
	struct fpga_bits* bits;
	// yx switches are fully extracted ones pointing into the
	// model, stored here for later processing into nets.
	int num_yx_pos;
	struct sw_yxpos yx_pos[MAX_YX_SWITCHES]; // needs to be dynamically alloced...
};

static int find_es_switch(struct extract_state* es, int y, int x, swidx_t sw)
{
	int i;
	if (sw == NO_SWITCH) { HERE(); return 0; }
	for (i = 0; i < es->num_yx_pos; i++) {
		if (es->yx_pos[i].y == y
		    && es->yx_pos[i].x == x
		    && es->yx_pos[i].idx == sw)
			return 1;
	}
	return 0;
}

static int write_iobs(struct fpga_bits* bits, struct fpga_model* model)
{
	int i, y, x, type_idx, part_idx, dev_idx, first_iob, rc;
	struct fpga_device* dev;
	uint64_t u64;
	const char* name;

	first_iob = 0;
	for (i = 0; (name = fpga_enum_iob(model, i, &y, &x, &type_idx)); i++) {
		dev_idx = fpga_dev_idx(model, y, x, DEV_IOB, type_idx);
		if (dev_idx == NO_DEV) FAIL(EINVAL);
		dev = FPGA_DEV(model, y, x, dev_idx);
		if (!dev->instantiated)
			continue;

		part_idx = find_iob_sitename(XC6SLX9, name);
		if (part_idx == -1) {
			HERE();
			continue;
		}

		if (!first_iob) {
			first_iob = 1;
			// todo: is this right on the other sides?
			set_bit(bits, /*row*/ 0, get_rightside_major(XC6SLX9),
				/*minor*/ 22, 64*15+XC6_HCLK_BITS+4);
		}

		if (dev->u.iob.istandard[0]) {
			if (!dev->u.iob.I_mux
			    || !dev->u.iob.bypass_mux
			    || dev->u.iob.ostandard[0])
				HERE();

			u64 = XC6_IOB_INPUT | XC6_IOB_INSTANTIATED;

			if (dev->u.iob.I_mux == IMUX_I_B)
				u64 |= XC6_IOB_IMUX_I_B;

			if (!strcmp(dev->u.iob.istandard, IO_LVCMOS33)
			    || !strcmp(dev->u.iob.istandard, IO_LVCMOS25)
			    || !strcmp(dev->u.iob.istandard, IO_LVTTL))
				u64 |= XC6_IOB_INPUT_LVCMOS33_25_LVTTL;
			else if (!strcmp(dev->u.iob.istandard, IO_LVCMOS18)
			    || !strcmp(dev->u.iob.istandard, IO_LVCMOS15)
			    || !strcmp(dev->u.iob.istandard, IO_LVCMOS12))
				u64 |= XC6_IOB_INPUT_LVCMOS18_15_12;
			else if (!strcmp(dev->u.iob.istandard, IO_LVCMOS18_JEDEC)
			    || !strcmp(dev->u.iob.istandard, IO_LVCMOS15_JEDEC)
			    || !strcmp(dev->u.iob.istandard, IO_LVCMOS12_JEDEC))
				u64 |= XC6_IOB_INPUT_LVCMOS18_15_12_JEDEC;
			else if (!strcmp(dev->u.iob.istandard, IO_SSTL2_I))
				u64 |= XC6_IOB_INPUT_SSTL2_I;
			else
				HERE();

			frame_set_u64(&bits->d[IOB_DATA_START
				+ part_idx*IOB_ENTRY_LEN], u64);
		} else if (dev->u.iob.ostandard[0]) {
			if (!dev->u.iob.drive_strength
			    || !dev->u.iob.slew
			    || !dev->u.iob.suspend
			    || dev->u.iob.istandard[0])
				HERE();

			u64 = XC6_IOB_INSTANTIATED;
			// for now we always turn on O_PINW even if no net
			// is connected to the pinw
			u64 |= XC6_IOB_O_PINW;
			if (!strcmp(dev->u.iob.ostandard, IO_LVTTL)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_12; break;
					case 16: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_16; break;
					case 24: u64 |= XC6_IOB_OUTPUT_LVTTL_DRIVE_24; break;
					default: FAIL(EINVAL);
				}
			} else if (!strcmp(dev->u.iob.ostandard, IO_LVCMOS33)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVCMOS33_25_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_12; break;
					case 16: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_16; break;
					case 24: u64 |= XC6_IOB_OUTPUT_LVCMOS33_DRIVE_24; break;
					default: FAIL(EINVAL);
				}
			} else if (!strcmp(dev->u.iob.ostandard, IO_LVCMOS25)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVCMOS33_25_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_12; break;
					case 16: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_16; break;
					case 24: u64 |= XC6_IOB_OUTPUT_LVCMOS25_DRIVE_24; break;
					default: FAIL(EINVAL);
				}
			} else if (!strcmp(dev->u.iob.ostandard, IO_LVCMOS18)
				   || !strcmp(dev->u.iob.ostandard, IO_LVCMOS18_JEDEC)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_12; break;
					case 16: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_16; break;
					case 24: u64 |= XC6_IOB_OUTPUT_LVCMOS18_DRIVE_24; break;
					default: FAIL(EINVAL);
				}
			} else if (!strcmp(dev->u.iob.ostandard, IO_LVCMOS15)
				   || !strcmp(dev->u.iob.ostandard, IO_LVCMOS15_JEDEC)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_12; break;
					case 16: u64 |= XC6_IOB_OUTPUT_LVCMOS15_DRIVE_16; break;
					default: FAIL(EINVAL);
				}
			} else if (!strcmp(dev->u.iob.ostandard, IO_LVCMOS12)
				   || !strcmp(dev->u.iob.ostandard, IO_LVCMOS12_JEDEC)) {
				switch (dev->u.iob.drive_strength) {
					case 2: u64 |= XC6_IOB_OUTPUT_LVCMOS12_DRIVE_2; break;
					case 4: u64 |= XC6_IOB_OUTPUT_LVCMOS12_DRIVE_4; break;
					case 6: u64 |= XC6_IOB_OUTPUT_LVCMOS12_DRIVE_6; break;
					case 8: u64 |= XC6_IOB_OUTPUT_LVCMOS12_DRIVE_8; break;
					case 12: u64 |= XC6_IOB_OUTPUT_LVCMOS12_DRIVE_12; break;
					default: FAIL(EINVAL);
				}
			} else FAIL(EINVAL);
			switch (dev->u.iob.slew) {
				case SLEW_SLOW: u64 |= XC6_IOB_SLEW_SLOW; break;
				case SLEW_FAST: u64 |= XC6_IOB_SLEW_FAST; break;
				case SLEW_QUIETIO: u64 |= XC6_IOB_SLEW_QUIETIO; break;
				default: FAIL(EINVAL);
			}
			switch (dev->u.iob.suspend) {
				case SUSP_LAST_VAL: u64 |= XC6_IOB_SUSP_LAST_VAL; break;
				case SUSP_3STATE: u64 |= XC6_IOB_SUSP_3STATE; break;
				case SUSP_3STATE_PULLUP: u64 |= XC6_IOB_SUSP_3STATE_PULLUP; break;
				case SUSP_3STATE_PULLDOWN: u64 |= XC6_IOB_SUSP_3STATE_PULLDOWN; break;
				case SUSP_3STATE_KEEPER: u64 |= XC6_IOB_SUSP_3STATE_KEEPER; break;
				case SUSP_3STATE_OCT_ON: u64 |= XC6_IOB_SUSP_3STATE_OCT_ON; break;
				default: FAIL(EINVAL);
			}

			frame_set_u64(&bits->d[IOB_DATA_START
				+ part_idx*IOB_ENTRY_LEN], u64);
		} else HERE();
	}
	return 0;
fail:
	return rc;
}

static int extract_iobs(struct extract_state* es)
{
	int i, num_iobs, iob_y, iob_x, iob_idx, dev_idx, first_iob, rc;
	uint64_t u64;
	const char* iob_sitename;
	struct fpga_device* dev;
	struct fpgadev_iob cfg;

	num_iobs = get_num_iobs(XC6SLX9);
	first_iob = 0;
	for (i = 0; i < num_iobs; i++) {
		u64 = frame_get_u64(&es->bits->d[
			IOB_DATA_START + i*IOB_ENTRY_LEN]);
		if (!u64) continue;

		iob_sitename = get_iob_sitename(XC6SLX9, i);
		if (!iob_sitename) {
			// The space for 6 IOBs on all four sides
			// (6*8 = 48 bytes, *4=192 bytes) is used
			// for clocks etc, so we ignore them here.
			continue;
		}
		rc = fpga_find_iob(es->model, iob_sitename, &iob_y, &iob_x, &iob_idx);
		if (rc) FAIL(rc);
		dev_idx = fpga_dev_idx(es->model, iob_y, iob_x, DEV_IOB, iob_idx);
		if (dev_idx == NO_DEV) FAIL(EINVAL);
		dev = FPGA_DEV(es->model, iob_y, iob_x, dev_idx);
		memset(&cfg, 0, sizeof(cfg));

		if (!first_iob) {
			first_iob = 1;
			// todo: is this right on the other sides?
			if (!get_bit(es->bits, /*row*/ 0, get_rightside_major(XC6SLX9),
				/*minor*/ 22, 64*15+XC6_HCLK_BITS+4))
				HERE();
			clear_bit(es->bits, /*row*/ 0, get_rightside_major(XC6SLX9),
				/*minor*/ 22, 64*15+XC6_HCLK_BITS+4);
		}
		if (u64 & XC6_IOB_INSTANTIATED)
			u64 &= ~XC6_IOB_INSTANTIATED;
		else
			HERE();

		switch (u64 & XC6_IOB_MASK_IO) {
			case XC6_IOB_INPUT:
				cfg.bypass_mux = BYPASS_MUX_I;

				if (u64 & XC6_IOB_IMUX_I_B) {
					cfg.I_mux = IMUX_I_B;
					u64 &= ~XC6_IOB_IMUX_I_B;
				} else
					cfg.I_mux = IMUX_I;

				switch (u64 & XC6_IOB_MASK_IN_TYPE) {
					case XC6_IOB_INPUT_LVCMOS33_25_LVTTL:
						u64 &= ~XC6_IOB_MASK_IN_TYPE;
						strcpy(cfg.istandard, IO_LVCMOS25);
						break;
					case XC6_IOB_INPUT_LVCMOS18_15_12:
						u64 &= ~XC6_IOB_MASK_IN_TYPE;
						strcpy(cfg.istandard, IO_LVCMOS12);
						break;
					case XC6_IOB_INPUT_LVCMOS18_15_12_JEDEC:
						u64 &= ~XC6_IOB_MASK_IN_TYPE;
						strcpy(cfg.istandard, IO_LVCMOS12_JEDEC);
						break;
					case XC6_IOB_INPUT_SSTL2_I:
						u64 &= ~XC6_IOB_MASK_IN_TYPE;
						strcpy(cfg.istandard, IO_SSTL2_I);
						break;
					default: HERE(); break;
				}
				break;

			case XC6_IOB_OUTPUT_LVCMOS33_25_DRIVE_2:	
				cfg.drive_strength = 2;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_16:
				cfg.drive_strength = 16;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;
			case XC6_IOB_OUTPUT_LVCMOS33_DRIVE_24:
				cfg.drive_strength = 24;
				strcpy(cfg.ostandard, IO_LVCMOS33);
				break;

			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_16:
				cfg.drive_strength = 16;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;
			case XC6_IOB_OUTPUT_LVCMOS25_DRIVE_24:
				cfg.drive_strength = 24;
				strcpy(cfg.ostandard, IO_LVCMOS25);
				break;

			case XC6_IOB_OUTPUT_LVTTL_DRIVE_2:
				cfg.drive_strength = 2;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_16:
				cfg.drive_strength = 16;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;
			case XC6_IOB_OUTPUT_LVTTL_DRIVE_24:
				cfg.drive_strength = 24;
				strcpy(cfg.ostandard, IO_LVTTL);
				break;

			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_2:
				cfg.drive_strength = 2;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_16:
				cfg.drive_strength = 16;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;
			case XC6_IOB_OUTPUT_LVCMOS18_DRIVE_24:
				cfg.drive_strength = 24;
				strcpy(cfg.ostandard, IO_LVCMOS18);
				break;

			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_2:
				cfg.drive_strength = 2;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;
			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;
			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;
			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;
			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;
			case XC6_IOB_OUTPUT_LVCMOS15_DRIVE_16:
				cfg.drive_strength = 16;
				strcpy(cfg.ostandard, IO_LVCMOS15);
				break;

			case XC6_IOB_OUTPUT_LVCMOS12_DRIVE_2:
				cfg.drive_strength = 2;
				strcpy(cfg.ostandard, IO_LVCMOS12);
				break;
			case XC6_IOB_OUTPUT_LVCMOS12_DRIVE_4:
				cfg.drive_strength = 4;
				strcpy(cfg.ostandard, IO_LVCMOS12);
				break;
			case XC6_IOB_OUTPUT_LVCMOS12_DRIVE_6:
				cfg.drive_strength = 6;
				strcpy(cfg.ostandard, IO_LVCMOS12);
				break;
			case XC6_IOB_OUTPUT_LVCMOS12_DRIVE_8:
				cfg.drive_strength = 8;
				strcpy(cfg.ostandard, IO_LVCMOS12);
				break;
			case XC6_IOB_OUTPUT_LVCMOS12_DRIVE_12:
				cfg.drive_strength = 12;
				strcpy(cfg.ostandard, IO_LVCMOS12);
				break;

			default: HERE(); break;
		}
		if (cfg.istandard[0] || cfg.ostandard[0])
			u64 &= ~XC6_IOB_MASK_IO;
		if (cfg.ostandard[0]) {
			cfg.O_used = 1;
			u64 &= ~XC6_IOB_O_PINW;

			if (!strcmp(cfg.ostandard, IO_LVCMOS12)
			    || !strcmp(cfg.ostandard, IO_LVCMOS15)
			    || !strcmp(cfg.ostandard, IO_LVCMOS18)
			    || !strcmp(cfg.ostandard, IO_LVCMOS25)
			    || !strcmp(cfg.ostandard, IO_LVCMOS33)
			    || !strcmp(cfg.ostandard, IO_LVTTL)) {
				switch (u64 & XC6_IOB_MASK_SLEW) {
					case XC6_IOB_SLEW_SLOW:
						u64 &= ~XC6_IOB_MASK_SLEW;
						cfg.slew = SLEW_SLOW;
						break;
					case XC6_IOB_SLEW_FAST:
						u64 &= ~XC6_IOB_MASK_SLEW;
						cfg.slew = SLEW_FAST;
						break;
					case XC6_IOB_SLEW_QUIETIO:
						u64 &= ~XC6_IOB_MASK_SLEW;
						cfg.slew = SLEW_QUIETIO;
						break;
					default: HERE();
				}
			}
			switch (u64 & XC6_IOB_MASK_SUSPEND) {
				case XC6_IOB_SUSP_3STATE:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_3STATE;
					break;
				case XC6_IOB_SUSP_3STATE_OCT_ON:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_3STATE_OCT_ON;
					break;
				case XC6_IOB_SUSP_3STATE_KEEPER:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_3STATE_KEEPER;
					break;
				case XC6_IOB_SUSP_3STATE_PULLUP:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_3STATE_PULLUP;
					break;
				case XC6_IOB_SUSP_3STATE_PULLDOWN:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_3STATE_PULLDOWN;
					break;
				case XC6_IOB_SUSP_LAST_VAL:
					u64 &= ~XC6_IOB_MASK_SUSPEND;
					cfg.suspend = SUSP_LAST_VAL;
					break;
				default: HERE();
			}
		}
		if (!u64) {
			frame_set_u64(&es->bits->d[IOB_DATA_START
				+ i*IOB_ENTRY_LEN], 0);
			dev->instantiated = 1;
			dev->u.iob = cfg;
		} else HERE();
	}
	return 0;
fail:
	return rc;
}

static int lut2str(uint64_t lut, int lut_pos, int lut5_used,
	char *lut6_buf, char** lut6_p, char *lut5_buf, char** lut5_p)
{
	int lut_map[64], rc;
	uint64_t lut_mapped;
	const char* str;

	if (lut5_used) {
		xc6_lut_bitmap(lut_pos, &lut_map, 32);
		lut_mapped = map_bits(lut, 64, lut_map);

		// lut6
		str = bool_bits2str(ULL_HIGH32(lut_mapped), 32);
		if (!str) FAIL(EINVAL);
		snprintf(lut6_buf, MAX_LUT_LEN, "(A6+~A6)*(%s)", str);
		*lut6_p = lut6_buf;

		// lut5
		str = bool_bits2str(ULL_LOW32(lut_mapped), 32);
		if (!str) FAIL(EINVAL);
		strcpy(lut5_buf, str);
		*lut5_p = lut5_buf;
	} else {
		xc6_lut_bitmap(lut_pos, &lut_map, 64);
		lut_mapped = map_bits(lut, 64, lut_map);
		str = bool_bits2str(lut_mapped, 64);
		if (!str) FAIL(EINVAL);
		strcpy(lut6_buf, str);
		*lut6_p = lut6_buf;
	}
	return 0;
fail:
	return rc;
}


static int extract_logic(struct extract_state* es)
{
	int row, row_pos, x, y, i, byte_off, last_minor, lut5_used, rc;
	int latch_ml, latch_x, l_col, lut;
	struct fpgadev_logic cfg_ml, cfg_x;
	uint64_t lut_X[4], lut_ML[4]; // LUT_A-LUT_D
	uint64_t mi20, mi23_M, mi2526;
	uint8_t* u8_p;
	char lut6_ml[NUM_LUTS][MAX_LUT_LEN];
	char lut5_ml[NUM_LUTS][MAX_LUT_LEN];
	char lut6_x[NUM_LUTS][MAX_LUT_LEN];
	char lut5_x[NUM_LUTS][MAX_LUT_LEN];
	struct fpga_device* dev_ml;

	for (x = LEFT_SIDE_WIDTH; x < es->model->x_width-RIGHT_SIDE_WIDTH; x++) {
		if (!is_atx(X_FABRIC_LOGIC_COL|X_CENTER_LOGIC_COL, es->model, x))
			continue;
		for (y = TOP_IO_TILES; y < es->model->y_height - BOT_IO_TILES; y++) {
			if (!has_device(es->model, y, x, DEV_LOGIC))
				continue;
			row = which_row(y, es->model);
			row_pos = pos_in_row(y, es->model);
			if (row == -1 || row_pos == -1 || row_pos == 8) {
				HERE();
				continue;
			}
			if (row_pos > 8) row_pos--;
			u8_p = get_first_minor(es->bits, row, es->model->x_major[x]);
			byte_off = row_pos * 8;
			if (row_pos >= 8) byte_off += HCLK_BYTES;

			//
			// Step 1:
			//
			// read all configuration bits for the 2 logic devices
			// into local variables
			//

			mi20 = frame_get_u64(u8_p + 20*FRAME_SIZE + byte_off) & XC6_MI20_LOGIC_MASK;
			if (has_device_type(es->model, y, x, DEV_LOGIC, LOGIC_M)) {
				mi23_M = frame_get_u64(u8_p + 23*FRAME_SIZE + byte_off);
				mi2526 = frame_get_u64(u8_p + 26*FRAME_SIZE + byte_off);
				lut_ML[LUT_A] = frame_get_lut64(u8_p + 24*FRAME_SIZE, row_pos*2+1);
				lut_ML[LUT_B] = frame_get_lut64(u8_p + 21*FRAME_SIZE, row_pos*2+1);
				lut_ML[LUT_C] = frame_get_lut64(u8_p + 24*FRAME_SIZE, row_pos*2);
				lut_ML[LUT_D] = frame_get_lut64(u8_p + 21*FRAME_SIZE, row_pos*2);
				lut_X[LUT_A] = frame_get_lut64(u8_p + 27*FRAME_SIZE, row_pos*2+1);
				lut_X[LUT_B] = frame_get_lut64(u8_p + 29*FRAME_SIZE, row_pos*2+1);
				lut_X[LUT_C] = frame_get_lut64(u8_p + 27*FRAME_SIZE, row_pos*2);
				lut_X[LUT_D] = frame_get_lut64(u8_p + 29*FRAME_SIZE, row_pos*2);
				l_col = 0;
			} else if (has_device_type(es->model, y, x, DEV_LOGIC, LOGIC_L)) {
				mi23_M = 0;
				mi2526 = frame_get_u64(u8_p + 25*FRAME_SIZE + byte_off);
				lut_ML[LUT_A] = frame_get_lut64(u8_p + 23*FRAME_SIZE, row_pos*2+1);
				lut_ML[LUT_B] = frame_get_lut64(u8_p + 21*FRAME_SIZE, row_pos*2+1);
				lut_ML[LUT_C] = frame_get_lut64(u8_p + 23*FRAME_SIZE, row_pos*2);
				lut_ML[LUT_D] = frame_get_lut64(u8_p + 21*FRAME_SIZE, row_pos*2);
				lut_X[LUT_A] = frame_get_lut64(u8_p + 26*FRAME_SIZE, row_pos*2+1);
				lut_X[LUT_B] = frame_get_lut64(u8_p + 28*FRAME_SIZE, row_pos*2+1);
				lut_X[LUT_C] = frame_get_lut64(u8_p + 26*FRAME_SIZE, row_pos*2);
				lut_X[LUT_D] = frame_get_lut64(u8_p + 28*FRAME_SIZE, row_pos*2);
				l_col = 1;
			} else {
				HERE();
				continue;
			}

			//
			// Step 2:
			//
			// If all is zero, assume the devices are not instantiated.
			// todo: we could check pin connectivity and infer 0-bit
			//       configured devices 
			//

		   	if (!mi20 && !mi23_M && !mi2526
			    && !lut_X[0] && !lut_X[1] && !lut_X[2] && !lut_X[3]
			    && !lut_ML[0] && !lut_ML[1] && !lut_ML[2] && !lut_ML[3])
				continue;

			//
			// Step 3:
			//
			// Parse all bits from minors 20 and 25/26 into more
			// easily usable cfg_ml and cfg_x structures.
			//

			memset(&cfg_ml, 0, sizeof(cfg_ml));
			memset(&cfg_x, 0, sizeof(cfg_x));
			latch_ml = 0;
			latch_x = 0;

			// minor20
			if (mi20 & (1ULL<<XC6_ML_D5_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_D].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_ML_D5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_X_D5_FFSRINIT_1)) {
				cfg_x.a2d[LUT_D].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_X_D5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_ML_C5_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_C].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_ML_C5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_X_C_FFSRINIT_1)) {
				cfg_x.a2d[LUT_C].ff_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_X_C_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_X_C5_FFSRINIT_1)) {
				cfg_x.a2d[LUT_C].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_X_C5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_ML_B5_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_B].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_ML_B5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_X_B5_FFSRINIT_1)) {
				cfg_x.a2d[LUT_B].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_X_B5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_M_A_FFSRINIT_1)) {
				if (l_col) {
					HERE();
					continue;
				}
				cfg_ml.a2d[LUT_A].ff_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_M_A_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_X_A5_FFSRINIT_1)) {
				cfg_x.a2d[LUT_A].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_X_A5_FFSRINIT_1);
			}
			if (mi20 & (1ULL<<XC6_ML_A5_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_A].ff5_srinit = FF_SRINIT1;
				mi20 &= ~(1ULL<<XC6_ML_A5_FFSRINIT_1);
			}

			// minor 25/26
			if (mi2526 & (1ULL<<XC6_ML_D_CY0_O5)) {
				cfg_ml.a2d[LUT_D].cy0 = CY0_O5;
				mi2526 &= ~(1ULL<<XC6_ML_D_CY0_O5);
			}
			if (mi2526 & (1ULL<<XC6_X_D_OUTMUX_O5)) {
				cfg_x.a2d[LUT_D].out_mux = MUX_O5;
				mi2526 &= ~(1ULL<<XC6_X_D_OUTMUX_O5);
			}
			if (mi2526 & (1ULL<<XC6_X_C_OUTMUX_O5)) {
				cfg_x.a2d[LUT_C].out_mux = MUX_O5;
				mi2526 &= ~(1ULL<<XC6_X_C_OUTMUX_O5);
			}
			if (mi2526 & (1ULL<<XC6_ML_D_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_D].ff_srinit = 1;
				mi2526 &= ~(1ULL<<XC6_ML_D_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_ML_C_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_C].ff_srinit = 1;
				mi2526 &= ~(1ULL<<XC6_ML_C_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_X_D_FFSRINIT_1)) {
				cfg_x.a2d[LUT_D].ff_srinit = 1;
				mi2526 &= ~(1ULL<<XC6_X_D_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_ML_C_CY0_O5)) {
				cfg_ml.a2d[LUT_C].cy0 = CY0_O5;
				mi2526 &= ~(1ULL<<XC6_ML_C_CY0_O5);
			}
			if (mi2526 & (1ULL<<XC6_X_B_OUTMUX_O5)) {
				cfg_x.a2d[LUT_B].out_mux = MUX_O5;
				mi2526 &= ~(1ULL<<XC6_X_B_OUTMUX_O5);
			}
			if (mi2526 & XC6_ML_D_OUTMUX_MASK) {
				switch ((mi2526 & XC6_ML_D_OUTMUX_MASK) >> XC6_ML_D_OUTMUX_O) {
					case XC6_ML_D_OUTMUX_O6:
						cfg_ml.a2d[LUT_D].out_mux = MUX_O6;
						break;
					case XC6_ML_D_OUTMUX_XOR:
						cfg_ml.a2d[LUT_D].out_mux = MUX_XOR;
						break;
					case XC6_ML_D_OUTMUX_O5:
						cfg_ml.a2d[LUT_D].out_mux = MUX_O5;
						break;
					case XC6_ML_D_OUTMUX_CY:
						cfg_ml.a2d[LUT_D].out_mux = MUX_CY;
						break;
					case XC6_ML_D_OUTMUX_5Q:
						cfg_ml.a2d[LUT_D].out_mux = MUX_5Q;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_D_OUTMUX_MASK;
			}
			if (mi2526 & XC6_ML_D_FFMUX_MASK) {
				switch ((mi2526 & XC6_ML_D_FFMUX_MASK) >> XC6_ML_D_FFMUX_O) {
					case XC6_ML_D_FFMUX_O5:
						cfg_ml.a2d[LUT_D].ff_mux = MUX_O5;
						break;
					case XC6_ML_D_FFMUX_X:
						cfg_ml.a2d[LUT_D].ff_mux = MUX_X;
						break;
					case XC6_ML_D_FFMUX_XOR:
						cfg_ml.a2d[LUT_D].ff_mux = MUX_XOR;
						break;
					case XC6_ML_D_FFMUX_CY:
						cfg_ml.a2d[LUT_D].ff_mux = MUX_CY;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_D_FFMUX_MASK;
			}
			if (mi2526 & (1ULL<<XC6_X_CLK_B)) {
				cfg_x.clk_inv = CLKINV_B;
				mi2526 &= ~(1ULL<<XC6_X_CLK_B);
			}
			if (mi2526 & (1ULL<<XC6_ML_ALL_LATCH)) {
				latch_ml = 1;
				mi2526 &= ~(1ULL<<XC6_ML_ALL_LATCH);
			}
			if (mi2526 & (1ULL<<XC6_ML_SR_USED)) {
				cfg_ml.sr_used = 1;
				mi2526 &= ~(1ULL<<XC6_ML_SR_USED);
			}
			if (mi2526 & (1ULL<<XC6_ML_SYNC)) {
				cfg_ml.sync_attr = SYNCATTR_SYNC;
				mi2526 &= ~(1ULL<<XC6_ML_SYNC);
			}
			if (mi2526 & (1ULL<<XC6_ML_CE_USED)) {
				cfg_ml.ce_used = 1;
				mi2526 &= ~(1ULL<<XC6_ML_CE_USED);
			}
			if (mi2526 & (1ULL<<XC6_X_D_FFMUX_X)) {
				cfg_x.a2d[LUT_D].ff_mux = MUX_X;
				mi2526 &= ~(1ULL<<XC6_X_D_FFMUX_X);
			}
			if (mi2526 & (1ULL<<XC6_X_C_FFMUX_X)) {
				cfg_x.a2d[LUT_C].ff_mux = MUX_X;
				mi2526 &= ~(1ULL<<XC6_X_C_FFMUX_X);
			}
			if (mi2526 & (1ULL<<XC6_X_CE_USED)) {
				cfg_x.ce_used = 1;
				mi2526 &= ~(1ULL<<XC6_X_CE_USED);
			}
			if (mi2526 & XC6_ML_C_OUTMUX_MASK) {
				switch ((mi2526 & XC6_ML_C_OUTMUX_MASK) >> XC6_ML_C_OUTMUX_O) {
					case XC6_ML_C_OUTMUX_XOR:
						cfg_ml.a2d[LUT_C].out_mux = MUX_XOR;
						break;
					case XC6_ML_C_OUTMUX_O6:
						cfg_ml.a2d[LUT_C].out_mux = MUX_O6;
						break;
					case XC6_ML_C_OUTMUX_5Q:
						cfg_ml.a2d[LUT_C].out_mux = MUX_5Q;
						break;
					case XC6_ML_C_OUTMUX_CY:
						cfg_ml.a2d[LUT_C].out_mux = MUX_CY;
						break;
					case XC6_ML_C_OUTMUX_O5:
						cfg_ml.a2d[LUT_C].out_mux = MUX_O5;
						break;
					case XC6_ML_C_OUTMUX_F7:
						cfg_ml.a2d[LUT_C].out_mux = MUX_F7;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_C_OUTMUX_MASK;
			}
			if (mi2526 & XC6_ML_C_FFMUX_MASK) {
				switch ((mi2526 & XC6_ML_C_FFMUX_MASK) >> XC6_ML_C_FFMUX_O) {
					case XC6_ML_C_FFMUX_O5:
						cfg_ml.a2d[LUT_C].ff_mux = MUX_O5;
						break;
					case XC6_ML_C_FFMUX_X:
						cfg_ml.a2d[LUT_C].ff_mux = MUX_X;
						break;
					case XC6_ML_C_FFMUX_F7:
						cfg_ml.a2d[LUT_C].ff_mux = MUX_F7;
						break;
					case XC6_ML_C_FFMUX_XOR:
						cfg_ml.a2d[LUT_C].ff_mux = MUX_XOR;
						break;
					case XC6_ML_C_FFMUX_CY:
						cfg_ml.a2d[LUT_C].ff_mux = MUX_CY;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_C_FFMUX_MASK;
			}
			if (mi2526 & XC6_ML_B_OUTMUX_MASK) {
				switch ((mi2526 & XC6_ML_B_OUTMUX_MASK) >> XC6_ML_B_OUTMUX_O) {
					case XC6_ML_B_OUTMUX_5Q:
						cfg_ml.a2d[LUT_B].out_mux = MUX_5Q;
						break;
					case XC6_ML_B_OUTMUX_F8:
						cfg_ml.a2d[LUT_B].out_mux = MUX_F8;
						break;
					case XC6_ML_B_OUTMUX_XOR:
						cfg_ml.a2d[LUT_B].out_mux = MUX_XOR;
						break;
					case XC6_ML_B_OUTMUX_CY:
						cfg_ml.a2d[LUT_B].out_mux = MUX_CY;
						break;
					case XC6_ML_B_OUTMUX_O6:
						cfg_ml.a2d[LUT_B].out_mux = MUX_O6;
						break;
					case XC6_ML_B_OUTMUX_O5:
						cfg_ml.a2d[LUT_B].out_mux = MUX_O5;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_B_OUTMUX_MASK;
			}
			if (mi2526 & (1ULL<<XC6_X_B_FFMUX_X)) {
				cfg_x.a2d[LUT_B].ff_mux = MUX_X;
				mi2526 &= ~(1ULL<<XC6_X_B_FFMUX_X);
			}
			if (mi2526 & (1ULL<<XC6_X_A_FFMUX_X)) {
				cfg_x.a2d[LUT_A].ff_mux = MUX_X;
				mi2526 &= ~(1ULL<<XC6_X_A_FFMUX_X);
			}
			if (mi2526 & (1ULL<<XC6_X_B_FFSRINIT_1)) {
				cfg_x.a2d[LUT_B].ff_srinit = FF_SRINIT1;
				mi2526 &= ~(1ULL<<XC6_X_B_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_X_A_OUTMUX_O5)) {
				cfg_x.a2d[LUT_A].out_mux = MUX_O5;
				mi2526 &= ~(1ULL<<XC6_X_A_OUTMUX_O5);
			}
			if (mi2526 & (1ULL<<XC6_X_SR_USED)) {
				cfg_x.sr_used = 1;
				mi2526 &= ~(1ULL<<XC6_X_SR_USED);
			}
			if (mi2526 & (1ULL<<XC6_X_SYNC)) {
				cfg_x.sync_attr = SYNCATTR_SYNC;
				mi2526 &= ~(1ULL<<XC6_X_SYNC);
			}
			if (mi2526 & (1ULL<<XC6_X_ALL_LATCH)) {
				latch_x = 1;
				mi2526 &= ~(1ULL<<XC6_X_ALL_LATCH);
			}
			if (mi2526 & (1ULL<<XC6_ML_CLK_B)) {
				cfg_ml.clk_inv = CLKINV_B;
				mi2526 &= ~(1ULL<<XC6_ML_CLK_B);
			}
			if (mi2526 & XC6_ML_B_FFMUX_MASK) {
				switch ((mi2526 & XC6_ML_B_FFMUX_MASK) >> XC6_ML_B_FFMUX_O) {
					case XC6_ML_B_FFMUX_XOR:
						cfg_ml.a2d[LUT_B].ff_mux = MUX_XOR;
						break;
					case XC6_ML_B_FFMUX_O5:
						cfg_ml.a2d[LUT_B].ff_mux = MUX_O5;
						break;
					case XC6_ML_B_FFMUX_CY:
						cfg_ml.a2d[LUT_B].ff_mux = MUX_CY;
						break;
					case XC6_ML_B_FFMUX_X:
						cfg_ml.a2d[LUT_B].ff_mux = MUX_X;
						break;
					case XC6_ML_B_FFMUX_F8:
						cfg_ml.a2d[LUT_B].ff_mux = MUX_F8;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_B_FFMUX_MASK;
			}
			if (mi2526 & XC6_ML_A_FFMUX_MASK) {
				switch ((mi2526 & XC6_ML_A_FFMUX_MASK) >> XC6_ML_A_FFMUX_O) {
					case XC6_ML_A_FFMUX_XOR:
						cfg_ml.a2d[LUT_A].ff_mux = MUX_XOR;
						break;
					case XC6_ML_A_FFMUX_X:
						cfg_ml.a2d[LUT_A].ff_mux = MUX_X;
						break;
					case XC6_ML_A_FFMUX_O5:
						cfg_ml.a2d[LUT_A].ff_mux = MUX_O5;
						break;
					case XC6_ML_A_FFMUX_CY:
						cfg_ml.a2d[LUT_A].ff_mux = MUX_CY;
						break;
					case XC6_ML_A_FFMUX_F7:
						cfg_ml.a2d[LUT_A].ff_mux = MUX_F7;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_A_FFMUX_MASK;
			}
			if (mi2526 & XC6_ML_A_OUTMUX_MASK) {
				switch ((mi2526 & XC6_ML_A_OUTMUX_MASK) >> XC6_ML_A_OUTMUX_O) {
					case XC6_ML_A_OUTMUX_5Q:
						cfg_ml.a2d[LUT_A].out_mux = MUX_5Q;
						break;
					case XC6_ML_A_OUTMUX_F7:
						cfg_ml.a2d[LUT_A].out_mux = MUX_F7;
						break;
					case XC6_ML_A_OUTMUX_XOR:
						cfg_ml.a2d[LUT_A].out_mux = MUX_XOR;
						break;
					case XC6_ML_A_OUTMUX_CY:
						cfg_ml.a2d[LUT_A].out_mux = MUX_CY;
						break;
					case XC6_ML_A_OUTMUX_O6:
						cfg_ml.a2d[LUT_A].out_mux = MUX_O6;
						break;
					case XC6_ML_A_OUTMUX_O5:
						cfg_ml.a2d[LUT_A].out_mux = MUX_O5;
						break;
					default: HERE(); continue;
				}
				mi2526 &= ~XC6_ML_A_OUTMUX_MASK;
			}
			if (mi2526 & (1ULL<<XC6_ML_B_CY0_O5)) {
				cfg_ml.a2d[LUT_B].cy0 = CY0_O5;
				mi2526 &= ~(1ULL<<XC6_ML_B_CY0_O5);
			}
			if (mi2526 & (1ULL<<XC6_ML_PRECYINIT_AX)) {
				cfg_ml.precyinit = PRECYINIT_AX;
				mi2526 &= ~(1ULL<<XC6_ML_PRECYINIT_AX);
			}
			if (mi2526 & (1ULL<<XC6_X_A_FFSRINIT_1)) {
				cfg_x.a2d[LUT_A].ff_srinit = FF_SRINIT1;
				mi2526 &= ~(1ULL<<XC6_X_A_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_ML_PRECYINIT_1)) {
				cfg_ml.precyinit = PRECYINIT_1;
				mi2526 &= ~(1ULL<<XC6_ML_PRECYINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_ML_B_FFSRINIT_1)) {
				cfg_ml.a2d[LUT_B].ff_srinit = FF_SRINIT1;
				mi2526 &= ~(1ULL<<XC6_ML_B_FFSRINIT_1);
			}
			if (mi2526 & (1ULL<<XC6_ML_A_CY0_O5)) {
				cfg_ml.a2d[LUT_A].cy0 = CY0_O5;
				mi2526 &= ~(1ULL<<XC6_ML_A_CY0_O5);
			}
			if (mi2526 & (1ULL<<XC6_L_A_FFSRINIT_1)) {
				if (!l_col) {
					HERE();
					continue;
				}
				cfg_ml.a2d[LUT_A].ff_srinit = FF_SRINIT1;
				mi2526 &= ~(1ULL<<XC6_L_A_FFSRINIT_1);
			}

			//
			// Step 4:
			//
			// If any bits remain in the configuration, do not
			// instantiate the logic devices.
			//

		   	if (mi20) {
				fprintf(stderr, "#E %s:%i y%02i x%02i l%i "
				  "mi20 0x%016lX\n",
				  __FILE__, __LINE__, y, x, l_col, mi20);
				continue;
			}
		   	if (mi23_M) {
				fprintf(stderr, "#E %s:%i y%02i x%02i l%i "
				  "mi23_M 0x%016lX\n",
				  __FILE__, __LINE__, y, x, l_col, mi23_M);
				continue;
			}
		   	if (mi2526) {
				fprintf(stderr, "#E %s:%i y%02i x%02i l%i "
				  "mi2526 0x%016lX\n",
				  __FILE__, __LINE__, y, x, l_col, mi2526);
				continue;
			}

			//
			// Step 5:
			//
			// Do device-global sanity checking pre-LUT
			//

			// cfg_ml.cout_used
			dev_ml = fdev_p(es->model, y, x, DEV_LOGIC, DEV_LOG_M_OR_L);
			if (!dev_ml) FAIL(EINVAL);
			if (find_es_switch(es, y, x, fpga_switch_first(
				es->model, y, x, dev_ml->pinw[LO_COUT], SW_FROM)))
				cfg_ml.cout_used = 1;

// todo: if srinit=1, the matching ff/latch must be on
// todo: handle all_latch
// todo: precyinit=0 has no bits
// todo: cy0=X has no bits, check connectivity
// todo: the x-device ffmux must not be left as MUX_X unless the ff is really in use

			//
			// Step 6:
			//
			// Determine lut5_usage and read the luts.
			//

// todo: in ML, if srinit=0 cannot decide between ff_mux=O6 or
//    direct-out, need to check pin connectivity, is_pin_connected()
// todo: in ML devs, ffmux=O6 has no bits set
// todo: 5Q-ff in X devices

			// ML-A
			if (lut_ML[LUT_A]
			    || !all_zero(&cfg_ml.a2d[LUT_A], sizeof(cfg_ml.a2d[LUT_A]))) {
				if (lut_ML[LUT_A]
				    && cfg_ml.a2d[LUT_A].out_mux != MUX_O6
				    && cfg_ml.a2d[LUT_A].out_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_A].out_mux != MUX_CY
				    && cfg_ml.a2d[LUT_A].out_mux != MUX_F7
				    && cfg_ml.a2d[LUT_A].ff_mux != MUX_O6
				    && cfg_ml.a2d[LUT_A].ff_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_A].ff_mux != MUX_CY
				    && cfg_ml.a2d[LUT_A].ff_mux != MUX_F7)
					cfg_ml.a2d[LUT_A].out_used = 1;

				lut5_used = (cfg_ml.a2d[LUT_A].ff_mux == MUX_O5
					|| cfg_ml.a2d[LUT_A].out_mux == MUX_5Q
					|| cfg_ml.a2d[LUT_A].out_mux == MUX_O5
					|| cfg_ml.a2d[LUT_A].cy0 == CY0_O5);
				rc = lut2str(lut_ML[LUT_A], l_col
					  ? XC6_LMAP_XL_L_A : XC6_LMAP_XM_M_A,
					lut5_used,
					lut6_ml[LUT_A], &cfg_ml.a2d[LUT_A].lut6,
					lut5_ml[LUT_A], &cfg_ml.a2d[LUT_A].lut5);
				if (rc) FAIL(rc);
			}
			// ML-B
			if (lut_ML[LUT_B]
			    || !all_zero(&cfg_ml.a2d[LUT_B], sizeof(cfg_ml.a2d[LUT_B]))) {
				if (lut_ML[LUT_B]
				    && cfg_ml.a2d[LUT_B].out_mux != MUX_O6
				    && cfg_ml.a2d[LUT_B].out_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_B].out_mux != MUX_CY
				    && cfg_ml.a2d[LUT_B].out_mux != MUX_F7
				    && cfg_ml.a2d[LUT_B].ff_mux != MUX_O6
				    && cfg_ml.a2d[LUT_B].ff_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_B].ff_mux != MUX_CY
				    && cfg_ml.a2d[LUT_B].ff_mux != MUX_F7)
					cfg_ml.a2d[LUT_B].out_used = 1;

				lut5_used = (cfg_ml.a2d[LUT_B].ff_mux == MUX_O5
					|| cfg_ml.a2d[LUT_B].out_mux == MUX_5Q
					|| cfg_ml.a2d[LUT_B].out_mux == MUX_O5
					|| cfg_ml.a2d[LUT_B].cy0 == CY0_O5);
				rc = lut2str(lut_ML[LUT_B], l_col
					  ? XC6_LMAP_XL_L_B : XC6_LMAP_XM_M_B,
					lut5_used,
					lut6_ml[LUT_B], &cfg_ml.a2d[LUT_B].lut6,
					lut5_ml[LUT_B], &cfg_ml.a2d[LUT_B].lut5);
				if (rc) FAIL(rc);
			}
			// ML-C
			if (lut_ML[LUT_C]
			    || !all_zero(&cfg_ml.a2d[LUT_C], sizeof(cfg_ml.a2d[LUT_C]))) {
				if (lut_ML[LUT_C]
				    && cfg_ml.a2d[LUT_C].out_mux != MUX_O6
				    && cfg_ml.a2d[LUT_C].out_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_C].out_mux != MUX_CY
				    && cfg_ml.a2d[LUT_C].out_mux != MUX_F7
				    && cfg_ml.a2d[LUT_C].ff_mux != MUX_O6
				    && cfg_ml.a2d[LUT_C].ff_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_C].ff_mux != MUX_CY
				    && cfg_ml.a2d[LUT_C].ff_mux != MUX_F7)
					cfg_ml.a2d[LUT_C].out_used = 1;

				lut5_used = (cfg_ml.a2d[LUT_C].ff_mux == MUX_O5
					|| cfg_ml.a2d[LUT_C].out_mux == MUX_5Q
					|| cfg_ml.a2d[LUT_C].out_mux == MUX_O5
					|| cfg_ml.a2d[LUT_C].cy0 == CY0_O5);
				rc = lut2str(lut_ML[LUT_C], l_col
					  ? XC6_LMAP_XL_L_C : XC6_LMAP_XM_M_C,
					lut5_used,
					lut6_ml[LUT_C], &cfg_ml.a2d[LUT_C].lut6,
					lut5_ml[LUT_C], &cfg_ml.a2d[LUT_C].lut5);
				if (rc) FAIL(rc);
			}
			// ML-D
			if (lut_ML[LUT_D]
			    || !all_zero(&cfg_ml.a2d[LUT_D], sizeof(cfg_ml.a2d[LUT_D]))) {
				if (lut_ML[LUT_D]
				    && cfg_ml.a2d[LUT_D].out_mux != MUX_O6
				    && cfg_ml.a2d[LUT_D].out_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_D].out_mux != MUX_CY
				    && cfg_ml.a2d[LUT_D].out_mux != MUX_F7
				    && cfg_ml.a2d[LUT_D].ff_mux != MUX_O6
				    && cfg_ml.a2d[LUT_D].ff_mux != MUX_XOR
				    && cfg_ml.a2d[LUT_D].ff_mux != MUX_CY
				    && cfg_ml.a2d[LUT_D].ff_mux != MUX_F7)
					cfg_ml.a2d[LUT_D].out_used = 1;

				lut5_used = (cfg_ml.a2d[LUT_D].ff_mux == MUX_O5
					|| cfg_ml.a2d[LUT_D].out_mux == MUX_5Q
					|| cfg_ml.a2d[LUT_D].out_mux == MUX_O5
					|| cfg_ml.a2d[LUT_D].cy0 == CY0_O5);
				rc = lut2str(lut_ML[LUT_D], l_col
					  ? XC6_LMAP_XL_L_D : XC6_LMAP_XM_M_D,
					lut5_used,
					lut6_ml[LUT_D], &cfg_ml.a2d[LUT_D].lut6,
					lut5_ml[LUT_D], &cfg_ml.a2d[LUT_D].lut5);
				if (rc) FAIL(rc);
			}
			// X-A
			if (lut_X[LUT_A]
			    || !all_zero(&cfg_x.a2d[LUT_A], sizeof(cfg_x.a2d[LUT_A]))) {
				if (lut_X[LUT_A]
				    && cfg_x.a2d[LUT_A].ff_mux != MUX_O6)
					cfg_x.a2d[LUT_A].out_used = 1;
				lut5_used = cfg_x.a2d[LUT_A].out_mux != 0;
				rc = lut2str(lut_X[LUT_A], l_col
					  ? XC6_LMAP_XL_X_A : XC6_LMAP_XM_X_A,
					lut5_used,
					lut6_x[LUT_A], &cfg_x.a2d[LUT_A].lut6,
					lut5_x[LUT_A], &cfg_x.a2d[LUT_A].lut5);
				if (rc) FAIL(rc);
			
			}
			// X-B
			if (lut_X[LUT_B]
			    || !all_zero(&cfg_x.a2d[LUT_B], sizeof(cfg_x.a2d[LUT_B]))) {
				if (lut_X[LUT_B]
				    && cfg_x.a2d[LUT_B].ff_mux != MUX_O6)
					cfg_x.a2d[LUT_B].out_used = 1;
				lut5_used = cfg_x.a2d[LUT_B].out_mux != 0;
				rc = lut2str(lut_X[LUT_B], l_col
					  ? XC6_LMAP_XL_X_B : XC6_LMAP_XM_X_B,
					lut5_used,
					lut6_x[LUT_B], &cfg_x.a2d[LUT_B].lut6,
					lut5_x[LUT_B], &cfg_x.a2d[LUT_B].lut5);
				if (rc) FAIL(rc);
			
			}
			// X-C
			if (lut_X[LUT_C]
			    || !all_zero(&cfg_x.a2d[LUT_C], sizeof(cfg_x.a2d[LUT_C]))) {
				if (lut_X[LUT_C]
				    && cfg_x.a2d[LUT_C].ff_mux != MUX_O6)
					cfg_x.a2d[LUT_C].out_used = 1;
				lut5_used = cfg_x.a2d[LUT_C].out_mux != 0;
				rc = lut2str(lut_X[LUT_C], l_col
					  ? XC6_LMAP_XL_X_C : XC6_LMAP_XM_X_C,
					lut5_used,
					lut6_x[LUT_C], &cfg_x.a2d[LUT_C].lut6,
					lut5_x[LUT_C], &cfg_x.a2d[LUT_C].lut5);
				if (rc) FAIL(rc);
			
			}
			// X-D
			if (lut_X[LUT_D]
			    || !all_zero(&cfg_x.a2d[LUT_D], sizeof(cfg_x.a2d[LUT_D]))) {
				if (lut_X[LUT_D]
				    && cfg_x.a2d[LUT_D].ff_mux != MUX_O6)
					cfg_x.a2d[LUT_D].out_used = 1;
				lut5_used = cfg_x.a2d[LUT_D].out_mux != 0;
				rc = lut2str(lut_X[LUT_D], l_col
					  ? XC6_LMAP_XL_X_D : XC6_LMAP_XM_X_D,
					lut5_used,
					lut6_x[LUT_D], &cfg_x.a2d[LUT_D].lut6,
					lut5_x[LUT_D], &cfg_x.a2d[LUT_D].lut5);
				if (rc) FAIL(rc);
			
			}

			//
			// Step 7:
			//
			// Do more and final sanity checks before instantiation.
			//

			// todo: latch cannot be combined with out_mux=5Q or ff5_srinit
			if (latch_ml
			    && !cfg_ml.a2d[LUT_A].ff_mux
			    && !cfg_ml.a2d[LUT_B].ff_mux
			    && !cfg_ml.a2d[LUT_C].ff_mux
			    && !cfg_ml.a2d[LUT_D].ff_mux) {
				HERE();
				continue;
			}
			if (latch_x
			    && !cfg_x.a2d[LUT_A].ff_mux
			    && !cfg_x.a2d[LUT_B].ff_mux
			    && !cfg_x.a2d[LUT_C].ff_mux
			    && !cfg_x.a2d[LUT_D].ff_mux) {
				HERE();
				continue;
			}
			// todo: latch and2l and or2l need to be determined
			//       from vcc connectivity, srinit etc.
			for (lut = LUT_A; lut <= LUT_D; lut++) {
				if (cfg_ml.a2d[lut].ff_mux) {
					cfg_ml.a2d[lut].ff = latch_ml ? FF_LATCH : FF_FF;
					if (!cfg_ml.a2d[lut].ff_srinit)
						cfg_ml.a2d[lut].ff_srinit = FF_SRINIT0;
					if (!cfg_ml.clk_inv)
						cfg_ml.clk_inv = CLKINV_CLK;
					if (!cfg_ml.sync_attr)
						cfg_ml.sync_attr = SYNCATTR_ASYNC;
				}
				if (cfg_x.a2d[lut].ff_mux) {
					cfg_x.a2d[lut].ff = latch_x ? FF_LATCH : FF_FF;
					if (!cfg_x.a2d[lut].ff_srinit)
						cfg_x.a2d[lut].ff_srinit = FF_SRINIT0;
					if (!cfg_x.clk_inv)
						cfg_x.clk_inv = CLKINV_CLK;
					if (!cfg_x.sync_attr)
						cfg_x.sync_attr = SYNCATTR_ASYNC;
				}
				// todo: ff5_srinit
				// todo: 5Q ff also needs clock/sync check
			}

			// Check whether we should default to PRECYINIT_0
			if (!cfg_ml.precyinit
			    && (cfg_ml.a2d[LUT_A].out_mux == MUX_XOR
				|| cfg_ml.a2d[LUT_A].ff_mux == MUX_XOR
				|| cfg_ml.a2d[LUT_A].cy0)) {
				int connpt_dests_o, num_dests, cout_y, cout_x;
				str16_t cout_str;

				if ((fpga_connpt_find(es->model, y, x,
					dev_ml->pinw[LI_CIN], &connpt_dests_o,
					&num_dests) == NO_CONN)
				    || num_dests != 1) {
					HERE();
				} else {
					fpga_conn_dest(es->model, y, x, connpt_dests_o,
						&cout_y, &cout_x, &cout_str);
					if (find_es_switch(es, cout_y, cout_x,fpga_switch_first(
						es->model, cout_y, cout_x, cout_str, SW_TO)))
						cfg_ml.precyinit = PRECYINIT_0;
				}
			}

			//
			// Step 8:
			//
			// Remove all bits.
			//
			frame_set_u64(u8_p + 20*FRAME_SIZE + byte_off,
				frame_get_u64(u8_p + 20*FRAME_SIZE + byte_off)
					& ~XC6_MI20_LOGIC_MASK);
			last_minor = l_col ? 29 : 30;
			for (i = 21; i <= last_minor; i++)
				frame_set_u64(u8_p + i*FRAME_SIZE + byte_off, 0);
		
			//
			// Step 9:
			//
			// Instantiate configuration.
			//

			if (!all_zero(&cfg_ml, sizeof(cfg_ml))) {
				rc = fdev_logic_setconf(es->model, y, x, DEV_LOG_M_OR_L, &cfg_ml);
				if (rc) FAIL(rc);
			}
			if (!all_zero(&cfg_x, sizeof(cfg_x))) {
				rc = fdev_logic_setconf(es->model, y, x, DEV_LOG_X, &cfg_x);
				if (rc) FAIL(rc);
			}
		}
	}
	return 0;
fail:
	return rc;
}

static int bitpos_is_set(struct extract_state* es, int y, int x,
	struct xc6_routing_bitpos* swpos, int* is_set)
{
	int row_num, row_pos, start_in_frame, two_bits_val, rc;

	*is_set = 0;
	is_in_row(es->model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	if (swpos->minor == 20) {
		two_bits_val = ((get_bit(es->bits, row_num, es->model->x_major[x],
			20, start_in_frame + swpos->two_bits_o) != 0) << 1)
			| ((get_bit(es->bits, row_num, es->model->x_major[x],
			20, start_in_frame + swpos->two_bits_o+1) != 0) << 0);
		if (two_bits_val != swpos->two_bits_val)
			return 0;

		if (!get_bit(es->bits, row_num, es->model->x_major[x], 20,
			start_in_frame + swpos->one_bit_o))
			return 0;
	} else {
		two_bits_val = 
			((get_bit(es->bits, row_num, es->model->x_major[x], swpos->minor,
				start_in_frame + swpos->two_bits_o/2) != 0) << 1)
			| ((get_bit(es->bits, row_num, es->model->x_major[x], swpos->minor+1,
			    start_in_frame + swpos->two_bits_o/2) != 0) << 0);
		if (two_bits_val != swpos->two_bits_val)
			return 0;

		if (!get_bit(es->bits, row_num, es->model->x_major[x],
			swpos->minor + (swpos->one_bit_o&1),
			start_in_frame + swpos->one_bit_o/2))
			return 0;
	}
	*is_set = 1;
	return 0;
fail:
	return rc;
}

static int bitpos_clear_bits(struct extract_state* es, int y, int x,
	struct xc6_routing_bitpos* swpos)
{
	int row_num, row_pos, start_in_frame, rc;

	is_in_row(es->model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	if (swpos->minor == 20) {
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor,
			start_in_frame + swpos->two_bits_o);
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor,
			start_in_frame + swpos->two_bits_o+1);
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor,
			start_in_frame + swpos->one_bit_o);
	} else {
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor,
			start_in_frame + swpos->two_bits_o/2);
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor + 1,
			start_in_frame + swpos->two_bits_o/2);
		clear_bit(es->bits, row_num, es->model->x_major[x], swpos->minor + (swpos->one_bit_o&1),
			start_in_frame + swpos->one_bit_o/2);
	}
	return 0;
fail:
	return rc;
}

static int bitpos_set_bits(struct fpga_bits* bits, struct fpga_model* model,
	int y, int x, struct xc6_routing_bitpos* swpos)
{
	int row_num, row_pos, start_in_frame, rc;

	is_in_row(model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	if (swpos->minor == 20) {
		if (swpos->two_bits_val & 0x02)
			set_bit(bits, row_num, model->x_major[x], swpos->minor,
				start_in_frame + swpos->two_bits_o);
		if (swpos->two_bits_val & 0x01)
			set_bit(bits, row_num, model->x_major[x], swpos->minor,
				start_in_frame + swpos->two_bits_o+1);
		set_bit(bits, row_num, model->x_major[x], swpos->minor,
			start_in_frame + swpos->one_bit_o);
	} else {
		if (swpos->two_bits_val & 0x02)
			set_bit(bits, row_num, model->x_major[x], swpos->minor,
				start_in_frame + swpos->two_bits_o/2);
		if (swpos->two_bits_val & 0x01)
			set_bit(bits, row_num, model->x_major[x], swpos->minor + 1,
				start_in_frame + swpos->two_bits_o/2);
		set_bit(bits, row_num, model->x_major[x], swpos->minor + (swpos->one_bit_o&1),
			start_in_frame + swpos->one_bit_o/2);
	}
	return 0;
fail:
	return rc;
}

static int extract_routing_switches(struct extract_state* es, int y, int x)
{
	struct fpga_tile* tile;
	swidx_t sw_idx;
	int i, is_set, rc;

	tile = YX_TILE(es->model, y, x);

	for (i = 0; i < es->model->num_bitpos; i++) {
		rc = bitpos_is_set(es, y, x, &es->model->sw_bitpos[i], &is_set);
		if (rc) FAIL(rc);
		if (!is_set) continue;

		sw_idx = fpga_switch_lookup(es->model, y, x,
			fpga_wire2str_i(es->model, es->model->sw_bitpos[i].from),
			fpga_wire2str_i(es->model, es->model->sw_bitpos[i].to));
		if (sw_idx == NO_SWITCH) FAIL(EINVAL);
		// todo: es->model->sw_bitpos[i].bidir handling

		if (tile->switches[sw_idx] & SWITCH_BIDIRECTIONAL)
			HERE();
		if (tile->switches[sw_idx] & SWITCH_USED)
			HERE();
		if (es->num_yx_pos >= MAX_YX_SWITCHES)
			{ FAIL(ENOTSUP); }
		es->yx_pos[es->num_yx_pos].y = y;
		es->yx_pos[es->num_yx_pos].x = x;
		es->yx_pos[es->num_yx_pos].idx = sw_idx;
		es->num_yx_pos++;
		rc = bitpos_clear_bits(es, y, x, &es->model->sw_bitpos[i]);
		if (rc) FAIL(rc);
	}
	return 0;
fail:
	return rc;
}

static int extract_logic_switches(struct extract_state* es, int y, int x)
{
	int row, row_pos, byte_off, minor, rc;
	uint8_t* u8_p;

	row = which_row(y, es->model);
	row_pos = pos_in_row(y, es->model);
	if (row == -1 || row_pos == -1 || row_pos == 8) FAIL(EINVAL);
	if (row_pos > 8) row_pos--;
	u8_p = get_first_minor(es->bits, row, es->model->x_major[x]);
	byte_off = row_pos * 8;
	if (row_pos >= 8) byte_off += HCLK_BYTES;

	if (has_device_type(es->model, y, x, DEV_LOGIC, LOGIC_M))
		minor = 26;
	else if (has_device_type(es->model, y, x, DEV_LOGIC, LOGIC_L))
		minor = 25;
	else
		FAIL(EINVAL);

	if (frame_get_bit(u8_p + minor*FRAME_SIZE, byte_off*8 + XC6_ML_CIN_USED)) {
		struct fpga_device* dev;
		int connpt_dests_o, num_dests, cout_y, cout_x;
		str16_t cout_str;
		swidx_t cout_sw;

		dev = fdev_p(es->model, y, x, DEV_LOGIC, DEV_LOG_M_OR_L);
		if (!dev) FAIL(EINVAL);

		if ((fpga_connpt_find(es->model, y, x,
			dev->pinw[LI_CIN], &connpt_dests_o,
			&num_dests) == NO_CONN)
		    || num_dests != 1) {
			HERE();
		} else {
			fpga_conn_dest(es->model, y, x, connpt_dests_o,
				&cout_y, &cout_x, &cout_str);
			cout_sw = fpga_switch_first(es->model, cout_y,
				cout_x, cout_str, SW_TO);
			if (cout_sw == NO_SWITCH) HERE();
			else {
				if (es->num_yx_pos >= MAX_YX_SWITCHES)
					{ FAIL(ENOTSUP); }
				es->yx_pos[es->num_yx_pos].y = cout_y;
				es->yx_pos[es->num_yx_pos].x = cout_x;
				es->yx_pos[es->num_yx_pos].idx = cout_sw;
				es->num_yx_pos++;

				frame_clear_bit(u8_p + minor*FRAME_SIZE,
					byte_off*8 + XC6_ML_CIN_USED);
			}
		}
	}
	return 0;
fail:
	return rc;
}

#define MAX_IOLOGIC_SWBLOCK	4
#define MAX_IOLOGIC_BITS	4
struct iologic_sw_pos
{
	const char* to[MAX_IOLOGIC_SWBLOCK];
	const char* from[MAX_IOLOGIC_SWBLOCK];
	int minor[MAX_IOLOGIC_BITS];
	int b64[MAX_IOLOGIC_BITS];
};

// todo: can we assume the maximum range for iologic
//       minors to be 21..29? that would be a total of
//	 9*64=675 bits for ilogic/ologic/iodelay?
struct iologic_sw_pos s_left_io_swpos[] = { {{0}} };
struct iologic_sw_pos s_right_io_swpos[] = { {{0}} };
struct iologic_sw_pos s_top_outer_io_swpos[] = { {{0}} };
struct iologic_sw_pos s_top_inner_io_swpos[] = { {{0}} };

struct iologic_sw_pos s_bot_inner_io_swpos[] =
{
	// input
	{{"D_ILOGIC_IDATAIN_IODELAY_S"},
	 {"BIOI_INNER_IBUF0"},
 	 {23, 23, -1},
	 {28, 29}},

	{{"D_ILOGIC_SITE"},
	 {"D_ILOGIC_IDATAIN_IODELAY"},
	 {26, -1},
	 {20}},

	{{"D_ILOGIC_SITE_S"},
	 {"D_ILOGIC_IDATAIN_IODELAY_S"},
	 {26, -1},
	 {23}},

	{{"DFB_ILOGIC_SITE"},
	 {"D_ILOGIC_SITE"},
	 {28, -1},
	 {63}},

	{{"DFB_ILOGIC_SITE_S"},
	 {"D_ILOGIC_SITE_S"},
	 {28, -1},
	 {0}},

	{{"FABRICOUT_ILOGIC_SITE"},
	 {"D_ILOGIC_SITE"},
	 {29, -1},
	 {49}},

	{{"FABRICOUT_ILOGIC_SITE_S"},
	 {"D_ILOGIC_SITE_S"},
	 {29, -1},
	 {14}},

	// output
	{{"OQ_OLOGIC_SITE", "BIOI_INNER_O0"},
	 {"D1_OLOGIC_SITE", "OQ_OLOGIC_SITE"},
	 {26, 27, 28, -1},
	 {40, 21, 57}},

	{{"OQ_OLOGIC_SITE_S", "BIOI_INNER_O1"},
	 {"D1_OLOGIC_SITE_S", "OQ_OLOGIC_SITE_S"},
	 {26, 27, 28, -1},
	 {43, 56, 6}},

	{{"IOI_LOGICOUT0"}, {"IOI_INTER_LOGICOUT0"}, {-1}},
	{{"IOI_LOGICOUT7"}, {"IOI_INTER_LOGICOUT7"}, {-1}},
	{{"IOI_INTER_LOGICOUT0"}, {"FABRICOUT_ILOGIC_SITE"}, {-1}},
	{{"IOI_INTER_LOGICOUT7"}, {"FABRICOUT_ILOGIC_SITE_S"}, {-1}},
	{{"D_ILOGIC_IDATAIN_IODELAY"}, {"BIOI_INNER_IBUF0"}, {-1}},
	{{"D_ILOGIC_IDATAIN_IODELAY_S"}, {"BIOI_INNER_IBUF1"}, {-1}},
	{{"D1_OLOGIC_SITE"}, {"IOI_LOGICINB31"}, {-1}},
	{{0}}
};

struct iologic_sw_pos s_bot_outer_io_swpos[] =
{
	// input
	{{"D_ILOGIC_IDATAIN_IODELAY_S"},
	 {"BIOI_OUTER_IBUF0"},
 	 {23, 23, -1},
	 {28, 29}},

	{{"D_ILOGIC_SITE"},
	 {"D_ILOGIC_IDATAIN_IODELAY"},
	 {26, -1},
	 {20}},

	{{"D_ILOGIC_SITE_S"},
	 {"D_ILOGIC_IDATAIN_IODELAY_S"},
	 {26, -1},
	 {23}},

	{{"DFB_ILOGIC_SITE"},
	 {"D_ILOGIC_SITE"},
	 {28, -1},
	 {63}},

	{{"DFB_ILOGIC_SITE_S"},
	 {"D_ILOGIC_SITE_S"},
	 {28, -1},
	 {0}},

	{{"FABRICOUT_ILOGIC_SITE"},
	 {"D_ILOGIC_SITE"},
	 {29, -1},
	 {49}},

	{{"FABRICOUT_ILOGIC_SITE_S"},
	 {"D_ILOGIC_SITE_S"},
	 {29, -1},
	 {14}},

	// output
	{{"OQ_OLOGIC_SITE", "BIOI_OUTER_O0"},
	 {"D1_OLOGIC_SITE", "OQ_OLOGIC_SITE"},
	 {26, 27, 28, -1},
	 {40, 21, 57}},

	{{"OQ_OLOGIC_SITE_S", "BIOI_OUTER_O1"},
	 {"D1_OLOGIC_SITE_S", "OQ_OLOGIC_SITE_S"},
	 {26, 27, 28, -1},
	 {43, 56, 6}},

	{{"IOI_LOGICOUT0"}, {"IOI_INTER_LOGICOUT0"}, {-1}},
	{{"IOI_LOGICOUT7"}, {"IOI_INTER_LOGICOUT7"}, {-1}},
	{{"IOI_INTER_LOGICOUT0"}, {"FABRICOUT_ILOGIC_SITE"}, {-1}},
	{{"IOI_INTER_LOGICOUT7"}, {"FABRICOUT_ILOGIC_SITE_S"}, {-1}},
	{{"D_ILOGIC_IDATAIN_IODELAY"}, {"BIOI_INNER_IBUF0"}, {-1}},
	{{"D_ILOGIC_IDATAIN_IODELAY_S"}, {"BIOI_INNER_IBUF1"}, {-1}},
	{{"D1_OLOGIC_SITE"}, {"IOI_LOGICINB31"}, {-1}},
	{{0}}
};

static int extract_iologic_switches(struct extract_state* es, int y, int x)
{
	int row_num, row_pos, bit_in_frame, i, j, rc;
	uint8_t* minor0_p;
	struct iologic_sw_pos* sw_pos;
	str16_t from_str_i, to_str_i;
	swidx_t sw_idx;

	// From y/x coordinate, determine major, row, bit offset
	// in frame (v64_i) and pointer to first minor.
	is_in_row(es->model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		bit_in_frame = (row_pos-1)*64 + XC6_HCLK_BITS;
	else
		bit_in_frame = row_pos*64;
	minor0_p = get_first_minor(es->bits, row_num, es->model->x_major[x]);

	if (x < LEFT_SIDE_WIDTH) {
		if (x != LEFT_IO_DEVS) FAIL(EINVAL);
		sw_pos = s_left_io_swpos;
	} else if (x >= es->model->x_width-RIGHT_SIDE_WIDTH) {
		if (x != es->model->x_width - RIGHT_IO_DEVS_O) FAIL(EINVAL);
		sw_pos = s_right_io_swpos;
	} else if (y == TOP_OUTER_IO)
		sw_pos = s_top_outer_io_swpos;
	else if (y == TOP_INNER_IO)
		sw_pos = s_top_inner_io_swpos;
	else if (y == es->model->y_height-BOT_INNER_IO)
		sw_pos = s_bot_inner_io_swpos;
	else if (y == es->model->y_height-BOT_OUTER_IO)
		sw_pos = s_bot_outer_io_swpos;
	else FAIL(EINVAL);

	// Go through switches
	for (i = 0; sw_pos[i].to[0]; i++) {
		for (j = 0; sw_pos[i].minor[j] != -1; j++) {
			if (!frame_get_bit(&minor0_p[sw_pos[i].minor[j]
				*FRAME_SIZE], bit_in_frame+sw_pos[i].b64[j]))
				break;
		}
		if (!j || sw_pos[i].minor[j] != -1)
			continue;
		for (j = 0; j < MAX_IOLOGIC_SWBLOCK && sw_pos[i].to[j]; j++) {
			from_str_i = strarray_find(&es->model->str, sw_pos[i].from[j]);
			to_str_i = strarray_find(&es->model->str, sw_pos[i].to[j]);
			if (from_str_i == STRIDX_NO_ENTRY
			    || to_str_i == STRIDX_NO_ENTRY)
				FAIL(EINVAL);
			sw_idx = fpga_switch_lookup(es->model, y, x,
				from_str_i, to_str_i);
			if (sw_idx == NO_SWITCH) FAIL(EINVAL);

			if (es->num_yx_pos >= MAX_YX_SWITCHES)
				{ FAIL(ENOTSUP); }
			es->yx_pos[es->num_yx_pos].y = y;
			es->yx_pos[es->num_yx_pos].x = x;
			es->yx_pos[es->num_yx_pos].idx = sw_idx;
			es->num_yx_pos++;
		}
		for (j = 0; sw_pos[i].minor[j] != -1; j++)
			frame_clear_bit(&minor0_p[sw_pos[i].minor[j]
			  *FRAME_SIZE], bit_in_frame+sw_pos[i].b64[j]);
	}
	// todo: we could implement an all-or-nothing system where
	//       the device bits are first copied into an u64 array,
	//	 and switches rewound by resetting num_yx_pos - unless
	//	 all bits have been processed...
	return 0;
fail:
	return rc;
}

static int extract_switches(struct extract_state* es)
{
	int x, y, rc;

	for (x = 0; x < es->model->x_width; x++) {
		for (y = 0; y < es->model->y_height; y++) {
			// routing switches
			if (is_atx(X_ROUTING_COL, es->model, x)
			    && y >= TOP_IO_TILES
			    && y < es->model->y_height-BOT_IO_TILES
			    && !is_aty(Y_ROW_HORIZ_AXSYMM|Y_CHIP_HORIZ_REGS,
					es->model, y)) {
				rc = extract_routing_switches(es, y, x);
				if (rc) FAIL(rc);
			}
			// logic switches
			if (has_device(es->model, y, x, DEV_LOGIC)) {
				rc = extract_logic_switches(es, y, x);
				if (rc) FAIL(rc);
			}
			// iologic switches
			if (has_device(es->model, y, x, DEV_ILOGIC)) {
				rc = extract_iologic_switches(es, y, x);
				if (rc) FAIL(rc);
			}
		}
	}
	return 0;
fail:
	return rc;
}

static int construct_extract_state(struct extract_state* es,
	struct fpga_model* model)
{
	memset(es, 0, sizeof(*es));
	es->model = model;
	return 0;
}

int extract_model(struct fpga_model* model, struct fpga_bits* bits)
{
	struct extract_state es;
	net_idx_t net_idx;
	int i, rc;

	rc = construct_extract_state(&es, model);
	if (rc) FAIL(rc);
	es.bits = bits;
	for (i = 0; i < sizeof(s_default_bits)/sizeof(s_default_bits[0]); i++) {
		if (!get_bitp(bits, &s_default_bits[i]))
			FAIL(EINVAL);
		clear_bitp(bits, &s_default_bits[i]);
	}

	rc = extract_switches(&es);
	if (rc) FAIL(rc);
	rc = extract_iobs(&es);
	if (rc) FAIL(rc);
	rc = extract_logic(&es);
	if (rc) FAIL(rc);

	// turn switches into nets
	if (model->nets)
		HERE(); // should be empty here
	for (i = 0; i < es.num_yx_pos; i++) {
		rc = fnet_new(model, &net_idx);
		if (rc) FAIL(rc);
		rc = fnet_add_sw(model, net_idx, es.yx_pos[i].y,
			es.yx_pos[i].x, &es.yx_pos[i].idx, 1);
		if (rc) FAIL(rc);
	}
	return 0;
fail:
	return rc;
}

int printf_swbits(struct fpga_model* model)
{
	char bit_str[129];
	int i, j, width;

	for (i = 0; i < model->num_bitpos; i++) {

		width = (model->sw_bitpos[i].minor == 20) ? 64 : 128;
		for (j = 0; j < width; j++)
			bit_str[j] = '0';
		bit_str[j] = 0;

		if (model->sw_bitpos[i].two_bits_val & 2)
			bit_str[model->sw_bitpos[i].two_bits_o] = '1';
		if (model->sw_bitpos[i].two_bits_val & 1)
			bit_str[model->sw_bitpos[i].two_bits_o+1] = '1';
		bit_str[model->sw_bitpos[i].one_bit_o] = '1';
		printf("mi%02i %s %s %s %s\n", model->sw_bitpos[i].minor,
			fpga_wire2str(model->sw_bitpos[i].to),
			bit_str,
			fpga_wire2str(model->sw_bitpos[i].from),
			model->sw_bitpos[i].bidir ? "<->" : "->");
	}
	return 0;
}

static int find_bitpos(struct fpga_model* model, int y, int x, swidx_t sw)
{
	enum extra_wires from_w, to_w;
	const char* from_str, *to_str;
	int i;

	from_str = fpga_switch_str(model, y, x, sw, SW_FROM);
	to_str = fpga_switch_str(model, y, x, sw, SW_TO);
	from_w = fpga_str2wire(from_str);
	to_w = fpga_str2wire(to_str);

	if (from_w == NO_WIRE || to_w == NO_WIRE) {
		HERE();
		return -1;
	}
	for (i = 0; i < model->num_bitpos; i++) {
		if (model->sw_bitpos[i].from == from_w
		    && model->sw_bitpos[i].to == to_w)
			return i;
		if (model->sw_bitpos[i].bidir
		    && model->sw_bitpos[i].to == from_w
		    && model->sw_bitpos[i].from == to_w) {
			if (!fpga_switch_is_bidir(model, y, x, sw))
				HERE();
			return i;
		}
	}
	fprintf(stderr, "#E switch %s (%i) to %s (%i) not in model\n",
		from_str, from_w, to_str, to_w);
	return -1;
}

static int write_routing_sw(struct fpga_bits* bits, struct fpga_model* model, int y, int x)
{
	struct fpga_tile* tile;
	int i, bit_pos, rc;

	// go through enabled switches, lookup in sw_bitpos
	// and set bits
	tile = YX_TILE(model, y, x);
	for (i = 0; i < tile->num_switches; i++) {
		if (!(tile->switches[i] & SWITCH_USED))
			continue;
		bit_pos = find_bitpos(model, y, x, i);
		if (bit_pos == -1) {
			HERE();
			continue;
		}
		rc = bitpos_set_bits(bits, model, y, x,
			&model->sw_bitpos[bit_pos]);
		if (rc) FAIL(rc);
	}
	return 0;
fail:
	return rc;
}

struct str_sw
{
	const char* from_str;
	const char* to_str;
};

static int get_used_switches(struct fpga_model* model, int y, int x,
	struct str_sw** str_sw, int* num_sw)
{
	int i, num_used, rc;
	struct fpga_tile* tile;

	tile = YX_TILE(model, y, x);
	num_used = 0;
	for (i = 0; i < tile->num_switches; i++) {
		if (tile->switches[i] & SWITCH_USED)
			num_used++;
	}
	if (!num_used) {
		*num_sw = 0;
		*str_sw = 0;
		return 0;
	}
	*str_sw = malloc(num_used * sizeof(**str_sw));
	if (!(*str_sw)) FAIL(ENOMEM);
	*num_sw = 0;
	for (i = 0; i < tile->num_switches; i++) {
		if (!(tile->switches[i] & SWITCH_USED))
			continue;
		(*str_sw)[*num_sw].from_str =
			fpga_switch_str(model, y, x, i, SW_FROM);
		(*str_sw)[*num_sw].to_str =
			fpga_switch_str(model, y, x, i, SW_TO);
		(*num_sw)++;
	}
	return 0;
fail:
	return rc;
}

// returns the number of found to-from pairs from search_sw that
// were found in the str_sw array, but only if all of the to-from
// pairs in search_sw were found. The found array then contains
// the indices into str_sw were the matches were made.
// If not all to-from pairs were found (or none), returns 0.
static int find_switches(struct iologic_sw_pos* search_sw,
	struct str_sw* str_sw, int num_str_sw, int (*found)[MAX_IOLOGIC_SWBLOCK])
{
	int i, j;

	for (i = 0; i < MAX_IOLOGIC_SWBLOCK && search_sw->to[i]; i++) {
		for (j = 0; j < num_str_sw; j++) {
			if (!str_sw[j].to_str)
				continue;
			if (strcmp(str_sw[j].to_str, search_sw->to[i])
			    || strcmp(str_sw[j].from_str, search_sw->from[i]))
				continue;
			(*found)[i] = j;
			break;
		}
		if (j >= num_str_sw)
			return 0;
	}
	return i;
}

static int write_iologic_sw(struct fpga_bits* bits, struct fpga_model* model,
	int y, int x)
{
	int i, j, row_num, row_pos, start_in_frame, rc;
	struct iologic_sw_pos* sw_pos;
	struct str_sw* str_sw;
	int found_i[MAX_IOLOGIC_SWBLOCK];
	int num_sw, num_found;

	if (x < LEFT_SIDE_WIDTH) {
		if (x != LEFT_IO_DEVS) FAIL(EINVAL);
		sw_pos = s_left_io_swpos;
	} else if (x >= model->x_width-RIGHT_SIDE_WIDTH) {
		if (x != model->x_width - RIGHT_IO_DEVS_O) FAIL(EINVAL);
		sw_pos = s_right_io_swpos;
	} else if (y == TOP_OUTER_IO)
		sw_pos = s_top_outer_io_swpos;
	else if (y == TOP_INNER_IO)
		sw_pos = s_top_inner_io_swpos;
	else if (y == model->y_height-BOT_INNER_IO)
		sw_pos = s_bot_inner_io_swpos;
	else if (y == model->y_height-BOT_OUTER_IO)
		sw_pos = s_bot_outer_io_swpos;
	else FAIL(EINVAL);

	is_in_row(model, y, &row_num, &row_pos);
	if (row_num == -1 || row_pos == -1
	    || row_pos == HCLK_POS) FAIL(EINVAL);
	if (row_pos > HCLK_POS)
		start_in_frame = (row_pos-1)*64 + 16;
	else
		start_in_frame = row_pos*64;

	rc = get_used_switches(model, y, x, &str_sw, &num_sw);
	if (rc) FAIL(rc);

	for (i = 0; sw_pos[i].to[0]; i++) {
		if (!(num_found = find_switches(&sw_pos[i], str_sw,
			num_sw, &found_i))) continue;
		for (j = 0; sw_pos[i].minor[j] != -1; j++) {
			set_bit(bits, row_num, model->x_major[x],
				sw_pos[i].minor[j], start_in_frame
				+ sw_pos[i].b64[j]);
		}
		// remove switches from 'used' array
		for (j = 0; j < num_found; j++)
			str_sw[found_i[j]].to_str = 0;
	}

	// check whether all switches were found
	for (i = 0; i < num_sw; i++) {
		if (!str_sw[i].to_str)
			continue;
		fprintf(stderr, "#E %s:%i unsupported switch "
			"y%02i x%02i %s -> %s\n", __FILE__, __LINE__,
			y, x, str_sw[i].from_str, str_sw[i].to_str);
	}
	free(str_sw);
	return 0;
fail:
	return rc;
}

static int write_switches(struct fpga_bits* bits, struct fpga_model* model)
{
	struct fpga_tile* tile;
	int x, y, i, rc;

	for (x = 0; x < model->x_width; x++) {
		for (y = 0; y < model->y_height; y++) {
			if (is_atx(X_ROUTING_COL, model, x)
			    && y >= TOP_IO_TILES
			    && y < model->y_height-BOT_IO_TILES
			    && !is_aty(Y_ROW_HORIZ_AXSYMM|Y_CHIP_HORIZ_REGS,
					model, y)) {
				rc = write_routing_sw(bits, model, y, x);
				if (rc) FAIL(rc);
				continue;
			}
			if (is_atyx(YX_DEV_ILOGIC, model, y, x)) {
				rc = write_iologic_sw(bits, model, y, x);
				if (rc) FAIL(rc);
				continue;
			}
			// todo: there are probably switches in logic and
			//       iob that need bits, and switches in other
			//       tiles that need no bits...
			if (is_atyx(YX_DEV_LOGIC|YX_DEV_IOB, model, y, x))
				continue;
			tile = YX_TILE(model, y, x);
			for (i = 0; i < tile->num_switches; i++) {
				if (!(tile->switches[i] & SWITCH_USED))
					continue;
				fprintf(stderr, "#E %s:%i unsupported switch "
					"y%02i x%02i %s\n", __FILE__, __LINE__,
					y, x,
					fpga_switch_print(model, y, x, i));
			}
		}
	}
	return 0;
fail:
	return rc;
}

static int write_logic(struct fpga_bits* bits, struct fpga_model* model)
{
	int dev_idx, row, row_pos, xm_col, rc;
	int x, y, byte_off;
	struct fpga_device* dev;
	uint64_t u64;
	uint8_t* u8_p;

	for (x = LEFT_SIDE_WIDTH; x < model->x_width-RIGHT_SIDE_WIDTH; x++) {
		xm_col = is_atx(X_FABRIC_LOGIC_XM_COL, model, x);
		if (!xm_col && !is_atx(X_FABRIC_LOGIC_XL_COL, model, x))
			continue;

		for (y = TOP_IO_TILES; y < model->y_height - BOT_IO_TILES; y++) {
			if (!has_device(model, y, x, DEV_LOGIC))
				continue;

			row = which_row(y, model);
			row_pos = pos_in_row(y, model);
			if (row == -1 || row_pos == -1 || row_pos == 8) {
				HERE();
				continue;
			}
			if (row_pos > 8) row_pos--;
			u8_p = get_first_minor(bits, row, model->x_major[x]);
			byte_off = row_pos * 8;
			if (row_pos >= 8) byte_off += HCLK_BYTES;

			if (xm_col) {
				// X device
				dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOG_X);
				if (dev_idx == NO_DEV) FAIL(EINVAL);
				dev = FPGA_DEV(model, y, x, dev_idx);
				if (dev->instantiated) {
					u64 = 0x000000B000600086ULL;
					frame_set_u64(u8_p + 26*FRAME_SIZE + byte_off, u64);

					if (dev->u.logic.a2d[LUT_A].lut6
					    && dev->u.logic.a2d[LUT_A].lut6[0]) {
						HERE(); // not supported
					}
					if (dev->u.logic.a2d[LUT_B].lut6
					    && dev->u.logic.a2d[LUT_B].lut6[0]) {
						HERE(); // not supported
					}
					if (dev->u.logic.a2d[LUT_C].lut6
					    && dev->u.logic.a2d[LUT_C].lut6[0]) {
						HERE(); // not supported
					}
					if (dev->u.logic.a2d[LUT_D].lut6
					    && dev->u.logic.a2d[LUT_D].lut6[0]) {
						rc = parse_boolexpr(dev->u.logic.a2d[LUT_D].lut6, &u64);
						if (rc) FAIL(rc);
						write_lut64(u8_p + 29*FRAME_SIZE, byte_off*8, u64);
					}
				}

				// M device
				dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOG_M_OR_L);
				if (dev_idx == NO_DEV) FAIL(EINVAL);
				dev = FPGA_DEV(model, y, x, dev_idx);
				if (dev->instantiated) {
					HERE();
				}
			} else {
				// X device
				dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOG_X);
				if (dev_idx == NO_DEV) FAIL(EINVAL);
				dev = FPGA_DEV(model, y, x, dev_idx);
				if (dev->instantiated) {
					HERE();
				}

				// L device
				dev_idx = fpga_dev_idx(model, y, x, DEV_LOGIC, DEV_LOG_M_OR_L);
				if (dev_idx == NO_DEV) FAIL(EINVAL);
				dev = FPGA_DEV(model, y, x, dev_idx);
				if (dev->instantiated) {
					HERE();
				}
			}
		}
	}
	return 0;
fail:
	return rc;
}

int write_model(struct fpga_bits* bits, struct fpga_model* model)
{
	int i, rc;

	for (i = 0; i < sizeof(s_default_bits)/sizeof(s_default_bits[0]); i++)
		set_bitp(bits, &s_default_bits[i]);
	rc = write_switches(bits, model);
	if (rc) FAIL(rc);
	rc = write_iobs(bits, model);
	if (rc) FAIL(rc);
	rc = write_logic(bits, model);
	if (rc) FAIL(rc);
	return 0;
fail:
	return rc;
}
