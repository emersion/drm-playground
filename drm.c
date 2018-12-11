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

struct device *device_open(const char *path) {
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

	struct device *dev = xalloc(sizeof(*dev));
	dev->fd = fd;

	dev->gbm = gbm_create_device(fd);
	if (!dev->gbm) {
		fatal_errno("failed to create GBM device");
	}

	return dev;
}

void device_destroy(struct device *dev) {
	if (!dev) {
		return;
	}

	for (size_t i = 0; i < dev->planes_len; ++i) {
		plane_finish(&dev->planes[i]);
	}

	for (size_t i = 0; i < dev->connectors_len; ++i) {
		connector_finish(&dev->connectors[i]);
	}

	gbm_device_destroy(dev->gbm);
	close(dev->fd);
	free(dev);
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
		fatal_errno("Failed to get DRM object properties");
	}

	bool seen[props_len + 1];
	memset(seen, false, props_len);
	for (uint32_t i = 0; i < obj_props->count_props; ++i) {
		drmModePropertyRes *prop =
			drmModeGetProperty(dev->fd, obj_props->props[i]);
		if (!prop) {
			fatal_errno("Failed to get DRM property");
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

void connector_init(struct connector *conn, struct device *dev,
		uint32_t conn_id) {
	printf("initializing conn-id %"PRIu32"\n", conn_id);

	conn->dev = dev;
	conn->id = conn_id;

	drmModeConnector *drm_conn = drmModeGetConnector(dev->fd, conn_id);
	if (!drm_conn) {
		fatal_errno("failed to get conn-id %"PRIu32, conn_id);
	}

	if (drm_conn->connection != DRM_MODE_CONNECTED ||
			drm_conn->count_modes == 0) {
		fatal("conn-id %"PRIu32" not connected", conn_id);
	}

	if (drmModeCreatePropertyBlob(dev->fd, &drm_conn->modes[0],
			sizeof(drm_conn->modes[0]), &conn->mode_id)) {
		fatal_errno("failed to create DRM property blob for mode");
	}

	conn->width = drm_conn->modes[0].hdisplay;
	conn->height = drm_conn->modes[0].vdisplay;

	drmModeFreeConnector(drm_conn);

	struct prop conn_props[] = {
		{ "CRTC_ID", &conn->props.crtc_id, &conn->crtc_id, true },
	};
	read_obj_props(dev, conn_id, DRM_MODE_OBJECT_CONNECTOR, conn_props,
		sizeof(conn_props) / sizeof(conn_props[0]));

	drmModeRes *res = drmModeGetResources(dev->fd);
	if (!res) {
		fatal("drmModeGetResources failed");
	}

	conn->crtc_idx = -1;
	for (int i = 0; i < res->count_crtcs; ++i) {
		if (res->crtcs[i] == conn->crtc_id) {
			conn->crtc_idx = i;
			break;
		}
	}
	if (conn->crtc_idx == -1) {
		fatal("failed to find CRTC in list");
	}

	drmModeFreeResources(res);

	struct prop crtc_props[] = {
		{ "ACTIVE", &conn->crtc_props.active, NULL, true },
		{ "MODE_ID", &conn->crtc_props.mode_id, NULL, true },
	};
	read_obj_props(dev, conn->crtc_id, DRM_MODE_OBJECT_CRTC, crtc_props,
		sizeof(crtc_props) / sizeof(crtc_props[0]));

	conn->old_crtc = drmModeGetCrtc(dev->fd, conn->crtc_id);

	conn->atomic = drmModeAtomicAlloc();
	if (!conn->atomic) {
		fatal_errno("drmModeAtomicAlloc failed");
	}

	printf("using crtc-id %"PRIu32" for conn-id %"PRIu32"\n", conn->crtc_id, conn_id);
	printf("using mode-id %"PRIu32" for conn-id %"PRIu32"\n", conn->mode_id, conn_id);

	drmModeAtomicAddProperty(conn->atomic, conn->id, conn->props.crtc_id, conn->crtc_id);
	drmModeAtomicAddProperty(conn->atomic, conn->crtc_id, conn->crtc_props.active, 1);
	drmModeAtomicAddProperty(conn->atomic, conn->crtc_id, conn->crtc_props.mode_id, conn->mode_id);

	connector_commit(conn, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK);
}

void connector_finish(struct connector *conn) {
	struct device *dev = conn->dev;

	drmModeDestroyPropertyBlob(dev->fd, conn->mode_id);
	drmModeAtomicFree(conn->atomic);

	drmModeCrtc *c = conn->old_crtc;
	drmModeSetCrtc(dev->fd, c->crtc_id, c->buffer_id, c->x, c->y,
		&conn->id, 1, &c->mode);
	drmModeFreeCrtc(conn->old_crtc);
}

static void plane_update(struct plane *plane, drmModeAtomicReq *req);

void connector_commit(struct connector *conn, uint32_t flags) {
	struct device *dev = conn->dev;
	int cursor = drmModeAtomicGetCursor(conn->atomic);

	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];
		if (plane->conn == conn) {
			plane_update(plane, conn->atomic);
		}
	}

	if (drmModeAtomicCommit(dev->fd, conn->atomic, flags, NULL)) {
		fatal_errno("atomic commit failed");
	}

	drmModeAtomicSetCursor(conn->atomic, cursor);
}

