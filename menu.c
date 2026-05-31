#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"

/* -------------------------------------------------------------------------
 * Layout constants
 * ---------------------------------------------------------------------- */
#define STATUSBAR_ROW	0	/* ncurses row used for the status/menu bar */
#define DROPDOWN_ROW	1	/* first row of a drop-down panel */
#define NAV_COL		13	/* column where nav buttons start (after menus) */

/*
 * ncurses owns a narrow band at the top of the terminal.  We keep a
 * persistent overlay window (s_win) sized to exactly the rows currently
 * in use.  When no menu is active it is 1 row tall (just the status bar).
 * While a dropdown or dialog is visible it grows to cover those rows too.
 *
 * Keeping a fixed-size window and resizing it (via wresize) is simpler than
 * creating/destroying windows, and avoids the delwin-after-framebuffer-write
 * crash that plagued the previous newwin/delwin approach.
 *
 * Input is always read via wgetch(stdscr) so KEY_MOUSE events, which ncurses
 * queues on stdscr, are never missed.
 */
#define MENU_ROWS_IDLE	1	/* rows owned when no dropdown is open */

static WINDOW *s_win;		/* the persistent overlay window */
static int     s_win_rows;	/* current height of s_win */

/* Resize (or create) s_win to cover exactly `rows` rows. */
static void win_resize(int rows)
{
	int cols = getmaxx(stdscr);
	if (rows < 1) rows = 1;
	if (cols < 1) cols = 1;
	if (!s_win) {
		s_win = newwin(rows, cols, 0, 0);
		s_win_rows = rows;
	} else if (rows != s_win_rows) {
		wresize(s_win, rows, cols);
		s_win_rows = rows;
	}
	if (s_win)
		keypad(s_win, FALSE);	/* input always via stdscr */
}

/* Flush only s_win to the terminal — stdscr is never refreshed. */
static void win_flush(void)
{
	if (s_win) {
		wnoutrefresh(s_win);
		doupdate();
	}
}

/* -------------------------------------------------------------------------
 * Module state — kept here so bar_draw and menu_statusbar stay in sync.
 * ---------------------------------------------------------------------- */
static int s_page  = 1;
static int s_pages = 1;
static int s_zoom  = 150;
static char s_filename[256];

/* -------------------------------------------------------------------------
 * Menu structure
 * ---------------------------------------------------------------------- */
typedef struct {
	const char *label;
	int         action;
} MenuItem;

typedef struct {
	const char *title;		/* shown in the menu bar */
	int         col;		/* starting column in the bar */
	const MenuItem *items;
	int         nitems;
} Menu;

static const MenuItem file_items[] = {
	{ "Open...    ", MENU_FILE_OPEN },
	{ "Exit       ", MENU_FILE_EXIT },
};

static const MenuItem view_items[] = {
	{ "Rotate Clockwise    ", MENU_VIEW_ROTATE_CW  },
	{ "Rotate Anti-CW      ", MENU_VIEW_ROTATE_CCW },
	{ "Zoom In             ", MENU_VIEW_ZOOM_IN    },
	{ "Zoom Out            ", MENU_VIEW_ZOOM_OUT   },
	{ "Fit to Page Width   ", MENU_VIEW_FIT_WIDTH  },
	{ "Go to Page...       ", MENU_VIEW_GOTO_PAGE  },
	{ "Search Keyword...   ", MENU_VIEW_SEARCH     },
};

static const Menu menus[] = {
	{ " File ", 0,  file_items, 2 },
	{ " View ", 6,  view_items, 7 },
};
#define NMENUS	2

/* -------------------------------------------------------------------------
 * Navigation button layout
 * ---------------------------------------------------------------------- */
#define NAV_NBTNS	5

typedef struct {
	int action;
	int col;
	int width;
} NavBtn;

