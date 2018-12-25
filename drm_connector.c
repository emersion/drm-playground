#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dp_drm.h"
#include "util.h"

void connector_init(struct connector *conn, struct device *dev,
		uint32_t conn_id, struct encoder *encoders, size_t encoders_len) {
	printf("initializing connector %"PRIu32"\n", conn_id);

	conn->dev = dev;
	conn->id = conn_id;

	uint32_t crtc_id = 0;
	struct prop conn_props[] = {
		{ "CRTC_ID", &conn->props.crtc_id, &crtc_id, true },
	};
	read_obj_props(dev, conn_id, DRM_MODE_OBJECT_CONNECTOR, conn_props,
		sizeof(conn_props) / sizeof(conn_props[0]));

	drmModeConnector *drm_conn = drmModeGetConnector(dev->fd, conn_id);
	if (!drm_conn) {
		fatal_errno("failed to get connector %"PRIu32, conn_id);
	}

	conn->state = drm_conn->connection;

	if (drm_conn->count_modes > 0) {
		size_t modes_size = drm_conn->count_modes * sizeof(drmModeModeInfo);
		conn->modes = xalloc(modes_size);
		memcpy(conn->modes, drm_conn->modes, modes_size);
		conn->modes_len = drm_conn->count_modes;
	}

	conn->possible_crtcs = (uint32_t)~0;
	for (int i = 0; i < drm_conn->count_encoders; ++i) {
		uint32_t enc_id = drm_conn->encoders[i];
		bool found = false;
		for (size_t j = 0; j < encoders_len; ++i) {
			if (encoders[i].id == enc_id) {
				conn->possible_crtcs &= encoders[i].possible_crtcs;
				found = true;
				break;
			}
		}
		if (!found) {
			fatal("failed to find encoder %"PRIu32, enc_id);
		}
	}
	if (drm_conn->count_encoders == 0) {
		conn->possible_crtcs = 0;
	}

	drmModeFreeConnector(drm_conn);

	conn->old_crtc = drmModeGetCrtc(dev->fd, crtc_id);
	conn->crtc = device_find_crtc(dev, crtc_id);
}

void connector_finish(struct connector *conn) {
	struct device *dev = conn->dev;

	drmModeCrtc *c = conn->old_crtc;
	if (c != NULL) {
		drmModeSetCrtc(dev->fd, c->crtc_id, c->buffer_id, c->x, c->y,
			&conn->id, 1, &c->mode);
		drmModeFreeCrtc(conn->old_crtc);
	}

	free(conn->modes);
}

bool connector_set_crtc(struct connector *conn, struct crtc *crtc) {
	if (conn->crtc == crtc) {
		return true;
	}

	if (crtc != NULL) {
		size_t crtc_idx = crtc - conn->dev->crtcs;
		if ((conn->possible_crtcs & (1 << crtc_idx)) == 0) {
			return false;
		}
	}

	printf("assigning CRTC %"PRIu32" to connector %"PRIu32"\n",
		crtc ? crtc->id : 0, conn->id);
	conn->crtc = crtc;
	return true;
}

void connector_update(struct connector *conn, drmModeAtomicReq *req) {
	uint32_t crtc_id = (conn->crtc != NULL) ? conn->crtc->id : 0;
	drmModeAtomicAddProperty(req, conn->id, conn->props.crtc_id, crtc_id);
}
