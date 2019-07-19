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
	guint fmt_watch;
	guint rfs_watch;
	guint write_watch;
	struct ipc_client *fmt_client;
	struct ipc_client *rfs_client;
	uint8_t next_id;
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

typedef struct {
	GSource parent;
	struct ipc_client *client;
} IpcSource;

typedef gboolean (*IpcSourceFunc) (struct ipc_message *resp,
                                   gpointer user_data);

static gboolean ipc_watch_prepare(GSource *source, gint *timeout_)
{
	IpcSource *ipc_source = (IpcSource *) source;
	struct timeval timeval;

	timeval.tv_sec = 0;
	timeval.tv_usec = 0;

	if (ipc_client_poll(ipc_source->client, NULL, &timeval))
		return TRUE;

	/* We are always ready to check again, no minimum time required */
	*timeout_ = 0;

	return FALSE;
}

static gboolean ipc_watch_dispatch(GSource *source, GSourceFunc callback,
							gpointer user_data)
{
	IpcSource *ipc_source = (IpcSource *) source;
	IpcSourceFunc func = (IpcSourceFunc) callback;
	struct ipc_message resp;
	int ret;

	if (func == NULL) {
		ofono_debug("%s: no callback func", __func__);
		return G_SOURCE_CONTINUE;
	}

	ret = ipc_client_recv(ipc_source->client, &resp);
	if (ret < 0) {
		ofono_debug("%s: failed to recv from client", __func__);
		return G_SOURCE_CONTINUE;
	}

	return func(&resp, user_data);
}

static GSourceFuncs ipc_watch_funcs = {
	ipc_watch_prepare,
	NULL, /* check */
	ipc_watch_dispatch,
	NULL, /* finalize */
};

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

static gint __request_compare_aseq(gconstpointer a, gconstpointer b)
{
	const struct ipc_request *req = a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return req->id - id;
}

static gint __request_compare_command(gconstpointer a, gconstpointer b)
{
	const struct ipc_request *req = a;
	uint16_t command = GPOINTER_TO_UINT(b);

	return req->command - command;
}

static void handle_response(struct ipc_device *device, struct ipc_message *resp)
{
	GList *list;
	struct ipc_request *req;

	if (ipc_seq_valid(resp->aseq)) {
		list = g_queue_find_custom(device->wait_queue,
				GUINT_TO_POINTER(resp->aseq),
				__request_compare_aseq);
	} else {
		/* Some devices don't always set aseq properly, so look up via
		 * response's command type
		 */
		list = g_queue_find_custom(device->wait_queue,
				GUINT_TO_POINTER(resp->command),
				__request_compare_command);
	}

	if (list == NULL) {
		ofono_debug("No corresponding request for received response");
		return;
	}

	req = list->data;
	if (req->cb)
		req->cb(resp->command, resp->data, resp->size, 0,
			req->cb_data);
}

static void handle_notification(struct ipc_device *device,
				struct ipc_message *resp)
{
	GList *list;

	for (list = g_list_first(device->notification_watches); list;
						list = g_list_next(list)) {
		struct notication_watch *watch = list->data;

		if (watch == NULL)
			continue;

		if (watch->type == resp->type && watch->cmd == resp->command)
			watch->notify_cb(resp->command, resp->data, resp->size,
					 watch->notify_data);
	}
}

static gboolean received_fmt_data(struct ipc_message *resp, struct ipc_device *device)
{
	switch (resp->type) {
	case IPC_TYPE_RESP:
		ofono_debug("Received IPC response message [id=%i, type=%s cmd=%s]",
				resp->aseq, ipc_response_type_string(resp->type),
				ipc_command_string(resp->command));
		handle_response(device, resp);
		break;
	case IPC_TYPE_NOTI:
	case IPC_TYPE_INDI:
		ofono_debug("Received IPC %s message [id=%i, type=%s, cmd=%s]",
				resp->type == IPC_TYPE_NOTI ? "notification" :
				"indication", resp->aseq,
				ipc_response_type_string(resp->type),
				ipc_command_string(resp->command));
		handle_notification(device, resp);
		break;
		break;
	default:
		ofono_error("Received unhandled IPC message type [id=%i, type=%s, cmd=%s]",
				resp->aseq, ipc_response_type_string(resp->type),
				ipc_command_string(resp->command));
		break;
	}

	if (resp->data != NULL)
		free(resp->data);

	return G_SOURCE_CONTINUE;
}

static gboolean received_rfs_data(struct ipc_message *resp, struct ipc_device *device)
{
	ipc_rfs_handle_msg(device->rfs_client, resp);

	if (resp->data != NULL)
		free(resp->data);

	return G_SOURCE_CONTINUE;
}

