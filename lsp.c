/*
 * lsp - list pages (or least significant pager)
 *
 * Copyright (C) 2023, Dirk Gouders
 *
 * This file is part of lsp.
 *
 * lsp is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * lsp is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * lsp. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Some definitions / explanation
 *
 * Files
 * -----
 *   In lsp, all data we want to page is stored in files (struct file_t):
 *   - files on disk
 *   - data from stdin (e.g. manual pages)
 *   - temporary files we use (e.g. list to switch to other file)
 *   - ...
 *
 *   These open files are kept in a ring structure (struct file_t)
 *   and only one such file is actively paged and pointed to by cf (or
 *   current_file).  Most functions just work on the current file,
 *   i.e. they use the pointer cf to do their work.
 *   To switch to another file, for example, we basically just need to modify
 *   cf.
 *
 *   File's data is stored in blocks of blksize reported by stat(2)
 *   and we try to actually read and store it only when needed.  For
 *   further processing data from those blocks is read by
 *   lsp_file_getch().
 *
 *   Paging then happens by processing file's data line-by-line
 *   (struct lsp_line_t).  Searches can occur on those lines, final
 *   output then happens char-by-char so that we can act on control
 *   sequences that are display attributes.
 *
 * Lines
 * -----
 *   When data blocks are read each block is inspected for newline
 *   characters and for each line it's start position in the file is
 *   stored in an array cf->lines.
 *
 *   Lines in cf->lines are indexed zero-based but for the outside
 *   world we start counting lines from 1 which means that a file with
 *   text without a newline in it consists of one line.  wc(1) differs
 *   here and reports line count 0 for such a file.
 *
 *   The above means: line 1 (index 0 in the array) always starts at
 *   position 0 in the file.  A constant value, yes.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <curses.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <pty.h>
#include <locale.h>
#include <sys/wait.h>
#include <ctype.h>
#include <getopt.h>
#include <assert.h>
#include <langinfo.h>
#include <search.h>

#include "lsp.h"

/*
 * Our memory allocators with error handling.
 */
static void *lsp_malloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr == NULL) {
		lsp_error("%s: %s\n", __func__, strerror(errno));
	}

	return ptr;
}

static void *lsp_calloc(size_t nmemb, size_t size)
{
	void *ptr = calloc(nmemb, size);

	if (ptr == NULL) {
		lsp_error("%s: %s\n", __func__, strerror(errno));
	}

	return ptr;
}

static void *lsp_realloc(void *ptr, size_t size)
{
	void *ret_ptr = realloc(ptr, size);

	if (ret_ptr == NULL) {
		lsp_error("%s: %s\n", __func__, strerror(errno));
	}

	return ret_ptr;
}

/*
 * Try to detect if file_t is a manual page.
 * Return a string xyz(n) if so, NULL otherwise.
 *
 * We are testing the first line of the current file
 * for a pattern like:
 *
 * "xxx(n)      some text       xxx(n)"
 *
 */
static char *lsp_detect_manpage()
{
	char *regex_mid = " {2,}.+ {2,}";
	char *ref_regex = lsp_search_ref_string;
	char *regex_str;
	regex_t preg;
	regmatch_t pmatch[1];
	struct lsp_line_t *line = NULL;

	line = lsp_get_line_at_pos(0);
	if (!line)
		return NULL;

	regex_str = lsp_calloc(1, 2 * strlen(ref_regex) + strlen(regex_mid) + 1);
	strcat(regex_str, ref_regex);
	strcat(regex_str, regex_mid);
	strcat(regex_str, ref_regex);

	int ret = regcomp(&preg, regex_str, REG_EXTENDED);

	if (ret != 0)
		lsp_error("%s: regcomp() failed: %d\n", __func__, ret);

	free(regex_str);

	ret = regexec(&preg, line->normalized, 1, pmatch, 0);

	regfree(&preg);

	if (ret != 0) {
		lsp_debug("%s: not a manual page \"%s\"", __func__, cf->name);
		lsp_line_dtor(line);
		return NULL;
	}

	char *name = strchr(line->normalized, ')');
	size_t len = name + 1 - line->normalized;
	line->normalized[len] = '\0';

	if (lsp_man_case_sensitivity)
		name = strdup(line->normalized);
	else
		name = lsp_to_lower(line->normalized);

	lsp_debug("%s: manual page detected \"%s\"", __func__, name);

	lsp_line_dtor(line);
	return name;
}

/*
 * lsp_file_getch() expects the data-buffer ring aligned to the buffer
 * containing the _last byte it served_ -- or the first one.
 *
 * So, after a series of functions happily altered cf->getch_pos we
 * should finally be called to clean up the mess that all caused.
 *
 * And that is steered by the flag "unaligned".
 *
 */
static void lsp_file_align_buffer()
{
	struct data_t *here = cf->data;
	off_t i = lsp_pos - 1;

	if (cf->unaligned == 0)
		return;

	if (i == (off_t)-1)
		i = 0;

	/* Search for the buffer that contains lsp_pos - 1. */
	while (1) {
		if ((i >= cf->data->seek) &&
		    (i < (cf->data->seek + cf->blksize)))
			break;

		if (i < cf->data->seek)
			cf->data = cf->data->prev;
		else
			cf->data = cf->data->next;

		if (here == cf->data)
			lsp_error("%s: Endless loop while aligning buffers.\n",
				  __func__);
	}

	cf->unaligned = 0;
}

/*
 * Unget the last received byte of current file.
 */
static void lsp_file_ungetch()
{
	if (lsp_pos > 0)
		lsp_file_set_pos(lsp_pos - 1);
}

/*
 * Move to the beginning of the current line.
 */
static void lsp_goto_bol()
{
	while (lsp_pos > 0 && lsp_file_peek_bw() != '\n')
		lsp_file_ungetch();
}

/*
 * Calculate length of SGR sequence and check for unknown characters.
 *
 * Return length or -1 if sequence is invalid.
 */
static size_t lsp_get_sgr_len(const char *seq)
{
	size_t sgr_len;

	for (sgr_len = 2; seq[sgr_len] != 'm'; sgr_len++) {
		if (seq[sgr_len] == ';' ||
		    (seq[sgr_len] >= '0' && seq[sgr_len] <= '9'))
			continue;

		if (seq[sgr_len] == '<') {
			/* Private sequence (first seen in Linus' git tree).
			   We deactivate it by ignoring the '\e' char:
			   return 1 which means the sequence consists of the
			   single character '\e'. */
			return 1;
		}

		lsp_error("%s: unknown char \"%c\" in SGR sequence \"\\e%s\".\n",
			  __func__, seq[sgr_len], seq+1);

	}

	return sgr_len + 1;
}

/*
 * Decode SGR sequence to attribute and/or color pair.
 *
 * Return the length of the processed SGR sequence.
 *
 * Note: only a subset of SGR parameters is implemented!
 */
static size_t lsp_decode_sgr(const char *seq, attr_t *attr, short *pair)
{
	char *sgr_endparam_p;
	size_t sgr_len;
	size_t i;
	int n;
	short sgr_fg_color;
	short sgr_bg_color;

	sgr_len = lsp_get_sgr_len(seq);

	if (sgr_len == (size_t)-1)
		return sgr_len;

	if (sgr_len == 1)
		/* Ignore/deactivate the sequence. */
		return sgr_len;

	if (sgr_len == 3) {
		/* SGR = "\e[m" */
		*attr = A_NORMAL;
		*pair = LSP_DEFAULT_PAIR;
		return sgr_len;
	}

	/* Get currently active colors in pair. */
	pair_content(*pair, &sgr_fg_color, &sgr_bg_color);

	/* We start after the initial \e[ */
	i = 2;

	while (1) {
		/* Get next parameter in sequence */
		n = strtol(seq + i, &sgr_endparam_p, 10);

		switch(n) {
		case 0:
			/* Reset */
			*attr = A_NORMAL;
			pair_content(LSP_DEFAULT_PAIR,
				     &sgr_fg_color, &sgr_bg_color);
			break;
		case 1:
			*attr = A_BOLD;
			pair_content(LSP_BOLD_PAIR,
				     &sgr_fg_color, &sgr_bg_color);
			break;
		case 2:
			*attr = A_DIM;
			break;
		case 3:
			*attr = A_ITALIC;
			break;
		case 4:
			*attr = A_UNDERLINE;
			pair_content(LSP_UL_PAIR,
				     &sgr_fg_color, &sgr_bg_color);
			break;
		case 5:
		case 6:
			*attr = A_BLINK;
			break;
		case 7:
			*attr = A_REVERSE;
			pair_content(LSP_REVERSE_PAIR,
				     &sgr_fg_color, &sgr_bg_color);
			break;
		case 8:
			*attr = A_INVIS;
			break;
		case 9:
			/* strikethrough could be left as is and
			   handled by the terminal... */
		case 21:
			*attr = A_UNDERLINE;
			pair_content(LSP_UL_PAIR, &sgr_fg_color, &sgr_bg_color);
			break;
		case 22:
			*attr &= ~(A_BOLD | A_DIM);
			sgr_fg_color = lsp_fg_color_default;
			sgr_bg_color = lsp_bg_color_default;
			break;
		case 24:
			*attr &= ~A_UNDERLINE;
			sgr_fg_color = lsp_fg_color_default;
			sgr_bg_color = lsp_bg_color_default;
			break;
		case 30:
			/* fg black */
			sgr_fg_color = COLOR_BLACK;
			break;
		case 31:
			/* fg red */
			sgr_fg_color = COLOR_RED;
			break;
		case 32:
			/* fg green */
			sgr_fg_color = COLOR_GREEN;
			break;
		case 33:
			/* fg yellow */
			sgr_fg_color = COLOR_YELLOW;
			break;
		case 34:
			/* fg blue */
			sgr_fg_color = COLOR_BLUE;
			break;
		case 35:
			/* fg magenta */
			sgr_fg_color = COLOR_MAGENTA;
			break;
		case 36:
			/* fg cyan */
			sgr_fg_color = COLOR_CYAN;
			break;
		case 37:
			/* fg white */
			sgr_fg_color = COLOR_WHITE;
			break;
		case 39:
			/* fg default */
			sgr_fg_color = lsp_fg_color_default;
			break;
		case 40:
			/* bg black */
			sgr_bg_color = COLOR_BLACK;
			break;
		case 41:
			/* bg red */
			sgr_bg_color = COLOR_RED;
			break;
		case 42:
			/* bg green */
			sgr_bg_color = COLOR_GREEN;
			break;
		case 43:
			/* bg yellow */
			sgr_bg_color = COLOR_YELLOW;
			break;
		case 44:
			/* bg blue */
			sgr_bg_color = COLOR_BLUE;
			break;
		case 45:
			/* bg magenta */
			sgr_bg_color = COLOR_MAGENTA;
			break;
		case 46:
			/* bg cyan */
			sgr_bg_color = COLOR_CYAN;
			break;
		case 47:
			/* bg white */
			sgr_bg_color = COLOR_WHITE;
			break;
		case 49:
			/* bg default */
			sgr_bg_color = lsp_bg_color_default;
			break;
		case 90:
			/* fg black */
			sgr_fg_color = COLOR_BLACK + 8;
			break;
		case 91:
			/* fg red */
			sgr_fg_color = COLOR_RED + 8;
			break;
		case 92:
			/* fg green */
			sgr_fg_color = COLOR_GREEN + 8;
			break;
		case 93:
			/* fg yellow */
			sgr_fg_color = COLOR_YELLOW + 8;
			break;
		case 94:
			/* fg blue */
			sgr_fg_color = COLOR_BLUE + 8;
			break;
		case 95:
			/* fg magenta */
			sgr_fg_color = COLOR_MAGENTA + 8;
			break;
		case 96:
			/* fg cyan */
			sgr_fg_color = COLOR_CYAN + 8;
			break;
		case 97:
			/* fg white */
			sgr_fg_color = COLOR_WHITE + 8;
			break;
		case 100:
			/* bg black */
			sgr_bg_color = COLOR_BLACK + 8;
			break;
		case 101:
			/* bg red */
			sgr_bg_color = COLOR_RED + 8;
			break;
		case 102:
			/* bg green */
			sgr_bg_color = COLOR_GREEN + 8;
			break;
		case 103:
			/* bg yellow */
			sgr_bg_color = COLOR_YELLOW + 8;
			break;
		case 104:
			/* bg blue */
			sgr_bg_color = COLOR_BLUE + 8;
			break;
		case 105:
			/* bg magenta */
			sgr_bg_color = COLOR_MAGENTA + 8;
			break;
		case 106:
			/* bg cyan */
			sgr_bg_color = COLOR_CYAN + 8;
			break;
		case 107:
			/* bg white */
			sgr_bg_color = COLOR_WHITE + 8;
			break;
		default:
			lsp_debug("%s: currently unhandled SGR parameter %ld",
				  __func__, n);
		} /* switch() */

		if (*sgr_endparam_p == 'm') {
			*pair = lsp_get_color_pair(sgr_fg_color, sgr_bg_color);
			break;
		}

		i += (sgr_endparam_p + 1) - (seq + i);
	}

	return sgr_len;
}

/*
 * Get color pair with specified fg and bg color.
 * Init a new one if it does not exist.
 */
static short lsp_get_color_pair(short fg, short bg)
{
	short pair_fg;
	short pair_bg;

	for (short pair = 0; pair < lsp_next_pair; pair++) {
		pair_content(pair, &pair_fg, &pair_bg);

		if (pair_fg == fg && pair_bg == bg)
			return pair;
	}

	/* No matching pair yet -> create a new one. */
	init_pair(lsp_next_pair, fg, bg);

	return lsp_next_pair++;
}

/*
 * Check if c starts an SGR sequence in the current file.
 *
 * c points to the current char in a line being processed.
 */
static bool lsp_is_sgr_sequence(const char *c)
{
	if (*c != '\e')
		return false;

	if (*(c + 1) != '[')
		return false;

	return true;
}

/*
 * Tell if current position is at the beginning of a line.
 */
static bool lsp_is_at_bol()
{
	/* We define the beginning of a file to be the beginning of
	   a (the first) line, as well. */
	return lsp_pos <= 0 || lsp_file_peek_bw() == '\n';
}

/*
 * Create TOC for active file.
 *
 * We create three levels containing lines with increasing
 * indentation: none, 4, 8.
 *
 * This was implemented for manual pages in which case we really get
 * kind of a handy TOC.	 But, because we are just hiding blocks of text
 * this actually should be called "folding", perhaps...
 */
static void lsp_file_create_toc()
{
	if (cf->toc) {
		/* TOC already exists. */
		if (cf->toc_first)
			lsp_toc_rewind(cf->toc_first->pos);
		else
			lsp_toc_rewind(0);

		return;
	}

	off_t pos_save = lsp_pos;

	lsp_file_set_pos(0);

	struct lsp_line_t *line = lsp_get_this_line();

	while (line) {
		/* Level 0: all lines with other than space as the
		   first character. */
		if (line->normalized[0] != ' ' && line->normalized[0] != '\n')
			lsp_file_toc_add(line, 0);

		/* Level 1: all lines that start with three spaces. */
		if (!strncmp(line->normalized, "   ", 3) &&
		    line->normalized[3] != ' ')
			lsp_file_toc_add(line, 1);

		/* Level 2: all lines starting with seven spaces and
		   their following line with indentation of
		   at least eleven spaces. */
		if (!strncmp(line->normalized, "       ", 7) &&
		    line->normalized[7] != ' ') {

			lsp_line_dtor(line);
			line = lsp_get_this_line();

			if (!strncmp(line->normalized, "           ", 11)) {
				lsp_line_dtor(line);
				lsp_file_set_prev_line();
				line = lsp_file_get_prev_line();
				lsp_file_toc_add(line, 2);
			} else {
				lsp_file_set_prev_line();
			}
		}

		lsp_line_dtor(line);
		line = lsp_get_this_line();
	}

	lsp_line_dtor(line);

	lsp_file_set_pos(pos_save);

	lsp_toc_rewind(0);
}

/*
 * Add entry to TOC
 * We are assuming that a TOC is created from top to bottom and
 * point cf->toc to the last added entry so that we just need to add
 * the new one at that position.
 *
 * Later, the TOC needs a rewind to start displaying it from top to
 * bottom.
 */
static void lsp_file_toc_add(const struct lsp_line_t *line, int level)
{
	struct toc_node_t *toc_new_p;

	lsp_debug("%s: adding toc line level %d: \"%s\"",
		  __func__, level, line->normalized);

	if (!cf->toc) {
		cf->toc = lsp_calloc(1, sizeof(struct toc_node_t));
		cf->toc->pos = line->pos;
		cf->toc->level = level;
		return;
	}

	/* Ensure lines are added in strictly ascending order. */
	if (line->pos <= cf->toc->pos)
		lsp_error("%s: TOC must be created top down (%ld after %ld).\n",
			  __func__, line->pos, cf->toc->pos);

	toc_new_p = lsp_calloc(1, sizeof(struct toc_node_t));

	toc_new_p->pos = line->pos;
	toc_new_p->level = level;
	toc_new_p->prev = cf->toc;

	cf->toc->next = toc_new_p;
	cf->toc = toc_new_p;
}

/*
 * Destructor for TOC entries.
 */
static void lsp_toc_dtor(struct file_t *file)
{
	struct toc_node_t *toc_p;

	if (!file->toc)
		return;

	/* Rewind TOC to first entry. */
	while (file->toc->prev)
		file->toc = file->toc->prev;

	/* free() all TOC entries. */
	while (file->toc) {
		toc_p = file->toc->next;
		free(file->toc);
		file->toc = toc_p;
	}

	file->toc_first = NULL;
	file->toc_last = NULL;
}

/*
 * Reposition TOC to entry to pos.
 *
 * pos == -1 means:
 *    Position it to the end so that a full last page can be
 *    displayed.
 */
