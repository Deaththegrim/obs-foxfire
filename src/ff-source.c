/*
Foxfire
Copyright (C) 2026 KitsuneStudio ninjaflashboy@gmail.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "ff-props.h"
#include <plugin-support.h>

static const char *src_name(void *d)
{
	UNUSED_PARAMETER(d);
	return obs_module_text("Foxfire.Visualizer");
}

static void *src_create(obs_data_t *s, obs_source_t *self)
{
	return ff_instance_create(s, self, false);
}

static void src_destroy(void *d)
{
	ff_instance_destroy(d);
}

static void src_update(void *d, obs_data_t *s)
{
	ff_instance_update(d, s);
}

static void src_defaults(obs_data_t *s)
{
	ff_instance_defaults(s, false);
}

static obs_properties_t *src_props(void *d)
{
	return ff_instance_properties(d);
}

static uint32_t src_w(void *d)
{
	return ((struct ff_instance *)d)->width;
}

static uint32_t src_h(void *d)
{
	return ((struct ff_instance *)d)->height;
}

/* video_render gets no frame time of its own; the tick that precedes it on the same thread does. */
static float g_dt;

static void src_tick(void *d, float seconds)
{
	UNUSED_PARAMETER(d);
	g_dt = seconds;
}

static void src_render(void *d, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ff_instance *in = d;
	gs_texture_t *tex = ff_instance_render(in, NULL, in->width, in->height, g_dt);
	if (!tex)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
	gs_effect_set_texture(img, tex);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); /* the layer stack writes premultiplied alpha */
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, in->width, in->height);
	gs_blend_state_pop();
}

struct obs_source_info ff_source_info = {
	.id = "foxfire_visualizer",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = src_name,
	.create = src_create,
	.destroy = src_destroy,
	.update = src_update,
	.get_defaults = src_defaults,
	.get_properties = src_props,
	.get_width = src_w,
	.get_height = src_h,
	.video_tick = src_tick,
	.video_render = src_render,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
