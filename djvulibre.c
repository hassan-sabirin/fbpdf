#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libdjvu/ddjvuapi.h>
#include <libdjvu/miniexp.h>
#include "doc.h"

#define MIN(a, b)	((a) < (b) ? (a) : (b))

struct doc {
	ddjvu_context_t *ctx;
	ddjvu_document_t *doc;
};

int djvu_handle(struct doc *doc)
{
	ddjvu_message_t *msg;
	msg = ddjvu_message_wait(doc->ctx);
	while ((msg = ddjvu_message_peek(doc->ctx))) {
		if (msg->m_any.tag == DDJVU_ERROR) {
			fprintf(stderr,"ddjvu: %s\n", msg->m_error.message);
			return 1;
		}
		ddjvu_message_pop(doc->ctx);
	}
	return 0;
}

static void djvu_render(ddjvu_page_t *page, int iw, int ih, void *bitmap)
{
	ddjvu_format_t *fmt;
	ddjvu_rect_t rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = iw;
	rect.h = ih;
	fmt = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, 0);
	ddjvu_format_set_row_order(fmt, 1);
	memset(bitmap, 0, ih * iw * 3);
	ddjvu_page_render(page, DDJVU_RENDER_COLOR,
				&rect, &rect, fmt, iw * 3, bitmap);
	ddjvu_format_release(fmt);
}

void *doc_draw(struct doc *doc, int p, int zoom, int rotate, int bpp, int *rows, int *cols)
{
	ddjvu_page_t *page;
	ddjvu_pageinfo_t info;
	int iw, ih, dpi;
	unsigned char *bmp;
	char *pbuf;
	int i, j;
	page = ddjvu_page_create_by_pageno(doc->doc, p - 1);
	if (!page)
		return NULL;
	while (!ddjvu_page_decoding_done(page))
		if (djvu_handle(doc))
			return NULL;
	if (rotate)
		ddjvu_page_set_rotation(page, (4 - (rotate / 90 % 4)) & 3);
	ddjvu_document_get_pageinfo(doc->doc, p - 1, &info);
	dpi = ddjvu_page_get_resolution(page);
	iw = ddjvu_page_get_width(page) * zoom / dpi;
	ih = ddjvu_page_get_height(page) * zoom / dpi;
	if (!(bmp = malloc(ih * iw * 3))) {
		ddjvu_page_release(page);
		return NULL;
	}
	djvu_render(page, iw, ih, bmp);
	ddjvu_page_release(page);
	if (!(pbuf = malloc(ih * iw * bpp))) {
		free(bmp);
		return NULL;
	}
	for (i = 0; i < ih; i++) {
		unsigned char *s = bmp + i * iw * 3;
		char *d = pbuf + (i * iw) * bpp;
		for (j = 0; j < iw; j++)
			fb_set(d + j * bpp, s[j * 3], s[j * 3 + 1], s[j * 3 + 2]);
	}
	free(bmp);
	*cols = iw;
	*rows = ih;
	return pbuf;
}

int doc_pages(struct doc *doc)
{
	return ddjvu_document_get_pagenum(doc->doc);
}

struct doc *doc_open(const char *path)
{
	struct doc *doc = malloc(sizeof(*doc));
	doc->ctx = ddjvu_context_create("fbpdf");
	if (!doc->ctx)
		goto fail;
	doc->doc = ddjvu_document_create_by_filename(doc->ctx, path, 1);
	if (!doc->doc)
		goto fail;
	while (!ddjvu_document_decoding_done(doc->doc))
		if (djvu_handle(doc))
			goto fail;
	return doc;
fail:
	doc_close(doc);
	return NULL;
}

void doc_close(struct doc *doc)
{
	if (doc->doc)
		ddjvu_document_release(doc->doc);
	if (doc->ctx)
		ddjvu_context_release(doc->ctx);
	free(doc);
}

/*
 * Recursively walk a miniexp page-text s-expression looking for a string
 * that contains keyword (case-insensitive).  Returns 1 if found.
 */
static int miniexp_search(miniexp_t expr, const char *keyword)
{
	if (miniexp_stringp(expr)) {
		const char *s = miniexp_to_str(expr);
		/* Case-insensitive substring search. */
		int klen = strlen(keyword);
		int slen = strlen(s);
		int i, j;
		for (i = 0; i <= slen - klen; i++) {
			for (j = 0; j < klen; j++)
				if (tolower((unsigned char)s[i + j]) !=
				    tolower((unsigned char)keyword[j]))
					break;
			if (j == klen)
				return 1;
		}
		return 0;
	}
	if (miniexp_listp(expr) && expr != miniexp_nil) {
		miniexp_t p;
		for (p = expr; p != miniexp_nil; p = miniexp_cdr(p))
			if (miniexp_search(miniexp_car(p), keyword))
				return 1;
	}
	return 0;
}

int doc_search(struct doc *doc, const char *keyword, int start_page)
{
	int pages = ddjvu_document_get_pagenum(doc->doc);
	int p;
	for (p = start_page; p <= pages; p++) {
		miniexp_t pagetext;
		/* Pump the message queue until the page text is ready. */
		while ((pagetext = ddjvu_document_get_pagetext(doc->doc,
				p - 1, "word")) == miniexp_dummy)
			djvu_handle(doc);
		if (pagetext == miniexp_nil)
			continue;
		if (miniexp_search(pagetext, keyword)) {
			miniexp_release(pagetext);
			return p;
		}
		miniexp_release(pagetext);
	}
	return 0;
}
