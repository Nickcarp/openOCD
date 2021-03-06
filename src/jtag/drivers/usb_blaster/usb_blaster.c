/*
 *   Driver for USB-JTAG, Altera USB-Blaster and compatibles
 *
 *   Inspired from original code from Kolja Waschk's USB-JTAG project
 *   (http://www.ixo.de/info/usb_jtag/), and from openocd project.
 *
 *   Copyright (C) 2012 Robert Jarzmik robert.jarzmik@free.fr
 *   Copyright (C) 2011 Ali Lown ali@lown.me.uk
 *   Copyright (C) 2009 Catalin Patulea cat@vv.carleton.ca
 *   Copyright (C) 2006 Kolja Waschk usbjtag@ixo.de
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 */

/*
 * The following information is originally from Kolja Waschk's USB-JTAG,
 * where it was obtained by reverse engineering an Altera USB-Blaster.
 * See http://www.ixo.de/info/usb_jtag/ for USB-Blaster block diagram and
 * usb_jtag-20080705-1200.zip#usb_jtag/host/openocd for protocol.
 *
 * The same information is also on the UrJTAG mediawiki, with some additional
 * notes on bits marked as "unknown" by usb_jtag.
 * (http://sourceforge.net/apps/mediawiki/urjtag/index.php?
 *    title=Cable_Altera_USB-Blaster)
 *
 * USB-JTAG, Altera USB-Blaster and compatibles are typically implemented as
 * an FTDIChip FT245 followed by a CPLD which handles a two-mode protocol:
 *
 *            _________
 *           |         |
 *           | AT93C46 |
 *           |_________|
 *            __|__________    _________
 *           |             |  |         |
 *      USB__| FTDI 245BM  |__| EPM7064 |__JTAG (B_TDO,B_TDI,B_TMS,B_TCK)
 *           |_____________|  |_________|
 *            __|__________    _|___________
 *           |             |  |             |
 *           | 6 MHz XTAL  |  | 24 MHz Osc. |
 *           |_____________|  |_____________|
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if IS_CYGWIN == 1
#include "windows.h"
#undef LOG_ERROR
#endif

/* project specific includes */
#include <jtag/interface.h>
#include <jtag/commands.h>
#include <helper/time_support.h>
#include "ublast_access.h"

/* system includes */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

/* Size of USB endpoint max packet size, ie. 64 bytes */
#define BUF_LEN 64

#define NO_TAP_SHIFT	0
#define TAP_SHIFT	1

struct ublast_info {
	int pin6;
	int pin8;
	int tms;
	int tdi;
	uint8_t buf[BUF_LEN];
	int bufidx;

	char *lowlevel_name;
	struct ublast_lowlevel *drv;
	char *ublast_device_desc;
	uint16_t ublast_vid, ublast_pid;
};

/*
 * Global device control
 */
static struct ublast_info info = {
	.ublast_vid = 0x09fb, /* Altera */
	.ublast_pid = 0x6001, /* USB-Blaster */
	.lowlevel_name = NULL,
};

/*
 * Available lowlevel drivers (FTDI, FTD2xx, ...)
 */
struct drvs_map {
	char *name;
	struct ublast_lowlevel *(*drv_register)(void);
};

static struct drvs_map lowlevel_drivers_map[] = {
	{ .name = "ftdi", .drv_register = ublast_register_ftdi },
	{ .name = "ftd2xx", .drv_register = ublast_register_ftd2xx },
	{ NULL, NULL },
};

/*
 * Access functions to lowlevel driver, agnostic of libftdi/libftdxx
 */
static char *hexdump(uint8_t *buf, unsigned int size)
{
	unsigned int i;
	char *str = calloc(size * 2 + 1, 1);

	for (i = 0; i < size; i++)
		sprintf(str + 2*i, "%02x", buf[i]);
	return str;
}

static int ublast_buf_read(uint8_t *buf, unsigned size, uint32_t *bytes_read)
{
	int ret = info.drv->read(info.drv, buf, size, bytes_read);
	char *str = hexdump(buf, *bytes_read);

	DEBUG_JTAG_IO("(size=%d, buf=[%s]) -> %u", size, str,
		      *bytes_read);
	free(str);
	return ret;
}