static void lsp_toc_rewind(off_t pos)
{
	if (!cf->toc)
		lsp_error("%s: called without active TOC for %s\n",
			  __func__, cf->name);

	if (pos == (off_t)-1) {
		/* Go to end */
		while (cf->toc->next)
			cf->toc = cf->toc->next;
		lsp_toc_bw(lsp_maxy - 2);
	} else {
		if (!lsp_pos_is_toc(pos))
			lsp_error("%s: called with invalid TOC position %ld\n",
				  __func__, pos);

		/* Go to pos */
		while (cf->toc->pos != pos) {
			if (cf->toc->pos < pos) {
				/* pos is right from current entry. */
				if (cf->toc->next)
					cf->toc = cf->toc->next;
				else
					break;
			} else {
				/* pos is left from current entry. */
				if (cf->toc->prev)
					cf->toc = cf->toc->prev;
				else
					break;
			}
		}
	}
}

/*
 * Translate cursor line of TOC to absolute file offset.
 */
static off_t lsp_toc_get_offset_at_cursor()
{
	struct toc_node_t *current_toc = cf->toc;
	size_t count = 0;
	off_t ret_pos;

	lsp_toc_rewind(cf->toc_first->pos);

	while (count != cf->toc_cursor) {
		cf->toc = cf->toc->next;
		if (cf->toc->level <= cf->current_toc_level)
			count++;
	}

	ret_pos = cf->toc->pos;

	cf->toc = current_toc;

	return ret_pos;
}

/*
 * Position current TOC entry n lines backward.
 */
static void lsp_toc_bw(size_t n)
{
	while (cf->toc->prev && n) {
		cf->toc = cf->toc->prev;
		if (cf->toc->level <= cf->current_toc_level)
			n--;
	}

	cf->toc_first = cf->toc;
}

/*
 * Position current TOC entry n lines forward.
 */
static void lsp_toc_fw(size_t n)
{
	while (cf->toc->next && n) {
		cf->toc = cf->toc->next;
		if (cf->toc->level <= cf->current_toc_level)
			n--;
	}

	cf->toc_first = cf->toc;
}

/*
 * Return previous line from current position or NULL if current
 * position is in first line.
 *
 * Note: this function will return the same line when it is called
 *	 repeatedly without modifying lsp_pos.	This is, because
 * if we read all bytes of the previous line (with respet to the current line),
 * after that, the pointer will finally be back at the first byte of the current
 * line.
 *
 * So, to read backward several lines use a pattern like:
 *
 * lsp_file_get_prev_line();
 * lsp_file_set_prev_line();
 */
static struct lsp_line_t *lsp_file_get_prev_line()
{
	lsp_goto_bol();

	if (lsp_pos <= 0)
		return NULL;

	if (lsp_mode_is_toc()) {
		if (lsp_toc_move_to_prev())
			return NULL;
		lsp_file_set_pos(cf->toc->pos);
	} else
		lsp_file_set_prev_line();

	return lsp_get_this_line();
}

/*
 * Move to the beginning of the previous line.
 */
static void lsp_file_set_prev_line()
{
	int ch;

	/* First go to beginning of this line */
	lsp_goto_bol();

	do {
		lsp_file_ungetch();
		ch = lsp_file_peek_bw();
	} while (ch != '\n' && ch != -1);

}

/*
 * Peek forward to the next byte that getch() would give us.
 */
int lsp_file_peek_fw()
{
       int ch = lsp_file_getch();
       if (ch != -1)
               lsp_file_ungetch();
       return ch;
}

/*
 * What was the last byte again?
 */
static int lsp_file_peek_bw()
{
	if (lsp_pos <= 0)
		return -1;	/* Beginning of file */

	lsp_file_align_buffer();

	if ((lsp_pos - 1) >= cf->data->seek)
		/* The byte is in the current buffer */
		return cf->data->buffer[(lsp_pos - 1) % cf->blksize];
	else
		/* The byte was the last one in the previous buffer */
		return cf->data->prev->buffer[cf->blksize - 1];
}

/*
 * Get the next byte from current file or -1 on EOF.
 */
static int lsp_file_getch()
{
	/* We use internal recursion but the maximum depth should be 1.
	   Otherwise we don't understand what we are doing. */
	static int once = 0;
again:
	if (cf->size != LSP_FSIZE_UNKNOWN &&
	    (lsp_pos == -1 || lsp_pos == cf->size))
		return -1; /* EOF */

	lsp_file_align_buffer();

	/* Position to next buffer if necessary and we already read it. */
	if (lsp_pos == (cf->data->seek + cf->blksize))
		if (cf->data->next->seek == (cf->data->seek + cf->blksize))
			cf->data = cf->data->next;

	if (cf->seek > lsp_pos && lsp_pos < (cf->data->seek + cf->blksize)) {
		if (lsp_pos < cf->data->seek)
			lsp_error("%s: problem with buffer ring!\n"
				  "lsp_pos  = %ld, cf->data->seek = %ld\n",
				  __func__, lsp_pos, cf->data->seek);
		/* Next byte is in this buffer */
		off_t i = lsp_pos % cf->blksize;
		cf->getch_pos += 1;
		once = 0;
		return cf->data->buffer[i];
	} else {
		/* Read next block from file. */
		lsp_file_add_block();

		if (once > 0)
			lsp_error("%s: unexpected recursion.\n", __func__);

		once++;	 /* Watch recursion depth */
		goto again;
	}
}

/*
 * Constructor for line structure.
 */
static struct lsp_line_t *lsp_line_ctor()
{
	struct lsp_line_t *line = lsp_malloc(sizeof(*line));

	line->pos = line->len = line->nlen = 0;

	line->raw = NULL;
	line->normalized = NULL;

	line->n_scr_line = 1;
	line->scr_line = lsp_malloc(sizeof(line->scr_line[0]));
	line->scr_line[0] = 0;	/* The beginning of a line also is the
				 * beginning of a screen line. */

	return line;
}

/*
 * Destructor for line structure.
 */
static void lsp_line_dtor(struct lsp_line_t *line)
{
	if (!line)
		return;

	free(line->raw);
	free(line->normalized);
	free(line->scr_line);
	free(line);
}

/*
 * Move to previous currently valid TOC entry from current file position.
 *
 * Return 0 on success and != 0 if we don't find a proper TOC entry before the
 * current position in the file.
 */
static int lsp_toc_move_to_prev()
{
	int ret_val = -1;
	struct toc_node_t *old_toc = cf->toc;

	/* Move back in TOC to the entry with pos < current file pos and
	   matching level.
	   The worst that can happen is that we reach the first TOC entry. */
	while (cf->toc &&
	       (cf->toc->pos >= lsp_pos || cf->toc->level > cf->current_toc_level))
		cf->toc = cf->toc->prev;

	if (cf->toc)
		ret_val = 0;
	else
		/* No entry found; restore old TOC position. */
		cf->toc = old_toc;

	return ret_val;
}

/*
 * Move to next currently valid TOC entry from current file position.
 *
 * Return 0 on success and != 0 if we don't find a proper TOC entry beyond the
 * current position in the file.
 */
static int lsp_toc_move_to_next()
{
	struct toc_node_t *old_toc = cf->toc;

	/* Move back in TOC to the entry with pos <= current file pos.
	   The worst that can happen is that we reach the first TOC entry. */
	while (cf->toc->pos > lsp_pos && cf->toc->prev)
		cf->toc = cf->toc->prev;

	/* Move forward until we find a proper entry or the end. */
	while (cf->toc->pos <= lsp_pos || cf->toc->level > cf->current_toc_level)
		if (!cf->toc->next) {
			/* Restore old TOC entry and return failure. */
			cf->toc = old_toc;
			return -1;
		} else {
			cf->toc = cf->toc->next;
		}

	return 0;
}

/*
 * Check if the given file position corresponds to a proper TOC entry.
 */
static bool lsp_pos_is_toc(off_t pos)
{
	if (lsp_pos_to_toc(pos))
		return true;

	return false;
}

/*
 * For the given position: return the currently active TOC entry.
 *
 * Return NULL if it doesn't exist.
 */
static struct toc_node_t *lsp_pos_to_toc(off_t pos)
{
	struct toc_node_t *ret = NULL;

	/* Safe current TOC entry and file position. */
	struct toc_node_t *old_toc = cf->toc;
	off_t old_pos = lsp_pos;

	/* Position file to the given position. */
	lsp_file_set_pos(pos);

	/* Go to the beginning of the current line and try to find that position
	   in the TOC entries. */
	lsp_goto_bol();

	/* Search for TOC entry starting at current position. */
	if (cf->toc->pos > pos)
		while (cf->toc->pos > lsp_pos && cf->toc->prev)
			cf->toc = cf->toc->prev;
	else
		while (cf->toc->pos < lsp_pos && cf->toc->next)
			cf->toc = cf->toc->next;

	/* Check if TOC entry matches correct position and current TOC level. */
	if (cf->toc->pos == lsp_pos &&
	    cf->toc->level <= cf->current_toc_level)
		ret = cf->toc;

	/* Restore original file position and TOC entry. */
	lsp_file_set_pos(old_pos);
	cf->toc = old_toc;

	return ret;
}

/*
 * Return full line that includes the current position.
 */
static struct lsp_line_t *lsp_get_this_line() {
	/* Return NULL if we reached EOF. */
	if (cf->size != LSP_FSIZE_UNKNOWN &&
	    (lsp_pos == -1 || lsp_pos == cf->size))
		return NULL;

	/* Make sure we are at the beginning of the current line */
	lsp_goto_bol();

	return lsp_get_line_from_here();
}

/*
 * One line might be longer than the current screen width and thus consist of
 * several lines on the screen.
 *
 * For the current screen width: add pointers to screen lines for the given
 * line.
 *
 * Caution, the line could contain tabs that would make the expanded line much
 * longer than line->len pretends!
 */
static void lsp_line_add_screen_lines(struct lsp_line_t *line)
{
	size_t i = 0;		  /* current byte in the line */
	size_t current_len = 0;	  /* current characters in one screen line */
	size_t scrlp = 0;	  /* screen line pointer (index) */
	char new_screen_line = 0; /* to identify parts containing just a newline */

	while (i < line->len) {
		if (current_len == lsp_maxx) {
			/* Add onother screen line. */
			scrlp += 1;
			line->n_scr_line += 1;
			line->scr_line = lsp_realloc(line->scr_line, line->n_scr_line);
			line->scr_line[scrlp] = i;
			current_len = 0;

			new_screen_line = 1;
		}
		/* Eat up possible SGR sequences */
		while (lsp_is_sgr_sequence(line->raw + i))
			i += lsp_get_sgr_len(line->raw + i);

		/* If we are in a new screen line and it consists of just a
		   newline (plus possible SGR sequences) we don't count this
		   line and are done. */
		if (new_screen_line && line->raw[i] == '\n') {
			line->n_scr_line -= 1;
			return;
		}

		new_screen_line = 0;

		size_t ch_len;

		while (1) {
			/* Proceed with next (possibly multibyte) character */
			ch_len = lsp_mbtowc(NULL, line->raw + i, line->len - i);

			/* Ignore \b sequences */
			if (line->raw[i + ch_len] == '\b') {
				/* Ignore this char and the following '\b' */
				i += ch_len + 1;
				continue;
			}

			break;
		}

		if (line->raw[i] == '\t') {
			current_len += lsp_expand_tab(current_len);
		} else
			current_len += 1;

		i += ch_len;
	}

	return;
}

/*
 * Calculate width of tab at given horizontal position.
 */
static int lsp_expand_tab(size_t x_pos)
{
	return lsp_tab_width - (x_pos % lsp_tab_width);
}

/*
 * Return the line from the current position inside the file.
 * i.e. the current position could be different from the beginning of a line.
 */
static struct lsp_line_t *lsp_get_line_from_here()
{
	off_t pos = 0;
	size_t size = 0;
	char *str = NULL;
	struct lsp_line_t *line = NULL;

	/* Return NULL if we reached EOF. */
	if (cf->size != LSP_FSIZE_UNKNOWN &&
	    (lsp_pos == -1 || lsp_pos == cf->size))
		return NULL;

	pos = lsp_pos;

	/* Don't return a line for a trailing newline in the file. */
	int ch = lsp_file_getch();
	if (ch == -1)
		return NULL;

	line = lsp_line_ctor();
	line->pos = pos;

	pos = 0;
	size = 128;
	str = lsp_malloc(size);

	while (ch != -1) {
		if (pos + 1 >= size) {
			str = lsp_realloc(str, size * 2);
			size *= 2;
		}

		str[pos++] = ch;
		str[pos] = '\0';

		if (ch == '\n')
			break;

		/* To deactivate the bad effects of '\r' convert it to the
		   printable characters "^M". */
		if (ch == '\r') {
			str[pos - 1] = '^';
			ch = 'M';
			continue;
		}

		/* A '\0' also is a character we won't take as is - it
		   would terminate our raw string.  First seen in bash's `git
		   log -p`.
		   If the following character is a \b, we replace the
		   '\0' with the character after \b which makes it the bold
		   version of it.  Otherwise we replace '\0' by "^@". */
		if (ch == '\0') {
			if (lsp_file_peek_fw() == '\b') {
				ch = lsp_file_getch();
				str[pos - 1] = lsp_file_peek_fw();
				continue;
			}

			str[pos - 1] = '^';
			ch = '@';
			continue;
		}

		ch = lsp_file_getch();
	}

	/* Reallocate to correct size */
	str = lsp_realloc(str, pos + 1);

	assert(strlen(str) == pos);

	line->len = pos;
	line->raw = str;
	line->normalized = lsp_normalize(str, pos);
	line->nlen = strlen(line->normalized);

	return line;
}

/*
 * Do a pseudo-normalization until the normalized string would have
 * the given length.  Return the length, to which the raw string was
 * processed to achieve this.
 */
static size_t lsp_normalize_count(const char *str, size_t length)
{
	size_t nlen;
	size_t i;

	if (!length)
		return 0;

	if (length > strlen(str))
		lsp_error("%s: length %ld > than strlen(str) %ld\n"
			  "str: \"%s\"\n", __func__, length, strlen(str), str);

	/* Process the string ignoring <char>\b sequences and SGR sequences. */
	for (i = 0, nlen = 0; nlen < length; i++) {

		if (str[i + 1] == '\b') {
			/* Ignore this c and the following \b */
			i++;
			continue;
		}

		while (lsp_is_sgr_sequence(str + i)) {
			i += lsp_get_sgr_len(str + i);
		}

		nlen++;
	}

	return i;
}

/*
 * Return a normalized version of the raw str up to length.
 *
 * Old man(1) style content works with \b (backspace):
 * - italics c is _\bc
 * - bold c is c\bc
 * We simply ignore all characters that are followed by a \b which
 * should give us the pure text.
 *
 * SGR sequences are also recognized and ignored.
 */
static char *lsp_normalize(const char *str, size_t length)
{
	char *normalized;
	size_t nlen;
	size_t i;

	/* We should be allocating too much memory, because the worst we do is
	   to ignore characters from the raw string.
	   We correct the allocated size below. */
	normalized = lsp_malloc(length + 1);

	/* Copy the string ignoring c\b sequences */
	for (i = 0, nlen = 0; i < length; i++) {
		if (str[i+1] == '\b') {
			/* Ignore this c and the following \b */
			i++;
			continue;
		}

		while (lsp_is_sgr_sequence(str + i)) {
			i += lsp_get_sgr_len(str + i);
		}

		normalized[nlen++] = str[i];
	}

	normalized[nlen++] = '\0';

	/* Adjust the allocated memory to the correct size */
	if ((length + 1) > nlen)
		normalized = lsp_realloc(normalized, nlen);

	/* ...or error out if our heuristics failed. */
	if ((length + 1) < nlen)
		lsp_error("Allocated only %ld bytes for string of %ld bytes\n",
			  length + 1, nlen);

	return normalized;
}

/*
 * Close the input fd for current_file.
 */
static void lsp_file_close()
{
	/* For popen()ed files, we need to call pclose()
	   on a FILE * pointer. */
	if (cf->flags & LSP_FLAG_POPEN) {
		if (cf->fp != NULL) {
			pclose(cf->fp);
			cf->fp = NULL;
			cf->fd = -1;
		}
	} else if (cf->fd != -1) {
		close(cf->fd);
		cf->fd = -1;
	}
}

/*
 * Add a line of text to a file.
 *
 * Our assumption: this is a small internal file that we use e.g. for
 * a list of all open files and it's content can be held in one single
 * block of memory.
 */
static ssize_t lsp_file_add_line(const char *line)
{
	size_t line_len;

	for (line_len = 1; line[line_len - 1] != '\n'; line_len++)
		;		/* just count */

	if (cf->data == NULL) {
		cf->data = lsp_malloc(sizeof(struct data_t));
		cf->data->prev = cf->data->next = cf->data;
		cf->data->buffer = NULL;
		cf->data->seek = 0;
	}

	if (cf->size == LSP_FSIZE_UNKNOWN)
		cf->size = 0;

	cf->data->buffer = lsp_realloc(cf->data->buffer, cf->size + line_len);

	memcpy(cf->data->buffer + cf->size, line, line_len);

	lsp_lines_add(cf->size);
	cf->size += line_len;
	cf->blksize = cf->size;
	cf->seek = cf->size;

	return line_len;
}

/*
 * Calculate unused space in data buffer.
 */
static size_t lsp_buffer_free_size()
{
	return (cf->data == NULL ? 0 : cf->blksize - (cf->seek - cf->data->seek));
}

/*
 * Constructer for new data buffer for current file.
 */
static void lsp_file_data_ctor(size_t size_to_read)
{
	struct data_t *new_data = lsp_malloc(sizeof(struct data_t));

	new_data->seek = cf->seek; /* Position this data block was read from */
	new_data->buffer = lsp_malloc(size_to_read);

	if (cf->data == NULL) {
		/* First buffer in ring */
		cf->data = new_data;
		cf->data->prev = cf->data->next = new_data;
	} else {
		new_data->prev = cf->data;
		new_data->next = cf->data->next;
		cf->data->next = new_data;
		new_data->next->prev = new_data;
		cf->data = new_data;
		cf->unaligned = 1;
	}
}

