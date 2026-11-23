/* gcal-google-meet.c
 *
 * Adds Google Meet video calls to events in Google calendars, through the
 * Calendar REST API and the account's existing GNOME Online Accounts sign-in.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "GcalGoogleMeet"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "gcal-calendar.h"
#include "gcal-google-meet.h"

#define GOOGLE_CALDAV_HOST "apidata.googleusercontent.com"
#define GOOGLE_EVENTS_URL "https://www.googleapis.com/calendar/v3/calendars/%s/events"

typedef struct
{
  GcalCalendar *calendar;
  gchar        *uid;
} MeetData;

static void
meet_data_free (gpointer data)
{
  MeetData *meet = data;

  g_clear_object (&meet->calendar);
  g_free (meet->uid);
  g_free (meet);
}

/* Google CalDAV calendars live at /caldav/v2/<calendar id>/events/ */
static gchar *
google_calendar_id (GcalCalendar *calendar)
{
  ESource *source = gcal_calendar_get_source (calendar);
  g_autoptr (GUri) uri = NULL;
  g_auto (GStrv) parts = NULL;

  if (!e_source_has_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND))
    return NULL;

  uri = e_source_webdav_dup_uri (e_source_get_extension (source, E_SOURCE_EXTENSION_WEBDAV_BACKEND));
  if (!uri || g_strcmp0 (g_uri_get_host (uri), GOOGLE_CALDAV_HOST) != 0)
    return NULL;

  parts = g_strsplit (g_uri_get_path (uri), "/", -1);
  if (g_strv_length (parts) < 4 || g_strcmp0 (parts[1], "caldav") != 0 || g_strcmp0 (parts[2], "v2") != 0)
    return NULL;

  return g_uri_unescape_string (parts[3], NULL);
}

static JsonNode *
google_request (SoupSession  *session,
                const gchar  *method,
                const gchar  *url,
                const gchar  *token,
                const gchar  *body,
                GError      **error)
{
  g_autoptr (SoupMessage) message = soup_message_new (method, url);
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GBytes) response = NULL;
  g_autofree gchar *authorization = g_strdup_printf ("Bearer %s", token);
  const gchar *data;
  gsize size;

  if (!message)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Google Calendar address");
      return NULL;
    }

  soup_message_headers_append (soup_message_get_request_headers (message), "Authorization", authorization);
  if (body)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (body, strlen (body));
      soup_message_set_request_body_from_bytes (message, "application/json", bytes);
    }

  response = soup_session_send_and_read (session, message, NULL, error);
  if (!response)
    return NULL;

  data = g_bytes_get_data (response, &size);

  if (soup_message_get_status (message) != SOUP_STATUS_OK)
    {
      const gchar *reason = NULL;
      JsonNode *root;

      /* Google explains refusals as {"error": {"message": "..."}} */
      if (json_parser_load_from_data (parser, data, size, NULL) &&
          (root = json_parser_get_root (parser)) && JSON_NODE_HOLDS_OBJECT (root) &&
          json_object_has_member (json_node_get_object (root), "error") &&
          JSON_NODE_HOLDS_OBJECT (json_object_get_member (json_node_get_object (root), "error")))
        reason = json_object_get_string_member_with_default (
          json_object_get_object_member (json_node_get_object (root), "error"), "message", NULL);

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Google Calendar refused the request (HTTP %u): %s"),
                   soup_message_get_status (message), reason ? reason : _("no details"));
      return NULL;
    }

  if (!json_parser_load_from_data (parser, data, size, error))
    return NULL;

  return json_node_copy (json_parser_get_root (parser));
}

/* Finds the event (the series, for recurring ones) by its iCalendar UID. */
static gboolean
find_event (SoupSession  *session,
            const gchar  *base,
            const gchar  *escaped_uid,
            const gchar  *token,
            gchar       **out_event_id,
            gboolean     *out_has_link,
            GError      **error)
{
  g_autofree gchar *lookup = g_strdup_printf ("%s?iCalUID=%s", base, escaped_uid);
  g_autoptr (JsonNode) root = google_request (session, "GET", lookup, token, NULL, error);
  JsonArray *items;

  *out_has_link = FALSE;
  if (!root)
    return FALSE;

  items = JSON_NODE_HOLDS_OBJECT (root) && json_object_has_member (json_node_get_object (root), "items")
            ? json_object_get_array_member (json_node_get_object (root), "items") : NULL;
  for (guint i = 0; items && i < json_array_get_length (items); i++)
    {
      JsonObject *item = json_array_get_object_element (items, i);

      /* A recurring event gets one link on the series, as Google Calendar does. */
      if (json_object_has_member (item, "recurringEventId"))
        continue;

      *out_has_link = json_object_has_member (item, "hangoutLink");
      g_free (*out_event_id);
      *out_event_id = g_strdup (json_object_get_string_member_with_default (item, "id", NULL));
      break;
    }

  return TRUE;
}

