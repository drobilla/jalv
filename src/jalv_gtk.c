/*
  Copyright 2007-2017 David Robillard <d@drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "jalv_internal.h"

#include "lilv/lilv.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/atom/util.h"
#include "lv2/core/attributes.h"
#include "lv2/core/lv2.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"
#include "suil/suil.h"
#include "zix/common.h"
#include "zix/sem.h"

LV2_DISABLE_DEPRECATION_WARNINGS

#include <gdk/gdk.h>
#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static GtkWidget* gtk_pset_menu = NULL;
static GtkCheckMenuItem* active_preset_item = NULL;
static bool              updating           = false;

/** Widget for a control. */
typedef struct {
	GtkSpinButton* spin;
	GtkWidget*     control;
} Controller;

static float
get_float(const LilvNode* node, float fallback)
{
	if (lilv_node_is_float(node) || lilv_node_is_int(node)) {
		return lilv_node_as_float(node);
	}

	return fallback;
}

static GtkWidget*
new_box(gboolean horizontal, gint spacing)
{
	#if GTK_MAJOR_VERSION == 3
	return gtk_box_new(
		horizontal ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL,
		spacing);
	#else
	return (horizontal
	        ? gtk_hbox_new(FALSE, spacing)
	        : gtk_vbox_new(FALSE, spacing));
	#endif
}

static GtkWidget*
new_hscale(gdouble min, gdouble max, gdouble step)
{
	#if GTK_MAJOR_VERSION == 3
	return gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
	#else
	return gtk_hscale_new_with_range(min, max, step);
	#endif
}

static void
size_request(GtkWidget* widget, GtkRequisition* req)
{
	#if GTK_MAJOR_VERSION == 3
	gtk_widget_get_preferred_size(widget, NULL, req);
	#else
	gtk_widget_size_request(widget, req);
	#endif
}

static void
on_window_destroy(GtkWidget* ZIX_UNUSED(widget), gpointer ZIX_UNUSED(data))
{
	gtk_main_quit();
}

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	char cwd[256];
	if (getcwd(cwd, sizeof(cwd)-1) != NULL) {
		opts->preset_path = jalv_strdup(cwd);
	} else {
		opts->preset_path = jalv_strdup(g_get_home_dir());
	}

	GOptionEntry entries[] = {
		{ "load", 'l', 0, G_OPTION_ARG_STRING, &opts->load,
		  "Load state from save directory", "DIR" },
		{ "preset", 'p', 0, G_OPTION_ARG_STRING, &opts->preset,
		  "Load state from preset", "URI" },
		{ "dump", 'd', 0, G_OPTION_ARG_NONE, &opts->dump,
		  "Dump plugin <=> UI communication", NULL },
		{ "ui-uri", 'U', 0, G_OPTION_ARG_STRING, &opts->ui_uri,
		  "Load the UI with the given URI", "URI" },
		{ "trace", 't', 0, G_OPTION_ARG_NONE, &opts->trace,
		  "Print trace messages from plugin", NULL },
		{ "show-hidden", 's', 0, G_OPTION_ARG_NONE, &opts->show_hidden,
		  "Show controls for ports with notOnGUI property on generic UI", NULL },
		{ "no-menu", 'n', 0, G_OPTION_ARG_NONE, &opts->no_menu,
		  "Do not show Jalv menu on window", NULL },
		{ "generic-ui", 'g', 0, G_OPTION_ARG_NONE, &opts->generic_ui,
		  "Use Jalv generic UI and not the plugin UI", NULL},
		{ "buffer-size", 'b', 0, G_OPTION_ARG_INT, &opts->buffer_size,
		  "Buffer size for plugin <=> UI communication", "SIZE"},
		{ "update-frequency", 'r', 0, G_OPTION_ARG_DOUBLE, &opts->update_rate,
		  "UI update frequency", NULL},
		{ "control", 'c', 0, G_OPTION_ARG_STRING_ARRAY, &opts->controls,
		  "Set control value (e.g. \"vol=1.4\")", NULL},
		{ "print-controls", 'p', 0, G_OPTION_ARG_NONE, &opts->print_controls,
		  "Print control output changes to stdout", NULL},
		{ "jack-name", 'n', 0, G_OPTION_ARG_STRING, &opts->name,
		  "JACK client name", NULL},
		{ "exact-jack-name", 'x', 0, G_OPTION_ARG_NONE, &opts->name_exact,
		  "Exact JACK client name (exit if taken)", NULL },
		//{ "preset-path", 'P', 0, G_OPTION_ARG_NONE, &opts->preset_path,
		//  "Default path to save presets", "./lv2" },
		{ 0, 0, 0, G_OPTION_ARG_NONE, 0, 0, 0 } };
	GError* error = NULL;
	const int err = gtk_init_with_args(
		argc, argv,
		"PLUGIN_URI - Run an LV2 plugin as a Jack application",
		entries, NULL, &error);

	if (!err) {
		fprintf(stderr, "%s\n", error->message);
	}

	return !err;
}

const char*
jalv_native_ui_type(void)
{
#if GTK_MAJOR_VERSION == 2
	return "http://lv2plug.in/ns/extensions/ui#GtkUI";
#elif GTK_MAJOR_VERSION == 3
	return "http://lv2plug.in/ns/extensions/ui#Gtk3UI";
#else
	return NULL;
#endif
}

static void
on_save_activate(GtkWidget* ZIX_UNUSED(widget), void* ptr)
{
	Jalv* jalv = (Jalv*)ptr;
	GtkWidget* dialog = gtk_file_chooser_dialog_new(
		"Save State",
		(GtkWindow*)jalv->window,
		GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Save", GTK_RESPONSE_ACCEPT,
		NULL);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		char* base = g_build_filename(path, "/", NULL);
		jalv_save(jalv, base);
		g_free(path);
		g_free(base);
	}

	gtk_widget_destroy(dialog);
}

static void
on_quit_activate(GtkWidget* ZIX_UNUSED(widget), gpointer data)
{
	GtkWidget* window = (GtkWidget*)data;
	gtk_widget_destroy(window);
}