/*
 * Add a block of data to the current file
 */
static ssize_t lsp_file_read_block(size_t size_to_read)
{
	if (lsp_buffer_free_size()) {
		/* Current buffer is not filled up.
		   Use it to read more data into memory. */
		if (size_to_read > lsp_buffer_free_size())
			size_to_read = lsp_buffer_free_size();
	} else {
		lsp_file_data_ctor(size_to_read);
	}

	/* Fill the buffer */
	ssize_t nread = 0;
	unsigned char *buffer_p = cf->data->buffer + (cf->blksize - lsp_buffer_free_size());

	/*
	 * Check if we need to consume a byte that was read from a preprocessor
	 * pipe.  Store that byte as the first one and emulate that we already
	 * read one byte.
	 */
	if (cf->seek == 0 && cf->flags & LSP_PRE_READ) {
		buffer_p[0] = (char)cf->pre_read;
		buffer_p += 1;
		size_to_read -= 1;
		nread = 1;
	}

	nread += read(cf->fd, buffer_p, size_to_read);

	if (nread == -1) {
		lsp_debug("%s: input file %s: %s",
			  __func__, cf->name, strerror(errno));

		/* When we read from ptmxfd given by forktty()
		   we get EIO at the end of data.
		   So, simulate normal EOF by setting nread = 0. */
		if (errno == EIO)
			nread = 0;
		else
			return -1;
	}

	/* Duplicate input to file given with -o */
	if (lsp_ofile > 0)
		write(lsp_ofile, buffer_p, nread);

	if (nread < size_to_read)
		lsp_debug("%s, pos %ld: read %ld bytes instead of %ld.",
			  __func__, cf->seek, nread, size_to_read);

	cf->seek += nread;

	if (nread == 0) {
		lsp_debug("%s: EOF detected for %s", __func__, cf->name);

		 /* We notice if a file is of unknown size and we read
		    all data from it. */
		if (cf->size == LSP_FSIZE_UNKNOWN) {
			cf->size = cf->seek;
		}
		lsp_file_close();
		return 0;
	}

	size_t read_offset = buffer_p - cf->data->buffer;

	/* When this is a new buffer and if a previous buffer exists and ends
	   with newline this buffer is the start of a new line. */
	if (read_offset == 0 && cf->seek - nread != 0) {
		if (cf->data->prev->buffer[cf->blksize - 1] == '\n')
			lsp_lines_add(cf->data->seek);
	}

	if (read_offset > 0) {
		if (buffer_p[-1] == '\n')
			lsp_lines_add(cf->data->seek + read_offset);
	}

	/* Inspect all of the read data to keep record of lines */
	for (int i = 0; (i + 1) < nread; i++)
		if (buffer_p[i] == '\n')
			/* We record the beginning of a line,
			   not the end => +1 */
			lsp_lines_add(cf->data->seek + read_offset + i + 1);

	return nread;
}

/*
 * For the current file: add the offset of the beginning of a line to the
 * array of offsets of lines.
 * Offsets must come in in increasing order!
 *
 * fixme: this means, we don't support holes among the buffers...
 */
static void lsp_lines_add(off_t next_line)
{
	/* The first line is already known. */
	if (next_line == 0)
		return;

	/* Adjust size of lines array. */
	if (cf->lines_count + 1 == cf->lines_size) {
		cf->lines = lsp_realloc(cf->lines,
				    (cf->lines_size * 2) * sizeof(next_line));
		cf->lines_size *= 2;
	}

	if (next_line < cf->lines[cf->lines_count - 1])
		lsp_error("%s: line offsets not increasing: line %ld@%ld vs. line %ld@%ld.\n",
			  __func__, cf->lines_count, cf->lines[cf->lines_count - 1],
			  cf->lines_count + 1, next_line);

	cf->lines[cf->lines_count++] = next_line;
}

/*
 * Read all remaining data from current input fd.
 */
static void lsp_file_read_all()
{
	while (!LSP_EOF)
		lsp_file_add_block();

}

/*
 * Read the next block of data.
 *
 * Reading actually happens in lsp_file_read_block()
 * and we just calculate the maximum size of data that should be
 * read.
 */
static void lsp_file_add_block()
{
	if (LSP_EOF)
		return;

	size_t size_to_read;

	if (cf->size != LSP_FSIZE_UNKNOWN)
		size_to_read = cf->size - cf->seek;
	else
		/* We don't know about the file's size.
		   Try to read a block. */
		size_to_read = cf->blksize;

	/* Read at most blksize bytes. */
	if (size_to_read > cf->blksize)
		size_to_read = cf->blksize;

	lsp_file_read_block(size_to_read);

}

/*
 * Error out with a message.
 */
static int lsp_error(const char *format, ...)
{
	va_list ap;

	endwin();

	va_start(ap, format);
#if DEBUG
	if (lsp_logfp) {
		va_list apd;
		va_copy(apd, ap);
		vfprintf(lsp_logfp, format, apd);
		va_end(apd);
	}
#endif
	vfprintf(stderr, format, ap);
	va_end(ap);

	/* Check if curses has been initialized and do cleanup */
	if (isendwin() == FALSE)
		endwin();

	lsp_file_ring_dtor();

	exit(EXIT_FAILURE);
}

/*
 * Output a debug message.
 */
static int lsp_debug(const char *format, ...)
{
	int result = 0;
#if DEBUG
	FILE *fp = lsp_logfp;

	/* Enable early logging before lsp_logfp is setup according to
	   a -l option: as long as lsp_logfp is unset we write to stderr. */
	if (!fp)
		fp = stderr;

	va_list ap;
	va_start(ap, format);
	result = vfprintf(fp, format, ap);
	fprintf(fp, "\n");
	result += 1;
	va_end(ap);
#endif
	return result;
}

/*
 * Destroy input file structures for clean exit.
 */
static void lsp_file_ring_dtor()
{
	while (cf != NULL)
		lsp_file_kill();

}

/*
 * Remove current file_t from the ring.
 */
static void lsp_file_kill()
{
	struct file_t *tmp;

	lsp_debug("%s: killing file \"%s\".", __func__, cf->name);

	tmp = cf;
	tmp->prev->next = tmp->next;
	tmp->next->prev = tmp->prev;
	cf = tmp->next;

	lsp_file_dtor(tmp);
	/*
	 * If it pointed to itself we just destroyed the last member
	 * of the ring.
	 */
	if (cf == tmp)
		cf = NULL;
}

/*
 * Cleanly remove a file_t
 */
static void lsp_file_dtor(struct file_t *file)
{
	free(file->name);
	free(file->rep_name);
	free(file->lines);
	lsp_file_data_dtor(file->data);

	if (file->flags & LSP_FLAG_POPEN) {
		if (file->fp != NULL)
			pclose(file->fp);
	} else if (file->fd != -1)
		close(file->fd);

	lsp_toc_dtor(file);

	free(file);
}

/*
 * Remove all data buffers of a file_t
 */
static void lsp_file_data_dtor(struct data_t *data)
{
	struct data_t *tmp = data;

	lsp_debug("%s: destroying data buffers of file", __func__);

	if (data == NULL)
		return;

	/* Break up the ring so that it has ends. */
	data->prev->next = NULL;

	while (tmp) {
		data = tmp;
		tmp = data->next;
		free(data->buffer);
		free(data);
	}
}

/*
 * Find a tty device for user command input.
 * Usually this should be stdout but if it is no tty we test stderr, as
 * well.
 */
static void lsp_find_tty_input()
{
	char *tty_path;
	if (isatty(STDOUT_FILENO)) {
		tty_path = ttyname(STDOUT_FILENO);
		lsp_debug("%s: using stdout (%s) for command input",
			  __func__, tty_path);
	} else if (isatty(STDERR_FILENO)) {
		tty_path = ttyname(STDERR_FILENO);
		lsp_debug("%s: using stderr (%s) for command input",
			  __func__, tty_path);
	} else
		lsp_error("Cannot find tty input device...\n");

	lsp_debug("%s: trying to open %s...", __func__, tty_path);

	int in_fd = open(tty_path, 0, "r");

	if (in_fd == -1)
		lsp_error("%s: %s: %s\n", __func__, tty_path, strerror(errno));

	if (in_fd != STDIN_FILENO)
		lsp_error("TTY input fd (%d) != STDIN_FILENO.\n", in_fd);

}

/*
 * Initialize stdin as the only data input.
 *
 * We first move the file descriptor away from STDIN_FILENO and then open a tty
 * device (stdout or stderr) as the then free to use STDIN_FILENO for reading
 * user commands.
 */
static void lsp_file_init_stdin()
{
	struct stat statbuf;

	lsp_debug("No input files given -- checking stdin...");

	if (isatty(STDIN_FILENO))
		lsp_error("STDIN is a tty; we don't support that -- yet.\n");

	/* This should be a name an ordinary file couldn't conflict with.
	   Let's use the the empty string. */
	lsp_file_add("", 0);

	fstat(STDIN_FILENO, &statbuf);

	cf->size = LSP_FSIZE_UNKNOWN;
	cf->blksize = statbuf.st_blksize;

	/* Move stdin to something > 2 and see if we can use stdout or stderr as
	 * the one we use for the user to communicate with us. */
	cf->fd = dup(STDIN_FILENO);
	close(STDIN_FILENO);

	if (cf->fd <= 2)
		lsp_debug("%s: file descriptor did not become > 2.\n", __func__);

	lsp_find_tty_input();

	lsp_file_add_block();

	char *name = lsp_detect_manpage();
	if (name == NULL)
		return;

	free(cf->name);
	cf->name = name;
	cf->ftype = LSP_FTYPE_MANPAGE;
}

/*
 * Initialization of input stream for user commands.
 *
 * This is necessary, because pipelines like e.g.
 * `bzcat /usr/share/man/man1/mandoc.1.bz2 | mandoc -l` give us an empty FIFO
 * and not the user's terminal device as stdin.
 */
static void lsp_init_cmd_input()
{
	int		result;
	struct stat	statbuf;

	/* Test if STDIN_FILENO is open */
	result = fstat(STDIN_FILENO, &statbuf);

	if (result != 0) {
		if (result == EBADF)
			lsp_error("%s: STDIN_FILENO not open for reading.\n",
				  __func__);
		else
			lsp_error("%s: STDIN_FILENO: %s\n", __func__, strerror(errno));
	}

	if (!isatty(STDIN_FILENO)) {
		/* No tty: try to read from stdin. */

		int	c = 'x';
		int	result;

		result = read(STDIN_FILENO, &c, 1);

		if (result == -1)
			lsp_error("%s: stdin: read failed: %s\n", __func__, strerror(errno));

		if (result == 0) {
			/* EOF on stdin.
			   Close it and try to find a tty to use. */
			fclose(stdin);
			lsp_find_tty_input();
		}
	}
}

/*
 * Open input file.
 *
 * Use a preprocessor if the environment provides one.
 */
static int lsp_open_file(const char *name)
{
	char buffer[512];

	if (lsp_env_open == NULL) {
		cf->fd = open(cf->name, 0, "r");
		return 0;
	}

	size_t cmd_len = strlen(lsp_env_open) + strlen(name) + 1;
	char *cmd = lsp_calloc(cmd_len, 1);
	snprintf(cmd, cmd_len, lsp_env_open, name);

	if (cmd[0] == '|') {
		/*
		 * Create a pipe the preprocessor writes the content to.
		 */
		FILE *fp = popen(cmd + 1, "r");

		if (fp == NULL) {
			lsp_error("%s: could not popen(\"%s\").\n", __func__, lsp_env_open);
		}

		/*
		 * Try to read from pipe.
		 * Don't use fread(3) which causes problems when we later use read(2)!
		 *
		 * Use original file if the pipe has no data for us.
		 */
		size_t n = read(fileno(fp), buffer, 1);

		if (n == 0) {
			/*
			 * No data in pipe.  Use original file.
			 */
			pclose(fp);
			cf->fd = open(cf->name, 0, "r");
		} else {
			/*
			 * There is data in the pipe.
			 */
			/*
			 * We cannot unget the read byte; store it in cf for
			 * later consumption.
			 */
			cf->flags |= LSP_PRE_READ;
			cf->pre_read = buffer[0];
			/* Remember that we need to pclose(3) this pipe. */
			cf->flags |= LSP_FLAG_POPEN;
			cf->fp = fp;
			cf->fd = fileno(fp);
		}
	} else {
		/*
		 * Don't establish a pipe to the preprocessor but let it tell us
		 * which replacement file we should read.
		 */
		FILE *fp = popen(cmd, "r");

		if (fp == NULL) {
			lsp_error("%s: could not popen(\"%s\").\n", __func__, lsp_env_open);
		}

		size_t nread = 0;

		while (!feof(fp)) {
			nread += fread(buffer + nread, 1, sizeof(buffer) - nread, fp);

			if (nread == sizeof(buffer))
				lsp_error("%s: replacement file name too long.\n", __func__);
		}

		pclose(fp);

		if (nread == 0) {
			lsp_debug("%s: no replacement file for \"%s\"", __func__, cf->name);
			cf->fd = open(cf->name, 0, "r");
		} else {
			lsp_debug("%s: opening replacement file \"%s\"", __func__, buffer);
			cf->rep_name = strdup(buffer);
			cf->fd = open(cf->rep_name, 0, "r");
		}
	}

	free(cmd);

	return 0;
}

/*
 * Initialize ring of input files
 * Open the first given file (or stdin) for reading.
 */
static void lsp_file_init_ring()
{
	struct stat statbuf;
	struct file_t *ring_start = cf;

	if (cf == NULL) {
		/* No file name given, try stdin. */
		lsp_file_init_stdin();
		return;
	}

	lsp_init_cmd_input();

	/* Initialize all input files in the ring */
	do {
		lsp_open_file(cf->name);

		if (cf->fd == -1)
			lsp_error("%s: %s\n", cf->name, strerror(errno));

		if (fstat(cf->fd, &statbuf) == -1)
			lsp_error("%s: %s\n", cf->name, strerror(errno));

		if (!(S_ISREG(statbuf.st_mode) || S_ISFIFO(statbuf.st_mode)))
			lsp_error("%s: %s: unsupported file type.\n",
				  __func__, cf->name);

		/* We know sizes only of regular files. */
		if (S_ISREG(statbuf.st_mode))
			cf->size = statbuf.st_size;
		else
			cf->size = LSP_FSIZE_UNKNOWN;

		cf->blksize = statbuf.st_blksize;

		lsp_file_add_block();

		cf = cf->next;
	} while (cf != ring_start);
}

/*
 * ncurses initialization
 */
static int lsp_init_screen()
{
	mmask_t new_mask, old_mask;

	lsp_win = initscr();
	if (lsp_win == NULL)
		return -1;

	getmaxyx(lsp_win, lsp_maxy, lsp_maxx);

	if (!has_colors())
		lsp_color = false;

	if (lsp_color) {
		start_color();

		use_default_colors();
		pair_content(LSP_DEFAULT_PAIR, &lsp_fg_color_default, &lsp_bg_color_default);

		lsp_next_pair = LSP_FREE_PAIR;

		init_pair(LSP_BOLD_PAIR,    COLOR_BLUE,	   lsp_bg_color_default);
		init_pair(LSP_UL_PAIR,	    COLOR_CYAN,	   lsp_bg_color_default);
		init_pair(LSP_REVERSE_PAIR, COLOR_WHITE,   COLOR_MAGENTA);

		bkgd(COLOR_PAIR(LSP_DEFAULT_PAIR));
	}

	/* Make white a bit whiter.  1000 is max. */
	int ret = init_color(COLOR_WHITE, 909, 909, 909);
	if (ret == ERR)
		lsp_error("%s: Could not change color.\n", __func__);

	cbreak();
	noecho();
	keypad(stdscr, TRUE);

	new_mask = ALL_MOUSE_EVENTS;
	mousemask(new_mask, &old_mask);

	if (lsp_color)
		wbkgd(lsp_win, COLOR_PAIR(LSP_DEFAULT_PAIR));

	wattr_set(lsp_win, A_NORMAL, LSP_DEFAULT_PAIR, 0);

	keypad(lsp_win, TRUE);

	return 0;
}

#if DEBUG
/*
 * Output the ring of input file names.
 */
static void lsp_print_file_ring()
{
	size_t i = 1;
	struct file_t *ptr = cf;

	lsp_debug("Input files:");

	if (cf == NULL)
		return;

	do {
		lsp_debug("%ld: name=\"%s\", size=%ld", i++, ptr->name, ptr->size);
		ptr = ptr->next;
	} while (ptr != cf);
}
#endif

/*
 * Reset getch_pos and mark the file's buffers unaligned.
 */
static void lsp_file_set_pos(off_t pos)
{
	lsp_pos = pos;

	/* Only existing buffers can be unaligned. */
	if (cf->data != NULL)
		cf->unaligned = 1;
}

/*
 * Find previous match in file.
 *
 * Searching backwards means to find all matches in a line and return
 * the last one!
 *
 * lsp_line must already be prepared for the correct
 * start/continuation of the search, i.e. cut off tail of already
 * found matches.
 */
