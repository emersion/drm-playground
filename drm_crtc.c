#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dp_drm.h"
#include "util.h"

void crtc_init(struct crtc *crtc, struct device *dev, uint32_t crtc_id) {
	crtc->dev = dev;
	crtc->id = crtc_id;

	uint32_t active, mode_id;
	struct prop crtc_props[] = {
		{ "ACTIVE", &crtc->props.active, &active, true },
		{ "MODE_ID", &crtc->props.mode_id, &mode_id, true },
	};
	read_obj_props(dev, crtc_id, DRM_MODE_OBJECT_CRTC, crtc_props,
		sizeof(crtc_props) / sizeof(crtc_props[0]));

	crtc->active = active;
	crtc->mode_id = mode_id;

	if (mode_id != 0) {
		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(dev->fd, mode_id);
		if (blob == NULL) {
			fatal_errno("failed to get MODE_ID blob");
		}
		drmModeModeInfo *mode = blob->data;
		crtc->mode = xalloc(sizeof(drmModeModeInfo));
		memcpy(crtc->mode, mode, sizeof(drmModeModeInfo));
		drmModeFreePropertyBlob(blob);
	}
}

void crtc_finish(struct crtc *crtc) {
	struct device *dev = crtc->dev;

	if (crtc->mode_id != 0) {
		drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
	}

	free(crtc->mode);
}

void crtc_update(struct crtc *crtc, drmModeAtomicReq *req) {
	drmModeAtomicAddProperty(req, crtc->id, crtc->props.mode_id, crtc->mode_id);
	drmModeAtomicAddProperty(req, crtc->id, crtc->props.active,
		crtc->mode_id != 0 && crtc->active);
}

void crtc_commit(struct crtc *crtc, uint32_t flags, void *user_data) {
	struct device *dev = crtc->dev;
	int cursor = drmModeAtomicGetCursor(dev->atomic_req);

	crtc_update(crtc, dev->atomic_req);

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		struct connector *conn = &dev->connectors[i];
		if (conn->crtc == crtc) {
			connector_update(conn, dev->atomic_req);
		}
	}

	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];
		if (plane->crtc == crtc) {
			plane_update(plane, dev->atomic_req);
		}
	}

	if (drmModeAtomicCommit(dev->fd, dev->atomic_req, flags, user_data)) {
		fatal_errno("drmModeAtomicCommit failed");
	}

	drmModeAtomicSetCursor(dev->atomic_req, cursor);
}

static bool compare_modes(const drmModeModeInfo *a, const drmModeModeInfo *b) {
	if (a == NULL && b == NULL) {
		return true;
	}
	if (a == NULL || b == NULL) {
		return false;
	}
	return memcmp(a, b, sizeof(drmModeModeInfo)) == 0;
}

void crtc_set_mode(struct crtc *crtc, const drmModeModeInfo *mode) {
	struct device *dev = crtc->dev;

	if (compare_modes(crtc->mode, mode)) {
		return;
	}

	if (crtc->mode_id != 0) {
		drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
		crtc->mode_id = 0;

		free(crtc->mode);
		crtc->mode = NULL;
	}

	if (mode == NULL) {
		printf("assigning NULL mode to CRTC %"PRIu32"\n", crtc->id);
		return;
	}

	if (drmModeCreatePropertyBlob(dev->fd, mode, sizeof(*mode),
			&crtc->mode_id)) {
		fatal_errno("failed to create DRM property blob for mode");
	}

	crtc->mode = xalloc(sizeof(*crtc->mode));
	memcpy(crtc->mode, mode, sizeof(*crtc->mode));

	printf("assigning mode %"PRIu32"x%"PRIu32" to CRTC %"PRIu32"\n",
		mode->hdisplay, mode->vdisplay, crtc->id);
}
