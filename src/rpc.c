/*
 * Copyright (c) 2014 Citrix Systems, Inc.
 * Copyright (c) 2015 Assured Information Security, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file   rpc.c
 * @author Jed Lejosne <lejosnej@ainfosec.com>
 * @date   Thu Jul 30 13:22:55 2015
 *
 * @brief  DBus service
 *
 * Implementation of the dbus methods we expose
 */

#include "project.h"

#define DEV_STATE_ERROR       -1 /**< Cannot find device */
#define DEV_STATE_UNUSED      0  /**< Device not in use by any VM */
#define DEV_STATE_ASSIGNED    1  /**< *ALWAYS* Assigned to another VM which is off */
#define DEV_STATE_IN_USE      2  /**< Assigned to another VM which is running */
#define DEV_STATE_BLOCKED     3  /**< Blocked by policy for this VM */
#define DEV_STATE_THIS        4  /**< In use by this VM */
#define DEV_STATE_THIS_ALWAYS 5  /**< In use by this VM and flagged "always" */
#define DEV_STATE_ALWAYS_ONLY 6  /**< Flagged as "always" assigned to this VM, but not currently in use */
#define DEV_STATE_PLATFORM    7  /**< Special platform device, listed purely for information */
#define DEV_STATE_HID_DOM0    8  /**< HiD device assigned to dom0 */
#define DEV_STATE_HID_ALWAYS  9  /**< HiD device currently assigned to dom0, but always assigned to another VM */
#define DEV_STATE_CD_DOM0     10 /**< External CD drive assigned to dom0 */
#define DEV_STATE_CD_ALWAYS   11 /**< External CD drive currently assigned to dom0, but always assigned to another VM */

#define SERVICE "com.citrix.xenclient.usbdaemon"
#define SERVICE_OBJ_PATH "/"

static DBusConnection  *g_dbus_conn = NULL;
static DBusGConnection *g_glib_dbus_conn = NULL;

/* CTXUSB_DAEMON dbus object implementation */
#include "rpcgen/ctxusb_daemon_server_obj.h"

/**
 * @brief Initialize the DBus RPC bits
 *
 * Grab the bus, initialize the xcdbus handle and export the server.
 */
void rpc_init(void)
{
  CtxusbDaemonObject *server_obj = NULL;

  g_glib_dbus_conn = dbus_g_bus_get(DBUS_BUS_SYSTEM, NULL);
  if (!g_glib_dbus_conn) {
    xd_log(LOG_ERR, "no bus");
    exit(1);
  }
  g_dbus_conn = dbus_g_connection_get_connection(g_glib_dbus_conn);
  g_xcbus = xcdbus_init2(SERVICE, g_glib_dbus_conn);
  if (!g_xcbus) {
    xd_log(LOG_ERR, "failed to init dbus connection / grab service name");
    exit(1);
  }
  /* export server object */
  server_obj = ctxusb_daemon_export_dbus(g_glib_dbus_conn, SERVICE_OBJ_PATH);
  if (!server_obj) {
    xd_log(LOG_ERR, "failed to export server object");
    exit(1);
  }
  /* Wait until all the services we talk to are up */
  xcdbus_wait_service(g_xcbus, "com.citrix.xenclient.input");
  xcdbus_wait_service(g_xcbus, "com.citrix.xenclient.xenmgr");
}

/**
 * @brief Helper to add a VM
 *
 * Reads the VM path from xenstore and call vm_add() on the UUID part.
 * We do this because VMs don't happen to have a "uuid" xenstore node...
 * Example:
 * @code
 * /local/domain/2/vm = "/vm/00000000-0000-0000-0000-000000000001"
 * @endcode
 * @param domid The VM domid
 *
 * @return A pointer to the new VM, or NULL if it failed
 */
static vm_t*
add_vm(int domid)
{
  char *uuid;
  vm_t *res;

  uuid = xenstore_dom_read(domid, "vm");
  if (uuid == NULL) {
    xd_log(LOG_ERR, "Couldn't find UUID for domid %d", domid);
    return NULL;
  }
  res = vm_add(domid, uuid + 4);
  free(uuid);

  return res;
}

