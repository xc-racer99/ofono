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
#include <string.h>

#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include <samsung-ipc.h>

struct ofono_modem *modem;

static int samsungipcmgr_init(void)
{
	int ret;

	if (ipc_device_detect() < 0) {
		/* No devices found, just exit */
		return 0;
	}

	modem = ofono_modem_create(NULL, "samsungipc");
	if (modem == NULL) {
		DBG("ofono_modem_create failed for samsungipc");
		return -ENODEV;
	}

	/* This causes driver->probe() to be called... */
	ret = ofono_modem_register(modem);
	if (ret != 0) {
		ofono_error("%s: ofono_modem_register returned: %d",
				__func__, ret);
		return ret;
	}

	return 0;
}

static void samsungipcmgr_exit(void)
{
	if (modem != NULL)
		ofono_modem_remove(modem);
}

OFONO_PLUGIN_DEFINE(samsungipcmgr, "Samsung IPC Modem Init Daemon detection",
			VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
			samsungipcmgr_init, samsungipcmgr_exit)
