/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2019 Jonathan Bakker <xc-racer2@live.ca>
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
#include <ofono/sms.h>

#include <glib.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"

struct sms_data {
	struct ipc_device *device;
	guint sms_incoming_msg_watch;
	unsigned char *default_smsc;
	guint default_smsc_len;
	struct cb_data *pending_sms_cb;
	guint sms_send_watch;
};

static void sms_send_msg_notif(uint16_t cmd, void *data, uint16_t length,
			    void *user_data)
{
	struct ipc_sms_send_msg_response_data *resp;
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd = sd->pending_sms_cb;
	ofono_sms_submit_cb_t cb;

	if (cbd == NULL) {
		DBG("Received SMS_SEND_MSG cb without pending message!");
		return;
	}

	cb = cbd->cb;
	sd->pending_sms_cb = NULL;

	if (data == NULL ||
			length < sizeof(struct ipc_sms_send_msg_response_data)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		g_free(cbd);
		return;
	}

	resp = data;

	if (resp->ack != IPC_SMS_ACK_NO_ERROR) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		g_free(cbd);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, resp->id, cbd->data);
	g_free(cbd);
}

static void sms_send_cb(uint16_t cmd, void *data, uint16_t length,
			void *user_data)
{
	struct sms_data *sd = user_data;
	struct cb_data *cbd = sd->pending_sms_cb;
	ofono_sms_submit_cb_t cb;

	/*
	 * Only callback with failure, otherwise this will be handled
	 * with the IPC_SMS_SEND_MSG callback
	 */
	if (cmd == IPC_GEN_PHONE_RES && ipc_gen_phone_res_check(data) < 0 &&
			cbd != NULL) {
		cb = cbd->cb;
		sd->pending_sms_cb = NULL;
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		g_free(cbd);
	}
}

static void samsungipc_sms_send(struct ofono_sms *sms,
				const unsigned char *pdu,
				int pdu_len, int tpdu_len, int mms,
				ofono_sms_submit_cb_t cb, void *data)
{
	struct ipc_sms_send_msg_request_header request_header;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd;
	void *ipc_data;
	uint16_t size = 0;
	int smsc_len;

	if (sd->pending_sms_cb != NULL) {
		DBG("Sending failed due to already pending SMS");
		CALLBACK_WITH_FAILURE(cb, -1, data);
		return;
	}

	DBG("pdu_len %d tpdu_len %d mms %d", pdu_len, tpdu_len, mms);

	cbd = cb_data_new(cb, data);
	sd->pending_sms_cb = cbd;

	/*
	 * SMSC address:
	 *
	 * smsc_len == 1, then zero-length SMSC was spec'd
	 */
	smsc_len = pdu_len - tpdu_len;

	memset(&request_header, 0, sizeof(request_header));
	request_header.type = IPC_SMS_TYPE_OUTGOING;

	if (mms)
		request_header.msg_type = IPC_SMS_MSG_TYPE_MULTIPLE;
	else
		request_header.msg_type = IPC_SMS_MSG_TYPE_SINGLE;

	if (smsc_len == 1) {
		size = ipc_sms_send_msg_size_setup(&request_header,
					sd->default_smsc, sd->default_smsc_len,
					pdu + smsc_len, tpdu_len);

		ipc_data = ipc_sms_send_msg_setup(&request_header,
					sd->default_smsc, sd->default_smsc_len,
					pdu + smsc_len, tpdu_len);
	} else {
		size = ipc_sms_send_msg_size_setup(&request_header, pdu,
					smsc_len, pdu + smsc_len, tpdu_len);

		ipc_data = ipc_sms_send_msg_setup(&request_header, pdu,
					smsc_len, pdu + smsc_len, tpdu_len);
	}

	if (ipc_data == NULL)
		goto error;

	if (size == 0) {
		g_free(ipc_data);
		goto error;
	}

	ipc_device_enqueue_message(sd->device, IPC_SMS_SEND_MSG,
				   IPC_TYPE_EXEC, ipc_data, size,
				   sms_send_cb, sd);

	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
	sd->pending_sms_cb = NULL;
	g_free(cbd);
}

static void sms_deliver_report_cb(uint16_t cmd, void *data, uint16_t length,
				void *user_data)
{
	if (data == NULL || length < sizeof(struct ipc_gen_phone_res_data) ||
			cmd != IPC_GEN_PHONE_RES) {
		ofono_debug("acking SMS delivery failed - invalid response");
		return;
	}

	if (ipc_gen_phone_res_check(data) < 0)
		ofono_debug("Failed to acknowledge receipt of SMS");
}