gboolean ctxusb_daemon_set_policy_domuuid(
  CtxusbDaemonObject *this,
  const char *uuid,
  const char *policy, GError **error)
{
  g_set_error(error,
              DBUS_GERROR,
              DBUS_GERROR_FAILED,
              "set_policy_domuuid hasn't been implemented yet");

  return FALSE;
}

gboolean ctxusb_daemon_get_policy_domuuid(
  CtxusbDaemonObject *this,
  const char *uuid,
  char **value, GError **error)
{
  g_set_error(error,
              DBUS_GERROR,
              DBUS_GERROR_FAILED,
              "get_policy_domuuid hasn't been implemented yet");

  return FALSE;
}

gboolean ctxusb_daemon_new_vm(CtxusbDaemonObject *this,
                              gint IN_dom_id, GError **error)
{
  vm_t *vm;

  vm = add_vm(IN_dom_id);

  if (vm == NULL) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Failed to add VM %d", IN_dom_id);
    return FALSE;
  } else {
    /* The VM was added correctly, let's run the sticky rules. If
     * anything goes wrong, this will return non-0, but the RPC
     * probably shouldn't fail... */
    policy_auto_assign_devices_to_new_vm(vm);
    return TRUE;
  }
}

gboolean ctxusb_daemon_vm_stopped(CtxusbDaemonObject *this,
                                  gint IN_dom_id, GError **error)
{
  int ret;

  device_unplug_all_from_vm(IN_dom_id);

  ret = vm_del(IN_dom_id);

  if (ret) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Failed to delete VM %d", IN_dom_id);
    return FALSE;
  } else {
    return TRUE;
  }
}

gboolean ctxusb_daemon_list_devices(CtxusbDaemonObject *this,
                                    GArray* *OUT_devices, GError **error)
{
  GArray *devArray;
  struct list_head *pos;
  device_t *device;
  int id;

  devArray = g_array_new(FALSE, FALSE, sizeof(gint));

  list_for_each(pos, &devices.list) {
    device = list_entry(pos, device_t, list);
    id = device_make_id(device->busid, device->devid);
    g_array_append_val(devArray, id);
  }

  *OUT_devices = devArray;

  return TRUE;
}

gboolean ctxusb_daemon_get_device_info(CtxusbDaemonObject *this,
                                       gint IN_dev_id, const char* IN_vm_uuid,
                                       char* *OUT_name, gint *OUT_state, char* *OUT_vm_assigned, char* *OUT_detail, GError **error)
{
  struct list_head *pos;
  device_t *device = NULL;
  int busid, devid;

  device_make_bus_dev_pair(IN_dev_id, &busid, &devid);
  list_for_each(pos, &devices.list) {
    device = list_entry(pos, device_t, list);
    if (device->busid == busid && device->devid == devid) {
      break;
    }
  }
  if (device == NULL || device->busid != busid || device->devid != devid) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device not found: %d", IN_dev_id);
    return FALSE;
  }
  *OUT_name = g_strdup(device->shortname);
  /* Figure out the state and assigned VM for the device. Default to unused. */
  /* We could simply output the assigned VM and an always-assign
   * flag, but finding the right DEV_STATE is more fun, here goes... */
  if (device->vm != NULL) {
    /* The device is currently assigned to a VM */
    if (!strncmp(device->vm->uuid, IN_vm_uuid, UUID_LENGTH)) {
      /* The VM is IN_vm_uuid */
      char *uuid = policy_get_sticky_uuid(IN_dev_id);
      if (uuid != NULL && !strncmp(uuid, IN_vm_uuid, UUID_LENGTH))
        /* And it's always-assigned to it */
        *OUT_state = DEV_STATE_THIS_ALWAYS;
      else
        /* But it's not always-assigned to it */
        *OUT_state = DEV_STATE_THIS;
    } else
      /* The VM is not IN_vm_uuid */
      *OUT_state = DEV_STATE_IN_USE;
    /* Either way, the assigned VM is this */
    *OUT_vm_assigned = g_strdup(device->vm->uuid);
  } else {
    /* The device is not currently assigned to a VM */
    char *uuid = policy_get_sticky_uuid(IN_dev_id);
    if (uuid != NULL) {
      /* But it has an always-assign VM */
      if (device->type & OPTICAL)
        /* It's a CD drive */
        *OUT_state = DEV_STATE_CD_ALWAYS;
      else {
        if (!strncmp(uuid, IN_vm_uuid, UUID_LENGTH))
          /* Which is IN_vm_uuid */
          *OUT_state = DEV_STATE_ALWAYS_ONLY;
        else
          /* Or not */
          *OUT_state = DEV_STATE_ASSIGNED;
      }
      /* Either way, the assigned VM is this */
      *OUT_vm_assigned = g_strdup(uuid);
    } else {
      /* It doesn't have an always-assign VM, it's all free */
      if (device->type & OPTICAL)
        /* Unless it's a CD drive */
        *OUT_state = DEV_STATE_CD_DOM0;
      else
        *OUT_state = DEV_STATE_UNUSED;
      *OUT_vm_assigned = g_strdup("");
    }
  }
  *OUT_detail = g_strdup(device->longname);

  return TRUE;
}

