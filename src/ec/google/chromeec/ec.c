/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2012 The Chromium OS Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdint.h>
#include <string.h>
#include <cbfs.h>
#include <cbmem.h>
#include <console/console.h>
#include <arch/io.h>
#include <bootmode.h>
#include <bootstate.h>
#include <delay.h>
#include <elog.h>
#include <halt.h>
#include <reset.h>
#include <elog.h>
#include <rtc.h>
#include <stdlib.h>
#include <vboot/vboot_common.h>
#include <assert.h>

#include "chip.h"
#include "ec.h"
#include "ec_commands.h"
#include "utility.h"

void log_recovery_mode_switch(void)
{
	uint32_t *events;

	if (cbmem_find(CBMEM_ID_EC_HOSTEVENT))
		return;

	events = cbmem_add(CBMEM_ID_EC_HOSTEVENT, sizeof(*events));
	if (!events)
		return;

	*events = google_chromeec_get_events_b();
}

static void google_chromeec_elog_add_recovery_event(void *unused)
{
	uint32_t *events = cbmem_find(CBMEM_ID_EC_HOSTEVENT);
	uint8_t event_byte = EC_EVENT_KEYBOARD_RECOVERY;

	if (!events)
		return;

	if (!(*events & EC_HOST_EVENT_MASK(EC_HOST_EVENT_KEYBOARD_RECOVERY)))
		return;

	if (*events &
	    EC_HOST_EVENT_MASK(EC_HOST_EVENT_KEYBOARD_RECOVERY_HW_REINIT))
		event_byte = EC_EVENT_KEYBOARD_RECOVERY_HWREINIT;

	elog_add_event_byte(ELOG_TYPE_EC_EVENT, event_byte);
}

BOOT_STATE_INIT_ENTRY(BS_WRITE_TABLES, BS_ON_ENTRY,
		      google_chromeec_elog_add_recovery_event, NULL);

uint8_t google_chromeec_calc_checksum(const uint8_t *data, int size)
{
	int csum;

	for (csum = 0; size > 0; data++, size--)
		csum += *data;
	return (uint8_t)(csum & 0xff);
}

int google_chromeec_kbbacklight(int percent)
{
	struct chromeec_command cec_cmd;
	struct ec_params_pwm_set_keyboard_backlight cmd_backlight;
	struct ec_response_pwm_get_keyboard_backlight rsp_backlight;
	/* if they were dumb, help them out */
	percent = percent % 101;
	cec_cmd.cmd_code = EC_CMD_PWM_SET_KEYBOARD_BACKLIGHT;
	cec_cmd.cmd_version = 0;
	cmd_backlight.percent = percent;
	cec_cmd.cmd_data_in = &cmd_backlight;
	cec_cmd.cmd_data_out = &rsp_backlight;
	cec_cmd.cmd_size_in = sizeof(cmd_backlight);
	cec_cmd.cmd_size_out = sizeof(rsp_backlight);
	cec_cmd.cmd_dev_index = 0;
	google_chromeec_command(&cec_cmd);
	printk(BIOS_DEBUG, "Google Chrome set keyboard backlight: %x status (%x)\n",
	       rsp_backlight.percent, cec_cmd.cmd_code);
	return cec_cmd.cmd_code;

}

void google_chromeec_post(u8 postcode)
{
	/* backlight is a percent. postcode is a u8.
	 * Convert the u8 to %.
	 */
	postcode = (postcode/4) + (postcode/8);
	google_chromeec_kbbacklight(postcode);
}

/*
 * Query the EC for specified mask indicating enabled events.
 * The EC maintains separate event masks for SMI, SCI and WAKE.
 */
static u32 google_chromeec_get_mask(u8 type)
{
	struct ec_params_host_event_mask req;
	struct ec_response_host_event_mask rsp;
	struct chromeec_command cmd;

	cmd.cmd_code = type;
	cmd.cmd_version = 0;
	cmd.cmd_data_in = &req;
	cmd.cmd_size_in = sizeof(req);
	cmd.cmd_data_out = &rsp;
	cmd.cmd_size_out = sizeof(rsp);
	cmd.cmd_dev_index = 0;

	if (google_chromeec_command(&cmd) == 0)
		return rsp.mask;
	return 0;
}

static int google_chromeec_set_mask(u8 type, u32 mask)
{
	struct ec_params_host_event_mask req;
	struct ec_response_host_event_mask rsp;
	struct chromeec_command cmd;

	req.mask = mask;
	cmd.cmd_code = type;
	cmd.cmd_version = 0;
	cmd.cmd_data_in = &req;
	cmd.cmd_size_in = sizeof(req);
	cmd.cmd_data_out = &rsp;
	cmd.cmd_size_out = sizeof(rsp);
	cmd.cmd_dev_index = 0;

	return google_chromeec_command(&cmd);
}

