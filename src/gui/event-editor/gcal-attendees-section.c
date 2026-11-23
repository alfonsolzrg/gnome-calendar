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

#include "gcal-attendee-row.h"
#include "gcal-attendee-summary-row.h"
#include "gcal-event-attendee.h"
#include "gcal-event-organizer.h"
#include "gcal-event.h"
#include "gcal-google-meet.h"
#include <libebook/libebook.h>
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
  AdwActionRow        *join_row;
  gchar               *meeting_url;

  gchar               *own_email;
  gboolean             can_edit_guests;

  /* Guest suggestions from the desktop address books */
  GPtrArray           *books;
  GCancellable        *books_cancellable;
  GCancellable        *query_cancellable;
  guint                query_serial;
  GtkWidget           *suggestions_popover;
  GtkListBox          *suggestions;

  GPtrArray           *guest_rows; /* owned by the group */

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
  self->can_edit_guests = FALSE; /* until set_calendar() runs for this event */
  gtk_editable_set_text (GTK_EDITABLE (self->add_guest_row), "");
  gtk_popover_popdown (GTK_POPOVER (self->suggestions_popover));
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
  GtkListBoxRow *suggestion = gtk_list_box_get_selected_row (self->suggestions);
  const gchar *name = NULL;
  gboolean first_guest;

  if (suggestion && gtk_widget_get_visible (self->suggestions_popover) &&
      (!is_email (email) || g_ascii_strcasecmp (email, g_object_get_data (G_OBJECT (suggestion), "email")) == 0))
    {
      g_free (email);
      email = g_strdup (g_object_get_data (G_OBJECT (suggestion), "email"));
      name = g_object_get_data (G_OBJECT (suggestion), "name");
    }

  if (!event || !is_email (email))
    {
      gtk_widget_add_css_class (GTK_WIDGET (self->add_guest_row), "error");
      return;
    }

  gtk_widget_remove_css_class (GTK_WIDGET (self->add_guest_row), "error");
  first_guest = gcal_event_get_organizer (event) == NULL;
  gcal_event_add_attendee (event, email, name, self->own_email);
  gtk_editable_set_text (GTK_EDITABLE (self->add_guest_row), "");
  gtk_popover_popdown (GTK_POPOVER (self->suggestions_popover));

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
      /* Editable events list their guests inline instead of in the summary rows. */
      g_value_set_object (value, self->can_edit_guests ? NULL : self->attendees);
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
  g_clear_pointer (&self->meeting_url, g_free);
  g_clear_pointer (&self->books, g_ptr_array_unref);
  g_clear_pointer (&self->guest_rows, g_ptr_array_unref);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->finalize (object);
}

static void
on_join_button_clicked_cb (GcalAttendeesSection *self)
{
  g_autoptr (GtkUriLauncher) launcher = NULL;

  if (!self->meeting_url)
    return;

  launcher = gtk_uri_launcher_new (self->meeting_url);
  gtk_uri_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))), NULL, NULL, NULL);
}

static void
gcal_attendees_section_dispose (GObject *object)
{
  GcalAttendeesSection *self = (GcalAttendeesSection *) object;

  g_cancellable_cancel (self->books_cancellable);
  g_cancellable_cancel (self->query_cancellable);
  g_clear_object (&self->books_cancellable);
  g_clear_object (&self->query_cancellable);
  g_clear_pointer (&self->suggestions_popover, gtk_widget_unparent);

  G_OBJECT_CLASS (gcal_attendees_section_parent_class)->dispose (object);
}

static void
on_book_connected_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  EClient *client = e_book_client_connect_finish (result, &error);

  if (!client)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Address book unavailable for guest suggestions: %s", error->message);
      return;
    }

  g_ptr_array_add (GCAL_ATTENDEES_SECTION (user_data)->books, client);
}

static void
on_registry_ready_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (ESourceRegistry) registry = e_source_registry_new_finish (result, NULL);
  GcalAttendeesSection *self = user_data;
  GList *sources;

  if (!registry || g_cancellable_is_cancelled (self->books_cancellable))
    return;

  sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  for (GList *l = sources; l != NULL; l = l->next)
    e_book_client_connect (l->data, 5, self->books_cancellable, on_book_connected_cb, self);
  g_list_free_full (sources, g_object_unref);
}

