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

struct framebuffer_dumb {
	struct framebuffer fb;

	uint32_t stride;
	uint32_t handle; // driver-specific handle
	uint64_t size; // size of mapping
};

struct plane {
	struct device *dev;
	uint32_t id;
	uint32_t type;
	uint32_t possible_crtcs;

	uint32_t *linear_formats;
	size_t linear_formats_len;

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

	drmModeModeInfo *mode;
	uint32_t mode_id;
	bool active;

	struct {
		uint32_t active;
		uint32_t mode_id;
	} props;
};

struct connector {
	struct device *dev;
	uint32_t id;
	uint32_t type;
	uint32_t possible_crtcs;
	drmModeConnection state;

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

	struct {
		bool dumb;
		uint32_t cursor_width, cursor_height;
	} caps;

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

void crtc_commit(struct crtc *crtc, uint32_t flags, void *user_data);
void crtc_set_mode(struct crtc *crtc, const drmModeModeInfo *mode);

void plane_set_framebuffer(struct plane *plane, struct framebuffer *fb);
bool plane_set_crtc(struct plane *plane, struct crtc *crtc);

void framebuffer_dumb_init(struct framebuffer_dumb *fb, struct device *dev,
	uint32_t fmt, uint32_t width, uint32_t height);
void framebuffer_dumb_finish(struct framebuffer_dumb *fb);
void framebuffer_dumb_map(struct framebuffer_dumb *fb, uint32_t flags,
	void **data_ptr);
void framebuffer_dumb_unmap(struct framebuffer_dumb *fb, void *data);

#endif
