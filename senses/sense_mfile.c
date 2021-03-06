/*
 * Copyright 2015, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in the senseye source repository.
 * Reference: http://senseye.arcan-fe.com
 * Description: Mfile is simplified sensor that implements side-by-side
 * comparison of multiple input sources. As such, it does not make use of the
 * rwstats or senseye_connect support as they focus a lot of mapping and
 * transfer modes that do not make sense here. This means that we don't have to
 * follow other restrictions, e.g. having a base that is a power of two.
 */

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <getopt.h>

#include <arcan_shmif.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include "font_8x8.h"

struct ent {
	uint8_t* map;
	size_t map_sz;
	int fd;
	const char* arg;
};

static struct {
	shmif_pixel border;
	shmif_pixel pad;
	shmif_pixel diff;
	shmif_pixel match;
}
color = {
	.border = RGBA(0xff, 0x00, 0x00, 0xff),
	.pad = RGBA(0x00, 0x00, 0x00, 0xff),
	.diff = RGBA(0x00, 0xff, 0x00, 0xff),
	.match = RGBA(0x00, 0x00, 0x00, 0xff)
};

enum pack_mode {
	PACK_INTENS = 0,
	PACK_TIGHT,
	PACK_TNOALPHA
};

static size_t pack_szlut[] = {
	1, 4, 3
};

static int usage()
{
	const char* const argp[] = {
		"-d,--nodiff", "disable diff subwindow",
		"-s,--border=val", "set border width (0..10), default: 1",
		"-?,--help", "this text"
	};

	printf("Usage: sense_mfile [options] file1 file2 ...\n");
	for (size_t i = 0; i < sizeof(argp)/sizeof(argp[0]); i+=2)
		printf("%-15s %s\n", argp[i], argp[i+1]);

/* add options for size, border-color,
* and if we should connect in loop or fatalfail */

	return EXIT_SUCCESS;
}

static const struct option longopts[] = {
	{"nodiff", no_argument,       NULL, 'd'},
	{"help",   no_argument,       NULL, '?'},
	{"border", required_argument, NULL, 's'},
	{NULL, no_argument, NULL, 0}
};

struct ent* load_context(char** files, size_t nfiles, size_t* min, size_t* max)
{
	struct ent* res = malloc(sizeof(struct ent) * nfiles);
	memset(res, '\0', sizeof(struct ent) * nfiles);
	*max = 0;
	*min = INT_MAX;

	for (size_t i=0; i < nfiles; i++){
		struct ent* dent = &res[i];
		dent->fd = open(files[i], O_RDONLY);
		if (-1 == dent->fd){
			fprintf(stderr, "Failed while trying to open %s\n", files[i]);
			return NULL;
		}

		struct stat buf;
		if (1 == fstat(dent->fd, &buf)){
			fprintf(stderr, "Couldn't get stat for %s, reason: %s\n",
				files[i], strerror(errno));
			return NULL;
		}

		if (!S_ISREG(buf.st_mode)){
			fprintf(stderr, "Invalid file mode for %s, expecting a normal file.\n",
				files[i]);
		}

		dent->map_sz = buf.st_size;
		dent->map = mmap(NULL, dent->map_sz, PROT_READ, MAP_PRIVATE, dent->fd, 0);
		if (dent->map == MAP_FAILED){
			fprintf(stderr,
				"Failed to map %s, reason: %s\n", files[i], strerror(errno));
			return NULL;
		}

		if (dent->map_sz > *max)
			*max = dent->map_sz;

		if (dent->map_sz < *min)
			*min = dent->map_sz;
	}

	return res;
}

static inline shmif_pixel pack_pixel(enum pack_mode mode, uint8_t* buf)
{
	switch(mode){
	case PACK_INTENS:
		return RGBA(*buf, *buf, *buf, 0xff);
	case PACK_TIGHT:
		return RGBA(buf[0], buf[1], buf[2], buf[3]);
	case PACK_TNOALPHA:
		return RGBA(buf[0], buf[1], buf[2], 0xff);
	}

/* because poor' ol' gcc */
	return RGBA(0xde, 0xad, 0xbe, 0xef);
}

/*
 * sweep all entries and generate a 1-bit tile that indicates if the input
 * files match or diff at each packing position
 */
static void draw_dtile(struct arcan_shmif_cont* dst,
	struct ent* ents, size_t n_ents, size_t pos, size_t x, size_t y,
	enum pack_mode mode, size_t base)
{
	size_t step = pack_szlut[mode];

	for (size_t row = y; row < y+base; row++)
		for (size_t col = x; col < x+base; col++, pos += step){
			shmif_pixel pxbuf[n_ents];
			for (size_t i = 0; i < n_ents; i++)
				pxbuf[i] = pos+step >= ents[i].map_sz ? color.pad :
					pack_pixel(mode, ents[i].map + pos);

			float n_delta = 0;
			for (size_t i = 1; i < n_ents; i++)
				n_delta += pxbuf[i] != pxbuf[0] ? 1 : 0;

			dst->vidp[row*dst->pitch+col] = n_delta ?
				RGBA(0x00, 255.0 * (n_delta / (float)n_ents), 0x00, 0xff) : color.match;
	}
}