static regmatch_t lsp_search_find_prev_match(struct lsp_line_t **line)
{
	int ret;
	regmatch_t pmatch[1];
	 /* In LSP_REFS_MODE we need to validate references that we find.
	    So, match positions first go to the variable match and if validation
	    is needed they are copied to valid_match only after they are
	    validated. */
	regmatch_t match = lsp_no_match;
	regmatch_t valid_match = lsp_no_match;
	off_t offset;

	do {
		offset = 0;

		/* Find all matches in the line; we need the last one. */
		while (1) {
			int eflags = 0;

			if (offset > 0)
				eflags = REG_NOTBOL;

			ret = regexec(cf->regex_p, (*line)->normalized + offset,
				      1, pmatch, eflags);

			if (ret != 0)
				break;

			lsp_debug("%s: regexec match \"%s\"",
				  __func__, (*line)->normalized + offset);

			/* Store offsets relative to line start. */
			match.rm_so = offset + pmatch[0].rm_so;
			match.rm_eo = offset + pmatch[0].rm_eo;
			offset += pmatch[0].rm_eo;

			/* Now, calculate match offsets for raw string. */
			match.rm_so = (*line)->pos +
				lsp_normalize_count((*line)->raw, match.rm_so);
			match.rm_eo = (*line)->pos +
				lsp_normalize_count((*line)->raw, match.rm_eo);

			if (lsp_mode_is_search()) {
				valid_match = match;
			} else {
				/* LSP_REFS_MODE:
				   we need to validate the match. */
				bool valid = lsp_validate_ref_at_pos(match);

				if (valid)
					valid_match = match;
			}
		}

		if (lsp_is_a_match(valid_match)) {
			lsp_mode_set_highlight();
			return valid_match;
		}

		lsp_file_set_pos((*line)->pos);
		lsp_line_dtor(*line);
		*line = lsp_file_get_prev_line();

	} while (*line);

	return valid_match;
}

/*
 * Wrapper function for forward searches in the file or TOC.
 */
static regmatch_t lsp_search_next()
{
	if (lsp_mode_is_toc())
		return lsp_search_toc_next();
	else
		return lsp_search_file_next();
}

/*
 * From the current TOC position:
 *
 * find first match for search pattern
 */
static regmatch_t lsp_search_toc_next()
{
	int ret;
	regmatch_t pmatch[1];
	regmatch_t ret_val = lsp_no_match;
	struct lsp_line_t *line = NULL;

	if (!cf->toc)
		return ret_val;

	off_t start_pos = lsp_pos;
	struct toc_node_t *start_toc = cf->toc;

	while (1) {
		regmatch_t match;

		lsp_line_dtor(line);

		line = lsp_get_this_line();

		if (!line)
			break;

		/* Remove newline character to enable matches for characters at
		   end of line. */
		char *ptr = strchr(line->normalized, '\n');
		if (ptr)
			*ptr = '\0';

		size_t offset = 0;

		/* Adjust start position for search based on the
		   position we were called with.

		   Translate the position of the raw part to the one
		   in the normalized where we will do the search! */
		if (start_pos > line->pos) {
			/* For now, we go the ugly way:
			   Normalize the raw string up to the offset
			   position.  The length of the normalized
			   string should then be the correct offset for the
			   normalized string.  Puh... */
			char *tmp =
				lsp_normalize(line->raw, start_pos - line->pos);
			offset = strlen(tmp);
			lsp_debug("%s: normalized offset for raw %ld: %ld",
				  __func__, start_pos - line->pos, offset);
			free(tmp);
		}

		int eflags = 0;

		if (offset > 0)
			eflags = REG_NOTBOL;

		ret = regexec(cf->regex_p, line->normalized + offset,
			      1, pmatch, eflags);

		if (ret == 0) {
			lsp_debug("%s: regexec match \"%s\"",
				  __func__, line->normalized + offset);
			match.rm_so = line->pos +
				lsp_normalize_count(line->raw,
						    offset + pmatch[0].rm_so);
			match.rm_eo = line->pos +
				lsp_normalize_count(line->raw,
						    offset + pmatch[0].rm_eo);

			lsp_mode_set_highlight();
			ret_val = match;
			break;
		}
		if (lsp_toc_move_to_next())
			break;
		lsp_file_set_pos(cf->toc->pos);
	}

	lsp_file_set_pos(start_pos);
	cf->toc = start_toc;
	lsp_line_dtor(line);
	return ret_val;
}

/*
 * From current file position:
 *
 * find first match for search pattern
 */
static regmatch_t lsp_search_file_next()
{
	int ret;
	regmatch_t pmatch[1];
	regmatch_t ret_val = lsp_no_match;
	struct lsp_line_t *line = NULL;
	off_t start_pos = lsp_pos;

	while (1) {
		regmatch_t match;

		lsp_line_dtor(line);

		line = lsp_get_this_line();

		if (!line)
			break;

		/* Remove newline character to enable matches for characters at
		   end of line. */
		char *ptr = strchr(line->normalized, '\n');
		if (ptr)
			*ptr = '\0';

		size_t offset = 0;

		/* Adjust start position for search based on the
		   position we were called with.

		   Translate the position of the raw part to the one
		   in the normalized where we will do the search! */
		if (start_pos > line->pos) {
			/* For now, we go the ugly way:
			   Normalize the raw string up to the offset
			   position.  The length of the normalized
			   string should then be the correct offset for the
			   normalized string.  Puh... */
			char *tmp =
				lsp_normalize(line->raw, start_pos - line->pos);
			offset = strlen(tmp);
			lsp_debug("%s: normalized offset for raw %ld: %ld",
				  __func__, start_pos - line->pos, offset);
			free(tmp);
		}

		int eflags = 0;

		if (offset > 0)
			eflags = REG_NOTBOL;

		ret = regexec(cf->regex_p, line->normalized + offset,
			      1, pmatch, eflags);

		if (ret == 0) {
			lsp_debug("%s: regexec match \"%s\"",
				  __func__, line->normalized + offset);
			match.rm_so = line->pos +
				lsp_normalize_count(line->raw,
						    offset + pmatch[0].rm_so);
			match.rm_eo = line->pos +
				lsp_normalize_count(line->raw,
						    offset + pmatch[0].rm_eo);

			lsp_mode_set_highlight();
			ret_val = match;
			break;
		}
	}

	lsp_file_set_pos(start_pos);
	lsp_line_dtor(line);
	return ret_val;
}

/*
 * Insert a new gref into the grefs hash table.
 */
static int lsp_gref_henter(struct gref_t *gref_p)
{
	ENTRY e;
	ENTRY *ep;

	e.key = gref_p->name;
	e.data = gref_p;

	ep = hsearch(e, ENTER);

	if (ep == NULL)
		lsp_error("Enter \"%s\" into hash table: %s\n", gref_p->name, strerror(errno));

	return 0;
}

/*
 * Duplicate the given string to one with all lowercase chars.
 */
static char *lsp_to_lower(char *str)
{
	int i;
	char *new_str = lsp_malloc(strlen(str) + 1);

	for (i = 0; str[i]; i++)
		new_str[i] = tolower(str[i]);

	new_str[i] = '\0';

	return new_str;
}

/*
 * Search for a global reference.
 * Add it if it does not exist.
 */
static struct gref_t *lsp_gref_search(char *name)
{
	struct gref_t *ptr;
	char *tmp_name;

	if (!lsp_man_case_sensitivity) {
		tmp_name = lsp_to_lower(name);
	} else {
		tmp_name = strdup(name);
	}

	if (lsp_grefs) {
		ptr = lsp_gref_find(tmp_name);

		if (ptr != NULL) {
			free(tmp_name);
			return ptr; /* gref already exists */
		}
	} else {
		/*
		 * If we have no grefs the hash table is also missing.
		 */
		int ret = hcreate(lsp_htable_entries);

		if (ret == 0)
			lsp_error("hcreate(): %s\n", strerror(errno));
	}

	/* gref not found: add it. */
	ptr = lsp_malloc(sizeof(struct gref_t));
	ptr->name = tmp_name;   /* strdup() happend in lsp_to_lower()! */
	ptr->valid = -1;	/* not yet validated */

	if (lsp_grefs)
		ptr->next = lsp_grefs;
	else
		ptr->next = NULL;

	lsp_grefs = ptr;

	lsp_debug("%s: gref created: %s", __func__, ptr->name);

	/* Insert pointer to gref into hash table. */
	lsp_gref_henter(ptr);

	lsp_grefs_count++;

	return ptr;
}

/*
 * Find a global reference or retrun NULL.
 */
static struct gref_t *lsp_gref_find(char *name)
{
	ENTRY e;
	ENTRY *ep;

	e.key = name;

	ep = hsearch(e, FIND);

	if (ep == NULL)
		return (struct gref_t *)ep;

	return (struct gref_t *)(ep->data);
}

/*
 * Check if the given position is inside the given line.
 *
 * Return:   0 if it is
 *         < 0 if pos is in a previous line (left of the given)
 *         > 0 if pos is in a following line (right of the given)
 */
static int lsp_cmp_line_pos(size_t line_no, off_t pos)
{
	off_t next;

	if (cf->lines[line_no] > pos)
		return -1;	/* pos is to the left of the given line */

	/*
	 * Here, we know: this line is either left of the given position or it
	 * contains it.
	 */

	/*
	 * Calculate start pos of next line.
	 * Use current file size (seek) if we are looking at the last line.
	 */
	if (line_no + 1 == cf->lines_count)
		next = cf->seek;
	else
		next = cf->lines[line_no + 1];

	if (next > pos)
		return 0;	/* Next line starts right of pos, so this line
				 * must include it */

	return 1;		/* Pos is to the right of the given line */
}

/*
 * Find the line number for a given position inside the file.
 * That number is returned 1-based.
 *
 * Search for the line in cf->lines array that starts at <= pos with
 * it's following line starting at > pos.
 */
static size_t lsp_file_pos2line(off_t pos)
{
	size_t start;
	size_t mid;
	size_t end;

	if (pos > cf->seek) {
		lsp_file_read_all();

		if (cf->size == LSP_FSIZE_UNKNOWN)
			lsp_debug("%s: file %s: cf->size == 0 "
				  "after reading the whole file.",
				  __func__, cf->name);
	}

	if (cf->size != LSP_FSIZE_UNKNOWN && pos > cf->size)
		lsp_error("%s: cannot get a line number outside "
			  "the size of the file: %ld vs. %ld\n",
			  cf->name, cf->size, pos);

	if (pos == 0 || cf->lines_count == 1)
		return 1;

	if (pos == cf->size)
		return cf->lines_count;

	/*
	 * Do a binary search in lines for the one containing the given
	 * position.
	 */
	start = 0;
	end = cf->lines_count - 1;

	while (start <= end) {
		mid = (end + start) / 2;

		int cmp = lsp_cmp_line_pos(mid, pos);

		if (cmp == 0)
			break;

		if (cmp < 0)
			end = mid - 1;
		else
			start = mid + 1;
	}

	lsp_debug("%s (%s): pos %ld found in line #%ld",
		  __func__, cf->name, pos, start + 1);

	/* Return the line number 1-based. */
	return mid + 1;
}

/*
 * Verify if a reference is known to man(1).
 */
static bool lsp_ref_is_valid(struct gref_t *gref)
{
	int ret;
	size_t cmd_len;

	/*
	 * If apropos(1) is used for verification create the apropos buffer
	 * which will add grefs for all it's content.
	 *
	 * Save current file and mode, because they get reset by creating the
	 * apropos file and then switching back to the current file.
	 */
	if (lsp_verify_with_apropos) {
		char *current_cf = cf->name;
		int current_mode = cf->mode;

		lsp_cmd_create_apropos();

		cf = lsp_file_find(current_cf);
		lsp_mode_set(current_mode);

		return gref->valid == 1;
	}

	/* Calculate length of command string.
	   First part is the length of the format string minus 2 bytes
	   conversion specifier. */
	cmd_len = strlen(lsp_verify_command) - 2;

	/* Next part is the length of gref name plus terminator. */
	cmd_len += strlen(gref->name) + 1;

	char *command = lsp_malloc(cmd_len);

	sprintf(command, lsp_verify_command, gref->name);

	ret = system(command);

	lsp_debug("%s: reference %s is %s",
		  __func__, command, ret == 0 ? "valid" : "invalid");

	free(command);

	return ret == 0;
}

/*
 * Compile regular expression for search.
 *
 * Return NULL if successful, otherwise an error message.
 */
static char *lsp_search_compile_regex(lsp_mode_t search_mode)
{
	int ret;

	if (search_mode == LSP_REFS_MODE) {
		/* For references, we re-use the same regular expression.
		   No need to compile it more than once. */
		if (lsp_refs_regex == NULL) {
			lsp_refs_regex = lsp_calloc(1, sizeof(regex_t));

			ret = regcomp(lsp_refs_regex, lsp_search_ref_string, REG_EXTENDED);

			/* No error handling here; we really should have a valid
			   expression for refs in this program. */
		}

		/* In any case: return success. */
		return NULL;
	} else {
		if (lsp_search_regex) {
			regfree(lsp_search_regex);
		} else {
			lsp_search_regex = lsp_calloc(1, sizeof(regex_t));
		}
		int cflags = REG_EXTENDED;
		if (!lsp_case_sensitivity)
			cflags |= REG_ICASE;

		ret = regcomp(lsp_search_regex, lsp_search_string, cflags);
	}

	if (ret != 0) {
		char *err_text;
		size_t err_len = regerror(ret, lsp_search_regex, NULL, 0);
		err_text = lsp_malloc(err_len);
		regerror(ret, lsp_search_regex, err_text, err_len);
		lsp_file_set_pos(cf->page_first);
		return err_text;
	}

	return NULL;
}

static void lsp_set_no_current_match()
{
	cf->current_match = lsp_no_match;
}

/*
 * Search command.
 * get_string specifies if we need to read a search string.
 * If false lsp_search_string is already prepared.
 */
static void lsp_cmd_search(bool get_string)
{
	if (get_string) {
		/* Read search string */
		if (wmove(lsp_win, lsp_maxy - 1, 0) == ERR)
			lsp_error("wmove failed.\n");
		wattr_set(lsp_win, A_NORMAL, LSP_DEFAULT_PAIR, NULL);

		if (lsp_search_direction == LSP_FW)
			mvwaddch(lsp_win, lsp_maxy - 1, 0, '/');
		else
			mvwaddch(lsp_win, lsp_maxy - 1, 0, '?');

		wclrtoeol(lsp_win);

		wrefresh(lsp_win);

		/* Save old search string if any */
		if (*lsp_search_string != '\0')
			strcpy(lsp_search_string_old, lsp_search_string);

		curs_set(1);
		echo();
		mvwgetnstr(lsp_win, lsp_maxy - 1, 1, lsp_search_string,
			   sizeof(lsp_search_string));
		noecho();
		curs_set(0);
	}

	if (*lsp_search_string == '\0')
		if (*lsp_search_string_old != '\0')
			strcpy(lsp_search_string, lsp_search_string_old);
		else {
			/* New and old search string are empty.
			   Do nothing but turn off highlighting. */
			lsp_mode_unset_highlight();
			lsp_file_set_pos(cf->page_first);
			return;
		}
	else if (lsp_search_regex) {
		regfree(lsp_search_regex);
		free(lsp_search_regex);
		lsp_search_regex = NULL;
	}

	if (lsp_search_regex == NULL ) {
		/* Compile regex if we got a new one. */
		char *reg_err_text;

		reg_err_text = lsp_search_compile_regex(LSP_SEARCH_MODE);

		if (reg_err_text) {
			regfree(lsp_search_regex);
			free(lsp_search_regex);
			lsp_search_regex = NULL;
			lsp_mode_unset_highlight();
			waddstr(lsp_win, reg_err_text);
			free(reg_err_text);
			wgetch(lsp_win);
			return;
		}
	}

	cf->regex_p = lsp_search_regex;

	if (lsp_search_direction == LSP_FW)
		lsp_file_search_next(LSP_SEARCH_MODE);
	else
		lsp_file_search_prev(LSP_SEARCH_MODE);
}

static void lsp_cmd_search_refs()
{
	lsp_search_compile_regex(LSP_REFS_MODE);

	cf->regex_p = lsp_refs_regex;

	if (lsp_search_direction == LSP_FW)
		lsp_file_search_next(LSP_REFS_MODE);
	else
		lsp_file_search_prev(LSP_REFS_MODE);
}

static bool lsp_is_no_match(regmatch_t match)
{
	return match.rm_so == (off_t)-1;
}

static bool lsp_is_a_match(regmatch_t match)
{
	return match.rm_so != (off_t)-1;
}

/*
 * Search backwards for pattern.
 */
static void lsp_file_search_prev(lsp_mode_t search_mode)
{
	struct lsp_line_t *line = NULL;

	/* Set start position for search:
	   If we are in highlight mode and the current match is on
	   the current page, we use it's position to start this search.
	   Otherwise we search from the top of the page backwards.*/
	if (lsp_mode_is_highlight() &&
	    lsp_pos_is_current_page(cf->current_match.rm_so))
		lsp_file_set_pos(cf->current_match.rm_so);
	else
		if (lsp_mode_is_toc()) {
			cf->toc = cf->toc_first;
			lsp_file_set_pos(cf->toc->pos);
		} else
			lsp_file_set_pos(cf->page_first);

	/* No backward searching from beginning of file */
	if (lsp_pos == 0) {
		lsp_prompt = lsp_not_found;
		return;
	}

	lsp_prompt = "Searching...";
	lsp_create_status_line();

	/* Find match backwards.
	   If we are in the middle of a line we cut the tail
	   starting from the previous match and start with inspecting the
	   remaining part of the line.
	   Otherwise we inspect the previous line. */
	if (lsp_is_at_bol())
		line = lsp_file_get_prev_line();
	else {
		line = lsp_get_this_line();
		lsp_line_cut_tail(line, cf->current_match.rm_so);
	}

	lsp_mode_set(search_mode);

	regmatch_t pos = lsp_search_find_prev_match(&line);
	lsp_line_dtor(line);

	if (lsp_is_no_match(pos)) {
		lsp_prompt = lsp_not_found;
		if (lsp_mode_is_toc())
			cf->toc = cf->toc_first;
		else
			lsp_file_set_pos(cf->page_first);
		return;
	}

	cf->current_match = pos;

	lsp_search_align_to_match();

}

/*
 * Cut tail starting at t_pos off the given line
 */
