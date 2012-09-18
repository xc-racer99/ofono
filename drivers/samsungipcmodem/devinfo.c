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

#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>

#include <glib.h>

#include "samsungipcmodem.h"
#include "ipc.h"
#include "util.h"

struct devinfo_data {
	struct ipc_device *device;
};

static void samsungipc_query_manufacturer(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void samsungipc_query_model(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void misc_me_version_cb(uint16_t cmd, void *data, uint16_t length,
			       void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	struct ipc_misc_me_version_response_data *version = data;
	char *str;

	if (data == NULL || length < sizeof(struct ipc_misc_me_version_response_data)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	str = g_try_new0(char, 32);
	if (str == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	strncpy(str, version->software_version, 32);
	str[32] = '\0';

	CALLBACK_WITH_SUCCESS(cb, str, cbd->data);
}

static void samsungipc_query_revision(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	uint8_t *msg;

	msg = g_try_new0(uint8_t, 1);
	msg[0] = 0xff;

	ipc_device_enqueue_message(data->device, IPC_MISC_ME_VERSION,
				   IPC_TYPE_GET, msg, 1,
				   misc_me_version_cb, cbd);
}

static void misc_me_sn_cb(uint16_t cmd, void *data, uint16_t length,
			  void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_devinfo_query_cb_t cb = cbd->cb;
	struct ipc_misc_me_sn_response_data *sn = data;
	char *str;

	if (data == NULL || length < sizeof(struct ipc_misc_me_sn_response_data)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	str = g_try_new0(char, sn->length);
	if (str == NULL) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	strncpy(str, sn->data, sn->length);
	str[sn->length] = '\0';

	CALLBACK_WITH_SUCCESS(cb, str, cbd->data);
}

static void samsungipc_query_serial(struct ofono_devinfo *devinfo,
				ofono_devinfo_query_cb_t cb, void *user_data)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	uint8_t *msg;

	msg = g_try_new0(uint8_t, 1);
	msg[0] = IPC_MISC_ME_SN_SERIAL_NUM;

	ipc_device_enqueue_message(data->device, IPC_MISC_ME_SN,
				   IPC_TYPE_GET, msg, 1, misc_me_sn_cb, cbd);
}

static void reachable_cb(uint16_t cmd, void *data, uint16_t length,
			 void *user_data)
{
	struct ofono_devinfo *info = user_data;

	if (data == NULL || length < sizeof(struct ipc_misc_me_version_response_data))
		ofono_devinfo_remove(info);
	else
		ofono_devinfo_register(info);
}

static int samsungipc_devinfo_probe(struct ofono_devinfo *devinfo,
				unsigned int vendor, void *user_data)
{
	struct devinfo_data *data;
	struct ipc_device *device = user_data;
	uint8_t *msg;

	DBG("");

	data = g_new0(struct devinfo_data, 1);

	data->device = device;

	ofono_devinfo_set_data(devinfo, data);

	/*
	 * Just issue a IPC_MISC_ME_VERSION request here to check modem
	 * availability and defer ofono_devinfo_register call
	 */
	msg = g_try_new0(uint8_t, 1);
	msg[0] = 0xff;
	ipc_device_enqueue_message(data->device, IPC_MISC_ME_VERSION,
				   IPC_TYPE_GET, msg, 1, reachable_cb,
				   devinfo);

	return 0;
}

static void samsungipc_devinfo_remove(struct ofono_devinfo *devinfo)
{
	struct devinfo_data *data = ofono_devinfo_get_data(devinfo);

	DBG("");

	ofono_devinfo_set_data(devinfo, NULL);

	g_free(data);
}

static struct ofono_devinfo_driver driver = {
	.name				= "samsungipcmodem",
	.probe				= samsungipc_devinfo_probe,
	.remove				= samsungipc_devinfo_remove,
	.query_manufacturer	= samsungipc_query_manufacturer,
	.query_model		= samsungipc_query_model,
	.query_revision		= samsungipc_query_revision,
	.query_serial		= samsungipc_query_serial,
};

void samsungipc_devinfo_init(void)
{
	ofono_devinfo_driver_register(&driver);
}

void samsungipc_devinfo_exit(void)
{
	ofono_devinfo_driver_unregister(&driver);
}
