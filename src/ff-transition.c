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

/* The scene transition: a pack's layer stack, given the two scenes and where it is between them.
 *
 * It is the same renderer the visualizer and the filter use -- `tex_a` and `tex_b` are two more
 * builtins and `progress` already existed for the alert cards -- so a transition preset is an
 * ordinary pack preset with kind "transition" and gets the placement controls, the gradient
 * controls and the licence gate for free. Deliberately NOT a second renderer: a hand-rolled
 * variant of the main path silently drops whatever it forgot to copy.
 *
 * Two things a transition must do that no other Foxfire source does:
 *
 *   AUDIO. libobs will not mix the outgoing and incoming scenes for us. A transition that
 *   implements video and forgets audio does not fall back to a cut -- the scene goes SILENT for
 *   the whole duration, which is far worse than an ugly wipe and is invisible in every screenshot
 *   gate this project has. obs_transition_audio_render with the two mix callbacks below is the
 *   whole fix.
 *
 *   SIZE. It has no size of its own. libobs hands the render callback the canvas it wants drawn,
 *   and that is what the renderer is given -- which is why ff_owns_canvas is false for this kind
 *   and there are no width/height properties in the panel.
 */

#include "ff-props.h"
#include <plugin-support.h>

static const char *tr_name(void *d)
{
	UNUSED_PARAMETER(d);
	return obs_module_text("Foxfire.Transition");
}

static void *tr_create(obs_data_t *s, obs_source_t *self)
{
	return ff_instance_create(s, self, FF_KIND_TRANSITION);
}

static void tr_destroy(void *d)
{
	ff_instance_destroy(d);
}

static void tr_update(void *d, obs_data_t *s)
{
	ff_instance_update(d, s);
}

static void tr_defaults(obs_data_t *s)
{
	ff_instance_defaults(s, FF_KIND_TRANSITION);
}

static obs_properties_t *tr_props(void *d)
{
	return ff_instance_properties(d);
}

static void tr_tick(void *d, float seconds)
{
	((struct ff_instance *)d)->dt = seconds;
}

/* libobs calls this from inside obs_transition_video_render with the two scenes already rendered
   to textures, `t` in 0..1, and the canvas it expects back. */
static void tr_draw(void *d, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct ff_instance *in = d;
	const struct ff_pair_tex pair = {.a = a, .b = b};
	/* the size comes from libobs every frame, exactly as the filter's comes from its target */
	in->width = cx;
	in->height = cy;
	gs_texture_t *tex = ff_instance_render_ex(in, NULL, &pair, t, cx, cy);
	if (!tex)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
	gs_effect_set_texture(img, tex);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); /* the layer stack writes premultiplied alpha */
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, cx, cy);
	gs_blend_state_pop();
}

static void tr_render(void *d, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ff_instance *in = d;
	obs_transition_video_render(in->self, tr_draw);
}

/* The crossfade. Equal-POWER, not equal-gain: two uncorrelated signals summed at (1-t) and t sum
   to a level that dips audibly in the middle, which is heard as a hole in the middle of every
   transition. sqrt keeps the power constant instead. */
static float tr_mix_a(void *d, float t)
{
	UNUSED_PARAMETER(d);
	return sqrtf(1.0f - t);
}

static float tr_mix_b(void *d, float t)
{
	UNUSED_PARAMETER(d);
	return sqrtf(t);
}

static bool tr_audio(void *d, uint64_t *ts_out, struct obs_source_audio_mix *out, uint32_t mixers, size_t channels,
		     size_t sample_rate)
{
	struct ff_instance *in = d;
	return obs_transition_audio_render(in->self, ts_out, out, mixers, channels, sample_rate, tr_mix_a, tr_mix_b);
}

struct obs_source_info ff_transition_info = {
	.id = "foxfire_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	/* no OBS_SOURCE_CUSTOM_DRAW: libobs always sets it for a transition (obs-source.h), and
	   listing it here would be a claim that is not ours to make. */
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = tr_name,
	.create = tr_create,
	.destroy = tr_destroy,
	.update = tr_update,
	.get_defaults = tr_defaults,
	.get_properties = tr_props,
	.video_tick = tr_tick,
	.video_render = tr_render,
	.audio_render = tr_audio,
};
