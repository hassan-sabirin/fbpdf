/*
 * FBPDF LINUX FRAMEBUFFER PDF VIEWER
 *
 * Copyright (C) 2009-2025 Ali Gholami Rudi <ali at rudi dot ir>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include "draw.h"
#include "doc.h"
#include "menu.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define PAGESTEPS	8
#define MAXZOOM		1000
#define MARGIN		1
#define CTRLKEY(x)	((x) - 96)
#define ISMARK(x)	(isalpha(x) || (x) == '\'' || (x) == '`')

#define ZOOM_STEP	10	/* zoom increment per keypress (x10 == 10%) */

static struct doc *doc;
static char *pbuf;		/* current page */
static int srows, scols;	/* screen dimensions */
static int prows, pcols;	/* current page dimensions */
static int prow, pcol;		/* page position */
static int srow, scol;		/* screen position */
static int bpp;			/* bytes per pixel */

static char filename[256];
static int mark[128];		/* mark page number */
static int mark_row[128];	/* mark head position */
static int num = 1;		/* page number */
static int numdiff;		/* G command page number difference */
static int zoom = 150;
static int zoom_def = 150;	/* default zoom */
static int rotate;
static int count;
static int invert;		/* invert colors? */

/* Drag-to-pan state. */
static int drag_active;
static int drag_srow;		/* srow at drag start */
static int drag_scol;		/* scol at drag start */
static int drag_trow;		/* terminal row at drag start */
static int drag_tcol;		/* terminal col at drag start */

/* Search state. */
static char search_keyword[256];
static int  search_last_page;	/* page of last successful match, or 0 */

static void draw(void)
{
	int i;
	char *rbuf;
	void *row;
	if (!pbuf)
		return;
	rbuf = malloc(scols * bpp);
	if (!rbuf)
		return;
	for (i = srow; i < srow + srows; i++) {
		int cbeg = MAX(scol, pcol);
		int cend = MIN(scol + scols, pcol + pcols);
		memset(rbuf, 0, scols * bpp);
		if (i >= prow && i < prow + prows && cbeg < cend) {
			memcpy(rbuf + (cbeg - scol) * bpp,
				pbuf + ((i - prow) * pcols + cbeg - pcol) * bpp,
				(cend - cbeg) * bpp);
		}
		row = fb_mem(i - srow);
		if (row)
			memcpy(row, rbuf, scols * bpp);
	}
	free(rbuf);
	fb_present();
}

static int loadpage(int p)
{
	int i;
	char *newbuf;
	int newrows = 0, newcols = 0;
	if (p < 1 || p > doc_pages(doc))
		return 1;
	newbuf = doc_draw(doc, p, zoom, rotate, bpp, &newrows, &newcols);
	if (!newbuf)
		return 1;
	free(pbuf);
	pbuf  = newbuf;
	prows = newrows;
	pcols = newcols;
	if (invert) {
		for (i = 0; i < prows * pcols * bpp; i++) {
			int val = (unsigned char) pbuf[i] ^ 0xff;
			pbuf[i] = val * invert / 255 + (255 - invert);
		}
	}
	prow = -prows / 2;
	pcol = -pcols / 2;
	num = p;
	return 0;
}

static void zoom_page(int z)
{
	int _zoom = zoom;
	zoom = MIN(MAXZOOM, MAX(1, z));
	if (!loadpage(num))
		srow = srow * zoom / _zoom;
}

static void setmark(int c)
{
	if (ISMARK(c)) {
		mark[c] = num;
		mark_row[c] = srow * 100 / zoom;
	}
}

static void jmpmark(int c, int offset)
{
	if (c == '`')
		c = '\'';
	if (ISMARK(c) && mark[c]) {
		int dst = mark[c];
		int dst_row = offset ? mark_row[c] * zoom / 100 : 0;
		setmark('\'');
		if (!loadpage(dst))
			srow = offset ? dst_row : prow;
	}
}

static int getcount(int def)
{
	int result = count ? count : def;
	count = 0;
	return result;
}

static void updatestatus(void)
{
	menu_statusbar(filename, num, doc_pages(doc), zoom);
}

static void sigcont(int sig)
{
	/* Reinitialise ncurses after a SIGSTOP/SIGCONT cycle. */
	menu_cleanup();
	menu_init();
	updatestatus();
}

/*
 * Open a new document, replacing the current one.
 * Returns 0 on success.
 */
static int opendoc(const char *path)
{
	struct doc *newdoc = doc_open(path);
	if (!newdoc || !doc_pages(newdoc)) {
		if (newdoc)
			doc_close(newdoc);
		return 1;
	}
	if (doc)
		doc_close(doc);
	doc = newdoc;
	snprintf(filename, sizeof(filename), "%s", path);
	num = 1;
	return 0;
}