u32 google_chromeec_get_events_b(void)
{
	return google_chromeec_get_mask(EC_CMD_HOST_EVENT_GET_B);
}

int google_chromeec_clear_events_b(u32 mask)
{
	printk(BIOS_DEBUG, "Chrome EC: clear events_b mask to 0x%08x\n", mask);
	return google_chromeec_set_mask(
		EC_CMD_HOST_EVENT_CLEAR_B, mask);
}

int google_chromeec_check_feature(int feature)
{
	struct chromeec_command cmd;
	struct ec_response_get_features r;

	cmd.cmd_code = EC_CMD_GET_FEATURES;
	cmd.cmd_version = 0;
	cmd.cmd_size_in = 0;
	cmd.cmd_data_out = &r;
	cmd.cmd_size_out = sizeof(r);
	cmd.cmd_dev_index = 0;

	if (google_chromeec_command(&cmd) != 0)
		return -1;

	if (feature >= 8 * sizeof(r.flags))
		return -1;

	return r.flags[feature / 32] & EC_FEATURE_MASK_0(feature);
}

#if IS_ENABLED(CONFIG_EC_GOOGLE_CHROMEEC_RTC)
int rtc_get(struct rtc_time *time)
{
	struct chromeec_command cmd;
	struct ec_response_rtc r;

	cmd.cmd_code = EC_CMD_RTC_GET_VALUE;
	cmd.cmd_version = 0;
	cmd.cmd_size_in = 0;
	cmd.cmd_data_out = &r;
	cmd.cmd_size_out = sizeof(r);
	cmd.cmd_dev_index = 0;

	if (google_chromeec_command(&cmd) != 0)
		return -1;

	return rtc_to_tm(r.time, time);
}
#endif

#ifndef __SMM__
#ifdef __PRE_RAM__
void google_chromeec_check_ec_image(int expected_type)
{
	struct chromeec_command cec_cmd;
	struct ec_response_get_version cec_resp = { { 0 } };

	cec_cmd.cmd_code = EC_CMD_GET_VERSION;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_out = &cec_resp;
	cec_cmd.cmd_size_in = 0;
	cec_cmd.cmd_size_out = sizeof(cec_resp);
	cec_cmd.cmd_dev_index = 0;
	google_chromeec_command(&cec_cmd);

	if (cec_cmd.cmd_code || cec_resp.current_image != expected_type) {
		struct ec_params_reboot_ec reboot_ec;
		/* Reboot the EC and make it come back in RO mode */
		reboot_ec.cmd = EC_REBOOT_COLD;
		reboot_ec.flags = 0;
		cec_cmd.cmd_code = EC_CMD_REBOOT_EC;
		cec_cmd.cmd_version = 0;
		cec_cmd.cmd_data_in = &reboot_ec;
		cec_cmd.cmd_size_in = sizeof(reboot_ec);
		cec_cmd.cmd_size_out = 0; /* ignore response, if any */
		cec_cmd.cmd_dev_index = 0;
		printk(BIOS_DEBUG, "Rebooting with EC in RO mode:\n");
		post_code(0); /* clear current post code */
		/* Let the platform prepare for the EC taking out the system power. */
		if (IS_ENABLED(CONFIG_VBOOT))
			vboot_platform_prepare_reboot();
		google_chromeec_command(&cec_cmd);
		udelay(1000);
		hard_reset();
		halt();
	}
}

/* Check for recovery mode and ensure PD/EC is in RO */
void google_chromeec_early_init(void)
{
	if (!IS_ENABLED(CONFIG_CHROMEOS) || !vboot_recovery_mode_enabled())
		return;

	/* Check USB PD chip state first */
	if (IS_ENABLED(CONFIG_EC_GOOGLE_CHROMEEC_PD))
		google_chromeec_check_pd_image(EC_IMAGE_RO);

	/* If in recovery ensure EC is running RO firmware. */
	google_chromeec_check_ec_image(EC_IMAGE_RO);
}

void google_chromeec_check_pd_image(int expected_type)
{
	struct chromeec_command cec_cmd;
	struct ec_response_get_version cec_resp = { { 0 } };

	cec_cmd.cmd_code = EC_CMD_GET_VERSION;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_out = &cec_resp;
	cec_cmd.cmd_size_in = 0;
	cec_cmd.cmd_size_out = sizeof(cec_resp);
	cec_cmd.cmd_dev_index = 1; /* PD */
	google_chromeec_command(&cec_cmd);

	if (cec_cmd.cmd_code || cec_resp.current_image != expected_type) {
		struct ec_params_reboot_ec reboot_ec;
		/* Reboot the PD and make it come back in RO mode */
		reboot_ec.cmd = EC_REBOOT_COLD;
		reboot_ec.flags = 0;
		cec_cmd.cmd_code = EC_CMD_REBOOT_EC;
		cec_cmd.cmd_version = 0;
		cec_cmd.cmd_data_in = &reboot_ec;
		cec_cmd.cmd_size_in = sizeof(reboot_ec);
		cec_cmd.cmd_size_out = 0; /* ignore response, if any */
		cec_cmd.cmd_dev_index = 1; /* PD */
		printk(BIOS_DEBUG, "Rebooting PD to RO mode\n");
		google_chromeec_command(&cec_cmd);
		udelay(1000);
	}
}
#endif

