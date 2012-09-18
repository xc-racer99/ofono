/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis at gravedo.de>
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
	struct ipc_client *client;
	struct ipc_device *device;
	uint16_t state;
	guint power_state_watch;
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

static void retrieve_power_state_cb(uint16_t cmd, void *data, uint16_t length, uint8_t error, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct samsungipc_data *ipcdata = ofono_modem_get_data(modem);
	struct ipc_pwr_phone_state_response_data *resp;

	if (error) {
		ofono_error("Received error instead of power state response");
		return;
	}

	resp = (struct ipc_pwr_phone_state_response_data *) data;

	switch (resp->state) {
	case IPC_PWR_PHONE_STATE_RESPONSE_NORMAL:
		ofono_error("Modem is already in NORMAL power state; thats wrong!");
		// FIXME shutdown everything here or switch to LPM/perform a modem reset?
		break;
	case IPC_PWR_PHONE_STATE_RESPONSE_LPM:
		ofono_modem_set_powered(modem, TRUE);
		ipcdata->state = IPC_PWR_PHONE_STATE_RESPONSE_LPM;
		break;
	}
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

	data->client = ipc_client_create(IPC_CLIENT_TYPE_FMT);
	if (!data->client)
		return -ENODEV;

	if (getenv("OFONO_IPC_DEBUG"))
		ipc_client_log_callback_register(data->client, log_handler, data);

	err = ipc_client_data_create(data->client);
	if (err < 0)
		return err;

	err = ipc_client_boot(data->client);
	if (err < 0)
		return err;

	usleep(300);

	err = ipc_client_open(data->client);
	if (err < 0)
		return err;

	err = ipc_client_power_on(data->client);
	if (err < 0)
		return err;

	ipc_device_enqueue_message(data->device, IPC_PWR_PHONE_STATE, IPC_TYPE_GET,
						NULL, 0, retrieve_power_state_cb, modem);

	return -EINPROGRESS;
}

static int samsungipc_disable(struct ofono_modem *modem)
{
	struct samsungipc_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ipc_client_power_off(data->client);
	ipc_client_close(data->client);

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

static void notify_power_state_cb(uint16_t cmd, void *data, uint16_t length, void *user_data)
{
	struct cb_data *cbd = user_data;
	struct samsungipc_data *sid = cbd->user;
	ofono_modem_online_cb_t cb = cbd->cb;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	ipc_device_remove_watch(sid->device, sid->power_state_watch);

	g_free(cbd);
}

static void set_device_rf_power_state_cb(uint16_t cmd, void *data, uint16_t length, uint8_t error, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct samsungipc_data *sid = cbd->user;
	struct ipc_gen_phone_res_data *resp = data;

	if (error || ipc_gen_phone_res_check(resp) < 0) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		g_free(cbd);
		return;
	}

	sid->power_state_watch = ipc_device_add_notifcation_watch(sid->device, IPC_PWR_PHONE_STATE,
														notify_power_state_cb, cbd);
}

static void set_device_rf_power_state(struct ofono_modem *modem, uint16_t state, struct cb_data *cbd)
{
	struct samsungipc_data *sid = ofono_modem_get_data(modem);
	uint8_t *msg;
	ofono_modem_online_cb_t cb = cbd->cb;

	msg = g_try_new0(uint8_t, 2);
	if (!msg)
		return;

	memcpy(msg, &state, sizeof(uint16_t));

	switch (state) {
	case IPC_PWR_PHONE_STATE_REQUEST_NORMAL:
		ipc_device_enqueue_message(sid->device, IPC_PWR_PHONE_STATE, IPC_TYPE_EXEC, msg, 2,
							set_device_rf_power_state_cb, cbd);

		ofono_debug("Done sending power state change to NORMAL level");

		break;
	case IPC_PWR_PHONE_STATE_REQUEST_LPM:
		/* We will not get any response for this request! */
		ipc_device_enqueue_message(sid->device, IPC_PWR_PHONE_STATE, IPC_TYPE_EXEC, msg, 2,
							NULL, NULL);

		ofono_debug("Done sending power state change to LPM level");

		/* FIXME maybe retrieving power state from modem to make sure it's set? */

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

static void samsungipc_set_online(struct ofono_modem *modem, ofono_bool_t online,
							ofono_modem_online_cb_t cb, void *user_data)
{
	struct samsungipc_data *sid = ofono_modem_get_data(modem);
	struct cb_data *cbd;
	uint16_t state;

	state = online ? IPC_PWR_PHONE_STATE_REQUEST_NORMAL : IPC_PWR_PHONE_STATE_REQUEST_LPM;

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
	ofono_voicecall_create(modem, 0, "samsungipcmodem", data->device);
	gprs = ofono_gprs_create(modem, 0, "samsungipcmodem", data->device);
	gc = ofono_gprs_context_create(modem, 0, "samsungipcmodem", data->device);

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
