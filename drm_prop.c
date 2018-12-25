#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dp_drm.h"
#include "util.h"

static int prop_cmp(const void *arg1, const void *arg2) {
	const char *key = arg1;
	const struct prop *val = arg2;

	return strcmp(key, val->name);
}

void read_obj_props(struct device *dev, uint32_t obj_id, uint32_t obj_type,
		struct prop *props, size_t props_len) {
	drmModeObjectProperties *obj_props =
		drmModeObjectGetProperties(dev->fd, obj_id, obj_type);
	if (!obj_props) {
		fatal_errno("drmModeObjectGetProperties failed");
	}

	bool seen[props_len + 1];
	memset(seen, false, props_len);
	for (uint32_t i = 0; i < obj_props->count_props; ++i) {
		drmModePropertyRes *prop =
			drmModeGetProperty(dev->fd, obj_props->props[i]);
		if (!prop) {
			fatal_errno("drmModeGetProperty failed");
		}

		struct prop *p = bsearch(prop->name, props, props_len,
			sizeof(*props), prop_cmp);
		if (p) {
			seen[p - props] = true;
			*p->dest = prop->prop_id;
			if (p->value) {
				*p->value = obj_props->prop_values[i];
			}
		}

		drmModeFreeProperty(prop);
	}

	for (size_t i = 0; i < props_len; ++i) {
		if (!seen[i] && props[i].required) {
			fatal("object is missing required property %s", props[i].name);
		}
	}

	drmModeFreeObjectProperties(obj_props);
}