u16 google_chromeec_get_board_version(void)
{
	struct chromeec_command cmd;
	struct ec_response_board_version board_v;

	cmd.cmd_code = EC_CMD_GET_BOARD_VERSION;
	cmd.cmd_version = 0;
	cmd.cmd_size_in = 0;
	cmd.cmd_size_out = sizeof(board_v);
	cmd.cmd_data_out = &board_v;
	cmd.cmd_dev_index = 0;

	if (google_chromeec_command(&cmd) != 0)
		return 0;

	return board_v.board_version;
}

int google_chromeec_vbnv_context(int is_read, uint8_t *data, int len)
{
	struct chromeec_command cec_cmd;
	struct ec_params_vbnvcontext cmd_vbnvcontext;
	struct ec_response_vbnvcontext rsp_vbnvcontext;
	int retries = 3;

	if (len != EC_VBNV_BLOCK_SIZE)
		return -1;

retry:
	cec_cmd.cmd_code = EC_CMD_VBNV_CONTEXT;
	cec_cmd.cmd_version = EC_VER_VBNV_CONTEXT;
	cec_cmd.cmd_data_in = &cmd_vbnvcontext;
	cec_cmd.cmd_data_out = &rsp_vbnvcontext;
	cec_cmd.cmd_size_in = sizeof(cmd_vbnvcontext);
	cec_cmd.cmd_size_out = is_read ? sizeof(rsp_vbnvcontext) : 0;
	cec_cmd.cmd_dev_index = 0;

	cmd_vbnvcontext.op = is_read ? EC_VBNV_CONTEXT_OP_READ :
					EC_VBNV_CONTEXT_OP_WRITE;

	if (!is_read)
		memcpy(&cmd_vbnvcontext.block, data, EC_VBNV_BLOCK_SIZE);

	if (google_chromeec_command(&cec_cmd)) {
		printk(BIOS_ERR, "ERROR: failed to %s vbnv_ec context: %d\n",
			is_read ? "read" : "write", (int)cec_cmd.cmd_code);
		mdelay(10);	/* just in case */
		if (--retries)
			goto retry;
	}

	if (is_read)
		memcpy(data, &rsp_vbnvcontext.block, EC_VBNV_BLOCK_SIZE);

	return cec_cmd.cmd_code;
}

#endif /* ! __SMM__ */

#ifndef __PRE_RAM__

int google_chromeec_i2c_xfer(uint8_t chip, uint8_t addr, int alen,
			     uint8_t *buffer, int len, int is_read)
{
	union {
		struct ec_params_i2c_passthru p;
		uint8_t outbuf[EC_HOST_PARAM_SIZE];
	} params;
	union {
		struct ec_response_i2c_passthru r;
		uint8_t inbuf[EC_HOST_PARAM_SIZE];
	} response;
	struct ec_params_i2c_passthru *p = &params.p;
	struct ec_response_i2c_passthru *r = &response.r;
	struct ec_params_i2c_passthru_msg *msg = p->msg;
	struct chromeec_command cmd;
	uint8_t *pdata;
	int read_len, write_len;
	int size;
	int rv;

	p->port = 0;

	if (alen != 1) {
		printk(BIOS_ERR, "Unsupported address length %d\n", alen);
		return -1;
	}
	if (is_read) {
		read_len = len;
		write_len = alen;
		p->num_msgs = 2;
	} else {
		read_len = 0;
		write_len = alen + len;
		p->num_msgs = 1;
	}

	size = sizeof(*p) + p->num_msgs * sizeof(*msg);
	if (size + write_len > sizeof(params)) {
		printk(BIOS_ERR, "Params too large for buffer\n");
		return -1;
	}
	if (sizeof(*r) + read_len > sizeof(response)) {
		printk(BIOS_ERR, "Read length too big for buffer\n");
		return -1;
	}

	/* Create a message to write the register address and optional data */
	pdata = (uint8_t *)p + size;
	msg->addr_flags = chip;
	msg->len = write_len;
	pdata[0] = addr;
	if (!is_read)
		memcpy(pdata + 1, buffer, len);
	msg++;

	if (read_len) {
		msg->addr_flags = chip | EC_I2C_FLAG_READ;
		msg->len = read_len;
	}

	cmd.cmd_code = EC_CMD_I2C_PASSTHRU;
	cmd.cmd_version = 0;
	cmd.cmd_data_in = p;
	cmd.cmd_size_in = size + write_len;
	cmd.cmd_data_out = r;
	cmd.cmd_size_out = sizeof(*r) + read_len;
	cmd.cmd_dev_index = 0;
	rv = google_chromeec_command(&cmd);
	if (rv != 0)
		return rv;

	/* Parse response */
	if (r->i2c_status & EC_I2C_STATUS_ERROR) {
		printk(BIOS_ERR, "Transfer failed with status=0x%x\n",
		       r->i2c_status);
		return -1;
	}

	if (cmd.cmd_size_out < sizeof(*r) + read_len) {
		printk(BIOS_ERR, "Truncated read response\n");
		return -1;
	}

	if (read_len)
		memcpy(buffer, r->data, read_len);

	return 0;
}

