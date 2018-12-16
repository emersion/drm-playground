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

	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fatal_errno("failed to open \"%s\"", path);
	}

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fatal("DRM device must support atomic modesetting");
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		fatal("DRM device must support universal planes");
	}

	uint64_t has_dumb;
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
		fatal("DRM device must support dumb buffers");
	}

	dev->fd = fd;

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
		drmModeEncoder *enc = drmModeGetEncoder(fd, res->encoders[i]);
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

static void connector_update(struct connector *conn, drmModeAtomicReq *req);
static void crtc_update(struct crtc *crtc, drmModeAtomicReq *req);
static void plane_update(struct plane *plane, drmModeAtomicReq *req);

void device_commit(struct device *dev, uint32_t flags) {
	int cursor = drmModeAtomicGetCursor(dev->atomic_req);

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		connector_update(&dev->connectors[i], dev->atomic_req);
	}

	for (size_t i = 0; i < dev->crtcs_len; ++i) {
		crtc_update(&dev->crtcs[i], dev->atomic_req);
	}

	for (size_t i = 0; i < dev->planes_len; ++i) {
		plane_update(&dev->planes[i], dev->atomic_req);
	}

	if (drmModeAtomicCommit(dev->fd, dev->atomic_req, flags, NULL)) {
		fatal_errno("drmModeAtomicCommit failed");
	}

	drmModeAtomicSetCursor(dev->atomic_req, cursor);
}

struct prop {
	const char *name;
	uint32_t *dest;
	uint32_t *value;
	bool required;
};

static int prop_cmp(const void *arg1, const void *arg2) {
	const char *key = arg1;
	const struct prop *val = arg2;

	return strcmp(key, val->name);
}

void read_obj_props(struct device *dev, uint32_t obj_id, uint32_t obj_type,
		struct prop *props, size_t props_len) {
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

		struct prop *p = bsearch(prop->name, props, props_len,
			sizeof(*props), prop_cmp);
		if (p) {
			seen[p - props] = true;
			*p->dest = prop->prop_id;
			if (p->value) {
				*p->value = obj_props->prop_values[i];
			}
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

static void connector_init(struct connector *conn, struct device *dev,
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

	if (!connector_set_crtc(conn, device_find_crtc(dev, crtc_id))) {
		fatal("failed to set CRTC for connector %"PRIu32, conn->id);
	}
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

bool connector_set_crtc(struct connector *conn, struct crtc *crtc) {
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

static void connector_update(struct connector *conn, drmModeAtomicReq *req) {
	uint32_t crtc_id = (conn->crtc != NULL) ? conn->crtc->id : 0;
	drmModeAtomicAddProperty(req, conn->id, conn->props.crtc_id, crtc_id);
}

static void crtc_init(struct crtc *crtc, struct device *dev, uint32_t crtc_id) {
	crtc->dev = dev;
	crtc->id = crtc_id;

	// TODO: read current mode
	uint32_t active;
	struct prop crtc_props[] = {
		{ "ACTIVE", &crtc->props.active, &active, true },
		{ "MODE_ID", &crtc->props.mode_id, NULL, true },
	};
	read_obj_props(dev, crtc_id, DRM_MODE_OBJECT_CRTC, crtc_props,
		sizeof(crtc_props) / sizeof(crtc_props[0]));

	crtc->active = active;
}

static void crtc_finish(struct crtc *crtc) {
	struct device *dev = crtc->dev;
	drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
}

static void crtc_update(struct crtc *crtc, drmModeAtomicReq *req) {
	drmModeAtomicAddProperty(req, crtc->id, crtc->props.mode_id, crtc->mode_id);
	drmModeAtomicAddProperty(req, crtc->id, crtc->props.active,
		crtc->mode_id != 0 && crtc->active);
}

void crtc_set_mode(struct crtc *crtc, drmModeModeInfo *mode) {
	struct device *dev = crtc->dev;

	if (crtc->mode_id != 0) {
		drmModeDestroyPropertyBlob(dev->fd, crtc->mode_id);
		crtc->mode_id = 0;
		crtc->width = crtc->height = 0;
	}

	if (mode == NULL) {
		printf("assigning NULL mode to CRTC %"PRIu32"\n", crtc->id);
		return;
	}

	if (drmModeCreatePropertyBlob(dev->fd, mode, sizeof(*mode),
			&crtc->mode_id)) {
		fatal_errno("failed to create DRM property blob for mode");
	}

	crtc->width = mode->hdisplay;
	crtc->height = mode->vdisplay;

	printf("assigning mode %"PRIu32"x%"PRIu32" to CRTC %"PRIu32"\n",
		crtc->width, crtc->height, crtc->id);
}

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

	printf("plane %"PRIu32" has type %"PRIu32"\n", plane_id, plane->type);

	plane_set_crtc(plane, device_find_crtc(dev, crtc_id));
}

static void plane_finish(struct plane *plane) {
	// No-op
}

uint32_t plane_dumb_format(struct plane *plane) {
	uint32_t fb_fmt = DRM_FORMAT_INVALID;

	// We could use IN_FORMATS instead here, but it's not yet widely supported
	drmModePlane *drm_plane = drmModeGetPlane(plane->dev->fd, plane->id);
	if (!plane) {
		fatal("drmModeGetPlane failed");
	}

	for (uint32_t i = 0; i < drm_plane->count_formats; ++i) {
		uint32_t fmt = drm_plane->formats[i];
		switch (fmt) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			fb_fmt = fmt;
			break;
		}
	}

	drmModeFreePlane(drm_plane);

	return fb_fmt;
}

void plane_set_framebuffer(struct plane *plane, struct framebuffer *fb) {
	plane->fb = fb;
}

bool plane_set_crtc(struct plane *plane, struct crtc *crtc) {
	if (crtc != NULL) {
		size_t crtc_idx = crtc - plane->dev->crtcs;
		if ((plane->possible_crtcs & (1 << crtc_idx)) == 0) {
			return false;
		}
	}

	plane->crtc = crtc;

	if (crtc == NULL) {
		plane->width = plane->height = 0;
		printf("assigning NULL CRTC to plane %"PRIu32"\n", plane->id);
		return true;
	}

	switch (plane->type) {
	case DRM_PLANE_TYPE_OVERLAY:
		plane->width = plane->height = 100;
		break;
	case DRM_PLANE_TYPE_PRIMARY:
		plane->width = crtc->width;
		plane->height = crtc->height;
		break;
	case DRM_PLANE_TYPE_CURSOR:;
		// Some drivers *require* the FB to have exactly this size
		uint64_t width, height;
		if (drmGetCap(plane->dev->fd, DRM_CAP_CURSOR_WIDTH, &width) != 0) {
			fatal("drmGetCap(DRM_CAP_CURSOR_WIDTH) failed");
		}
		if (drmGetCap(plane->dev->fd, DRM_CAP_CURSOR_HEIGHT, &height) != 0) {
			fatal("drmGetCap(DRM_CAP_CURSOR_HEIGHT) failed");
		}
		plane->width = width;
		plane->height = height;
		break;
	}

	printf("assigning CRTC %"PRIu32" to plane %"PRIu32"\n",
		crtc->id, plane->id);
	return true;
}

static void plane_update(struct plane *plane, drmModeAtomicReq *req) {
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

void dumb_framebuffer_init(struct dumb_framebuffer *fb, struct device *dev,
		uint32_t fmt, uint32_t width, uint32_t height) {
	int ret;

	printf("initializing dumb framebuffer with format %"PRIu32" and "
		"size %"PRIu32"x%"PRIu32"\n", fmt, width, height);

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
