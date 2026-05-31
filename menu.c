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
 *
 * Rendered as:  |<  <  [N/M]  >  >|
 * Each button has a start column and width tracked in nav_btns[].
 * ---------------------------------------------------------------------- */
#define NAV_NBTNS	5

typedef struct {
	int action;
	int col;	/* filled in by nav_layout() */
	int width;	/* filled in by nav_layout() */
} NavBtn;

static NavBtn nav_btns[NAV_NBTNS] = {
	{ MENU_NAV_FIRST, 0, 0 },
	{ MENU_NAV_PREV,  0, 0 },
	{ MENU_NAV_PAGE,  0, 0 },	/* the [N/M] box */
	{ MENU_NAV_NEXT,  0, 0 },
	{ MENU_NAV_LAST,  0, 0 },
};

/* Labels for the fixed buttons; the page box is rendered dynamically. */
static const char *nav_labels[NAV_NBTNS] = {
	"|<", "<", NULL, ">", ">|"
};

/*
 * Compute nav button column positions from NAV_COL.
 * The page box width varies with page count; call whenever s_pages changes.
 */
static void nav_layout(void)
{
	char pagebuf[32];
	int pagew;
	int col = NAV_COL;

	snprintf(pagebuf, sizeof(pagebuf), "[%d/%d]", s_page, s_pages);
	pagew = (int)strlen(pagebuf);

	/* |<  (width 2) + space */
	nav_btns[0].col   = col;
	nav_btns[0].width = 2;
	col += 3;
	/* <   (width 1) + space */
	nav_btns[1].col   = col;
	nav_btns[1].width = 1;
	col += 2;
	/* [N/M] */
	nav_btns[2].col   = col;
	nav_btns[2].width = pagew;
	col += pagew + 1;
	/* >   (width 1) + space */
	nav_btns[3].col   = col;
	nav_btns[3].width = 1;
	col += 2;
	/* >|  (width 2) */
	nav_btns[4].col   = col;
	nav_btns[4].width = 2;
}

/*
 * Return the nav action for a given status-bar column, or MENU_NONE.
 */
static int nav_hit(int col)
{
	int i;
	for (i = 0; i < NAV_NBTNS; i++) {
		if (col >= nav_btns[i].col &&
		    col <  nav_btns[i].col + nav_btns[i].width)
			return nav_btns[i].action;
	}
	return MENU_NONE;
}

