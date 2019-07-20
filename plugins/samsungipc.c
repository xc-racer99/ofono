/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis@gravedo.de>
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

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/phonebook.h>
#include <ofono/voicecall.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/sms.h>

#include "drivers/samsungipcmodem/ipc.h"
#include "drivers/samsungipcmodem/util.h"

#include <samsung-ipc.h>

struct samsungipc_data {
	struct ipc_client *fmt_client;
	struct ipc_client *rfs_client;
	struct ipc_device *device;
	guint pwr_up_watch;
};

static int samsungipc_probe(struct ofono_modem *modem)
{
	struct samsungipc_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct samsungipc_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void samsungipc_remove(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static void power_status_cb(uint16_t cmd, void *data, uint16_t length,
			    void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsungipc_data *sid = ofono_modem_get_data(modem);
	struct ipc_pwr_phone_state_request_data *request_data;

	request_data = g_try_new0(struct ipc_pwr_phone_state_request_data, 1);
	if (request_data == NULL)
		return;

	/* Send a power-off request so we know that we're powered off */
	request_data->state = IPC_PWR_PHONE_STATE_REQUEST_LPM;
	ipc_device_enqueue_message(sid->device, IPC_PWR_PHONE_STATE,
			IPC_TYPE_EXEC, request_data,
			sizeof(struct ipc_pwr_phone_state_request_data),
			NULL, NULL);

	ofono_modem_set_powered(modem, TRUE);

	ipc_device_remove_watch(sid->device, sid->pwr_up_watch);

	sid->pwr_up_watch = 0;
}

static void log_handler(void *user_data, const char *message)
{
	ofono_debug("IPC: %s", message);
}

static int samsungipc_enable(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);
	int err;

	DBG("%p", modem);

	/* Create FMT client */
	data->fmt_client = ipc_client_create(IPC_CLIENT_TYPE_FMT);
	if (data->fmt_client == NULL) {
		ofono_debug("IPC: failed to create FMT client");
		return -ENODEV;
	}

	if (getenv("OFONO_IPC_DEBUG"))
		ipc_client_log_callback_register(data->fmt_client,
						 log_handler, data);

	err = ipc_client_data_create(data->fmt_client);
	if (err < 0) {
		ofono_debug("IPC: failed to create FMT client data");
		return err;
	}

	err = ipc_client_boot(data->fmt_client);
	if (err < 0) {
		ofono_debug("IPC: failed to create boot client");
		return err;
	}

	usleep(300);

	err = ipc_client_open(data->fmt_client);
	if (err < 0) {
		ofono_debug("IPC: failed to open FMT client");
		return err;
	}

	err = ipc_client_power_on(data->fmt_client);
	if (err < 0) {
		ofono_debug("IPC: failed to power on FMT client");
		return err;
	}

	/* Create RFS client */
	data->rfs_client = ipc_client_create(IPC_CLIENT_TYPE_RFS);
	if (data->rfs_client == NULL) {
		ofono_debug("IPC: failed to create RFS client");
		return -EIO;
	}

	err = ipc_client_data_create(data->rfs_client);
	if (err < 0) {
		ofono_debug("IPC: failed to create RFS client data");
		return err;
	}

	if (getenv("OFONO_IPC_DEBUG"))
		ipc_client_log_callback_register(data->rfs_client,
						 log_handler, data);

	err = ipc_client_open(data->rfs_client);
	if (err < 0) {
		ofono_debug("IPC: failed to open RFS client");
		return err;
	}

	/* Now that both clients are setup, we can pass them to our driver */
	data->device = ipc_device_new(data->fmt_client, data->rfs_client);

	/* Add a watch for the initial powerup */
	data->pwr_up_watch = ipc_device_add_notification_watch(data->device,
							IPC_PWR_PHONE_PWR_UP,
							power_status_cb,
							modem);

	ofono_debug("IPC: initial setup completed");

	return -EINPROGRESS;
}

static int samsungipc_disable(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);

	DBG("%s", __func__);

	if (data->pwr_up_watch > 0)
		ipc_device_remove_watch(data->device, data->pwr_up_watch);

	ipc_client_close(data->fmt_client);
	ipc_client_power_off(data->fmt_client);

	ipc_client_close(data->rfs_client);
	ipc_client_power_off(data->rfs_client);

	ipc_device_close(data->device);

	return 0;
}