static void draw_tile(struct arcan_shmif_cont* dst,
	struct ent* ent, size_t pos, size_t x, size_t y,
	enum pack_mode mode, size_t base)
{
	size_t ntw = base*base;
	size_t step = pack_szlut[mode];
	size_t end = pos + ntw * step;

	if (end > ent->map_sz)
		ntw = end > ent->map_sz + ntw * step ? 0 : (ent->map_sz - pos) / step;

	size_t row = y;
	size_t col = x;
	size_t row_lim = base+y;
	size_t col_lim = base+x;

	uint8_t* buf = ent->map + pos;

	for (; row < row_lim && ntw; row++)
		for (col = x; col < col_lim && ntw; col++, ntw--, buf += step)
			dst->vidp[row*dst->pitch+col] = pack_pixel(mode, buf);

	for (; row < row_lim; row++)
		for (col = x; col < col_lim; col++)
			dst->vidp[row*dst->pitch+col] = RGBA(0x00, 0x00, 0x00, 0xff);
}

static void refresh_diff(struct arcan_shmif_cont* dst,
	struct ent* entries, size_t n_entries, size_t base,
	enum pack_mode mode, size_t pos)
{
	draw_dtile(dst, entries, n_entries, pos, 0, 0,mode, base);
	arcan_shmif_signal(dst, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

static void refresh_data(struct arcan_shmif_cont* dst,
	struct ent* entries, size_t n_entries, size_t base,
	enum pack_mode mode, size_t pos, size_t border
)
{
	size_t y = 0, x = 0;

/* flood-fill "draw_tile", this could well be thread- split per tile */
	for (size_t i = 0; i < n_entries && y <= dst->h - base; i++){
		draw_tile(dst, &entries[i], pos, x, y, mode, base);
		x = x + base + border;
		if (x + base > dst->w){
			x = 0;
			if (border)
				draw_box(dst, 0, y + base, dst->w, border, color.border);
			y += base + border;
		}
		else if (border)
			draw_box(dst, x - border, y, border, base, color.border);
	}

	arcan_event ev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = EVENT_EXTERNAL_FRAMESTATUS,
		.ext.framestatus.pts = base,
		.ext.framestatus.framenumber = pos
	};
	arcan_shmif_enqueue(dst, &ev);
	arcan_shmif_signal(dst, SHMIF_SIGVID);
}

static int resize_base(struct arcan_shmif_cont* cont,
	size_t base, size_t n, size_t border)
{
	int npr = ceil(sqrtf(n));
	int nr = ceil((float)n / (float)npr);

	if (!arcan_shmif_resize(cont,
		(npr * (base + border)) - border, nr * (base+border) - border)){
		fprintf(stderr, "Couldn't resize shmif segment, try with a smaller number "
			" of tiles or a smaller base dimension.\n");
		return 0;
	}

	draw_box(cont, 0, 0, cont->w, cont->h, color.pad);
	return !0;
}

static inline void send_streaminfo(struct arcan_shmif_cont* cont,
	size_t n, size_t border, enum pack_mode mode)
{
	arcan_event ev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = EVENT_EXTERNAL_STREAMINFO,
		.ext.streaminf.streamid = n,
		.ext.streaminf.langid[0] = '0' + border,
		.ext.streaminf.langid[1] = '0' + pack_szlut[mode]
	};

	arcan_shmif_enqueue(cont, &ev);
}

int main(int argc, char* argv[])
{
	struct arcan_shmif_cont cont;
	struct arcan_shmif_cont diffcont = {0};
	struct arg_arr* aarr;

	size_t base = 64;
	size_t border = 1;
	off_t ofs = 0;
	enum pack_mode pack_mode = PACK_INTENS;

	bool difftile = true;
	int ch;

	while((ch = getopt_long(argc, argv, "db:?", longopts, NULL)) >= 0)
	switch(ch){
	case '?' :
		return usage();
	break;
	case 'b' :
		border = strtoul(optarg, NULL, 10);
		border = border > 9 ? 9 : border;
	break;
	case 'd' :
		difftile = true;
	break;
	}

	if (optind >= argc - 1 || (argc - optind) > 256){
		printf("Error: missing filenames (n files within: 1 < n < 256)\n");
		return usage();
	}

	size_t min_r, max_r, n_ent = argc - optind;
	struct ent* entries = load_context(argv + optind, n_ent, &min_r, &max_r);
	if (entries == NULL)
		return EXIT_FAILURE;

	if (NULL == getenv("ARCAN_CONNPATH"))
		setenv("ARCAN_CONNPATH", "senseye", 0);
	cont = arcan_shmif_open(SEGID_SENSOR, SHMIF_CONNECT_LOOP, &aarr);
	unsetenv("ARCAN_CONNPATH");
	resize_base(&cont, base, n_ent, border);

	arcan_event ev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = EVENT_EXTERNAL_IDENT,
		.ext.message = "mfsense"
	};
	arcan_shmif_enqueue(&cont, &ev);
	send_streaminfo(&cont, n_ent, border, pack_mode);

	if (difftile){
		ev.ext.kind = EVENT_EXTERNAL_SEGREQ;
		ev.ext.segreq.width = base;
		ev.ext.segreq.height = base;
		ev.ext.segreq.id = 0xcafe;
		arcan_shmif_enqueue(&cont, &ev);
	}

