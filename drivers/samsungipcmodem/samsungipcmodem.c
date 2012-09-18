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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>

#include "samsungipcmodem.h"

static int samsungipcmodem_init(void)
{
	samsungipc_devinfo_init();
	samsungipc_sim_init();
	samsungipc_netreg_init();
	samsungipc_voicecall_init();
	samsungipc_gprs_init();
	return 0;
}

static void samsungipcmodem_exit(void)
{
	samsungipc_gprs_exit();
	samsungipc_voicecall_exit();
	samsungipc_netreg_exit();
	samsungipc_sim_exit();
	samsungipc_devinfo_exit();
}

OFONO_PLUGIN_DEFINE(samsungipcmodem, "Samsung IPC modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, samsungipcmodem_init, samsungipcmodem_exit)