typedef struct
{
  GcalAttendeesSection *self;
  guint                 serial;
  gchar                *text; /* casefolded */
} SuggestionQuery;

/* 0 when the name, a word of it, or the email starts with the typed text; 1 otherwise */
static gint
suggestion_rank (EContact    *contact,
                 const gchar *text)
{
  const gchar *name = e_contact_get_const (contact, E_CONTACT_FULL_NAME);
  const gchar *email = e_contact_get_const (contact, E_CONTACT_EMAIL_1);
  g_autofree gchar *words = g_strconcat (" ", name ? name : "", NULL);
  g_autofree gchar *folded_words = g_utf8_casefold (words, -1);
  g_autofree gchar *folded_email = email ? g_utf8_casefold (email, -1) : NULL;
  g_autofree gchar *word_start = g_strconcat (" ", text, NULL);

  return strstr (folded_words, word_start) || (folded_email && g_str_has_prefix (folded_email, text)) ? 0 : 1;
}

static gint
compare_suggestions (gconstpointer a,
                     gconstpointer b,
                     gpointer      text)
{
  return suggestion_rank ((EContact *) a, text) - suggestion_rank ((EContact *) b, text);
}

static gboolean
is_invited (GcalAttendeesSection *self,
            const gchar          *email)
{
  for (guint i = 0; self->attendees && i < g_list_model_get_n_items (self->attendees); i++)
    {
      g_autoptr (GcalEventAttendee) attendee = g_list_model_get_item (self->attendees, i);
      const gchar *uri = gcal_event_attendee_get_uri (attendee);

      if (uri && g_ascii_strcasecmp (uri + (g_str_has_prefix (uri, "mailto:") ? 7 : 0), email) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
is_suggested (GcalAttendeesSection *self,
              const gchar          *email)
{
  GtkListBoxRow *row;

  for (gint i = 0; (row = gtk_list_box_get_row_at_index (self->suggestions, i)) != NULL; i++)
    {
      if (g_ascii_strcasecmp (g_object_get_data (G_OBJECT (row), "email"), email) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
on_contacts_found_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  SuggestionQuery *query = user_data;
  GcalAttendeesSection *self = query->self;
  g_autofree gchar *text = query->text;
  GSList *contacts = NULL;
  GtkWidget *focus;
  GtkRoot *root;

  gboolean current;

  current = e_book_client_get_contacts_finish (E_BOOK_CLIENT (source_object), result, &contacts, NULL) &&
            query->serial == self->query_serial;
  g_free (query);
  if (!current)
    {
      g_slist_free_full (contacts, g_object_unref);
      return;
    }

  contacts = g_slist_sort_with_data (contacts, compare_suggestions, text);

  for (GSList *l = contacts; l != NULL; l = l->next)
    {
      const gchar *name = e_contact_get_const (l->data, E_CONTACT_FULL_NAME);
      GList *emails = e_contact_get (l->data, E_CONTACT_EMAIL);

      for (GList *e = emails; e != NULL && gtk_list_box_get_row_at_index (self->suggestions, 7) == NULL; e = e->next)
        {
          const gchar *email = e->data;
          GtkWidget *row;

          if (!is_email (email) || is_invited (self, email) || is_suggested (self, email))
            continue;

          row = adw_action_row_new ();
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name && *name ? name : email);
          if (name && *name)
            adw_action_row_set_subtitle (ADW_ACTION_ROW (row), email);
          gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
          g_object_set_data_full (G_OBJECT (row), "email", g_strdup (email), g_free);
          g_object_set_data_full (G_OBJECT (row), "name", g_strdup (name), g_free);
          gtk_list_box_append (self->suggestions, row);
        }

      g_list_free_full (emails, g_free);
    }

  g_slist_free_full (contacts, g_object_unref);

  root = gtk_widget_get_root (GTK_WIDGET (self->add_guest_row));
  focus = root ? gtk_root_get_focus (root) : NULL;
  if (gtk_list_box_get_row_at_index (self->suggestions, 0) && focus &&
      gtk_widget_is_ancestor (focus, GTK_WIDGET (self->add_guest_row)))
    {
      gtk_list_box_select_row (self->suggestions, gtk_list_box_get_row_at_index (self->suggestions, 0));
      gtk_widget_set_size_request (GTK_WIDGET (self->suggestions),
                                   MAX (240, gtk_widget_get_width (GTK_WIDGET (self->add_guest_row)) - 24), -1);
      gtk_popover_popup (GTK_POPOVER (self->suggestions_popover));
    }
}

static void
on_add_guest_row_changed_cb (GcalAttendeesSection *self)
{
  g_autofree gchar *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->add_guest_row))));
  g_autoptr (EBookQuery) query = NULL;
  g_autofree gchar *sexp = NULL;
  EBookQuery *terms[3];

  self->query_serial++;
  g_cancellable_cancel (self->query_cancellable);
  g_clear_object (&self->query_cancellable);
  gtk_list_box_remove_all (self->suggestions);
  gtk_widget_remove_css_class (GTK_WIDGET (self->add_guest_row), "error");

  if (g_utf8_strlen (text, -1) < 2 || !self->books || self->books->len == 0)
    {
      gtk_popover_popdown (GTK_POPOVER (self->suggestions_popover));
      return;
    }

  terms[0] = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_BEGINS_WITH, text);
  terms[1] = e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, text);
  terms[2] = e_book_query_field_test (E_CONTACT_NICKNAME, E_BOOK_QUERY_BEGINS_WITH, text);
  query = e_book_query_or (3, terms, TRUE);
  sexp = e_book_query_to_string (query);

  self->query_cancellable = g_cancellable_new ();
  for (guint i = 0; i < self->books->len; i++)
    {
      SuggestionQuery *data = g_new0 (SuggestionQuery, 1);

      data->self = self;
      data->serial = self->query_serial;
      data->text = g_utf8_casefold (text, -1);
      e_book_client_get_contacts (g_ptr_array_index (self->books, i), sexp, self->query_cancellable,
                                  on_contacts_found_cb, data);
    }
}

