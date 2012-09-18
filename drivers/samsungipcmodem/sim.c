/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis@gravedo.de>. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include <glib.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"
#include "simutil.h"

struct sim_data {
	struct ipc_device *device;
	guint sim_state_query;
	int pin_attempts[OFONO_SIM_PASSWORD_INVALID];
};

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

#define SIM_MAX_IMSI_LENGTH	16

static void pin_query_locked_cb(uint16_t cmd, void *data, uint16_t length,
				uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_query_facility_lock_cb_t cb = cbd->cb;
	struct ipc_sec_phone_lock_response_data *resp = data;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, resp->active, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_pin_query_enabled(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				ofono_query_facility_lock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_phone_lock_request_get_data *req;

	DBG("");

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, -1, data);
		return;
	}

	req = g_try_new0(struct ipc_sec_phone_lock_request_get_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_PHONE_LOCK,
			IPC_TYPE_GET, req,
			sizeof(struct ipc_sec_phone_lock_request_get_data),
			pin_query_locked_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void change_passwd_cb(uint16_t cmd, void *data, uint16_t length,
			     uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || cmd != IPC_GEN_PHONE_RES ||
		ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_change_locking_pw_data *req;

	DBG("");

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	req = g_try_new0(struct ipc_sec_change_locking_pw_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ipc_sec_change_locking_pw_setup(req, IPC_SEC_FACILITY_TYPE_SC,
					(char *) old_passwd,
					(char *) new_passwd);

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_CHANGE_LOCKING_PW,
				IPC_TYPE_SET, req,
				sizeof(struct ipc_sec_change_locking_pw_data),
				change_passwd_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void pin_enable_cb(uint16_t cmd, void *data, uint16_t length,
			  uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || cmd != IPC_GEN_PHONE_RES ||
		ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_pin_enable(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_phone_lock_request_set_data *req;
	int pin_type;

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		pin_type = IPC_SEC_PIN_TYPE_PIN1;
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		pin_type = IPC_SEC_PIN_TYPE_PIN2;
		break;
	default:
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	req = g_try_new0(struct ipc_sec_phone_lock_request_set_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ipc_sec_phone_lock_request_set_setup(req, pin_type, enable,
					     (char *) passwd);

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_PHONE_LOCK,
			IPC_TYPE_SET, req,
			sizeof(struct ipc_sec_phone_lock_request_set_data),
			pin_enable_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void puk_send_cb(uint16_t cmd, void *data, uint16_t length,
			uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || cmd != IPC_GEN_PHONE_RES ||
		ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_send_puk(struct ofono_sim *sim, const char *puk,
				const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;

	struct ipc_sec_pin_status_request_data *req;

	DBG("");

	req = g_try_new0(struct ipc_sec_pin_status_request_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ipc_sec_pin_status_setup(req, IPC_SEC_PIN_TYPE_PIN1, (char *) passwd,
				 (char *) puk);

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_PIN_STATUS,
				IPC_TYPE_SET, req,
				sizeof(struct ipc_sec_pin_status_request_data),
				puk_send_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void read_binary_cb(uint16_t cmd, void *data, uint16_t length,
			   uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	const guint8 *response;
	struct ipc_sec_rsim_access_response_header *resp;
	struct ofono_error err;

	if (error || cmd != IPC_SEC_RSIM_ACCESS) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
		goto done;
	}

	resp = data;

	DBG("rsim_access_cb: sw1=%02x, sw2=%02x, len=%i", resp->sw1,
			resp->sw2, resp->length);

	/* taken from drivers/atmodem/sim.c */
	if ((resp->sw1 != 0x90 && resp->sw1 != 0x91 && resp->sw1 != 0x92) ||
		(resp->sw1 == 0x90 && resp->sw2 != 0x00)) {
		memset(&error, 0, sizeof(error));

		err.type = OFONO_ERROR_TYPE_SIM;
		err.error = (resp->sw1 << 8) | resp->sw2;

		cb(&err, NULL, 0, cbd->data);
		goto done;
	}

	response = (const guint8 *) data +
			sizeof(struct ipc_sec_rsim_access_response_header);

	cb(&err, response, resp->length, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_read_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_read_cb_t cb, void *cb_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_rsim_access_request_header *req;

	DBG("");

	req = g_try_new0(struct ipc_sec_rsim_access_request_header, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, 0, cb_data);
		return;
	}

	cbd = cb_data_new(cb, cb_data);

	req->command = IPC_SEC_RSIM_COMMAND_READ_BINARY;
	req->file_id = (unsigned short) fileid;
	req->p1 = start >> 8;
	req->p2 = start & 0xff;
	req->p3 = length;

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_RSIM_ACCESS,
			IPC_TYPE_GET, req,
			sizeof(struct ipc_sec_rsim_access_request_header),
			read_binary_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, cb_data);
}

static void query_passwd_state_cb(uint16_t cmd, void *data, uint16_t length,
				  uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_passwd_cb_t cb = cbd->cb;
	struct ipc_sec_pin_status_response_data *resp = data;
	enum ofono_sim_password_type passwd_state;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		goto done;
	}

	switch (resp->facility_lock) {
	case IPC_SEC_FACILITY_LOCK_TYPE_SC_PIN1_REQ:
		passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case IPC_SEC_FACILITY_LOCK_TYPE_SC_PUK_REQ:
		passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case IPC_SEC_FACILITY_LOCK_TYPE_SC_UNLOCKED:
		passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	default:
		passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}

	CALLBACK_WITH_SUCCESS(cb, passwd_state, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_query_passwd_state(struct ofono_sim *sim,
					  ofono_sim_passwd_cb_t cb,
					  void *user_data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_PIN_STATUS,
				       IPC_TYPE_GET, NULL, 0,
				       query_passwd_state_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void notify_sim_status_cb(uint16_t cmd, void *data, uint16_t length,
				 void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ipc_sec_pin_status_response_data *resp = data;
	struct sim_data *sd = cbd->user;

	switch (resp->status) {
	case IPC_SEC_PIN_STATUS_READY:
	case IPC_SEC_PIN_STATUS_PB_INIT_COMPLETE:
		/* Ok, next status update will tell us more */
		break;
	case IPC_SEC_PIN_STATUS_INIT_COMPLETE:
		/* SIM initialization is done now */
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		goto done;
	default:
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	return;

done:
	ipc_device_remove_watch(sd->device, sd->sim_state_query);
	g_free(cbd);
}

static void pin_send_cb(uint16_t cmd, void *data, uint16_t length,
			uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || cmd != IPC_GEN_PHONE_RES ||
		ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
		return;
	}

	/* wait until SIM init complete status notification arrives */
	sd->sim_state_query = ipc_device_add_notification_watch(sd->device,
							IPC_SEC_PIN_STATUS,
							notify_sim_status_cb,
							cbd);
}

static void samsungipc_pin_send(struct ofono_sim *sim, const char *passwd,
			ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_pin_status_request_data *req;

	DBG("");

	req = g_try_new0(struct ipc_sec_pin_status_request_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = sd;

	ipc_sec_pin_status_setup(req, IPC_SEC_PIN_TYPE_PIN1, (char *) passwd,
				 NULL);

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_PIN_STATUS,
				IPC_TYPE_SET, req,
				sizeof(struct ipc_sec_pin_status_request_data),
				pin_send_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void read_imsi_cb(uint16_t cmd, void *data, uint16_t length,
			 uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	char *imsi;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		g_free(cbd);
		return;
	}

	imsi = ipc_misc_me_imsi_imsi_extract(data, length);
	if (imsi == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		g_free(cbd);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, imsi, cbd->data);

	free(imsi);

	g_free(cbd);
}

static void samsungipc_read_imsi(struct ofono_sim *sim,
				ofono_sim_imsi_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;

	cbd = cb_data_new(cb, data);

	DBG("");

	if (ipc_device_enqueue_message(sd->device, IPC_MISC_ME_IMSI,
				       IPC_TYPE_GET, NULL, 0,
				       read_imsi_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void lock_info_cb(uint16_t cmd, void *data, uint16_t length,
			 uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	struct ipc_sec_lock_infomation_response_data *resp = data;
	struct sim_data *sd = cbd->user;
	struct ipc_sec_lock_infomation_request_data *req;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		goto done;
	}

	switch (resp->type) {
	case IPC_SEC_PIN_TYPE_PIN1:
		sd->pin_attempts[OFONO_SIM_PASSWORD_SIM_PIN] =
				resp->retry_count;

		req = g_try_new0(struct ipc_sec_lock_infomation_request_data,
				 1);
		if (req == NULL) {
			CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
			goto done;
		}

		ipc_sec_lock_infomation_setup(req, IPC_SEC_PIN_TYPE_PIN2);

		if (ipc_device_enqueue_message(sd->device,
				IPC_SEC_LOCK_INFOMATION,
				IPC_TYPE_GET, req,
				sizeof(struct ipc_sec_lock_infomation_request_data),
				lock_info_cb, cbd) > 0)
			return;

		goto error;
	case IPC_SEC_PIN_TYPE_PIN2:
		sd->pin_attempts[OFONO_SIM_PASSWORD_SIM_PIN2] =
				resp->retry_count;
		break;
	}

	CALLBACK_WITH_SUCCESS(cb, sd->pin_attempts, cbd->data);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
done:
	g_free(cbd);
}

static void samsungipc_query_pin_retries(struct ofono_sim *sim,
					 ofono_sim_pin_retries_cb_t cb,
					 void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct ipc_sec_lock_infomation_request_data *req;

	DBG("");

	req = g_try_new0(struct ipc_sec_lock_infomation_request_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = sd;

	ipc_sec_lock_infomation_setup(req, IPC_SEC_PIN_TYPE_PIN1);

	memset(sd->pin_attempts, 0, sizeof(sd->pin_attempts));

	if (ipc_device_enqueue_message(sd->device, IPC_SEC_LOCK_INFOMATION,
			IPC_TYPE_GET, req,
			sizeof(struct ipc_sec_lock_infomation_request_data),
			lock_info_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void initial_sim_status_cb(uint16_t cmd, void *data, uint16_t length,
				  uint8_t error, void *user_data)
{
	struct ofono_sim *sim = user_data;
	struct ipc_sec_pin_status_response_data *resp = data;

	if (error) {
		ofono_sim_remove(sim);
		return;
	}

	ofono_sim_register(sim);

	switch (resp->status) {
	case IPC_SEC_PIN_STATUS_READY:
	case IPC_SEC_PIN_STATUS_SIM_LOCK_REQUIRED:
	case IPC_SEC_PIN_STATUS_INIT_COMPLETE:
	case IPC_SEC_PIN_STATUS_PB_INIT_COMPLETE:
	case IPC_SEC_PIN_STATUS_LOCK_SC:
	case IPC_SEC_PIN_STATUS_LOCK_FD:
	case IPC_SEC_PIN_STATUS_LOCK_PN:
	case IPC_SEC_PIN_STATUS_LOCK_PU:
	case IPC_SEC_PIN_STATUS_LOCK_PP:
	case IPC_SEC_PIN_STATUS_LOCK_PC:
		ofono_sim_inserted_notify(sim, TRUE);
		break;
	case IPC_SEC_PIN_STATUS_CARD_ERROR:
	case IPC_SEC_PIN_STATUS_CARD_NOT_PRESENT:
	case IPC_SEC_PIN_STATUS_INSIDE_PF_ERROR:
		ofono_sim_inserted_notify(sim, FALSE);
		break;
	}
}

static int samsungipc_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *user_data)
{
	struct sim_data *sd;

	DBG("");

	sd = g_try_new0(struct sim_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->device = user_data;

	ofono_sim_set_data(sim, sd);

	ipc_device_enqueue_message(sd->device, IPC_SEC_PIN_STATUS,
				   IPC_TYPE_GET, NULL, 0,
				   initial_sim_status_cb, sim);

	return 0;
}

static void samsungipc_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *data = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	if (data == NULL)
		return;

	g_free(data);
}

static struct ofono_sim_driver driver = {
	.name				= "samsungipcmodem",
	.probe				= samsungipc_sim_probe,
	.remove				= samsungipc_sim_remove,
	.query_passwd_state		= samsungipc_query_passwd_state,
	.query_pin_retries		= samsungipc_query_pin_retries,
	.send_passwd			= samsungipc_pin_send,
	.reset_passwd			= samsungipc_send_puk,
	.lock				= samsungipc_pin_enable,
	.change_passwd			= samsungipc_change_passwd,
	.query_facility_lock		= samsungipc_pin_query_enabled,
	.read_imsi			= samsungipc_read_imsi,
	.read_file_transparent		= samsungipc_read_binary,
};

void samsungipc_sim_init(void)
{
	ofono_sim_driver_register(&driver);
}

void samsungipc_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}
