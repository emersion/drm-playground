#ifndef DP_H
#define DP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <drm_mode.h>
#include <xf86drmMode.h>

struct device;
struct connector;

struct framebuffer {
	struct device *dev;
	uint32_t id;
	uint32_t width, height;
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
	uint32_t x, y;
	uint32_t width, height;
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
	uint32_t width, height;
	bool active;

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

	drmModeModeInfo *modes;
	size_t modes_len;

	struct crtc *crtc; // can be NULL

	struct {
		uint32_t crtc_id;
	} props;

	drmModeCrtc *old_crtc;
};

struct device {
	int fd;
	drmModeAtomicReq *atomic_req;

	size_t connectors_len;
	struct connector *connectors;

	size_t crtcs_len;
	struct crtc *crtcs;

	size_t planes_len;
	struct plane *planes;
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