static int reload(void)
{
	doc_close(doc);
	doc = NULL;
	free(pbuf);
	pbuf = NULL;
	doc = doc_open(filename);
	if (!doc || !doc_pages(doc))
		return 1;
	if (!loadpage(num))
		draw();
	return 0;
}

/* this can be optimised based on framebuffer pixel format */
void fb_set(char *d, unsigned r, unsigned g, unsigned b)
{
	unsigned c = fb_val(r, g, b);
	int i;
	for (i = 0; i < bpp; i++)
		d[i] = (c >> (i << 3)) & 0xff;
}

static int iswhite(char *pix)
{
	int val = 255 - invert;
	int i;
	for (i = 0; i < 3 && i < bpp; i++)
		if (((unsigned char) pix[i]) != val)
			return 0;
	return 1;
}

static int rmargin(void)
{
	int ret = 0;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = pcols - 1;
		while (j > ret && iswhite(pbuf + (i * pcols + j) * bpp))
			j--;
		if (ret < j)
			ret = j;
	}
	return ret;
}

static int lmargin(void)
{
	int ret = pcols;
	int i, j;
	for (i = 0; i < prows; i++) {
		j = 0;
		while (j < ret && iswhite(pbuf + (i * pcols + j) * bpp))
			j++;
		if (ret > j)
			ret = j;
	}
	return ret;
}

/*
 * Convert terminal character-cell deltas to framebuffer pixel deltas.
 * The terminal occupies the same physical display as the framebuffer, so
 * pixels_per_cell = fb_pixels / term_cells for each axis.
 */
static int cells_to_px_row(int dcells)
{
	int trows = LINES ? LINES : 24;
	return dcells * srows / trows;
}

static int cells_to_px_col(int dcells)
{
	int tcols = COLS ? COLS : 80;
	return dcells * scols / tcols;
}

/*
 * Apply a screen-space delta (dsr = screen rows down, dsc = screen cols right)
 * to srow/scol, accounting for page rotation.
 *
 * mupdf/poppler rotate the rendered buffer so that the page's natural "down"
 * axis maps to screen directions as follows:
 *   rotate=0:   page-down  == screen-down  (srow+), page-right == screen-right
 *   rotate=90:  page-down  == screen-right (scol+), page-right == screen-up
 *   rotate=180: page-down  == screen-up    (srow-), page-right == screen-left
 *   rotate=270: page-down  == screen-left  (scol-), page-right == screen-down
 */
static void pan_by(int dsr, int dsc)
{
	switch ((rotate / 90) % 4) {
	default:
	case 0: srow += dsr; scol += dsc; break;
	case 1: srow += dsc; scol -= dsr; break;
	case 2: srow -= dsr; scol -= dsc; break;
	case 3: srow -= dsc; scol += dsr; break;
	}
}

static void clamp_position(void)
{
	srow = MAX(prow - srows + MARGIN, MIN(prow + prows - MARGIN, srow));
	scol = MAX(pcol - scols + MARGIN, MIN(pcol + pcols - MARGIN, scol));
}

/*
 * Search for search_keyword starting from start_page.
 * On a match, navigate to that page and update search_last_page.
 * If wrap is non-zero, wraps around from the last page back to page 1.
 */
static void do_search(int start_page, int wrap)
{
	int found;
	if (!search_keyword[0] || !doc)
		return;
	found = doc_search(doc, search_keyword, start_page);
	if (!found && wrap)
		found = doc_search(doc, search_keyword, 1);
	if (found) {
		setmark('\'');
		search_last_page = found;
		if (!loadpage(found)) {
			srow = prow;
			clamp_position();
			draw();
		}
	}
	updatestatus();
}

/*
 * Handle an action returned by the menu.
 * Returns 1 if the main loop should exit, 0 otherwise.
 */
