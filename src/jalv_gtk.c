/*
  Copyright 2007-2015 David Robillard <http://drobilla.net>

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

#include <math.h>

#include <gtk/gtk.h>

#include "lv2/lv2plug.in/ns/ext/patch/patch.h"
#include "lv2/lv2plug.in/ns/ext/port-props/port-props.h"

#include "jalv_internal.h"

static GtkCheckMenuItem* active_preset_item = NULL;

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
on_window_destroy(GtkWidget* widget,
                  gpointer   data)
{
	Jalv* jalv = (Jalv*)data;
	suil_instance_free(jalv->ui_instance);
	jalv->ui_instance = NULL;
	gtk_main_quit();
}

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	GOptionEntry entries[] = {
		{ "uuid", 'u', 0, G_OPTION_ARG_STRING, &opts->uuid,
		  "UUID for Jack session restoration", "UUID" },
		{ "load", 'l', 0, G_OPTION_ARG_STRING, &opts->load,
		  "Load state from save directory", "DIR" },
		{ "preset", 'p', 0, G_OPTION_ARG_STRING, &opts->preset,
		  "Load state from preset", "URI" },
		{ "dump", 'd', 0, G_OPTION_ARG_NONE, &opts->dump,
		  "Dump plugin <=> UI communication", NULL },
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
		{ 0, 0, 0, 0, 0, 0, 0 } };
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
jalv_native_ui_type(Jalv* jalv)
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
on_save_activate(GtkWidget* widget, void* ptr)
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
on_quit_activate(GtkWidget* widget, gpointer data)
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
on_preset_destroy(gpointer data, GClosure* closure)
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
menu_cmp(gconstpointer a, gconstpointer b, gpointer data)
{
	return strcmp(((PresetMenu*)a)->label, ((PresetMenu*)b)->label);
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
	return g_sequence_get(i);
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
	                      0);

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
		gtk_container_remove(pset_menu, items->data);
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

	char* dot_lv2 = g_build_filename(g_get_home_dir(), ".lv2", NULL);
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
		const char* path   = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		const char* uri    = gtk_entry_get_text(GTK_ENTRY(uri_entry));
		const char* prefix = "";
		const char* sep    = "";
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_prefix))) {
			LilvNode* plug_name = lilv_plugin_get_name(jalv->plugin);
			prefix = lilv_node_as_string(plug_name);
			sep    = "_";
			lilv_node_free(plug_name);
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
		free(sym);
		g_free(basename);
		g_free(dirname);
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
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
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
	Jalv* jalv = control->jalv;
	if (control->type == PORT && type == jalv->forge.Float) {
		struct Port* port = &control->jalv->ports[control->index];
		port->control = *(float*)body;
	} else if (control->type == PROPERTY) {
		// Copy forge since it is used by process thread
		LV2_Atom_Forge       forge = jalv->forge;
		LV2_Atom_Forge_Frame frame;
		uint8_t              buf[1024];
		lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

		lv2_atom_forge_object(&forge, &frame, 0, jalv->urids.patch_Set);
		lv2_atom_forge_key(&forge, jalv->urids.patch_property);
		lv2_atom_forge_urid(&forge, control->property);
		lv2_atom_forge_key(&forge, jalv->urids.patch_value);
		lv2_atom_forge_atom(&forge, size, type);
		lv2_atom_forge_write(&forge, body, size);

		const LV2_Atom* atom = lv2_atom_forge_deref(&forge, frame.ref);
		jalv_ui_write(jalv,
		              jalv->control_in,
		              lv2_atom_total_size(atom),
		              jalv->urids.atom_eventTransfer,
		              atom);
	}
}

static void
set_float_control(const ControlID* control, float value)
{
	if (control->value_type == control->jalv->forge.Int) {
		const int32_t ival = lrint(value);
		set_control(control, sizeof(ival), control->jalv->forge.Int, &ival);
	} else if (control->value_type == control->jalv->forge.Long) {
		const int64_t lval = lrint(value);
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
	    gtk_spin_button_get_value(controller->spin) != value) {
		gtk_spin_button_set_value(controller->spin, value);
	}
}

static double
get_atom_double(Jalv* jalv, uint32_t size, LV2_URID type, const void* body)
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
		// Numeric control
		if (controller->spin) {
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(controller->spin),
			                          fvalue);
		}

		if (GTK_IS_COMBO_BOX(widget)) {
			GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
			GValue        value = { 0, { { 0 } } };
			GtkTreeIter   i;
			bool          valid = gtk_tree_model_get_iter_first(model, &i);
			while (valid) {
				gtk_tree_model_get_value(model, &i, 0, &value);
				const double v = g_value_get_double(&value);
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
	if (protocol == 0) {
		control_changed(jalv,
		                jalv->ports[port_index].widget,
		                buffer_size,
		                jalv->forge.Float,
		                buffer);
		return;
	} else if (protocol != jalv->urids.atom_eventTransfer) {
		fprintf(stderr, "Unknown port event protocol\n");
		return;
	}

	const LV2_Atom* atom = (const LV2_Atom*)buffer;
	if (lv2_atom_forge_is_object_type(&jalv->forge, atom->type)) {
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
	const double     value      = gtk_spin_button_get_value(spin);
	set_float_control(control, value);
	gtk_range_set_value(GTK_RANGE(controller->control), value);
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
	const double     value      = gtk_spin_button_get_value(spin);
	set_float_control(control, value);
	gtk_range_set_value(GTK_RANGE(controller->control), logf(value));
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
		const double v = g_value_get_double(&value);
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

	set_control(control, strlen(filename), jalv->forge.Path, filename);
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
		if (fabs(value - point->value) < FLT_EPSILON) {
			active = i;
		}
	}

	GtkWidget* combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(list_store));
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
	g_object_unref(list_store);

	GtkCellRenderer* cell = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cell, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cell, "text", 1, NULL);

	g_signal_connect(G_OBJECT(combo), "changed",
	                 G_CALLBACK(combo_changed), record);

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

	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_range_set_value(GTK_RANGE(scale), ldft);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);

	g_signal_connect(G_OBJECT(scale), "value-changed",
	                 G_CALLBACK(log_scale_changed), record);
	g_signal_connect(G_OBJECT(spin), "value-changed",
	                 G_CALLBACK(log_spin_changed), record);

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

	g_signal_connect(G_OBJECT(scale), "value-changed",
	                 G_CALLBACK(scale_changed), record);
	g_signal_connect(G_OBJECT(spin), "value-changed",
	                 G_CALLBACK(spin_changed), record);

	return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_toggle(ControlID* record, float value)
{
	GtkWidget* check = gtk_check_button_new();
	if (value) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
	}
	g_signal_connect(G_OBJECT(check), "toggled",
	                 G_CALLBACK(toggle_changed), record);
	return new_controller(NULL, check);
}

static Controller*
make_entry(ControlID* control)
{
	GtkWidget* entry = gtk_entry_new();
	g_signal_connect(G_OBJECT(entry), "activate",
	                 G_CALLBACK(string_changed), control);
	return new_controller(NULL, entry);
}

static Controller*
make_file_chooser(ControlID* record)
{
	GtkWidget* button = gtk_file_chooser_button_new(
		"Open File", GTK_FILE_CHOOSER_ACTION_OPEN);
	g_signal_connect(G_OBJECT(button), "file-set",
	                 G_CALLBACK(file_changed), record);
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
	                 GTK_FILL, GTK_FILL | GTK_EXPAND, 8, 1);
	int control_left_attach = 1;
	if (controller->spin) {
		control_left_attach = 2;
		gtk_table_attach(GTK_TABLE(table), GTK_WIDGET(controller->spin),
		                 1, 2, row, row + 1,
		                 GTK_FILL, GTK_FILL, 2, 1);
	}
	gtk_table_attach(GTK_TABLE(table), controller->control,
	                 control_left_attach, 3, row, row + 1,
	                 GTK_FILL | GTK_EXPAND, GTK_FILL, 2, 1);
}

static int
port_group_cmp(const void* p1, const void* p2, void* data)
{
	Jalv*           jalv  = (Jalv*)data;
	const LilvPort* port1 = *(const LilvPort**)p1;
	const LilvPort* port2 = *(const LilvPort**)p2;

	LilvNode* group1 = lilv_port_get(
		jalv->plugin, port1, jalv->nodes.pg_group);
	LilvNode* group2 = lilv_port_get(
		jalv->plugin, port2, jalv->nodes.pg_group);

	const int cmp = (group1 && group2)
		? strcmp(lilv_node_as_string(group1), lilv_node_as_string(group2))
		: ((intptr_t)group1 - (intptr_t)group2);

	lilv_node_free(group2);
	lilv_node_free(group1);

	return cmp;
}

static GtkWidget*
build_control_widget(Jalv* jalv, GtkWidget* window)
{
	const LilvPlugin* plugin = jalv->plugin;
	LilvWorld*        world  = jalv->world;

	LilvNode*   patch_writable  = lilv_new_uri(world, LV2_PATCH__writable);
	LilvNode*   pprop_notOnGUI  = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);
	GtkWidget*  port_table      = gtk_table_new(jalv->num_ports, 3, false);

	/* Make an array of control port pointers and sort it by group */
	GArray* control_ports = g_array_new(FALSE, TRUE, sizeof(LilvPort*));
	for (unsigned i = 0; i < jalv->num_ports; ++i) {
		if (jalv->ports[i].type == TYPE_CONTROL) {
			g_array_append_vals(control_ports, &jalv->ports[i].lilv_port, 1);
		}
	}
	g_array_sort_with_data(control_ports, port_group_cmp, jalv);

	/* Iterate over control ports ordered by group */
	LilvNode* last_group = NULL;
	int       n_rows     = 0;
	for (unsigned i = 0; i < control_ports->len; ++i) {
		const LilvPort* port = g_array_index(control_ports, LilvPort*, i);
		if (!jalv->opts.show_hidden &&
		    lilv_port_has_property(plugin, port, pprop_notOnGUI)) {
			continue;
		}

		uint32_t  index = lilv_port_get_index(plugin, port);
		LilvNode* name  = lilv_port_get_name(plugin, port);
		LilvNode* group = lilv_port_get(plugin, port, jalv->nodes.pg_group);
		if (group && !lilv_node_equals(group, last_group)) {
			/* Group has changed, add a heading row here */
			LilvNode* group_name = lilv_world_get(
				world, group, jalv->nodes.lv2_name, NULL);
			GtkWidget* group_label = new_label(
				lilv_node_as_string(group_name), true, 0.0f, 1.0f);
			gtk_table_attach(GTK_TABLE(port_table), group_label,
			                 0, 2, n_rows, n_rows + 1,
			                 GTK_FILL, GTK_FILL, 0, 6);
			++n_rows;
		}
		last_group = group;

		ControlID* control_id = new_port_control(jalv, index);

		/* Get scale points */
		LilvScalePoints* sp = lilv_port_get_scale_points(plugin, port);
		if (sp) {
			control_id->points = (ScalePoint*)malloc(
				lilv_scale_points_size(sp) * sizeof(ScalePoint));
			size_t np = 0;
			LILV_FOREACH(scale_points, s, sp) {
				const LilvScalePoint* p = lilv_scale_points_get(sp, s);
				if (lilv_node_is_float(lilv_scale_point_get_value(p)) ||
				    lilv_node_is_int(lilv_scale_point_get_value(p))) {
					control_id->points[np].value = lilv_node_as_float(
						lilv_scale_point_get_value(p));
					control_id->points[np].label = g_strdup(
						lilv_node_as_string(lilv_scale_point_get_label(p)));
					++np;
				}
				/* TODO: Non-float scale points? */
			}

			qsort(control_id->points, np, sizeof(ScalePoint),
			      (int (*)(const void*, const void*))scale_point_cmp);
			control_id->n_points = np;

			lilv_scale_points_free(sp);
		}

		/* Make controller */
		struct Port* jport      = &jalv->ports[index];
		Controller*  controller = make_controller(control_id, jport->control);
		control_id->widget = controller;
		jport->widget      = controller;

		/* Set tooltip text from comment, if available */
		LilvNode* comment = lilv_port_get(plugin, port, jalv->nodes.rdfs_comment);
		if (comment) {
			gtk_widget_set_tooltip_text(controller->control,
			                            lilv_node_as_string(comment));
		}
		lilv_node_free(comment);

		add_control(&jalv->controls, control_id);
		add_control_row(
			port_table, n_rows++, lilv_node_as_string(name), controller);

		lilv_node_free(name);
	}

	/* Add controllers for writable properties (event-based controls) */
	LilvNodes* properties = lilv_world_find_nodes(
		world,
		lilv_plugin_get_uri(plugin),
		patch_writable,
		NULL);
	LILV_FOREACH(nodes, p, properties) {
		const LilvNode* property = lilv_nodes_get(properties, p);
		LilvNode*       label    = lilv_nodes_get_first(
			lilv_world_find_nodes(
				world, property, jalv->nodes.rdfs_label, NULL));

		Controller* controller = NULL;
		ControlID*  record     = new_property_control(jalv, property);
		if (!record->value_type) {
			fprintf(stderr, "Unknown property range, no control shown\n");
		} else if (record->value_type == jalv->forge.String) {
			controller = make_entry(record);
		} else if (record->value_type == jalv->forge.Path) {
			controller = make_file_chooser(record);
		} else {
			controller = make_controller(record, get_float(record->def, 0.0f));
		}

		record->widget = controller;
		if (controller) {
			add_control(&jalv->controls, record);
			add_control_row(
				port_table, n_rows++,
				label ? lilv_node_as_string(label) : lilv_node_as_uri(property),
				controller);
		}
	}
	lilv_nodes_free(properties);

	lilv_node_free(patch_writable);
	lilv_node_free(pprop_notOnGUI);

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

int
jalv_open_ui(Jalv* jalv)
{
	GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	jalv->window = window;
	jalv->has_ui = TRUE;

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
		jalv_ui_instantiate(jalv, jalv_native_ui_type(jalv), alignment);
	}

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

		GtkRequisition controls_size, box_size;
		size_request(GTK_WIDGET(controls), &controls_size);
		size_request(GTK_WIDGET(vbox), &box_size);

		gtk_window_set_default_size(
			GTK_WINDOW(window),
			MAX(MAX(box_size.width, controls_size.width) + 24, 640),
			box_size.height + controls_size.height);
	}

	g_timeout_add(1000 / jalv->ui_update_hz, (GSourceFunc)jalv_update, jalv);

	gtk_window_present(GTK_WINDOW(window));

	gtk_main();
	zix_sem_post(jalv->done);
	return 0;
}

int
jalv_close_ui(Jalv* jalv)
{
	gtk_main_quit();
	return 0;
}