static void lsp_line_cut_tail(struct lsp_line_t *line, off_t t_pos)
{
	if (t_pos < line->pos ||
	    t_pos > line->pos + line->len)
		lsp_error("%s: dangerous position %ld to "
			  "cut the current line [%ld..%ld].\n",
			  __func__, t_pos, line->pos, line->pos + line->len);

	line->raw[t_pos - line->pos] = '\0';
	line->len = strlen(line->raw);
	free(line->normalized);
	line->normalized = lsp_normalize(line->raw, line->len);
	line->nlen = strlen(line->normalized);
}

/*
 * Find next match for pattern.
 */
static void lsp_file_search_next(lsp_mode_t search_mode)
{
	lsp_prompt = "Searching...";
	lsp_create_status_line();
	/*
	 * If we are in highlight mode and the current match is on
	   the current page, we use it's position for this search.
	   Otherwise we start at the top of the page.*/
	if (lsp_mode_is_highlight() &&
	    lsp_pos_is_current_page(cf->current_match.rm_so)) {
		/* We are in a search.
		 * Search continues after last match if it was on the current
		 * page.
		 *
		 * But if search_mode changed (e.g. from normal search to refs)
		 * we use the start of the last match.  Users could search for
		 * some pattern and at a match realize that they actually want
		 * to visit the reference this match forms.
		 */
		if (cf->mode & search_mode)
			lsp_file_set_pos(cf->current_match.rm_eo);
		else
			lsp_file_set_pos(cf->current_match.rm_so);
	} else
		/* Search starts at top of page */
		if (lsp_mode_is_toc()) {
			cf->toc = cf->toc_first;
			lsp_file_set_pos(cf->toc->pos);
		} else {
			lsp_file_set_pos(cf->page_first);
		}

	regmatch_t pos;

	/* If we are searching references we only want to match valid
	   ones.  Hence the loop. */
	while (1) {
		/* Find next match */
		pos = lsp_search_next();

		if (lsp_is_no_match(pos)) {
			lsp_prompt = lsp_not_found;
			if (lsp_mode_is_toc()) {
				cf->toc = cf->toc_first;
			} else {
				lsp_file_set_pos(cf->page_first);
			}
			return;
		}

		/* In search mode just a match is required. */
		if (search_mode == LSP_SEARCH_MODE)
			break;

		bool valid = lsp_validate_ref_at_pos(pos);

		if (valid)
			break;

		/* Reference is invalid.
		   Try to find one after the current match. */
		lsp_file_set_pos(pos.rm_eo);
	}

	lsp_mode_set(search_mode);
	cf->current_match = pos;

	lsp_search_align_to_match();

}

/*
 * Get full line that contains the given offset.
 */
struct lsp_line_t *lsp_get_line_at_pos(off_t pos)
{
	struct lsp_line_t *line;
	off_t old_pos = lsp_pos;

	lsp_file_set_pos(pos);

	line = lsp_get_this_line();

	lsp_file_set_pos(old_pos);

	return line;
}

struct gref_t *lsp_get_gref_at_pos(regmatch_t pos)
{
	struct lsp_line_t *line = lsp_get_line_at_pos(pos.rm_so);

	if (!line) {
		lsp_error("%s: could not get a line at pos %ld\n",
			  __func__, pos);
	}

	char *ref_start = line->raw + (pos.rm_so - line->pos);

	char *ref_name =
		lsp_normalize(ref_start, pos.rm_eo - pos.rm_so);

	/* Create gref or get existing one. */
	struct gref_t *gref = lsp_gref_search(ref_name);

	free(ref_name);
	lsp_line_dtor(line);

	return gref;
}

static bool lsp_validate_ref_at_pos(regmatch_t pos)
{
	struct gref_t *gref = lsp_get_gref_at_pos(pos);

	/* Return true if verification is turned off. */
	if (!lsp_verify)
		return true;

	if (gref->valid == -1)
		gref->valid = lsp_ref_is_valid(gref);

	return gref->valid;
}

static void lsp_search_align_to_match()
{
	if (lsp_mode_is_toc())
		lsp_search_align_toc_to_match();
	else
		lsp_search_align_page_to_match();
}

/* Position TOC according to the search match.
 *
 * We do that in an emacs-like fashion:
 * 1) position 1/2 TOC page fw if match is on last line
 *    else
 * 2) stand still if match is on current page
 *    else
 * 3) position 1/2 TOC page above the match
 */
static void lsp_search_align_toc_to_match()
{
	if (lsp_is_no_match(cf->current_match))
		lsp_error("%s: function called with no active match\n",
			  __func__);

	size_t match_line = lsp_file_pos2line(cf->current_match.rm_so);

	size_t bottom_line = lsp_file_pos2line(cf->toc_last ? cf->toc_last->pos : cf->size - 1);

	if (match_line == bottom_line && cf->toc_last) {
		cf->toc = cf->toc_first;
		lsp_toc_fw(lsp_maxy / 2);
	} else if (lsp_pos_is_current_page(cf->current_match.rm_so) == TRUE) {
		cf->toc = cf->toc_first;
	} else {
		cf->toc = lsp_pos_to_toc(cf->current_match.rm_so);
		lsp_toc_bw(lsp_maxy / 2);
	}
}

/* Position page according to the search match.
 *
 * We do that in an emacs-like fashion:
 * 1) position 1/2 page fw if match is on last line
 *    else
 * 2) stand still if match is on current page
 *    else
 * 3) position 1/2 page above the match
 */
static void lsp_search_align_page_to_match()
{
	if (lsp_is_no_match(cf->current_match))
		lsp_error("%s: function called with no active match\n",
			  __func__);

	size_t match_line = lsp_file_pos2line(cf->current_match.rm_so);

	size_t bottom_line = lsp_file_pos2line(cf->page_last - 1);

	if (match_line == bottom_line)
		lsp_cmd_forward(lsp_maxy / 2);
	else if (lsp_pos_is_current_page(cf->current_match.rm_so) == TRUE)
		lsp_file_set_pos(cf->page_first);
	else {
		/* pos2line gives us 1-based line numbers! */
		lsp_file_set_pos(cf->lines[match_line - 1]);
		lsp_file_backward(lsp_maxy / 2);
	}
}

/*
 * Check if pos is on current page.
 *
 * We do not only need to check if the given position lies between the positions
 * of the first and last character of the current page, but also if it is
 * visible (TOC mode hides stuff).
 *
 * This is necessary to get smooth operation when switching from normal view to
 * TOC in searches.
 */
static bool lsp_pos_is_current_page(off_t pos)
{
	if (!lsp_mode_is_toc())
		return (cf->page_first <= pos && cf->page_last > pos);

	/* TOC mode. */
	if (cf->toc_first->pos <= pos &&
	    /* Last TOC entry is on this page if cf->toc_last == NULL */
	    (!cf->toc_last || cf->toc_last->pos > pos)) {
		/*
		 * Our answer could be "true".
		 * Test for visibility, i.e. pos must be part of a currently
		 * visible TOC line (according to active level).
		 */
		return lsp_pos_is_toc(pos);
	}

	return false;
}

/*
 * As the function name says.
 */
static void lsp_cmd_goto_start()
{
	lsp_file_set_pos(0);
}

/*
 * Go to end of file and then back one page.
 */
static void lsp_cmd_goto_end()
{
	lsp_file_read_all();

	lsp_file_set_pos(cf->size);

	lsp_file_backward(0);
}

/*
 * Find all matches in a line and store them in an array of type
 * regmatch_t.  We allocate the needed memory, the caller must free()
 * it.
 *
 * Return the number of found matches.
 */
static size_t lsp_line_get_matches(const struct lsp_line_t *line, regmatch_t **pmatch)
{
	size_t i = 0;
	size_t pmatch_len = 0;

	/* Allocate memory for max possible number of matches and that
	   should be the number of bytes in the line. */
	pmatch_len = line->nlen + 1;
	*pmatch = lsp_realloc(*pmatch, pmatch_len * sizeof(regmatch_t));

	char *ptr = line->normalized;

	/* Collect all pattern matches in this line */
	for (i = 0; i < pmatch_len; i++) {
		size_t offset;
		(*pmatch)[i].rm_so = -1;
		(*pmatch)[i].rm_eo = -1;

		/* Stop if the end of previous match is at the end of the line.
		   Needed for searches for e.g. '$'. */
		if (i > 0 && (*pmatch)[i - 1].rm_eo == line->len)
			break;

		int eflags = 0;

		if (i > 0) {
			eflags = REG_NOTBOL;
		}

		if (regexec(cf->regex_p, ptr, 1, *pmatch + i, eflags))
			break;

		offset = ptr - line->normalized;
		ptr += (*pmatch)[i].rm_eo;

		/* Rebase match offsets to beginning of line */
		(*pmatch)[i].rm_so += offset;
		(*pmatch)[i].rm_eo += offset;

		/* Compute offsets for raw string, because it is the
		   one that we need to do highlighting for. */
		(*pmatch)[i].rm_so = lsp_normalize_count(line->raw,
						       (*pmatch)[i].rm_so);
		(*pmatch)[i].rm_eo = lsp_normalize_count(line->raw,
						       (*pmatch)[i].rm_eo);

		/* For references: only mark valid ones. */
		if (cf->regex_p == lsp_refs_regex) {
			/* For validation, we need absolute offsets. */
			regmatch_t tmp = (*pmatch)[i];
			tmp.rm_so += line->pos;
			tmp.rm_eo += line->pos;

			if (!lsp_validate_ref_at_pos((tmp)))
				i--;
		}
	}
	return i;
}

/*
 * Convert multibyte sequence to wide character and handle errors.
 *
 * Currently, we distinguish three special conditions but do the same for all of
 * them.  Could be cleaned up if it turns out to be the right thing.
 */
static size_t lsp_mbtowc(wchar_t *wc_p, const char *mb_p, size_t n)
{
	size_t ch_len;

	ch_len = mbrtowc(wc_p, mb_p, n, NULL);

	if (ch_len == 0) {
		/* L'\0' found.
		   We take it as is but return 1 for it's length. */
		wc_p[0] = mb_p[0];
		ch_len = 1;
	} else if (ch_len == (size_t)-1) {
		/* Invalid multibyte sequnce.
		   Take char as is. */
		wc_p[0] = mb_p[0];
		ch_len = 1;
	} else if (ch_len == (size_t)-2) {
		/* Not sure how to react here.
		   The line ends with '\n' which means
		   that it sits in the middle of an mb
		   seqence...

		   So, for now: take char as is. */
		wc_p[0] = mb_p[0];
		ch_len = 1;
	}

	return ch_len;
}

static void lsp_invalidate_cm_cursor()
{
	cf->cmatch_x = -1;
}

/*
 * Get next line for display depending on the current mode (TOC mode or not).
 */
static struct lsp_line_t *lsp_get_next_display_line()
{
	struct lsp_line_t *line;

	if (lsp_mode_is_toc()) {
		/* Search next TOC line with level <= active level. */
		while (cf->toc->level > cf->current_toc_level) {
			if (cf->toc->next)
				cf->toc = cf->toc->next;
		}

		if (cf->toc->level > cf->current_toc_level) {
			/* No more TOC entries in this level. */
			line = NULL;
		} else {
			line = lsp_get_line_at_pos(cf->toc->pos);

			if (!line)
				lsp_error("%s: could not get line at pos %ld\n",
					  __func__, cf->toc->pos);
		}
	} else {
		/* No TOC mode; just return next line in file. */
		line = lsp_get_line_from_here();
	}

	return line;
}

/*
 * For a tail of a line: handle SGR sequences in the heading part of that line.
 *
 * Return 0 if we didn't find an SGR sequence, != 0 otherwise.
 */
static int lsp_line_handle_leading_sgr(attr_t *attr, short *pair)
{
	int ret_val = 0;

	off_t old_pos = lsp_pos;

	struct lsp_line_t *line = lsp_get_this_line();

	assert(line->len > (old_pos - line->pos));

	/* Terminate line where the tail starts. */
	line->raw[old_pos - line->pos] = '\0';

	size_t lindex = 0;

	while (line->raw[lindex]) {
		while (lsp_is_sgr_sequence(line->raw + lindex)) {
			size_t l;
			/* Get attributes according to SGR sequence. */
			l = lsp_decode_sgr(line->raw + lindex, attr, pair);

			/* Only use correct SGR sequences. */
			if (l != (size_t)-1) {
				if (l > 1)
					ret_val = 1;
				lindex += l;
			} else
				break;
		}
		/* Skip next possible multibyte sequence in the line. */
		lindex += lsp_mbtowc(NULL, line->raw + lindex, strlen(line->raw) - lindex);
	}

	lsp_line_dtor(line);
	lsp_file_set_pos(old_pos);

	return ret_val;
}

/*
 * Display page of data at current file position.
 */