static int handle_action(int action, char *buf, MenuMouse *mm)
{
	int hstep = scols / PAGESTEPS;

	switch (action) {
	case MENU_FILE_EXIT:
		return 1;
	case MENU_FILE_OPEN:
		if (buf && buf[0]) {
			if (opendoc(buf)) {
				/* TODO: show error in status bar */
			} else if (!loadpage(1)) {
				srow = prow;
				scol = -scols / 2;
				draw();
			}
			updatestatus();
		}
		break;
	case MENU_VIEW_ROTATE_CW:
		rotate = (rotate + 90) % 360;
		if (!loadpage(num))
			srow = prow;
		draw();
		updatestatus();
		break;
	case MENU_VIEW_ROTATE_CCW:
		rotate = (rotate + 270) % 360;
		if (!loadpage(num))
			srow = prow;
		draw();
		updatestatus();
		break;
	case MENU_VIEW_ZOOM_IN:
		zoom_page(zoom + ZOOM_STEP);
		draw();
		updatestatus();
		break;
	case MENU_VIEW_ZOOM_OUT:
		zoom_page(zoom - ZOOM_STEP);
		draw();
		updatestatus();
		break;
	case MENU_VIEW_FIT_WIDTH:
		if (lmargin() < rmargin())
			zoom_page(zoom * (scols - hstep) /
				(rmargin() - lmargin()));
		else if (pcols)
			zoom_page(zoom * scols / pcols);
		draw();
		updatestatus();
		break;
	case MENU_VIEW_GOTO_PAGE:
		if (buf && buf[0]) {
			int p = atoi(buf);
			setmark('\'');
			if (!loadpage(p)) {
				srow = prow;
				draw();
				updatestatus();
			}
		}
		break;
	case MENU_VIEW_SEARCH:
		if (buf && buf[0]) {
			snprintf(search_keyword, sizeof(search_keyword), "%s", buf);
			search_last_page = 0;
			do_search(num, 1);
		}
		break;
	case MENU_SCROLL_UP: {
		int step = srows / PAGESTEPS;
		pan_by(-step, 0);
		clamp_position();
		if (srow <= prow - srows + MARGIN && num > 1) {
			if (!loadpage(num - 1))
				srow = prow + prows - srows;
		}
		draw();
		updatestatus();
		break;
	}
	case MENU_SCROLL_DOWN: {
		int step = srows / PAGESTEPS;
		pan_by(step, 0);
		clamp_position();
		if (srow >= prow + prows - MARGIN && num < doc_pages(doc)) {
			if (!loadpage(num + 1))
				srow = prow;
		}
		draw();
		updatestatus();
		break;
	}
	case MENU_NAV_PREV:
		if (!loadpage(num - 1))
			srow = prow;
		draw();
		updatestatus();
		break;
	case MENU_NAV_NEXT:
		if (!loadpage(num + 1))
			srow = prow;
		draw();
		updatestatus();
		break;
	case MENU_NAV_FIRST:
		setmark('\'');
		if (!loadpage(1))
			srow = prow;
		draw();
		updatestatus();
		break;
	case MENU_NAV_LAST:
		setmark('\'');
		if (!loadpage(doc_pages(doc)))
			srow = prow;
		draw();
		updatestatus();
		break;
	/* MENU_NAV_PAGE is handled by dispatch_action as MENU_VIEW_GOTO_PAGE */
	case MENU_MOUSE_PRESS:
		if (mm) {
			drag_active = 1;
			drag_srow   = srow;
			drag_scol   = scol;
			drag_trow   = mm->row;
			drag_tcol   = mm->col;
		}
		break;
	case MENU_MOUSE_DRAG:
		if (drag_active && mm) {
			int dsr = cells_to_px_row(mm->row - drag_trow);
			int dsc = cells_to_px_col(mm->col - drag_tcol);
			srow = drag_srow;
			scol = drag_scol;
			pan_by(-dsr, -dsc);
			clamp_position();
		}
		/* Redraw unconditionally to erase the GPM cursor trail. */
		draw();
		break;
	case MENU_MOUSE_RELEASE:
		if (drag_active && mm) {
			int dsr = cells_to_px_row(mm->row - drag_trow);
			int dsc = cells_to_px_col(mm->col - drag_tcol);
			srow = drag_srow;
			scol = drag_scol;
			pan_by(-dsr, -dsc);
			clamp_position();
			draw();
			updatestatus();
		}
		drag_active = 0;
		break;
	case MENU_MOUSE_ZOOM_IN:
		zoom_page(zoom + ZOOM_STEP);
		draw();
		updatestatus();
		break;
	case MENU_MOUSE_ZOOM_OUT:
		zoom_page(zoom - ZOOM_STEP);
		draw();
		updatestatus();
		break;
	default:
		break;
	}
	return 0;
}

