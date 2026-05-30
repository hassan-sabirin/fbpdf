struct doc *doc_open(const char *path);
int doc_pages(struct doc *doc);
void *doc_draw(struct doc *doc, int page, int zoom, int rotate, int bpp, int *rows, int *cols);
void doc_close(struct doc *doc);

/*
 * Search for keyword starting from start_page (1-based, inclusive).
 * Returns the page number of the first match, or 0 if not found.
 * Pass start_page > 1 to continue searching from a previous result.
 */
int doc_search(struct doc *doc, const char *keyword, int start_page);

void fb_set(char *d, unsigned r, unsigned g, unsigned b);
