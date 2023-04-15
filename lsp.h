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

#ifndef _LSP_H_GUARD_
#define _LSP_H_GUARD_

#include <stdio.h>
#include <regex.h>
#include <curses.h>
#include <fcntl.h>

#define LSP_STR_EQ(a, b) (strcmp(a, b) == 0)
#define LSP_STR_NEQ(a, b) (strcmp(a, b) != 0)

/*
 * We store each file's content in a ring of buffers of size blksize.
 */
struct data_t {
	off_t seek;	       // position of buffer inside the file
	unsigned char *buffer; // the content
	struct data_t *prev;
	struct data_t *next;
};

/*
 * TOC entries are pointers to lines with indentation levels 0,4,8
 * which are kept in a linked list:
 */
struct toc_node_t {
	off_t pos;		/* Position in source file */

	int level;		/* Indentation level 0-2 */

	struct toc_node_t *prev;
	struct toc_node_t *next;
};

/*
 * Structure for line operations.
 * The member normalized is for a string without formatting information,
 * i.e. backspace and SGR sequences.
 *
 * Lines might be longer than the screen width.  So, we also maintain pointers
 * to "screen-lines".  Currently, this overhead just makes scrolling backwards
 * simpler.
 */
struct lsp_line_t {
	off_t pos;		/* absolute position in file */

	size_t len;		/* length of raw */
	char *raw;		/* raw line content */

	size_t nlen;		/* length of normalized */
	char *normalized;	/* normalized content */

	size_t n_scr_line;	/* number of screen lines */
	off_t *scr_line;	/* pointers to offsets in raw that
				 * correspond to lines on the screen */
};

/*
 * Globally keep track of what references we validated.
 * No matter what file we are paging it came from.
 */
struct gref_t {
	char *name;
	int valid;
	struct gref_t *next;
} *lsp_grefs;

/* lsp modes of operation */
enum lsp_mode {
	LSP_INITIAL_MODE = 0,
	/*
	 * The following two are mutually exclusive.
	 */
	LSP_REFS_MODE = 1,		// currently looking at references

	LSP_SEARCH_MODE = 2,		// search has been done and
					// current_match is meaningful

	LSP_SEARCH_OR_REFS_MODE = 3,	// mask to check if we are
					// in one of search or refs
					// mode -- both of them are
					// searches and share functionality

	LSP_TOC_MODE = 4,		// currently operating in TOC

	LSP_HIGHLIGHT_MODE = 8,		// Highlight matches after a search.
					// Toggle with '-h'.

	LSP_REFS_HIGHLIGHT_MODE = 9,    // Two constants for combinations of modes.
	LSP_SEARCH_HIGHLIGHT_MODE = 10  // Just for the debugger.
};

typedef enum lsp_mode lsp_mode_t;