typedef struct {
	Jalv*     jalv;
	LilvNode* preset;
} PresetRecord;

static char*
symbolify(const char* in)
{
	const size_t len = strlen(in);
	char*        out = (char*)calloc(len + 1, 1);
	for (size_t i = 0; i < len; ++i) {
		if (g_ascii_isalnum(in[i])) {
			out[i] = in[i];
		} else {
			out[i] = '_';
		}
	}
	return out;
}

static void
set_window_title(Jalv* jalv)
{
	LilvNode*   name   = lilv_plugin_get_name(jalv->plugin);
	const char* plugin = lilv_node_as_string(name);
	if (jalv->preset) {
		const char* preset_label = lilv_state_get_label(jalv->preset);
		char* title = g_strdup_printf("%s - %s", plugin, preset_label);
		gtk_window_set_title(GTK_WINDOW(jalv->window), title);
		free(title);
	} else {
		gtk_window_set_title(GTK_WINDOW(jalv->window), plugin);
	}
	lilv_node_free(name);
}

static void
on_preset_activate(GtkWidget* widget, gpointer data)
{
	if (GTK_CHECK_MENU_ITEM(widget) != active_preset_item) {
		PresetRecord* record = (PresetRecord*)data;
		jalv_apply_preset(record->jalv, record->preset);
		if (active_preset_item) {
			gtk_check_menu_item_set_active(active_preset_item, FALSE);
		}

		active_preset_item = GTK_CHECK_MENU_ITEM(widget);
		gtk_check_menu_item_set_active(active_preset_item, TRUE);
		set_window_title(record->jalv);
	}
}

static void
on_preset_destroy(gpointer data, GClosure* ZIX_UNUSED(closure))
{
	PresetRecord* record = (PresetRecord*)data;
	lilv_node_free(record->preset);
	free(record);
}

typedef struct {
	GtkMenuItem* item;
	char*        label;
	GtkMenu*     menu;
	GSequence*   banks;
} PresetMenu;

static PresetMenu*
pset_menu_new(const char* label)
{
	PresetMenu* menu = (PresetMenu*)malloc(sizeof(PresetMenu));
	menu->label = g_strdup(label);
	menu->item  = GTK_MENU_ITEM(gtk_menu_item_new_with_label(menu->label));
	menu->menu  = GTK_MENU(gtk_menu_new());
	menu->banks = NULL;
	return menu;
}

static void
pset_menu_free(PresetMenu* menu)
{
	if (menu->banks) {
		for (GSequenceIter* i = g_sequence_get_begin_iter(menu->banks);
		     !g_sequence_iter_is_end(i);
		     i = g_sequence_iter_next(i)) {
			PresetMenu* bank_menu = (PresetMenu*)g_sequence_get(i);
			pset_menu_free(bank_menu);
		}
		g_sequence_free(menu->banks);
	}

	free(menu->label);
	free(menu);
}

static gint
menu_cmp(gconstpointer a, gconstpointer b, gpointer ZIX_UNUSED(data))
{
	return strcmp(((const PresetMenu*)a)->label, ((const PresetMenu*)b)->label);
}

static PresetMenu*
get_bank_menu(Jalv* jalv, PresetMenu* menu, const LilvNode* bank)
{
	LilvNode* label = lilv_world_get(
		jalv->world, bank, jalv->nodes.rdfs_label, NULL);

	const char*    uri = lilv_node_as_string(bank);
	const char*    str = label ? lilv_node_as_string(label) : uri;
	PresetMenu     key = { NULL, (char*)str, NULL, NULL };
	GSequenceIter* i   = g_sequence_lookup(menu->banks, &key, menu_cmp, NULL);
	if (!i) {
		PresetMenu* bank_menu = pset_menu_new(str);
		gtk_menu_item_set_submenu(bank_menu->item, GTK_WIDGET(bank_menu->menu));
		g_sequence_insert_sorted(menu->banks, bank_menu, menu_cmp, NULL);
		return bank_menu;
	}
	return (PresetMenu*)g_sequence_get(i);
}

static int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
	PresetMenu* menu  = (PresetMenu*)data;
	const char* label = lilv_node_as_string(title);
	GtkWidget*  item  = gtk_check_menu_item_new_with_label(label);
	gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
	if (jalv->preset &&
	    lilv_node_equals(lilv_state_get_uri(jalv->preset), node)) {
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
		active_preset_item = GTK_CHECK_MENU_ITEM(item);
	}

	LilvNode* bank = lilv_world_get(
		jalv->world, node, jalv->nodes.pset_bank, NULL);

	if (bank) {
		PresetMenu* bank_menu = get_bank_menu(jalv, menu, bank);
		gtk_menu_shell_append(GTK_MENU_SHELL(bank_menu->menu), item);
	} else {
		gtk_menu_shell_append(GTK_MENU_SHELL(menu->menu), item);
	}

	PresetRecord* record = (PresetRecord*)malloc(sizeof(PresetRecord));
	record->jalv   = jalv;
	record->preset = lilv_node_duplicate(node);

	g_signal_connect_data(G_OBJECT(item), "activate",
	                      G_CALLBACK(on_preset_activate),
	                      record, on_preset_destroy,
	                      (GConnectFlags)0);

	return 0;
}

static void
finish_menu(PresetMenu* menu)
{
	for (GSequenceIter* i = g_sequence_get_begin_iter(menu->banks);
	     !g_sequence_iter_is_end(i);
	     i = g_sequence_iter_next(i)) {
		PresetMenu* bank_menu = (PresetMenu*)g_sequence_get(i);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu->menu),
		                      GTK_WIDGET(bank_menu->item));
	}
	g_sequence_free(menu->banks);
}

static void
rebuild_preset_menu(Jalv* jalv, GtkContainer* pset_menu)
{
	// Clear current menu
	active_preset_item = NULL;
	for (GList* items = g_list_nth(gtk_container_get_children(pset_menu), 3);
	     items;
	     items = items->next) {
		gtk_container_remove(pset_menu, GTK_WIDGET(items->data));
	}

	// Load presets and build new menu
	PresetMenu menu = {
		NULL, NULL, GTK_MENU(pset_menu),
		g_sequence_new((GDestroyNotify)pset_menu_free)
	};
	jalv_load_presets(jalv, add_preset_to_menu, &menu);
	finish_menu(&menu);
	gtk_widget_show_all(GTK_WIDGET(pset_menu));
}

