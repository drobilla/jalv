/*
  Copyright 2007-2012 David Robillard <http://drobilla.net>

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

typedef struct {
	Jalv*     jalv;
	LilvNode* property;
} PropertyRecord;

typedef struct {
	GtkSpinButton* spin;
	GtkWidget*     control;
} Controller;

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
		{ "dump", 'd', 0, G_OPTION_ARG_NONE, &opts->dump,
		  "Dump plugin <=> UI communication", NULL },
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
		  "UI update frequency", NULL},
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
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
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
on_preset_activate(GtkWidget* widget, gpointer data)
{
	PresetRecord* record = (PresetRecord*)data;
	jalv_apply_preset(record->jalv, record->preset);
}

static void
on_preset_destroy(gpointer data, GClosure* closure)
{
	PresetRecord* record = (PresetRecord*)data;
	lilv_node_free(record->preset);
	free(record);
}

static int
add_preset_to_menu(Jalv*           jalv,
                   const LilvNode* node,
                   const LilvNode* title,
                   void*           data)
{
	GtkWidget*  presets_menu = GTK_WIDGET(data);
	const char* label        = lilv_node_as_string(title);
	GtkWidget*  item         = gtk_menu_item_new_with_label(label);

	PresetRecord* record = (PresetRecord*)malloc(sizeof(PresetRecord));
	record->jalv   = jalv;
	record->preset = lilv_node_duplicate(node);

	g_signal_connect_data(G_OBJECT(item), "activate",
	                      G_CALLBACK(on_preset_activate),
	                      record, on_preset_destroy,
	                      0);

	gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu), item);
	return 0;
}

static void
on_save_preset_activate(GtkWidget* widget, void* ptr)
{
	Jalv* jalv = (Jalv*)ptr;

	GtkWidget* dialog = gtk_file_chooser_dialog_new(
		"Save Preset",
		(GtkWindow*)jalv->window,
		GTK_FILE_CHOOSER_ACTION_SAVE,
		GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
		GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
		NULL);

	char* dot_lv2 = g_build_filename(g_get_home_dir(), ".lv2", NULL);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dot_lv2);
	free(dot_lv2);

	GtkWidget* content   = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	GtkBox*    box       = GTK_BOX(new_box(true, 8));
	GtkWidget* uri_label = gtk_label_new("URI (Optional):");
	GtkWidget* uri_entry = gtk_entry_new();

	gtk_box_pack_start(box, uri_label, FALSE, TRUE, 2);
	gtk_box_pack_start(box, uri_entry, TRUE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(box), FALSE, FALSE, 6);

	gtk_widget_show_all(GTK_WIDGET(dialog));
	gtk_entry_set_activates_default(GTK_ENTRY(uri_entry), TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		const char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		const char* uri  = gtk_entry_get_text(GTK_ENTRY(uri_entry));

		char* dirname  = g_path_get_dirname(path);
		char* basename = g_path_get_basename(path);
		char* sym      = symbolify(basename);
		char* bundle   = g_strjoin(NULL, sym, ".lv2/", NULL);
		char* file     = g_strjoin(NULL, sym, ".ttl", NULL);
		char* dir      = g_build_filename(dirname, bundle, NULL);

		jalv_save_preset(jalv, dir, (strlen(uri) ? uri : NULL), basename, file);

		// Load preset so it is now known to LilvWorld
		SerdNode  sdir = serd_node_new_file_uri((const uint8_t*)dir, 0, 0, 0);
		LilvNode* ldir = lilv_new_uri(jalv->world, (const char*)sdir.buf);
		lilv_world_load_bundle(jalv->world, ldir);
		serd_node_free(&sdir);
		lilv_node_free(ldir);

		// Rebuild preset menu
		GtkContainer* pset_menu = GTK_CONTAINER(gtk_widget_get_parent(widget));
		GList*        items     = gtk_container_get_children(pset_menu);
		for (items = items->next; items; items = items->next) {
			gtk_container_remove(pset_menu, items->data);
		}
		jalv_load_presets(jalv, add_preset_to_menu, pset_menu);
		gtk_widget_show_all(GTK_WIDGET(pset_menu));

		g_free(dir);
		g_free(file);
		g_free(bundle);
		free(sym);
		g_free(basename);
		g_free(dirname);
	}

	gtk_widget_destroy(GTK_WIDGET(dialog));
}

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer)
{
	Controller* controller = (Controller*)jalv->ports[port_index].widget;
	if (!controller) {
		return;
	}

	if (controller->spin) {
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(controller->spin),
		                          *(const float*)buffer);
	}

	GtkWidget* widget = controller->control;
	if (GTK_IS_COMBO_BOX(widget)) {
		GtkTreeModel* model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
		GValue        value = { 0, { { 0 } } };
		GtkTreeIter   i;
		bool          valid = gtk_tree_model_get_iter_first(model, &i);
		while (valid) {
			gtk_tree_model_get_value(model, &i, 0, &value);
			const double v = g_value_get_double(&value);
			g_value_unset(&value);
			if (fabs(v - *(const float*)buffer) < FLT_EPSILON) {
				gtk_combo_box_set_active_iter(GTK_COMBO_BOX(widget), &i);
				return;
			}
			valid = gtk_tree_model_iter_next(model, &i);
		}
	} else if (GTK_IS_TOGGLE_BUTTON(widget)) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget),
		                             *(const float*)buffer > 0.0f);
	} else if (GTK_IS_RANGE(widget)) {
		gtk_range_set_value(GTK_RANGE(widget), *(const float*)buffer); 
	} else {
		fprintf(stderr, "Unknown widget type for port %d\n", port_index);
	}
}

static gboolean
scale_changed(GtkRange* range, gpointer data)
{
	struct Port* port = (struct Port*)data;
	port->control = gtk_range_get_value(range);
	gtk_spin_button_set_value(
		GTK_SPIN_BUTTON(((Controller*)port->widget)->spin), port->control);
	return FALSE;
}

static gboolean
spin_changed(GtkSpinButton* spin, gpointer data)
{
	struct Port* port = (struct Port*)data;
	port->control = gtk_spin_button_get_value(spin);
	gtk_range_set_value(
		GTK_RANGE(((Controller*)port->widget)->control), port->control);
	return FALSE;
}

static gboolean
log_scale_changed(GtkRange* range, gpointer data)
{
	struct Port* port = (struct Port*)data;
	port->control = expf(gtk_range_get_value(range));
	gtk_spin_button_set_value(
		GTK_SPIN_BUTTON(((Controller*)port->widget)->spin), port->control);
	return FALSE;
}

static gboolean
log_spin_changed(GtkSpinButton* spin, gpointer data)
{
	struct Port* port = (struct Port*)data;
	port->control = gtk_spin_button_get_value(spin);
	gtk_range_set_value(
		GTK_RANGE(((Controller*)port->widget)->control), logf(port->control));
	return FALSE;
}

static void
combo_changed(GtkComboBox* box, gpointer data)
{
	GtkTreeIter iter;
	if (gtk_combo_box_get_active_iter(box, &iter)) {
		GtkTreeModel* model = gtk_combo_box_get_model(box);
		GValue        value = { 0, { { 0 } } };

		gtk_tree_model_get_value(model, &iter, 0, &value);
		const double v = g_value_get_double(&value);
		g_value_unset(&value);

		((struct Port*)data)->control = v;
	}
}

static gboolean
toggle_changed(GtkToggleButton* button, gpointer data)
{
	float fval = gtk_toggle_button_get_active(button) ? 1.0f : 0.0f;
	((struct Port*)data)->control = fval;
	return FALSE;
}

static void
file_changed(GtkFileChooserButton* widget,
             gpointer              data)
{
	PropertyRecord* record   = (PropertyRecord*)data;
	Jalv*           jalv     = record->jalv;
	const char*     property = lilv_node_as_uri(record->property);
	const char*     filename = gtk_file_chooser_get_filename(
		GTK_FILE_CHOOSER(widget));

	// Copy forge since it is used by process thread
	LV2_Atom_Forge       forge = jalv->forge;
	LV2_Atom_Forge_Frame frame;
	uint8_t              buf[1024];
	lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

	lv2_atom_forge_blank(&forge, &frame, 1, jalv->urids.patch_Set);
	lv2_atom_forge_property_head(&forge, jalv->urids.patch_property, 0);
	lv2_atom_forge_urid(&forge, jalv->map.map(jalv, property));
	lv2_atom_forge_property_head(&forge, jalv->urids.patch_value, 0);
	lv2_atom_forge_path(&forge, filename, strlen(filename));

	const LV2_Atom* atom = lv2_atom_forge_deref(&forge, frame.ref);
	jalv_ui_write(jalv,
	              jalv->control_in,
	              lv2_atom_total_size(atom),
	              jalv->urids.atom_eventTransfer,
	              atom);
}

static gint
dcmp(gconstpointer a, gconstpointer b)
{
	double y = *(const double*)a;
	double z = *(const double*)b;
	return y < z ? -1 : z < y ? 1 : 0;
}

static gint
drcmp(gconstpointer a, gconstpointer b)
{
	double y = *(const double*)a;
	double z = *(const double*)b;
	return y < z ? 1 : z < y ? -1 : 0;
}

static Controller*
make_controller(GtkSpinButton* spin, GtkWidget* control)
{
	Controller* controller = (Controller*)malloc(sizeof(Controller));
	controller->spin    = spin;
	controller->control = control;
	return controller;
}

static Controller*
make_combo(struct Port* port, GHashTable* points)
{
	GList*        list       = g_hash_table_get_keys(points);
	GtkListStore* list_store = gtk_list_store_new(
		2, G_TYPE_DOUBLE, G_TYPE_STRING);
	gint active = -1, count = 0;
	for (GList* cur = g_list_sort(list, dcmp); cur; cur = cur->next, ++count) {
		GtkTreeIter iter;
		gtk_list_store_append(list_store, &iter);
		gtk_list_store_set(list_store, &iter,
		                   0, *(double*)cur->data,
		                   1, g_hash_table_lookup(points, cur->data),
		                   -1);
		if (fabs(port->control - *(double*)cur->data) < FLT_EPSILON) {
			active = count;
		}
	}
	g_list_free(list);

	GtkWidget* combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(list_store));
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
	g_object_unref(list_store);

	GtkCellRenderer* cell = gtk_cell_renderer_text_new();
	gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cell, TRUE);
	gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cell, "text", 1, NULL);

	g_signal_connect(G_OBJECT(combo),
	                 "changed", G_CALLBACK(combo_changed), port);

	return make_controller(NULL, combo);
}

static void
add_mark(gdouble key, const gchar* value, void* scale)
{
	gchar* str = g_markup_printf_escaped("<span font_size=\"small\">%s</span>",
	                                     value);
	gtk_scale_add_mark(GTK_SCALE(scale), key, GTK_POS_TOP, str);
}

static Controller*
make_log_slider(struct Port* port, GHashTable* points, float min, float max)
{
	float      lmin  = logf(min);
	float      lmax  = logf(max);
	float      ldft  = logf(port->control);
	GtkWidget* scale = new_hscale(lmin, lmax, 0.001);
	GtkWidget* spin  = gtk_spin_button_new_with_range(min, max, 0.000001);

	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_range_set_value(GTK_RANGE(scale), ldft);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), port->control);

	g_signal_connect(
		G_OBJECT(scale), "value-changed", G_CALLBACK(log_scale_changed), port);
	g_signal_connect(
		G_OBJECT(spin), "value-changed", G_CALLBACK(log_spin_changed), port);

	return make_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_slider(struct Port* port, GHashTable* points,
            bool is_int, float min, float max)
{
	const double step  = is_int ? 1.0 : ((max - min) / 100.0);
	GtkWidget*   scale = new_hscale(min, max, step);
	GtkWidget*   spin  = gtk_spin_button_new_with_range(min, max, 0.000001);

	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_range_set_value(GTK_RANGE(scale), port->control);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), port->control);
	if (points) {
		GList* list = g_hash_table_get_keys(points);
		for (GList* cur = g_list_sort(list, drcmp); cur; cur = cur->next) {
			add_mark(*(gdouble*)cur->data,
			         g_hash_table_lookup(points, cur->data),
			         scale);
		}
	}

	g_signal_connect(
		G_OBJECT(scale), "value-changed", G_CALLBACK(scale_changed), port);
	g_signal_connect(
		G_OBJECT(spin), "value-changed", G_CALLBACK(spin_changed), port);

	return make_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_toggle(struct Port* port)
{
	GtkWidget* check = gtk_check_button_new();
	if (port->control) {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
	}
	g_signal_connect(G_OBJECT(check),
	                 "toggled", G_CALLBACK(toggle_changed), port);
	return make_controller(NULL, check);
}

static Controller*
make_file_chooser(Jalv* jalv, const LilvNode* property)
{
	GtkWidget* button = gtk_file_chooser_button_new(
		lilv_node_as_uri(property), GTK_FILE_CHOOSER_ACTION_OPEN);
	PropertyRecord* record = (PropertyRecord*)malloc(sizeof(PropertyRecord));
	record->jalv     = jalv;
	record->property = lilv_node_duplicate(property);
	g_signal_connect(
		G_OBJECT(button), "file-set", G_CALLBACK(file_changed), record);
	return make_controller(NULL, button);
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
	Jalv*              jalv  = (Jalv*)data;
	const struct Port* port1 = (const struct Port*)p1;
	const struct Port* port2 = (const struct Port*)p2;

	LilvNode* group1 = lilv_port_get(
		jalv->plugin, port1->lilv_port, jalv->nodes.pg_group);
	LilvNode* group2 = lilv_port_get(
		jalv->plugin, port2->lilv_port, jalv->nodes.pg_group);

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

	LilvNode*   lv2_enumeration = lilv_new_uri(world, LV2_CORE__enumeration);
	LilvNode*   lv2_integer     = lilv_new_uri(world, LV2_CORE__integer);
	LilvNode*   logarithmic     = lilv_new_uri(world, LV2_PORT_PROPS__logarithmic);
	LilvNode*   lv2_sampleRate  = lilv_new_uri(world, LV2_CORE__sampleRate);
	LilvNode*   lv2_toggled     = lilv_new_uri(world, LV2_CORE__toggled);
	LilvNode*   patch_writable  = lilv_new_uri(world, LV2_PATCH__writable);
	LilvNode*   rdfs_comment    = lilv_new_uri(world, LILV_NS_RDFS "comment");
	LilvNode*   pprop_notOnGUI  = lilv_new_uri(world, LV2_PORT_PROPS__notOnGUI);
	GtkWidget*  port_table      = gtk_table_new(jalv->num_ports, 3, false);

	/* Get the min and max of all ports (or NaN if unavailable) */
	float* mins = (float*)calloc(jalv->num_ports, sizeof(float));
	float* maxs = (float*)calloc(jalv->num_ports, sizeof(float));
	lilv_plugin_get_port_ranges_float(plugin, mins, maxs, NULL);

	/* Make an array of control port pointers and sort it by group */
	GArray* control_ports = g_array_new(FALSE, TRUE, sizeof(struct Port*));
	for (unsigned i = 0; i < jalv->num_ports; ++i) {
		if (jalv->ports[i].type == TYPE_CONTROL) {
			g_array_append_vals(control_ports, &jalv->ports[i], 1);
		}
	}
	g_array_sort_with_data(control_ports, port_group_cmp, jalv);

	/* Iterate over control ports ordered by group */
	LilvNode* last_group = NULL;
	int       n_rows     = 0;
	for (unsigned i = 0; i < control_ports->len; ++i) {
		const LilvPort* port  = g_array_index(control_ports, LilvPort*, i);

		if (!jalv->opts.show_hidden &&
		    lilv_port_has_property(plugin, port, pprop_notOnGUI)) {
			continue;
		}

		uint32_t        index = lilv_port_get_index(plugin, port);
		LilvNode*       name  = lilv_port_get_name(plugin, port);

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

		if (lilv_port_has_property(plugin, port, lv2_sampleRate)) {
			/* Adjust range for lv2:sampleRate controls */
			mins[index] *= jalv->sample_rate;
			maxs[index] *= jalv->sample_rate;
		}

		/* Get scale points */
		LilvScalePoints* sp     = lilv_port_get_scale_points(plugin, port);
		GHashTable*      points = NULL;
		if (sp) {
			points = g_hash_table_new(g_double_hash, g_double_equal);
			int      idx    = 0;
			gdouble* values = (gdouble*)malloc(
				lilv_scale_points_size(sp) * sizeof(gdouble));
			LILV_FOREACH(scale_points, s, sp) {
				const LilvScalePoint* p = lilv_scale_points_get(sp, s);
				values[idx] = lilv_node_as_float(lilv_scale_point_get_value(p));
				char* label = g_strdup(
					lilv_node_as_string(lilv_scale_point_get_label(p)));
				g_hash_table_insert(points, values + idx, label);
				++idx;
			}
			lilv_scale_points_free(sp);
		}

		/* Make controller */
		Controller* control = NULL;
		if (lilv_port_has_property(plugin, port, lv2_toggled)) {
			control = make_toggle(&jalv->ports[index]);
		} else if (lilv_port_has_property(plugin, port, lv2_enumeration)) {
			control = make_combo(&jalv->ports[index], points);
		} else if (lilv_port_has_property(plugin, port, logarithmic)) {
			control = make_log_slider(
				&jalv->ports[index], points, mins[index], maxs[index]);
		} else {
			bool is_int = lilv_port_has_property(plugin, port, lv2_integer);
			control = make_slider(
				&jalv->ports[index], points, is_int, mins[index], maxs[index]);
		}
		jalv->ports[index].widget = control;

		/* Set tooltip text from comment, if available */
		LilvNode* comment = lilv_port_get(plugin, port, rdfs_comment);
		if (comment) {
			gtk_widget_set_tooltip_text(control->control,
			                            lilv_node_as_string(comment));
		}
		lilv_node_free(comment);

		add_control_row(
			port_table, n_rows++, lilv_node_as_string(name), control);

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

		Controller* controller = make_file_chooser(jalv, property);
		add_control_row(
			port_table, n_rows++,
			label ? lilv_node_as_string(label) : lilv_node_as_uri(property),
			controller);
	}
	lilv_nodes_free(properties);

	free(mins);
	free(maxs);
	lilv_node_free(rdfs_comment);
	lilv_node_free(patch_writable);
	lilv_node_free(lv2_toggled);
	lilv_node_free(lv2_sampleRate);
	lilv_node_free(lv2_integer);
	lilv_node_free(lv2_enumeration);
	lilv_node_free(pprop_notOnGUI);
	lilv_node_free(logarithmic);

	if (n_rows > 0) {
		gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
		GtkWidget* alignment = gtk_alignment_new(0.5, 0.0, 1.0, 0.0);
		gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 0, 0, 8, 8);
		gtk_container_add(GTK_CONTAINER(alignment), port_table);
		return alignment;
	} else {
		gtk_widget_destroy(port_table);
		GtkWidget* button = gtk_button_new_with_label("Close");
		g_signal_connect_swapped(
			button, "clicked", G_CALLBACK(gtk_widget_destroy), window);
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

	GtkWidget* presets      = gtk_menu_item_new_with_mnemonic("_Presets");
	GtkWidget* presets_menu = gtk_menu_new();
	GtkWidget* save_preset  = gtk_menu_item_new_with_mnemonic(
		"_Save Preset...");
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(presets), presets_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu), save_preset);
	gtk_menu_shell_append(GTK_MENU_SHELL(presets_menu),
	                      gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), presets);

	jalv_load_presets(jalv, add_preset_to_menu, presets_menu);

	g_signal_connect(G_OBJECT(quit), "activate",
	                 G_CALLBACK(on_quit_activate), window);

	g_signal_connect(G_OBJECT(save), "activate",
	                 G_CALLBACK(on_save_activate), jalv);

	g_signal_connect(G_OBJECT(save_preset), "activate",
	                 G_CALLBACK(on_save_preset_activate), jalv);

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

	LilvNode* name = lilv_plugin_get_name(jalv->plugin);
	gtk_window_set_title(GTK_WINDOW(window), lilv_node_as_string(name));
	lilv_node_free(name);

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

	g_timeout_add(1000 / jalv->ui_update_hz,
	              (GSourceFunc)jalv_emit_ui_events, jalv);

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
