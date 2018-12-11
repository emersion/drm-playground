#include <stdlib.h>
#include <time.h>

#include <drm_fourcc.h>

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

	drmModeFreeResources(res);

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res) {
		fatal("drmModeGetPlaneResources failed");
	}

	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		struct plane *plane = &dev->planes[dev->planes_len];
		plane_init(plane, dev, plane_res->planes[i]);
		++dev->planes_len;
	}

	drmModeFreePlaneResources(plane_res);

	struct dumb_framebuffer fbs[PLANES_CAP];
	size_t fbs_len = 0;

	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];

		if (!plane_set_connector(plane, conn)) {
			continue;
		}

		uint32_t fb_fmt = plane_dumb_format(plane);
		if (fb_fmt == DRM_FORMAT_INVALID) {
			continue;
		}

		struct dumb_framebuffer *fb = &fbs[fbs_len];
		dumb_framebuffer_init(fb, dev, fb_fmt,
			plane->width, plane->height);
		++fbs_len;

		plane_set_framebuffer(plane, &fb->fb);
	}

	// B G R
	const uint8_t colors[][3] = {
		{ 0xFF, 0x00, 0x00 },
		{ 0x00, 0xFF, 0x00 },
		{ 0x00, 0x00, 0xFF },
	};
	const size_t colors_len = sizeof(colors) / sizeof(colors[0]);

	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];
		if (plane->conn != conn) {
			continue;
		}

		struct dumb_framebuffer *fb = (struct dumb_framebuffer *)plane->fb;
		if (!fb) {
			continue;
		}

		plane->x = i * 10;
		plane->y = i * 20;
		plane->alpha = 0.5;

		const uint8_t *color = colors[i % colors_len];
		for (uint32_t y = 0; y < fb->fb.height; ++y) {
			uint8_t *row = fb->data + fb->stride * y;

			for (uint32_t x = 0; x < fb->fb.width; ++x) {
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

	for (size_t i = 0; i < fbs_len; ++i) {
		dumb_framebuffer_finish(&fbs[i]);
	}

	device_destroy(dev);
	return EXIT_SUCCESS;
}
