/* gcal-attendees-section.c
 *
 * Copyright (C) 2025 Alen Galinec <mind.on.warp@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gcal-attendees-section.h"

#include <adwaita.h>
#include <gtk/gtk.h>
#include <libintl.h>

#include "gcal-attendee-summary-row.h"
#include "gcal-event-attendee.h"
#include "gcal-event-organizer.h"
#include "gcal-event.h"
#include "gcal-google-meet.h"
#include "gcal-utils.h"
#include "gcal-organizer-row.h"
#include "glib-object.h"

struct _GcalAttendeesSection
{
  GcalEventEditorSection  parent_instance;

  GcalEventOrganizer  *organizer;

  AdwActionRow        *summary_row;
  GtkWidget           *organizer_row;
  AdwEntryRow         *add_guest_row;
  AdwSwitchRow        *meet_row;

  gchar               *own_email;
  gboolean             can_edit_guests;

  GListModel          *attendees;
};

G_DEFINE_FINAL_TYPE (GcalAttendeesSection, gcal_attendees_section, GCAL_TYPE_EVENT_EDITOR_SECTION)

enum
{
  PROP_0,
  PROP_ATTENDEES,
  PROP_ORGANIZER,
  N_PROPS
};

static GParamSpec *properties[N_PROPS] = { NULL, };


/*
 * GcalEventEditorSection overrides
 */

static void
gcal_attendees_section_event_set_cb (GcalEventEditorSection *section,
                                     GcalEvent              *event)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (section);

  self->attendees = event ? gcal_event_get_attendees (event) : NULL;
  gtk_editable_set_text (GTK_EDITABLE (self->add_guest_row), "");
  adw_switch_row_set_active (self->meet_row, FALSE);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ATTENDEES]);
  gcal_attendees_section_refresh (self);
}

static gboolean
is_email (const gchar *text)
{
  const gchar *at = strchr (text, '@');

  return at && at != text && strchr (at + 1, '.') && !strchr (at + 1, '@') &&
         !strpbrk (text, " \t\r\n<>,;:\"");
}

static void
on_add_guest_row_apply_cb (GcalAttendeesSection *self)
{
  GcalEvent *event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));
  g_autofree gchar *email = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->add_guest_row))));
  gboolean first_guest;

  if (!event || !is_email (email))
    {
      gtk_widget_add_css_class (GTK_WIDGET (self->add_guest_row), "error");
      return;
    }

  gtk_widget_remove_css_class (GTK_WIDGET (self->add_guest_row), "error");
  first_guest = gcal_event_get_organizer (event) == NULL;
  gcal_event_add_attendee (event, email, self->own_email);
  gtk_editable_set_text (GTK_EDITABLE (self->add_guest_row), "");

  /* Meetings with guests usually need a call, so offer one by default. */
  if (first_guest && gtk_widget_get_visible (GTK_WIDGET (self->meet_row)))
    adw_switch_row_set_active (self->meet_row, TRUE);

  gcal_attendees_section_refresh (self);
}


/*
 * GObject overrides
 */

