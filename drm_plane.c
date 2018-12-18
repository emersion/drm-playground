#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "drm.h"
#include "util.h"

void plane_init(struct plane *plane, struct device *dev, uint32_t plane_id) {
	printf("initializing plane %"PRIu32"\n", plane_id);

	plane->dev = dev;
	plane->id = plane_id;

	drmModePlane *drm_plane = drmModeGetPlane(dev->fd, plane_id);
	if (!drm_plane) {
		fatal("drmModeGetPlane failed");
	}

	plane->possible_crtcs = drm_plane->possible_crtcs;

	// We could use IN_FORMATS instead here, but it's not yet widely supported
	plane->linear_formats_len = drm_plane->count_formats;
	size_t formats_size = plane->linear_formats_len * sizeof(uint32_t);
	plane->linear_formats = xalloc(formats_size);
	memcpy(plane->linear_formats, drm_plane->formats, formats_size);

	drmModeFreePlane(drm_plane);

	plane->alpha = 1.0;

	// TODO: read the properties
	uint32_t crtc_id = 0;
	struct prop plane_props[] = {
		{ "CRTC_H", &plane->props.crtc_h, NULL, true },
		{ "CRTC_ID", &plane->props.crtc_id, &crtc_id, true },
		{ "CRTC_W", &plane->props.crtc_w, NULL, true },
		{ "CRTC_X", &plane->props.crtc_x, NULL, true },
		{ "CRTC_Y", &plane->props.crtc_y, NULL, true },
		{ "FB_ID", &plane->props.fb_id, NULL, true },
		{ "SRC_H", &plane->props.src_h, NULL, true },
		{ "SRC_W", &plane->props.src_w, NULL, true },
		{ "SRC_X", &plane->props.src_x, NULL, true },
		{ "SRC_Y", &plane->props.src_y, NULL, true },
		{ "alpha", &plane->props.alpha, NULL, false },
		{ "type", &plane->props.type, &plane->type, true },
	};
	read_obj_props(dev, plane_id, DRM_MODE_OBJECT_PLANE, plane_props,
		sizeof(plane_props) / sizeof(plane_props[0]));

	plane->crtc = device_find_crtc(dev, crtc_id);

	printf("plane %"PRIu32" has type %"PRIu32"\n", plane_id, plane->type);
}

void plane_finish(struct plane *plane) {
	free(plane->linear_formats);
}

void plane_set_framebuffer(struct plane *plane, struct framebuffer *fb) {
	if (plane->fb == fb) {
		return;
	}

	plane->fb = fb;

	printf("assigning framebuffer %"PRIu32" to plane %"PRIu32"\n",
		fb->id, plane->id);
}

bool plane_set_crtc(struct plane *plane, struct crtc *crtc) {
	if (plane->crtc == crtc) {
		return true;
	}

	if (crtc != NULL) {
		size_t crtc_idx = crtc - plane->dev->crtcs;
		if ((plane->possible_crtcs & (1 << crtc_idx)) == 0) {
			return false;
		}
	}

	plane->crtc = crtc;

	if (crtc == NULL) {
		printf("assigning NULL CRTC to plane %"PRIu32"\n", plane->id);
		return true;
	}

	printf("assigning CRTC %"PRIu32" to plane %"PRIu32"\n",
		crtc->id, plane->id);
	return true;
}

void plane_update(struct plane *plane, drmModeAtomicReq *req) {
	uint32_t crtc_id = 0;
	uint32_t fb_id = 0;
	if (plane->crtc != NULL && plane->fb != NULL) {
		crtc_id = plane->crtc->id;
		fb_id = plane->fb->id;
	}
	drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_id, crtc_id);
	drmModeAtomicAddProperty(req, plane->id, plane->props.fb_id, fb_id);

	if (plane->crtc != NULL && plane->fb != NULL) {
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_x, plane->x);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_y, plane->y);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_w, plane->width);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_h, plane->height);

		// The src_* properties are in 16.16 fixed point
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_x, 0);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_y, 0);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_w, plane->fb->width << 16);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_h, plane->fb->height << 16);

		if (plane->props.alpha) {
			drmModeAtomicAddProperty(req, plane->id, plane->props.alpha, plane->alpha * 0xFFFF);
		}
	}
}