int google_chromeec_set_sci_mask(u32 mask)
{
	printk(BIOS_DEBUG, "Chrome EC: Set SCI mask to 0x%08x\n", mask);
	return google_chromeec_set_mask(
		EC_CMD_HOST_EVENT_SET_SCI_MASK, mask);
}

int google_chromeec_set_smi_mask(u32 mask)
{
	printk(BIOS_DEBUG, "Chrome EC: Set SMI mask to 0x%08x\n", mask);
	return google_chromeec_set_mask(
		EC_CMD_HOST_EVENT_SET_SMI_MASK, mask);
}

int google_chromeec_set_wake_mask(u32 mask)
{
	printk(BIOS_DEBUG, "Chrome EC: Set WAKE mask to 0x%08x\n", mask);
	return google_chromeec_set_mask(
		EC_CMD_HOST_EVENT_SET_WAKE_MASK, mask);
}

u32 google_chromeec_get_wake_mask(void)
{
	return google_chromeec_get_mask(
		EC_CMD_HOST_EVENT_GET_WAKE_MASK);
}

void google_chromeec_log_events(u32 mask)
{
#if CONFIG_ELOG
	u8 event;
	u32 wake_mask;

	/* Set wake mask so events will be read from ACPI interface */
	wake_mask = google_chromeec_get_wake_mask();
	google_chromeec_set_wake_mask(mask);

	while ((event = google_chromeec_get_event()) != 0) {
		if (EC_HOST_EVENT_MASK(event) & mask)
			elog_add_event_byte(ELOG_TYPE_EC_EVENT, event);
	}

	google_chromeec_set_wake_mask(wake_mask);
#endif
}

int google_chromeec_set_usb_charge_mode(u8 port_id, enum usb_charge_mode mode)
{
	struct chromeec_command cmd;
	struct ec_params_usb_charge_set_mode set_mode = {
		.usb_port_id = port_id,
		.mode = mode,
	};

	cmd.cmd_code = EC_CMD_USB_CHARGE_SET_MODE;
	cmd.cmd_version = 0;
	cmd.cmd_size_in = sizeof(set_mode);
	cmd.cmd_data_in = &set_mode;
	cmd.cmd_size_out = 0;
	cmd.cmd_data_out = NULL;
	cmd.cmd_dev_index = 0;

	return google_chromeec_command(&cmd);
}

int google_chromeec_set_usb_pd_role(u8 port, enum usb_pd_control_role role)
{
	struct ec_params_usb_pd_control req = {
		.port = port,
		.role = role,
		.mux = USB_PD_CTRL_MUX_NO_CHANGE,
		.swap = USB_PD_CTRL_SWAP_NONE,
	};
	struct ec_response_usb_pd_control rsp;
	struct chromeec_command cmd = {
		.cmd_code = EC_CMD_USB_PD_CONTROL,
		.cmd_version = 0,
		.cmd_data_in = &req,
		.cmd_size_in = sizeof(req),
		.cmd_data_out = &rsp,
		.cmd_size_out = sizeof(rsp),
		.cmd_dev_index = 0,
	};

	return google_chromeec_command(&cmd);
}

#ifndef __SMM__

static
int google_chromeec_hello(void)
{
	struct chromeec_command cec_cmd;
	struct ec_params_hello cmd_hello;
	struct ec_response_hello rsp_hello;
	cmd_hello.in_data = 0x10203040;
	cec_cmd.cmd_code = EC_CMD_HELLO;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = &cmd_hello.in_data;
	cec_cmd.cmd_data_out = &rsp_hello.out_data;
	cec_cmd.cmd_size_in = sizeof(cmd_hello.in_data);
	cec_cmd.cmd_size_out = sizeof(rsp_hello.out_data);
	cec_cmd.cmd_dev_index = 0;
	google_chromeec_command(&cec_cmd);
	printk(BIOS_DEBUG, "Google Chrome EC: Hello got back %x status (%x)\n",
	       rsp_hello.out_data, cec_cmd.cmd_code);
	return cec_cmd.cmd_code;
}

static int ec_image_type; /* Cached EC image type (ro or rw). */

