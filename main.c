#include <stdlib.h>
#include <time.h>

#include "dp.h"
#include "util.h"

int main(int argc, char *argv[]) {
	struct device *dev = device_open("/dev/dri/card0");

	drmModeRes *res = drmModeGetResources(dev->fd);
	if (!res) {
		fatal("drmModeGetResources failed");
	}

	if (res->count_connectors == 0) {
		fatal("no connector");
	}
	struct connector *conn = &dev->connectors[0];
	connector_init(conn, dev, res->connectors[0]);
	++dev->connectors_len;

	int crtc_idx = -1;
	for (int i = 0; i < res->count_crtcs; ++i) {
		if (res->crtcs[i] == conn->crtc_id) {
			crtc_idx = i;
			break;
		}
	}
	if (crtc_idx == -1) {
		fatal("failed to find CRTC in list");
	}

	drmModeFreeResources(res);

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res) {
		fatal("drmModeGetPlaneResources failed");
	}

	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		drmModePlane *drm_plane =
			drmModeGetPlane(dev->fd, plane_res->planes[i]);
		if (!drm_plane) {
			fatal("drmModeGetPlane failed");
		}

		if (drm_plane->possible_crtcs & (1 << crtc_idx)) {
			plane_init(&conn->planes[conn->planes_len], conn,
				plane_res->planes[i]);
			++conn->planes_len;
		}

		drmModeFreePlane(drm_plane);
	}

	drmModeFreePlaneResources(plane_res);

	// B G R
	const uint8_t colors[][3] = {
		{ 0xFF, 0x00, 0x00 },
		{ 0x00, 0xFF, 0x00 },
		{ 0x00, 0x00, 0xFF },
	};
	const size_t colors_len = sizeof(colors) / sizeof(colors[0]);

	for (size_t i = 0; i < conn->planes_len; ++i) {
		const uint8_t *color = colors[i % colors_len];

		struct plane *plane = &conn->planes[i];
		struct dumb_framebuffer *fb = &plane->fb;

		plane->x = i * 10;
		plane->y = i * 20;
		plane->alpha = 0.5;

		for (uint32_t y = 0; y < fb->height; ++y) {
			uint8_t *row = fb->data + fb->stride * y;

			for (uint32_t x = 0; x < fb->width; ++x) {
				row[x * 4 + 0] = color[0];
				row[x * 4 + 1] = color[1];
				row[x * 4 + 2] = color[2];
				row[x * 4 + 3] = 0xF0;
			}
		}
	}

	connector_commit(conn, DRM_MODE_ATOMIC_NONBLOCK);

	for (int i = 0; i < 60 * 5; ++i) {
		struct timespec ts = { .tv_nsec = 16666667 };
		nanosleep(&ts, NULL);
	}

	device_destroy(dev);
	return EXIT_SUCCESS;
}
