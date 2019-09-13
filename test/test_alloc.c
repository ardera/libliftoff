#include <assert.h>
#include <unistd.h>
#include <libliftoff.h>
#include <stdio.h>
#include <string.h>
#include "libdrm_mock.h"

static struct liftoff_layer *add_layer(struct liftoff_output *output,
				       int x, int y, int width, int height)
{
	uint32_t fb_id;
	struct liftoff_layer *layer;

	layer = liftoff_layer_create(output);
	fb_id = liftoff_mock_drm_create_fb(layer);
	liftoff_layer_set_property(layer, "FB_ID", fb_id);
	liftoff_layer_set_property(layer, "CRTC_X", x);
	liftoff_layer_set_property(layer, "CRTC_Y", y);
	liftoff_layer_set_property(layer, "CRTC_W", width);
	liftoff_layer_set_property(layer, "CRTC_H", height);
	liftoff_layer_set_property(layer, "SRC_X", 0);
	liftoff_layer_set_property(layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer, "SRC_W", width << 16);
	liftoff_layer_set_property(layer, "SRC_H", height << 16);

	return layer;
}

struct test_plane {
	int type;
};

struct test_layer {
	int x, y, width, height;
	struct test_plane *compat[64];
	struct test_plane *result;
};

struct test_case {
	const char *name;
	struct test_layer layers[64];
};

static struct test_plane test_setup[] = {
	{ .type = DRM_PLANE_TYPE_PRIMARY },
	{ .type = DRM_PLANE_TYPE_OVERLAY },
	{ .type = DRM_PLANE_TYPE_OVERLAY },
	{ .type = DRM_PLANE_TYPE_CURSOR },
};

static const size_t test_setup_len = sizeof(test_setup) / sizeof(test_setup[0]);

static struct test_case tests[] = {
	{
		.name = "primary-nomatch",
		.layers = {
			{
				.width = 1920,
				.height = 1080,
				.compat = { NULL },
				.result = NULL,
			},
		},
	},
	{
		.name = "primary-match",
		.layers = {
			{
				.width = 1920,
				.height = 1080,
				.compat = { &test_setup[0] },
				.result = &test_setup[0],
			},
		},
	},
};

static void run_test(struct test_layer *test_layers)
{
	size_t i, j;
	ssize_t plane_index_got, plane_index_want;
	struct liftoff_mock_plane *mock_planes[64];
	struct liftoff_mock_plane *mock_plane;
	struct test_layer *test_layer;
	int drm_fd;
	struct liftoff_display *display;
	struct liftoff_output *output;
	struct liftoff_layer *layers[64];
	drmModeAtomicReq *req;
	bool ok;
	uint32_t plane_id;

	for (i = 0; i < test_setup_len; i++) {
		mock_planes[i] = liftoff_mock_drm_create_plane(test_setup[i].type);
	}

	drm_fd = liftoff_mock_drm_open();
	display = liftoff_display_create(drm_fd);
	assert(display != NULL);

	output = liftoff_output_create(display, liftoff_mock_drm_crtc_id);
	for (i = 0; test_layers[i].width > 0; i++) {
		test_layer = &test_layers[i];
		layers[i] = add_layer(output, test_layer->x, test_layer->y,
				      test_layer->width, test_layer->height);
		for (j = 0; test_layer->compat[j] != NULL; j++) {
			mock_plane = mock_planes[test_layer->compat[j] -
						 test_setup];
			liftoff_mock_plane_add_compatible_layer(mock_plane,
								layers[i]);
		}
	}

	req = drmModeAtomicAlloc();
	ok = liftoff_display_apply(display, req);
	assert(ok);
	drmModeAtomicFree(req);

	for (i = 0; test_layers[i].width > 0; i++) {
		plane_id = liftoff_layer_get_plane_id(layers[i]);
		mock_plane = NULL;
		if (plane_id != 0) {
			mock_plane = liftoff_mock_drm_get_plane(plane_id);
		}
		plane_index_got = -1;
		for (j = 0; j < test_setup_len; j++) {
			if (mock_planes[j] == mock_plane) {
				plane_index_got = j;
				break;
			}
		}
		assert(mock_plane == NULL || plane_index_got >= 0);

		fprintf(stderr, "layer %zu got assigned to plane %d\n",
			i, (int)plane_index_got);

		plane_index_want = -1;
		if (test_layers[i].result != NULL) {
			plane_index_want = test_layers[i].result - test_setup;
		}

		if (plane_index_got != plane_index_want) {
			fprintf(stderr, "  ERROR: want plane %d\n",
				(int)plane_index_want);
			ok = false;
		}
	}
	assert(ok);

	liftoff_display_destroy(display);
	close(drm_fd);
}

static void test_basic(void)
{
	struct liftoff_mock_plane *mock_plane;
	int drm_fd;
	struct liftoff_display *display;
	struct liftoff_output *output;
	struct liftoff_layer *layer;
	drmModeAtomicReq *req;
	bool ok;

	mock_plane = liftoff_mock_drm_create_plane(DRM_PLANE_TYPE_PRIMARY);

	drm_fd = liftoff_mock_drm_open();
	display = liftoff_display_create(drm_fd);
	assert(display != NULL);

	output = liftoff_output_create(display, liftoff_mock_drm_crtc_id);
	layer = add_layer(output, 0, 0, 1920, 1080);

	liftoff_mock_plane_add_compatible_layer(mock_plane, layer);

	req = drmModeAtomicAlloc();
	ok = liftoff_display_apply(display, req);
	assert(ok);
	assert(liftoff_mock_plane_get_layer(mock_plane, req) == layer);
	drmModeAtomicFree(req);

	liftoff_display_destroy(display);
	close(drm_fd);
}

int main(int argc, char *argv[]) {
	const char *test_name;
	size_t i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <test-name>\n", argv[0]);
		return 1;
	}
	test_name = argv[1];

	if (strcmp(test_name, "basic") == 0) {
		test_basic();
		return 0;
	}

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (strcmp(tests[i].name, test_name) == 0) {
			run_test(tests[i].layers);
			return 0;
		}
	}

	fprintf(stderr, "no such test: %s\n", test_name);
	return 1;
}
