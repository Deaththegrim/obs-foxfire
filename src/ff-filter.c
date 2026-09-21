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

static const char *flt_name(void *d)
{
	UNUSED_PARAMETER(d);
	return obs_module_text("Foxfire.Effects");
}

static void *flt_create(obs_data_t *s, obs_source_t *self)
{
	return ff_instance_create(s, self, true);
}

static void flt_destroy(void *d)
{
	ff_instance_destroy(d);
}

static void flt_update(void *d, obs_data_t *s)
{
	ff_instance_update(d, s);
}

static void flt_defaults(obs_data_t *s)
{
	ff_instance_defaults(s, true);
}

static obs_properties_t *flt_props(void *d)
{
	return ff_instance_properties(d);
}

/* see the comment on src_tick in ff-source.c: dt lives on the instance, not a file-scope float */
static void flt_tick(void *d, float seconds)
{
	((struct ff_instance *)d)->dt = seconds;
}

static void flt_render(void *d, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ff_instance *in = d;
	obs_source_t *target = obs_filter_get_target(in->self);
	uint32_t w = target ? obs_source_get_base_width(target) : 0;
	uint32_t h = target ? obs_source_get_base_height(target) : 0;
	/* in->capture is the one GPU object ff_instance_create does not guarantee (see ff_require at
	   its gs_texrender_create call in ff-props.c) -- a create failure there must skip the filter
	   here, not reach gs_texrender_reset/begin below with a NULL texrender and crash OBS. */
	if (!target || !w || !h || !in->renderer->nlayers || !in->capture) {
		obs_source_skip_video_filter(in->self);
		return;
	}
	/* a filter has no S_WIDTH/S_HEIGHT settings -- the target's size drives uv_size every frame,
	   never the settings (ff_instance_update skips them for a filter, see ff-props.c) */
	in->width = w;
	in->height = h;

	/* capture the target into our own texrender */
	gs_texrender_reset(in->capture);
	if (!gs_texrender_begin(in->capture, w, h)) {
		obs_source_skip_video_filter(in->self);
		return;
	}
	struct vec4 clear = {0};
	gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
	gs_ortho(0.f, (float)w, 0.f, (float)h, -100.f, 100.f);
	gs_blend_state_push();
	/* Colour channels blend SRCALPHA/INVSRCALPHA, alpha blends ONE/INVSRCALPHA -- the same
	   asymmetric pair libobs's own obs_source_process_filter_begin uses to capture a filter's
	   target. Onto our zero-cleared destination that computes exactly rgb*=alpha (straight ->
	   premultiplied) with alpha passed through, which is what the layer stack (bars.effect,
	   glow.effect, ...) is written to consume -- the same convention its output is drawn with
	   below (GS_BLEND_ONE/GS_BLEND_INVSRCALPHA), and the one src_render in ff-source.c already
	   uses. A target that manages its own blend state around its own draw call, as any
	   well-behaved OBS source does so it composites correctly in an ordinary scene too (verified
	   against image_source, which pushes GS_BLEND_ONE/GS_BLEND_INVSRCALPHA around its sprite and
	   already uploads premultiplied pixel data -- gs_premultiply_xyza*_loop runs at image load),
	   overrides this regardless, so this only matters for a target that does not. Either way the
	   destination it lands in is premultiplied, never straight. */
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	/* obs_source_video_render(target) hands the target's own video_render callback whatever
	   gs_get_effect() currently reports as the "current" effect -- a source that does its own
	   thing regardless (OBS_SOURCE_CUSTOM_DRAW, e.g. color_source) ignores it, but a plain one
	   (image_source, text, browser, most real content) samples ITS "image" param OUT OF that
	   passed-in effect and draws nothing if it is NULL. Normal scene compositing always has one
	   bound from further up the render tree; called bare, from here, there may be none (this is
	   the same technique-begin/end bracket obs_source_default_render opens -- obs-source.c:2883-
	   2898, itself called from the filter capture path in
	   obs_source_process_filter_begin_with_color_space, obs-source.c:4456 -- not the bypass path's
	   render_filter_bypass, which runs from the DRAW side, obs_source_process_filter_tech_end)
	   -- so bind the default effect ourselves before capturing, or any non-custom-draw target
	   renders as fully transparent. */
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *cap_tech = gs_effect_get_technique(def, "Draw");
	size_t cap_passes = gs_technique_begin(cap_tech);
	if (!cap_passes) {
		/* a technique with no passes would leave the texrender holding only the clear colour --
		   no different from a capture that never ran, so treat it exactly like one. gs_technique_end
		   still has to run: gs_technique_begin already set cur_technique/cur_effect regardless of
		   the pass count, and only _end clears them back. */
		obs_log(LOG_ERROR, "filter: the default effect's Draw technique has no passes -- "
				   "graphics subsystem in a bad state?");
		gs_technique_end(cap_tech);
		gs_blend_state_pop();
		gs_texrender_end(in->capture);
		obs_source_skip_video_filter(in->self);
		return;
	}
	for (size_t p = 0; p < cap_passes; p++) {
		gs_technique_begin_pass(cap_tech, p);
		obs_source_video_render(target);
		gs_technique_end_pass(cap_tech);
	}
	gs_technique_end(cap_tech);
	gs_blend_state_pop();
	gs_texrender_end(in->capture);

	gs_texture_t *tex = ff_instance_render(in, gs_texrender_get_texture(in->capture), w, h);
	if (!tex) {
		obs_source_skip_video_filter(in->self);
		return;
	}
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
	gs_blend_state_push();
	/* same convention as src_render in ff-source.c: the layer stack's output is premultiplied
	   alpha, so drawing it onto whatever is already there needs ONE/INVSRCALPHA, not the default
	   straight-alpha SRCALPHA/INVSRCALPHA -- premultiplied output through the wrong blend function
	   double-applies alpha to the colour and darkens every translucent pixel */
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
	gs_blend_state_pop();
}

struct obs_source_info ff_filter_info = {
	.id = "foxfire_effects",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = flt_name,
	.create = flt_create,
	.destroy = flt_destroy,
	.update = flt_update,
	.get_defaults = flt_defaults,
	.get_properties = flt_props,
	.video_tick = flt_tick,
	.video_render = flt_render,
};