static void mainloop(void)
{
	int step  = srows / PAGESTEPS;
	int hstep = scols / PAGESTEPS;
	char buf[256];
	int c;

	signal(SIGCONT, sigcont);
	loadpage(num);
	srow = prow;
	scol = -scols / 2;
	draw();
	updatestatus();

	while ((c = menu_readkey()) != -1) {
		int action;
		MenuMouse mm;

		/* Let the menu layer intercept F10 and all mouse events. */
		action = menu_handle_key(c, buf, sizeof(buf), &mm);
		if (action != MENU_NONE) {
			if (handle_action(action, buf, &mm))
				break;
			continue;
		}

		if (c == 'q')
			break;
		if (c == 'e' && reload())
			break;

		switch (c) {	/* commands that do not require redrawing */
		case 'o':
			numdiff = num - getcount(num);
			break;
		case 'Z':
			count *= 10;
			zoom_def = getcount(zoom);
			break;
		case 'i':
			updatestatus();
			break;
		case 27:
			count = 0;
			break;
		case 'm':
			setmark(menu_readkey());
			break;
		case 'd':
			sleep(getcount(1));
			break;
		default:
			if (isdigit(c))
				count = count * 10 + c - '0';
		}

		switch (c) {	/* commands that require redrawing */
		case CTRLKEY('f'):
		case 'J':
			if (!loadpage(num + getcount(1)))
				srow = prow;
			break;
		case CTRLKEY('b'):
		case 'K':
			if (!loadpage(num - getcount(1)))
				srow = prow;
			break;
		case 'G':
			setmark('\'');
			if (!loadpage(getcount(doc_pages(doc) - numdiff) + numdiff))
				srow = prow;
			break;
		case 'O':
			numdiff = num - getcount(num);
			setmark('\'');
			if (!loadpage(num + numdiff))
				srow = prow;
			break;
		case 'z':
			count *= 10;
			zoom_page(getcount(zoom_def));
			break;
		case 'w':
			zoom_page(pcols ? zoom * scols / pcols : zoom);
			break;
		case 'W':
			if (lmargin() < rmargin())
				zoom_page(zoom * (scols - hstep) /
					(rmargin() - lmargin()));
			break;
		case 'f':
			zoom_page(prows ? zoom * srows / prows : zoom);
			break;
		case 'r':
			rotate = getcount(0);
			if (!loadpage(num))
				srow = prow;
			break;
		case '`':
		case '\'':
			jmpmark(menu_readkey(), c == '`');
			break;
		case 'j':
			pan_by(step * getcount(1), 0);
			break;
		case 'k':
			pan_by(-step * getcount(1), 0);
			break;
		case 'l':
			pan_by(0, hstep * getcount(1));
			break;
		case 'h':
			pan_by(0, -hstep * getcount(1));
			break;
		case 'H':
			srow = prow;
			break;
		case 'L':
			srow = prow + prows - srows;
			break;
		case 'M':
			srow = prow + prows / 2 - srows / 2;
			break;
		case 'C':
			scol = -scols / 2;
			break;
		case ' ':
		case CTRLKEY('d'):
			pan_by(srows * getcount(1) - step, 0);
			break;
		case 127:
		case CTRLKEY('u'):
			pan_by(-(srows * getcount(1) - step), 0);
			break;
		case '[':
			scol = pcol;
			break;
		case ']':
			scol = pcol + pcols - scols;
			break;
		case '{':
			scol = pcol + lmargin() - hstep / 2;
			break;
		case '}':
			scol = pcol + rmargin() + hstep / 2 - scols;
			break;
		case CTRLKEY('l'):
			break;
		case 'I':
			invert = count || !invert ? 255 - (getcount(48) & 0xff) : 0;
			loadpage(num);
			break;
		case 'n':
			/* Next match: start from the page after the last hit. */
			do_search(search_last_page ? search_last_page + 1 : num, 1);
			continue;
		case 'N':
			/* Previous match: search backwards by scanning from page 1
			 * up to the page before the last hit. */
			if (search_keyword[0]) {
				int target = search_last_page ? search_last_page - 1 : num - 1;
				int found = 0, p;
				for (p = 1; p <= target; p++) {
					int r = doc_search(doc, search_keyword, p);
					if (r && r <= target) {
						found = r;
						p = r;	/* skip ahead to avoid re-scanning */
					}
				}
				if (!found)	/* wrap: find last match in whole doc */
					for (p = 1; p <= doc_pages(doc); p++) {
						int r = doc_search(doc, search_keyword, p);
						if (r) { found = r; p = r; }
					}
				if (found) {
					setmark('\'');
					search_last_page = found;
					if (!loadpage(found)) {
						srow = prow;
						clamp_position();
						draw();
					}
					updatestatus();
				}
			}
			continue;
		default:
			continue;
		}
		clamp_position();
		draw();
		updatestatus();
	}
}

static char *usage =
	"usage: fbpdf [-r rotation] [-z zoom x10] [-p page] filename";

int main(int argc, char *argv[])
{
	int i = 1;
	if (argc < 2) {
		puts(usage);
		return 1;
	}
	snprintf(filename, sizeof(filename), "%s", argv[argc - 1]);
	doc = doc_open(filename);
	if (!doc || !doc_pages(doc)) {
		fprintf(stderr, "fbpdf: cannot open <%s>\n", filename);
		return 1;
	}
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		switch (argv[i][1]) {
		case 'r':
			rotate = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "0"));
			break;
		case 'z':
			zoom = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "0")) * 10;
			break;
		case 'p':
			num = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "1"));
			break;
		}
	}
	if (fb_init(getenv("FBDEV")))
		return 1;
	srows = fb_rows();
	scols = fb_cols();
	bpp = FBM_BPP(fb_mode());
	menu_init();
	mainloop();
	menu_cleanup();
	fb_free();
	free(pbuf);
	if (doc)
		doc_close(doc);
	return 0;
}
