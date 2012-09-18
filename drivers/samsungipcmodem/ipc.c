/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2012 Simon Busch <morphis at gravedo.de>. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <ofono/log.h>

#include "ipc.h"

struct ipc_request {
	uint16_t command;
	uint8_t type;
	uint8_t id;
	void *data;
	uint16_t length;
	ipc_response_func_t cb;
	void *cb_data;
};

struct ipc_device {
	int ref_count;
	int fd;
	GIOChannel *io;
	bool close_on_unref;
	guint read_watch;
	guint write_watch;
	ipc_debug_func_t debug_func;
	void *debug_data;
	struct ipc_client *client;
	uint8_t next_id;
	GQueue *req_queue;
	GQueue *wait_queue;
	GList *notification_watches;
	guint next_watch_id;
};

struct notication_watch {
	guint id;
	uint8_t type;
	uint16_t cmd;
	ipc_notify_func_t notify_cb;
	void *notify_data;
};

static void __debug_device(struct ipc_device *device,
						const char *format, ...)
{
	char strbuf[72 + 16];
	va_list ap;

	if (!device->debug_func)
		return;

	va_start(ap, format);
	vsnprintf(strbuf, sizeof(strbuf), format, ap);
	va_end(ap);

	device->debug_func(strbuf, device->debug_data);
}

static guint next_message_id(struct ipc_device *device)
{
	if (device->next_id == 255)
		device->next_id = 1;

	return device->next_id++;
}

static guint next_watch_id(struct ipc_device *device)
{
	if (device->next_watch_id == G_MAXUINT)
		device->next_watch_id = 1;

	return device->next_watch_id++;
}

static gint __request_compare(gconstpointer a, gconstpointer b)
{
	const struct ipc_request *req = a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return req->id - id;
}

static void handle_response(struct ipc_device *device, struct ipc_message *resp)
{
	GList *list;
	struct ipc_request *req;

	list = g_queue_find_custom(device->wait_queue,
			GUINT_TO_POINTER(resp->aseq), __request_compare);
	if (!list) {
		__debug_device(device, "Did not found corresponding request for received response message");
		return;
	}

	req = list->data;
	if (req->cb)
		req->cb(resp->command, resp->data, resp->size, 0, req->cb_data);
}

static void handle_notification(struct ipc_device *device, struct ipc_message *resp)
{
	GList *list;

	for (list = g_list_first(device->notification_watches); list;
						list = g_list_next(list)) {
		struct notication_watch *watch = list->data;

		if (!watch)
			continue;

		if (watch->type == resp->type && watch->cmd == resp->command)
			watch->notify_cb(resp->command, resp->data, resp->size, watch->notify_data);
	}
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							  gpointer user_data)
{
	struct ipc_device *device = user_data;
	struct ipc_message resp;
	int ret;

	if (cond & G_IO_NVAL)
		return FALSE;

	ret = ipc_client_recv(device->client, &resp);
	if (ret < 0) {
		ofono_error("Could not receive IPC message from modem");
		return FALSE;
	}

	switch (resp.type) {
	case IPC_TYPE_RESP:
		ofono_debug("Received IPC response message [id=%i, type=%s cmd=%s]",
				resp.aseq, ipc_response_type_string(resp.type),
				ipc_command_string(resp.command));
		handle_response(device, &resp);
		break;
	case IPC_TYPE_NOTI:
	case IPC_TYPE_INDI:
		ofono_debug("Received IPC %s message [id=%i, type=%s, cmd=%s]",
				resp.type == IPC_TYPE_NOTI ? "notification" : "indication",
				resp.aseq, ipc_response_type_string(resp.type),
				ipc_command_string(resp.command));
		handle_notification(device, &resp);
		break;
		break;
	default:
		ofono_error("Received unhandled IPC message type [id=%i, type=%s, cmd=%s]",
				resp.aseq, ipc_response_type_string(resp.type),
				ipc_command_string(resp.command));
		break;
	}

	// FIXME this leads to a SEGV somehow:
	// g_source_callback_ref (cb_data=0x0) at /build/buildd/glib2.0-2.32.3/./glib/gmain.c:1275
	// 1275/build/buildd/glib2.0-2.32.3/.32/glib/gmain.c: Datei oder Verzeichnis nicht gefunden.
	// ipc_client_response_free(device->client, &resp);

	return TRUE;
}

static void read_watch_destroy(gpointer user_data)
{
	struct ipc_device *device = user_data;

	device->read_watch = 0;
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct ipc_device *device = user_data;
	struct ipc_request *req;

	req = g_queue_pop_head(device->req_queue);
	if (!req)
		return FALSE;

	ofono_debug("Sending IPC request message [id=%i, cmd=%s, type=%s]",
			req->id, ipc_command_string(req->command),
			ipc_request_type_string(req->type));

	ipc_client_send(device->client, req->id, req->command, req->type,
				req->data, req->length);

	// FIXME start timeout for message

	g_free(req->data);
	req->data = NULL;

	if (req->cb != NULL)
		g_queue_push_tail(device->wait_queue, req);

	if (g_queue_get_length(device->req_queue) > 0)
		return TRUE;

	return FALSE;
}