static NavBtn nav_btns[NAV_NBTNS] = {
	{ MENU_NAV_FIRST, 0, 0 },
	{ MENU_NAV_PREV,  0, 0 },
	{ MENU_NAV_PAGE,  0, 0 },
	{ MENU_NAV_NEXT,  0, 0 },
	{ MENU_NAV_LAST,  0, 0 },
};

static const char *nav_labels[NAV_NBTNS] = {
	"|<", "<", NULL, ">", ">|"
};

static void nav_layout(void)
{
	char pagebuf[32];
	int pagew;
	int col = NAV_COL;

	snprintf(pagebuf, sizeof(pagebuf), "[%d/%d]", s_page, s_pages);
	pagew = (int)strlen(pagebuf);

	nav_btns[0].col = col; nav_btns[0].width = 2; col += 3;
	nav_btns[1].col = col; nav_btns[1].width = 1; col += 2;
	nav_btns[2].col = col; nav_btns[2].width = pagew; col += pagew + 1;
	nav_btns[3].col = col; nav_btns[3].width = 1; col += 2;
	nav_btns[4].col = col; nav_btns[4].width = 2;
}

static int nav_hit(int col)
{
	int i;
	for (i = 0; i < NAV_NBTNS; i++)
		if (col >= nav_btns[i].col && col < nav_btns[i].col + nav_btns[i].width)
			return nav_btns[i].action;
	return MENU_NONE;
}