static void
gcal_attendees_section_get_property (GObject *object,
                                     guint property_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (property_id)
    {
    case PROP_ATTENDEES:
      g_value_set_object (value, self->attendees);
      break;

    case PROP_ORGANIZER:
      g_value_set_object (value, self->organizer);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_attendees_section_set_property (GObject *object,
                                     guint property_id,
                                     const GValue *value,
                                     GParamSpec *pspec)
{
  GcalAttendeesSection *self = GCAL_ATTENDEES_SECTION (object);

  switch (property_id)
    {
    case PROP_ORGANIZER:
      self->organizer = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
gcal_attendees_section_finalize (GObject *object)
{
  GcalAttendeesSection *self = (GcalAttendeesSection *) object;

  self->attendees = NULL; /* not owned */
  g_clear_pointer (&self->own_email, g_free);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->finalize (object);
}

static void
gcal_attendees_section_init (GcalAttendeesSection *instance)
{
  gtk_widget_init_template (GTK_WIDGET (instance));
}

static void
gcal_attendees_section_class_init (GcalAttendeesSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GcalEventEditorSectionClass *section_class = GCAL_EVENT_EDITOR_SECTION_CLASS (klass);

  g_type_ensure (GCAL_TYPE_ORGANIZER_ROW);
  g_type_ensure (GCAL_TYPE_ATTENDEE_SUMMARY_ROW);

  object_class->finalize = gcal_attendees_section_finalize;
  object_class->get_property = gcal_attendees_section_get_property;
  object_class->set_property = gcal_attendees_section_set_property;

  section_class->event_set_cb = gcal_attendees_section_event_set_cb;

  /**
   * GcalAttendeesSection:attendees:
   *
   * ListStore of #GcalEventAttendee.
   * Passed to the summary rows #GcalAttendeeSummaryRow.
   */
  properties[PROP_ATTENDEES] =
    g_param_spec_object ("attendees", NULL, NULL,
                         G_TYPE_LIST_MODEL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * GcalAttendeesSection:organizer:
   *
   * Organizer of the event.
   * #GcalOrganizerRow binds to this property.
   */
  properties[PROP_ORGANIZER] =
    g_param_spec_object ("organizer", NULL, NULL,
                         GCAL_TYPE_EVENT_ORGANIZER,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/event-editor/gcal-attendees-section.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, summary_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, organizer_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, add_guest_row);
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, meet_row);

  gtk_widget_class_bind_template_callback (widget_class, on_add_guest_row_apply_cb);
}

/**
 * gcal_attendees_section_refresh:
 * @self: a #GcalAttendeesSection
 *
 * Re-reads the organizer after the event's participants changed.
 */
void
gcal_attendees_section_refresh (GcalAttendeesSection *self)
{
  GcalEvent *event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));

  self->organizer = event ? gcal_event_get_organizer (event) : NULL;
  gtk_widget_set_visible (self->organizer_row, self->organizer != NULL);
  gtk_widget_set_visible (GTK_WIDGET (self),
                          self->organizer != NULL || gtk_widget_get_visible (GTK_WIDGET (self->add_guest_row)));
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ORGANIZER]);
}

/**
 * gcal_attendees_section_set_calendar:
 * @self: a #GcalAttendeesSection
 * @calendar: (nullable): the calendar the event will be saved to
 * @writable: whether the event can be edited
 *
 * Shows the guest and video call controls the calendar supports.
 */
void
gcal_attendees_section_set_calendar (GcalAttendeesSection *self,
                                     GcalCalendar         *calendar,
                                     gboolean              writable)
{
  GcalEvent *event = gcal_event_editor_section_get_event (GCAL_EVENT_EDITOR_SECTION (self));
  g_autofree gchar *meeting_url = NULL;
  ECalClient *client = calendar ? gcal_calendar_get_client (calendar) : NULL;
  gboolean can_invite;

  g_clear_pointer (&self->own_email, g_free);
  /* Only calendars whose server sends the invitations, like Google, can add guests. */
  can_invite = writable && client && e_cal_client_check_save_schedules (client) &&
               e_client_get_backend_property_sync (E_CLIENT (client), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS,
                                                   &self->own_email, NULL, NULL) &&
               self->own_email && *self->own_email;

  if (event)
    gcal_utils_extract_meeting_url (gcal_event_get_description (event), NULL, &meeting_url);

  /* Invitations from others also live in writable calendars; only their organizer may change guests. */
  if (event && gcal_event_get_organizer (event))
    {
      const gchar *organizer = gcal_event_organizer_get_uri (gcal_event_get_organizer (event));

      writable = writable && self->own_email && organizer &&
                 g_ascii_strcasecmp (organizer + (g_str_has_prefix (organizer, "mailto:") ? 7 : 0), self->own_email) == 0;
    }

  self->can_edit_guests = can_invite && writable;
  gtk_widget_set_visible (GTK_WIDGET (self->add_guest_row), self->can_edit_guests);
  gtk_widget_set_visible (GTK_WIDGET (self->meet_row),
                          writable && !meeting_url && gcal_google_meet_is_supported (calendar));
  if (!gtk_widget_get_visible (GTK_WIDGET (self->meet_row)))
    adw_switch_row_set_active (self->meet_row, FALSE);

  gcal_attendees_section_refresh (self);
}

/**
 * gcal_attendees_section_get_add_meet:
 * @self: a #GcalAttendeesSection
 *
 * Returns: %TRUE if the user asked for a Google Meet call on save
 */
gboolean
gcal_attendees_section_get_add_meet (GcalAttendeesSection *self)
{
  return gtk_widget_get_visible (GTK_WIDGET (self->meet_row)) && adw_switch_row_get_active (self->meet_row);
}

/**
 * gcal_attendees_section_get_can_edit_guests:
 * @self: a #GcalAttendeesSection
 *
 * Returns: %TRUE if the user organizes the event and its calendar sends invitations
 */
gboolean
gcal_attendees_section_get_can_edit_guests (GcalAttendeesSection *self)
{
  return self->can_edit_guests;
}
