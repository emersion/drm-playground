#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include "dp.h"
#include "util.h"

#include <stdio.h>

static bool running = true;

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
	crtc_set_mode(conn->pending.crtc, mode);
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

static int n_page_flips = 0;
static bool to_right = true;

static void handle_page_flip(int drm_fd, unsigned sequence, unsigned tv_sec,
		unsigned tv_usec, void *data) {
	struct connector *conn = data;
	struct device *dev = conn->dev;

	++n_page_flips;
	if (n_page_flips > 60 * 5) {
		running = false;
		return;
	}

	if (n_page_flips % 60 == 0) {
		to_right = !to_right;
	}

	int delta = to_right ? 1 : -1;
	int x = 0;
	for (size_t j = 0; j < dev->planes_len; ++j) {
		struct plane *plane = &dev->planes[j];
		if (plane->pending.crtc != conn->pending.crtc) {
			continue;
		}

		if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
			x += delta;
			plane->pending.x += x;
			plane->pending.y += delta;
		}
	}

	crtc_commit(conn->pending.crtc,
		DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, conn);
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
		if (dev.connectors[i].state == DRM_MODE_CONNECTED && conn == NULL) {
			conn = &dev.connectors[i];
		} else {
			connector_set_crtc(&dev.connectors[i], NULL);
		}
	}
	if (conn == NULL) {
		fatal("failed to find a connected connector");
	}

	pick_crtc(conn);
	pick_mode(conn);

	for (size_t i = 0; i < dev.crtcs_len; ++i) {
		dev.crtcs[i].pending.active = (conn->pending.crtc == &dev.crtcs[i]);
	}

	printf("setting modes on device\n");
	device_commit(&dev, DRM_MODE_ATOMIC_ALLOW_MODESET);

	struct dumb_framebuffer fbs[dev.planes_len + 1];
	size_t fbs_len = 0;

	for (size_t i = 0; i < dev.planes_len; ++i) {
		struct plane *plane = &dev.planes[i];

		switch (plane->type) {
		case DRM_PLANE_TYPE_OVERLAY:
			plane->pending.width = plane->pending.height = 100;
			break;
		case DRM_PLANE_TYPE_PRIMARY:;
			const drmModeModeInfo *mode = conn->pending.crtc->pending.mode;
			plane->pending.width = mode->hdisplay;
			plane->pending.height = mode->vdisplay;
			break;
		case DRM_PLANE_TYPE_CURSOR:
			// Some drivers *require* the FB to have exactly this size
			plane->pending.width = dev.caps.cursor_width;
			plane->pending.height = dev.caps.cursor_height;
			break;
		}

		uint32_t fb_fmt = pick_rgb_format(plane);
		if (fb_fmt == DRM_FORMAT_INVALID) {
			continue;
		}

		if (!plane_set_crtc(plane, conn->pending.crtc)) {
			plane_set_crtc(plane, NULL);
			continue;
		}

		struct dumb_framebuffer *fb = &fbs[fbs_len];
		dumb_framebuffer_init(fb, &dev, fb_fmt,
			plane->pending.width, plane->pending.height);
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
		if (plane->pending.crtc != conn->pending.crtc) {
			continue;
		}

		struct dumb_framebuffer *fb =
			(struct dumb_framebuffer *)plane->pending.fb;
		if (!fb) {
			continue;
		}

		if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
			x += 10;
			plane->pending.x = x;
			plane->pending.y = 2 * x;
		}
		plane->pending.alpha = 0.5;

		const uint8_t *color = colors[i % colors_len];
		for (uint32_t y = 0; y < fb->fb.height; ++y) {
			uint8_t *row = fb->data + fb->stride * y;

			for (uint32_t x = 0; x < fb->fb.width; ++x) {
				row[x * 4 + 0] = color[0];
				row[x * 4 + 1] = color[1];
				row[x * 4 + 2] = color[2];
				row[x * 4 + 3] = 0x80;
			}
		}
	}

	printf("starting rendering\n");
	crtc_commit(conn->pending.crtc,
		DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, conn);

	struct pollfd pollfd = {
		.fd = dev.fd,
		.events = POLLIN,
	};

	while (running) {
		int ret = poll(&pollfd, 1, -1);
		if (ret < 0 && errno != EAGAIN) {
			fatal("poll failed");
		}

		if (pollfd.revents & POLLIN) {
			drmEventContext context = {
				.version = 2,
				.page_flip_handler = handle_page_flip,
			};

			if (drmHandleEvent(dev.fd, &context) < 0) {
				fatal_errno("drmHandleEvent failed");
			}
		}
	}

	for (size_t i = 0; i < fbs_len; ++i) {
		dumb_framebuffer_finish(&fbs[i]);
	}

	device_finish(&dev);
	return EXIT_SUCCESS;
}