static int ublast_buf_write(uint8_t *buf, int size, uint32_t *bytes_written)
{
	int ret = info.drv->write(info.drv, buf, size, bytes_written);
	char *str = hexdump(buf, *bytes_written);

	DEBUG_JTAG_IO("(size=%d, buf=[%s]) -> %u", size, str,
		      *bytes_written);
	free(str);
	return ret;
}

static int nb_buf_remaining(void)
{
	return BUF_LEN - info.bufidx;
}

static void ublast_flush_buffer(void)
{
	unsigned int retlen;
	int nb = info.bufidx, ret = ERROR_OK;

	while (ret == ERROR_OK && nb > 0) {
		ret = ublast_buf_write(info.buf, nb, &retlen);
		nb -= retlen;
	}
	info.bufidx = 0;
}

/*
 * Actually, the USB-Blaster offers a byte-shift mode to transmit up to 504 data
 * bits (bidirectional) in a single USB packet. A header byte has to be sent as
 * the first byte in a packet with the following meaning:
 *
 *   Bit 7 (0x80): Must be set to indicate byte-shift mode.
 *   Bit 6 (0x40): If set, the USB-Blaster will also read data, not just write.
 *   Bit 5..0:     Define the number N of following bytes
 *
 * All N following bytes will then be clocked out serially on TDI. If Bit 6 was
 * set, it will afterwards return N bytes with TDO data read while clocking out
 * the TDI data. LSB of the first byte after the header byte will appear first
 * on TDI.
 */

/* Simple bit banging mode:
 *
 *   Bit 7 (0x80): Must be zero (see byte-shift mode above)
 *   Bit 6 (0x40): If set, you will receive a byte indicating the state of TDO
 *                 in return.
 *   Bit 5 (0x20): Output Enable/LED.
 *   Bit 4 (0x10): TDI Output.
 *   Bit 3 (0x08): nCS Output (not used in JTAG mode).
 *   Bit 2 (0x04): nCE Output (not used in JTAG mode).
 *   Bit 1 (0x02): TMS Output.
 *   Bit 0 (0x01): TCK Output.
 *
 * For transmitting a single data bit, you need to write two bytes (one for
 * setting up TDI/TMS/TCK=0, and one to trigger TCK high with same TDI/TMS
 * held). Up to 64 bytes can be combined in a single USB packet.
 * It isn't possible to read a data without transmitting data.
 */

#define TCK		(1 << 0)
#define TMS		(1 << 1)
#define NCE		(1 << 2)
#define NCS		(1 << 3)
#define TDI		(1 << 4)
#define LED		(1 << 5)
#define READ		(1 << 6)
#define SHMODE		(1 << 7)
#define READ_TDO	(1 << 0)

/**
 * ublast_reset - reset the JTAG device is possible
 * @trst: 1 if TRST is to be asserted
 * @srst: 1 if SRST is to be asserted
 *
 * This is not implemented yet. If pin6 or pin8 controlls the TRST/SRST, code
 * should be added so that this function makes use of it.
 */
static void ublast_reset(int trst, int srst)
{
	DEBUG_JTAG_IO("TODO: ublast_reset(%d,%d) isn't implemented!",
		  trst, srst);
	if (trst == 1)
		tap_set_state(TAP_RESET);
}

/**
 * ublast_queue_byte - queue one 'bitbang mode' byte for USB Blaster
 * @abyte: the byte to queue
 *
 * Queues one byte in 'bitbang mode' to the USB Blaster. The byte is not
 * actually sent, but stored in a 64 bytes buffer. The write is performed once
 * the buffer is filled, or if an explicit ublast_flush_buffer() is called.
 */
static void ublast_queue_byte(uint8_t abyte)
{
	if (nb_buf_remaining() < 1)
		ublast_flush_buffer();
	info.buf[info.bufidx++] = abyte;
	if (nb_buf_remaining() == 0)
		ublast_flush_buffer();
	DEBUG_JTAG_IO("(byte=0x%02x) (TDI = %d, TMS = %d, TCK = %d, LED = %d, READ = %d)", abyte,
			(abyte&TDI)?1:0, (abyte&TMS)?1:0, (abyte&TCK)?1:0, (abyte&LED)?1:0, (abyte&READ)?1:0);
}

