#include <iostream>
#include <fstream>
#include <string>
#include <zlib.h>
#include "vector_tile.pb.h"

#define XMAX 4096
#define YMAX 4096

extern "C" {
	#include "graphics.h"
	#include "clip.h"
}

struct line {
	int x0;
	int y0;
	int x1;
	int y1;
};

struct point {
	int x;
	int y;
};

int linecmp(const void *v1, const void *v2) {
	const struct line *l1 = (const struct line *) v1;
	const struct line *l2 = (const struct line *) v2;

	if (l1->x0 != l2->x0) {
		return l1->x0 - l2->x0;
	}
	if (l1->y0 != l2->y0) {
		return l1->y0 - l2->y0;
	}

	if (l1->x1 != l2->x1) {
		return l1->x1 - l2->x1;
	}

	return l1->y1 - l2->y1;
}

int startcmp(const void *v1, const void *v2) {
	const struct line *l1 = (const struct line *) v1;
	const struct line *l2 = (const struct line *) v2;

	if (l1->x0 != l2->x0) {
		return l1->x0 - l2->x0;
	}

	return l1->y0 - l2->y0;
}

class env {
public:
	mapnik::vector::tile tile;
	mapnik::vector::tile_layer *layer;
	mapnik::vector::tile_feature *feature;

	int x;
	int y;

	int cmd_idx;
	int cmd;
	int length;

	struct line *lines;
	int nlines;
	int nlalloc;

	struct point *points;
	int npoints;
	int npalloc;
};

#define MOVE_TO 1
#define LINE_TO 2
#define CLOSE_PATH 7
#define CMD_BITS 3

struct graphics {
	int width;
	int height;
	env *e;
};

struct graphics *graphics_init(int width, int height) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	env *e = new env;

	e->nlalloc = 1024;
	e->nlines = 0;
	e->lines = (struct line *) malloc(e->nlalloc * sizeof(struct line));

	e->npalloc = 1024;
	e->npoints = 0;
	e->points = (struct point *) malloc(e->npalloc * sizeof(struct point));

	struct graphics *g = (struct graphics *) malloc(sizeof(struct graphics));
	g->e = e;
	g->width = width;
	g->height = height;

	return g;
}

// from mapnik-vector-tile/src/vector_tile_compression.hpp
static inline int compress(std::string const& input, std::string & output)
{
	z_stream deflate_s;
	deflate_s.zalloc = Z_NULL;
	deflate_s.zfree = Z_NULL;
	deflate_s.opaque = Z_NULL;
	deflate_s.avail_in = 0;
	deflate_s.next_in = Z_NULL;
	deflateInit(&deflate_s, Z_DEFAULT_COMPRESSION);
	deflate_s.next_in = (Bytef *)input.data();
	deflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		size_t increase = input.size() / 2 + 1024;
		output.resize(length + increase);
		deflate_s.avail_out = increase;
		deflate_s.next_out = (Bytef *)(output.data() + length);
		int ret = deflate(&deflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			return -1;
		}
		length += (increase - deflate_s.avail_out);
	} while (deflate_s.avail_out == 0);
	deflateEnd(&deflate_s);
	output.resize(length);
	return 0;
}

static void op(env *e, int cmd, int x, int y);

void out(struct graphics *gc, int transparency, double gamma, int invert, int color, int color2, int saturate, int mask) {
	env *e = gc->e;

#ifdef CHAIN
	qsort(e->lines, e->nlines, sizeof(struct line), linecmp);
#endif

	e->layer = e->tile.add_layers();
	e->layer->set_name("lines");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	e->feature = e->layer->add_features();
	e->feature->set_type(mapnik::vector::tile::LineString);

	e->x = 0;
	e->y = 0;

	e->cmd_idx = -1;
	e->cmd = -1;
	e->length = 0;

	int i;
	for (i = 0; i < e->nlines; i++) {
		// printf("draw %d %d to %d %d\n", e->lines[i].x0, e->lines[i].y0, e->lines[i].x1, e->lines[i].y1);

		if (e->lines[i].x0 != e->x || e->lines[i].y0 != e->y || e->length == 0) {
			op(e, MOVE_TO, e->lines[i].x0, e->lines[i].y0);
		}

		op(e, LINE_TO, e->lines[i].x1, e->lines[i].y1);

#ifdef CHAIN
		struct line l2;
		l2.x0 = e->lines[i].x1;
		l2.y0 = e->lines[i].y1;

		while (i < e->nlines) {
			// printf("looking for %d,%d\n", l2.x0, l2.y0);
			// printf("searching %d\n", e->nlines - i);

			struct line *next = (struct line *) bsearch(&l2, e->lines + i, e->nlines - i,
						sizeof(struct line), startcmp);

			if (next != NULL) {
				// printf("found %d,%d to %d,%d at %d\n", next->x0, next->y0, next->x1, next->y1, (int) (next - e->lines));

				op(e, LINE_TO, next->x1, next->y1);

				l2.x0 = next->x1;
				l2.y0 = next->y1;

				int n = next - e->lines;

				memmove(e->lines + n, e->lines + n + 1, (e->nlines - (n + 1)) * sizeof(struct line));
				e->nlines--;
			} else {
				break;
			}
		}
#endif
	}

	if (e->cmd_idx >= 0) {
		//printf("old command: %d %d\n", e->cmd, e->length);
		e->feature->set_geometry(e->cmd_idx, 
			(e->length << CMD_BITS) |
			(e->cmd & ((1 << CMD_BITS) - 1)));
	}

	//////////////////////////////////

	e->layer = e->tile.add_layers();
	e->layer->set_name("polygons");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	e->feature = e->layer->add_features();
	e->feature->set_type(mapnik::vector::tile::Polygon);

	e->x = 0;
	e->y = 0;

	e->cmd_idx = -1;
	e->cmd = -1;
	e->length = 0;

	for (i = 0; i < e->npoints; i++) {
		op(e, MOVE_TO, e->points[i].x - 1, e->points[i].y);
		op(e, LINE_TO, e->points[i].x, e->points[i].y - 1);
		op(e, LINE_TO, e->points[i].x + 1, e->points[i].y);
		op(e, LINE_TO, e->points[i].x, e->points[i].y + 1);
		op(e, CLOSE_PATH, 0, 0);
	}

	if (e->cmd_idx >= 0) {
		e->feature->set_geometry(e->cmd_idx, 
			(e->length << CMD_BITS) |
			(e->cmd & ((1 << CMD_BITS) - 1)));
	}

	//////////////////////////////////

	std::string s;
	e->tile.SerializeToString(&s);

	std::string compressed;
	compress(s, compressed);

	std::cout << compressed;
}

