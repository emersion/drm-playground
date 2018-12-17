#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "dp.h"
#include "util.h"

struct encoder {
	uint32_t id;
	uint32_t possible_crtcs;
};

static void connector_init(struct connector *conn, struct device *dev,
	uint32_t conn_id, struct encoder *encoders, size_t encoders_len);
static void crtc_init(struct crtc *crtc, struct device *dev, uint32_t crtc_id);
static void plane_init(struct plane *plane, struct device *dev,
	uint32_t plane_id);

void device_init(struct device *dev, const char *path) {
	printf("opening device \"%s\"\n", path);

	dev->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (dev->fd < 0) {
		fatal_errno("failed to open \"%s\"", path);
	}

	if (drmSetClientCap(dev->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		fatal("DRM device must support atomic modesetting");
	}
	if (drmSetClientCap(dev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		fatal("DRM device must support universal planes");
	}

	uint64_t has_dumb;
	if (drmGetCap(dev->fd, DRM_CAP_DUMB_BUFFER, &has_dumb) != 0) {
		fatal("drmGetCap(DRM_CAP_DUMB_BUFFER) failed");
	}
	dev->caps.dumb = has_dumb;

	uint64_t cursor_width, cursor_height;
	if (drmGetCap(dev->fd, DRM_CAP_CURSOR_WIDTH, &cursor_width) != 0) {
		fatal("drmGetCap(DRM_CAP_CURSOR_WIDTH) failed");
	}
	if (drmGetCap(dev->fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height) != 0) {
		fatal("drmGetCap(DRM_CAP_CURSOR_HEIGHT) failed");
	}
	dev->caps.cursor_width = cursor_width;
	dev->caps.cursor_height = cursor_height;

	dev->atomic_req = drmModeAtomicAlloc();
	if (!dev->atomic_req) {
		fatal_errno("drmModeAtomicAlloc failed");
	}

	drmModeRes *res = drmModeGetResources(dev->fd);
	if (!res) {
		fatal("drmModeGetResources failed");
	}

	size_t encoders_len = res->count_encoders;
	struct encoder *encoders = xalloc(encoders_len * sizeof(struct encoder));
	for (int i = 0; i < res->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(dev->fd, res->encoders[i]);
		if (enc == NULL) {
			fatal("drmModeGetEncoder failed");
		}
		encoders[i] = (struct encoder){
			.id = res->encoders[i],
			.possible_crtcs = enc->possible_crtcs,
		};
		drmModeFreeEncoder(enc);
	}

	// CRTCs need to be initialized before connectors
	dev->crtcs = xalloc(res->count_crtcs * sizeof(struct crtc));
	for (int i = 0; i < res->count_crtcs; ++i) {
		struct crtc *crtc = &dev->crtcs[dev->crtcs_len];
		crtc_init(crtc, dev, res->crtcs[i]);
		++dev->crtcs_len;
	}

	dev->connectors = xalloc(res->count_connectors * sizeof(struct connector));
	for (int i = 0; i < res->count_connectors; ++i) {
		struct connector *conn = &dev->connectors[dev->connectors_len];
		connector_init(conn, dev, res->connectors[i], encoders, encoders_len);
		++dev->connectors_len;
	}

	free(encoders);

	drmModeFreeResources(res);

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res) {
		fatal("drmModeGetPlaneResources failed");
	}

	dev->planes = xalloc(plane_res->count_planes * sizeof(struct plane));
	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		struct plane *plane = &dev->planes[dev->planes_len];
		plane_init(plane, dev, plane_res->planes[i]);
		++dev->planes_len;
	}

	drmModeFreePlaneResources(plane_res);
}

static void connector_finish(struct connector *conn);
static void crtc_finish(struct crtc *crtc);
static void plane_finish(struct plane *plane);

void device_finish(struct device *dev) {
	for (size_t i = 0; i < dev->planes_len; ++i) {
		plane_finish(&dev->planes[i]);
	}

	for (size_t i = 0; i < dev->crtcs_len; ++i) {
		crtc_finish(&dev->crtcs[i]);
	}

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		connector_finish(&dev->connectors[i]);
	}

	free(dev->planes);
	free(dev->crtcs);
	free(dev->connectors);
	drmModeAtomicFree(dev->atomic_req);
	close(dev->fd);
}

static struct crtc *device_find_crtc(struct device *dev, uint32_t crtc_id) {
	if (crtc_id == 0) {
		return NULL;
	}

