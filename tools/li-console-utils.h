/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*-
 *
 * Copyright (C) 2015 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the license, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __LI_CONSOLE_UTILS_H
#define __LI_CONSOLE_UTILS_H

#include <glib-object.h>

G_BEGIN_DECLS

void			li_write_progress_step (const gchar *format, ...);
void			li_draw_progress_bar (const gchar *title, guint progress);
void			li_abort_progress_bar (void);

G_END_DECLS

#endif /* __LI_CONSOLE_UTILS_H */
