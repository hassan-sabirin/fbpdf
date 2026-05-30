#ifndef MENU_H
#define MENU_H

/* Actions returned by menu_open() and menu_handle_key() */
#define MENU_NONE		0
#define MENU_FILE_OPEN		1
#define MENU_FILE_EXIT		2
#define MENU_VIEW_ROTATE_CW	3
#define MENU_VIEW_ROTATE_CCW	4
#define MENU_VIEW_ZOOM_IN	5
#define MENU_VIEW_ZOOM_OUT	6
#define MENU_VIEW_FIT_WIDTH	7
#define MENU_VIEW_GOTO_PAGE	8
#define MENU_VIEW_SEARCH	9
/* Scroll-wheel on the document (within-page scroll). */
#define MENU_SCROLL_UP		10
#define MENU_SCROLL_DOWN	11
/* Navigation bar buttons. */
#define MENU_NAV_FIRST		12
#define MENU_NAV_PREV		13
#define MENU_NAV_PAGE		14	/* click on [N/M] opens Go to Page dialog */
#define MENU_NAV_NEXT		15
#define MENU_NAV_LAST		16
/* Mouse events on the document area, returned alongside MenuMouse data. */
#define MENU_MOUSE_PRESS	17	/* button-1 pressed on document */
#define MENU_MOUSE_DRAG		18	/* button-1 held and pointer moved */
#define MENU_MOUSE_RELEASE	19	/* button-1 released */
#define MENU_MOUSE_ZOOM_IN	20	/* ctrl+scroll-up or middle-click */
#define MENU_MOUSE_ZOOM_OUT	21	/* ctrl+scroll-down */

/*
 * Companion data for MENU_MOUSE_* actions.
 * col/row are in terminal character cells; the caller converts to pixels
 * using the terminal-to-framebuffer scale (fb_cols/term_cols etc.).
 */
typedef struct {
	int row;	/* terminal row of the event */
	int col;	/* terminal column of the event */
} MenuMouse;

/*
 * Initialise ncurses and draw the status bar.
 * Call once after fb_init().
 */
void menu_init(void);

/*
 * Tear down ncurses.  Call before fb_free().
 */
void menu_cleanup(void);

/*
 * Redraw the status bar with current file/page/zoom info.
 */
void menu_statusbar(const char *filename, int page, int pages, int zoom);

/*
 * Open the menu bar interactively.
 * Returns a MENU_* action constant.
 * For MENU_FILE_OPEN and MENU_VIEW_SEARCH the result string is written
 * into buf (size bufsz).  buf may be NULL for actions that need no string.
 */
int menu_open(char *buf, int bufsz);

/*
 * Process a key that was read outside the menu.
 * Returns a MENU_* action if the key triggers one, MENU_NONE otherwise.
 * For MENU_FILE_OPEN / MENU_VIEW_SEARCH the result is written into buf.
 * For MENU_MOUSE_* actions the companion position is written into *mm
 * (mm may be NULL if the caller does not need it).
 */
int menu_handle_key(int key, char *buf, int bufsz, MenuMouse *mm);

/*
 * Read one key via ncurses (replaces the old readkey / termios loop).
 * Returns the ncurses key value, or -1 on EOF.
 */
int menu_readkey(void);

#endif
