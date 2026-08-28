/* Exercise actual legacy public-Mir screencast format negotiation. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct MirConnection MirConnection;
typedef struct MirScreencastSpec MirScreencastSpec;
typedef struct MirScreencast MirScreencast;
typedef struct MirBufferStream MirBufferStream;
typedef struct { int left, top; unsigned int width, height; } MirRectangle;
typedef struct { int width, height, stride, pixel_format; void *vaddr; } MirGraphicsRegion;

struct api {
	void *library;
	MirConnection *(*connect_sync)(char const *, char const *);
	int (*connection_is_valid)(MirConnection *);
	void (*connection_release)(MirConnection *);
	MirScreencastSpec *(*create_spec)(MirConnection *);
	void (*set_width)(MirScreencastSpec *, unsigned int);
	void (*set_height)(MirScreencastSpec *, unsigned int);
	void (*set_format)(MirScreencastSpec *, int);
	void (*set_region)(MirScreencastSpec *, MirRectangle const *);
	void (*set_buffers)(MirScreencastSpec *, unsigned int);
	void (*set_mirror)(MirScreencastSpec *, int);
	MirScreencast *(*create)(MirScreencastSpec *);
	void (*release_spec)(MirScreencastSpec *);
	MirBufferStream *(*get_stream)(MirScreencast *);
	void (*release)(MirScreencast *);
	void (*get_region)(MirBufferStream *, MirGraphicsRegion *);
};

#define LOAD(field, name) do { *(void **)(&a.field) = dlsym(a.library, name); \
	if (!a.field) { fprintf(stderr, "missing %s\n", name); return 1; } } while (0)

int main(int argc, char **argv)
{
	static char const *names[] = {"invalid", "abgr8888", "xbgr8888", "argb8888",
		"xrgb8888", "bgr888", "rgb888", "rgb565"};
	int requested = argc > 1 ? atoi(argv[1]) : 1;
	struct api a = {0};
	MirConnection *connection;
	MirScreencastSpec *spec;
	MirScreencast *cast;
	MirBufferStream *stream;
	MirGraphicsRegion region = {0};
	MirRectangle rectangle = {1080, 0, 1920, 1080};
	if (requested < 1 || requested > 7) return 2;
	a.library = dlopen("libmirclient.so.9", RTLD_NOW | RTLD_LOCAL);
	if (!a.library) return 1;
	LOAD(connect_sync, "mir_connect_sync");
	LOAD(connection_is_valid, "mir_connection_is_valid");
	LOAD(connection_release, "mir_connection_release");
	LOAD(create_spec, "mir_create_screencast_spec");
	LOAD(set_width, "mir_screencast_spec_set_width");
	LOAD(set_height, "mir_screencast_spec_set_height");
	LOAD(set_format, "mir_screencast_spec_set_pixel_format");
	LOAD(set_region, "mir_screencast_spec_set_capture_region");
	LOAD(set_buffers, "mir_screencast_spec_set_number_of_buffers");
	LOAD(set_mirror, "mir_screencast_spec_set_mirror_mode");
	LOAD(create, "mir_screencast_create_sync");
	LOAD(release_spec, "mir_screencast_spec_release");
	LOAD(get_stream, "mir_screencast_get_buffer_stream");
	LOAD(release, "mir_screencast_release_sync");
	LOAD(get_region, "mir_buffer_stream_get_graphics_region");
	connection = a.connect_sync("/run/mir_socket", "mir-format-audit");
	if (!connection || !a.connection_is_valid(connection)) return 1;
	spec = a.create_spec(connection);
	a.set_width(spec, 1920); a.set_height(spec, 1080);
	a.set_format(spec, requested); a.set_region(spec, &rectangle);
	a.set_mirror(spec, 1); a.set_buffers(spec, 2);
	cast = a.create(spec);
	a.release_spec(spec);
	if (!cast) {
		printf("requested=%d requested_name=%s accepted=no\n", requested, names[requested]);
		return 0;
	}
	stream = a.get_stream(cast);
	if (stream) a.get_region(stream, &region);
	printf("requested=%d requested_name=%s accepted=%s actual=%d actual_name=%s width=%d "
	       "height=%d stride=%d bytes_per_pixel=%d mapped=%s\n", requested,
		names[requested], stream && region.vaddr ? "yes" : "no", region.pixel_format,
		region.pixel_format >= 1 && region.pixel_format <= 7 ? names[region.pixel_format] : "unknown",
		region.width, region.height, region.stride,
		region.width ? region.stride / region.width : 0, region.vaddr ? "yes" : "no");
	fflush(NULL);
	/* Some target releases hang while releasing an actively consumed cast. */
	_exit(0);
}
