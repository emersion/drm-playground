#ifndef DP_H
#define DP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <drm_mode.h>
#include <gbm.h>
#include <xf86drmMode.h>

#define CONNECTORS_CAP 32
#define CRTCS_CAP 32
#define PLANES_CAP 64

struct device;
struct connector;

struct framebuffer {
	struct device *dev;
	uint32_t id;
	uint32_t width;
	uint32_t height;
};

struct dumb_framebuffer {
	struct framebuffer fb;

	uint32_t stride;
	uint32_t handle; // driver-specific handle
	uint64_t size; // size of mapping

	uint8_t *data; // mmapped data we can write to
};

struct plane {
	struct device *dev;
	uint32_t id;
	uint32_t type;
	uint32_t possible_crtcs;

	struct crtc *crtc; // can be NULL
	struct framebuffer *fb; // can be NULL
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	float alpha;

	struct {
		uint32_t alpha;
		uint32_t crtc_h;
		uint32_t crtc_id;
		uint32_t crtc_w;
		uint32_t crtc_x;
		uint32_t crtc_y;
		uint32_t fb_id;
		uint32_t src_h;
		uint32_t src_w;
		uint32_t src_x;
		uint32_t src_y;
		uint32_t type;
	} props;
};

struct crtc {
	struct device *dev;
	uint32_t id;

	uint32_t mode_id;
	uint32_t width;
	uint32_t height;

	struct {
		uint32_t active;
		uint32_t mode_id;
	} props;
};

struct connector {
	struct device *dev;
	uint32_t id;
	drmModeConnection state;
	uint32_t possible_crtcs;

	struct crtc *crtc; // can be NULL

	struct {
		uint32_t crtc_id;
	} props;

	drmModeCrtc *old_crtc;
};

struct device {
	int fd;
	struct gbm_device *gbm;
	drmModeAtomicReq *atomic_req;

	size_t connectors_len;
	struct connector connectors[CONNECTORS_CAP];

	size_t crtcs_len;
	struct crtc crtcs[CRTCS_CAP];

	size_t planes_len;
	struct plane planes[PLANES_CAP];
};

void device_init(struct device *dev, const char *path);
void device_finish(struct device *dev);
void device_commit(struct device *dev, uint32_t flags);

bool connector_set_crtc(struct connector *conn, struct crtc *crtc);

void crtc_set_mode(struct crtc *crtc, drmModeModeInfo *mode);

uint32_t plane_dumb_format(struct plane *plane);
void plane_set_framebuffer(struct plane *plane, struct framebuffer *fb);
bool plane_set_crtc(struct plane *plane, struct crtc *crtc);

void dumb_framebuffer_init(struct dumb_framebuffer *fb, struct device *dev,
	uint32_t fmt, uint32_t width, uint32_t height);
void dumb_framebuffer_finish(struct dumb_framebuffer *fb);

#endif