gboolean ctxusb_daemon_assign_device(CtxusbDaemonObject *this,
                                     gint IN_dev_id, const char* IN_vm_uuid, GError **error)
{
  struct list_head *pos;
  device_t *device = NULL;
  vm_t *vm = NULL;
  char *sticky_uuid;
  int busid, devid;
  int ret;

  device_make_bus_dev_pair(IN_dev_id, &busid, &devid);
  list_for_each(pos, &devices.list) {
    device = list_entry(pos, device_t, list);
    if (device->busid == busid && device->devid == devid) {
      break;
    }
  }
  list_for_each(pos, &vms.list) {
    vm = list_entry(pos, vm_t, list);
    if (!strncmp(vm->uuid, IN_vm_uuid, UUID_LENGTH)) {
      break;
    }
  }

  if (device == NULL || device->busid != busid || device->devid != devid) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device not found: %d", IN_dev_id);
    return FALSE;
  }
  if (vm == NULL || strncmp(vm->uuid, IN_vm_uuid, UUID_LENGTH)) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "VM not found: %s", IN_vm_uuid);
    return FALSE;
  }
  if (vm->domid < 0) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Can't assign device %d to stopped VM %s", IN_dev_id, IN_vm_uuid);
    return FALSE;
  }
  if (device->vm != NULL) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device %d is already assigned to a VM", IN_dev_id);
    return FALSE;
  }
  sticky_uuid = policy_get_sticky_uuid(IN_dev_id);
  if (sticky_uuid != NULL && strcmp(vm->uuid, sticky_uuid)) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device %d is set to be always assigned to another VM", IN_dev_id);
    return FALSE;
  }
  if (!policy_is_allowed(device, vm)) {
    notify_com_citrix_xenclient_usbdaemon_device_rejected(g_xcbus,
							  USBDAEMON,
							  USBDAEMON_OBJ,
							  device->shortname,
							  "policy");
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "The policy denied assignation of device %d to VM %s", IN_dev_id, IN_vm_uuid);
    return FALSE;
  }

  device->vm = vm;
  ret = usbowls_plug_device(vm->domid, device->busid, device->devid);
  if (ret != 0) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Failed to plug device %d-%d to VM %d", device->busid, device->devid, vm->domid);
    device->vm = NULL;
    return FALSE;
  }

  return TRUE;
}