static void			lsp_apropos_create_grefs(void);
static size_t			lsp_buffer_free_size(void);
static void *			lsp_calloc(size_t, size_t);
static bool			lsp_cm_cursor_is_valid(void);
static void			lsp_cmd_backward(int);
static void			lsp_cmd_create_apropos(void);
static void			lsp_cmd_forward(int);
static void			lsp_cmd_goto_end(void);
static void			lsp_cmd_goto_start(void);
static void			lsp_cmd_kill_file(void);
static void			lsp_cmd_mouse(void);
static void			lsp_cmd_open_manpage(void);
static void			lsp_cmd_resize(void);
static void			lsp_cmd_search(bool);
static void			lsp_cmd_search_refs(void);
static void			lsp_cmd_toggle_options(void);
static void			lsp_cmd_visit_reference(void);
static char *			lsp_cmd_select_file(void);
static int			lsp_cmp_line_pos(size_t, off_t);
static char**			lsp_create_argv(char *, char *);
static void			lsp_create_status_line(void);
static void			lsp_cursor_care(void);
static int			lsp_debug(const char *, ...);
static size_t			lsp_decode_sgr(const char *, attr_t *, short *);
static char *			lsp_detect_manpage(void);
static void			lsp_display_page(void);
static int			lsp_error(const char *, ...) __attribute__ ((noreturn));
static void			lsp_exec_man(void);
static int			lsp_expand_tab(size_t);
static void			lsp_file_add(char *, bool);
static void			lsp_file_add_block(void);
static ssize_t			lsp_file_add_line(const char *);
static void			lsp_file_align_buffer(void);
static void			lsp_file_backward(int);
static void			lsp_file_close(void);
static void			lsp_file_create_toc(void);
static struct file_t *		lsp_file_ctor(void);
static void			lsp_file_data_ctor(size_t);
static void			lsp_file_data_dtor(struct data_t *);
static void			lsp_file_do_reload(void);
static void			lsp_file_dtor(struct file_t *);
static struct file_t *		lsp_file_find(char *);
static void			lsp_file_forward_empty_lines(size_t);
static void			lsp_file_forward_words(size_t);
static struct lsp_line_t *	lsp_file_get_prev_line(void);
static int			lsp_file_getch(void);
static void			lsp_file_init_ring(void);
static void			lsp_file_init_stdin(void);
static void			lsp_file_kill(void);
static void			lsp_file_move_here(struct file_t *);
static int			lsp_file_peek_bw(void);
static int			lsp_file_peek_fw(void);
static size_t			lsp_file_pos2line(off_t);
static void			lsp_file_read_all(void);
static ssize_t			lsp_file_read_block(size_t);
static void			lsp_file_reload(void);
static void			lsp_file_reset(void);
static void			lsp_file_ring_dtor(void);
static void			lsp_file_search_next(lsp_mode_t);
static void			lsp_file_search_prev(lsp_mode_t);
static void			lsp_file_set_pos(off_t);
static void			lsp_file_set_prev_line(void);
static void			lsp_file_toc_add(const struct lsp_line_t *, int);
static void			lsp_file_ungetch(void);
static void			lsp_files_list(void);
static void			lsp_finish(void) __attribute__ ((noreturn));
static short			lsp_get_color_pair(short, short);
static struct gref_t *		lsp_get_gref_at_pos(regmatch_t);
static struct lsp_line_t *	lsp_get_line_at_pos(off_t);
static struct lsp_line_t *	lsp_get_line_from_here(void);
static struct lsp_line_t *	lsp_get_next_display_line(void);
static size_t			lsp_get_sgr_len(const char *);
static struct lsp_line_t *	lsp_get_this_line(void);
static struct gref_t *		lsp_gref_find(char *);
static int			lsp_gref_henter(struct gref_t *);
static void			lsp_goto_bol(void);
static void			lsp_grefs_dtor(void);
static struct gref_t *		lsp_gref_search(char *);
static void			lsp_init(void);
static void			lsp_init_cmd_input(void);
#if DEBUG
static void			lsp_init_logfile(void);
#endif
static int			lsp_init_screen(void);
static void			lsp_invalidate_cm_cursor(void);
static bool			lsp_is_a_match(regmatch_t);
static bool			lsp_is_at_bol(void);
static bool			lsp_is_no_match(regmatch_t);
static bool			lsp_is_sgr_sequence(const char *);
static void			lsp_line_add_screen_lines(struct lsp_line_t *);
static size_t			lsp_line_count_words(struct lsp_line_t *);
static struct lsp_line_t *	lsp_line_ctor(void);
static void			lsp_line_cut_tail(struct lsp_line_t *, off_t);
static void			lsp_line_dtor(struct lsp_line_t *);
static int			lsp_line_handle_leading_sgr(attr_t *, short *);
static void			lsp_line_fw_screen_line(struct lsp_line_t *);
static size_t			lsp_line_get_matches(const struct lsp_line_t *, regmatch_t **);
static void			lsp_lines_add(off_t);
static void *			lsp_malloc(size_t);
static char *			lsp_man_get_section(off_t);
static void			lsp_man_goto_section(char *);
static void			lsp_man_reposition(char *);
static size_t			lsp_mbtowc(wchar_t *, const char *, size_t);
static bool			lsp_mode_is_highlight(void);
static bool			lsp_mode_is_refs(void);
static bool			lsp_mode_is_search(void);
static bool			lsp_mode_is_toc(void);
static void			lsp_mode_set(lsp_mode_t);
static void			lsp_mode_set_highlight(void);
static void			lsp_mode_set_initial(void);
static void			lsp_mode_set_toc(void);
static void			lsp_mode_toggle_highlight(void);
static void			lsp_mode_unset_highlight(void);
static void			lsp_mode_unset_search_or_refs(void);
static void			lsp_mode_unset_toc(void);
static char *			lsp_normalize(const char *, size_t);
static size_t			lsp_normalize_count(const char *, size_t);
static int			lsp_open_file(const char *);
static void			lsp_open_manpage(char *);
static void			lsp_parse_env_options(int *, char ***, char *);
#if DEBUG
static void			lsp_print_file_ring(void);
#endif
static void			lsp_process_env_options(void);
static void			lsp_process_options(int, char **);
static bool			lsp_pos_is_current_page(off_t);
static bool			lsp_pos_is_toc(off_t);
static void			lsp_process_env_open(void);
static void *			lsp_realloc(void *, size_t);
static bool			lsp_ref_is_valid(struct gref_t *);
static void			lsp_screen_line_bw(int);
static void			lsp_screen_line_fw(int);
static void			lsp_search_align_page_to_match(void);
static void			lsp_search_align_toc_to_match(void);
static void			lsp_search_align_to_match(void);
static char *			lsp_search_compile_regex(lsp_mode_t);
static regmatch_t		lsp_search_file_next(void);
static regmatch_t		lsp_search_find_prev_match(struct lsp_line_t **);
static regmatch_t		lsp_search_next(void);
static regmatch_t		lsp_search_toc_next(void);
static void			lsp_set_no_current_match(void);
static char *			lsp_to_lower(char *);
static struct toc_node_t *	lsp_pos_to_toc(off_t);
static void			lsp_toc_bw(size_t);
static void			lsp_toc_dtor(struct file_t *);
static void			lsp_toc_first_adjust(void);
static void			lsp_toc_fw(size_t);
static off_t			lsp_toc_get_offset_at_cursor(void);
static int			lsp_toc_move_to_next(void);
static int			lsp_toc_move_to_prev(void);
static void			lsp_toc_rewind(off_t);
static void			lsp_usage(const char *);
static bool			lsp_validate_ref_at_pos(regmatch_t);
static void			lsp_version(void);
static void			lsp_workhorse(void);
/*
 * Ring for input file housekeeping
 */