/* Draw just the navigation buttons onto the already-reverse-video status bar. */
static void nav_draw(void)
{
	char pagebuf[32];
	int i;

	snprintf(pagebuf, sizeof(pagebuf), "[%d/%d]", s_page, s_pages);

	for (i = 0; i < NAV_NBTNS; i++) {
		const char *label = (i == 2) ? pagebuf : nav_labels[i];
		mvprintw(STATUSBAR_ROW, nav_btns[i].col, "%s", label);
	}
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/*
 * Return which top-level menu index the given column falls on, or -1.
 * Each menu title occupies [col, col + strlen(title)).
 */
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

static void bar_draw(int highlight)
{
	int i;
	attron(A_REVERSE);
	move(STATUSBAR_ROW, 0);
	clrtoeol();
	/* Menu titles */
	for (i = 0; i < NMENUS; i++) {
		if (i == highlight)
			attroff(A_REVERSE);
		mvprintw(STATUSBAR_ROW, menus[i].col, "%s", menus[i].title);
		if (i == highlight)
			attron(A_REVERSE);
	}
	/* Navigation buttons */
	nav_draw();
	/* File info right-aligned */
	{
		char info[320];
		int infolen, cols;
		cols = getmaxx(stdscr);
		snprintf(info, sizeof(info), " %s  zoom %d%% ", s_filename, s_zoom);
		infolen = (int)strlen(info);
		if (infolen < cols)
			mvprintw(STATUSBAR_ROW, cols - infolen, "%s", info);
	}
	attroff(A_REVERSE);
	refresh();
}

/* Returns the width of the widest item label in a menu. */
static int dropdown_width(const Menu *m)
{
	int i, w = 0;
	for (i = 0; i < m->nitems; i++) {
		int l = (int)strlen(m->items[i].label);
		if (l > w)
			w = l;
	}
	return w + 2;	/* 1-char padding each side */
}

/*
 * Show a drop-down for menu m, pre-selecting item sel.
 * Returns the chosen action, or MENU_NONE if cancelled.
 * Returns -KEY_LEFT / -KEY_RIGHT to signal switching to adjacent menu.
 */
static int dropdown_run(const Menu *m, int sel)
{
	int w = dropdown_width(m);
	int h = m->nitems + 2;	/* border top + items + border bottom */
	int dcol = m->col;
	WINDOW *win;
	int i, c;

	/* Guard against zero/negative terminal dimensions (can happen on a
	 * framebuffer console before the terminal size is properly set). */
	if (LINES < DROPDOWN_ROW + h || COLS < dcol + w)
		return MENU_NONE;

	win = newwin(h, w, DROPDOWN_ROW, dcol);
	if (!win)
		return MENU_NONE;
	keypad(win, TRUE);
	clearok(win, TRUE);
	box(win, 0, 0);

	while (1) {
		for (i = 0; i < m->nitems; i++) {
			if (i == sel)
				wattron(win, A_REVERSE);
			mvwprintw(win, i + 1, 1, "%s", m->items[i].label);
			if (i == sel)
				wattroff(win, A_REVERSE);
		}
		wrefresh(win);

		c = wgetch(win);
		switch (c) {
		case KEY_UP:
			sel = (sel + m->nitems - 1) % m->nitems;
			break;
		case KEY_DOWN:
			sel = (sel + 1) % m->nitems;
			break;
		case '\n': case '\r': case KEY_ENTER:
			delwin(win);
			return m->items[sel].action;
		case KEY_LEFT:
			delwin(win);
			return -KEY_LEFT;
		case KEY_RIGHT:
			delwin(win);
			return -KEY_RIGHT;
		case 27:
			delwin(win);
			return MENU_NONE;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK)
				break;
			/* Click on the status bar — switch to that menu or close. */
			if (ev.y == STATUSBAR_ROW &&
			    (ev.bstate & BUTTON1_PRESSED)) {
				int hit = bar_hit(ev.x);
				delwin(win);
				if (hit >= 0 && hit != (int)(m - menus))
					/* Signal the caller to open that menu. */
					return -(KEY_RIGHT * 100 + hit);
				return MENU_NONE;
			}
			/* Click inside the drop-down content area. */
			if (ev.bstate & BUTTON1_PRESSED) {
				int item = ev.y - DROPDOWN_ROW - 1;
				if (ev.x >= dcol && ev.x < dcol + w &&
				    item >= 0 && item < m->nitems) {
					sel = item;
					if (ev.bstate & BUTTON1_DOUBLE_CLICKED) {
						delwin(win);
						return m->items[sel].action;
					}
				}
			}
			/* Scroll wheel inside the drop-down moves the highlight. */
			if (ev.bstate & BUTTON4_PRESSED)
				sel = (sel + m->nitems - 1) % m->nitems;
			if (ev.bstate & BUTTON5_PRESSED)
				sel = (sel + 1) % m->nitems;
			/* Click outside the menu area entirely — close. */
			if ((ev.bstate & BUTTON1_PRESSED) &&
			    ev.y > STATUSBAR_ROW &&
			    (ev.x < dcol || ev.x >= dcol + w)) {
				delwin(win);
				return MENU_NONE;
			}
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
	int rows, cols;
	int w, h, r, c;
	WINDOW *win;
	int ch, len;

	getmaxyx(stdscr, rows, cols);
	w = (int)strlen(prompt) + bufsz + 4;
	if (w > cols - 4)
		w = cols - 4;
	h = 3;
	r = rows / 2 - 1;
	c = (cols - w) / 2;

	win = newwin(h, w, r, c);
	if (!win)
		return 0;
	keypad(win, TRUE);
	clearok(win, TRUE);
	echo();
	curs_set(1);
	box(win, 0, 0);
	mvwprintw(win, 1, 1, "%s", prompt);
	wrefresh(win);

	buf[0] = '\0';
	len = 0;

	while (1) {
		ch = wgetch(win);
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
			buf[len] = '\0';
			break;
		}
		if (ch == 27) {
			buf[0] = '\0';
			len = 0;
			break;
		}
		if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
			len--;
			buf[len] = '\0';
			mvwprintw(win, 1, 1 + (int)strlen(prompt), "%-*s", bufsz - 1, buf);
			wmove(win, 1, 1 + (int)strlen(prompt) + len);
			wrefresh(win);
			continue;
		}
		if (ch >= 32 && ch < 127 && len < bufsz - 1) {
			buf[len++] = (char)ch;
			buf[len] = '\0';
			mvwprintw(win, 1, 1 + (int)strlen(prompt), "%s", buf);
			wrefresh(win);
		}
	}

	noecho();
	curs_set(0);
	delwin(win);
	return len > 0;
}

/*
 * Dispatch an action that may need a text-input dialog.
 * Returns the final action to report to the caller, or MENU_NONE.
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
 * Returns an action, or MENU_NONE if cancelled.
 * Writes into buf for actions that return a string.
 */