void google_chromeec_init(void)
{
	struct chromeec_command cec_cmd;
	struct ec_response_get_version cec_resp = {{0}};
	struct ec_response_get_version cec_resp2 = {{0}};

	printk(BIOS_DEBUG, "Google Chrome EC: Initializing\n");

	google_chromeec_hello();

	cec_cmd.cmd_code = EC_CMD_GET_VERSION;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_out = &cec_resp;
	cec_cmd.cmd_size_in = 0;
	cec_cmd.cmd_size_out = sizeof(cec_resp);
	cec_cmd.cmd_dev_index = 0;
	google_chromeec_command(&cec_cmd);

	if (cec_cmd.cmd_code) {
		printk(BIOS_DEBUG,
		       "Google Chrome EC: version command failed!\n");
	} else {
		ec_image_type = cec_resp.current_image;
	}

	if (cec_cmd.cmd_code ||
	    (vboot_recovery_mode_enabled() &&
	     (cec_resp.current_image != EC_IMAGE_RO))) {
		google_chromeec_reboot_ro();
	} 
	
	/* Check/update EC RW image if needed */
	if (google_chromeec_swsync()) {
		printk(BIOS_ERR, "ChromeEC: EC SW SYNC FAILED\n");
	} else {
		/* EC RW image is up to date, switch to it */
		google_chromeec_reboot_rw();
		mdelay(50);
	}

	/* re-run version command & print */
	cec_cmd.cmd_code = EC_CMD_GET_VERSION;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_out = &cec_resp2;
	cec_cmd.cmd_size_in = 0;
	cec_cmd.cmd_size_out = sizeof(cec_resp2);
	cec_cmd.cmd_dev_index = 0;
	google_chromeec_command(&cec_cmd);

	if (cec_cmd.cmd_code) {
		printk(BIOS_DEBUG,
		       "Google Chrome EC: version command failed!\n");
	} else {
		printk(BIOS_DEBUG, "Google Chrome EC: version:\n");
		printk(BIOS_DEBUG, "    ro: %s\n", cec_resp2.version_string_ro);
		printk(BIOS_DEBUG, "    rw: %s\n", cec_resp2.version_string_rw);
		printk(BIOS_DEBUG, "  running image: %d\n",
		       cec_resp2.current_image);
		ec_image_type = cec_resp2.current_image;
	}

}

int google_ec_running_ro(void)
{
	return (ec_image_type == EC_IMAGE_RO);
}

void google_chromeec_reboot_ro(void)
{
	struct chromeec_command cec_cmd;
	struct ec_params_reboot_ec reboot_ec;
	/* Reboot the EC and make it come back in RO mode */
	reboot_ec.cmd = EC_REBOOT_COLD;
	reboot_ec.flags = 0;
	cec_cmd.cmd_code = EC_CMD_REBOOT_EC;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = &reboot_ec;
	cec_cmd.cmd_size_in = sizeof(reboot_ec);
	cec_cmd.cmd_size_out = 0; /* ignore response, if any */
	cec_cmd.cmd_dev_index = 0;
	printk(BIOS_DEBUG, "Rebooting EC in RO mode\n");
	post_code(0); /* clear current post code */
	google_chromeec_command(&cec_cmd);
	udelay(1000);
	hard_reset();
	halt();
}

void google_chromeec_reboot_rw(void) {

	struct chromeec_command cec_cmd;
	struct ec_params_reboot_ec reboot_ec;
	/* Reboot the EC and make it come back in RW mode */
	reboot_ec.cmd = EC_REBOOT_JUMP_RW;
	reboot_ec.flags = 0;
	cec_cmd.cmd_code = EC_CMD_REBOOT_EC;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = &reboot_ec;
	cec_cmd.cmd_size_in = sizeof(reboot_ec);
	cec_cmd.cmd_size_out = 0; /* ignore response, if any */
	cec_cmd.cmd_dev_index = 0;
	printk(BIOS_DEBUG, "Rebooting EC in RW mode\n");
	post_code(0); /* clear current post code */
	google_chromeec_command(&cec_cmd);
}


/* Timeout waiting for EC hash calculation completion */
static const int CROS_EC_HASH_TIMEOUT_MS = 2000;

/* Time to delay between polling status of EC hash calculation */
static const int CROS_EC_HASH_CHECK_DELAY_MS = 10;