	for (size_t i = 0; i < dev->crtcs_len; ++i) {
		struct crtc *crtc = &dev->crtcs[i];
		if (crtc->id == crtc_id) {
			return crtc;
		}
	}
	return NULL;
}

static void connector_push(struct connector *conn, drmModeAtomicReq *req);
static void crtc_push(struct crtc *crtc, drmModeAtomicReq *req);
static void plane_push(struct plane *plane, drmModeAtomicReq *req);

void device_commit(struct device *dev, uint32_t flags) {
	int cursor = drmModeAtomicGetCursor(dev->atomic_req);

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		connector_push(&dev->connectors[i], dev->atomic_req);
	}

	for (size_t i = 0; i < dev->crtcs_len; ++i) {
		crtc_push(&dev->crtcs[i], dev->atomic_req);
	}

	for (size_t i = 0; i < dev->planes_len; ++i) {
		plane_push(&dev->planes[i], dev->atomic_req);
	}

	if (drmModeAtomicCommit(dev->fd, dev->atomic_req, flags, NULL)) {
		fatal_errno("drmModeAtomicCommit failed");
	}

	drmModeAtomicSetCursor(dev->atomic_req, cursor);
}

struct prop_init {
	const char *name;
	uint32_t *id;
	bool required;
};

static struct prop_init *find_prop_init(struct prop_init *props,
		size_t props_len, const char *name) {
	for (size_t i = 0; i < props_len; ++i) {
		if (strcmp(props[i].name, name) == 0) {
			return &props[i];
		}
	}
	return NULL;
}

static void init_obj_props(struct device *dev, uint32_t obj_id,
		uint32_t obj_type, struct prop_init *props, size_t props_len) {
	drmModeObjectProperties *obj_props =
		drmModeObjectGetProperties(dev->fd, obj_id, obj_type);
	if (!obj_props) {
		fatal_errno("drmModeObjectGetProperties failed");
	}

	bool seen[props_len + 1];
	memset(seen, false, props_len);
	for (uint32_t i = 0; i < obj_props->count_props; ++i) {
		drmModePropertyRes *prop =
			drmModeGetProperty(dev->fd, obj_props->props[i]);
		if (!prop) {
			fatal_errno("drmModeGetProperty failed");
		}

		struct prop_init *p = find_prop_init(props, props_len, prop->name);
		if (p != NULL) {
			seen[p - props] = true;
			*p->id = prop->prop_id;
		}

		drmModeFreeProperty(prop);
	}

	for (size_t i = 0; i < props_len; ++i) {
		if (!seen[i] && props[i].required) {
			fatal("object is missing required property %s", props[i].name);
		}
	}

	drmModeFreeObjectProperties(obj_props);
}

struct prop_pull {
	uint32_t id;
	uint64_t *value;
};

static struct prop_pull *find_prop_pull(struct prop_pull *props,
		size_t props_len, uint32_t id) {
	for (size_t i = 0; i < props_len; ++i) {
		if (props[i].id == id) {
			return &props[i];
		}
	}
	return NULL;
}

static void pull_obj_props(struct device *dev, uint32_t obj_id,
		uint32_t obj_type, struct prop_pull *props, size_t props_len) {
	drmModeObjectProperties *obj_props =
		drmModeObjectGetProperties(dev->fd, obj_id, obj_type);
	if (!obj_props) {
		fatal_errno("drmModeObjectGetProperties failed");
	}

	for (uint32_t i = 0; i < obj_props->count_props; ++i) {
		struct prop_pull *p =
			find_prop_pull(props, props_len, obj_props->props[i]);
		if (p != NULL) {
			*p->value = obj_props->prop_values[i];
		}
	}

	drmModeFreeObjectProperties(obj_props);
}

static void connector_pull(struct connector *conn);

