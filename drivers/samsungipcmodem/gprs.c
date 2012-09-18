/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis at gravedo.de>. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include <glib.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"

struct gprs_data {
	struct ipc_device *device;
	guint status_watch;
	guint hsdpa_status_watch;
};

static void samsungipc_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	/* No attach/detach possible so always succeed here */
	CALLBACK_WITH_SUCCESS(cb, data);
}

static void gprs_attached_status_cb(uint16_t cmd, void *data, uint16_t length, uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ipc_net_regist_response_data *resp = data;

	if (error) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		goto done;
	}

	CALLBACK_WITH_SUCCESS(cb, resp->status - 1, cbd->data);

done:
	g_free(cbd);
}

static void samsungipc_attached_status(struct ofono_gprs *gprs,
				ofono_gprs_status_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd;
	struct ipc_net_regist_request_data *req;

	DBG("");

	req = g_try_new0(struct ipc_net_regist_request_data, 1);
	if (!req) {
		CALLBACK_WITH_FAILURE(cb, -1, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	ipc_net_regist_setup(req, IPC_NET_SERVICE_DOMAIN_GPRS);

	if(ipc_device_enqueue_message(gd->device, IPC_NET_REGIST, IPC_TYPE_GET,
						req, sizeof(struct ipc_net_regist_request_data),
						gprs_attached_status_cb, cbd) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
	g_free(req);
}

static void notify_gprs_status_cb(uint16_t cmd, void *data, uint16_t length, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ipc_net_regist_response_data *resp = data;

	if (resp->domain != IPC_NET_SERVICE_DOMAIN_GPRS)
		return;

	ofono_gprs_status_notify(gprs, resp->status - 1);
}

static void gprs_status_cb(uint16_t cmd, void *data, uint16_t length, uint8_t error, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ipc_net_regist_response_data *resp = data;

	if (resp->domain != IPC_NET_SERVICE_DOMAIN_GPRS || error)
		return;

	ofono_gprs_status_notify(gprs, resp->status - 1);
}

static void notify_hsdpa_status_cb(uint16_t cmd, void *data, uint16_t length, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct ipc_net_regist_request_data *req;

	DBG("");

	req = g_try_new0(struct ipc_net_regist_request_data, 1);
	if (!req)
		return;

	ipc_net_regist_setup(req, IPC_NET_SERVICE_DOMAIN_GPRS);

	if(ipc_device_enqueue_message(gd->device, IPC_NET_REGIST, IPC_TYPE_GET,
						req, sizeof(struct ipc_net_regist_request_data),
						gprs_status_cb, gprs) > 0)
		return;

	g_free(req);
}

static void initial_status_cb(uint16_t cmd, void *data, uint16_t length, void *user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct ipc_net_regist_response_data *resp = data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	if (resp->domain != IPC_NET_SERVICE_DOMAIN_GPRS)
		return;

	ofono_gprs_register(gprs);

	ofono_gprs_status_notify(gprs, resp->status - 1);

	ipc_device_remove_watch(gd->device, gd->status_watch);

	gd->status_watch = ipc_device_add_notifcation_watch(gd->device, IPC_NET_REGIST,
								notify_gprs_status_cb, gprs);
}

static int samsungipc_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *user_data)
{
	struct gprs_data *gd;
	struct ipc_client *client;
	struct ipc_client_gprs_capabilities caps;

	DBG("");

	gd = g_new0(struct gprs_data, 1);
	gd->device = user_data;

	ofono_gprs_set_data(gprs, gd);

	gd->status_watch = ipc_device_add_notifcation_watch(gd->device, IPC_NET_REGIST,
												initial_status_cb, gprs);
	gd->hsdpa_status_watch = ipc_device_add_notifcation_watch(gd->device, IPC_GPRS_HSDPA_STATUS,
														notify_hsdpa_status_cb, gprs);

	client = ipc_device_get_client(gd->device);
	ipc_client_gprs_get_capabilities(client, &caps);

	ofono_gprs_set_cid_range(gprs, 1, caps.cid_count);

	return 0;
}

static void samsungipc_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	DBG("");

	ipc_device_remove_watch(gd->device, gd->status_watch);

	ofono_gprs_set_data(gprs, NULL);

	g_free(gd);
}

static struct ofono_gprs_driver driver = {
	.name			= "samsungipcmodem",
	.probe			= samsungipc_gprs_probe,
	.remove			= samsungipc_gprs_remove,
	.attached_status	= samsungipc_attached_status,
	.set_attached	= samsungipc_set_attached,
};

void samsungipc_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void samsungipc_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
