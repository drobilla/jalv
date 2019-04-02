/*
  Copyright 2019 Timo Wischer <twischer@de.adit-jv.com>

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

#include "control_backend.h"

struct ControlPort {
	float value;
};

int jalv_control_backend_init(Jalv* const jalv)
{
	/* not used */
	jalv->control_backend = NULL;
	return 0;
}

int jalv_control_port_init(Jalv* const jalv, struct Port* const port)
{
	port->control = calloc(sizeof(struct ControlPort), 1);
	if (!port->control)
		return -1;

	port->control->value = 0.0f;

	return 0;
}

int jalv_control_port_destroy(Jalv* const jalv, struct Port* const port)
{
	free(port->control);

	return 0;
}

int jalv_control_backend_destroy(Jalv* const jalv)
{
	return 0;
}

int jalv_control_lock(Jalv* const jalv)
{
	return 0;
}

int jalv_control_before_run(Jalv* const jalv)
{
	return 0;
}

int jalv_control_after_run(Jalv* const jalv)
{
	return 0;
}

int jalv_control_unlock(Jalv* const jalv)
{
	return 0;
}

float* const jalv_control_data(Jalv* const jalv, struct Port* const port)
{
	return &port->control->value;
}

float jalv_control_get(Jalv* const jalv, struct Port* const port)
{
	return port->control->value;
}

int jalv_control_set(Jalv* const jalv, struct Port* const port, const float value)
{
	port->control->value = value;

	return 0;
}