static void write_watch_destroy(gpointer user_data)
{
	struct ipc_device *device = user_data;

	device->write_watch = 0;
}

static void wakeup_writer(struct ipc_device *device)
{
	if (device->write_watch > 0)
		return;

	device->write_watch = g_io_add_watch_full(device->io, G_PRIORITY_HIGH,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, device, write_watch_destroy);
}

struct ipc_device *ipc_device_new(int fd, struct ipc_client *client)
{
	struct ipc_device *device;

	device = g_try_new0(struct ipc_device, 1);
	if (!device)
		return NULL;

	device->ref_count = 1;
	device->fd = fd;
	device->next_id = 1;
	device->next_watch_id = 1;
	device->client = client;
	device->close_on_unref = false;
	device->req_queue = g_queue_new();
	device->wait_queue = g_queue_new();
	device->notification_watches = g_list_alloc();

	device->io = g_io_channel_unix_new(device->fd);

	g_io_channel_set_encoding(device->io, NULL, NULL);
	g_io_channel_set_buffered(device->io, FALSE);

	device->read_watch = g_io_add_watch_full(device->io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, device, read_watch_destroy);

	g_io_channel_unref(device->io);

	return device;
}

struct ipc_device *ipc_device_ref(struct ipc_device *device)
{
	if (!device)
		return NULL;

	__sync_fetch_and_add(&device->ref_count, 1);

	return device;
}

static void free_notification_watch(gpointer data)
{
	struct notication_watch *watch = data;
	free(watch);
}

void ipc_device_unref(struct ipc_device *device)
{
	if (!device)
		return;

	if (__sync_sub_and_fetch(&device->ref_count, 1))
		return;

	__debug_device(device, "device %p free", device);

	if (device->write_watch > 0)
		g_source_remove(device->write_watch);

	if (device->read_watch > 0)
		g_source_remove(device->read_watch);

	if (device->close_on_unref)
		close(device->fd);

	g_list_free_full(device->notification_watches, free_notification_watch);

	g_free(device);
}

static guint add_response_watch(struct ipc_device *device, uint8_t type, uint16_t cmd,
							ipc_notify_func_t notify_cb, void *user_data)
{
	struct notication_watch *watch;

	watch = g_try_new0(struct notication_watch, 1);
	if (!watch)
		return -1;

	watch->id = next_watch_id(device);
	watch->type = type;
	watch->cmd = cmd;
	watch->notify_cb = notify_cb;
	watch->notify_data = user_data;

	device->notification_watches = g_list_append(device->notification_watches, watch);

	return watch->id;
}

guint ipc_device_add_notifcation_watch(struct ipc_device *device, uint16_t cmd,
							ipc_notify_func_t notify_cb, void *user_data)
{
	return add_response_watch(device, IPC_TYPE_NOTI, cmd, notify_cb, user_data);
}

guint ipc_device_add_indication_watch(struct ipc_device *device, uint16_t cmd,
							ipc_notify_func_t notify_cb, void *user_data)
{
	return add_response_watch(device, IPC_TYPE_INDI, cmd, notify_cb, user_data);
}

static gint __notification_watch_id_compare(gconstpointer a, gconstpointer b)
{
	const struct notication_watch *watch = a;
	const guint id = GPOINTER_TO_UINT(b);

	if (a == NULL || b == NULL)
		return -1;

	return watch->id - id;
}

static void remove_response_watch(struct ipc_device *device, guint id)
{
	GList *watches;

	watches = g_list_find_custom(device->notification_watches,
						GUINT_TO_POINTER(id), __notification_watch_id_compare);

	if (watches == NULL)
		return;

	device->notification_watches = g_list_delete_link(device->notification_watches, watches);
}

void ipc_device_remove_watch(struct ipc_device *device, guint id)
{
	remove_response_watch(device, id);
}

int ipc_device_get_fd(struct ipc_device *device)
{
	if (!device)
		return -1;

	return device->fd;
}

struct ipc_client *ipc_device_get_client(struct ipc_device *device)
{
	if (!device)
		return NULL;

	return device->client;
}

int ipc_device_enqueue_message(struct ipc_device *device,
			uint16_t command, uint8_t type, void *data, uint16_t length,
			ipc_response_func_t cb, void *user_data)
{
	struct ipc_request *req;

	req = g_try_new0(struct ipc_request, 1);
	if (!req)
		return -1;

	req->command = command;
	req->type = type;
	req->cb = cb;
	req->cb_data = user_data;
	req->id = next_message_id(device);
	req->data = data;
	req->length = length;

	g_queue_push_tail(device->req_queue, req);

	wakeup_writer(device);

	return req->id;
}

void ipc_device_set_debug(struct ipc_device *device,
				ipc_debug_func_t func, void *user_data)
{
	if (device == NULL)
		return;

	device->debug_func = func;
	device->debug_data = user_data;
}

void ipc_device_set_close_on_unref(struct ipc_device *device, bool do_close)
{
	if (!device)
		return;

	device->close_on_unref = do_close;
}
