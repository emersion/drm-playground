#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <xf86drm.h>

#include "drm.h"
#include "util.h"

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

struct crtc *device_find_crtc(struct device *dev, uint32_t crtc_id) {
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