void plane_init(struct plane *plane, struct device *dev, uint32_t plane_id) {
	printf("initializing plane-id %"PRIu32"\n", plane_id);

	plane->dev = dev;
	plane->id = plane_id;

	drmModePlane *drm_plane = drmModeGetPlane(dev->fd, plane_id);
	if (!drm_plane) {
		fatal("drmModeGetPlane failed");
	}
	plane->possible_crtcs = drm_plane->possible_crtcs;
	drmModeFreePlane(drm_plane);

	plane->alpha = 1.0;

	struct prop plane_props[] = {
		{ "CRTC_H", &plane->props.crtc_h, NULL, true },
		{ "CRTC_ID", &plane->props.crtc_id, NULL, true },
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

	printf("plane-id %"PRIu32" has type %"PRIu32"\n", plane_id, plane->type);
}

void plane_finish(struct plane *plane) {}

uint32_t plane_dumb_format(struct plane *plane) {
	uint32_t fb_fmt = DRM_FORMAT_INVALID;

	// We could use IN_FORMATS instead here, but it's not yet widely supported
	drmModePlane *drm_plane = drmModeGetPlane(plane->conn->dev->fd, plane->id);
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

bool plane_set_connector(struct plane *plane, struct connector *conn) {
	if (conn && (plane->possible_crtcs & (1 << conn->crtc_idx)) == 0) {
		return false;
	}

	plane->conn = conn;

	if (conn == NULL) {
		plane->width = plane->height = 0;
		return true;
	}

	switch (plane->type) {
	case DRM_PLANE_TYPE_OVERLAY:
		plane->width = plane->height = 100;
		break;
	case DRM_PLANE_TYPE_PRIMARY:
		plane->width = conn->width;
		plane->height = conn->height;
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

	return true;
}

static void plane_update(struct plane *plane, drmModeAtomicReq *req) {
	drmModeAtomicAddProperty(req, plane->id, plane->props.crtc_id, plane->conn->crtc_id);

	if (!plane->fb) {
		return;
	}

	drmModeAtomicAddProperty(req, plane->id, plane->props.fb_id, plane->fb->id);
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

	printf("dumb framebuffer initialized with fb-id %"PRIu32"\n", fb->fb.id);
}

void dumb_framebuffer_finish(struct dumb_framebuffer *fb) {
	munmap(fb->data, fb->size);
	fb->data = NULL;

	drmModeRmFB(fb->fb.dev->fd, fb->fb.id);
	fb->fb.id = 0;

	struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
	drmIoctl(fb->fb.dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}