static void lsp_display_page()
{
	off_t top_line = (off_t)-1;
	/* For conversion to cchar_t we need strings of wchar_t
	   terminatet by L'\0'. */
	wchar_t ch[2] = { L'\0', L'\0' };
	size_t ch_len;
	wchar_t next_ch = L'\0';
	/* Needed to distinguish a bold '_' and an underline. */
	wchar_t next_ch2;
	/* Complex char for cursesw routines. */
	cchar_t cchar_ch[2];

	int y, x;

	/* Remember if the text has SGR sequences in it. */
	char sgr_active = 0;

	regmatch_t *pmatch = NULL;
	struct lsp_line_t *line = NULL;
	size_t match_index = 0;

	/* Index of pmatch array that is the current match */
	ssize_t lsp_cm_index = -1;

	attr_t attr;
	short pair;
	/* attr and pair saved when highlighting matches. */
	attr_t attr_old;
	short pair_old;

	y = x = 0;		/* Start in upper left corner */

	/* Reload file if necessary, e.g. after a resize. */
	if (cf->do_reload)
		lsp_file_reload();

	if (!lsp_mode_is_toc()) {
		/* Nothing to display at EOF. */
		if (cf->size != LSP_FSIZE_UNKNOWN && (lsp_pos == cf->size))
			return;

		/* Save offset from where we build this page. */
		cf->page_first = lsp_pos;
	}

	lsp_invalidate_cm_cursor();

	/*
	 * Process lines until EOF or the screen is filled.
	 */
	while ((y < (lsp_maxy - 1))) {
		size_t lindex = 0;

		attr = A_NORMAL;
		pair = LSP_DEFAULT_PAIR;

		/* If we have long lines that consume multiple lines on the page
		   we need to process SGR sequences that might be in the
		   previous part of the line. */
		if (!lsp_is_at_bol())
			if (lsp_line_handle_leading_sgr(&attr, &pair))
				sgr_active = 1;

		lsp_line_dtor(line);

		line = lsp_get_next_display_line();

		if (!line)
			break;	/* EOF */

		/* Display line numbers. */
		if (lsp_do_line_numbers) {
			mvwprintw(lsp_win, y, x, "%7ld|",
				  lsp_file_pos2line(line->pos));
			getyx(lsp_win, y, x);
		}

		/* If we are searching: find matches in current line */
		if (lsp_mode_is_highlight())
			match_index = lsp_line_get_matches(line, &pmatch);

		/*
		 * We record a separate x position inside the current line.
		 * This information is used for chopping and horizontal shifting.
		 */
		int line_x = 0;
		/* Amount of spaces we still need to insert to expand a current
		   TAB in the line. */
		int tab_spaces = 0;

		/* Remember if we are currently inside a search match. */
		int match_active = 0;

		/* Output our interpretation of the line to ncurses window. */
		while (lindex < line->len) {
			if (lsp_mode_is_toc() && top_line == (off_t)-1)
				top_line = line->pos;

			/* Convert tabs to spaces. */
			if (line->raw[lindex] == '\t')
				tab_spaces = lsp_expand_tab(line_x);

			/* Convert next wide character */
			ch_len = lsp_mbtowc(ch, line->raw + lindex, line->len - lindex);

			/* Also get it's following two characters */
			if (lindex + ch_len == line->len) {
				//assert(ch[0] == '\n');
				/* Force newline at the end of the line. */
				next_ch = L'\n';
			} else {
				size_t l;
				l = lsp_mbtowc(&next_ch, line->raw + lindex + ch_len,
					       line->len - (lindex + ch_len));
				lsp_mbtowc(&next_ch2, line->raw + lindex + ch_len + l,
						line->len - (lindex + ch_len));
			}

			/* Highlight matches */
			if (match_index && lsp_mode_is_highlight()) {
				/* Emphasize found search matches. */
				size_t i;

				for (i = 0; pmatch[i].rm_so != -1; i++) {
					/* Highlight the matches. */
					if (pmatch[i].rm_so <= lindex &&
						pmatch[i].rm_eo > lindex) {

						if (pmatch[i].rm_so == lindex) {
							attr_old = attr;
							pair_old = pair;
							match_active = 1;
						}

						if (lsp_mode_is_refs()) {
							attr = A_UNDERLINE;
							pair = LSP_UL_PAIR;
						} else {
							attr = A_STANDOUT;
							pair = LSP_REVERSE_PAIR;
						}

						/* Notice if we are working on
						   the current match. */
						if (line->pos + lindex == cf->current_match.rm_so)
							lsp_cm_index = i;
					}

					/* Notice the end of the match */
					if (pmatch[i].rm_eo + 1 == lindex) {
						attr = attr_old;
						pair = pair_old;
						match_active = 0;

					}

					/* Notice if it was the current match, remember its
					   coords for positioning the cursor, later. */
					if (lsp_cm_index == i && pmatch[i].rm_eo <= lindex) {
							getyx(lsp_win, cf->cmatch_y, cf->cmatch_x);
							lsp_cm_index = -1;
					}
				}
			}

			/* Handle control characters to emphasize parts of the
			   text. */
			if (next_ch == L'\b') {
				if (attr == A_NORMAL) {
					if (ch[0] == L'_' && next_ch2 != L'_') {
						attr = A_UNDERLINE;
						pair = LSP_UL_PAIR;
					} else {
						attr = A_BOLD;
						pair = LSP_BOLD_PAIR;
					}
				}
				lindex += ch_len + 1;

				/* Convert tabs to spaces. */
				if (line->raw[lindex] == '\t')
					tab_spaces = lsp_expand_tab(line_x);

				ch_len = lsp_mbtowc(ch, line->raw + lindex, line->len - lindex);
				lsp_mbtowc(&next_ch, line->raw + lindex + ch_len, line->len - (lindex + ch_len));
			} else {
				while (lsp_is_sgr_sequence(line->raw + lindex)) {
					size_t l;
					/* Get attributes according to SGR
					 * sequence.  We could be inside a
					 * search match and in this case we
					 * need to set attribute/color for the
					 * part after the match. */
					if (match_active)
						l = lsp_decode_sgr(line->raw + lindex, &attr_old, &pair_old);
					else
						l = lsp_decode_sgr(line->raw + lindex, &attr, &pair);

					/* Only use correct SGR sequences. */
					if (l != (size_t)-1) {
						if (l > 1)
							sgr_active = 1;
						lindex += l;
						if (lindex >= line->len)
							goto line_done;

						/* Convert tabs to spaces. */
						if (line->raw[lindex] == '\t')
							tab_spaces = lsp_expand_tab(line_x);

						/* Convert next wide character */
						ch_len = lsp_mbtowc(ch, line->raw + lindex, line->len - lindex);
						/* No fetch of next_ch, because that would only be meaningful,
						   if we supported a mixture of backspace and SGR sequences.
						   We don't. */
					} else
						break;
				}
			}

			/* Record position of last TOC entry or first byte not part of this page. */
			if (lsp_mode_is_toc()) {
				cf->toc_last = cf->toc->next;
			} else {
				cf->page_last = line->pos + lindex + 1;
			}

			/* Expand TAB with spaces */
			if (ch[0] == '\t' && tab_spaces)
				ch[0] = ' ';

			/*
			 * line_x: we maintain an artificial x position inside
			 * the current line.
			 * This information is used for horizontal shifting.
			 */
			if (line_x >= lsp_shift || ch[0] == '\n') {
				/* Chop the line if we reach the width of the
				   screen. */
				if (lsp_chop_lines && x == lsp_maxx - 1) {
					if (next_ch != '\n')
						ch[0] = '>';

					setcchar(cchar_ch, ch, attr, pair, NULL);

					mvwadd_wch(lsp_win, y, x, cchar_ch);

					getyx(lsp_win, y, x);
					wmove(lsp_win, y, x);
					break;
				}

				if (lsp_mode_is_toc()) {
					if (lindex == 0) {
						/* Avoid nirvana cursor on last page. */
						if (!cf->toc->next)
							if (cf->toc_cursor > y)
								cf->toc_cursor = y;
					}

					/* Highlight cursor line if we are not searching. */
					if (!lsp_mode_is_highlight() && y == cf->toc_cursor) {
						attr = A_REVERSE;
						pair = LSP_REVERSE_PAIR;
					}
				}

				setcchar(cchar_ch, ch, attr, pair, NULL);

				mvwadd_wch(lsp_win, y, x, cchar_ch);

				getyx(lsp_win, y, x);
				wmove(lsp_win, y, x);

				/* Line is done if ncurses already skipped to the next
				   line and we only have a linefeed left in this
				   line. */
				if (x == 0) {
					/* The line could end with an SGR
					   sequence before the linefeed */
					size_t l_offset = lindex + ch_len;

					/* Consume SGR sequences if any */
					while (lsp_is_sgr_sequence(line->raw + l_offset))
						l_offset += lsp_get_sgr_len(line->raw + l_offset);

					/* Read correct next_ch if there was an SGR sequence */
					if (l_offset != lindex + ch_len)
						lsp_mbtowc(&next_ch, line->raw + l_offset, line->len - l_offset);

					if (next_ch == L'\n')
						break;
				}
			}

			line_x++;

			/* Reset attributes if we are not in a search match or
			   an SGR sequence. */
			if (attr != A_NORMAL && lsp_cm_index == -1)
				if (!sgr_active) {
					attr = A_NORMAL;
					pair = LSP_DEFAULT_PAIR;
				}

			/* Stay at position in line, if we are currently
			   processing the expansion of a TAB character. */
			if (tab_spaces) {
				assert(line->raw[lindex] == '\t');

				/* Advance in line if we are done with
				   expansion. */
				if (--tab_spaces == 0)
					lindex++;
			} else
				lindex += ch_len;
		}
line_done:
		free(pmatch);
		pmatch = NULL;

		if (lsp_mode_is_toc()) {
			if (!cf->toc->next)
				break;

			cf->toc = cf->toc->next;
		}
	}

	/* Fill the remainder of the screen with empty lines */
	ch[0] = L'\n';
	setcchar(cchar_ch, ch, attr, pair, NULL);

	while ((y < (lsp_maxy - 1))) {
		mvwadd_wch(lsp_win, y, x, cchar_ch);
		getyx(lsp_win, y, x);
		wmove(lsp_win, y, x);
	}

	if (!lsp_mode_is_toc())
		lsp_file_set_pos(cf->page_last);

	if (lsp_mode_is_toc() && top_line != (off_t)-1)
		cf->toc_first = lsp_pos_to_toc(top_line);

	wrefresh(lsp_win);

	lsp_line_dtor(line);
}

/*
 * In the given "physical" line, forward one screen line.
 *
 * Finally, adjust offset of the remaining line in the input file.
 */
static void lsp_line_fw_screen_line(struct lsp_line_t *line)
{
	int i = 0;
	int n = 0;
	wchar_t ch;

	while (n < lsp_maxx && i < line->nlen) {
		if (line->normalized[i] == '\n') {
			i++;
			break;
		}
		if (line->normalized[i] == '\t') {
			n += lsp_expand_tab(n);
			i++;
		} else {
			i += lsp_mbtowc(&ch, line->normalized + i, line->nlen - i);
			n++;
		}
	}

	/* We consumed i bytes from line for one screen line.  Remove them.
	   We use memmove(), because the whole allocated memory gets free()'d, later. */
	int raw_len = lsp_normalize_count(line->raw, i);
	line->pos += raw_len;
	line->len -= raw_len;
	memmove(line->raw, line->raw + raw_len, line->len);
	line->nlen -= i;
	memmove(line->normalized, line->normalized + i, line->nlen);

}

/*
 * Move forward n lines on the screen.
 * Screen lines are at minimum the length of a "physical" line (the bytes
 * between thwo '\n' characters), if the latter is shorter than the width of the
 * screen.  In cases where "physical" lines are longer than the screen width,
 * they are divided into parts that fit the screen width.
 *
 * So: from the current position, get consecutive "physical" lines, divide them if
 *     necessary and count until n is reached.  We need to ignore control
 *     characters to get net lengths.  Sounds like an understandable task...
 *
 * Not so obvious: the important part this function does is positioning the
 * current file to the target screen line by calling
 *
 *			lsp_file_set_pos(line->pos);
 */
static void lsp_screen_line_fw(int n)
{
	struct lsp_line_t *line = NULL;

	while (n) {
		line = lsp_get_line_from_here();

		if (!line)
			return;

		if (line->len == 1) {
			n--;
			lsp_line_dtor(line);
			continue;
		}

		while (n && line->len > 1) {
			/* Forward one screen width in the line */
			lsp_line_fw_screen_line(line);
			n--;

			/* We're done with the line if only a linefeed is left. */
			if (*(line->normalized) == '\n') {
				line->pos += line->len;
				break;
			}
		}

		lsp_file_set_pos(line->pos);

		lsp_line_dtor(line);
	}
}

/*
 * Move forward n lines.
 */
static void lsp_cmd_forward(int n)
{
	/* Rewind to first byte in the page */
	lsp_file_set_pos(cf->page_first);

	if (cf->page_last == cf->size)
		return;

	if (lsp_chop_lines != 0) {
		/* read forward n lines */
		int i = 0;
		while (i < n) {
			int ch = lsp_file_getch();
			if (ch == '\n')
				i++;
		}
	} else
		/* read forward n screen lines */
		lsp_screen_line_fw(n);

}

/*
 * Scroll backward n lines or one page if n == 0.
 */
static void lsp_cmd_backward(int n)
{
	/* Rewind to first byte in the page */
	lsp_file_set_pos(cf->page_first);

	if (lsp_chop_lines != 0)
		lsp_file_backward(n);
	else
		lsp_screen_line_bw(n);
}

/*
 * Move backward in file using screen lines.
 * Move n lines or one page if n == 0;
 *
 * This function must be called with the current position set to the top of the
 * screen (i.e. cf->page_first).
 */
static void lsp_screen_line_bw(int n)
{
	if (lsp_pos <= 0)
		return;

	/* read backward n lines or one page */
	if (n == 0)
		n = lsp_maxy - 1;

	/*
	 * Get the current top line of the screen and position to the
	 * screen-line in it where the current page starts.
	 */
	struct lsp_line_t *line = lsp_get_this_line();

	/*
	 * After lsp_get_this_line() lsp_pos is at the character following that
	 * line; we want it at the beginning of the just read line.
	 */
	lsp_file_set_pos(line->pos);

	lsp_line_add_screen_lines(line);

	size_t screen_line = 0;

	while (1) {
		if (line->pos + line->scr_line[screen_line] == cf->page_first)
			break;

		screen_line++;

		if (screen_line == line->n_scr_line)
			lsp_error("Cannot find start of current page.\n");
	}

	/*
	 * Set file position and return, if backward movement can be done inside
	 * the current line.
	 */
	if (n <= screen_line) {
		lsp_file_set_pos(line->pos + line->scr_line[screen_line - n]);
		lsp_line_dtor(line);
		return;
	}

	n -= screen_line;

	/*
	 * Go backward in screen-line steps in the current line until count
	 * reaches n.
	 * Get previous lines if we reach the beginning of current lines.
	 */
	while (n > 0) {
		if (lsp_pos == 0)
			break;

		lsp_file_set_prev_line();

		lsp_line_dtor(line);

		line = lsp_get_this_line();
		lsp_file_set_pos(line->pos);

		lsp_line_add_screen_lines(line);

		/*
		 * If movement will end in this line, position to that offset in
		 * the line and return.
		 */
		if (n <= line->n_scr_line) {
			screen_line = line->n_scr_line - n;
			lsp_file_set_pos(line->pos + line->scr_line[screen_line]);
			break;
		}
		n -= line->n_scr_line;
	}

	lsp_line_dtor(line);
}

/*
 * Move backward in file.
 * Move n lines or one page if n == 0;
 */
static void lsp_file_backward(int n)
{
	if (lsp_pos <= 0)
		return;

	/* read backward n lines or one page */
	if (n == 0)
		n = lsp_maxy - 1;

	int i = 0;
	while (i < n) {
		lsp_file_set_prev_line();
		if (lsp_pos == 0)
			break;
		i++;
	}
}

/*
 * Create a file_t stucture for the active reference and fill it by
 * calling man(1).
 *
 * When done, a following lsp_display_page() will display the first
 * page of the then new cf (current_file).
 */
static void lsp_cmd_visit_reference()
{
	struct gref_t *gref = lsp_get_gref_at_pos(cf->current_match);

	/* The ref was not visited yet. Create a new file_t for it. */
	lsp_open_manpage(gref->name);
}

static void lsp_open_manpage(char *name)
{
	lsp_file_add(name, TRUE);

	/* Check if manpage was already open */
	if (cf->blksize)
		return;

	cf->ftype = LSP_FTYPE_MANPAGE;

	lsp_exec_man();
}

/*
 * Create argument vector for exec().
 *
 * For now, we expect a format string that contains exactly one "%s" to be
 * replaced by the second argument str.
 */
char** lsp_create_argv(char *format, char *str)
{
	char **argv = lsp_malloc(sizeof(char *));
	char *tok;
	unsigned int i = 0;

	lsp_debug("%s: building argv for \"%s\" and \"%s\"", __func__, format, str);

	/* Get first token. */
	tok = strtok(strdup(format), " \t");

	while (tok) {
		if (LSP_STR_EQ(tok, "%s"))
			argv[i] = str;
		else
			argv[i] = tok;

		/* Get next token. */
		tok = strtok(NULL, " \t");

		i++;
		argv = lsp_realloc(argv, (i + 1) * sizeof(char *));
	}

	/* Terminate argv. */
	argv[i] = NULL;

	return argv;
}

static void lsp_exec_man()
{
	/*
	 * We want man(1) to see a tty so that it sends us a well
	 * formatted man-page.
	 */
	struct winsize winsize;
	struct termios termios;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);
	ioctl(STDOUT_FILENO, TCGETS, &termios);

	int ptmxfd;
	pid_t pid = forkpty(&ptmxfd, NULL, &termios, &winsize);

	if (pid == -1)
		lsp_error("%s", strerror(errno));

	if (pid == 0) {		/* child process */
		putenv("PAGER=cat");

		char **e_argv = lsp_create_argv(lsp_reload_command, cf->name);

		execvp(e_argv[0], e_argv);
		lsp_error("execvp() failed.\n");
	}

	cf->fd = ptmxfd;

	/* parent process */
	cf->size = LSP_FSIZE_UNKNOWN;

	struct stat statbuf;

	fstat(ptmxfd, &statbuf);

	cf->blksize = statbuf.st_blksize;
	// fixme: perhaps, use pagesize, here.
	//cf->blksize = 1024;

	lsp_file_read_all();

	while (1) {
		int wstatus;

		pid_t ret_pid = waitpid(pid, &wstatus, 0);

		if (ret_pid == -1)
			lsp_error("waitpid: %s\n", strerror(errno));

		if (WIFEXITED(ret_pid)) {
			lsp_debug("%s: child %d exited: %d",
				  __func__, pid, WEXITSTATUS(wstatus));
			break;
		} else if (WIFSIGNALED(ret_pid)) {
			lsp_debug("%s: child %d terminated by signal: %d (%s)",
				  __func__, pid, WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
			break;
		}
		lsp_debug("%s: still waiting for child %d to exit...",
			  __func__, pid);
	}

	/*
	 * Set file name to correct name of manual page.
	 */
	char *name = lsp_detect_manpage();
	if (name == NULL || LSP_STR_EQ(cf->name, name)) {
		free(name);
		return;
	}

	struct file_t *fp = lsp_file_find(name);
	if (fp) {
		/*
		 * That manual page is already there.
		 * Remove the just created file and use the already existing
		 * one.
		 */
		free(name);
		lsp_file_kill();
		cf = fp;
	} else {
		/* New file -- use correct name. */
		free(cf->name);
		cf->name = name;
	}
}

static void lsp_cmd_kill_file()
{
	/* We are all done if the file to kill is the only one. */
	if (cf == cf->next)
		lsp_finish();

	lsp_file_kill();
}

static void lsp_cmd_resize()
{
	getmaxyx(lsp_win, lsp_maxy, lsp_maxx);

	lsp_file_set_pos(cf->page_first);

	if (cf->ftype == LSP_FTYPE_MANPAGE)
		lsp_file_reload();

	struct file_t *here = cf->next;

	/* Mark other manual pages in the ring for reload as soon as they become
	   current. */
	while (here != cf) {
		if (here->ftype == LSP_FTYPE_MANPAGE)
			here->do_reload = true;
		here = here->next;
	}
}

static void lsp_file_reload()
{
	lsp_file_reset();

	lsp_file_do_reload();

	cf->do_reload = false;

	/* Fix pointers */
	if (cf->page_first > cf->size) {
		lsp_file_set_pos(cf->size);
		lsp_file_backward(0);
	} else {
		lsp_file_set_pos(cf->page_first);
		lsp_goto_bol();
	}

	cf->page_first = lsp_pos;
	lsp_set_no_current_match();
}

static void lsp_file_do_reload()
{
	lsp_mode_t old_mode;

	if (cf->ftype == LSP_FTYPE_MANPAGE)
		lsp_exec_man();

	if (cf->toc) {
		lsp_toc_dtor(cf);
		/* A TOC must be created in neutral mode (!toc).
		   But we also want to stay in TOC mode if this is where the
		   resize happened. */
		old_mode = cf->mode;
		lsp_mode_unset_toc();
		lsp_file_create_toc();
		cf->mode = old_mode;
	}
}

/*
 * Reset file_t structure prior to re-reading the file.
 * This usually happens on window resize.
 */
static void lsp_file_reset()
{
	lsp_file_data_dtor(cf->data);
	cf->data = NULL;

	lsp_file_close();

	cf->size = LSP_FSIZE_UNKNOWN;
	cf->seek = 0;
	cf->page_last = 0;
	cf->getch_pos = 0;
	cf->unaligned = 0;
	cf->lines_count = 1;
	cf->current_match = lsp_no_match;

}

/*
 * Create a file_t structure that lists all open files.
 */
static void lsp_files_list()
{
	size_t line_size = 1024;
	char *line = lsp_malloc(line_size);
	struct file_t *file_p = cf->prev;

	/* Do nothing if there is only a single file. */
	if (cf == file_p) {
		lsp_prompt = "No other files opened.";
		return;
	}

	lsp_file_add("List of open files", 1);

	do {
		char *name = file_p->name;
		size_t nlen = strlen(name);

		/* Create name for unnamed stdin. */
		if (nlen == 0) {
			name = "*stdin*";
			nlen = strlen(name);
		}

		if (nlen >= line_size) {
			line = lsp_realloc(line, nlen + 1);
			line_size = nlen + 1;
		}

		memcpy(line, name, nlen);
		line[nlen] = '\n';

		lsp_file_add_line(line);

		file_p = file_p->prev;
	} while (LSP_STR_NEQ(file_p->name, "List of open files"));

	free(line);

	char *file_name = lsp_cmd_select_file();

	lsp_file_kill();

	/* Go to selected file or stay where we were if no file was
	   selected. */
	if (file_name != NULL)
		cf = lsp_file_find(file_name);
	else
		cf = cf->prev;

	free(file_name);
}