int google_chromeec_swsync(void)
{
	static struct ec_response_vboot_hash resp;
	uint8_t *ec_hash;
	int ec_hash_size;
	uint8_t *ecrw_hash, *ecrw;
	int need_update = 0, i;
	size_t ecrw_size;

	/* Get hash of current EC-RW */
	if (google_chromeec_read_hash(&resp)) {
		printk(BIOS_ERR, "Failed to read EC hash.\n");
		return -1;
	}
	ec_hash = resp.hash_digest;
	ec_hash_size = resp.digest_size;
	/* Check hash size */
	if (ec_hash_size != SHA256_DIGEST_SIZE) {
		printk(BIOS_ERR, "ChromeEC SW Sync: - "
			 "read_hash says size %d, not %d\n",
			 ec_hash_size, SHA256_DIGEST_SIZE);
		return -1;
	}
	
	/* We got a proper hash */
	printk(BIOS_DEBUG, "ChromeEC SW Sync: EC hash: ");
	for (i = 0; i < SHA256_DIGEST_SIZE; i++)
		printk(BIOS_DEBUG, "%02x", ec_hash[i]);
	printk(BIOS_DEBUG, "\n");

	/* Get EC_RW binary, hash from CBFS */
	ecrw_hash = cbfs_boot_map_with_leak("ecrw.hash", CBFS_TYPE_RAW, NULL);
	
	if (!ecrw_hash) {
		/* Assume no EC update file for this board */
		return 0;
	}

	/* Got an expected hash */
	printk(BIOS_DEBUG, "ChromeEC SW Sync: Expected hash: ");
	for (i = 0; i < SHA256_DIGEST_SIZE; i++)
		printk(BIOS_DEBUG, "%02x", ecrw_hash[i]);
	printk(BIOS_DEBUG, "\n");
	/* compare hashes */
	need_update = SafeMemcmp(ec_hash, ecrw_hash, SHA256_DIGEST_SIZE);
	
	/* If in RW and need to update, return/reboot to RO */
	if (need_update && !google_ec_running_ro()) {
		printk(BIOS_DEBUG, "ChromeEC SW Sync: EC_RW needs update but in RW; rebooting...\n");
		google_chromeec_reboot_ro();
		return -1;
	}
	
	/* Update EC if necessary */
	if (need_update) {
		printk(BIOS_DEBUG, "ChromeEC SW Sync: updating EC_RW...\n");

		/* Get ecrw image from CBFS */
		ecrw = cbfs_boot_map_with_leak("ecrw", CBFS_TYPE_RAW, &ecrw_size);
		if (!ecrw) {
			printk(BIOS_ERR, "ChromeEC SW Sync: no ecrw image found in CBFS; cannot update\n");
			return -1;
		}

		if (google_chromeec_flash_update_rw(ecrw, ecrw_size)) {
			printk(BIOS_ERR, "ChromeEC SW Sync: Failed to update EC_RW.\n");
			return -1;
		}

		/* Have EC recompute hash for new EC_RW block */
		if (google_chromeec_read_hash(&resp) ) {
			printk(BIOS_ERR, "ChromeEC SW Sync: Failed to read EC hash.\n");
			return -1;
		}

		/* Compare new EC_RW hash to value from CBFS */
		ec_hash = resp.hash_digest;
		if(SafeMemcmp(ec_hash, ecrw_hash, SHA256_DIGEST_SIZE)) {
			/* hash mismatch! */
			printk(BIOS_DEBUG, "ChromeEC SW Sync: Expected hash: ");
			for (i = 0; i < SHA256_DIGEST_SIZE; i++)
				printk(BIOS_DEBUG, "%02x", ecrw_hash[i]);
			printk(BIOS_DEBUG, "\n");
			printk(BIOS_DEBUG, "ChromeEC SW Sync: EC hash: ");
			for (i = 0; i < SHA256_DIGEST_SIZE; i++)
				printk(BIOS_DEBUG, "%02x", ec_hash[i]);
			printk(BIOS_DEBUG, "\n");
			return -1;
		}
		printk(BIOS_DEBUG, "ChromeEC SW Sync: EC_RW hashes match\n");
	} else {
		printk(BIOS_DEBUG, "ChromeEC SW Sync: EC_RW is up to date\n");
	}

	printk(BIOS_DEBUG, "ChromeEC SW Sync: done\n");
	return 0;
}