/**
 * ublast_build_out - build bitbang mode output byte
 * @type: says if reading back TDO is required
 *
 * Returns the compute bitbang mode byte
 */
static uint8_t ublast_build_out(enum scan_type type)
{
	uint8_t abyte = 0;

	abyte |= info.tms ? TMS : 0;
	abyte |= info.pin6 ? NCE : 0;
	abyte |= info.pin8 ? NCS : 0;
	abyte |= info.tdi ? TDI : 0;
	abyte |= LED;
	if (type == SCAN_IN || type == SCAN_IO)
		abyte |= READ;
	return abyte;
}

/**
 * ublast_clock_tms - clock a TMS transition
 * @tms: the TMS to be sent
 *
 * Triggers a TMS transition (ie. one JTAG TAP state move).
 */
static void ublast_clock_tms(int tms)
{
	uint8_t out;

	DEBUG_JTAG_IO("(tms=%d)", !!tms);
	info.tms = !!tms;
	info.tdi = 0;
	out = ublast_build_out(SCAN_OUT);
	ublast_queue_byte(out);
	ublast_queue_byte(out | TCK);
}

/**
 * ublast_idle_clock - put back TCK to low level
 *
 * See ublast_queue_tdi() comment for the usage of this function.
 */
static void ublast_idle_clock(void)
{
	uint8_t out = ublast_build_out(SCAN_OUT);

	DEBUG_JTAG_IO(".");
	ublast_queue_byte(out);
}

/**
 * ublast_clock_tdi - Output a TDI with bitbang mode
 * @tdi: the TDI bit to be shifted out
 * @type: scan type (ie. does a readback of TDO is required)
 *
 * Output a TDI bit and assert clock to push it into the JTAG device :
 *  - writing out TCK=0, TMS=<old_state>=0, TDI=<tdi>
 * - writing out TCK=1, TMS=<new_state>, TDI=<tdi> which triggers the JTAG
 *    device aquiring the data.
 *
 * If a TDO is to be read back, the required read is requested (bitbang mode),
 * and the USB Blaster will send back a byte with bit0 reprensenting the TDO.
 */
static void ublast_clock_tdi(int tdi, enum scan_type type)
{
	uint8_t out;

	DEBUG_JTAG_IO("(tdi=%d)",  !!tdi);
	info.tdi = !!tdi;

	out = ublast_build_out(SCAN_OUT);
	ublast_queue_byte(out);

	out = ublast_build_out(type);
	ublast_queue_byte(out | TCK);
}

/**
 * ublast_clock_tdi_flip_tms - Output a TDI with bitbang mode, change JTAG state
 * @tdi: the TDI bit to be shifted out
 * @type: scan type (ie. does a readback of TDO is required)
 *
 * This function is the same as ublast_clock_tdi(), but it changes also the TMS
 * while outputing the TDI. This should be the last TDI output of a TDI
 * sequence, which will change state from :
 *   - IRSHIFT -> IREXIT1
 *   - or DRSHIFT -> DREXIT1
 */
static void ublast_clock_tdi_flip_tms(int tdi, enum scan_type type)
{
	uint8_t out;

	DEBUG_JTAG_IO("(tdi=%d)", !!tdi);
	info.tdi = !!tdi;
	info.tms = !info.tms;

	out = ublast_build_out(SCAN_OUT);
	ublast_queue_byte(out);

	out = ublast_build_out(type);
	ublast_queue_byte(out | TCK);

	out = ublast_build_out(SCAN_OUT);
	ublast_queue_byte(out);
}

/**
 * ublast_queue_bytes - queue bytes for the USB Blaster
 * @bytes: byte array
 * @nb_bytes: number of bytes (limited to 64)
 *
 * Queues bytes to be sent to the USB Blaster.
 *
 * As the USB endpoint can only accept 64 bytes, the caller should ensure enough
 * space is available by calling nb_buf_remaining().
 * If the 64 bytes buffer is filled, the write is issued to the USB device.
 */