static void samsungipc_pre_sim(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "samsungipcmodem", data->device);
	ofono_sim_create(modem, 0, "samsungipcmodem", data->device);
}

static void samsungipc_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void notify_power_state_cb(uint16_t cmd, void *data, uint16_t length,
				    uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	g_free(cbd);
}

static void set_device_rf_power_state(struct ofono_modem *modem,
				      uint16_t state, struct cb_data *cbd)
{
	struct samsungipc_data *sid = ofono_modem_get_data(modem);
	struct ipc_pwr_phone_state_request_data *request_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	request_data = g_try_new0(struct ipc_pwr_phone_state_request_data, 1);
	if (request_data == NULL) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
		return;
	}

	request_data->state = state;

	switch (state) {
	case IPC_PWR_PHONE_STATE_REQUEST_NORMAL:
		if (ipc_device_enqueue_message(sid->device, IPC_PWR_PHONE_STATE,
				IPC_TYPE_EXEC, request_data,
				sizeof(struct ipc_pwr_phone_state_request_data),
				notify_power_state_cb, cbd) < 0) {
			CALLBACK_WITH_FAILURE(cb, cbd->data);
			g_free(cbd);
			ofono_debug("Failed changing power state to NORMAL");
		}

		ofono_debug("Done sending power state change to NORMAL level");

		break;
	case IPC_PWR_PHONE_STATE_REQUEST_LPM:
		/*
		 * We will not get any response for this request,
		 * so assume success all the time.
		 *
		 * TODO Some devices do put out a IPC_PWR_PHONE_STATE
		 * after this that we could check.
		 */
		ipc_device_enqueue_message(sid->device, IPC_PWR_PHONE_STATE,
				IPC_TYPE_EXEC, request_data,
				sizeof(struct ipc_pwr_phone_state_request_data),
				NULL, NULL);

		ofono_debug("Done sending power state change to LPM level");

		CALLBACK_WITH_SUCCESS(cb, cbd->data);
		g_free(cbd);

		break;
	default:
		/* Not supported power state */
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);

		break;
	}
}

static void samsungipc_set_online(struct ofono_modem *modem,
				  ofono_bool_t online,
				  ofono_modem_online_cb_t cb,
				  void *user_data)
{
	struct samsungipc_data *sid = ofono_modem_get_data(modem);
	struct cb_data *cbd;
	uint16_t state;

	state = online ? IPC_PWR_PHONE_STATE_REQUEST_NORMAL :
			 IPC_PWR_PHONE_STATE_REQUEST_LPM;

	cbd = cb_data_new(cb, user_data);
	cbd->user = sid;

	set_device_rf_power_state(modem, state, cbd);
}

static void samsungipc_post_online(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_netreg_create(modem, 0, "samsungipcmodem", data->device);
	ofono_sms_create(modem, 0, "samsungipcmodem", data->device);
	ofono_voicecall_create(modem, 0, "samsungipcmodem", data->device);
	gprs = ofono_gprs_create(modem, 0, "samsungipcmodem", data->device);
	gc = ofono_gprs_context_create(modem, 0, "samsungipcmodem",
				       data->device);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static struct ofono_modem_driver samsungipc_driver = {
	.name		= "samsungipc",
	.probe		= samsungipc_probe,
	.remove		= samsungipc_remove,
	.enable		= samsungipc_enable,
	.disable	= samsungipc_disable,
	.pre_sim	= samsungipc_pre_sim,
	.post_sim	= samsungipc_post_sim,
	.set_online	= samsungipc_set_online,
	.post_online	= samsungipc_post_online,
};

static int samsungipc_init(void)
{
	int err;

	err = ofono_modem_driver_register(&samsungipc_driver);
	if (err < 0)
		return err;

	return 0;
}

static void samsungipc_exit(void)
{
	ofono_modem_driver_unregister(&samsungipc_driver);
}

OFONO_PLUGIN_DEFINE(samsungipc, "Samsung IPC driver", VERSION,
	OFONO_PLUGIN_PRIORITY_DEFAULT, samsungipc_init, samsungipc_exit)