gboolean ctxusb_daemon_unassign_device(CtxusbDaemonObject *this,
                                       gint IN_dev_id, GError **error)
{
  struct list_head *pos;
  device_t *device = NULL;
  int busid, devid;
  int res;
  gboolean ret = TRUE;

  device_make_bus_dev_pair(IN_dev_id, &busid, &devid);
  list_for_each(pos, &devices.list) {
    device = list_entry(pos, device_t, list);
    if (device->busid == busid && device->devid == devid) {
      break;
    }
  }
  if (device == NULL || device->busid != busid || device->devid != devid) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device not found: %d", IN_dev_id);
    return FALSE;
  }

  if (device->vm == NULL) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device %d is not currently assigned to a VM, can't unassign", IN_dev_id);
    return FALSE;
  }
  res = usbowls_unplug_device(device->vm->domid, device->busid, device->devid);
  if (res != 0) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Failed to gracefully unplug device %d-%d from VM %d", device->busid, device->devid, device->vm->domid);
    ret = FALSE;
  }

  device->vm = NULL;

  return ret;
}

gboolean ctxusb_daemon_set_sticky(CtxusbDaemonObject *this,
                                  gint IN_dev_id, gint IN_sticky, GError **error)
{
  if (IN_sticky == 1 && false/* && policy_get_sticky_uuid(IN_dev_id) != NULL */) {
    g_set_error(error,
                DBUS_GERROR,
                DBUS_GERROR_FAILED,
                "Device %d is set to be always assigned to a VM", IN_dev_id);
    return FALSE;
  }

  if (IN_sticky == 0)
    policy_unset_sticky(IN_dev_id);
  else
    policy_set_sticky(IN_dev_id);

  return TRUE;
}

gboolean ctxusb_daemon_name_device(CtxusbDaemonObject *this,
                                   gint IN_dev_id, const char* IN_name, GError **error)
{
  return TRUE;
}

static int add_to_string(char **s, int len, const char *fmt, ...)
{
  int new_len;
  va_list ap;

  va_start(ap, fmt);
  new_len = len + vsnprintf(NULL, 0, fmt, ap) + 1;
  va_end(ap);

  *s = g_realloc(*s, new_len + 1);

  va_start(ap, fmt);
  vsprintf(*s + len, fmt, ap);
  va_end(ap);

  len = new_len;
  (*s)[len - 1] = '\n';
  (*s)[len] = '\0';

  return len;
}

gboolean ctxusb_daemon_state(CtxusbDaemonObject *this,
                             char **OUT_state, GError **error)
{
  int l = 0;
  struct list_head *pos;
  vm_t *vm;
  int vm_count = 0;
  device_t *device;
  int device_count = 0;

  l = add_to_string(OUT_state, l, "vusb-daemon state:");
  list_for_each(pos, &vms.list) {
    vm_count++;
  }
  l = add_to_string(OUT_state, l, "  VMs (%d):", vm_count);
  list_for_each(pos, &vms.list) {
    vm = list_entry(pos, vm_t, list);
    if (vm->domid >= 0)
      l = add_to_string(OUT_state, l, "    Running - %3d - %s", vm->domid, vm->uuid);
    else
      l = add_to_string(OUT_state, l, "    Stopped -     - %s", vm->uuid);
  }
  list_for_each(pos, &devices.list) {
    device_count++;
  }
  l = add_to_string(OUT_state, l, "  Devices (%d):", device_count);
  list_for_each(pos, &devices.list) {
    device = list_entry(pos, device_t, list);
    l = add_to_string(OUT_state, l, "    %s - %s", device->shortname, device->longname);
    l = add_to_string(OUT_state, l, "      ID: %d", device_make_id(device->busid, device->devid));
    l = add_to_string(OUT_state, l, "      Type: %d", device->type);
    l = add_to_string(OUT_state, l, "      Bus ID: %d, Device ID: %d", device->busid, device->devid);
    l = add_to_string(OUT_state, l, "      Vendor: 0x%04X, Device: 0x%04X", device->vendorid, device->deviceid);
    if (device->vm != NULL)
      l = add_to_string(OUT_state, l, "      Assigned to domid %d", device->vm->domid);
    else
      l = add_to_string(OUT_state, l, "      Not assigned to any VM");
  }
  /* Remove last \n */
  (*OUT_state)[l - 1] = '\0';

  return TRUE;
}

gboolean ctxusb_daemon_reload_policy(CtxusbDaemonObject *this, GError** error)
{
  policy_reload_from_db();

  return TRUE;
}