static void connector_init(struct connector *conn, struct device *dev,
		uint32_t conn_id, struct encoder *encoders, size_t encoders_len) {
	printf("initializing connector %"PRIu32"\n", conn_id);

	conn->dev = dev;
	conn->id = conn_id;

	struct prop_init props[] = {
		{ "CRTC_ID", &conn->props.crtc_id, true },
	};
	init_obj_props(conn->dev, conn->id, DRM_MODE_OBJECT_CONNECTOR, props,
		sizeof(props) / sizeof(props[0]));

	drmModeConnector *drm_conn = drmModeGetConnector(conn->dev->fd, conn->id);
	if (!drm_conn) {
		fatal_errno("failed to get connector %"PRIu32, conn->id);
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

	connector_pull(conn);
	conn->old_crtc = drmModeGetCrtc(dev->fd,
		conn->current.crtc ? conn->current.crtc->id : 0);
}

static void connector_finish(struct connector *conn) {
	struct device *dev = conn->dev;

	drmModeCrtc *c = conn->old_crtc;
	if (c != NULL) {
		drmModeSetCrtc(dev->fd, c->crtc_id, c->buffer_id, c->x, c->y,
			&conn->id, 1, &c->mode);
		drmModeFreeCrtc(conn->old_crtc);
	}

	free(conn->modes);
}

static void connector_pull(struct connector *conn) {
	drmModeConnector *drm_conn = drmModeGetConnector(conn->dev->fd, conn->id);
	if (!drm_conn) {
		fatal_errno("failed to get connector %"PRIu32, conn->id);
	}

	conn->state = drm_conn->connection;

	if (drm_conn->count_modes > 0) {
		size_t modes_size = drm_conn->count_modes * sizeof(drmModeModeInfo);
		free(conn->modes);
		conn->modes = xalloc(modes_size);
		memcpy(conn->modes, drm_conn->modes, modes_size);
		conn->modes_len = drm_conn->count_modes;
	}

	drmModeFreeConnector(drm_conn);

	uint64_t crtc_id = 0;
	struct prop_pull props[] = {
		{ conn->props.crtc_id, &crtc_id },
	};
	pull_obj_props(conn->dev, conn->id, DRM_MODE_OBJECT_CONNECTOR, props,
		sizeof(props) / sizeof(props[0]));

	conn->current.crtc = device_find_crtc(conn->dev, crtc_id);
}

static void connector_push(struct connector *conn, drmModeAtomicReq *req) {
	struct connector_state *state = &conn->pending;

	uint32_t crtc_id = (state->crtc != NULL) ? state->crtc->id : 0;
	drmModeAtomicAddProperty(req, conn->id, conn->props.crtc_id, crtc_id);

	memcpy(&conn->current, &conn->pending, sizeof(struct connector_state));
}

bool connector_set_crtc(struct connector *conn, struct crtc *crtc) {
	if (conn->pending.crtc == crtc) {
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
	conn->pending.crtc = crtc;
	return true;
}

static void crtc_pull(struct crtc *crtc);

static void crtc_init(struct crtc *crtc, struct device *dev, uint32_t crtc_id) {
	crtc->dev = dev;
	crtc->id = crtc_id;

	struct prop_init props[] = {
		{ "ACTIVE", &crtc->props.active, true },
		{ "MODE_ID", &crtc->props.mode_id, true },
	};
	init_obj_props(crtc->dev, crtc->id, DRM_MODE_OBJECT_CRTC, props,
		sizeof(props) / sizeof(props[0]));

	crtc_pull(crtc);
}

static void crtc_finish(struct crtc *crtc) {
	struct device *dev = crtc->dev;

	if (crtc->mode_id != 0) {
		drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
	}

	free(crtc->current.mode);
	free(crtc->pending.mode);
}

static void crtc_state_set_mode(struct crtc_state *state,
		const drmModeModeInfo *mode) {
	free(state->mode);
	state->mode = NULL;

	if (mode != NULL) {
		state->mode = xalloc(sizeof(drmModeModeInfo));
		memcpy(state->mode, mode, sizeof(drmModeModeInfo));
	}
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

static void crtc_pull(struct crtc *crtc) {
	uint64_t active, mode_id;
	struct prop_pull props[] = {
		{ crtc->props.active, &active },
		{ crtc->props.mode_id, &mode_id },
	};
	pull_obj_props(crtc->dev, crtc->id, DRM_MODE_OBJECT_CRTC, props,
		sizeof(props) / sizeof(props[0]));

	crtc->current.active = !!active;

	crtc->mode_id = mode_id;
	if (mode_id != 0) {
		drmModePropertyBlobRes *blob =
			drmModeGetPropertyBlob(crtc->dev->fd, mode_id);
		if (blob == NULL) {
			fatal_errno("failed to get MODE_ID blob");
		}
		drmModeModeInfo *mode = blob->data;
		crtc_state_set_mode(&crtc->current, mode);
		drmModeFreePropertyBlob(blob);
	}
}

static void crtc_push(struct crtc *crtc, drmModeAtomicReq *req) {
	struct crtc_state *state = &crtc->pending;

	// Create a new blob for the mode if it has changed
	if (!compare_modes(state->mode, crtc->current.mode)) {
		if (crtc->mode_id != 0) {
			drmModeDestroyPropertyBlob(crtc->dev->fd, crtc->mode_id);
			crtc->mode_id = 0;
		}

		if (state->mode != NULL) {
			if (drmModeCreatePropertyBlob(crtc->dev->fd, state->mode,
					sizeof(drmModeModeInfo), &crtc->mode_id) != 0) {
				fatal_errno("failed to create DRM property blob for mode");
			}
		}
	}

	drmModeAtomicAddProperty(req, crtc->id, crtc->props.mode_id, crtc->mode_id);
	drmModeAtomicAddProperty(req, crtc->id, crtc->props.active, state->active);

	crtc->current.active = crtc->pending.active;
	crtc_state_set_mode(&crtc->current, crtc->pending.mode);
}

void crtc_commit(struct crtc *crtc, uint32_t flags, void *user_data) {
	struct device *dev = crtc->dev;
	int cursor = drmModeAtomicGetCursor(dev->atomic_req);

	crtc_push(crtc, dev->atomic_req);

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		struct connector *conn = &dev->connectors[i];
		if (conn->current.crtc == crtc || conn->pending.crtc == crtc) {
			connector_push(conn, dev->atomic_req);
		}
	}

	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];
		if (plane->current.crtc == crtc || plane->pending.crtc == crtc) {
			plane_push(plane, dev->atomic_req);
		}
	}

	if (drmModeAtomicCommit(dev->fd, dev->atomic_req, flags, user_data)) {
		fatal_errno("drmModeAtomicCommit failed");
	}

	drmModeAtomicSetCursor(dev->atomic_req, cursor);
}

