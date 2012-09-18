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
#include <string.h>

#include <glib.h>
#include <gdbus.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/dbus.h>

#define MGR_SERVICE		"org.samsung.modem"
#define MGR_INTERFACE		MGR_SERVICE ".Manager"

enum modem_status {
	OFFLINE,
	INITIALIZING,
	ONLINE
};

static guint modem_daemon_watch;
static guint property_changed_watch;
static DBusConnection *connection;
static enum modem_status modem_status = OFFLINE;
struct ofono_modem *modem;

static void set_modem_power_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError err;

	dbus_error_init(&err);

	if (dbus_set_error_from_message(&err, reply) == TRUE) {
		g_printerr("%s: %s\n", err.name, err.message);
		dbus_error_free(&err);
	}

	dbus_message_unref(reply);
}

static int set_modem_power(ofono_bool_t power)
{
	DBusMessage *msg;
	DBusMessageIter iter, value;
	DBusPendingCall *call;
	const char *key = "Powered";

	msg = dbus_message_new_method_call(MGR_SERVICE, "/",
					MGR_INTERFACE, "SetProperty");
	if (msg == NULL)
		return -ENOMEM;

	dbus_message_set_auto_start(msg, FALSE);

	dbus_message_iter_init_append(msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
							DBUS_TYPE_BOOLEAN_AS_STRING, &value);
	dbus_message_iter_append_basic(&value, DBUS_TYPE_BOOLEAN, &power);
	dbus_message_iter_close_container(&iter, &value);

	if (dbus_connection_send_with_reply(connection, msg, &call, -1) == FALSE) {
		dbus_message_unref(msg);
		return -EIO;
	}

	dbus_message_unref(msg);

	if (call == NULL)
		return -EINVAL;

	dbus_pending_call_set_notify(call, set_modem_power_reply, NULL, NULL);

	dbus_pending_call_unref(call);

	return 0;
}

static void modem_status_changed(const char *status)
{
	switch (modem_status) {
		case OFFLINE:
			if(g_str_equal(status, "online") == TRUE) {
				modem = ofono_modem_create(NULL, "samsungipc");
				if (modem == NULL) {
					ofono_error("Could not create modem");
					return;
				}
				DBG("registering samsung ipc modem ...");
				ofono_modem_register(modem);
				modem_status = ONLINE;
			}
			break;
		case ONLINE:
			if (g_str_equal(status, "offline") == TRUE) {
				DBG("Samsung modem unregistering");
				ofono_modem_remove(modem);
				modem = NULL;
				modem_status = OFFLINE;
			}
			break;
		default:
			break;
	}
}

static gboolean property_changed(DBusConnection *conn,
					DBusMessage *message, void *user_data)
{
	DBusMessageIter iter, value;
	const char *key, *status;

	if (dbus_message_iter_init(message, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	if (g_str_equal(key, "Status") == TRUE) {
		dbus_message_iter_get_basic(&value, &status);
		modem_status_changed(status);
	}

	return TRUE;
}

static void mgr_connect(DBusConnection *conn, void *user_data)
{
	property_changed_watch = g_dbus_add_signal_watch(conn,
						MGR_SERVICE, NULL,
						MGR_INTERFACE,
						"PropertyChanged",
						property_changed,
						NULL, NULL);

	// FIXME check wether modem is already powered up

	set_modem_power(TRUE);
}

static void mgr_disconnect(DBusConnection *conn, void *user_data)
{
	g_dbus_remove_watch(conn, property_changed_watch);
	property_changed_watch = 0;
}

static int samsungmgr_init(void)
{
	connection = ofono_dbus_get_connection();

	modem_daemon_watch = g_dbus_add_service_watch(connection, MGR_SERVICE,
				mgr_connect, mgr_disconnect, NULL, NULL);

	return 0;
}

static void samsungmgr_exit(void)
{
	set_modem_power(FALSE);

	g_dbus_remove_watch(connection, modem_daemon_watch);

	if (property_changed_watch > 0)
		g_dbus_remove_watch(connection, property_changed_watch);
}

OFONO_PLUGIN_DEFINE(samsungmgr, "Samsung Modem Init Daemon detection", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, samsungmgr_init, samsungmgr_exit)