static void
on_save_preset_activate(GtkWidget* widget, void* ptr)
{
	Jalv* jalv = (Jalv*)ptr;

	GtkWidget* dialog = gtk_file_chooser_dialog_new(
		"Save Preset",
		(GtkWindow*)jalv->window,
		GTK_FILE_CHOOSER_ACTION_SAVE,
		"_Cancel", GTK_RESPONSE_REJECT,
		"_Save", GTK_RESPONSE_ACCEPT,
		NULL);

	char* dot_lv2;
	char cwd[256];
	if (getcwd(cwd, sizeof(cwd)-1) != NULL) {
		dot_lv2 = g_build_filename(cwd, ".lv2", NULL);
	} else {
		dot_lv2 = g_build_filename(g_get_home_dir(), ".lv2", NULL);
	}
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dot_lv2);
	free(dot_lv2);

	GtkWidget* content    = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	GtkBox*    box        = GTK_BOX(new_box(true, 8));
	GtkWidget* uri_label  = gtk_label_new("URI (Optional):");
	GtkWidget* uri_entry  = gtk_entry_new();
	GtkWidget* add_prefix = gtk_check_button_new_with_mnemonic(
		"_Prefix plugin name");

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(add_prefix), TRUE);
	gtk_box_pack_start(box, uri_label, FALSE, TRUE, 2);
	gtk_box_pack_start(box, uri_entry, TRUE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(box), FALSE, FALSE, 6);
	gtk_box_pack_start(GTK_BOX(content), add_prefix, FALSE, FALSE, 6);

	gtk_widget_show_all(GTK_WIDGET(dialog));
	gtk_entry_set_activates_default(GTK_ENTRY(uri_entry), TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		LilvNode*   plug_name = lilv_plugin_get_name(jalv->plugin);
		const char* path      = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		const char* uri       = gtk_entry_get_text(GTK_ENTRY(uri_entry));
		const char* prefix    = "";
		const char* sep       = "";
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_prefix))) {
			prefix = lilv_node_as_string(plug_name);
			sep    = "_";
		}

		char* dirname  = g_path_get_dirname(path);
		char* basename = g_path_get_basename(path);
		char* sym      = symbolify(basename);
		char* sprefix  = symbolify(prefix);
		char* bundle   = g_strjoin(NULL, sprefix, sep, sym, ".preset.lv2/", NULL);
		char* file     = g_strjoin(NULL, sym, ".ttl", NULL);
		char* dir      = g_build_filename(dirname, bundle, NULL);

		jalv_save_preset(jalv, dir, (strlen(uri) ? uri : NULL), basename, file);

		// Reload bundle into the world
		LilvNode* ldir = lilv_new_file_uri(jalv->world, NULL, dir);
		lilv_world_unload_bundle(jalv->world, ldir);
		lilv_world_load_bundle(jalv->world, ldir);
		lilv_node_free(ldir);

		// Rebuild preset menu and update window title
		rebuild_preset_menu(jalv, GTK_CONTAINER(gtk_widget_get_parent(widget)));
		set_window_title(jalv);

		g_free(dir);
		g_free(file);
		g_free(bundle);
		free(sprefix);
		free(sym);
		g_free(basename);
		g_free(dirname);
		lilv_node_free(plug_name);
	}

	gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
on_delete_preset_activate(GtkWidget* widget, void* ptr)
{
	Jalv* jalv = (Jalv*)ptr;
	if (!jalv->preset) {
		return;
	}

	GtkWidget* dialog = gtk_dialog_new_with_buttons(
		"Delete Preset?",
		(GtkWindow*)jalv->window,
		(GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
		"_Cancel", GTK_RESPONSE_REJECT,
		"_OK", GTK_RESPONSE_ACCEPT,
		NULL);

	char* msg = g_strdup_printf("Delete preset \"%s\" from the file system?",
	                            lilv_state_get_label(jalv->preset));

	GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	GtkWidget* text    = gtk_label_new(msg);
	gtk_box_pack_start(GTK_BOX(content), text, TRUE, TRUE, 4);

	gtk_widget_show_all(dialog);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		jalv_delete_current_preset(jalv);
		rebuild_preset_menu(jalv, GTK_CONTAINER(gtk_widget_get_parent(widget)));
	}

	lilv_state_free(jalv->preset);
	jalv->preset = NULL;
	set_window_title(jalv);

	g_free(msg);
	gtk_widget_destroy(text);
	gtk_widget_destroy(dialog);
}

static void
set_control(const ControlID* control,
            uint32_t         size,
            LV2_URID         type,
            const void*      body)
{
	if (!updating) {
		jalv_set_control(control, size, type, body);
	}
}

static bool
differ_enough(float a, float b)
{
	return fabsf(a - b) >= FLT_EPSILON;
}

static void
set_float_control(const ControlID* control, float value)
{
	if (control->value_type == control->jalv->forge.Int) {
		const int32_t ival = lrintf(value);
		set_control(control, sizeof(ival), control->jalv->forge.Int, &ival);
	} else if (control->value_type == control->jalv->forge.Long) {
		const int64_t lval = lrintf(value);
		set_control(control, sizeof(lval), control->jalv->forge.Long, &lval);
	} else if (control->value_type == control->jalv->forge.Float) {
		set_control(control, sizeof(value), control->jalv->forge.Float, &value);
	} else if (control->value_type == control->jalv->forge.Double) {
		const double dval = value;
		set_control(control, sizeof(dval), control->jalv->forge.Double, &dval);
	} else if (control->value_type == control->jalv->forge.Bool) {
		const int32_t ival = value;
		set_control(control, sizeof(ival), control->jalv->forge.Bool, &ival);
	}

	Controller* controller = (Controller*)control->widget;
	if (controller && controller->spin &&
	    differ_enough(gtk_spin_button_get_value(controller->spin), value)) {
		gtk_spin_button_set_value(controller->spin, value);
	}
}

