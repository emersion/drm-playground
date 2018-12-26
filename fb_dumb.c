#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include "dp.h"
#include "util.h"

void framebuffer_dumb_init(struct framebuffer_dumb *fb, struct device *dev,
		uint32_t fmt, uint32_t width, uint32_t height) {
	int ret;

	printf("initializing dumb framebuffer with format %"PRIu32" and "
		"size %"PRIu32"x%"PRIu32"\n", fmt, width, height);

	if (!dev->caps.dumb) {
		fatal("DRM device doesn't support dumb frambuffers");
	}

	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		break;
	default:
		fatal("format %"PRIu32" not supported");
	}

	struct drm_mode_create_dumb create = {
		.width = width,
		.height = height,
		.bpp = 32,
		.flags = 0,
	};
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret < 0) {
		fatal("DRM_IOCTL_MODE_CREATE_DUMB failed");
	}

	fb->fb.dev = dev;
	fb->fb.width = width;
	fb->fb.height = height;
	fb->stride = create.pitch;
	fb->handle = create.handle;
	fb->size = create.size;

	uint32_t handles[4] = { fb->handle };
	uint32_t strides[4] = { fb->stride };
	uint32_t offsets[4] = { 0 };
	ret = drmModeAddFB2(dev->fd, width, height, fmt, handles, strides, offsets,
		&fb->fb.id, 0);
	if (ret < 0) {
		fatal("drmModeAddFB2 failed");
	}

	void *data = NULL;
	framebuffer_dumb_map(fb, PROT_WRITE, &data);
	memset(data, 0xFF, fb->size);
	framebuffer_dumb_unmap(fb, data);

	printf("dumb framebuffer %"PRIu32" initialized\n", fb->fb.id);
}

void framebuffer_dumb_finish(struct framebuffer_dumb *fb) {
	drmModeRmFB(fb->fb.dev->fd, fb->fb.id);
	fb->fb.id = 0;

	struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
	drmIoctl(fb->fb.dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

void framebuffer_dumb_map(struct framebuffer_dumb *fb, uint32_t flags,
		void **data_ptr) {
	struct drm_mode_map_dumb map = { .handle = fb->handle };
	int ret = drmIoctl(fb->fb.dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret < 0) {
		fatal("DRM_IOCTL_MODE_MAP_DUMB failed");
	}

	void *data = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		fb->fb.dev->fd, map.offset);
	if (!data) {
		fatal("mmap failed");
	}

	*data_ptr = data;
}

void framebuffer_dumb_unmap(struct framebuffer_dumb *fb, void *data) {
	munmap(data, fb->size);
}