static void nav_draw(void)
{
	char pagebuf[32];
	int i;
	snprintf(pagebuf, sizeof(pagebuf), "[%d/%d]", s_page, s_pages);
	for (i = 0; i < NAV_NBTNS; i++) {
		const char *label = (i == 2) ? pagebuf : nav_labels[i];
		mvwprintw(s_win, STATUSBAR_ROW, nav_btns[i].col, "%s", label);
	}
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static int bar_hit(int col)
{
	int i;
	for (i = 0; i < NMENUS; i++) {
		int len = (int)strlen(menus[i].title);
		if (col >= menus[i].col && col < menus[i].col + len)
			return i;
	}
	return -1;
}

/* Draw the status bar into row 0 of s_win and flush only that row. */
static void bar_draw(int highlight)
{
	int i, cols;

	if (!s_win) return;
	cols = getmaxx(s_win);

	wattron(s_win, A_REVERSE);
	wmove(s_win, STATUSBAR_ROW, 0);
	wclrtoeol(s_win);
	for (i = 0; i < NMENUS; i++) {
		if (i == highlight) wattroff(s_win, A_REVERSE);
		mvwprintw(s_win, STATUSBAR_ROW, menus[i].col, "%s", menus[i].title);
		if (i == highlight) wattron(s_win, A_REVERSE);
	}
	nav_draw();
	{
		char info[320];
		int infolen;
		snprintf(info, sizeof(info), " %s  zoom %d%% ", s_filename, s_zoom);
		infolen = (int)strlen(info);
		if (infolen < cols)
			mvwprintw(s_win, STATUSBAR_ROW, cols - infolen, "%s", info);
	}
	wattroff(s_win, A_REVERSE);
	win_flush();
}

static int dropdown_width(const Menu *m)
{
	int i, w = 0;
	for (i = 0; i < m->nitems; i++) {
		int l = (int)strlen(m->items[i].label);
		if (l > w) w = l;
	}
	return w + 2;
}

/*
 * Show a drop-down for menu m, pre-selecting item sel.
 * Returns the chosen action, or MENU_NONE if cancelled.
 * Returns -KEY_LEFT / -KEY_RIGHT to signal switching to adjacent menu.
 */
static int dropdown_run(const Menu *m, int sel)
{
	int w = dropdown_width(m);
	int h = m->nitems + 2;
	int dcol = m->col;
	int i, c;

	if (LINES < DROPDOWN_ROW + h || COLS < dcol + w)
		return MENU_NONE;

	/* Grow s_win to cover the status bar + dropdown rows. */
	win_resize(DROPDOWN_ROW + h);

	while (1) {
		bar_draw(-1);

		/* Top border */
		mvwaddch(s_win, DROPDOWN_ROW, dcol, ACS_ULCORNER);
		for (i = 1; i < w - 1; i++) waddch(s_win, ACS_HLINE);
		waddch(s_win, ACS_URCORNER);
		/* Items */
		for (i = 0; i < m->nitems; i++) {
			int cur;
			mvwaddch(s_win, DROPDOWN_ROW + 1 + i, dcol, ACS_VLINE);
			if (i == sel) wattron(s_win, A_REVERSE);
			wprintw(s_win, "%s", m->items[i].label);
			if (i == sel) wattroff(s_win, A_REVERSE);
			cur = (int)strlen(m->items[i].label) + 1;
			while (cur < w - 1) { waddch(s_win, ' '); cur++; }
			waddch(s_win, ACS_VLINE);
		}
		/* Bottom border */
		mvwaddch(s_win, DROPDOWN_ROW + h - 1, dcol, ACS_LLCORNER);
		for (i = 1; i < w - 1; i++) waddch(s_win, ACS_HLINE);
		waddch(s_win, ACS_LRCORNER);
		win_flush();

		c = wgetch(stdscr);
		switch (c) {
		case KEY_UP:
			sel = (sel + m->nitems - 1) % m->nitems;
			break;
		case KEY_DOWN:
			sel = (sel + 1) % m->nitems;
			break;
		case '\n': case '\r': case KEY_ENTER:
			win_resize(MENU_ROWS_IDLE);
			return m->items[sel].action;
		case KEY_LEFT:
			win_resize(MENU_ROWS_IDLE);
			return -KEY_LEFT;
		case KEY_RIGHT:
			win_resize(MENU_ROWS_IDLE);
			return -KEY_RIGHT;
		case 27:
			win_resize(MENU_ROWS_IDLE);
			return MENU_NONE;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK) break;
			if (ev.y == STATUSBAR_ROW && (ev.bstate & BUTTON1_PRESSED)) {
				int hit = bar_hit(ev.x);
				win_resize(MENU_ROWS_IDLE);
				if (hit >= 0 && hit != (int)(m - menus))
					return -(KEY_RIGHT * 100 + hit);
				return MENU_NONE;
			}
			if (ev.bstate & BUTTON1_PRESSED) {
				int item = ev.y - DROPDOWN_ROW - 1;
				if (ev.x >= dcol && ev.x < dcol + w &&
				    item >= 0 && item < m->nitems) {
					sel = item;
					if (ev.bstate & BUTTON1_DOUBLE_CLICKED) {
						win_resize(MENU_ROWS_IDLE);
						return m->items[sel].action;
					}
				} else if (ev.y > STATUSBAR_ROW) {
					win_resize(MENU_ROWS_IDLE);
					return MENU_NONE;
				}
			}
			if (ev.bstate & BUTTON4_PRESSED)
				sel = (sel + m->nitems - 1) % m->nitems;
			if (ev.bstate & BUTTON5_PRESSED)
				sel = (sel + 1) % m->nitems;
			break;
		}
		default:
			break;
		}
	}
}

/*
 * Prompt the user for a single line of text.
 * Returns 1 if confirmed, 0 if cancelled.
 */