static double
get_atom_double(Jalv*       jalv,
                uint32_t    ZIX_UNUSED(size),
                LV2_URID    type,
                const void* body)
{
	if (type == jalv->forge.Int || type == jalv->forge.Bool) {
		return *(const int32_t*)body;
	} else if (type == jalv->forge.Long) {
		return *(const int64_t*)body;
	} else if (type == jalv->forge.Float) {
		return *(const float*)body;
	} else if (type == jalv->forge.Double) {
		return *(const double*)body;
	}
	return NAN;
}

static void
control_changed(Jalv*       jalv,
                Controller* controller,
                uint32_t    size,
                LV2_URID    type,
                const void* body)
{
	GtkWidget*   widget = controller->control;
	const double fvalue = get_atom_double(jalv, size, type, body);

	if (!isnan(fvalue)) {
		if (GTK_IS_COMBO_BOX(widget)) {
			GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
			GValue        value = { 0, { { 0 } } };
			GtkTreeIter   i;
			bool          valid = gtk_tree_model_get_iter_first(model, &i);
			while (valid) {
				gtk_tree_model_get_value(model, &i, 0, &value);
				const double v = g_value_get_float(&value);
				g_value_unset(&value);
				if (fabs(v - fvalue) < FLT_EPSILON) {
					gtk_combo_box_set_active_iter(GTK_COMBO_BOX(widget), &i);
					return;
				}
				valid = gtk_tree_model_iter_next(model, &i);
			}
		} else if (GTK_IS_TOGGLE_BUTTON(widget)) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
			                             fvalue > 0.0f);
		} else if (GTK_IS_RANGE(widget)) {
			gtk_range_set_value(GTK_RANGE(widget), fvalue);
		} else {
			fprintf(stderr, "Unknown widget type for value\n");
		}

		if (controller->spin) {
			// Update spinner for numeric control
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(controller->spin),
			                          fvalue);
		}
	} else if (GTK_IS_ENTRY(widget) && type == jalv->urids.atom_String) {
		gtk_entry_set_text(GTK_ENTRY(widget), (const char*)body);
	} else if (GTK_IS_FILE_CHOOSER(widget) && type == jalv->urids.atom_Path) {
		gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(widget), (const char*)body);
	} else {
		fprintf(stderr, "Unknown widget type for value\n");
	}
}

static int
patch_set_get(Jalv*                  jalv,
              const LV2_Atom_Object* obj,
              const LV2_Atom_URID**  property,
              const LV2_Atom**       value)
{
	lv2_atom_object_get(obj,
	                    jalv->urids.patch_property, (const LV2_Atom*)property,
	                    jalv->urids.patch_value,    value,
	                    0);
	if (!*property) {
		fprintf(stderr, "patch:Set message with no property\n");
		return 1;
	} else if ((*property)->atom.type != jalv->forge.URID) {
		fprintf(stderr, "patch:Set property is not a URID\n");
		return 1;
	}

	return 0;
}

static int
patch_put_get(Jalv*                   jalv,
              const LV2_Atom_Object*  obj,
              const LV2_Atom_Object** body)
{
	lv2_atom_object_get(obj,
	                    jalv->urids.patch_body, (const LV2_Atom*)body,
	                    0);
	if (!*body) {
		fprintf(stderr, "patch:Put message with no body\n");
		return 1;
	} else if (!lv2_atom_forge_is_object_type(&jalv->forge, (*body)->atom.type)) {
		fprintf(stderr, "patch:Put body is not an object\n");
		return 1;
	}

	return 0;
}

static LV2UI_Request_Value_Status
on_request_value(LV2UI_Feature_Handle      handle,
                 const LV2_URID            key,
                 const LV2_URID            ZIX_UNUSED(type),
                 const LV2_Feature* const* ZIX_UNUSED(features))
{
	Jalv*      jalv    = (Jalv*)handle;
	ControlID* control = get_property_control(&jalv->controls, key);

	if (!control) {
		return LV2UI_REQUEST_VALUE_ERR_UNKNOWN;
	} else if (control->value_type != jalv->forge.Path) {
		return LV2UI_REQUEST_VALUE_ERR_UNSUPPORTED;
	}

	GtkWidget* dialog =
	        gtk_file_chooser_dialog_new("Choose file",
	                                    GTK_WINDOW(jalv->window),
	                                    GTK_FILE_CHOOSER_ACTION_OPEN,
	                                    GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
	                                    GTK_STOCK_OK, GTK_RESPONSE_OK,
	                                    NULL);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

		set_control(control, strlen(path) + 1, jalv->forge.Path, path);

		g_free(path);
	}

	gtk_widget_destroy(dialog);

	return 0;
}

static void
property_changed(Jalv* jalv, LV2_URID key, const LV2_Atom* value)
{
	ControlID* control = get_property_control(&jalv->controls, key);
	if (control) {
		control_changed(jalv,
		                (Controller*)control->widget,
		                value->size,
		                value->type,
		                value + 1);
	}
}

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer)
{
	if (jalv->ui_instance) {
		suil_instance_port_event(jalv->ui_instance, port_index,
		                         buffer_size, protocol, buffer);
		return;
	} else if (protocol == 0 && (Controller*)jalv->ports[port_index].widget) {
		control_changed(jalv,
		                (Controller*)jalv->ports[port_index].widget,
		                buffer_size,
		                jalv->forge.Float,
		                buffer);
		return;
	} else if (protocol == 0) {
		return;  // No widget (probably notOnGUI)
	} else if (protocol != jalv->urids.atom_eventTransfer) {
		fprintf(stderr, "Unknown port event protocol\n");
		return;
	}

	const LV2_Atom* atom = (const LV2_Atom*)buffer;
	if (lv2_atom_forge_is_object_type(&jalv->forge, atom->type)) {
		updating = true;
		const LV2_Atom_Object* obj = (const LV2_Atom_Object*)buffer;
		if (obj->body.otype == jalv->urids.patch_Set) {
			const LV2_Atom_URID* property = NULL;
			const LV2_Atom*      value    = NULL;
			if (!patch_set_get(jalv, obj, &property, &value)) {
				property_changed(jalv, property->body, value);
			}
		} else if (obj->body.otype == jalv->urids.patch_Put) {
			const LV2_Atom_Object* body = NULL;
			if (!patch_put_get(jalv, obj, &body)) {
				LV2_ATOM_OBJECT_FOREACH(body, prop) {
					property_changed(jalv, prop->key, &prop->value);
				}
			}
		} else {
			printf("Unknown object type?\n");
		}
		updating = false;
	}
}