static int menubar_run(int start_menu, char *buf, int bufsz)
{
	int cur = start_menu;
	int action;

	/*
	 * On a framebuffer console, direct mmap writes to /dev/fb0 leave
	 * ncurses' internal screen model stale.  Force a full repaint so
	 * subsequent wrefresh() calls don't try to apply a delta against
	 * a screen image that no longer exists.
	 */
	clearok(stdscr, TRUE);
	bar_draw(cur);

	while (1) {
		int c = getch();

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
			/* dropdown_run encodes "switch to menu N" as -(KEY_RIGHT*100+N) */
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
		case 27: /* Escape */
			bar_draw(-1);
			return MENU_NONE;
		case KEY_MOUSE: {
			MEVENT ev;
			if (getmouse(&ev) != OK)
				break;
			if (ev.y == STATUSBAR_ROW &&
			    (ev.bstate & BUTTON1_PRESSED)) {
				int hit = bar_hit(ev.x);
				if (hit >= 0) {
					cur = hit;
					bar_draw(cur);
					goto open_dropdown;
				}
				/* Check nav buttons */
				action = nav_hit(ev.x);
				if (action != MENU_NONE) {
					bar_draw(-1);
					return dispatch_action(action, buf, bufsz);
				}
				/* Clicked the info area — close menu. */
				bar_draw(-1);
				return MENU_NONE;
			}
			/* Click outside the bar while no dropdown is open — close. */
			if (ev.y != STATUSBAR_ROW &&
			    (ev.bstate & BUTTON1_PRESSED)) {
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
	/*
	 * Enable mouse support. On a Linux framebuffer console this
	 * automatically uses GPM if the daemon is running.
	 * REPORT_MOUSE_POSITION delivers motion events while button-1 is
	 * held, enabling drag-to-pan.
	 */
	mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED |
	          BUTTON1_DOUBLE_CLICKED | BUTTON2_PRESSED |
	          BUTTON4_PRESSED | BUTTON5_PRESSED |
	          REPORT_MOUSE_POSITION, NULL);
}

void menu_cleanup(void)
{
	endwin();
}

void menu_statusbar(const char *filename, int page, int pages, int zoom)
{
	int cols = getmaxx(stdscr);
	int changed_pages = (pages != s_pages);

	snprintf(s_filename, sizeof(s_filename), "%s", filename);
	s_page  = page;
	s_pages = pages;
	s_zoom  = zoom;

	/* Recompute nav layout if page count changed (box width may shift). */
	if (changed_pages)
		nav_layout();

	attron(A_REVERSE);
	move(STATUSBAR_ROW, 0);
	clrtoeol();
	/* Menu titles */
	mvprintw(STATUSBAR_ROW, 0, " File  View ");
	/* Navigation buttons */
	nav_draw();
	/* File info right-aligned */
	{
		char info[320];
		int infolen;
		snprintf(info, sizeof(info), " %s  zoom %d%% ", filename, zoom);
		infolen = (int)strlen(info);
		if (infolen < cols)
			mvprintw(STATUSBAR_ROW, cols - infolen, "%s", info);
	}
	attroff(A_REVERSE);
	refresh();
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

		/* ---- Status bar row: menus, nav buttons ---- */
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

		/* ---- Document area ---- */
		if (mm) {
			mm->row = ev.y;
			mm->col = ev.x;
		}
		/* Middle-click zooms in; ctrl+scroll not reliably detectable
		 * in all terminals, so middle-click = zoom in, right-click
		 * (button3) = zoom out is the portable fallback. */
		if (ev.bstate & BUTTON2_PRESSED)
			return MENU_MOUSE_ZOOM_IN;
		/* Scroll wheel scrolls within the page. */
		if (ev.bstate & BUTTON4_PRESSED)
			return MENU_SCROLL_UP;
		if (ev.bstate & BUTTON5_PRESSED)
			return MENU_SCROLL_DOWN;
		/* Drag-to-pan: press, motion, release. */
		if (ev.bstate & BUTTON1_PRESSED)
			return MENU_MOUSE_PRESS;
		if (ev.bstate & BUTTON1_RELEASED)
			return MENU_MOUSE_RELEASE;
		/* Motion events arrive as REPORT_MOUSE_POSITION with no button
		 * bits set in some terminals, or with BUTTON1_PRESSED still set
		 * in others. Treat any motion on the document row as a drag. */
		if (ev.bstate & BUTTON1_DOUBLE_CLICKED)
			return MENU_MOUSE_ZOOM_IN;
		/* Pure motion (button held, position report) */
		if (ev.bstate == REPORT_MOUSE_POSITION ||
		    (ev.bstate & BUTTON1_PRESSED))
			return MENU_MOUSE_DRAG;
	}
	return MENU_NONE;
}

int menu_readkey(void)
{
	int c = getch();
	return c == ERR ? -1 : c;
}