static void ublast_queue_bytes(uint8_t *bytes, int nb_bytes)
{
	if (info.bufidx + nb_bytes > BUF_LEN) {
		LOG_ERROR("buggy code, should never queue more that %d bytes",
			  info.bufidx + nb_bytes);
		exit(-1);
	}
	DEBUG_JTAG_IO("(nb_bytes=%d, bytes=[0x%02x, ...])", nb_bytes,
		      bytes ? bytes[0] : 0);
	if (bytes)
		memcpy(&info.buf[info.bufidx], bytes, nb_bytes);
	else
		memset(&info.buf[info.bufidx], 0, nb_bytes);
	info.bufidx += nb_bytes;
	if (nb_buf_remaining() == 0)
		ublast_flush_buffer();
}

/**
 * ublast_tms_seq - write a TMS sequence transition to JTAG
 * @bits: TMS bits to be written (bit0, bit1 .. bitN)
 * @nb_bits: number of TMS bits (between 1 and 8)
 *
 * Write a serie of TMS transitions, where each transition consists in :
 *  - writing out TCK=0, TMS=<new_state>, TDI=<???>
 *  - writing out TCK=1, TMS=<new_state>, TDI=<???> which triggers the transition
 * The function ensures that at the end of the sequence, the clock (TCK) is put
 * low.
 */
static void ublast_tms_seq(const uint8_t *bits, int nb_bits)
{
	int i;

	DEBUG_JTAG_IO("(bits=%02x..., nb_bits=%d)", bits[0], nb_bits);
	for (i = 0; i < nb_bits; i++)
		ublast_clock_tms((bits[i / 8] >> (i % 8)) & 0x01);
	ublast_idle_clock();
}

/**
 * ublast_tms - write a tms command
 * @cmd: tms command
 */
static void ublast_tms(struct tms_command *cmd)
{
	DEBUG_JTAG_IO("(num_bits=%d)", cmd->num_bits);
	ublast_tms_seq(cmd->bits, cmd->num_bits);
}

/**
 * ublast_path_move - write a TMS sequence transition to JTAG
 * @cmd: path transition
 *
 * Write a serie of TMS transitions, where each transition consists in :
 *  - writing out TCK=0, TMS=<new_state>, TDI=<???>
 *  - writing out TCK=1, TMS=<new_state>, TDI=<???> which triggers the transition
 * The function ensures that at the end of the sequence, the clock (TCK) is put
 * low.
 */
static void ublast_path_move(struct pathmove_command *cmd)
{
	int i;

	DEBUG_JTAG_IO("(num_states=%d, last_state=%d)",
		  cmd->num_states, cmd->path[cmd->num_states - 1]);
	for (i = 0; i < cmd->num_states; i++) {
		if (tap_state_transition(tap_get_state(), false) == cmd->path[i])
			ublast_clock_tms(0);
		if (tap_state_transition(tap_get_state(), true) == cmd->path[i])
			ublast_clock_tms(1);
		tap_set_state(cmd->path[i]);
	}
	ublast_idle_clock();
}

/**
 * ublast_state_move - move JTAG state to the target state
 * @state: the target state
 *
 * Input the correct TMS sequence to the JTAG TAP so that we end up in the
 * target state. This assumes the current state (tap_get_state()) is correct.
 */
static void ublast_state_move(tap_state_t state)
{
	uint8_t tms_scan;
	int tms_len;

	DEBUG_JTAG_IO("(from %s to %s)", tap_state_name(tap_get_state()),
		  tap_state_name(state));
	if (tap_get_state() == state)
		return;
	tms_scan = tap_get_tms_path(tap_get_state(), state);
	tms_len = tap_get_tms_path_len(tap_get_state(), state);
	ublast_tms_seq(&tms_scan, tms_len);
	tap_set_state(state);
}

/**
 * ublast_read_byteshifted_tdos - read TDO of byteshift writes
 * @buf: the buffer to store the bits
 * @nb_bits: the number of bits
 *
 * Reads back from USB Blaster TDO bits, triggered by a 'byteshift write', ie. eight
 * bits per received byte from USB interface, and store them in buffer.
 *
 * As the USB blaster stores the TDO bits in LSB (ie. first bit in (byte0,
 * bit0), second bit in (byte0, bit1), ...), which is what we want to return,
 * simply read bytes from USB interface and store them.
 *
 * Returns ERROR_OK if OK, ERROR_xxx if a read error occured
 */