static gboolean
scale_changed(GtkRange* range, gpointer data)
{
	set_float_control((const ControlID*)data, gtk_range_get_value(range));
	return FALSE;
}

static gboolean
spin_changed(GtkSpinButton* spin, gpointer data)
{
	const ControlID* control    = (const ControlID*)data;
	Controller*      controller = (Controller*)control->widget;
	GtkRange*        range      = GTK_RANGE(controller->control);
	const double     value      = gtk_spin_button_get_value(spin);
	if (differ_enough(gtk_range_get_value(range), value)) {
		gtk_range_set_value(range, value);
	}
	return FALSE;
}

static gboolean
log_scale_changed(GtkRange* range, gpointer data)
{
	set_float_control((const ControlID*)data, expf(gtk_range_get_value(range)));
	return FALSE;
}

static gboolean
log_spin_changed(GtkSpinButton* spin, gpointer data)
{
	const ControlID* control    = (const ControlID*)data;
	Controller*      controller = (Controller*)control->widget;
	GtkRange*        range      = GTK_RANGE(controller->control);
	const double     value      = gtk_spin_button_get_value(spin);
	if (differ_enough(gtk_range_get_value(range), logf(value))) {
		gtk_range_set_value(range, logf(value));
	}
	return FALSE;
}

static void
combo_changed(GtkComboBox* box, gpointer data)
{
	const ControlID* control = (const ControlID*)data;

	GtkTreeIter iter;
	if (gtk_combo_box_get_active_iter(box, &iter)) {
		GtkTreeModel* model = gtk_combo_box_get_model(box);
		GValue        value = { 0, { { 0 } } };

		gtk_tree_model_get_value(model, &iter, 0, &value);
		const double v = g_value_get_float(&value);
		g_value_unset(&value);

		set_float_control(control, v);
	}
}

static gboolean
toggle_changed(GtkToggleButton* button, gpointer data)
{
	set_float_control((const ControlID*)data,
	                  gtk_toggle_button_get_active(button) ? 1.0f : 0.0f);
	return FALSE;
}

static void
string_changed(GtkEntry* widget, gpointer data)
{
	ControlID*  control = (ControlID*)data;
	const char* string  = gtk_entry_get_text(widget);

	set_control(control, strlen(string) + 1, control->jalv->forge.String, string);
}

static void
file_changed(GtkFileChooserButton* widget,
             gpointer              data)
{
	ControlID*  control   = (ControlID*)data;
	Jalv*       jalv     = control->jalv;
	const char* filename = gtk_file_chooser_get_filename(
		GTK_FILE_CHOOSER(widget));

	set_control(control, strlen(filename) + 1, jalv->forge.Path, filename);
}

static Controller*
new_controller(GtkSpinButton* spin, GtkWidget* control)
{
	Controller* controller = (Controller*)malloc(sizeof(Controller));
	controller->spin    = spin;
	controller->control = control;
	return controller;
}

static Controller*
make_combo(ControlID* record, float value)
{
	GtkListStore* list_store = gtk_list_store_new(
		2, G_TYPE_FLOAT, G_TYPE_STRING);
	int active = -1;
	for (size_t i = 0; i < record->n_points; ++i) {
		const ScalePoint* point = &record->points[i];
		GtkTreeIter       iter;
		gtk_list_store_append(list_store, &iter);
		gtk_list_store_set(list_store, &iter,
		                   0, point->value,
		                   1, point->label,
		                   -1);
		if (fabsf(value - point->value) < FLT_EPSILON) {
			active = i;
		}
	}

	GtkWidget* combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(list_store));
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
	g_object_unref(list_store);

	gtk_widget_set_sensitive(combo, record->is_writable);

	GtkCellRenderer* cell = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cell, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cell, "text", 1, NULL);

	if (record->is_writable) {
		g_signal_connect(G_OBJECT(combo), "changed",
		                 G_CALLBACK(combo_changed), record);
	}

	return new_controller(NULL, combo);
}

static Controller*
make_log_slider(ControlID* record, float value)
{
	const float min   = get_float(record->min, 0.0f);
	const float max   = get_float(record->max, 1.0f);
	const float lmin  = logf(min);
	const float lmax  = logf(max);
	const float ldft  = logf(value);
	GtkWidget*  scale = new_hscale(lmin, lmax, 0.001);
	GtkWidget*  spin  = gtk_spin_button_new_with_range(min, max, 0.000001);

	gtk_widget_set_sensitive(scale, record->is_writable);
	gtk_widget_set_sensitive(spin, record->is_writable);

	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_range_set_value(GTK_RANGE(scale), ldft);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);

	if (record->is_writable) {
		g_signal_connect(G_OBJECT(scale), "value-changed",
		                 G_CALLBACK(log_scale_changed), record);
		g_signal_connect(G_OBJECT(spin), "value-changed",
		                 G_CALLBACK(log_spin_changed), record);
	}

	return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_slider(ControlID* record, float value)
{
	const float  min   = get_float(record->min, 0.0f);
	const float  max   = get_float(record->max, 1.0f);
	const double step  = record->is_integer ? 1.0 : ((max - min) / 100.0);
	GtkWidget*   scale = new_hscale(min, max, step);
	GtkWidget*   spin  = gtk_spin_button_new_with_range(min, max, step);

	gtk_widget_set_sensitive(scale, record->is_writable);
	gtk_widget_set_sensitive(spin, record->is_writable);

	if (record->is_integer) {
		gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 0);
	} else {
		gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 7);
	}

	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_range_set_value(GTK_RANGE(scale), value);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
	if (record->points) {
		for (size_t i = 0; i < record->n_points; ++i) {
			const ScalePoint* point = &record->points[i];

			gchar* str = g_markup_printf_escaped(
				"<span font_size=\"small\">%s</span>", point->label);
			gtk_scale_add_mark(
				GTK_SCALE(scale), point->value, GTK_POS_TOP, str);
		}
	}

	if (record->is_writable) {
		g_signal_connect(G_OBJECT(scale), "value-changed",
		                 G_CALLBACK(scale_changed), record);
		g_signal_connect(G_OBJECT(spin), "value-changed",
		                 G_CALLBACK(spin_changed), record);
	}

	return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_toggle(ControlID* record, float value)
{
	GtkWidget* check = gtk_check_button_new();

	gtk_widget_set_sensitive(check, record->is_writable);

	if (value) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
	}

	if (record->is_writable) {
		g_signal_connect(G_OBJECT(check), "toggled",
		                 G_CALLBACK(toggle_changed), record);
	}

	return new_controller(NULL, check);
}