static void
on_suggestion_activated_cb (GtkListBox           *list,
                            GtkListBoxRow        *row,
                            GcalAttendeesSection *self)
{
  gtk_list_box_select_row (self->suggestions, row);
  gtk_editable_set_text (GTK_EDITABLE (self->add_guest_row), g_object_get_data (G_OBJECT (row), "email"));
  on_add_guest_row_apply_cb (self);
  gtk_widget_grab_focus (GTK_WIDGET (self->add_guest_row));
}

/* Arrows move through the suggestions while the keyboard stays in the email field. */
static gboolean
on_add_guest_row_key_pressed_cb (GtkEventControllerKey *controller,
                                 guint                  keyval,
                                 guint                  keycode,
                                 GdkModifierType        state,
                                 GcalAttendeesSection  *self)
{
  GtkListBoxRow *selected, *next;
  gint index;

  if (!gtk_widget_get_visible (self->suggestions_popover))
    return GDK_EVENT_PROPAGATE;

  selected = gtk_list_box_get_selected_row (self->suggestions);
  index = selected ? gtk_list_box_row_get_index (selected) : -1;

  switch (keyval)
    {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      index += keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down ? 1 : -1;
      next = gtk_list_box_get_row_at_index (self->suggestions, MAX (index, 0));
      if (next)
        gtk_list_box_select_row (self->suggestions, next);
      return GDK_EVENT_STOP;

    case GDK_KEY_Escape:
      gtk_popover_popdown (GTK_POPOVER (self->suggestions_popover));
      return GDK_EVENT_STOP;

    default:
      return GDK_EVENT_PROPAGATE;
    }
}