static int ublast_read_byteshifted_tdos(uint8_t *buf, int nb_bytes)
{
	unsigned int retlen;
	int ret = ERROR_OK;

	DEBUG_JTAG_IO("%s(buf=%p, num_bits=%d)", __func__, buf, nb_bytes * 8);
	ublast_flush_buffer();
	while (ret == ERROR_OK && nb_bytes > 0) {
		ret = ublast_buf_read(buf, nb_bytes, &retlen);
		nb_bytes -= retlen;
	}
	return ret;
}

/**
 * ublast_read_bitbang_tdos - read TDO of bitbang writes
 * @buf: the buffer to store the bits
 * @nb_bits: the number of bits
 *
 * Reads back from USB Blaster TDO bits, triggered by a 'bitbang write', ie. one
 * bit per received byte from USB interface, and store them in buffer, where :
 *  - first bit is stored in byte0, bit0 (LSB)
 *  - second bit is stored in byte0, bit 1
 *  ...
 *  - eight bit is sotred in byte0, bit 7
 *  - ninth bit is sotred in byte1, bit 0
 *  - etc ...
 *
 * Returns ERROR_OK if OK, ERROR_xxx if a read error occured
 */
static int ublast_read_bitbang_tdos(uint8_t *buf, int nb_bits)
{
	int nb1 = nb_bits;
	int i, ret = ERROR_OK;
	unsigned int retlen;
	uint8_t tmp[8];

	DEBUG_JTAG_IO("%s(buf=%p, num_bits=%d)", __func__, buf, nb_bits);

	/*
	 * Ensure all previous bitbang writes were issued to the dongle, so that
	 * it returns back the read values.
	 */
	ublast_flush_buffer();

	ret = ublast_buf_read(tmp, nb1, &retlen);
	for (i = 0; ret == ERROR_OK && i < nb1; i++)
		if (tmp[i] & READ_TDO)
			*buf |= (1 << i);
		else
			*buf &= ~(1 << i);
	return ret;
}

/**
 * ublast_queue_tdi - short description
 * @bits: bits to be queued on TDI (or NULL if 0 are to be queued)
 * @nb_bits: number of bits
 * @scan: scan type (ie. if TDO read back is required or not)
 *
 * Outputs a serie of TDI bits on TDI.
 * As a side effect, the last TDI bit is sent along a TMS=1, and triggers a JTAG
 * TAP state shift if input bits were non NULL.
 *
 * In order to not saturate the USB Blaster queues, this method reads back TDO
 * if the scan type requests it, and stores them back in bits.
 *
 * As a side note, the state of TCK when entering this function *must* be
 * low. This is because byteshift mode outputs TDI on rising TCK and reads TDO
 * on falling TCK if and only if TCK is low before queuing byteshift mode bytes.
 * If TCK was high, the USB blaster will queue TDI on falling edge, and read TDO
 * on rising edge !!!
 */