static Controller*
make_entry(ControlID* control)
{
	GtkWidget* entry = gtk_entry_new();

	gtk_widget_set_sensitive(entry, control->is_writable);
	if (control->is_writable) {
		g_signal_connect(G_OBJECT(entry), "activate",
		                 G_CALLBACK(string_changed), control);
	}

	return new_controller(NULL, entry);
}

static Controller*
make_file_chooser(ControlID* record)
{
	GtkWidget* button = gtk_file_chooser_button_new(
		"Open File", GTK_FILE_CHOOSER_ACTION_OPEN);

	gtk_widget_set_sensitive(button, record->is_writable);

	if (record->is_writable) {
		g_signal_connect(G_OBJECT(button), "file-set",
		                 G_CALLBACK(file_changed), record);
	}

	return new_controller(NULL, button);
}

static Controller*
make_controller(ControlID* control, float value)
{
	Controller* controller = NULL;

	if (control->is_toggle) {
		controller = make_toggle(control, value);
	} else if (control->is_enumeration) {
		controller = make_combo(control, value);
	} else if (control->is_logarithmic) {
		controller = make_log_slider(control, value);
	} else {
		controller = make_slider(control, value);
	}

	return controller;
}

static GtkWidget*
new_label(const char* text, bool title, float xalign, float yalign)
{
	GtkWidget*  label = gtk_label_new(NULL);
	const char* fmt   = title ? "<span font_weight=\"bold\">%s</span>" : "%s:";
	gchar*      str   = g_markup_printf_escaped(fmt, text);
	gtk_label_set_markup(GTK_LABEL(label), str);
	g_free(str);
	gtk_misc_set_alignment(GTK_MISC(label), xalign, yalign);
	return label;
}

static void
add_control_row(GtkWidget*  table,
                int         row,
                const char* name,
                Controller* controller)
{
	GtkWidget* label = new_label(name, false, 1.0, 0.5);
	gtk_table_attach(GTK_TABLE(table),
	                 label,
	                 0, 1, row, row + 1,
	                 GTK_FILL, (GtkAttachOptions)(GTK_FILL|GTK_EXPAND), 8, 1);
	int control_left_attach = 1;
	if (controller->spin) {
		control_left_attach = 2;
		gtk_table_attach(GTK_TABLE(table), GTK_WIDGET(controller->spin),
		                 1, 2, row, row + 1,
		                 GTK_FILL, GTK_FILL, 2, 1);
	}
	gtk_table_attach(GTK_TABLE(table), controller->control,
	                 control_left_attach, 3, row, row + 1,
	                 (GtkAttachOptions)(GTK_FILL|GTK_EXPAND), GTK_FILL, 2, 1);
}

static int
control_group_cmp(const void* p1, const void* p2, void* ZIX_UNUSED(data))
{
	const ControlID* control1 = *(const ControlID*const*)p1;
	const ControlID* control2 = *(const ControlID*const*)p2;

	const int cmp = (control1->group && control2->group)
		? strcmp(lilv_node_as_string(control1->group),
		         lilv_node_as_string(control2->group))
		: ((intptr_t)control1->group - (intptr_t)control2->group);

	return cmp;
}

static GtkWidget*
build_control_widget(Jalv* jalv, GtkWidget* window)
{
	GtkWidget* port_table = gtk_table_new(jalv->num_ports, 3, false);

	/* Make an array of controls sorted by group */
	GArray* controls = g_array_new(FALSE, TRUE, sizeof(ControlID*));
	for (unsigned i = 0; i < jalv->controls.n_controls; ++i) {
		g_array_append_vals(controls, &jalv->controls.controls[i], 1);
	}
	g_array_sort_with_data(controls, control_group_cmp, jalv);

	/* Add controls in group order */
	LilvNode* last_group = NULL;
	int       n_rows     = 0;
	for (size_t i = 0; i < controls->len; ++i) {
		ControlID*  record     = g_array_index(controls, ControlID*, i);
		Controller* controller = NULL;
		LilvNode*   group      = record->group;

		/* Check group and add new heading if necessary */
		if (group && !lilv_node_equals(group, last_group)) {
			LilvNode* group_name = lilv_world_get(
				jalv->world, group, jalv->nodes.lv2_name, NULL);
			GtkWidget* group_label = new_label(
				lilv_node_as_string(group_name), true, 0.0f, 1.0f);
			gtk_table_attach(GTK_TABLE(port_table), group_label,
			                 0, 2, n_rows, n_rows + 1,
			                 GTK_FILL, GTK_FILL, 0, 6);
			++n_rows;
		}
		last_group = group;

		/* Make control widget */
		if (record->value_type == jalv->forge.String) {
			controller = make_entry(record);
		} else if (record->value_type == jalv->forge.Path) {
			controller = make_file_chooser(record);
		} else {
			const float val = get_float(record->def, 0.0f);
			controller = make_controller(record, val);
		}

		record->widget = controller;
		if (record->type == PORT) {
			jalv->ports[record->index].widget = controller;
		}
		if (controller) {
			/* Add row to table for this controller */
			add_control_row(
				port_table, n_rows++,
				(record->label
				 ? lilv_node_as_string(record->label)
				 : lilv_node_as_uri(record->node)),
				controller);

			/* Set tooltip text from comment, if available */
			LilvNode* comment = lilv_world_get(
				jalv->world, record->node, jalv->nodes.rdfs_comment, NULL);
			if (comment) {
				gtk_widget_set_tooltip_text(controller->control,
				                            lilv_node_as_string(comment));
			}
			lilv_node_free(comment);
		}
	}

	if (n_rows > 0) {
		gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
		GtkWidget* alignment = gtk_alignment_new(0.5, 0.0, 1.0, 0.0);
		gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 0, 0, 8, 8);
		gtk_container_add(GTK_CONTAINER(alignment), port_table);
		return alignment;
	} else {
		gtk_widget_destroy(port_table);
		GtkWidget* button = gtk_button_new_with_label("Close");
		g_signal_connect_swapped(button, "clicked",
		                         G_CALLBACK(gtk_widget_destroy), window);
		gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
		return button;
	}
}