static void sms_incoming_msg_cb(uint16_t cmd, void *data, uint16_t length,
				 void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct ipc_sms_incoming_msg_header *header = data;
	struct ipc_sms_deliver_report_request_data *report_data;
	uint16_t pdu_size, smsc_len;
	const unsigned char *pdu;

	report_data =
		g_try_new0(struct ipc_sms_deliver_report_request_data, 1);
	if (report_data == NULL)
		return;

	pdu_size = ipc_sms_incoming_msg_pdu_size_extract(data, length);
	if (pdu_size == 0) {
		ofono_debug("Failed to extract PDU size from SMS");
		return;
	}

	pdu = ipc_sms_incoming_msg_pdu_extract(data, length);
	if (pdu == NULL) {
		ofono_debug("Failed to extract PDU from SMS");
		return;
	}

	/*
	 * The first octect in the pdu contains the SMSC address length
	 * which is the X following octects it reads. We add 1 octet to
	 * the read length to take into account this read octet in order
	 * to calculate the proper tpdu length.
	 */
	smsc_len = pdu[0] + 1;
	ofono_debug("smsc_len is %d", smsc_len);

	if (header->type == IPC_SMS_TYPE_STATUS_REPORT) {
		ofono_sms_status_notify(sms, pdu, pdu_size,
					pdu_size - smsc_len);
	} else {
		ofono_sms_deliver_notify(sms, pdu, pdu_size,
					pdu_size - smsc_len);
	}

	/* Acknowledge receipt of SMS */
	report_data->type = IPC_SMS_TYPE_STATUS_REPORT;
	report_data->ack = IPC_SMS_ACK_NO_ERROR;
	report_data->id = header->id;

	ipc_device_enqueue_message(sd->device, IPC_SMS_DELIVER_REPORT,
			IPC_TYPE_EXEC, report_data,
			sizeof(struct ipc_sms_deliver_report_request_data),
			sms_deliver_report_cb, NULL);
}

static void sms_device_ready_cb(uint16_t cmd, void *data, uint16_t length,
				void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	void *smsc;

	if (data == NULL || length < sizeof(struct ipc_sms_svc_center_addr_header))
		return;

	sd->default_smsc_len = ipc_sms_svc_center_addr_smsc_size_extract(data, length);
	if (sd->default_smsc_len == 0)
		return;

	smsc = ipc_sms_svc_center_addr_smsc_extract(data, length);
	if (smsc == NULL)
		return;

	sd->default_smsc = g_try_malloc(sd->default_smsc_len);
	if (sd->default_smsc == NULL)
		return;

	memcpy(sd->default_smsc, smsc, sd->default_smsc_len);

	ofono_sms_register(sms);

	sd->sms_incoming_msg_watch = ipc_device_add_notification_watch(sd->device,
							IPC_SMS_INCOMING_MSG,
							sms_incoming_msg_cb,
							sms);

	sd->sms_send_watch = ipc_device_add_notification_watch(sd->device,
							IPC_SMS_SEND_MSG,
							sms_send_msg_notif,
							sms);
}

static int samsungipc_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user_data)
{
	struct sms_data *sd;

	sd = g_new0(struct sms_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	ofono_sms_set_data(sms, sd);

	sd->device = user_data;
	sd->pending_sms_cb = NULL;

	ipc_device_enqueue_message(sd->device, IPC_SMS_SVC_CENTER_ADDR,
			IPC_TYPE_GET, NULL, 0, sms_device_ready_cb, sms);

	return 0;
}

static void samsungipc_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);

	ipc_device_remove_watch(sd->device, sd->sms_incoming_msg_watch);
	ipc_device_remove_watch(sd->device, sd->sms_send_watch);

	if (sd->default_smsc != NULL)
		g_free(sd->default_smsc);

	ofono_sms_set_data(sms, NULL);

	if (sd == NULL)
		return;

	g_free(sd);
}

static const struct ofono_sms_driver driver = {
	.name		= "samsungipcmodem",
	.probe		= samsungipc_sms_probe,
	.remove		= samsungipc_sms_remove,
	.submit		= samsungipc_sms_send,
};

void samsungipc_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void samsungipc_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