static void ublast_queue_tdi(uint8_t *bits, int nb_bits, enum scan_type scan, int tap_shift)
{
	int nb8 = nb_bits / 8;
	int nb1 = nb_bits % 8;
	int nbfree, i, trans = 0, read_tdos;
	uint8_t *tdos = calloc(1, nb_bits / 8 + 1);
	static uint8_t byte0[BUF_LEN];

	/*
	 * As the last TDI bit should always be output in bitbang mode in order
	 * to activate the TMS=1 transition to EXIT_?R state. Therefore a
	 * situation where nb_bits is a multiple of 8 is handled as follows:
	 * - the number of TDI shifted out in "byteshift mode" is 8 less than
	 *   nb_bits
	 * - nb1 = 8
	 * This ensures that nb1 is never 0, and allows the TMS transition.
	 */
	if (nb8 > 0 && nb1 == 0) {
		nb8--;
		nb1 = 8;
	}

	read_tdos = (scan == SCAN_IN || scan == SCAN_IO);
	for (i = 0; i < nb8; i += trans) {
		nbfree = nb_buf_remaining();
		trans = MIN(nbfree - 1, nb8 - i);

		/*
		 * Queue a byte-shift mode transmission, with as many bytes as
		 * is possible with regard to :
		 *  - current filling level of write buffer
		 *  - remaining bytes to write in byte-shift mode
		 */
		if (read_tdos)
			ublast_queue_byte(SHMODE | READ | trans);
		else
			ublast_queue_byte(SHMODE | trans);
		if (bits)
			ublast_queue_bytes(&bits[i], trans);
		else
			ublast_queue_bytes(byte0, trans);
		if (read_tdos)
			ublast_read_byteshifted_tdos(&tdos[i], trans);
	}

	/*
	 * Queue the remaining TDI bits in bitbang mode.
	 */
	for (i = 0; i < nb1; i++) {
		int tdi = bits ? bits[nb8 + i / 8] & (1 << i) : 0;
		if (tap_shift && bits && i == nb1 - 1)
			ublast_clock_tdi_flip_tms(tdi, scan);
		else
			ublast_clock_tdi(tdi, scan);
	}
	if (nb1 && read_tdos)
		ublast_read_bitbang_tdos(&tdos[nb8], nb1);

	if (bits)
		memcpy(bits, tdos, DIV_ROUND_UP(nb_bits, 8));
	free(tdos);

	/*
	 * Ensure clock is in lower state
	 */
	ublast_idle_clock();
}

static void ublast_runtest(int cycles, tap_state_t state)
{
	DEBUG_JTAG_IO("%s(cycles=%i, end_state=%d)", __func__, cycles, state);

	ublast_state_move(TAP_IDLE);
	ublast_queue_tdi(NULL, cycles, SCAN_OUT,TAP_SHIFT);
	ublast_state_move(state);
}

static void ublast_stableclocks(int cycles)
{
	DEBUG_JTAG_IO("%s(cycles=%i)", __func__, cycles);
	ublast_queue_tdi(NULL, cycles, SCAN_OUT, TAP_SHIFT);
}

/**
 * ublast_scan - launches a DR-scan or IR-scan
 * @cmd: the command to launch
 *
 * Launch a JTAG IR-scan or DR-scan
 *
 * Returns ERROR_OK if OK, ERROR_xxx if a read/write error occured.
 */
static int ublast_scan(struct scan_command *cmd)
{
	int scan_bits;
	uint8_t *buf = NULL;
	enum scan_type type;
	int ret = ERROR_OK;
	static const char * const type2str[] = { "", "SCAN_IN", "SCAN_OUT", "SCAN_IO" };
	char *log_buf = NULL;

	type = jtag_scan_type(cmd);
	scan_bits = jtag_build_buffer(cmd, &buf);

	if (cmd->ir_scan)
		ublast_state_move(TAP_IRSHIFT);
	else
		ublast_state_move(TAP_DRSHIFT);

	log_buf = hexdump(buf, DIV_ROUND_UP(scan_bits, 8));
	DEBUG_JTAG_IO("%s(scan=%s, type=%s, bits=%d, buf=[%s], end_state=%d)", __func__,
		  cmd->ir_scan ? "IRSCAN" : "DRSCAN",
		  type2str[type],
		  scan_bits, log_buf, cmd->end_state);
	free(log_buf);

	if(cmd->end_state == TAP_DRSHIFT)
		ublast_queue_tdi(buf, scan_bits, type, NO_TAP_SHIFT);
	else
		ublast_queue_tdi(buf, scan_bits, type, TAP_SHIFT);

	if(cmd->end_state != TAP_DRSHIFT) {
		/*
		 * As our JTAG is in an unstable state (IREXIT1 or DREXIT1), move it
		 * forward to a stable IRPAUSE or DRPAUSE.
		 */
		ublast_clock_tms(0);
		if (cmd->ir_scan)
			tap_set_state(TAP_IRPAUSE);
		else
			tap_set_state(TAP_DRPAUSE);
	}

	ret = jtag_read_buffer(buf, cmd);
	if (buf)
		free(buf);

	if(cmd->end_state != TAP_DRSHIFT)
		ublast_state_move(cmd->end_state);

	return ret;
}