int google_chromeec_read_hash(struct ec_response_vboot_hash *hash)
{
	struct chromeec_command cec_cmd;
	struct ec_params_vboot_hash p;
	int recalc_requested = 0;
	uint64_t start = timer_us(0);

	do {
		/* Get hash if available. */
		p.cmd = EC_VBOOT_HASH_GET;
		cec_cmd.cmd_code = EC_CMD_VBOOT_HASH;
		cec_cmd.cmd_version = 0;
		cec_cmd.cmd_data_in = &p;
		cec_cmd.cmd_data_out = hash;
		cec_cmd.cmd_size_in = sizeof(p);
		cec_cmd.cmd_size_out = sizeof(*hash);
		cec_cmd.cmd_dev_index = 0;
		printk(BIOS_DEBUG, "ChromeEC: Getting hash:\n");
		if (google_chromeec_command(&cec_cmd))
			return -1;

		switch (hash->status) {
		case EC_VBOOT_HASH_STATUS_NONE:
			/* We have no valid hash - let's request a recalc
			 * if we haven't done so yet. */
			if (recalc_requested != 0) {
				mdelay(CROS_EC_HASH_CHECK_DELAY_MS);
				break;
			}
			printk(BIOS_DEBUG, "ChromeEC: No valid hash (status=%d size=%d). "
			      "Compute one...\n", hash->status, hash->size);
			p.cmd = EC_VBOOT_HASH_RECALC;
			p.hash_type = EC_VBOOT_HASH_TYPE_SHA256;
			p.nonce_size = 0;
			p.offset = EC_VBOOT_HASH_OFFSET_RW;
			p.size = 0;
			cec_cmd.cmd_code = EC_CMD_VBOOT_HASH;
			cec_cmd.cmd_version = 0;
			cec_cmd.cmd_data_in = &p;
			cec_cmd.cmd_data_out = hash;
			cec_cmd.cmd_size_in = sizeof(p);
			cec_cmd.cmd_size_out = sizeof(*hash);
			cec_cmd.cmd_dev_index = 0;
			printk(BIOS_DEBUG, "ChromeEC: Starting EC hash:\n");
			if (google_chromeec_command(&cec_cmd))
				return -1;
			recalc_requested = 1;
			/* Command will wait to return until hash is done/ready */
			break;
		case EC_VBOOT_HASH_STATUS_BUSY:
			/* Hash is still calculating. */
			mdelay(CROS_EC_HASH_CHECK_DELAY_MS);
			break;
		case EC_VBOOT_HASH_STATUS_DONE:
		default:
			/* We have a valid hash. */
			break;
		}
	} while (hash->status != EC_VBOOT_HASH_STATUS_DONE &&
		 timer_us(start) < CROS_EC_HASH_TIMEOUT_MS * 1000);
	if (hash->status != EC_VBOOT_HASH_STATUS_DONE) {
		printk(BIOS_DEBUG, "ChromeEC: Hash status not done: %d\n", hash->status);
		return -1;
	}
	return 0;
}

int google_chromeec_flash_update_rw(const uint8_t *image, int image_size)
{
	uint32_t rw_offset, rw_size;
	int ret;

	/* get max size that can be written, offset to write */
	if (google_chromeec_flash_offset(EC_FLASH_REGION_RW, &rw_offset, &rw_size))
		return -1;
	if (image_size > rw_size)
		return -1;
	/*
	 * Erase the entire RW section, so that the EC doesn't see any garbage
	 * past the new image if it's smaller than the current image.
	 *
	 */
	ret = google_chromeec_flash_erase(rw_offset, rw_size);
	if (ret)
		return ret;
	/* Write the image */
	return(google_chromeec_flash_write(image, rw_offset, image_size));
}

int google_chromeec_flash_offset(enum ec_flash_region region,
			 uint32_t *offset, uint32_t *size)
{
	struct chromeec_command cec_cmd;
	struct ec_params_flash_region_info p;
	struct ec_response_flash_region_info r;
	p.region = region;

	/* Get offset and size */
	cec_cmd.cmd_code = EC_CMD_FLASH_REGION_INFO;
	cec_cmd.cmd_version = EC_VER_FLASH_REGION_INFO;
	cec_cmd.cmd_data_in = &p;
	cec_cmd.cmd_data_out = &r;
	cec_cmd.cmd_size_in = sizeof(p);
	cec_cmd.cmd_size_out = sizeof(r);
	cec_cmd.cmd_dev_index = 0;
	printk(BIOS_DEBUG, "Getting EC region info\n");
	if (google_chromeec_command(&cec_cmd))
		return -1;

	if (offset)
		*offset = r.offset;
	if (size)
		*size = r.size;
	return 0;
}

int google_chromeec_flash_erase(uint32_t offset, uint32_t size)
{
	struct chromeec_command cec_cmd;
	struct ec_params_flash_erase p;
	p.offset = offset;
	p.size = size;

	/* Erase block of size at offset */
	cec_cmd.cmd_code = EC_CMD_FLASH_ERASE;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = &p;
	cec_cmd.cmd_size_in = sizeof(p);
	cec_cmd.cmd_dev_index = 0;
	printk(BIOS_DEBUG, "Erasing EC RW region\n");

	return google_chromeec_command(&cec_cmd);
}