static int input_dialog(const char *prompt, char *buf, int bufsz)
{
	int scr_rows, scr_cols;
	int w, h, dr, dc;
	int ch, len, i, plen;

	getmaxyx(stdscr, scr_rows, scr_cols);
	plen = (int)strlen(prompt);
	w = plen + bufsz + 4;
	if (w > scr_cols - 4) w = scr_cols - 4;
	h = 3;
	dr = scr_rows / 2 - 1;
	dc = (scr_cols - w) / 2;

	/* Grow s_win to reach the bottom of the dialog box. */
	win_resize(dr + h);

	echo();
	curs_set(1);

	buf[0] = '\0';
	len = 0;

	while (1) {
		/* Draw dialog box rows into s_win. */
		mvwaddch(s_win, dr, dc, ACS_ULCORNER);
		for (i = 1; i < w - 1; i++) waddch(s_win, ACS_HLINE);
		waddch(s_win, ACS_URCORNER);

		mvwaddch(s_win, dr + 1, dc, ACS_VLINE);
		wprintw(s_win, " %s%-*s", prompt, w - plen - 3, buf);
		waddch(s_win, ACS_VLINE);

		mvwaddch(s_win, dr + 2, dc, ACS_LLCORNER);
		for (i = 1; i < w - 1; i++) waddch(s_win, ACS_HLINE);
		waddch(s_win, ACS_LRCORNER);

		wmove(s_win, dr + 1, dc + 1 + plen + 1 + len);
		win_flush();

		ch = wgetch(stdscr);
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			buf[len] = '\0';
			break;
		}
		if (ch == 27) { buf[0] = '\0'; len = 0; break; }
		if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
			len--;
			buf[len] = '\0';
		} else if (ch >= 32 && ch < 127 && len < bufsz - 1) {
			buf[len++] = (char)ch;
			buf[len] = '\0';
		}
	}

	noecho();
	curs_set(0);
	win_resize(MENU_ROWS_IDLE);
	return len > 0;
}

/*
 * Dispatch an action that may need a text-input dialog.
 */
static int dispatch_action(int action, char *buf, int bufsz)
{
	if (action == MENU_FILE_OPEN) {
		if (buf && input_dialog("Open file: ", buf, bufsz))
			return MENU_FILE_OPEN;
		return MENU_NONE;
	}
	if (action == MENU_VIEW_GOTO_PAGE || action == MENU_NAV_PAGE) {
		if (buf && input_dialog("Go to page: ", buf, bufsz))
			return MENU_VIEW_GOTO_PAGE;
		return MENU_NONE;
	}
	if (action == MENU_VIEW_SEARCH) {
		if (buf && input_dialog("Search keyword: ", buf, bufsz))
			return MENU_VIEW_SEARCH;
		return MENU_NONE;
	}
	return action;
}

/*
 * Run the interactive menu bar starting at menu index start_menu.
 */
static int menubar_run(int start_menu, char *buf, int bufsz)
{
	int cur = start_menu;
	int action;

	bar_draw(cur);

	while (1) {
		int c = wgetch(stdscr);

		switch (c) {
		case KEY_RIGHT:
			cur = (cur + 1) % NMENUS;
			bar_draw(cur);
			break;
		case KEY_LEFT:
			cur = (cur + NMENUS - 1) % NMENUS;
			bar_draw(cur);
			break;
		case '\n': case '\r': case KEY_ENTER: case KEY_DOWN:
open_dropdown:
			action = dropdown_run(&menus[cur], 0);
			if (action <= -(KEY_RIGHT * 100)) {
				cur = -(action + KEY_RIGHT * 100);
				bar_draw(cur);
				goto open_dropdown;
			}
			if (action == -KEY_LEFT) {
				cur = (cur + NMENUS - 1) % NMENUS;
				bar_draw(cur);
				break;
			}
			if (action == -KEY_RIGHT) {
				cur = (cur + 1) % NMENUS;
				bar_draw(cur);
				break;
			}
			if (action == MENU_NONE) {
				bar_draw(-1);
				return MENU_NONE;
			}
			bar_draw(-1);
			return dispatch_action(action, buf, bufsz);
		case 27:
			bar_draw(-1);
			return MENU_NONE;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK) break;
			if (ev.y == STATUSBAR_ROW && (ev.bstate & BUTTON1_PRESSED)) {
				int hit = bar_hit(ev.x);
				if (hit >= 0) {
					cur = hit;
					bar_draw(cur);
					goto open_dropdown;
				}
				action = nav_hit(ev.x);
				if (action != MENU_NONE) {
					bar_draw(-1);
					return dispatch_action(action, buf, bufsz);
				}
				bar_draw(-1);
				return MENU_NONE;
			}
			if (ev.y != STATUSBAR_ROW && (ev.bstate & BUTTON1_PRESSED)) {
				bar_draw(-1);
				return MENU_NONE;
			}
			break;
		}
		default:
			break;
		}
	}
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void menu_init(void)
{
	initscr();
	cbreak();
	noecho();
	curs_set(0);
	keypad(stdscr, TRUE);
	mouseinterval(0);
	mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED |
	          BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON2_PRESSED |
	          BUTTON4_PRESSED | BUTTON5_PRESSED |
	          REPORT_MOUSE_POSITION, NULL);

	nav_layout();
	win_resize(MENU_ROWS_IDLE);
}