static void ublast_msleep(int ms)
{
	DEBUG_JTAG_IO("%s(ms=%d)",  __func__, ms);
	jtag_sleep(ms);
}

static int ublast_execute_queue(void)
{
	struct jtag_command *cmd;
	int ret = ERROR_OK;

	for (cmd = jtag_command_queue; ret == ERROR_OK && cmd != NULL;
	     cmd = cmd->next) {
		switch (cmd->type) {
		case JTAG_RESET:
			ublast_reset(cmd->cmd.reset->trst, cmd->cmd.reset->srst);
			break;
		case JTAG_RUNTEST:
			ublast_runtest(cmd->cmd.runtest->num_cycles,
				       cmd->cmd.runtest->end_state);
			break;
		case JTAG_STABLECLOCKS:
			ublast_stableclocks(cmd->cmd.stableclocks->num_cycles);
			break;
		case JTAG_TLR_RESET:
			ublast_state_move(cmd->cmd.statemove->end_state);
			break;
		case JTAG_PATHMOVE:
			ublast_path_move(cmd->cmd.pathmove);
			break;
		case JTAG_TMS:
			ublast_tms(cmd->cmd.tms);
			break;
		case JTAG_SLEEP:
			ublast_msleep(cmd->cmd.sleep->us);
			break;
		case JTAG_SCAN:
			ret = ublast_scan(cmd->cmd.scan);
			break;
		}
	}

	ublast_flush_buffer();
	return ret;
}

static int ublast_speed(int speed)
{
	return info.drv->speed(info.drv, speed);
}

/**
 * ublast_init - Initialize the Altera device
 *
 * Initialize the device :
 *  - open the USB device
 *  - empty the write FIFO (128 bytes)
 *  - empty the read FIFO (384 bytes)
 *
 * Returns ERROR_OK if USB device found, error if not.
 */
static int ublast_init(void)
{
	static uint8_t buf0[BUF_LEN], tms_reset = 0xff;
	unsigned int retlen;
	int ret, i;

	if (info.lowlevel_name) {
		for (i = 0; lowlevel_drivers_map[i].name; i++)
			if (!strcmp(lowlevel_drivers_map[i].name, info.lowlevel_name))
				break;
		if (lowlevel_drivers_map[i].name)
			info.drv = lowlevel_drivers_map[i].drv_register();
		if (!info.drv) {
			LOG_ERROR("no lowlevel driver found for %s or lowlevel driver opening error",
				  info.lowlevel_name);
			return ERROR_JTAG_DEVICE_ERROR;
		}
	} else {
		LOG_INFO("No lowlevel driver configured, will try them all");
		for (i = 0; !info.drv && lowlevel_drivers_map[i].name; i++)
			info.drv = lowlevel_drivers_map[i].drv_register();
		if (!info.drv) {
			LOG_ERROR("no lowlevel driver found");
			return ERROR_JTAG_DEVICE_ERROR;
		}
	}

	/*
	 * Register the lowlevel driver
	 */
	info.drv->ublast_vid = info.ublast_vid;
	info.drv->ublast_pid = info.ublast_pid;
	info.drv->ublast_device_desc = info.ublast_device_desc;

	ret = info.drv->open(info.drv);
	if (ret == ERROR_OK) {
		/*
		 * Flush USB-Blaster queue fifos
		 */
		for (i = 0; i < 128 / BUF_LEN; i += BUF_LEN)
			ublast_buf_write(buf0, BUF_LEN, &retlen);
		retlen = 1;
		/*
		 * Put JTAG in RESET state (five 1 on TMS)
		 */
		ublast_tms_seq(&tms_reset, 5);
		tap_set_state(TAP_RESET);
	}
	return ret;
}

/**
 * ublast_quit - Release the Altera device
 *
 * Releases the device :
 *   - put the device pins in 'high impedance' mode
 *   - close the USB device
 *
 * Returns always ERROR_OK
 */
static int ublast_quit(void)
{
	uint8_t byte0 = 0;
	unsigned int retlen;

	ublast_buf_write(&byte0, 1, &retlen);
	return info.drv->close(info.drv);
}

