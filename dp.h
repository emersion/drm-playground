#ifndef DP_H
#define DP_H

#include <stdint.h>

#include <drm_mode.h>
#include <gbm.h>
#include <xf86drmMode.h>

#define CONNECTORS_CAP 32
#define CRTCS_CAP 32
#define PLANES_CAP 64

struct device;
struct connector;

struct dumb_framebuffer {
	struct device *dev;

	uint32_t id; // DRM object ID
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t handle; // driver-specific handle
	uint64_t size; // size of mapping

	uint8_t *data; // mmapped data we can write to
};

struct plane {
	struct connector *conn;

	uint32_t id;
	uint32_t type;

	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;

	struct dumb_framebuffer fb;

	struct {
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

struct connector {
	struct device *dev;

	uint32_t id;
	uint32_t crtc_id;

	uint32_t mode_id;
	uint32_t width;
	uint32_t height;

	drmModeCrtc *old_crtc;

	struct {
		uint32_t crtc_id;
	} props;

	struct {
		uint32_t active;
		uint32_t mode_id;
	} crtc_props;

	drmModeAtomicReq *atomic;

	uint64_t start_ns;
	uint64_t curr_ns;

	size_t planes_len;
	struct plane planes[PLANES_CAP];
};

struct device {
	int fd;

	struct gbm_device *gbm;

	size_t connectors_len;
	struct connector connectors[CONNECTORS_CAP];
};

struct device *device_open(const char *path);
void device_destroy(struct device *dev);

void connector_init(struct connector *conn, struct device *dev,
	uint32_t conn_id);
void connector_finish(struct connector *conn);
void connector_commit(struct connector *conn, uint32_t flags);

void plane_init(struct plane *plane, struct connector *conn,
	uint32_t plane_id);
void plane_finish(struct plane *plane);
void plane_update(struct plane *plane, drmModeAtomicReq *req);

void dumb_framebuffer_init(struct dumb_framebuffer *fb, struct device *dev,
	uint32_t fmt, uint32_t width, uint32_t height);
void dumb_framebuffer_finish(struct dumb_framebuffer *fb);

#endif
