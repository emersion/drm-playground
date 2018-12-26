#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include "dp.h"
#include "util.h"

#include <stdio.h>

// Pick the CRTC with the maximum number of planes
static void pick_crtc(struct connector *conn) {
	struct device *dev = conn->dev;

	size_t planes_per_crtc[dev->crtcs_len + 1];
	memset(planes_per_crtc, 0, sizeof(planes_per_crtc));

	size_t best_crtc = 0;
	for (size_t i = 0; i < dev->planes_len; ++i) {
		struct plane *plane = &dev->planes[i];
		for (size_t j = 0; j < dev->crtcs_len; ++j) {
			if ((conn->possible_crtcs & (1 << j)) == 0) {
				continue;
			}
			if (plane->possible_crtcs & (1 << j)) {
				++planes_per_crtc[j];
				if (planes_per_crtc[j] > planes_per_crtc[best_crtc]) {
					best_crtc = j;
				}
			}
		}
	}

	connector_set_crtc(conn, &dev->crtcs[best_crtc]);
}

// Pick the preferred mode
static void pick_mode(struct connector *conn) {
	if (conn->modes_len == 0) {
		fatal("connector %"PRIu32" has no mode", conn->id);
	}
	drmModeModeInfo *mode = &conn->modes[0];
	for (size_t i = 0; i < conn->modes_len; ++i) {
		if (conn->modes[i].flags & DRM_MODE_TYPE_PREFERRED) {
			mode = &conn->modes[i];
			break;
		}
	}
	crtc_set_mode(conn->crtc, mode);
}

static uint32_t pick_rgb_format(struct plane *plane) {
	uint32_t fb_fmt = DRM_FORMAT_INVALID;

	for (uint32_t i = 0; i < plane->linear_formats_len; ++i) {
		uint32_t fmt = plane->linear_formats[i];
		switch (fmt) {
		case DRM_FORMAT_XRGB8888:
			fb_fmt = fmt;
			break;
		case DRM_FORMAT_ARGB8888:
			// Prefer formats with an alpha channel
			return fmt;
		}
	}

	return fb_fmt;
}

int main(int argc, char *argv[]) {
	const char *device_path = "/dev/dri/card0";
	if (argc == 2) {
		device_path = argv[1];
	}

	struct device dev = { 0 };
	device_init(&dev, device_path);

	if (dev.connectors_len == 0) {
		fatal("no connector");
	}
	if (dev.crtcs_len == 0) {
		fatal("no CRTC");
	}

	struct connector *conn = NULL;
	for (size_t i = 0; i < dev.connectors_len; ++i) {
		if (dev.connectors[i].type == DRM_MODE_CONNECTOR_WRITEBACK &&
				conn == NULL) {
			conn = &dev.connectors[i];
		} else {
			connector_set_crtc(&dev.connectors[i], NULL);
		}
	}
	if (conn == NULL) {
		fatal("failed to find a writeback connector");
	}

	pick_crtc(conn);
	pick_mode(conn);

	for (size_t i = 0; i < dev.crtcs_len; ++i) {
		struct crtc *crtc = &dev.crtcs[i];
		crtc->active = (conn->crtc == crtc);
		if (!crtc->active) {
			crtc_set_mode(crtc, NULL);
		}
	}

	device_commit(&dev, DRM_MODE_ATOMIC_ALLOW_MODESET);

	struct framebuffer_dumb fbs[dev.planes_len + 1];
	size_t fbs_len = 0;

	for (size_t i = 0; i < dev.planes_len; ++i) {
		struct plane *plane = &dev.planes[i];

		switch (plane->type) {
		case DRM_PLANE_TYPE_OVERLAY:
			plane->width = plane->height = 100;
			break;
		case DRM_PLANE_TYPE_PRIMARY:
			plane->width = conn->crtc->mode->hdisplay;
			plane->height = conn->crtc->mode->vdisplay;
			break;
		case DRM_PLANE_TYPE_CURSOR:
			// Some drivers *require* the FB to have exactly this size
			plane->width = dev.caps.cursor_width;
			plane->height = dev.caps.cursor_height;
			break;
		}

		uint32_t fb_fmt = pick_rgb_format(plane);
		if (fb_fmt == DRM_FORMAT_INVALID) {
			continue;
		}

		if (!plane_set_crtc(plane, conn->crtc)) {
			plane_set_crtc(plane, NULL);
			continue;
		}

		struct framebuffer_dumb *fb = &fbs[fbs_len];
		framebuffer_dumb_init(fb, &dev, fb_fmt, plane->width, plane->height);
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

	int x = 0;
	for (size_t i = 0; i < dev.planes_len; ++i) {
		struct plane *plane = &dev.planes[i];
		if (plane->crtc != conn->crtc) {
			continue;
		}

		struct framebuffer_dumb *fb = (struct framebuffer_dumb *)plane->fb;
		if (!fb) {
			continue;
		}

		if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
			x += 10;
			plane->x = x;
			plane->y = 2 * x;
		}
		plane->alpha = 0.5;

		const uint8_t *color = colors[i % colors_len];
		for (uint32_t y = 0; y < fb->fb.height; ++y) {
			uint8_t *row = (uint8_t *)fb->data + fb->stride * y;

			for (uint32_t x = 0; x < fb->fb.width; ++x) {
				row[x * 4 + 0] = color[0];
				row[x * 4 + 1] = color[1];
				row[x * 4 + 2] = color[2];
				row[x * 4 + 3] = 0x80;
			}
		}
	}

	struct framebuffer_dumb writeback_fb = {0};
	// TODO: pick format from writeback formats
	framebuffer_dumb_init(&writeback_fb, &dev, DRM_FORMAT_XRGB8888,
		conn->crtc->mode->hdisplay, conn->crtc->mode->vdisplay);

	int out_fence = -1;
	connector_set_writeback(conn, &writeback_fb.fb, &out_fence);

	crtc_commit(conn->crtc, DRM_MODE_ATOMIC_NONBLOCK, conn);

	struct pollfd pollfd = { .fd = out_fence, .events = POLLIN };

	while (true) {
		int ret = poll(&pollfd, 1, -1);
		if (ret < 0 && errno != EAGAIN) {
			fatal("poll failed");
		}

		if (pollfd.revents & POLLIN) {
			// TODO: write to file
			break;
		}
	}

	close(out_fence);
	framebuffer_dumb_finish(&writeback_fb);

	for (size_t i = 0; i < fbs_len; ++i) {
		framebuffer_dumb_finish(&fbs[i]);
	}

	device_finish(&dev);
	return EXIT_SUCCESS;
}