void menu_cleanup(void)
{
	if (s_win) { delwin(s_win); s_win = NULL; }
	endwin();
}

void menu_statusbar(const char *filename, int page, int pages, int zoom)
{
	int cols;
	int changed_pages = (pages != s_pages);

	snprintf(s_filename, sizeof(s_filename), "%s", filename);
	s_page  = page;
	s_pages = pages;
	s_zoom  = zoom;

	if (changed_pages)
		nav_layout();

	if (!s_win) return;
	cols = getmaxx(s_win);

	wattron(s_win, A_REVERSE);
	wmove(s_win, STATUSBAR_ROW, 0);
	wclrtoeol(s_win);
	mvwprintw(s_win, STATUSBAR_ROW, 0, " File  View ");
	nav_draw();
	{
		char info[320];
		int infolen;
		snprintf(info, sizeof(info), " %s  zoom %d%% ", filename, zoom);
		infolen = (int)strlen(info);
		if (infolen < cols)
			mvwprintw(s_win, STATUSBAR_ROW, cols - infolen, "%s", info);
	}
	wattroff(s_win, A_REVERSE);
	win_flush();
}

int menu_open(char *buf, int bufsz)
{
	return menubar_run(0, buf, bufsz);
}

int menu_handle_key(int key, char *buf, int bufsz, MenuMouse *mm)
{
	if (key == KEY_F(10))
		return menubar_run(0, buf, bufsz);
	if (key == KEY_MOUSE) {
		MEVENT ev;
		if (getmouse(&ev) != OK)
			return MENU_NONE;

		if (ev.y == STATUSBAR_ROW) {
			if (ev.bstate & BUTTON1_PRESSED) {
				int hit = bar_hit(ev.x);
				if (hit >= 0)
					return menubar_run(hit, buf, bufsz);
				{
					int action = nav_hit(ev.x);
					if (action != MENU_NONE)
						return dispatch_action(action, buf, bufsz);
				}
			}
			return MENU_NONE;
		}

		if (mm) { mm->row = ev.y; mm->col = ev.x; }
		if (ev.bstate & BUTTON2_PRESSED)   return MENU_MOUSE_ZOOM_IN;
		if (ev.bstate & BUTTON4_PRESSED)   return MENU_SCROLL_UP;
		if (ev.bstate & BUTTON5_PRESSED)   return MENU_SCROLL_DOWN;
		if (ev.bstate & BUTTON1_PRESSED)   return MENU_MOUSE_PRESS;
		if (ev.bstate & BUTTON1_RELEASED)  return MENU_MOUSE_RELEASE;
		if (ev.bstate & BUTTON1_DOUBLE_CLICKED) return MENU_MOUSE_ZOOM_IN;
		if (ev.bstate == REPORT_MOUSE_POSITION ||
		    (ev.bstate & BUTTON1_PRESSED))
			return MENU_MOUSE_DRAG;
	}
	return MENU_NONE;
}

int menu_readkey(void)
{
	int c = wgetch(stdscr);
	return c == ERR ? -1 : c;
}