void crtc_set_mode(struct crtc *crtc, const drmModeModeInfo *mode) {
	struct device *dev = crtc->dev;

	if (compare_modes(crtc->pending.mode, mode)) {
		return;
	}

	if (crtc->mode_id != 0) {
		drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
		crtc->mode_id = 0;

		free(crtc->pending.mode);
		crtc->pending.mode = NULL;
	}

	if (mode == NULL) {
		printf("assigning NULL mode to CRTC %"PRIu32"\n", crtc->id);
		return;
	}

	if (drmModeCreatePropertyBlob(dev->fd, mode, sizeof(*mode),
			&crtc->mode_id)) {
		fatal_errno("failed to create DRM property blob for mode");
	}

	crtc->pending.mode = xalloc(sizeof(drmModeModeInfo));
	memcpy(crtc->pending.mode, mode, sizeof(drmModeModeInfo));

	printf("assigning mode %"PRIu32"x%"PRIu32" to CRTC %"PRIu32"\n",
		mode->hdisplay, mode->vdisplay, crtc->id);
}

static void plane_pull(struct plane *plane);

static void plane_init(struct plane *plane, struct device *dev,
		uint32_t plane_id) {
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

	plane->current.alpha = plane->pending.alpha = 1.0;

	struct prop_init props[] = {
		{ "CRTC_H", &plane->props.crtc_h, true },
		{ "CRTC_ID", &plane->props.crtc_id, true },
		{ "CRTC_W", &plane->props.crtc_w, true },
		{ "CRTC_X", &plane->props.crtc_x, true },
		{ "CRTC_Y", &plane->props.crtc_y, true },
		{ "FB_ID", &plane->props.fb_id, true },
		{ "SRC_H", &plane->props.src_h, true },
		{ "SRC_W", &plane->props.src_w, true },
		{ "SRC_X", &plane->props.src_x, true },
		{ "SRC_Y", &plane->props.src_y, true },
		{ "alpha", &plane->props.alpha, false },
		{ "type", &plane->props.type, true },
	};
	init_obj_props(plane->dev, plane->id, DRM_MODE_OBJECT_PLANE, props,
		sizeof(props) / sizeof(props[0]));

	plane_pull(plane);

	printf("plane %"PRIu32" has type %"PRIu32"\n", plane_id, plane->type);
}

static void plane_finish(struct plane *plane) {
	free(plane->linear_formats);
}

