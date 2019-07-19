/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis@gravedo.de>. All rights reserved.
 *
 *  Some parts of the code below are copied from drivers/qmimodem/qmi.c and are
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include <stdbool.h>
#include <stdint.h>

#include <samsung-ipc.h>

typedef void (*ipc_response_func_t)(uint16_t cmd, void *data, uint16_t length, uint8_t error, void *user_data);
typedef void (*ipc_notify_func_t)(uint16_t cmd, void *data, uint16_t length, void *user_data);

struct ipc_device;

struct ipc_device *ipc_device_new(struct ipc_client *fmt_client, struct ipc_client *rfs_client);
void ipc_device_close(struct ipc_device *device);

guint ipc_device_add_notification_watch(struct ipc_device *device, uint16_t cmd,
							ipc_notify_func_t notify_cb, void *user_data);
guint ipc_device_add_indication_watch(struct ipc_device *device, uint16_t cmd,
							ipc_notify_func_t notify_cb, void *user_data);

void ipc_device_remove_watch(struct ipc_device *device, guint id);

struct ipc_client *ipc_device_get_fmt_client(struct ipc_device *device);

int ipc_device_enqueue_message(struct ipc_device *device,
			uint16_t command, uint8_t type, void *data, uint16_t length,
			ipc_response_func_t cb, void *user_data);