static void
build_menu(Jalv* jalv, GtkWidget* window, GtkWidget* vbox)
{
	GtkWidget* menu_bar  = gtk_menu_bar_new();
	GtkWidget* file      = gtk_menu_item_new_with_mnemonic("_File");
	GtkWidget* file_menu = gtk_menu_new();

	GtkAccelGroup* ag = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(window), ag);

	GtkWidget* save = gtk_image_menu_item_new_from_stock(GTK_STOCK_SAVE, ag);
	GtkWidget* quit = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, ag);

	gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), file_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), save);
	gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file);

	GtkWidget* pset_item   = gtk_menu_item_new_with_mnemonic("_Presets");
	GtkWidget* pset_menu   = gtk_menu_new();
	GtkWidget* save_preset = gtk_menu_item_new_with_mnemonic(
		"_Save Preset...");
	GtkWidget* delete_preset = gtk_menu_item_new_with_mnemonic(
		"_Delete Current Preset...");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(pset_item), pset_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu), save_preset);
	gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu), delete_preset);
	gtk_menu_shell_append(GTK_MENU_SHELL(pset_menu),
	                      gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), pset_item);

	PresetMenu menu = {
		NULL, NULL, GTK_MENU(pset_menu),
		g_sequence_new((GDestroyNotify)pset_menu_free)
	};
	jalv_load_presets(jalv, add_preset_to_menu, &menu);
	finish_menu(&menu);

	//Save pset_menu as global for rebuilding from CLI
	gtk_pset_menu = pset_menu;
	
	g_signal_connect(G_OBJECT(quit), "activate",
	                 G_CALLBACK(on_quit_activate), window);

	g_signal_connect(G_OBJECT(save), "activate",
	                 G_CALLBACK(on_save_activate), jalv);

	g_signal_connect(G_OBJECT(save_preset), "activate",
	                 G_CALLBACK(on_save_preset_activate), jalv);

	g_signal_connect(G_OBJECT(delete_preset), "activate",
	                 G_CALLBACK(on_delete_preset_activate), jalv);

	gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);
}

bool
jalv_discover_ui(Jalv* ZIX_UNUSED(jalv))
{
	return TRUE;
}

float
jalv_ui_refresh_rate(Jalv* ZIX_UNUSED(jalv))
{
#if GTK_MAJOR_VERSION == 2
	return 30.0f;
#else
	GdkDisplay* const display = gdk_display_get_default();
	GdkMonitor* const monitor = gdk_display_get_primary_monitor(display);

	const float rate = (float)gdk_monitor_get_refresh_rate(monitor);

	return rate < 30.0f ? 30.0f : rate;
#endif
}

pthread_t init_cli_thread(Jalv* jalv);

int
jalv_open_ui(Jalv* jalv)
{
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	jalv->window = window;

	g_signal_connect(window, "destroy",
	                 G_CALLBACK(on_window_destroy), jalv);

	set_window_title(jalv);

	GtkWidget* vbox = new_box(false, 0);
	gtk_window_set_role(GTK_WINDOW(window), "plugin_ui");
	gtk_container_add(GTK_CONTAINER(window), vbox);

	if (!jalv->opts.no_menu) {
		build_menu(jalv, window, vbox);
	}

	/* Create/show alignment to contain UI (whether custom or generic) */
	GtkWidget* alignment = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
	gtk_box_pack_start(GTK_BOX(vbox), alignment, TRUE, TRUE, 0);
	gtk_widget_show(alignment);

	/* Attempt to instantiate custom UI if necessary */
	if (jalv->ui && !jalv->opts.generic_ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(), alignment);
	}

	jalv->features.request_value.request = on_request_value;

	if (jalv->ui_instance) {
		GtkWidget* widget = (GtkWidget*)suil_instance_get_widget(
			jalv->ui_instance);

		gtk_container_add(GTK_CONTAINER(alignment), widget);
		gtk_window_set_resizable(GTK_WINDOW(window), jalv_ui_is_resizable(jalv));
		gtk_widget_show_all(vbox);
		gtk_widget_grab_focus(widget);
	} else {
		GtkWidget* controls   = build_control_widget(jalv, window);
		GtkWidget* scroll_win = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_add_with_viewport(
			GTK_SCROLLED_WINDOW(scroll_win), controls);
		gtk_scrolled_window_set_policy(
			GTK_SCROLLED_WINDOW(scroll_win),
			GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_container_add(GTK_CONTAINER(alignment), scroll_win);
		gtk_widget_show_all(vbox);

		GtkRequisition controls_size = {0, 0};
		GtkRequisition box_size      = {0, 0};
		size_request(GTK_WIDGET(controls), &controls_size);
		size_request(GTK_WIDGET(vbox), &box_size);

		gtk_window_set_default_size(
			GTK_WINDOW(window),
			MAX(MAX(box_size.width, controls_size.width) + 24, 640),
			box_size.height + controls_size.height);
	}

	jalv_init_ui(jalv);

	g_timeout_add(1000 / jalv->ui_update_hz, (GSourceFunc)jalv_update, jalv);

	gtk_window_present(GTK_WINDOW(window));

	init_cli_thread(jalv);
	gtk_main();

	suil_instance_free(jalv->ui_instance);
	jalv->ui_instance = NULL;
	zix_sem_post(&jalv->done);
	return 0;
}