static void plane_pull(struct plane *plane) {
	// TODO: pull other properties too
	uint64_t type = 0, crtc_id = 0;
	struct prop_pull props[] = {
		{ plane->props.crtc_id, &crtc_id },
		{ plane->props.type, &type },
	};
	pull_obj_props(plane->dev, plane->id, DRM_MODE_OBJECT_PLANE, props,
		sizeof(props) / sizeof(props[0]));

	// TODO: maybe move immutable properties to init?
	plane->type = type;

	plane->current.crtc = device_find_crtc(plane->dev, crtc_id);
}

static void plane_push(struct plane *plane, drmModeAtomicReq *req) {
	struct plane_state *state = &plane->pending;

	uint32_t crtc_id = 0;
	uint32_t fb_id = 0;
	if (state->crtc != NULL && state->fb != NULL) {
		crtc_id = state->crtc->id;
		fb_id = state->fb->id;
	}
	drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_id, crtc_id);
	drmModeAtomicAddProperty(req, plane->id, plane->props.fb_id, fb_id);

	if (state->crtc != NULL && state->fb != NULL) {
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_x, state->x);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_y, state->y);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_w, state->width);
		drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_h, state->height);

		// The src_* properties are in 16.16 fixed point
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_x, 0);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_y, 0);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_w, state->fb->width << 16);
		drmModeAtomicAddProperty(req, plane->id, plane->props.src_h, state->fb->height << 16);

		if (plane->props.alpha) {
			drmModeAtomicAddProperty(req, plane->id, plane->props.alpha, state->alpha * 0xFFFF);
		}
	}

	memcpy(&plane->current, &plane->pending, sizeof(struct plane_state));
}

void plane_set_framebuffer(struct plane *plane, struct framebuffer *fb) {
	if (plane->pending.fb == fb) {
		return;
	}

	plane->pending.fb = fb;

	printf("assigning framebuffer %"PRIu32" to plane %"PRIu32"\n",
		fb->id, plane->id);
}

bool plane_set_crtc(struct plane *plane, struct crtc *crtc) {
	if (plane->pending.crtc == crtc) {
		return true;
	}

	if (crtc != NULL) {
		size_t crtc_idx = crtc - plane->dev->crtcs;
		if ((plane->possible_crtcs & (1 << crtc_idx)) == 0) {
			return false;
		}
	}

	plane->pending.crtc = crtc;

	if (crtc == NULL) {
		printf("assigning NULL CRTC to plane %"PRIu32"\n", plane->id);
		return true;
	}

	printf("assigning CRTC %"PRIu32" to plane %"PRIu32"\n",
		crtc->id, plane->id);
	return true;
}

void dumb_framebuffer_init(struct dumb_framebuffer *fb, struct device *dev,
		uint32_t fmt, uint32_t width, uint32_t height) {
	int ret;

	printf("initializing dumb framebuffer with format %"PRIu32" and "
		"size %"PRIu32"x%"PRIu32"\n", fmt, width, height);

	if (!dev->caps.dumb) {
		fatal("DRM device doesn't support dumb frambuffers");
	}

	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		break;
	default:
		fatal("format %"PRIu32" not supported");
	}

	struct drm_mode_create_dumb create = {
		.width = width,
		.height = height,
		.bpp = 32,
		.flags = 0,
	};
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret < 0) {
		fatal("DRM_IOCTL_MODE_CREATE_DUMB failed");
	}

	fb->fb.dev = dev;
	fb->fb.width = width;
	fb->fb.height = height;
	fb->stride = create.pitch;
	fb->handle = create.handle;
	fb->size = create.size;

	uint32_t handles[4] = { fb->handle };
	uint32_t strides[4] = { fb->stride };
	uint32_t offsets[4] = { 0 };
	ret = drmModeAddFB2(dev->fd, width, height, fmt, handles, strides, offsets,
		&fb->fb.id, 0);
	if (ret < 0) {
		fatal("drmModeAddFB2 failed");
	}

	struct drm_mode_map_dumb map = { .handle = fb->handle };
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret < 0) {
		fatal("DRM_IOCTL_MODE_MAP_DUMB failed");
	}

	fb->data = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		dev->fd, map.offset);
	if (!fb->data) {
		fatal("mmap failed");
	}

	memset(fb->data, 0xFF, fb->size);

	printf("dumb framebuffer %"PRIu32" initialized\n", fb->fb.id);
}

void dumb_framebuffer_finish(struct dumb_framebuffer *fb) {
	munmap(fb->data, fb->size);
	fb->data = NULL;

	drmModeRmFB(fb->fb.dev->fd, fb->fb.id);
	fb->fb.id = 0;

	struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
	drmIoctl(fb->fb.dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}