struct file_t {
	lsp_mode_t mode;      // mode of operation: TOC mode, search mode etc.
	off_t getch_pos;      // next byte for getch() (0-based)
	int unaligned;	      // chetch_pos not aligned to buffers

	char *name;	      // pathname
	char *rep_name;	      // replacement name from preprocessor (if any)
	/*
	 * If we are a pipe to a preprocessor we read one byte to test if we
	 * get data from it.  This byte gets stored here and we need to consume
	 * it when reading the first full buffer.
	 * LSP_PRE_READ is set in flags when this has valid data.
	 */
	int pre_read;
	/*
	 * Usually, we work on file descriptors.
	 * Files fed by popen(3) are different, because closing them
	 * is special.
	 */
	FILE *fp;
	int fd;

	off_t page_first;     // first byte in current page
	off_t page_last;      // last byte in current page

	size_t lines_count;   // number of lines in file -- so far
	off_t *lines;	       // record the offsets of the lines in
			      // the file.
	size_t lines_size;    // current size of the above array

	off_t seek;	      // current position in file
	off_t size;	      // size of the file
	blksize_t blksize;    // preferred blksize to use

	struct data_t *data;  // content of the file that we already read

	struct file_t *prev;
	struct file_t *next;

	char flags;
	char ftype;
	bool do_reload;
	/*
	 * Pointer to last used regular expression for searching or showing refs.
	 */
	regex_t *regex_p;
	/*
	 * This variable contains _absolute_ start end end offsets of a search
	 * match (either references or user-search-pattern).
	 *
	 * (off_t)-1 means there is no current match.
	 */
	regmatch_t current_match;

	/* Position of current match for blinking cursor.
	   cmatch_x == -1 := no valid cursor position */
	int cmatch_y, cmatch_x;

	struct toc_node_t *toc;
	/*
	 * The current active line in the TOC window.
	 */
	size_t toc_cursor;
	/*
	 * Remember the first and last toc entries (offsets in the file) in the
	 * window for navigation.
	 */
	struct toc_node_t *toc_first;
	/* toc_last == NULL means: last TOC entry is on current page. */
	struct toc_node_t *toc_last;
	int current_toc_level;
} *cf;				/* cf == current_file */

regmatch_t lsp_no_match;

WINDOW *lsp_win;		/* Global window all the paging happens in. */

