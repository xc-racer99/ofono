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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include <glib.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"

#define IP_MAX_LENGTH	16

struct gprs_context_data {
	struct ipc_device *device;
	unsigned int active_context;
	unsigned int enabled;
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
};

static void notify_gprs_ip_configuration_cb(uint16_t cmd, void *data,
					    uint16_t length, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ipc_gprs_ip_configuration_data *resp = data;
	struct ipc_client *client;
	char local[IP_MAX_LENGTH];
	char dns1[IP_MAX_LENGTH];
	char dns2[IP_MAX_LENGTH];
	char gateway[IP_MAX_LENGTH];
	char subnetmask[IP_MAX_LENGTH];
	const char *dns[3];
	char *interface;

	snprintf(local, IP_MAX_LENGTH, "%i.%i.%i.%i", resp->ip[0], resp->ip[1],
			 resp->ip[2], resp->ip[3]);
	snprintf(dns1, IP_MAX_LENGTH, "%i.%i.%i.%i", resp->dns1[0],
		 resp->dns1[1], resp->dns1[2], resp->dns1[3]);
	snprintf(dns2, IP_MAX_LENGTH, "%i.%i.%i.%i", resp->dns2[0],
		 resp->dns2[1], resp->dns2[2], resp->dns2[3]);
	snprintf(local, IP_MAX_LENGTH, "%i.%i.%i.%i", resp->gateway[0],
		 resp->gateway[1], resp->gateway[2], resp->gateway[3]);
	snprintf(local, IP_MAX_LENGTH, "%i.%i.%i.%i", resp->subnet_mask[0],
		 resp->subnet_mask[1], resp->subnet_mask[2],
		 resp->subnet_mask[3]);

	dns[0] = dns1;
	dns[1] = dns2;
	dns[2] = 0;

	client = ipc_device_get_fmt_client(gcd->device);
	interface = ipc_client_gprs_get_iface(client, gcd->active_context);

	ofono_gprs_context_set_interface(gc, interface);
	ofono_gprs_context_set_ipv4_address(gc, local, TRUE);
	ofono_gprs_context_set_ipv4_netmask(gc, subnetmask);
	ofono_gprs_context_set_ipv4_dns_servers(gc, dns);
	ofono_gprs_context_set_ipv4_gateway(gc, gateway);

	gcd->enabled = 1;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	g_free(cbd);
	g_free(interface);
}

static void pdp_context_activate_cb(uint16_t cmd, void *data, uint16_t length,
				    uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_gprs_context *gc = cbd->user;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	if (error) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		gcd->active_context = 0;
		g_free(cbd);
		return;
	}

	ipc_device_add_notification_watch(gcd->device,
					 IPC_GPRS_IP_CONFIGURATION,
					 notify_gprs_ip_configuration_cb, cbd);
}

static void define_pdp_context_cb(uint16_t cmd, void *data, uint16_t length,
				  uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ipc_gen_phone_res_data *resp = data;
	struct ipc_gprs_pdp_context_request_set_data *req;

	if (error || ipc_gen_phone_res_check(resp) < 0)
		goto error;

	req = g_try_new0(struct ipc_gprs_pdp_context_request_set_data, 1);
	if (req == NULL)
		goto error;

	/* activate the context */
	ipc_gprs_pdp_context_request_set_setup(req, 1, gcd->active_context,
					       gcd->username, gcd->password);

	if (ipc_device_enqueue_message(gcd->device, IPC_GPRS_DEFINE_PDP_CONTEXT,
			IPC_TYPE_SET, req,
			sizeof(struct ipc_gprs_pdp_context_request_set_data),
			pdp_context_activate_cb, cbd) > 0)
		return;

error:
	gcd->active_context = 0;
	CALLBACK_WITH_FAILURE(cb, cbd->data);
	g_free(cbd);
}

static void samsungipc_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ipc_gprs_define_pdp_context_data *req;
	struct cb_data *cbd;

	req = g_try_new0(struct ipc_gprs_define_pdp_context_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = gc;

	/* IPv6 support not implemented */
	if (ctx->proto != OFONO_GPRS_PROTO_IP)
		goto error;

	DBG("cid %u", ctx->cid);

	gcd->active_context = ctx->cid;
	memcpy(gcd->username, ctx->username, sizeof(ctx->username));
	memcpy(gcd->password, ctx->password, sizeof(ctx->password));

	req->cid = ctx->cid;
	req->enable = 1; /* enable the context */
	strncpy((char *) req->apn, ctx->apn, 124);

	if (ipc_device_enqueue_message(gcd->device,
			IPC_GPRS_DEFINE_PDP_CONTEXT, IPC_TYPE_SET, req,
			sizeof(struct ipc_gprs_define_pdp_context_data),
			define_pdp_context_cb, cbd) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, data);
}

static void pdp_context_deactivate_cb(uint16_t cmd, void *data,
				      uint16_t length, uint8_t error,
				      void *user_data)
{
	struct ipc_gen_phone_res_data *resp = data;
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct gprs_context_data *gcd = cbd->user;

	if (error || ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		goto done;
	}

	gcd->enabled = 0;
	gcd->active_context = 0;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ipc_gprs_pdp_context_request_set_data *req;
	struct cb_data *cbd;

	DBG("cid %u", cid);

	req = g_try_new0(struct ipc_gprs_pdp_context_request_set_data, 1);
	if (req == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = gcd;

	/* deactivate the context */
	ipc_gprs_pdp_context_request_set_setup(req, 0, cid, NULL, NULL);

	if (ipc_device_enqueue_message(gcd->device,
			IPC_GPRS_DEFINE_PDP_CONTEXT,
			IPC_TYPE_SET, req,
			sizeof(struct ipc_gprs_pdp_context_request_set_data),
			pdp_context_deactivate_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void samsungipc_gprs_detach_shutdown(struct ofono_gprs_context *gc,
					    unsigned int cid)
{
	DBG("cid %u", cid);
}

static void notify_gprs_status_cb(uint16_t cmd, void *data, uint16_t length,
				  void *user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	struct ipc_gprs_call_status_data *resp = data;

	if (gcd->enabled &&
		(resp->status == IPC_GPRS_STATUS_NOT_ENABLED ||
		 resp->status == IPC_GPRS_STATUS_DISABLED)) {
		ofono_gprs_context_deactivated(gc, gcd->active_context);
	}
}

static int samsungipc_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	struct gprs_context_data *gcd;

	DBG("");

	gcd = g_try_new0(struct gprs_context_data, 1);
	if (gcd == NULL)
		return -ENOMEM;

	gcd->device = data;
	gcd->active_context = 0;
	gcd->enabled = 0;

	ofono_gprs_context_set_data(gc, gcd);

	ipc_device_add_notification_watch(gcd->device, IPC_GPRS_CALL_STATUS,
					 notify_gprs_status_cb, gc);

	return 0;
}

static void samsungipc_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	g_free(gcd);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "samsungipcmodem",
	.probe			= samsungipc_gprs_context_probe,
	.remove			= samsungipc_gprs_context_remove,
	.activate_primary	= samsungipc_gprs_activate_primary,
	.deactivate_primary	= samsungipc_gprs_deactivate_primary,
	.detach_shutdown	= samsungipc_gprs_detach_shutdown,
};

void samsungipc_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void samsungipc_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