#define REFRESH() {\
	if (difftile && diffcont.vidp) refresh_diff(\
		&diffcont, entries, n_ent, base, pack_mode, ofs);\
		refresh_data(&cont, entries, n_ent, base, pack_mode, ofs, border);\
	}

/* flush twice, once to initialize state, second to make sure secondary
 * buffers are in synch for translators etc. */
	REFRESH();
	REFRESH();

	size_t small_step = base;
	size_t large_step = base*base;

	while (arcan_shmif_wait(&cont, &ev) != 0){
		if (ev.category == EVENT_TARGET)
			switch(ev.tgt.kind){
/* displayhint here would mean that width is the new base */
			case TARGET_COMMAND_DISPLAYHINT:{
				size_t lb = ev.tgt.ioevs[0].iv;
					if (lb > 0 && (lb & (lb - 1)) == 0 &&
						resize_base(&cont,lb,n_ent + (difftile?1:0), border)){
/* we may have a custom override step size, then don't adapt */
						if (small_step == base)
							small_step = lb;
						if (large_step == base * base)
							large_step = lb * lb;

						base = lb;
						if (difftile && diffcont.vidp)
							arcan_shmif_resize(&diffcont, base, base);
						REFRESH();
					}
			}
			break;
			case TARGET_COMMAND_NEWSEGMENT:
				diffcont = arcan_shmif_acquire(&cont,
					NULL, SEGID_SENSOR, SHMIF_DISABLE_GUARD);
				ev.ext.kind = EVENT_EXTERNAL_IDENT;
				ev.category = EVENT_EXTERNAL;
				snprintf((char*)ev.ext.message, sizeof(ev.ext.message)/
					sizeof(ev.ext.message[0]), "mfsense_diff");
				arcan_shmif_enqueue(&diffcont, &ev);

				if (diffcont.vidp)
					refresh_diff(&diffcont, entries, n_ent, base, pack_mode, ofs);
			break;
			case TARGET_COMMAND_STEPFRAME:{
				size_t small = small_step;
				size_t large = large_step * pack_szlut[pack_mode];

				switch (ev.tgt.ioevs[0].iv){
				case -1: ofs  = ofs > small ? ofs - small : 0; break;
				case -2: ofs  = ofs > large ? ofs - large : 0; break;
				case  1: ofs += small; break;
				case  2: ofs += large; break;
				}
				REFRESH();
			}
			break;
			case TARGET_COMMAND_GRAPHMODE:
/* should we acknowledge the change in pack mode? */
				if (ev.tgt.ioevs[0].iv >= 20 && ev.tgt.ioevs[0].iv <= 22)
					pack_mode = ev.tgt.ioevs[0].iv - 20;
				send_streaminfo(&cont, n_ent, border, pack_mode);
				REFRESH();
			break;
			default:
			break;
		}
/* same input mapping as used in sense_file */
		else if (ev.category == EVENT_IO){
			if (strcmp(ev.io.label, "STEP_BYTE") == 0)
				small_step = 1;
			else if (strcmp(ev.io.label, "STEP_PIXEL") == 0)
				small_step = pack_szlut[pack_mode];
			else if (strcmp(ev.io.label, "STEP_ROW") == 0)
				small_step = base;
			else if (strcmp(ev.io.label, "STEP_HALFPAGE") == 0)
				large_step = (base * base) >> 1;
			else if (strcmp(ev.io.label, "STEP_PAGE") == 0)
				large_step = base * base;
			else if (strncmp(ev.io.label, "CSTEP_", 6) == 0){
				unsigned sz = strtoul(&ev.io.label[6], NULL, 10);
				if (sz > 0)
					small_step = sz;
			}
			else if (strncmp(ev.io.label, "STEP_ALIGN_", 11) == 0){
				size_t align = strtoul(&ev.io.label[11], NULL, 10);
				if (align && ofs > align){
					if (ofs % align != 0){
						ofs -= ofs % align;
						REFRESH();
					}
				}
			}
			else
				;
		}
		else
			;
	}

#undef REFRESH
	arcan_shmif_drop(&cont);

	return EXIT_SUCCESS;
}
