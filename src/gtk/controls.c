// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "controls.h"

#include "../control.h"
#include "../jalv.h"
#include "../types.h"
#include "jalv_gtk.h"

#include <float.h>
#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <zix/attributes.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void
set_control(Jalv* const    jalv,
            const Control* control,
            uint32_t       size,
            LV2_URID       type,
            const void*    body)
{
  if (!jalv->updating) {
    jalv_set_control(jalv, control, size, type, body);
  }
}

static bool
differ_enough(float a, float b)
{
  return fabsf(a - b) >= FLT_EPSILON;
}

static void
set_float_control(Jalv* const jalv, const Control* control, float value)
{
  const LV2_Atom_Forge* const forge = &jalv->forge;
  if (control->value_type == forge->Int) {
    const int32_t ival = lrintf(value);
    set_control(jalv, control, sizeof(ival), forge->Int, &ival);
  } else if (control->value_type == forge->Long) {
    const int64_t lval = lrintf(value);
    set_control(jalv, control, sizeof(lval), forge->Long, &lval);
  } else if (control->value_type == forge->Float) {
    set_control(jalv, control, sizeof(value), forge->Float, &value);
  } else if (control->value_type == forge->Double) {
    const double dval = value;
    set_control(jalv, control, sizeof(dval), forge->Double, &dval);
  } else if (control->value_type == forge->Bool) {
    const int32_t ival = value;
    set_control(jalv, control, sizeof(ival), forge->Bool, &ival);
  }

  Controller* controller = (Controller*)control->widget;
  if (controller && controller->spin &&
      differ_enough(gtk_spin_button_get_value(controller->spin), value)) {
    gtk_spin_button_set_value(controller->spin, value);
  }
}

// Widget changed callbacks

static gboolean
scale_changed(GtkRange* range, gpointer data)
{
  Jalv* const jalv = g_object_get_data(G_OBJECT(range), "jalv");

  set_float_control(jalv, (const Control*)data, gtk_range_get_value(range));
  return FALSE;
}

static gboolean
spin_changed(GtkSpinButton* spin, gpointer data)
{
  const Control* control    = (const Control*)data;
  Controller*    controller = (Controller*)control->widget;
  GtkRange*      range      = GTK_RANGE(controller->control);
  const double   value      = gtk_spin_button_get_value(spin);
  if (differ_enough(gtk_range_get_value(range), value)) {
    gtk_range_set_value(range, value);
  }
  return FALSE;
}

static gboolean
log_scale_changed(GtkRange* range, gpointer data)
{
  Jalv* const jalv = g_object_get_data(G_OBJECT(range), "jalv");

  set_float_control(
    jalv, (const Control*)data, expf(gtk_range_get_value(range)));
  return FALSE;
}

static gboolean
log_spin_changed(GtkSpinButton* spin, gpointer data)
{
  const Control* control    = (const Control*)data;
  Controller*    controller = (Controller*)control->widget;
  GtkRange*      range      = GTK_RANGE(controller->control);
  const double   value      = gtk_spin_button_get_value(spin);
  if (differ_enough(gtk_range_get_value(range), logf(value))) {
    gtk_range_set_value(range, logf(value));
  }
  return FALSE;
}

static void
combo_changed(GtkComboBox* box, gpointer data)
{
  Jalv* const    jalv    = g_object_get_data(G_OBJECT(box), "jalv");
  const Control* control = (const Control*)data;

  GtkTreeIter iter;
  if (gtk_combo_box_get_active_iter(box, &iter)) {
    GtkTreeModel* model = gtk_combo_box_get_model(box);
    GValue        value = G_VALUE_INIT;

    gtk_tree_model_get_value(model, &iter, 0, &value);
    const double v = g_value_get_float(&value);
    g_value_unset(&value);

    set_float_control(jalv, control, v);
  }
}

static gboolean
switch_changed(GtkSwitch* toggle_switch, gboolean state, gpointer data)
{
  Jalv* const jalv = g_object_get_data(G_OBJECT(toggle_switch), "jalv");

  set_float_control(jalv, (const Control*)data, state ? 1.0f : 0.0f);
  return FALSE;
}

static void
string_changed(GtkEntry* widget, gpointer data)
{
  Jalv* const    jalv    = g_object_get_data(G_OBJECT(widget), "jalv");
  const Control* control = (const Control*)data;
  const char*    string  = gtk_entry_get_text(widget);

  set_control(jalv, control, strlen(string) + 1, jalv->forge.String, string);
}

static void
file_changed(GtkFileChooserButton* widget, gpointer data)
{
  Jalv* const    jalv    = g_object_get_data(G_OBJECT(widget), "jalv");
  const Control* control = (const Control*)data;
  const char*    filename =
    gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));

  set_control(jalv, control, strlen(filename) + 1, jalv->forge.Path, filename);
}