static void
add_meet_thread (GTask        *task,
                 gpointer      source_object,
                 gpointer      task_data,
                 GCancellable *cancellable)
{
  MeetData *data = task_data;
  g_autoptr (SoupSession) session = soup_session_new ();
  g_autoptr (JsonNode) updated = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *calendar_id = google_calendar_id (data->calendar);
  g_autofree gchar *escaped_calendar = NULL;
  g_autofree gchar *escaped_uid = g_uri_escape_string (data->uid, NULL, FALSE);
  g_autofree gchar *event_id = NULL;
  g_autofree gchar *token = NULL;
  g_autofree gchar *base = NULL;
  g_autofree gchar *url = NULL;
  g_autofree gchar *body = NULL;
  g_autofree gchar *request_id = g_uuid_string_random ();
  gboolean has_link = FALSE;

  if (!calendar_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This is not a Google calendar"));
      return;
    }

  if (!e_source_get_oauth2_access_token_sync (gcal_calendar_get_source (data->calendar), cancellable,
                                              &token, NULL, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  escaped_calendar = g_uri_escape_string (calendar_id, NULL, FALSE);
  base = g_strdup_printf (GOOGLE_EVENTS_URL, escaped_calendar);

  /* A new event may still be uploading to Google, so give it a few seconds to appear. */
  for (gint attempt = 0; attempt < 10 && !event_id; attempt++)
    {
      if (!find_event (session, base, escaped_uid, token, &event_id, &has_link, &error))
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }

      if (has_link)
        {
          g_task_return_boolean (task, TRUE);
          return;
        }

      if (!event_id)
        g_usleep (2 * G_USEC_PER_SEC);
    }

  if (!event_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               _("The event did not appear in Google Calendar"));
      return;
    }

  /* Google guests see the link in their calendars; only email the guests outside Google. */
  url = g_strdup_printf ("%s/%s?conferenceDataVersion=1&sendUpdates=externalOnly", base, event_id);
  body = g_strdup_printf ("{\"conferenceData\": {\"createRequest\": {\"requestId\": \"%s\", "
                          "\"conferenceSolutionKey\": {\"type\": \"hangoutsMeet\"}}}}", request_id);
  updated = google_request (session, "PATCH", url, token, body, &error);
  if (!updated)
    {
      g_autofree gchar *ignored_id = NULL;

      /* Another app (e.g. Cal Bar) may have added a call at the same time; that is a success. */
      if (!find_event (session, base, escaped_uid, token, &ignored_id, &has_link, NULL) || !has_link)
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
    }

  /* Pull the link into the local calendar now instead of at the next periodic sync. */
  e_client_refresh_sync (E_CLIENT (gcal_calendar_get_client (data->calendar)), cancellable, NULL);

  g_task_return_boolean (task, TRUE);
}

static void
on_meet_added_cb (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GError) error = NULL;
  GApplication *application;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    return;

  g_warning ("Could not add Google Meet: %s", error->message);

  application = g_application_get_default ();
  if (!application)
    return;

  notification = g_notification_new (_("Could not add a Google Meet video call"));
  g_notification_set_body (notification, error->message);
  g_application_send_notification (application, "google-meet", notification);
}

/**
 * gcal_google_meet_is_supported:
 * @calendar: a #GcalCalendar
 *
 * Returns: %TRUE if @calendar is a Google calendar that can host Meet calls
 */
gboolean
gcal_google_meet_is_supported (GcalCalendar *calendar)
{
  g_autofree gchar *calendar_id = NULL;

  if (!calendar)
    return FALSE;

  calendar_id = google_calendar_id (calendar);
  return calendar_id != NULL;
}

/**
 * gcal_google_meet_add:
 * @event: a #GcalEvent saved, or being saved, to a Google calendar
 *
 * Adds a Google Meet video call to @event in the background. Failures are
 * reported with a desktop notification.
 */
void
gcal_google_meet_add (GcalEvent *event)
{
  g_autoptr (GTask) task = NULL;
  MeetData *data;

  g_return_if_fail (GCAL_IS_EVENT (event));

  data = g_new0 (MeetData, 1);
  data->calendar = g_object_ref (gcal_event_get_calendar (event));
  data->uid = g_strdup (e_cal_component_get_uid (gcal_event_get_component (event)));

  task = g_task_new (NULL, NULL, on_meet_added_cb, NULL);
  g_task_set_source_tag (task, gcal_google_meet_add);
  g_task_set_task_data (task, data, meet_data_free);
  g_task_run_in_thread (task, add_meet_thread);
}