/*
 * Show buffer with open files and let the user select one of them.
 *
 * Return name of selected file -- without newline character.
 * Return NULL to indicate no selection, i.e. just close this
 * selection dialog.
 */
static char *lsp_cmd_select_file()
{
	size_t first_line;
	size_t last_line;
	size_t line_no = 0;
	struct lsp_line_t *line;
	char *file_name;
	lsp_prompt = "Select file and press ENTER.";

	lsp_display_page();
	lsp_create_status_line();

	while (1) {
		mvwchgat(lsp_win, line_no, 0, -1, A_STANDOUT, LSP_REVERSE_PAIR, NULL);

		int cmd = wgetch(lsp_win);

		switch (cmd) {
		case '\n':
			/* Visit file on active line. */
			first_line = lsp_file_pos2line(cf->page_first) - 1;

			line = lsp_get_line_at_pos(cf->lines[first_line + line_no]);

			/* Remove the final newline '\n'.*/
			line->raw[line->len - 1] = '\0';

			lsp_debug("%s: selected file %s\n", __func__, line->raw);

			/* The name *stdin* is a generated one that needs to be
			   converted. */
			if (LSP_STR_EQ(line->raw, "*stdin*")) {
				file_name = strdup("");
			} else
				file_name = strdup(line->raw);

			lsp_line_dtor(line);
			return file_name;

		case KEY_DOWN:
			if (line_no == (lsp_maxy - 2)) {
				/* If possible, go to next page. */
				if (cf->page_last < cf->size) {
					lsp_cmd_forward(1);
					lsp_display_page();
					lsp_prompt = "Select file and press ENTER.";
					lsp_create_status_line();
				}
				break;
			}

			/* Active line is not the last one. We stay on this page. */
			mvwchgat(lsp_win, line_no, 0, -1, A_NORMAL, LSP_DEFAULT_PAIR, NULL);

			if (cf->page_last == cf->size) {
				/* End of data is on this page.
				   Don't scroll beyond it. */
				first_line = lsp_file_pos2line(cf->page_first);
				last_line = lsp_file_pos2line(cf->page_last);

				if (line_no == last_line - first_line)
					break;
			}

			line_no++;
			break;

		case KEY_UP:
			/* Decrement line number if we are not already
			   at the top of the screen. */
			if (line_no) {
				mvwchgat(lsp_win, line_no, 0, -1, A_NORMAL, LSP_DEFAULT_PAIR, NULL);
				line_no--;
				break;
			}

			/* If we are not at the beginning of the data,
			   go back a line. */
			if (cf->page_first > 0) {
				lsp_cmd_backward(1);
				lsp_display_page();
				lsp_prompt = "Select file and press ENTER.";
				lsp_create_status_line();
			}

			break;

		case KEY_RESIZE:
			lsp_cmd_resize();
			lsp_display_page();
			lsp_prompt = "Select file and press ENTER.";
			lsp_create_status_line();
			break;

		case KEY_PPAGE:
			lsp_cmd_backward(0); /* 0 == one page */
			lsp_display_page();
			lsp_prompt = "Select file and press ENTER.";
			lsp_create_status_line();
			break;

		case KEY_NPAGE:
			lsp_display_page();
			lsp_prompt = "Select file and press ENTER.";
			lsp_create_status_line();
			break;

		case 'q':
		case 'Q':
			return NULL;
		} /* end switch() */
	}
}

/*
 * Create Apropos buffer that lists all manual page on this system.
 *
 * In combination with TAB, this can act as an entry point to access
 * any manual page.
 */
static void lsp_cmd_create_apropos()
{
	lsp_file_add("Apropos", true);

	/* Do nothing else if file already exists. */
	if (cf->size != LSP_FSIZE_UNKNOWN)
		return;

	FILE *fp = popen(lsp_apropos_command, "r");

	if (fp == NULL) {
		lsp_error("%s: could not popen(\"%s\").\n", __func__, lsp_apropos_command);
	}

	/* Remember that we need to pclose(3) this pipe. */
	cf->flags |= LSP_FLAG_POPEN;
	cf->fp = fp;
	cf->fd = fileno(fp);

	struct stat statbuf;
	fstat(STDIN_FILENO, &statbuf);

	cf->blksize = statbuf.st_blksize;

	lsp_file_add_block();

	if (lsp_verify_with_apropos)
		lsp_apropos_create_grefs();
}

/*
 * Process the apropos buffer and add valid grefs for each line in it.
 *
 * The apropos buffer consits of lines like:
 *
 * "xyz(nn) - some description\n"
 *
 * So, a gref can be build from the first part (which is a reference in
 * lsp-jargon).
 */
static void lsp_apropos_create_grefs()
{
	size_t line_nr;
	struct lsp_line_t *line;
	char *end_ref;
	struct gref_t *gref;

	lsp_file_read_all();

	/*
	 * Process each line of the apropos file and add a gref marked valid.
	 */
	for (line_nr = 0; line_nr < cf->lines_count; line_nr++) {
		/* Get next line */
		line = lsp_get_line_at_pos(cf->lines[line_nr]);

		/* Find closing parenthesis of "xyz(nn)" */
		end_ref = strchr(line->normalized, ')') + 1;

		assert(end_ref != (void *)1);

		/* Terminate the line string after the reference. */
		*end_ref = '\0';

		gref = lsp_gref_search(line->normalized);

		assert(gref != NULL);

		gref->valid = 1;

		lsp_line_dtor(line);
	}
}

static void lsp_cmd_open_manpage()
{
	char manpage_name[256];

	/* Read name of manpage */
	if (wmove(lsp_win, lsp_maxy - 1, 0) == ERR)
		lsp_error("wmove failed.\n");
	wattr_set(lsp_win, A_NORMAL, LSP_DEFAULT_PAIR, NULL);

	mvwaddstr(lsp_win, lsp_maxy - 1, 0, "Enter name of manpage, e.g. xyz(n): ");

	wclrtoeol(lsp_win);

	wrefresh(lsp_win);

	curs_set(1);
	echo();
	wgetnstr(lsp_win, manpage_name, sizeof(manpage_name));
	noecho();
	curs_set(0);

	lsp_open_manpage(manpage_name);
}

static void lsp_cmd_mouse()
{
	MEVENT event;

	int ret = getmouse(&event);

	if (ret == ERR) {
		lsp_debug("%s: no mouse event detected", __func__);
		goto out;
	}

/*
	char x = 1;
	if (BUTTON_RELEASE(event.bstate, x))
		lsp_debug("%s: button release", __func__);
	if (BUTTON_CLICK(event.bstate, x))
		lsp_debug("%s: button click", __func__);
	if (BUTTON_DOUBLE_CLICK(event.bstate, x))
		lsp_debug("%s: button double click", __func__);
	if (BUTTON_TRIPLE_CLICK(event.bstate, x))
		lsp_debug("%s: button triple click", __func__);
*/
	/* Button 1 click -> place cursor at this position */
	if (BUTTON_CLICK(event.bstate, 1)) {
		lsp_cursor_y = event.y;
		lsp_cursor_x = event.x;
		lsp_cursor_set = true;

		lsp_file_set_pos(cf->page_first);
		return;
	}

	/* Button 1 double click -> try to open reference if any at point */
	if (BUTTON_DOUBLE_CLICK(event.bstate, 1)) {
		lsp_file_set_pos(cf->page_first);
		return;
	}

	/* Wheel up => previous page */
	if (BUTTON_PRESS(event.bstate, 4)) {
		lsp_cmd_backward(1);
		return;
	}

	/* Wheel down => next page */
	if (BUTTON_PRESS(event.bstate, 5)) {
		lsp_cmd_forward(1);
		return;
	}

out:
	/* No known event -- change nothing. */
	lsp_file_set_pos(cf->page_first);
}

/*
 * Create status line at the bottom of the screen.
 */
static void lsp_create_status_line()
{
	int x;

	wmove(lsp_win, lsp_maxy - 1, 0);

	//wrefresh(lsp_win);

	wattr_set(lsp_win, A_STANDOUT, LSP_REVERSE_PAIR, NULL);

	if (cf->ftype == LSP_FTYPE_MANPAGE) {
		mvwaddstr(lsp_win, lsp_maxy - 1, 0, "Manual page ");
	}

	x = getcurx(lsp_win);

	/* Display filename.
	   stdin has no name and we want to display something reasonable. */
	if (cf->name[0] == '\0')
		mvwaddstr(lsp_win, lsp_maxy - 1, x, "*stdin*");
	else
		mvwaddstr(lsp_win, lsp_maxy - 1, x, cf->name);

	x = getcurx(lsp_win);
	if (cf->size == LSP_FSIZE_UNKNOWN || cf->seek < cf->size)
		mvwprintw(lsp_win, lsp_maxy - 1, x,
			  " line %ld",
			  lsp_file_pos2line(cf->page_first));
	else
		mvwprintw(lsp_win, lsp_maxy - 1, x,
			  " line %ld/%ld",
			  lsp_file_pos2line(cf->page_first),
			  cf->lines_count);

	wclrtoeol(lsp_win);

	/* If any, put temporary message in the middle of the footer. */
	if (lsp_prompt != NULL) {
		x = (lsp_maxx - strlen(lsp_prompt)) / 2;
		mvwaddstr(lsp_win, lsp_maxy - 1, x, lsp_prompt);
		lsp_prompt = NULL;
	}

	x = lsp_maxx - (strlen(" ('h'elp / 'q'uit)"));
	mvwaddstr(lsp_win, lsp_maxy - 1, x, " ('h'elp / 'q'uit)");

	wclrtoeol(lsp_win);

	lsp_cursor_care();

	wattr_set(lsp_win, A_NORMAL, LSP_DEFAULT_PAIR, NULL);

	wrefresh(lsp_win);
}

/*
 * Care for showing the cursor if necessary.
 */
static void lsp_cursor_care()
{
	if (lsp_cm_cursor_is_valid()) {
		/* Set cursor very visible after current match */
		wmove(lsp_win, cf->cmatch_y, cf->cmatch_x);
		curs_set(2);
		/* In TOC mode, the line with a current match becomes the new
		   active line. */
		cf->toc_cursor = cf->cmatch_y;
	} else {
		if (lsp_cursor_set) {
			wmove(lsp_win, lsp_cursor_y, lsp_cursor_x);
			curs_set(2);
		} else {
			curs_set(0);
		}
	}
}

/*
 * Check if we have a valid cursor position for a current_match.
 */
static bool lsp_cm_cursor_is_valid()
{
	return cf->cmatch_x != -1;
}

/*
 * Worker for toggles that start with '-', e.g. "-i".
 */
static void lsp_cmd_toggle_options()
{
	int cmd = wgetch(lsp_win);

	switch(cmd) {
	case 'h':
		lsp_mode_toggle_highlight();
		break;
	case 'i':
		lsp_case_sensitivity = !lsp_case_sensitivity;

		if (lsp_case_sensitivity) {
			lsp_prompt = "Case sensitivity ON";
		} else {
			lsp_prompt = "Case sensitivity OFF";
		}

		/* If we are in a search the regular expression must be
		   re-compiled. */
		if (lsp_search_regex) {
			lsp_search_compile_regex(LSP_SEARCH_MODE);
		}
		break;
	case 'c':
		lsp_chop_lines = !lsp_chop_lines;

		if (lsp_chop_lines) {
			lsp_prompt = "Chopping lines that do not fit.";
		} else {
			lsp_prompt = "Lines chopping turned OFF.";
		}
		break;
	case 'n':
		lsp_do_line_numbers = !lsp_do_line_numbers;
		if (lsp_do_line_numbers)
			lsp_maxx -= 8;
		else
			lsp_maxx += 8;
		break;
	case 'V':
		lsp_verify = !lsp_verify;

		if (lsp_verify) {
			lsp_prompt = "Verification of references turned ON.";
		} else {
			lsp_prompt = "Verification of references turned OFF.";
		}
		break;
	} /* switch() */
}

static void lsp_mode_toggle_highlight()
{
	cf->mode ^= LSP_HIGHLIGHT_MODE;
}

static void lsp_mode_set(lsp_mode_t mode)
{
	assert((mode & LSP_SEARCH_MODE) + (mode & LSP_REFS_MODE)
	       != LSP_SEARCH_OR_REFS_MODE);

	/* Ensure only one of the two search modes will be active */
	if (mode & LSP_SEARCH_OR_REFS_MODE)
		cf->mode &= ~LSP_SEARCH_OR_REFS_MODE;

	cf->mode |= mode;
}

static void lsp_mode_set_initial()
{
	cf->mode = LSP_INITIAL_MODE;
}

static void lsp_mode_set_highlight()
{
	cf->mode |= LSP_HIGHLIGHT_MODE;
}

static void lsp_mode_set_toc()
{
	cf->mode |= LSP_TOC_MODE;
}

static bool lsp_mode_is_toc()
{
	return cf->mode & LSP_TOC_MODE;
}

static bool lsp_mode_is_search()
{
	return cf->mode & LSP_SEARCH_MODE;
}

static bool lsp_mode_is_refs()
{
	return cf->mode & LSP_REFS_MODE;
}

static bool lsp_mode_is_highlight()
{
	return cf->mode & LSP_HIGHLIGHT_MODE;
}

static void lsp_mode_unset_highlight()
{
	cf->mode &= ~LSP_HIGHLIGHT_MODE;
}

static void lsp_mode_unset_toc()
{
	cf->mode &= ~LSP_TOC_MODE;
}

static void lsp_mode_unset_search_or_refs()
{
	cf->mode &= ~LSP_SEARCH_OR_REFS_MODE;
}

/*
 * Adjust first TOC line on page to one that is visible in the current level.
 */
static void lsp_toc_first_adjust()
{
	struct toc_node_t *toc = cf->toc_first;

	/* Start with searching backwards. */
	while (toc && toc->level > cf->current_toc_level)
		toc = toc->prev;

	if (toc) {
		cf->toc_first = toc;
		return;
	}

	/* We didn't find a matching TOC entry.
	   Try to search forward. */
	toc = cf->toc_first;
	while (toc && toc->level > cf->current_toc_level)
		toc = toc->next;

	if (!toc)
		lsp_error("%s: cannot find proper TOC entry.\n", __func__);

	cf->toc_first = toc;
}

/*
 * Main loop that acts on user input.
 *
 * Perhaps a misnomer, because lsp_display_page() probably works harder...
 */