// Controller construction

static Controller*
new_controller(GtkSpinButton* spin, GtkWidget* control)
{
  Controller* controller = (Controller*)malloc(sizeof(Controller));
  controller->spin       = spin;
  controller->control    = control;
  return controller;
}

static Controller*
make_combo(Control* record, float value)
{
  GtkListStore* list_store = gtk_list_store_new(2, G_TYPE_FLOAT, G_TYPE_STRING);
  int           active     = -1;
  for (size_t i = 0; i < record->n_points; ++i) {
    const ScalePoint* point = &record->points[i];
    GtkTreeIter       iter;
    gtk_list_store_append(list_store, &iter);
    gtk_list_store_set(list_store, &iter, 0, point->value, 1, point->label, -1);
    if (fabsf(value - point->value) < FLT_EPSILON) {
      active = i;
    }
  }

  GtkWidget* combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(list_store));
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
  g_object_unref(list_store);

  gtk_widget_set_sensitive(combo, record->is_writable);
  gtk_widget_set_halign(combo, GTK_ALIGN_START);
  gtk_widget_set_hexpand(combo, FALSE);

  GtkCellRenderer* cell = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), cell, TRUE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), cell, "text", 1, NULL);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(combo), "changed", G_CALLBACK(combo_changed), record);
  }

  return new_controller(NULL, combo);
}

static Controller*
make_log_slider(Control* record, float value)
{
  const float min  = record->min;
  const float max  = record->max;
  const float lmin = logf(min);
  const float lmax = logf(max);
  const float ldft = logf(value);

  GtkWidget* const scale =
    gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lmin, lmax, 0.001);

  GtkWidget* const spin = gtk_spin_button_new_with_range(min, max, 0.000001);

  gtk_widget_set_sensitive(scale, record->is_writable);
  gtk_widget_set_sensitive(spin, record->is_writable);

  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
  gtk_range_set_value(GTK_RANGE(scale), ldft);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(scale), "value-changed", G_CALLBACK(log_scale_changed), record);
    g_signal_connect(
      G_OBJECT(spin), "value-changed", G_CALLBACK(log_spin_changed), record);
  }

  return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_slider(Control* record, float value)
{
  const float  min  = record->min;
  const float  max  = record->max;
  const double step = record->is_integer ? 1.0 : ((max - min) / 100.0);

  GtkWidget* const scale =
    gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);

  GtkWidget* const spin = gtk_spin_button_new_with_range(min, max, step);

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
      gtk_scale_add_mark(GTK_SCALE(scale), point->value, GTK_POS_TOP, str);
      g_free(str);
    }
  }

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(scale), "value-changed", G_CALLBACK(scale_changed), record);
    g_signal_connect(
      G_OBJECT(spin), "value-changed", G_CALLBACK(spin_changed), record);
  }

  gtk_widget_set_halign(scale, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(scale, TRUE);
  return new_controller(GTK_SPIN_BUTTON(spin), scale);
}

static Controller*
make_toggle_switch(Control* record, float value)
{
  GtkWidget* toggle_switch = gtk_switch_new();
  gtk_widget_set_halign(toggle_switch, GTK_ALIGN_START);
  gtk_widget_set_hexpand(toggle_switch, FALSE);

  gtk_widget_set_sensitive(toggle_switch, record->is_writable);

  if (value) {
    gtk_switch_set_active(GTK_SWITCH(toggle_switch), TRUE);
  }

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(toggle_switch), "state-set", G_CALLBACK(switch_changed), record);
  }

  return new_controller(NULL, toggle_switch);
}

static Controller*
make_entry(Control* control)
{
  GtkWidget* entry = gtk_entry_new();

  gtk_widget_set_sensitive(entry, control->is_writable);
  if (control->is_writable) {
    g_signal_connect(
      G_OBJECT(entry), "activate", G_CALLBACK(string_changed), control);
  }

  return new_controller(NULL, entry);
}

static Controller*
make_file_chooser(Control* record)
{
  GtkWidget* button =
    gtk_file_chooser_button_new("Open File", GTK_FILE_CHOOSER_ACTION_OPEN);

  gtk_widget_set_sensitive(button, record->is_writable);

  if (record->is_writable) {
    g_signal_connect(
      G_OBJECT(button), "file-set", G_CALLBACK(file_changed), record);
  }

  return new_controller(NULL, button);
}

static Controller*
make_controller(Control* control, float value)
{
  Controller* controller = NULL;

  if (control->is_toggle) {
    controller = make_toggle_switch(control, value);
  } else if (control->is_enumeration) {
    controller = make_combo(control, value);
  } else if (control->is_logarithmic) {
    controller = make_log_slider(control, value);
  } else {
    controller = make_slider(control, value);
  }

  return controller;
}