static int dummy_khz(int khz, int *jtag_speed)
{
	*jtag_speed =khz;
	return ERROR_OK;
}

static int dummy_speed_div(int speed, int *khz)
{
	*khz = speed;
	return ERROR_OK;
}

COMMAND_HANDLER(ublast_handle_device_desc_command)
{
	if (CMD_ARGC == 1)
		info.ublast_device_desc = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("require exactly one argument to "
				  "ublast_device_desc <description>");

	return ERROR_OK;
}

COMMAND_HANDLER(ublast_handle_vid_pid_command)
{
	if (CMD_ARGC > 2) {
		LOG_WARNING("ignoring extra IDs in ublast_vid_pid "
					"(maximum is 1 pair)");
		CMD_ARGC = 2;
	}
	if (CMD_ARGC == 2) {
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[0], info.ublast_vid);
		COMMAND_PARSE_NUMBER(u16, CMD_ARGV[1], info.ublast_pid);
	} else {
		LOG_WARNING("incomplete ublast_vid_pid configuration");
	}

	return ERROR_OK;
}

COMMAND_HANDLER(ublast_handle_pin_command)
{
	uint8_t out_value;

	if (CMD_ARGC == 2) {
		const char * const pin_name = CMD_ARGV[0];
		unsigned int state;

		COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], state);
		if ((state != 0) && (state != 1)) {
			LOG_ERROR("%s: pin state must be 0 or 1", CMD_NAME);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}

		if (!strcmp(pin_name, "pin6")) {
			info.pin6 = state;
		} else if (!strcmp(pin_name, "pin8")) {
			info.pin8 = state;
		} else {
			LOG_ERROR("%s: pin name must be \"pin6\" or \"pin8\"",
					CMD_NAME);
			return ERROR_COMMAND_SYNTAX_ERROR;
		}

		if (info.drv) {
			out_value = ublast_build_out(SCAN_OUT);
			ublast_queue_byte(out_value);
			ublast_flush_buffer();
		}
		return ERROR_OK;
	} else {
		LOG_ERROR("%s takes exactly two arguments", CMD_NAME);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
}

COMMAND_HANDLER(ublast_handle_lowlevel_drv_command)
{
	if (CMD_ARGC == 1)
		info.lowlevel_name = strdup(CMD_ARGV[0]);
	else
		LOG_ERROR("require exactly one argument to "
				  "usb_blaster_lowlevel_driver (ftdi|ftd2xx)");
	return ERROR_OK;
}

static const struct command_registration ublast_command_handlers[] = {
	{
		.name = "usb_blaster_device_desc",
		.handler = ublast_handle_device_desc_command,
		.mode = COMMAND_CONFIG,
		.help = "set the USB device description of the USB-Blaster",
		.usage = "description-string",
	},
	{
		.name = "usb_blaster_vid_pid",
		.handler = ublast_handle_vid_pid_command,
		.mode = COMMAND_CONFIG,
		.help = "the vendor ID and product ID of the USB-Blaster",
		.usage = "vid pid",
	},
	{
		.name = "usb_blaster_lowlevel_driver",
		.handler = ublast_handle_lowlevel_drv_command,
		.mode = COMMAND_CONFIG,
		.help = "set the lowlevel access for the USB Blaster (ftdi, ftd2xx)",
		.usage = "(ftdi|ftd2xx)",
	},
	{
		.name = "usb_blaster",
		.handler = ublast_handle_pin_command,
		.mode = COMMAND_ANY,
		.help = "set pin state for the unused GPIO pins",
		.usage = "(pin6|pin8) (0|1)",
	},
	COMMAND_REGISTRATION_DONE
};

struct jtag_interface usb_blaster_interface = {
	.name = "usb_blaster",
	.commands = ublast_command_handlers,
	.supported = DEBUG_CAP_TMS_SEQ,
	.transports = jtag_only,

	.khz = &dummy_khz,
	.speed_div = &dummy_speed_div,

	.execute_queue = ublast_execute_queue,
	.speed = ublast_speed,
	.init = ublast_init,
	.quit = ublast_quit,
};