int google_chromeec_flash_write(const uint8_t *data, uint32_t offset, uint32_t size)
{
	//printk(BIOS_DEBUG, "google_chromeec_flash_write(): 0x%x bytes at 0x%x\n", size, offset);
	uint32_t burst = google_chromeec_flash_write_burst_size();
	uint32_t end, off;
	int ret;
	if (!burst)
		return -1;
	end = offset + size;
	printk(BIOS_DEBUG, "Writing EC RW region\n");
	for (off = offset; off < end; off += burst, data += burst) {
		uint32_t todo = MIN(end - off, burst);
		if (todo < burst) {
			uint8_t *buf = malloc(burst);
			memcpy(buf, data, todo);
			// Pad the buffer with a decent guess for erased data
			// value.
			memset(buf + todo, 0xff, burst - todo);
			ret = google_chromeec_flash_write_block(buf,
							off, burst);
			free(buf);
		} else {
			ret = google_chromeec_flash_write_block(data,
							off, burst);
		}
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * Return optimal flash write burst size
 */
int google_chromeec_flash_write_burst_size(void)
{
	struct chromeec_command cec_cmd;
	struct ec_response_flash_info info;
	uint32_t pdata_max_size = EC_LPC_HOST_PACKET_SIZE - sizeof(struct ec_host_request) -
		sizeof(struct ec_params_flash_write);
	/*
	 * Determine whether we can use version 1 of the command with more
	 * data, or only version 0.
	 */
	if (!google_chromeec_cmd_version_supported(EC_CMD_FLASH_WRITE, EC_VER_FLASH_WRITE))
		return EC_FLASH_WRITE_VER0_SIZE;
	/*
	 * Determine step size.  This must be a multiple of the write block
	 * size, and must also fit into the host parameter buffer.
	 */
	cec_cmd.cmd_code = EC_CMD_FLASH_INFO;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = NULL;
	cec_cmd.cmd_data_out = &info;
	cec_cmd.cmd_size_in = 0;
	cec_cmd.cmd_size_out = sizeof(info);
	cec_cmd.cmd_dev_index = 0;
	if (google_chromeec_command(&cec_cmd))
		return -1;

	return (pdata_max_size / info.write_block_size) *
		info.write_block_size;
}

static uint8_t *buf = NULL;
static uint32_t bufsize = 0;

/**
 * Write a single block to the flash
 *
 * Write a block of data to the EC flash. The size must not exceed the flash
 * write block size which you can obtain from cros_ec_flash_write_burst_size().
 *
 * The offset starts at 0. You can obtain the region information from
 * cros_ec_flash_offset() to find out where to write for a particular region.
 *
 * Attempting to write to the region where the EC is currently running from
 * will result in an error.
 *
 * @param data		Pointer to data buffer to write
 * @param offset	Offset within flash to write to.
 * @param size		Number of bytes to write
 * @return 0 if ok, -1 on error
 */
int google_chromeec_flash_write_block(const uint8_t *data, 
			uint32_t offset, uint32_t size)
{
	struct chromeec_command cec_cmd;
	struct ec_params_flash_write *p;

	assert(data);
	/* Make sure request fits in the allowed packet size */
	if (bufsize == 0) {
		bufsize = sizeof(*p) + size;
		buf = malloc(bufsize);
	} else if (bufsize != sizeof(*p) + size) {
		free(buf);
		bufsize = sizeof(*p) + size;
		buf = malloc(bufsize);
	}
	if (bufsize > EC_LPC_HOST_PACKET_SIZE)
		return -1;

	p = (struct ec_params_flash_write *)buf;
	p->offset = offset;
	p->size = size;
	memcpy(p + 1, data, size);

	cec_cmd.cmd_code = EC_CMD_FLASH_WRITE;
	cec_cmd.cmd_version = EC_VER_FLASH_WRITE;
	cec_cmd.cmd_data_in = buf;
	cec_cmd.cmd_data_out = NULL;
	cec_cmd.cmd_size_in = bufsize;
	cec_cmd.cmd_size_out = 0;
	cec_cmd.cmd_dev_index = 0;

	return google_chromeec_command(&cec_cmd);
}

/**
 * Return non-zero if the EC supports the command and version
 *
 * @param cmd		Command to check
 * @param ver		Version to check
 * @return non-zero if command version supported; 0 if not.
 */
int google_chromeec_cmd_version_supported(int cmd, int ver)
{
	uint32_t mask = 0;
	if (google_chromeec_get_cmd_versions(cmd, &mask))
		return 0;
	return (mask & EC_VER_MASK(ver)) ? 1 : 0;
}

/**
 * Get the versions of the command supported by the EC.
 *
 * @param cmd		Command
 * @param pmask		Destination for version mask; will be set to 0 on
 *			error.
 * @return 0 if success, <0 if error
 */
int google_chromeec_get_cmd_versions(int cmd, uint32_t *pmask)
{
	struct chromeec_command cec_cmd;
	struct ec_params_get_cmd_versions p;
	struct ec_response_get_cmd_versions r;
	*pmask = 0;
	p.cmd = cmd;
	/* Get command versions */
	cec_cmd.cmd_code = EC_CMD_GET_CMD_VERSIONS;
	cec_cmd.cmd_version = 0;
	cec_cmd.cmd_data_in = &p;
	cec_cmd.cmd_data_out = &r;
	cec_cmd.cmd_size_in = sizeof(p);
	cec_cmd.cmd_size_out = sizeof(r);
	cec_cmd.cmd_dev_index = 0;
	if (google_chromeec_command(&cec_cmd))
		return -1;
	*pmask = r.version_mask;
	return 0;
}

#endif /* ! __SMM__ */

#endif /* ! __PRE_RAM__ */