static void op(env *e, int cmd, int x, int y) {
	// printf("%d %d,%d\n", cmd, x, y);
	// printf("from cmd %d to %d\n", e->cmd, cmd);

	if (cmd != e->cmd) {
		if (e->cmd_idx >= 0) {
			// printf("old command: %d %d\n", e->cmd, e->length);
			e->feature->set_geometry(e->cmd_idx, 
				(e->length << CMD_BITS) |
				(e->cmd & ((1 << CMD_BITS) - 1)));
		}

		e->cmd = cmd;
		e->length = 0;
		e->cmd_idx = e->feature->geometry_size();

		e->feature->add_geometry(0); // placeholder
	}

	if (cmd == MOVE_TO || cmd == LINE_TO) {
		int dx = x - e->x;
		int dy = y - e->y;
		// printf("new geom: %d %d\n", x, y);

		e->feature->add_geometry((dx << 1) ^ (dx >> 31));
		e->feature->add_geometry((dy << 1) ^ (dy >> 31));
		
		e->x = x;
		e->y = y;
		e->length++;
	} else if (cmd == CLOSE_PATH) {
		e->length++;
	}
}

int drawClip(double x0, double y0, double x1, double y1, struct graphics *gc, double bright, double hue, int antialias, double thick, struct tilecontext *tc) {
	double mult = XMAX / gc->width;
	int accept = clip(&x0, &y0, &x1, &y1, 0, 0, XMAX / mult, YMAX / mult);

	if (accept) {
		int xx0 = x0 * mult;
		int yy0 = y0 * mult;
		int xx1 = x1 * mult;
		int yy1 = y1 * mult;

		// Guarding against rounding error

		if (xx0 < 0) {
			xx0 = 0;
		}
		if (xx0 > XMAX - 1) {
			xx0 = XMAX - 1;
		}
		if (yy0 < 0) {
			yy0 = 0;
		}
		if (yy0 > YMAX - 1) {
			yy0 = YMAX - 1;
		}

		if (xx1 < 0) {
			xx1 = 0;
		}
		if (xx1 > XMAX - 1) {
			xx1 = XMAX - 1;
		}
		if (yy1 < 0) {
			yy1 = 0;
		}
		if (yy1 > YMAX - 1) {
			yy1 = YMAX - 1;
		}

		env *e = gc->e;

		if (xx0 != xx1 || yy0 != yy1) {
			if (e->nlines + 1 >= e->nlalloc) {
				e->nlalloc *= 2;
				e->lines = (struct line *) realloc((void *) e->lines, e->nlalloc * sizeof(struct line));
			}

			e->lines[e->nlines].x0 = xx0;
			e->lines[e->nlines].y0 = yy0;
			e->lines[e->nlines].x1 = xx1;
			e->lines[e->nlines].y1 = yy1;

			e->nlines++;
		}
	}

	return 0;
}

void drawPixel(double x, double y, struct graphics *gc, double bright, double hue, struct tilecontext *tc) {
	double mult = XMAX / gc->width;
	int xx = x * mult;
	int yy = y * mult;

	// Guarding against rounding error

	if (xx < 1) {
		xx = 1;
	}
	if (xx > XMAX - 2) {
		xx = XMAX - 2;
	}
	if (yy < 1) {
		yy = 1;
	}
	if (yy > YMAX - 2) {
		yy = YMAX - 2;
	}

	env *e = gc->e;

	if (e->npoints + 1 >= e->npalloc) {
		e->npalloc *= 2;
		e->points = (struct point *) realloc((void *) e->points, e->npalloc * sizeof(struct point));
	}

	e->points[e->npoints].x = xx;
	e->points[e->npoints].y = yy;

	e->npoints++;
}

void drawBrush(double x, double y, struct graphics *gc, double bright, double brush, double hue, struct tilecontext *tc) {
	drawPixel(x, y, gc, bright, hue, tc);
}