struct ipc_device *ipc_device_new(struct ipc_client *fmt_client,
				  struct ipc_client *rfs_client)
{
	struct ipc_device *device;
	GSource *fmt_source;
	GSource *rfs_source;
	IpcSource *fmt_ipc_source;
	IpcSource *rfs_ipc_source;

	device = g_try_new0(struct ipc_device, 1);
	if (device == NULL)
		return NULL;

	device->next_id = 1;
	device->next_watch_id = 1;
	device->fmt_client = fmt_client;
	device->rfs_client = rfs_client;
	device->wait_queue = g_queue_new();
	device->notification_watches = g_list_alloc();

	/* Create FMT client */
	fmt_source = g_source_new(&ipc_watch_funcs, sizeof(IpcSource));
	if (fmt_source == NULL)
		return NULL;

	fmt_ipc_source = (IpcSource *) fmt_source;
	fmt_ipc_source->client = fmt_client;

	device->fmt_watch = g_source_attach(fmt_source, NULL);
	g_source_set_priority(fmt_source, G_PRIORITY_HIGH);
	g_source_set_callback(fmt_source, G_SOURCE_FUNC(received_fmt_data),
			      device, NULL);

	/* Create RFS client */
	rfs_source = g_source_new(&ipc_watch_funcs, sizeof(IpcSource));
	if (rfs_source == NULL)
		return NULL;

	rfs_ipc_source = (IpcSource *) rfs_source;
	rfs_ipc_source->client = rfs_client;

	device->rfs_watch = g_source_attach(rfs_source, NULL);
	g_source_set_priority(rfs_source, G_PRIORITY_HIGH);
	g_source_set_callback(rfs_source, G_SOURCE_FUNC(received_rfs_data),
			      device, NULL);

	return device;
}

static void free_notification_watch(gpointer data)
{
	struct notication_watch *watch = data;

	free(watch);
}

void ipc_device_close(struct ipc_device *device)
{
	if (device == NULL)
		return;

	ofono_debug("device %p free", device);

	if (device->write_watch > 0)
		g_source_remove(device->write_watch);

	if (device->fmt_watch > 0)
		g_source_remove(device->fmt_watch);

	g_list_free_full(device->notification_watches, free_notification_watch);

	g_free(device);
}

static guint add_response_watch(struct ipc_device *device, uint8_t type,
				uint16_t cmd, ipc_notify_func_t notify_cb,
				void *user_data)
{
	struct notication_watch *watch;

	watch = g_try_new0(struct notication_watch, 1);
	if (watch == NULL)
		return -1;

	watch->id = next_watch_id(device);
	watch->type = type;
	watch->cmd = cmd;
	watch->notify_cb = notify_cb;
	watch->notify_data = user_data;

	device->notification_watches =
			g_list_append(device->notification_watches, watch);

	return watch->id;
}

guint ipc_device_add_notification_watch(struct ipc_device *device, uint16_t cmd,
				       ipc_notify_func_t notify_cb,
				       void *user_data)
{
	return add_response_watch(device, IPC_TYPE_NOTI, cmd, notify_cb,
				  user_data);
}

guint ipc_device_add_indication_watch(struct ipc_device *device, uint16_t cmd,
				      ipc_notify_func_t notify_cb,
				      void *user_data)
{
	return add_response_watch(device, IPC_TYPE_INDI, cmd, notify_cb,
				  user_data);
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
				     GUINT_TO_POINTER(id),
				     __notification_watch_id_compare);

	if (watches == NULL)
		return;

	device->notification_watches =
			g_list_delete_link(device->notification_watches,
					   watches);
}

void ipc_device_remove_watch(struct ipc_device *device, guint id)
{
	remove_response_watch(device, id);
}

struct ipc_client *ipc_device_get_fmt_client(struct ipc_device *device)
{
	if (device == NULL)
		return NULL;

	return device->fmt_client;
}

int ipc_device_enqueue_message(struct ipc_device *device,
			       uint16_t command, uint8_t type, void *data,
			       uint16_t length, ipc_response_func_t cb,
			       void *user_data)
{
	struct ipc_request *req;

	req = g_try_new0(struct ipc_request, 1);
	if (req == NULL)
		return -1;

	req->command = command;
	req->type = type;
	req->cb = cb;
	req->cb_data = user_data;
	req->id = next_message_id(device);
	req->data = data;
	req->length = length;

	ofono_debug("Sending IPC request message [id=%i, cmd=%s, type=%s]",
			req->id, ipc_command_string(req->command),
			ipc_request_type_string(req->type));

	ipc_client_send(device->fmt_client, req->id, req->command, req->type,
				req->data, req->length);

	g_free(req->data);
	req->data = NULL;

	if (req->cb != NULL)
		g_queue_push_tail(device->wait_queue, req);

	return req->id;
}
