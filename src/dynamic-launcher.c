/*
 * Copyright © 2022 Matthew Leeds
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthew Leeds <mwleeds@protonmail.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include <gxdp.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>

#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"

#include "dynamic-launcher.h"
#include "request.h"
#include "utils.h"

#define DEFAULT_ICON_SIZE 192

typedef enum
{
  DYNAMIC_LAUNCHER_TYPE_APPLICATION = 1,
  DYNAMIC_LAUNCHER_TYPE_WEBAPP = 2,
} DynamicLauncherType;

typedef struct
{
  XdpImplDynamicLauncher *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GtkWindow *dialog;
  GtkWidget *entry;
  GxdpExternalWindow *external_parent;
  GVariant *icon_v;

  int response;
} InstallDialogHandle;

static void
install_dialog_handle_free (gpointer data)
{
  InstallDialogHandle *handle = data;

  g_clear_object (&handle->external_parent);
  g_clear_object (&handle->request);

  g_clear_pointer (&handle->icon_v, g_variant_unref);
  g_free (handle);
}

static void
install_dialog_handle_close (InstallDialogHandle *handle)
{
  g_clear_pointer (&handle->dialog, gtk_window_destroy);
  install_dialog_handle_free (handle);
}

static gboolean
handle_close (XdpImplRequest        *object,
              GDBusMethodInvocation *invocation,
              InstallDialogHandle   *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_dynamic_launcher_complete_prepare_install (handle->impl,
                                                      handle->invocation,
                                                      2,
                                                      g_variant_builder_end (&opt_builder));

  if (handle->request->exported)
    request_unexport (handle->request);

  install_dialog_handle_close (handle);

  xdp_impl_request_complete_close (object, invocation);

  return TRUE;
}

static void
send_prepare_install_response (InstallDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->response == 0)
    {
      const char *chosen_name = gtk_editable_get_text (GTK_EDITABLE (handle->entry));
      if (chosen_name == NULL || chosen_name[0] == '\0')
        {
          handle->response = 3;
        }
      else
        {
          g_variant_builder_add (&opt_builder, "{sv}", "name", g_variant_new_string (chosen_name));
          g_variant_builder_add (&opt_builder, "{sv}", "icon", handle->icon_v);
        }
    }

  if (handle->request->exported)
    request_unexport (handle->request);

  xdp_impl_dynamic_launcher_complete_prepare_install (handle->impl,
                                                      handle->invocation,
                                                      handle->response,
                                                      g_variant_builder_end (&opt_builder));

  install_dialog_handle_close (handle);
}

static void
handle_prepare_install_response (GtkDialog *dialog,
                                 gint       response,
                                 gpointer   data)
{
  InstallDialogHandle *handle = data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      handle->response = 2;
      break;

    case GTK_RESPONSE_DELETE_EVENT:
       G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      break;

    case GTK_RESPONSE_APPLY:
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_OK:
      handle->response = 0;
      break;
    }

  send_prepare_install_response (handle);
}

static gboolean
handle_prepare_install (XdpImplDynamicLauncher *object,
                        GDBusMethodInvocation  *invocation,
                        const char             *arg_handle,
                        const char             *arg_app_id,
                        const char             *arg_parent_window,
                        const char             *arg_name,
                        GVariant               *arg_icon_v,
                        GVariant               *arg_options)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  g_autoptr(Request) request = NULL;
  const char *sender;
  GdkSurface *surface;
  GxdpExternalWindow *external_parent = NULL;
  GtkWidget *fake_parent;
  InstallDialogHandle *handle;
  GtkWindow *dialog;
  GtkWidget *content_area, *box, *image, *label, *entry;
  GtkDialogFlags dialog_flags;
  const char *url = NULL;
  const char *title;
  gboolean modal, editable_name, editable_icon;
  DynamicLauncherType launcher_type;
  g_autoptr(GVariant) icon_v = NULL;
  g_autoptr(GInputStream) icon_stream = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GError) error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (arg_parent_window)
    {
      external_parent = gxdp_external_window_new_from_handle (arg_parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   arg_parent_window);
    }

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  handle = g_new0 (InstallDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->external_parent = external_parent;

  /* FIXME: Implement editable-icon option rather than passing along the default */
  handle->icon_v = g_variant_ref (arg_icon_v);

  icon_v = g_variant_get_variant (arg_icon_v);
  if (icon_v)
    icon = g_icon_deserialize (icon_v);
  if (!icon || !G_IS_BYTES_ICON (icon))
    {
      g_warning ("Icon deserialization failed");
      goto err;
    }

  if (!g_variant_lookup (arg_options, "launcher_type", "u", &launcher_type))
    launcher_type = DYNAMIC_LAUNCHER_TYPE_APPLICATION;
  if (launcher_type == DYNAMIC_LAUNCHER_TYPE_WEBAPP &&
      !g_variant_lookup (arg_options, "target", "&s", &url))
    url = NULL;
  if (!g_variant_lookup (arg_options, "modal", "b", &modal))
    modal = TRUE;
  if (!g_variant_lookup (arg_options, "editable_name", "b", &editable_name))
    editable_name = TRUE;
  if (!g_variant_lookup (arg_options, "editable_icon", "b", &editable_icon))
    editable_icon = FALSE;

  if (modal)
    dialog_flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
  else
    dialog_flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;

  if (launcher_type == DYNAMIC_LAUNCHER_TYPE_WEBAPP)
    title = _("Create Web App");
  else
    title = _("Create App");

  /* Show dialog with icon, title. */
  dialog = GTK_WINDOW (gtk_dialog_new_with_buttons (title,
                                                    GTK_WINDOW (fake_parent),
                                                    dialog_flags,
                                                    _("_Cancel"),
                                                    GTK_RESPONSE_CANCEL,
                                                    _("C_reate"),
                                                    GTK_RESPONSE_OK,
                                                    NULL));

  content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_margin_top (box, 15);
  gtk_widget_set_margin_bottom (box, 15);
  gtk_widget_set_margin_start (box, 15);
  gtk_widget_set_margin_end (box, 15);
  gtk_box_append (GTK_BOX (content_area), box);

  image = gtk_image_new ();
  gtk_widget_set_vexpand (image, TRUE);
  gtk_widget_set_size_request (image, DEFAULT_ICON_SIZE, DEFAULT_ICON_SIZE);
  gtk_widget_set_margin_bottom (image, 10);
  gtk_box_append (GTK_BOX (box), image);

  icon_stream = g_loadable_icon_load (G_LOADABLE_ICON (icon), 0, NULL, NULL, &error);
  if (icon_stream)
    pixbuf = gdk_pixbuf_new_from_stream_at_scale (icon_stream,
                                                  DEFAULT_ICON_SIZE, DEFAULT_ICON_SIZE,
                                                  TRUE, NULL, &error);
  if (error)
    {
      g_warning ("Loading icon into pixbuf failed: %s", error->message);
      goto err;
    }
  gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);

  entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), arg_name);
  gtk_widget_set_sensitive (entry, editable_name);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_box_append (GTK_BOX (box), entry);

  if (launcher_type == DYNAMIC_LAUNCHER_TYPE_WEBAPP)
    {
      g_autofree char *escaped_address = NULL;
      g_autofree char *markup = NULL;

      escaped_address = g_markup_escape_text (url, -1);
      markup = g_strdup_printf ("<small>%s</small>", escaped_address);

      label = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (label), markup);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 40);

      gtk_box_append (GTK_BOX (box), label);
      gtk_widget_add_css_class (label, "dim-label");
    }

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, dialog);

  handle->dialog = g_object_ref_sink (dialog);
  handle->entry = entry;

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);
  g_signal_connect (dialog, "response", G_CALLBACK (handle_prepare_install_response), handle);

  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    gxdp_external_window_set_parent_of (external_parent, surface);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  gtk_window_present (GTK_WINDOW (dialog));

  return TRUE;

