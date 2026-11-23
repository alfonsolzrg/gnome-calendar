/* gcal-google-meet.h
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "gcal-event.h"

G_BEGIN_DECLS

gboolean             gcal_google_meet_is_supported               (GcalCalendar       *calendar);

void                 gcal_google_meet_add                        (GcalEvent          *event);

G_END_DECLS