// Top-level control widget (controls panel or just a close button)

static GtkWidget*
new_label(const char* text, bool title, GtkAlign xalign, GtkAlign yalign)
{
  GtkWidget* const label = gtk_label_new(NULL);
  gchar* const     str =
    title
          ? g_markup_printf_escaped("<span font_weight=\"bold\">%s</span>", text)
          : g_markup_printf_escaped("%s:", text);

  gtk_widget_set_halign(label, xalign);
  gtk_widget_set_valign(label, yalign);
  gtk_label_set_markup(GTK_LABEL(label), str);

  g_free(str);
  return label;
}

static void
add_control_row(GtkWidget*  grid,
                int         row,
                const char* name,
                Controller* controller)
{
  GtkWidget* label = new_label(name, false, GTK_ALIGN_END, GTK_ALIGN_BASELINE);
  gtk_widget_set_margin_end(label, 8);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  if (controller->spin) {
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(controller->spin), 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), controller->control, 2, row, 1, 1);
  } else {
    gtk_grid_attach(GTK_GRID(grid), controller->control, 1, row, 2, 1);
  }
}

static int
control_group_cmp(const void* p1, const void* p2, void* ZIX_UNUSED(data))
{
  const Control* control1 = *(const Control* const*)p1;
  const Control* control2 = *(const Control* const*)p2;

  const int cmp = (control1->group && control2->group)
                    ? strcmp(lilv_node_as_string(control1->group),
                             lilv_node_as_string(control2->group))
                    : ((intptr_t)control1->group - (intptr_t)control2->group);

  return cmp;
}

GtkWidget*
build_control_widget(Jalv* jalv, GtkWidget* window)
{
  GtkWidget* port_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(port_grid), 4);

  // Make an array of controls sorted by group
  GArray* controls = g_array_new(FALSE, TRUE, sizeof(Control*));
  for (unsigned i = 0; i < jalv->controls.n_controls; ++i) {
    g_array_append_vals(controls, &jalv->controls.controls[i], 1);
  }
  g_array_sort_with_data(controls, control_group_cmp, jalv);

  // Add controls in group order
  const LilvNode* last_group = NULL;
  int             n_rows     = 0;
  for (size_t i = 0; i < controls->len; ++i) {
    Control*    record     = g_array_index(controls, Control*, i);
    Controller* controller = NULL;
    LilvNode*   group      = record->group;

    // Check group and add new heading if necessary
    if (group && !lilv_node_equals(group, last_group)) {
      LilvNode* group_name =
        lilv_world_get(jalv->world, group, jalv->nodes.lv2_name, NULL);

      if (!group_name) {
        group_name =
          lilv_world_get(jalv->world, group, jalv->nodes.rdfs_label, NULL);
      }

      GtkWidget* group_label = new_label(lilv_node_as_string(group_name),
                                         true,
                                         GTK_ALIGN_START,
                                         GTK_ALIGN_BASELINE);

      lilv_node_free(group_name);
      gtk_grid_attach(GTK_GRID(port_grid), group_label, 0, n_rows, 3, 1);
      ++n_rows;
    }
    last_group = group;

    // Make control widget
    if (record->value_type == jalv->forge.String) {
      controller = make_entry(record);
    } else if (record->value_type == jalv->forge.Path) {
      controller = make_file_chooser(record);
    } else {
      controller = make_controller(record, record->def);
    }

    // Set jalv pointer as data for use in callbacks
    g_object_set_data(G_OBJECT(controller->control), "jalv", jalv);

    record->widget = controller;
    if (record->type == PORT) {
      jalv->ports[record->id.index].widget = controller;
    }
    if (controller) {
      // Add row to table for this controller
      add_control_row(port_grid,
                      n_rows++,
                      (record->label ? lilv_node_as_string(record->label)
                                     : lilv_node_as_uri(record->node)),
                      controller);

      // Set tooltip text from comment, if available
      LilvNode* comment = lilv_world_get(
        jalv->world, record->node, jalv->nodes.rdfs_comment, NULL);
      if (comment) {
        gtk_widget_set_tooltip_text(controller->control,
                                    lilv_node_as_string(comment));
      }
      lilv_node_free(comment);
    }
  }

  g_array_free(controls, true);

  if (n_rows > 0) {
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_widget_set_margin_start(port_grid, 8);
    gtk_widget_set_margin_end(port_grid, 8);
    gtk_widget_set_halign(port_grid, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(port_grid, TRUE);
    gtk_widget_set_valign(port_grid, GTK_ALIGN_START);
    gtk_widget_set_vexpand(port_grid, FALSE);
    return port_grid;
  }

  gtk_widget_destroy(port_grid);
  GtkWidget* button = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(
    button, "clicked", G_CALLBACK(gtk_widget_destroy), window);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  return button;
}
