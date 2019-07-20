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
	GQueue *pending_pdu;
	guint sms_incoming_msg_watch;
};

struct pending_pdu {
	const unsigned char *pdu;
	uint16_t pdu_len;
};

static void smsc_query_cb(uint16_t cmd, void *data, uint16_t length,
				uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_query_cb_t cb = cbd->cb;
	struct ofono_phone_number sca;
	struct ipc_sms_svc_center_addr_header *header = data;
	uint16_t smsc_size;
	char *smsc;

	if (header->length == 0) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		goto done;
	}

	smsc_size = ipc_sms_svc_center_addr_smsc_size_extract(data, length);
	if (smsc_size == 0) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		goto done;
	}

	smsc = ipc_sms_svc_center_addr_smsc_extract(data, length);
	if (smsc == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		goto done;
	}

	if (smsc[0] == '+') {
		strncpy(sca.number, smsc + 1, smsc_size - 1);
		sca.number[smsc_size - 1] = '\0';
		sca.type = 145;
	} else {
		strncpy(sca.number, smsc, smsc_size);
		sca.number[smsc_size] = '\0';
		sca.type = 129;
	}

	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_smsc_query(struct ofono_sms *sms,
				  ofono_sms_sca_query_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd;

	cbd = cb_data_new(cb, data);

	if (ipc_device_enqueue_message(sd->device, IPC_SMS_SVC_CENTER_ADDR,
			IPC_TYPE_GET, NULL, 0, smsc_query_cb, cbd) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void samsungipc_smsc_set(struct ofono_sms *sms,
				const struct ofono_phone_number *sca,
				ofono_sms_sca_set_cb_t cb, void *data)
{
	/* Not implemented, probably involves IPC_SMS_SVC_OPTION
	 * but there is no documentation of this anywhere
	 */
	CALLBACK_WITH_FAILURE(cb, data);
}

static void sms_send_cb(uint16_t cmd, void *data, uint16_t length,
				uint8_t error, void *user_data)
{
	struct ipc_sms_send_msg_response_data *resp;
	struct cb_data *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	int mr;

	resp = data;

	if (error || resp->ack != IPC_SMS_ACK_NO_ERROR) {
		CALLBACK_WITH_FAILURE(cb, 0, cbd->data);
		return;
	}

	mr = resp->id;

	CALLBACK_WITH_SUCCESS(cb, mr, cbd->data);
}

static void sms_send(struct sms_data *sd, const void *smsc, uint16_t smsc_size,
		     const void *pdu, uint16_t pdu_size, void *data)
{
	struct ipc_sms_send_msg_request_header request_header;
	struct cb_data *cbd = data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	unsigned char *p;
	uint16_t size = 0;
	uint8_t count, index;

	memset(&request_header, 0, sizeof(request_header));
	request_header.type = IPC_SMS_TYPE_OUTGOING;
	request_header.msg_type = IPC_SMS_MSG_TYPE_SINGLE;

	p = (unsigned char *) pdu;

	/* PDU TP-DA length */
	p += 2;

	if (*p > (255 / 2)) {
		ofono_debug("PDU TP-DA length failed (0x%x)", *p);
		goto setup;
	}

	/* PDU TP-UDH length */
	p += *p;

	if (*p > (255 / 2) || *p < 5) {
		ofono_debug("PDU TP-UDH length failed (0x%x)", *p);
		goto setup;
	}

	/* PDU TO-UDH count */
	p += 4;
	count = (unsigned int) *p;

	if (count > 0x0f) {
		ofono_debug("PDU TP-UDH count failed (%d)", count);
		goto setup;
	}

	/* PDU TO-UDH index */
	p += 1;
	index = (unsigned int) *p;

	if (index > count) {
		ofono_debug("PDU TP-UDH index failed (%d)", index);
		goto setup;
	}

	if (count > 1) {
		request_header.msg_type = IPC_SMS_MSG_TYPE_MULTIPLE;
		ofono_debug("Sending multi-part msg %d/%d\n", index, count);
	}

setup:
	size = ipc_sms_send_msg_size_setup(&request_header, smsc, smsc_size,
					   pdu, pdu_size);
	if (size == 0)
		goto error;

	data = ipc_sms_send_msg_setup(&request_header, smsc, smsc_size,
				      pdu, pdu_size);
	if (data == NULL)
		goto error;

	if (ipc_device_enqueue_message(sd->device, IPC_SMS_SEND_MSG,
				       IPC_TYPE_EXEC, data, size,
				       sms_send_cb, cbd) > 0) {
		return;
	}

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void finish_sms_send(uint16_t cmd, void *data, uint16_t length,
				uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct sms_data *sd = cbd->user;
	struct pending_pdu *pend;
	void *smsc = NULL;
	uint16_t smsc_size = 0;

	pend = g_queue_pop_head(sd->pending_pdu);
	if (pend == NULL) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	smsc_size = ipc_sms_svc_center_addr_smsc_size_extract(data, length);
	if (smsc_size == 0) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	smsc = ipc_sms_svc_center_addr_smsc_extract(data, length);
	if (smsc == NULL) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	sms_send(sd, smsc, smsc_size, pend->pdu, pend->pdu_len, cbd);
}

static void samsungipc_sms_send(struct ofono_sms *sms,
				const unsigned char *pdu,
				int pdu_len, int tpdu_len, int mms,
				ofono_sms_submit_cb_t cb, void *data)
{
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct cb_data *cbd;
	int smsc_len;

	DBG("pdu_len %d tpdu_len %d mms %d", pdu_len, tpdu_len, mms);

	cbd = cb_data_new(cb, data);

	/*
	 * SMSC address:
	 *
	 * smsc_len == 1, then zero-length SMSC was spec'd
	 */
	smsc_len = pdu_len - tpdu_len;

	if (smsc_len == 1) {
		/* Find default SMSC, then send */
		struct pending_pdu *pend = g_try_new0(struct pending_pdu, 1);
		if (pend == NULL)
			goto error;

		pend->pdu = pdu + smsc_len;
		pend->pdu_len = tpdu_len;

		g_queue_push_tail(sd->pending_pdu, pend);

		if (ipc_device_enqueue_message(sd->device,
					       IPC_SMS_SVC_CENTER_ADDR,
					       IPC_TYPE_GET, NULL, 0,
					       finish_sms_send, cbd) > 0) {
			return;
		}

		g_queue_pop_tail(sd->pending_pdu);
	} else {
		sms_send(sd, pdu, smsc_len, pdu + smsc_len, tpdu_len, cbd);
		return;
	}

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void sms_deliver_report_cb(uint16_t cmd, void *data, uint16_t length,
				uint8_t error, void *user_data)
{
	struct ipc_sms_deliver_report_response_data *resp = data;

	if (resp->ack != IPC_SMS_ACK_NO_ERROR)
		ofono_debug("Failed to acknowledge receipt of SMS");
	else
		ofono_debug("Succesfully acknowledged receipt of SMS");
}

static void sms_incoming_msg_cb(uint16_t cmd, void *data, uint16_t length,
				 void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *sd = ofono_sms_get_data(sms);
	struct ipc_sms_incoming_msg_header *header = data;
	struct ipc_sms_deliver_report_request_data report_data;
	uint16_t pdu_size, smsc_len;
	const unsigned char *pdu;

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
	report_data.type = IPC_SMS_TYPE_STATUS_REPORT;
	report_data.ack = IPC_SMS_ACK_NO_ERROR;
	report_data.id = header->id;

	ipc_device_enqueue_message(sd->device, IPC_SMS_DELIVER_REPORT,
			IPC_TYPE_EXEC, &report_data,
			sizeof(struct ipc_sms_deliver_report_request_data),
			sms_deliver_report_cb, NULL);
}

static int samsungipc_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user_data)
{
	struct sms_data *sd;

	sd = g_new0(struct sms_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	ofono_sms_set_data(sms, sd);

	sd->pending_pdu = g_queue_new();
	sd->device = user_data;

	sd->sms_incoming_msg_watch = ipc_device_add_notification_watch(sd->device,
							IPC_SMS_INCOMING_MSG,
							sms_incoming_msg_cb,
							sms);

	return 0;
}

static void samsungipc_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *sd = ofono_sms_get_data(sms);

	ipc_device_remove_watch(sd->device, sd->sms_incoming_msg_watch);

	ofono_sms_set_data(sms, NULL);

	if (sd == NULL)
		return;

	g_free(sd);
}

static const struct ofono_sms_driver driver = {
	.name		= "samsungipcmodem",
	.probe		= samsungipc_sms_probe,
	.remove		= samsungipc_sms_remove,
	.sca_query	= samsungipc_smsc_query,
	.sca_set	= samsungipc_smsc_set,
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