enum lsp_flag {
	LSP_FLAG_POPEN = 1,	/* We need to use pclose() when this file is done. */
	LSP_PRE_READ = 2	/* We read a single byte from a pipe that needs
				 * to be consumed. */
};

typedef enum lsp_flag lsp_flag_t;

enum lsp_ftype {
	LSP_FTYPE_OTHER = 0,
	LSP_FTYPE_MANPAGE = 1
};

typedef enum lsp_ftype lsp_ftype_t;

/*
 * We may use "current_file" instead of "cf" where verbosity
 * increases readability.
 */
#define current_file cf

/* getch_pos is used everywhere, make it even shorter */
#define lsp_pos (current_file->getch_pos)

/* Initial size of array for recording offsets of lines */
enum { LSP_LINES_INITIAL_SIZE = 1024 };

enum { LSP_FSIZE_UNKNOWN = (off_t)-1 };
#define LSP_EOF (cf->size != LSP_FSIZE_UNKNOWN && cf->size == cf->seek)

/* Define some keys that we use */
enum lsp_key_code {
	KEY_ESC = 0x1b,
	 CTRL_L = 0x0c
};

/* Max dimensions of current window */
int lsp_maxy;
int lsp_maxx;

/* Directions for searches */
enum { LSP_FW = 0, LSP_BW };

/* Regular expression to match references, e.g. lsp(1) */
char *lsp_search_ref_string = "[A-Za-z0-9\b.:_+-]+\\((n|[0-9])[^)]{0,8}\\)";
int lsp_search_direction;
char lsp_search_string[256];
char lsp_search_string_old[256];

/* These variables hold a compiled regular expression for searches. */
regex_t *lsp_search_regex;
regex_t *lsp_refs_regex;

/* Names for color pairs that we use. */
enum { LSP_DEFAULT_PAIR = 0, LSP_BOLD_PAIR, LSP_UL_PAIR,
	LSP_REVERSE_PAIR, LSP_FREE_PAIR };

/* Next free color pair number we can use for our own purposes. */
short lsp_next_pair;
short lsp_fg_color_default;
short lsp_bg_color_default;

/* Coords of cursor, if in use. */
int	lsp_cursor_y;
int	lsp_cursor_x;
bool	lsp_cursor_set;

/*
 * Command line arguments and switches
 */
bool	lsp_chop_lines;
bool	lsp_load_apropos;
char	*lsp_apropos_command;

/* Toggle case sensitivity of searches with -i */
bool	lsp_case_sensitivity;

/*
 * Switch to turn on case sensitivity for manual page names.
 * Used e.g. for validating references.
 */
char	lsp_man_case_sensitivity;
char	*lsp_logfile;
FILE	*lsp_logfp;

/* Output file to duplicate our input to. */
int	lsp_ofile;
bool	lsp_do_line_numbers = false;

/* Do colored output or not.
   Depends on terminal capabilities; --no-color also turns this off */
bool	lsp_color;

/* Command to execute to reload a manual page on resize. */
char	*lsp_reload_command;
/* Command used to verify references. */
char	*lsp_verify_command;
bool	lsp_verify_with_apropos;
bool	lsp_verify;

/*
 * Further global variables.
 */
/* xxx: probably not needed. */
bool	lsp_utf_8;

/* Temporary prompt to display in the center of the footer. */
char	*lsp_prompt;
int	lsp_tab_width;

/* Columns to horizontaly shift.
   key-left and key-right modify it.*/
unsigned char lsp_shift;

/* xxx put into lsp_init() or so */
char	*lsp_not_found = "Pattern not found";

/* String from LSP_OPEN or LESSOPEN environment variable. */
char *lsp_env_open;

/*
 * Number of entries for grefs hash table.
 *
 * Searching for references e.g. in source code can produce many grefs
 * (invalid ones)!
 */
size_t lsp_htable_entries;
/* We count grefs -- just to be able to lookup that number. */
size_t lsp_grefs_count;

/*
 * Counter for words and empty lines for repositioning after reload of manual
 * pages.
 *
 * For this we count the words inside the current section up to an empty line or
 * the section header.  If it wasn't the section header we then continue to
 * count empty lines until the section header.
 *
 * The motivation for counting empty lines is to leave out as many word counts
 * as possible because hyphenation makes this a more or less unreliable value.
 */
struct {
	size_t words;
	size_t elines;
} lsp_reposition;

#endif // _LSP_H_GUARD_