int
jalv_close_ui(Jalv* ZIX_UNUSED(jalv))
{
	gtk_main_quit();
	return 0;
}

//************************************************************

static void
jalv_print_controls(Jalv* jalv, bool writable, bool readable)
{
	for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
		ControlID* const control = jalv->controls.controls[i];
		if ((control->is_writable && writable) ||
		    (control->is_readable && readable)) {
			struct Port* const port = &jalv->ports[control->index];
			printf("%s = %f\n",
			       lilv_node_as_string(control->symbol),
			       port->control);
		}
	}
}

static int
jalv_print_preset(Jalv*           ZIX_UNUSED(jalv),
                  const LilvNode* node,
                  const LilvNode* title,
                  void*           ZIX_UNUSED(data))
{
	printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
	return 0;
}

static void
jalv_process_command(Jalv* jalv, const char* cmd)
{
	char     sym[1024];
	char     sym2[1024];
	uint32_t index = 0;
	float    value = 0.0f;
	int      count;
	if (!strncmp(cmd, "help", 4)) {
		fprintf(stderr,
		        "Commands:\n"
		        "  help              Display this help message\n"
		        "  controls          Print settable control values\n"
		        "  monitors          Print output control values\n"
		        "  presets           Print available presets\n"
		        "  preset URI        Set preset\n"
		        "  save preset [BANK_URI,] LABEL\n"
		        "                    Save preset (BANK_URI is optional)\n"
		        "  set INDEX VALUE   Set control value by port index\n"
		        "  set SYMBOL VALUE  Set control value by symbol\n"
		        "  SYMBOL = VALUE    Set control value by symbol\n");
	} else if (strcmp(cmd, "presets\n") == 0) {
		jalv_unload_presets(jalv);
		jalv_load_presets(jalv, jalv_print_preset, NULL);
	} else if (sscanf(cmd, "preset %1023[-a-zA-Z0-9_:/.%%#]", sym) == 1) {
		LilvNode* preset = lilv_new_uri(jalv->world, sym);
		lilv_world_load_resource(jalv->world, preset);
		jalv_apply_preset(jalv, preset);
		set_window_title(jalv);
		lilv_node_free(preset);
		jalv_print_controls(jalv, true, false);
	} else if (sscanf(cmd, "save preset %1023[-a-zA-Z0-9_:/.%%#, ]", sym) == 1) {
		char dir_preset[1024];
		char fname_preset[1024];
		char *plugin_name = jalv_get_plugin_name(jalv);
		char *saveptr = sym;
		char *bank_uri = strtok_r(sym, ",", &saveptr);
		char *label_preset = strtok_r(NULL, ",", &saveptr);
		if (!label_preset) {
			label_preset = bank_uri;
			bank_uri = NULL;
		}
		sprintf(dir_preset, "%s/%s.presets.lv2/", jalv->opts.preset_path, plugin_name);
		sprintf(fname_preset, "%s.ttl", label_preset);
		jalv_fix_filename(fname_preset);
		if (bank_uri) {
			jalv_save_bank_preset(jalv, dir_preset, bank_uri, NULL, label_preset, fname_preset);
		} else {
			jalv_save_preset(jalv, dir_preset, NULL, label_preset, fname_preset);
		}
		//Print saved preset uri
		printf("file://%s%s\n", dir_preset, fname_preset);
		// Reload bundle into the world
		LilvNode* ldir = lilv_new_file_uri(jalv->world, NULL, dir_preset);
		lilv_world_unload_bundle(jalv->world, ldir);
		lilv_world_load_bundle(jalv->world, ldir);
		lilv_node_free(ldir);
		free(plugin_name);
		// Rebuild preset menu and update window title
		rebuild_preset_menu(jalv, GTK_CONTAINER(gtk_pset_menu));
		set_window_title(jalv);
	} else if (strcmp(cmd, "controls\n") == 0) {
		jalv_print_controls(jalv, true, false);
	} else if (strcmp(cmd, "monitors\n") == 0) {
		jalv_print_controls(jalv, false, true);
	} else if (sscanf(cmd, "set %u %f", &index, &value) == 2) {
		if (index < jalv->num_ports) {
			jalv->ports[index].control = value;
			jalv_print_control(jalv, &jalv->ports[index], value);
		} else {
			fprintf(stderr, "error: port index out of range\n");
		}
	} else if (sscanf(cmd, "set %1023[a-zA-Z0-9_] %f", sym, &value) == 2 ||
	           sscanf(cmd, "%1023[a-zA-Z0-9_] = %f", sym, &value) == 2) {
		struct Port* port = NULL;
		for (uint32_t i = 0; i < jalv->num_ports; ++i) {
			struct Port* p = &jalv->ports[i];
			const LilvNode* s = lilv_port_get_symbol(jalv->plugin, p->lilv_port);
			if (!strcmp(lilv_node_as_string(s), sym)) {
				port = p;
				break;
			}
		}
		if (port) {
			port->control = value;
			jalv_print_control(jalv, port, value);
		} else {
			fprintf(stderr, "error: no control named `%s'\n", sym);
		}
	} else {
		fprintf(stderr, "error: invalid command (try `help')\n");
	}
}


void * cli_thread(void *arg) {
	while (1) {
		char line[1024];
		printf("> ");
		if (fgets(line, sizeof(line), stdin)) {
			jalv_process_command((Jalv*) arg, line);
		} else {
			break;
		}
	}
	return NULL;
}

pthread_t init_cli_thread(Jalv* jalv) {
	//Drop stderr output
	stderr = fopen("/dev/null", "w");

	pthread_t tid;
	int err=pthread_create(&tid, NULL, &cli_thread, jalv);
	if (err != 0) {
		printf("Can't create CLI thread :[%s]", strerror(err));
		return 0;
	} else {
		printf("CLI thread created successfully\n");
		return tid;
	}
}


LV2_RESTORE_WARNINGS
