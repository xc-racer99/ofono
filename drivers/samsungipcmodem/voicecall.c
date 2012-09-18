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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"

/* Amount of ms we wait between call status requests */
#define CALL_STATUS_POLL_INTERVAL 500

struct voicecall_data {
	struct ipc_device *device;
	int dtmf_active;
	unsigned int status_update_pending;
};

static void send_dtmf_cb(uint16_t cmd, void *data, uint16_t length,
			 uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	unsigned char ret;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	ret = *((unsigned char *) data);
	if (!ret) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd;
	struct ipc_call_cont_dtmf_data *reqs;
	int tone_count;
	int n;

	DBG("");

	tone_count = strlen(dtmf);

	reqs = g_try_new0(struct ipc_call_cont_dtmf_data, tone_count);
	if (reqs == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	for (n = 0; n > tone_count; n++) {
		reqs[n].status = IPC_CALL_DTMF_STATUS_START;
		reqs[n].tone = dtmf[n];
	}

	if (ipc_device_enqueue_message(vd->device, IPC_CALL_RELEASE,
			IPC_TYPE_EXEC, reqs,
			sizeof(struct ipc_call_cont_dtmf_data) * tone_count,
			send_dtmf_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void common_call_cb(uint16_t cmd, void *data, uint16_t length,
			   uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_hangup_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("");

	if (ipc_device_enqueue_message(vd->device, IPC_CALL_RELEASE,
				       IPC_TYPE_EXEC, NULL, 0,
				       common_call_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void samsungipc_answer(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);

	DBG("");

	if (ipc_device_enqueue_message(vd->device, IPC_CALL_ANSWER,
				       IPC_TYPE_EXEC, NULL, 0,
				       common_call_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void samsungipc_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ipc_call_outgoing_data *req;
	struct cb_data *cbd;

	DBG("");

	req = g_try_new0(struct ipc_call_outgoing_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	switch (clir) {
	case OFONO_CLIR_OPTION_DEFAULT:
		req->identity = IPC_CALL_IDENTITY_DEFAULT;
		break;
	case OFONO_CLIR_OPTION_INVOCATION:
		req->identity = IPC_CALL_IDENTITY_SHOW;
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		req->identity = IPC_CALL_IDENTITY_HIDE;
		break;
	}

	req->type = IPC_CALL_TYPE_VOICE;
	req->prefix = (ph->type == 145) ?
			IPC_CALL_PREFIX_INTL : IPC_CALL_PREFIX_NONE;
	req->number_length = strlen(ph->number);
	strncpy((char *) req->number, ph->number, 86);

	if (ipc_device_enqueue_message(vd->device, IPC_CALL_OUTGOING,
					IPC_TYPE_EXEC, req,
					sizeof(struct ipc_call_outgoing_data),
					common_call_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);

	g_free(cbd);
}

static void call_list_cb(uint16_t cmd, void *data, uint16_t length,
			 uint8_t error, void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ipc_call_list_entry *entry;
	struct ofono_call call;
	char *entry_number;
	int num_entries;
	int n;

	DBG("");

	if (error)
		return;

	num_entries = ipc_call_list_count_extract(data, length);

	DBG("Got %i entries", num_entries);

	for (n = 0; n < num_entries; n++) {
		ofono_call_init(&call);

		entry = ipc_call_list_entry_extract(data, length, n);

		call.id = entry->id;
		call.type = (entry->type == IPC_CALL_TYPE_VOICE ||
				entry->type == IPC_CALL_TYPE_DEFAULT) ? 0 : 1;
		call.direction = (entry->term == IPC_CALL_TERM_MT) ? 0 : 1;
		call.status = entry->status - 1;

		memset(&call.phone_number.type, 0,
				OFONO_MAX_PHONE_NUMBER_LENGTH);

		entry_number = ipc_call_list_entry_number_extract(entry);
		if (entry_number != NULL) {
			strncpy(call.phone_number.number, entry_number,
					OFONO_MAX_CALLER_NAME_LENGTH);
			call.phone_number.number[entry->number_length] = '\0';
			call.phone_number.type = (entry->number_length > 0 &&
					call.phone_number.number[0] == '+') ?
					145 : 129;
			g_free(entry_number);
		}

		DBG("id=%i, type=%i, direction=%i, status=%i, number=%s",
			call.id, call.type, call.direction, call.status,
			call.phone_number.number);

		ofono_voicecall_notify(vc, &call);
	}

	vd->status_update_pending = 0;
}

static void update_call_status(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->status_update_pending)
		return;

	vd->status_update_pending = 1;

	if (ipc_device_enqueue_message(vd->device, IPC_CALL_LIST, IPC_TYPE_GET,
				       NULL, 0, call_list_cb, vc) > 0)
		return;
}

static void notify_call_incoming_cb(uint16_t cmd, void *data, uint16_t length,
				    void *user_data)
{
	struct ofono_voicecall *vc = user_data;

	update_call_status(vc);
}

static void notify_call_status_cb(uint16_t cmd, void *data, uint16_t length,
				  void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct ipc_call_status_data *resp = data;

	DBG("");

	if (resp->status == IPC_CALL_STATUS_RELEASED) {
		// FIXME what are possible values for resp->reason?
		ofono_voicecall_disconnected(vc, resp->id,
				OFONO_DISCONNECT_REASON_UNKNOWN, NULL);
		return;
	}

	update_call_status(vc);
}

static gboolean initialization_done_cb(gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ipc_device_add_notification_watch(vd->device, IPC_CALL_INCOMING,
			notify_call_incoming_cb, vc);
	ipc_device_add_notification_watch(vd->device, IPC_CALL_STATUS,
			notify_call_status_cb, vc);
	ipc_device_add_notification_watch(vd->device, IPC_CALL_RELEASE,
			notify_call_status_cb, vc);
	ipc_device_add_notification_watch(vd->device, IPC_CALL_ANSWER,
			notify_call_status_cb, vc);
	ipc_device_add_notification_watch(vd->device, IPC_CALL_OUTGOING,
			notify_call_status_cb, vc);
	ipc_device_add_notification_watch(vd->device, IPC_CALL_WAITING,
			notify_call_status_cb, vc);

	ofono_voicecall_register(vc);

	return FALSE;
}

static int samsungipc_voicecall_probe(struct ofono_voicecall *vc,
				      unsigned int vendor, void *data)
{
	struct voicecall_data *vd;

	DBG("");

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->device = data;
	vd->dtmf_active = 0;
	vd->status_update_pending = 0;

	ofono_voicecall_set_data(vc, vd);

	g_timeout_add_seconds(0, initialization_done_cb, vc);

	return 0;
}

static void samsungipc_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name		= "samsungipcmodem",
	.probe		= samsungipc_voicecall_probe,
	.remove		= samsungipc_voicecall_remove,
	.dial		= samsungipc_dial,
	.answer		= samsungipc_answer,
	.hangup_active	= samsungipc_hangup_active,
	.send_tones	= samsungipc_send_dtmf,
};

void samsungipc_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void samsungipc_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