static void
gcal_attendees_section_init (GcalAttendeesSection *instance)
{
  GtkEventController *keys;

  gtk_widget_init_template (GTK_WIDGET (instance));

  instance->books = g_ptr_array_new_with_free_func (g_object_unref);
  instance->books_cancellable = g_cancellable_new ();
  e_source_registry_new (instance->books_cancellable, on_registry_ready_cb, instance);

  instance->suggestions = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (instance->suggestions, GTK_SELECTION_SINGLE);
  /* Plain rows with a highlight, not a card inside the popover */
  gtk_widget_add_css_class (GTK_WIDGET (instance->suggestions), "navigation-sidebar");
  g_signal_connect (instance->suggestions, "row-activated", G_CALLBACK (on_suggestion_activated_cb), instance);

  /* Not autohide: an autohide popover would take the keyboard away from the entry while typing. */
  instance->suggestions_popover = gtk_popover_new ();
  gtk_popover_set_autohide (GTK_POPOVER (instance->suggestions_popover), FALSE);
  gtk_popover_set_has_arrow (GTK_POPOVER (instance->suggestions_popover), FALSE);
  gtk_popover_set_position (GTK_POPOVER (instance->suggestions_popover), GTK_POS_BOTTOM);
  /* Leave a gap below the field instead of drawing over its edge */
  gtk_popover_set_offset (GTK_POPOVER (instance->suggestions_popover), 0, 6);
  gtk_popover_set_child (GTK_POPOVER (instance->suggestions_popover), GTK_WIDGET (instance->suggestions));
  gtk_widget_set_parent (instance->suggestions_popover, GTK_WIDGET (instance->add_guest_row));

  g_signal_connect_swapped (instance->add_guest_row, "changed", G_CALLBACK (on_add_guest_row_changed_cb), instance);

  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_add_guest_row_key_pressed_cb), instance);
  gtk_widget_add_controller (GTK_WIDGET (instance->add_guest_row), keys);

  instance->guest_rows = g_ptr_array_new ();
}

static void
gcal_attendees_section_class_init (GcalAttendeesSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GcalEventEditorSectionClass *section_class = GCAL_EVENT_EDITOR_SECTION_CLASS (klass);

  g_type_ensure (GCAL_TYPE_ORGANIZER_ROW);
  g_type_ensure (GCAL_TYPE_ATTENDEE_SUMMARY_ROW);

  object_class->dispose = gcal_attendees_section_dispose;
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
  gtk_widget_class_bind_template_child (widget_class, GcalAttendeesSection, join_row);

  gtk_widget_class_bind_template_callback (widget_class, on_join_button_clicked_cb);

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
  const gchar *organizer_uri;

  self->organizer = event ? gcal_event_get_organizer (event) : NULL;
  organizer_uri = self->organizer ? gcal_event_organizer_get_uri (self->organizer) : NULL;
  /* When editing, the organizer is the first guest row instead. */
  gtk_widget_set_visible (self->organizer_row, self->organizer != NULL && !self->can_edit_guests);

  for (guint i = 0; i < self->guest_rows->len; i++)
    adw_preferences_group_remove (ADW_PREFERENCES_GROUP (self), g_ptr_array_index (self->guest_rows, i));
  g_ptr_array_set_size (self->guest_rows, 0);

  for (guint i = 0; self->can_edit_guests && self->attendees && i < g_list_model_get_n_items (self->attendees); i++)
    {
      g_autoptr (GcalEventAttendee) attendee = g_list_model_get_item (self->attendees, i);
      GtkWidget *row = gcal_attendee_row_new (attendee);

      if (organizer_uri && g_ascii_strcasecmp (gcal_event_attendee_get_uri (attendee), organizer_uri) == 0)
        {
          const gchar *email = adw_action_row_get_subtitle (ADW_ACTION_ROW (row));
          g_autofree gchar *subtitle = email && *email ? g_strdup_printf ("%s · %s", email, gettext ("Organizer"))
                                                       : g_strdup (gettext ("Organizer"));

          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
        }
      else
        {
          gcal_attendee_row_set_removable (GCAL_ATTENDEE_ROW (row), TRUE);
        }

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (self), row);
      g_ptr_array_add (self->guest_rows, row);
    }

  /* The summary rows copy the guest list when it is set, so hand it over again. */
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ATTENDEES]);
  gtk_widget_set_visible (GTK_WIDGET (self),
                          self->organizer != NULL || gtk_widget_get_visible (GTK_WIDGET (self->add_guest_row)) ||
                          gtk_widget_get_visible (GTK_WIDGET (self->join_row)));
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

  g_free (self->meeting_url);
  self->meeting_url = g_strdup (meeting_url);
  gtk_widget_set_visible (GTK_WIDGET (self->join_row), meeting_url != NULL);
  adw_action_row_set_subtitle (self->join_row, meeting_url ? meeting_url : "");

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
