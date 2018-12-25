#ifndef DP_DRM_H
#define DP_DRM_H

#include <xf86drmMode.h>

#include "dp.h"

struct encoder {
	uint32_t id;
	uint32_t possible_crtcs;
};

struct prop {
	const char *name;
	uint32_t *dest;
	uint32_t *value;
	bool required;
};

void read_obj_props(struct device *dev, uint32_t obj_id, uint32_t obj_type,
	struct prop *props, size_t props_len);

struct crtc *device_find_crtc(struct device *dev, uint32_t crtc_id);

void connector_init(struct connector *conn, struct device *dev,
	uint32_t conn_id, struct encoder *encoders, size_t encoders_len);
void connector_finish(struct connector *conn);
void connector_update(struct connector *conn, drmModeAtomicReq *req);

void crtc_init(struct crtc *crtc, struct device *dev, uint32_t crtc_id);
void crtc_finish(struct crtc *crtc);
void crtc_update(struct crtc *crtc, drmModeAtomicReq *req);

void plane_init(struct plane *plane, struct device *dev,
	uint32_t plane_id);
void plane_finish(struct plane *plane);
void plane_update(struct plane *plane, drmModeAtomicReq *req);

#endif
