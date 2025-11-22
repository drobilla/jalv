// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_GTK_ACTIONS_H
#define JALV_GTK_ACTIONS_H

#include <gio/gio.h>
#include <glib.h>

void
action_delete_preset(GSimpleAction* action, GVariant* parameter, void* data);

void
action_load_preset(GSimpleAction* action, GVariant* parameter, void* data);

void
action_quit(GSimpleAction* action, GVariant* parameter, void* data);

void
action_save_as(GSimpleAction* action, GVariant* parameter, void* data);

void
action_save_preset(GSimpleAction* action, GVariant* parameter, void* data);

void
action_about(GSimpleAction* action, GVariant* parameter, void* data);

#endif // JALV_GTK_ACTIONS_H