err:
  handle->response = 2;
  send_prepare_install_response (handle);
  return TRUE;
}

static gboolean
handle_request_install_token (XdpImplDynamicLauncher *object,
                              GDBusMethodInvocation  *invocation,
                              const char             *arg_app_id,
                              GVariant               *arg_options)
{
  static const char *allowlist[] = {
    "org.gnome.Software",
    "org.gnome.SoftwareDevel",
    NULL
  };
  guint response;

  if (arg_app_id != NULL && g_strv_contains (allowlist, arg_app_id))
    response = 0;
  else
    response = 2;

  xdp_impl_dynamic_launcher_complete_request_install_token (object,
                                                            invocation,
                                                            response);
  return TRUE;
}

gboolean
dynamic_launcher_init (GDBusConnection  *bus,
                       GError          **error)
{
  GDBusInterfaceSkeleton *helper;
  DynamicLauncherType supported_types = DYNAMIC_LAUNCHER_TYPE_APPLICATION |
                                        DYNAMIC_LAUNCHER_TYPE_WEBAPP;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_dynamic_launcher_skeleton_new ());

  xdp_impl_dynamic_launcher_set_supported_launcher_types (XDP_IMPL_DYNAMIC_LAUNCHER (helper),
                                                          supported_types);

  g_signal_connect (helper, "handle-prepare-install", G_CALLBACK (handle_prepare_install), NULL);
  g_signal_connect (helper, "handle-request-install-token", G_CALLBACK (handle_request_install_token), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