static void lsp_workhorse()
{
	/*
	 * The initial command is to display the first page of content.
	 */
	int cmd = ' ';

	while (1) {
		switch (cmd) {
		case 'B':
			lsp_mode_set_initial();
			lsp_file_set_pos(cf->page_first);
			lsp_files_list();
			lsp_display_page();
			break;
		case 'a':
			lsp_mode_set_initial();
			lsp_cmd_create_apropos();
			lsp_display_page();
			break;
		case 'h':
			lsp_open_manpage("lsp-help(1)");
			lsp_display_page();
			break;
		case '-':
			lsp_file_set_pos(cf->page_first);
			lsp_cmd_toggle_options();
			lsp_display_page();
			break;
		case KEY_MOUSE:
			lsp_cmd_mouse();
			lsp_display_page();
			break;
		case KEY_ESC:
			lsp_cursor_set = false;
			lsp_mode_unset_highlight();
			if (lsp_mode_is_toc())
				cf->toc = cf->toc_first;
			else
				lsp_file_set_pos(cf->page_first);
			lsp_display_page();
			break;
		case CTRL_L:
			/*
			 * Bring current search match to the first line.
			 */
			if (lsp_is_a_match(cf->current_match)) {
				lsp_file_set_pos(cf->current_match.rm_so);
				lsp_goto_bol();
				lsp_display_page();
			}
			break;
		case 'c':
			lsp_mode_set_initial();
			lsp_cmd_kill_file();
			lsp_display_page();
			break;
		case KEY_NPAGE:
		case ' ':
		case 'f':
			lsp_cursor_set = false;
			lsp_display_page(); /* next page */
			break;
		case 'g':
		case '<':
			lsp_cursor_set = false;
			if (lsp_mode_is_toc())
				lsp_toc_rewind(0);
			else
				lsp_cmd_goto_start();
			lsp_display_page();
			break;
		case 'G':
		case '>':
			lsp_cursor_set = false;
			if (lsp_mode_is_toc())
				lsp_toc_rewind((off_t)-1);
			else
				lsp_cmd_goto_end();
			lsp_display_page();
			break;
		case KEY_BTAB:	/* Shift-TAB */
			lsp_cursor_set = false;
			lsp_search_direction = LSP_BW;
			lsp_cmd_search_refs();
			lsp_display_page();
			break;
		case '\t':
			lsp_cursor_set = false;
			lsp_search_direction = LSP_FW;
			lsp_cmd_search_refs();
			lsp_display_page();
			break;
		case KEY_RIGHT:
			lsp_shift += 1;
			if (lsp_mode_is_toc())
				lsp_toc_rewind(cf->toc_first->pos);
			else
				lsp_file_set_pos(cf->page_first);
			lsp_display_page();
			break;
		case KEY_LEFT:
			if (lsp_shift)
				lsp_shift = lsp_shift - 1;
			if (lsp_mode_is_toc())
				lsp_toc_rewind(cf->toc_first->pos);
			else
				lsp_file_set_pos(cf->page_first);
			lsp_display_page();
			break;
		case '\n':
			if (lsp_mode_is_toc()) {
				/*
				 * A line in TOC mode was selected.
				 * Show original file starting at that position.
				 */
				lsp_mode_unset_toc();
				lsp_file_set_pos(lsp_toc_get_offset_at_cursor());
				lsp_display_page();
				break;
			} else {
				lsp_cursor_set = false;
				if (lsp_mode_is_refs() &&
				    lsp_is_a_match(cf->current_match)) {
					lsp_cmd_visit_reference();
					lsp_display_page();
					break;
				}
			}
			/* fallthrouh */
		case 'e':
		case KEY_DOWN:
			lsp_cursor_set = false;

			if (!lsp_mode_is_toc()) {
				lsp_cmd_forward(1);
			} else {
				/* In TOC mode, line movement ends a search. */
				lsp_mode_unset_highlight();
				if (cf->toc->next) {
					lsp_toc_rewind(cf->toc_first->pos);

					if (cf->toc_cursor + 1 < lsp_maxy - 1) {
						cf->toc_cursor++;
					} else {
						lsp_toc_fw(lsp_maxy / 2);
						cf->toc_cursor = lsp_maxy / 2 - 1;
					}
				} else {
					if (lsp_toc_get_offset_at_cursor() <
					    cf->toc->pos)
						cf->toc_cursor++;
					lsp_toc_rewind(cf->toc_first->pos);
				}
			}

			lsp_display_page();
			break;
		case KEY_PPAGE:
		case 'b':
			if (lsp_mode_is_toc()) {
				lsp_toc_rewind(cf->toc_first->pos);
				lsp_toc_bw(lsp_maxy - 1);
			} else {
				lsp_cursor_set = false;
				lsp_cmd_backward(0); /* 0 == one page */
			}
			lsp_display_page();
			break;
		case 'y':
		case KEY_UP:
			lsp_cursor_set = false;

			if (lsp_mode_is_toc()) {
				/* In TOC mode, line movement ends a search. */
				lsp_mode_unset_highlight();

				lsp_toc_rewind(cf->toc_first->pos);

				if (cf->toc_cursor) {
					/* Cursor stays on page */
					cf->toc_cursor--;
				} else
					/* Cursor moves out of page */
					if (cf->toc_first->prev) {
						/* We are not on first page.
						   Go 1/2 page up. */
						lsp_toc_bw(lsp_maxy / 2);
						cf->toc_cursor = lsp_maxy / 2 - 1;
					}
			} else {
				lsp_cmd_backward(1);
			}
			lsp_display_page();
			break;
		case 'n':
			lsp_cursor_set = false;
			if (lsp_search_regex) {
				cf->regex_p = lsp_search_regex;

				lsp_file_search_next(LSP_SEARCH_MODE);
			} else {
				if (lsp_mode_is_toc())
					cf->toc = cf->toc_first;
				else
					lsp_file_set_pos(cf->page_first);
			}

			lsp_display_page();
			break;
		case 'm':
			lsp_cmd_open_manpage();
			lsp_display_page();
			break;
		case 'p':
			lsp_cursor_set = false;
			if (lsp_search_regex) {
				cf->regex_p = lsp_search_regex;

				lsp_file_search_prev(LSP_SEARCH_MODE);
			} else {
				if (lsp_mode_is_toc())
					cf->toc = cf->toc_first;
				else
					lsp_file_set_pos(cf->page_first);
			}

			lsp_display_page();
			break;
		case '/':
			lsp_cursor_set = false;
			lsp_search_direction = LSP_FW;
			lsp_cmd_search(true);
			lsp_display_page();
			break;
		case '?':
			lsp_cursor_set = false;
			lsp_search_direction = LSP_BW;
			lsp_cmd_search(true);
			lsp_display_page();
			break;
		case 'T':
			if (lsp_mode_is_toc()) {
				cf->current_toc_level = (cf->current_toc_level + 1) % 3;
				/* If we are switching from
				   level 2 to 0 the current toc_first could
				   become invisible.
				   Rewind to a valid TOC entry. */
				if (!cf->current_toc_level)
					lsp_toc_first_adjust();

				lsp_toc_rewind(cf->toc_first->pos);
			} else {
				lsp_file_create_toc();
				lsp_mode_set_toc();
			}

			lsp_display_page();
			break;
		case KEY_RESIZE:
			lsp_cursor_set = false;
			nodelay(lsp_win, true);

			/* Try to stand storms of resize signals that hit us
			   when users do resizes using their mice. */
			while (cmd == KEY_RESIZE) {
				usleep(200000);
				cmd = wgetch(lsp_win);
				if (cmd == ERR)
					break;
				lsp_debug("Got another KEY_RESIZE");
			}

			lsp_cmd_resize();
			lsp_display_page();
			nodelay(lsp_win, false);
			break;
		case 'q':
		case 'Q':
			if (lsp_mode_is_toc()) {
				lsp_mode_unset_toc();
				lsp_file_set_pos(cf->page_first);
			} else {
				/* Allow to exit from help with 'q' */
				if (LSP_STR_EQ(cf->name, "lsp-help(1)")) {
					lsp_cmd_kill_file();
				} else {
					return;
				}
			}
			lsp_display_page();
			break;
		case ERR:
			lsp_error("%s: cannot read user commands.\n", __func__);
		}

		lsp_create_status_line();

		cmd = wgetch(lsp_win);
		lsp_debug("Next command: %s (0x%04x)", keyname(cmd), cmd);

		if (lsp_mode_is_refs() &&
		    cmd != '\t' &&
		    cmd != KEY_BTAB &&
		    cmd != '\n') {
			lsp_mode_unset_highlight();
			lsp_mode_unset_search_or_refs();
		}
	}
}

/*
 * Find a file structure with the given name or return NULL.
 */
static struct file_t *lsp_file_find(char *name)
{
	struct file_t *file_p = cf;

	if (file_p == NULL)
		return NULL;	/* No files in the ring yet. */

	do {
		if (LSP_STR_EQ(file_p->name, name))
			return file_p;
		file_p = file_p->next;
	} while (file_p != cf);

	return NULL;
}

/*
 * Add a structure for a new file to the ring of file structures.
 *
 * Argument "new_current" steers if it becomes the new current_file or
 * if it is inserted before it.
 *
 * In the first case current_file will become it's predecessor in the
 * ring of input files.
 */
static void lsp_file_add(char *name, bool new_current)
{
	struct file_t *new_file;

	/* Check if a file with that name already exists.
	   Make it current_file if requested. */
	new_file = lsp_file_find(name);

	if (new_file != NULL) {
		if (new_current == TRUE)
			cf = new_file;
		return;
	}

	new_file = lsp_file_ctor();

	new_file->name = strdup(name);

	if (cf == NULL) {
		cf = new_file;
		new_file->prev = new_file->next = new_file;
		return;
	}

	if (new_current == TRUE) {
		/* Save position of current_file */
		lsp_file_set_pos(cf->page_first);

		/*
		new_file->prev = cf;
		new_file->next = cf->next;
		cf->next = new_file;
		new_file->next->prev = new_file;
		cf = new_file;
		*/
		new_file->next= cf;
		new_file->prev = cf->prev;
		cf->prev = new_file;
		new_file->prev->next = new_file;
		cf = new_file;
	} else {
		cf->prev->next = new_file;
		new_file->prev = cf->prev;
		cf->prev = new_file;
		new_file->next = cf;
	}

#if DEBUG
	lsp_print_file_ring();
#endif
}

/*
 * Constructor for new instance of file_t structure.
 */
static struct file_t *lsp_file_ctor()
{
	struct file_t *new_file = lsp_malloc(sizeof(struct file_t));

	new_file->mode = LSP_INITIAL_MODE;
	new_file->name = NULL;
	new_file->rep_name = NULL;
	new_file->fp = NULL;
	new_file->fd = -1;
	new_file->page_first = (off_t)-1;
	new_file->page_last = 0;
	new_file->getch_pos = 0;
	new_file->unaligned = 0;
	new_file->lines_count = 1;
	new_file->lines = lsp_malloc(LSP_LINES_INITIAL_SIZE * sizeof(off_t));
	/* The first line always starts at pos 0 */
	new_file->lines[0] = 0;
	new_file->lines_size = LSP_LINES_INITIAL_SIZE;
	new_file->seek = 0;
	new_file->size = LSP_FSIZE_UNKNOWN;
	new_file->blksize = 0;
	new_file->data = NULL;

	new_file->flags = 0;
	new_file->ftype = LSP_FTYPE_OTHER;
	new_file->do_reload = FALSE;

	new_file->regex_p = NULL;
	new_file->current_match = lsp_no_match;

	new_file->toc = NULL;
	new_file->toc_cursor = 0;
	new_file->toc_first = NULL;
	new_file->toc_last = NULL;
	new_file->current_toc_level = 0;

	return new_file;
}

static void lsp_version()
{
	endwin();
	printf("lsp version %s\n", LSP_VERSION);
}

static void lsp_usage(const char *pathname)
{
	endwin();
	printf("Usage:\n");
	printf("%s [options] [file_name]...\n", pathname);
	printf("%s -v\t\tprint version\n", pathname);
	printf("%s -h\t\tprint help\n", pathname);
}

/*
 * Parse options from environment variable:
 * identify words separated by one or more spaces but also recognize quotes.
 */
static void lsp_parse_env_options(int *argc, char **argv[], char *options)
{
	int ac1;

	*argc = 1;
	size_t i;
	char in_quotes = FALSE;
	char in_word = FALSE;

	/* First pass: count options for allocation of pointer array. */
	for (i = 0; options[i]; i++) {
		if (options[i] == ' ') {
			(*argc)++;
			while (options[++i] == ' ')
				; /* just forward */
		}
	}

	(*argc)++;
	/* Save value for later comparison */
	ac1 = *argc;

	*argv = lsp_malloc(*argc * sizeof(char *));

	*argc = 0;

	(*argv)[0] = "lsp_options";

	/* Second pass: actually build argument vector. */
	for (i = 0; options[i]; i++) {
		if (options[i] == '"') {
			in_quotes = !in_quotes;
			if (in_quotes == TRUE)
				(*argv)[++(*argc)] = options + i + 1;
			if (in_quotes == FALSE)
				options[i] = '\0';
			continue;
		}

		if (in_quotes == TRUE)
			continue;

		if (options[i] == ' ') {
			options[i] = '\0';
			in_word = FALSE;
			continue;
		}

		if (in_word == FALSE) {
			(*argv)[++(*argc)] = options + i;
			in_word = TRUE;
		}
	}

	(*argc)++;

	if (ac1 != *argc)
		lsp_error("%s: problem with counting options: %d vs %d\n",
			  __func__, ac1, *argc);

	if (in_quotes == TRUE)
		lsp_error("%s: unmatched quotes in options: %s.\n",
			  __func__, options);

}

/*
 * Read LSP_OPEN or LESSOPEN environment variables.
 *
 * LSP_OPEN has higher priority, LESSOPEN is processed if the former isn't set.
 */
static void lsp_process_env_open()
{
	lsp_env_open = getenv("LSP_OPEN");

	if (lsp_env_open == NULL)
		lsp_env_open = getenv("LESSOPEN");
}

/*
 * Options can also be given in the environment variable LSP_OPTIONS.
 *
 * Break it up into words and then process them as options.
 */
static void lsp_process_env_options()
{
	int env_argc;
	char **env_argv;

	char *lsp_options = getenv("LSP_OPTIONS");

	if (lsp_options == NULL)
		return;

	/* Remove leading white-space */
	size_t i;

	for (i = 0; isspace(lsp_options[i]); i++)
		;		/* just count */

	if (i != 0)
		memmove(lsp_options, lsp_options + i, (strlen(lsp_options) + 1) - i);

	/* We're done if white-space was the only content. */
	if (lsp_options[0] == '\0')
		return;

	lsp_options = strdup(lsp_options);

	lsp_parse_env_options(&env_argc, &env_argv, lsp_options);

	lsp_process_options(env_argc, env_argv);

	free(env_argv);
	free(lsp_options);
}

static void lsp_process_options(int argc, char *argv[])
{
	int opt;
	int long_index;

	/* getopt(3p) says the behavior of getopt() is unspecified if
	   the caller sets optind to 0.	 But at least glibc uses this
	   value to cause an initialization -- e.g. for cases where
	   multiple argument vectors are processed and this is the
	   case for lsp. */
	optind = 0;

	struct option long_options[] = {
		{"load-apropos",	optional_argument,	0, 'a'},
		{"chop-lines",		no_argument,		0, 'c'},
		{"help",		no_argument,		0, 'h'},
		{"no-case",		no_argument,		0, 'i'},
		{"man-case",		no_argument,		0, 'I'},
		{"log-file",		required_argument,	0, 'l'},
		{"line-numbers",	no_argument,		0, 'n'},
		{"output-file",		required_argument,	0, 'o'},
		{"search_string",	required_argument,	0, 's'},
		{"version",		no_argument,		0, 'v'},
		{"no-color",		no_argument,		0, '0'},
		{"no-verify",		no_argument,		0, 'V'},
		{"reload-command",	required_argument,	0, '1'},
		{"verify-command",	required_argument,	0, '2'},
		{"verify-with-apropos", no_argument,		0, '3'},
		{0,			0,			0,  0 }
	};

	while (1) {
		opt = getopt_long(argc, argv, "achiIl:no:s:Vv",
				  long_options, &long_index);

		if (opt == -1)
			break;

		switch (opt) {
		case '0':
			/* --no-color */
			lsp_color = false;
			break;
		case '1':
			/* --reload-command */
			lsp_reload_command = strdup(optarg);
			break;
		case '2':
			/* --verify-command */
			lsp_verify_command = strdup(optarg);
			break;
		case '3':
			/* --verify-with-apropos */
			lsp_verify_with_apropos = true;
			break;
		case 'a':
			lsp_load_apropos = true;
			if (optarg)
				lsp_apropos_command = strdup(optarg);
			break;
		case 'c':
			lsp_chop_lines = !lsp_chop_lines;
			break;
		case 'i':
			/* Toggle case sensitivity for searches */
			lsp_case_sensitivity = !lsp_case_sensitivity;
			break;
		case 'I':
			/* Toggle case sensitivity for man-pages */
			lsp_man_case_sensitivity = 1;
			break;
		case 'l':
			lsp_logfile = strdup(optarg);
			break;
		case 's':
			strcpy(lsp_search_string, optarg);
			break;
		case 'n':
			lsp_do_line_numbers = true;
			break;
		case 'o':
			lsp_ofile = open(optarg,
					 O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU);
			break;
		case 'v':
			lsp_version();
			exit(EXIT_SUCCESS);
		case 'V':
			lsp_verify = !lsp_verify;
			break;
		case 'h':
		default:
			lsp_usage(argv[0]);
			exit(EXIT_SUCCESS);
		}

	}
#if DEBUG
	lsp_init_logfile();
#endif
	/* Now, process file names in argv */
	while (optind < argc) {
		lsp_file_add(argv[optind++], 0);
	}

}

/*
 * Cleanly remove global reference data
 */
static void lsp_grefs_dtor()
{
	struct gref_t *next;

	lsp_debug("%s: destroying grefs", __func__);

	while (lsp_grefs != NULL) {
		free(lsp_grefs->name);

		/* Safe next ptr before freeing it's container. */
		next = lsp_grefs->next;

		free(lsp_grefs);
		lsp_grefs = next;
	}

	/* Destroy the corresponding hash table. */
	hdestroy();
}

/*
 * We're done.	Clean up and exit.
 */
static void lsp_finish()
{
	lsp_debug("Doing cleanup to exit.");

	lsp_file_ring_dtor();

	lsp_grefs_dtor();

	if (isendwin() == FALSE)
		endwin();

	if (lsp_ofile > 0)
		close(lsp_ofile);

	if (lsp_logfp && lsp_logfp != stderr)
		fclose(lsp_logfp);

	free(lsp_reload_command);
	free(lsp_verify_command);
	free(lsp_apropos_command);

	exit(EXIT_SUCCESS);
}

#if DEBUG

/*
 * Prepare FILE* for logging - it could be a file given by -l.
 * If so, that stream should be line buffered so that tail-f'ing it
 * works fine (lsp-f'ing is not yet possible).
 */
static void lsp_init_logfile()
{
	int log_fd;

	if (lsp_logfp != NULL)
		return;

	if (!lsp_logfile) {
		lsp_logfp = stderr;
		return;
	}

	log_fd = mkstemp(lsp_logfile);

	if (log_fd == -1)
		lsp_error("%s: %s\n", lsp_logfile, strerror(errno));

	lsp_logfp = fdopen(log_fd, "w");

	if (lsp_logfp == NULL)
		lsp_error("%s: %s\n", lsp_logfile, strerror(errno));

	setlinebuf(lsp_logfp);
}
#endif

static void lsp_init()
{
	/*
	 * git-diff(1) sets this and makes resizes behave bad.
	 * Especially those that shrink the window.
	 */
	unsetenv("COLUMNS");

	setlocale(LC_CTYPE, "");

	if (LSP_STR_EQ(nl_langinfo(CODESET), "UTF-8"))
		lsp_utf_8 = TRUE;

	lsp_cursor_y = 0;
	lsp_cursor_x = 0;
	lsp_cursor_set = false;

	lsp_no_match.rm_so = lsp_no_match.rm_eo = (off_t)-1;
	lsp_case_sensitivity = false;

	lsp_color = true;

	lsp_tab_width = 8;
	lsp_chop_lines = false;
	lsp_load_apropos = false;
	lsp_apropos_command = strdup("apropos . | sort | sed 's/ (/(/'");

	lsp_reload_command = strdup("man %s");

	lsp_verify_command = strdup("man -w \"%s\" > /dev/null 2>&1");

	lsp_verify_with_apropos = false;

	lsp_verify = true;

	lsp_htable_entries = 100000;
	lsp_grefs_count = 0;

}

int main(int argc, char *argv[])
{
	lsp_init();

	lsp_init_screen();

	lsp_process_env_open();

	lsp_process_env_options();

	lsp_process_options(argc, argv);

	lsp_file_init_ring();

#if DEBUG
	lsp_print_file_ring();
#endif

	if (*lsp_search_string != '\0') {
		lsp_display_page();
		lsp_search_direction = LSP_FW;
		lsp_cmd_search(false);
	}

	lsp_workhorse();

	lsp_finish();

	/* Usually, we should not reach this. */
	exit(EXIT_SUCCESS);
}
