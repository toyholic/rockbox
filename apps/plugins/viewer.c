/***************************************************************************
 *
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *
 * Copyright (C) 2002 Gilles Roux, 2003 Garrett Derner
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "plugin.h"
#include <ctype.h>

#if PLUGIN_API_VERSION < 3
#error Scrollbar function requires PLUGIN_API_VERSION 3 at least
#endif

#define WRAP_TRIM          44  /* Max number of spaces to trim (arbitrary) */
#define MAX_COLUMNS        64  /* Max displayable string len (over-estimate) */
#define MAX_WIDTH         910  /* Max line length in WIDE mode */
#define READ_PREV_ZONE    910  /* Arbitrary number less than SMALL_BLOCK_SIZE */
#define SMALL_BLOCK_SIZE  0x1000 /* 4k: Smallest file chunk we will read */
#define LARGE_BLOCK_SIZE  0x2000 /* 8k: Preferable size of file chunk to read */
#define BUFFER_SIZE       0x3000 /* 12k: Mem reserved for buffered file data */
#define TOP_SECTOR     buffer
#define MID_SECTOR     (buffer + SMALL_BLOCK_SIZE)
#define BOTTOM_SECTOR  (buffer + 2*(SMALL_BLOCK_SIZE))

/* Out-Of-Bounds test for any pointer to data in the buffer */
#define BUFFER_OOB(p)    ((p) < buffer || (p) >= buffer_end)

/* Does the buffer contain the beginning of the file? */
#define BUFFER_BOF()     (file_pos==0)

/* Does the buffer contain the end of the file? */
#define BUFFER_EOF()     (file_size-file_pos <= BUFFER_SIZE)

/* Formula for the endpoint address outside of buffer data */
#define BUFFER_END() \
 ((BUFFER_EOF()) ? (file_size-file_pos+buffer) : (buffer+BUFFER_SIZE))

/* Is the entire file being shown in one screen? */
#define ONE_SCREEN_FITS_ALL() \
 (next_screen_ptr==NULL && screen_top_ptr==buffer && BUFFER_BOF())

/* Is a scrollbar called for on the current screen? */
#define NEED_SCROLLBAR() ((!(ONE_SCREEN_FITS_ALL())) && \
 (view_mode==WIDE? scrollbar_mode[WIDE]==SB_ON: scrollbar_mode[NARROW]==SB_ON))

enum {
    WRAP=0,
    CHOP,
    WORD_MODES
} word_mode = 0;
static unsigned char *word_mode_str[] = {"wrap", "chop", "words"};

enum {
    NORMAL=0,
    JOIN,
    EXPAND,
    LINE_MODES
} line_mode = 0;
static unsigned char *line_mode_str[] = {"normal", "join", "expand", "lines"};

enum {
    NARROW=0,
    WIDE,
    VIEW_MODES
} view_mode = 0;
static unsigned char *view_mode_str[] = {"narrow", "wide", "view"};

#ifdef HAVE_LCD_BITMAP
enum {
    SB_OFF=0,
    SB_ON,
    SCROLLBAR_MODES
} scrollbar_mode[VIEW_MODES] = {SB_OFF, SB_ON};
static unsigned char *scrollbar_mode_str[] = {"off", "on", "scrollbar"};
static bool need_scrollbar;
enum {
    NO_OVERLAP=0,
    OVERLAP,
    PAGE_MODES
} page_mode = 0;
static unsigned char *page_mode_str[] = {"don't overlap", "overlap", "pages"};
#endif

static unsigned char buffer[BUFFER_SIZE + 1];
static unsigned char line_break[] = {0,0x20,'-',9,0xB,0xC};
static int display_columns; /* number of columns on the display */
static int display_lines; /* number of lines on the display */
static int fd;
static long file_size;
static bool mac_text;
static long file_pos; /* Position of the top of the buffer in the file */
static unsigned char *buffer_end; /*Set to BUFFER_END() when file_pos changes*/
static int max_line_len;
static unsigned char *screen_top_ptr;
static unsigned char *next_screen_ptr;
static unsigned char *next_screen_to_draw_ptr;
static unsigned char *next_line_ptr;
static struct plugin_api* rb;

static unsigned char* find_first_feed(const unsigned char* p, int size)
{
    int i;

    for (i=0; i < size; i++)
        if (p[i] == 0)
            return (unsigned char*) p+i;

    return NULL;
}

static unsigned char* find_last_feed(const unsigned char* p, int size)
{
    int i;

    for (i=size-1; i>=0; i--)
        if (p[i] == 0)
            return (unsigned char*) p+i;

    return NULL;
}

static unsigned char* find_last_space(const unsigned char* p, int size)
{
    int i, j, k;

    k = line_mode==JOIN? 0:1;

    for (i=size-1; i>=0; i--)
        for (j=k; j < (int) sizeof(line_break); j++)
            if (p[i] == line_break[j])
                return (unsigned char*) p+i;

    return NULL;
}

static unsigned char* find_next_line(const unsigned char* cur_line)
{
    const unsigned char *next_line = NULL;
    int size, i, j, k, chop_len, search_len, spaces, newlines, draw_columns;
    unsigned char c;

    if BUFFER_OOB(cur_line)
        return NULL;

#ifdef HAVE_LCD_BITMAP
    draw_columns = need_scrollbar? display_columns-1: display_columns;
#else
    draw_columns = display_columns;
#endif

    if (view_mode == WIDE) {
        search_len = chop_len = MAX_WIDTH;
    }
    else {   /* view_mode == NARROW */
        chop_len = draw_columns;
        search_len =  chop_len + 1;
    }

    size = BUFFER_OOB(cur_line+search_len) ? buffer_end-cur_line : search_len;

    if (line_mode == JOIN) {
        /* Need to scan ahead and possibly increase search_len and size,
         or possibly set next_line at second hard return in a row. */
        next_line = NULL;
        for (j=k=spaces=newlines=0; j < size; j++) {
            if (k == MAX_COLUMNS)
                break;

            c = cur_line[j];
            switch (c) {
                case ' ':
                    spaces++;
                    break;

                case 0:
                    if (newlines > 0) {
                        size = j;
                        next_line = cur_line + size - spaces - 1;
                        if (next_line != cur_line)
                            return (unsigned char*) next_line;
                        break;
                    }
                    newlines++;
                    size += spaces;
                    if (BUFFER_OOB(cur_line+size) || size > 2*search_len)
                        return NULL;

                    search_len = size;
                    spaces = 0;
                    k++;
                    break;

                default:
                    newlines = 0;
                    while (spaces) {
                        spaces--;
                        k++;
                        if (k == MAX_COLUMNS - 1)
                            break;
                    }
                    k++;
                    break;
            }
        }
    }
    else {
        /* find first hard return */
        next_line = find_first_feed(cur_line, size);
    }

    if (next_line == NULL)
        if (size == search_len) {
            if (word_mode == WRAP)  /* Find last space */
                next_line = find_last_space(cur_line, size);

            if (next_line == NULL)
                next_line = cur_line + chop_len;
            else
                if (word_mode == WRAP)
                    for (i=0;
                    i<WRAP_TRIM && isspace(next_line[0]) && !BUFFER_OOB(next_line);
                    i++)
                        next_line++;
        }

    if (line_mode == EXPAND)
        if (!BUFFER_OOB(next_line))  /* Not Null & not out of bounds */
            if (next_line[0] == 0)
                if (next_line != cur_line)
                    return (unsigned char*) next_line;

    /* If next_line is pointing to a zero, increment it; i.e.,
     leave the terminator at the end of cur_line. If pointing
     to a hyphen, increment only if there is room to display
     the hyphen on current line (won't apply in WIDE mode,
     since it's guarenteed there won't be room). */
    if (!BUFFER_OOB(next_line))  /* Not Null & not out of bounds */
        if (next_line[0] == 0 ||
        (next_line[0] == '-' && next_line-cur_line < draw_columns))
            next_line++;

    if (BUFFER_OOB(next_line))
        return NULL;

    return (unsigned char*) next_line;
}

static unsigned char* find_prev_line(const unsigned char* cur_line)
{
    const unsigned char *prev_line = NULL;
    const unsigned char *p;

    if BUFFER_OOB(cur_line)
        return NULL;

    /* To wrap consistently at the same places, we must
     start with a known hard return, then work downwards.
     We can either search backwards for a hard return,
     or simply start wrapping downwards from top of buffer.
       If current line is not near top of buffer, this is
     a file with long lines (paragraphs). We would need to
     read earlier sectors before we could decide how to
     properly wrap the lines above the current line, but
     it probably is not worth the disk access. Instead,
     start with top of buffer and wrap down from there.
     This may result in some lines wrapping at different
     points from where they wrap when scrolling down.
       If buffer is at top of file, start at top of buffer. */

    if (line_mode == JOIN)
        prev_line = p = NULL;
    else
        prev_line = p = find_last_feed(buffer, cur_line-buffer-1);
        /* Null means no line feeds in buffer above current line. */

    if (prev_line == NULL)
        if (BUFFER_BOF() || cur_line - buffer > READ_PREV_ZONE)
            prev_line = p = buffer;
        /* (else return NULL and read previous block) */

    /* Wrap downwards until too far, then use the one before. */
    while (p < cur_line && p != NULL) {
        prev_line = p;
        p = find_next_line(prev_line);
    }

    if (BUFFER_OOB(prev_line))
        return NULL;

    return (unsigned char*) prev_line;
}

static void fill_buffer(long pos, unsigned char* buf, unsigned size)
{
    /* Read from file and preprocess the data */
    /* To minimize disk access, always read on sector boundaries */
    unsigned numread, i;
    bool found_CR = false;

    rb->lseek(fd, pos, SEEK_SET);
    numread = rb->read(fd, buf, size);
    while (rb->button_get(false)); /* clear button queue */

    for(i = 0; i < numread; i++) {
        switch(buf[i]) {
            case '\r':
                if (mac_text) {
                    buf[i] = 0;
                }
                else {
                    buf[i] = ' ';
                    found_CR = true;
                }
                break;

            case '\n':
                buf[i] = 0;
                found_CR = false;
                break;

            case 0:  /* No break between case 0 and default, intentionally */
                buf[i] = ' ';
            default:
                if (found_CR) {
                    buf[i - 1] = 0;
                    found_CR = false;
                    mac_text = true;
                }
                break;
        }
    }
}

static int read_and_synch(int direction)
{
/* Read next (or prev) block, and reposition global pointers. */
/* direction: 1 for down (i.e., further into file), -1 for up */
    int move_size, move_vector, offset;
    unsigned char *fill_buf;
    
    if (direction == -1) /* up */ {
        move_size = SMALL_BLOCK_SIZE;
        offset = 0;
        fill_buf = TOP_SECTOR;
        rb->memcpy(BOTTOM_SECTOR, MID_SECTOR, SMALL_BLOCK_SIZE);
        rb->memcpy(MID_SECTOR, TOP_SECTOR, SMALL_BLOCK_SIZE);
    }
    else /* down */ {
        if (view_mode == WIDE) {
            /* WIDE mode needs more buffer so we have to read smaller blocks */
            move_size = SMALL_BLOCK_SIZE;
            offset = LARGE_BLOCK_SIZE;
            fill_buf = BOTTOM_SECTOR;
            rb->memcpy(TOP_SECTOR, MID_SECTOR, SMALL_BLOCK_SIZE);
            rb->memcpy(MID_SECTOR, BOTTOM_SECTOR, SMALL_BLOCK_SIZE);
        }
        else {
            move_size = LARGE_BLOCK_SIZE;
            offset = SMALL_BLOCK_SIZE;
            fill_buf = MID_SECTOR;
            rb->memcpy(TOP_SECTOR, BOTTOM_SECTOR, SMALL_BLOCK_SIZE);
        }
    }
    move_vector = direction * move_size;
    screen_top_ptr -= move_vector;
    file_pos += move_vector;
    buffer_end = BUFFER_END();  /* Update whenever file_pos changes */
    fill_buffer(file_pos + offset, fill_buf, move_size);
    return move_vector;
}

static void viewer_scroll_up(void)
{
    unsigned char *p;

    p = find_prev_line(screen_top_ptr);
    if (p == NULL && !BUFFER_BOF()) {
        read_and_synch(-1);
        p = find_prev_line(screen_top_ptr);
    }
    if (p != NULL)
        screen_top_ptr = p;
}

#ifdef HAVE_LCD_BITMAP
static void viewer_scrollbar(void) {
    int w, h, items, min_shown, max_shown;

    rb->lcd_getstringsize("o", &w, &h);
    items = (int) file_size;  /* (SH1 int is same as long) */
    min_shown = (int) file_pos + (screen_top_ptr - buffer);

    if (next_screen_ptr == NULL)
        max_shown = items;
    else
        max_shown = min_shown + (next_screen_ptr - screen_top_ptr);

    rb->scrollbar(0, 0, w-1, LCD_HEIGHT, items, min_shown, max_shown, VERTICAL);
}
#endif

static void viewer_draw(int col)
{
    int i, j, k, line_len, resynch_move, spaces, left_col=0;
    unsigned char *line_begin;
    unsigned char *line_end;
    unsigned char c;
    unsigned char scratch_buffer[MAX_COLUMNS + 1];

    /* If col==-1 do all calculations but don't display */
    if (col != -1) {
#ifdef HAVE_LCD_BITMAP
        left_col = need_scrollbar? 1:0;
#else
        left_col = 0;
#endif
        rb->lcd_clear_display();
    }
    max_line_len = 0;
    line_begin = line_end = screen_top_ptr;

    for (i = 0; i < display_lines; i++) {
        if (BUFFER_OOB(line_end))
            break;  /* Happens after display last line at BUFFER_EOF() */

        line_begin = line_end;
        line_end = find_next_line(line_begin);

        if (line_end == NULL) {
            if (BUFFER_EOF()) {
                if (i < display_lines - 1 && !BUFFER_BOF()) {
                    if (col != -1)
                        rb->lcd_clear_display();

                    for (; i < display_lines - 1; i++)
                        viewer_scroll_up();

                    line_begin = line_end = screen_top_ptr;
                    i = -1;
                    continue;
                }
                else {
                    line_end = buffer_end;
                }
            }
            else {
                resynch_move = read_and_synch(1); /* Read block & move ptrs */
                line_begin -= resynch_move;
                if (i > 0)
                    next_line_ptr -= resynch_move;

                line_end = find_next_line(line_begin);
                if (line_end == NULL)  /* Should not really happen */
                    break;
            }
        }
        line_len = line_end - line_begin;

        if (line_mode == JOIN) {
            if (line_begin[0] == 0) {
                line_begin++;
                if (word_mode == CHOP)
                    line_end++;
            }
            for (j=k=spaces=0; j < line_len; j++) {
                if (k == MAX_COLUMNS)
                    break;

                c = line_begin[j];
                switch (c) {
                    case ' ':
                        spaces++;
                        break;
                    case 0:
                        spaces = 0;
                        scratch_buffer[k++] = ' ';
                        break;
                    default:
                        while (spaces) {
                            spaces--;
                            scratch_buffer[k++] = ' ';
                            if (k == MAX_COLUMNS - 1)
                                break;
                        }
                        scratch_buffer[k++] = c;
                        break;
                }
            }
            if (col != -1)
                if (k > col) {
                    scratch_buffer[k] = 0;
                    rb->lcd_puts(left_col, i, scratch_buffer + col);
                }
        }
        else {
            if (col != -1)
                if (line_len > col) {
                    c = line_end[0];
                    line_end[0] = 0;
                    rb->lcd_puts(left_col, i, line_begin + col);
                    line_end[0] = c;
                }
        }
        if (line_len > max_line_len)
            max_line_len = line_len;

        if (i == 0)
            next_line_ptr = line_end;
    }
    next_screen_ptr = line_end;
    if (BUFFER_OOB(next_screen_ptr))
        next_screen_ptr = NULL;

#ifdef HAVE_LCD_BITMAP
    next_screen_to_draw_ptr = page_mode==OVERLAP? line_begin: next_screen_ptr;

    if (need_scrollbar)
        viewer_scrollbar();

    if (col != -1)
        rb->lcd_update();
#else
    next_screen_to_draw_ptr = next_screen_ptr;
#endif
}

static void viewer_top(void)
{
    /* Read top of file into buffer
      and point screen pointer to top */
    file_pos = 0;
    buffer_end = BUFFER_END();  /* Update whenever file_pos changes */
    screen_top_ptr = buffer;
    fill_buffer(0, buffer, BUFFER_SIZE);
}

static void viewer_bottom(void)
{
    /* Read bottom of file into buffer
      and point screen pointer to bottom */
    long last_sectors;

    if (file_size > BUFFER_SIZE) {
        /* Find last buffer in file, round up to next sector boundary */
        last_sectors = file_size - BUFFER_SIZE + SMALL_BLOCK_SIZE;
        last_sectors /= SMALL_BLOCK_SIZE;
        last_sectors *= SMALL_BLOCK_SIZE;
    }
    else {
        last_sectors = 0;
    }
    file_pos = last_sectors;
    buffer_end = BUFFER_END();  /* Update whenever file_pos changes */
    screen_top_ptr = buffer_end-1;
    fill_buffer(last_sectors, buffer, BUFFER_SIZE);
}

#ifdef HAVE_LCD_BITMAP
static void init_need_scrollbar(void) {
    /* Call viewer_draw in quiet mode to initialize next_screen_ptr,
     and thus ONE_SCREEN_FITS_ALL(), and thus NEED_SCROLLBAR() */
    viewer_draw(-1);
    need_scrollbar = NEED_SCROLLBAR();
}
#endif

static bool viewer_init(char* file)
{
#ifdef HAVE_LCD_BITMAP
    int w,h;

    rb->lcd_getstringsize("o", &w, &h);
    display_lines = LCD_HEIGHT / h;
    display_columns = LCD_WIDTH / w;
#else
    display_lines = 2;
    display_columns = 11;
#endif
    /*********************
    * (Could re-initialize settings here, if you
    *   wanted viewer to start the same way every time)
    word_mode = WRAP;
    line_mode = NORMAL;
    view_mode = NARROW;
#ifdef HAVE_LCD_BITMAP
    page_mode = NO_OVERLAP;
    scrollbar_mode[NARROW] = SB_OFF;
    scrollbar_mode[WIDE] = SB_ON;
#endif
    **********************/

    fd = rb->open(file, O_RDONLY);
    if (fd==-1)
        return false;

    file_size = rb->filesize(fd);
    if (file_size==-1)
        return false;

    /* Init mac_text value used in processing buffer */
    mac_text = false;

    /* Read top of file into buffer;
      init file_pos, buffer_end, screen_top_ptr */
    viewer_top();

#ifdef HAVE_LCD_BITMAP
    /* Init need_scrollbar value */
    init_need_scrollbar();
#endif

    return true;
}

static void viewer_exit(void)
{
    rb->close(fd);
}

static int col_limit(int col)
{
    if (col < 0)
        col = 0;
    else
        if (col > max_line_len - 2)
            col = max_line_len - 2;

    return col;
}

#ifdef HAVE_LCD_BITMAP
static int viewer_recorder_on_button(int col)
{
    bool exit = false;

    while (!exit) {
        switch (rb->button_get(true)) {
            case BUTTON_ON | BUTTON_F1:
                /* Page-overlap mode */
                if (++page_mode == PAGE_MODES)
                    page_mode = 0;

                rb->splash(HZ, true, "%s %s",
                           page_mode_str[page_mode],
                           page_mode_str[PAGE_MODES]);

                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_F3:
                /* Show-scrollbar mode for current view-width mode */
                if (!(ONE_SCREEN_FITS_ALL())) {
                    if (++scrollbar_mode[view_mode] == SCROLLBAR_MODES)
                        scrollbar_mode[view_mode] = 0;

                    init_need_scrollbar();
                    viewer_draw(col);

                    rb->splash(HZ, true, "%s %s (%s %s)",
                               scrollbar_mode_str[SCROLLBAR_MODES],
                               scrollbar_mode_str[scrollbar_mode[view_mode]],
                               view_mode_str[view_mode],
                               view_mode_str[VIEW_MODES]);
                }
                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_UP:
            case BUTTON_ON | BUTTON_UP | BUTTON_REPEAT:
                /* Scroll up one line */
                viewer_scroll_up();
                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_DOWN:
            case BUTTON_ON | BUTTON_DOWN | BUTTON_REPEAT:
                /* Scroll down one line */
                if (next_screen_ptr != NULL)
                    screen_top_ptr = next_line_ptr;

                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_LEFT:
            case BUTTON_ON | BUTTON_LEFT | BUTTON_REPEAT:
                /* Scroll left one column */
                col--;
                col = col_limit(col);
                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_RIGHT:
            case BUTTON_ON | BUTTON_RIGHT | BUTTON_REPEAT:
                /* Scroll right one column */
                col++;
                col = col_limit(col);
                viewer_draw(col);
                break;

            case BUTTON_ON | BUTTON_REL:
            case BUTTON_ON | BUTTON_DOWN | BUTTON_REL:
            case BUTTON_ON | BUTTON_UP | BUTTON_REL:
                /* Drop out of this loop (when ON btn released) */
                exit = true;
                break;
        }
    }
    return col;
}
#endif

enum plugin_status plugin_start(struct plugin_api* api, void* file)
{
    bool exit=false;
    int col = 0;
    int i;
    int ok;

    TEST_PLUGIN_API(api);
    rb = api;

    if (!file)
        return PLUGIN_ERROR;

    ok = viewer_init(file);
    if (!ok) {
        rb->splash(HZ, false, "Error");
        viewer_exit();
        return PLUGIN_OK;
    }

    viewer_draw(col);

    while (!exit) {
        switch (rb->button_get(true)) {
#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_OFF:
#else
            case BUTTON_STOP:
#endif
                viewer_exit();
                exit = true;
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_F1:
#else
            case BUTTON_ON | BUTTON_LEFT:
#endif
                /* Word-wrap mode: WRAP or CHOP */
                if (++word_mode == WORD_MODES)
                    word_mode = 0;

#ifdef HAVE_LCD_BITMAP
                init_need_scrollbar();
#endif
                viewer_draw(col);

                rb->splash(HZ, true, "%s %s",
                           word_mode_str[word_mode],
                           word_mode_str[WORD_MODES]);

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_F2:
#else
            case BUTTON_ON | BUTTON_MENU | BUTTON_RIGHT:
#endif
                /* Line-paragraph mode: NORMAL, JOIN or EXPAND */
                if (++line_mode == LINE_MODES)
                    line_mode = 0;

                if (view_mode == WIDE)
                    if (line_mode == JOIN)
                        if (++line_mode == LINE_MODES)
                            line_mode = 0;

#ifdef HAVE_LCD_BITMAP
                init_need_scrollbar();
#endif
                viewer_draw(col);

                rb->splash(HZ, true, "%s %s",
                           line_mode_str[line_mode],
                           line_mode_str[LINE_MODES]);

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_F3:
#else
            case BUTTON_ON | BUTTON_RIGHT:
#endif
                /* View-width mode: NARROW or WIDE */
                if (line_mode == JOIN)
                    rb->splash(HZ, true, "(no %s %s)",
                               view_mode_str[WIDE],
                               line_mode_str[JOIN]);
                else
                    if (++view_mode == VIEW_MODES)
                        view_mode = 0;

                col = 0;

                /***** Could do this after change of word-wrap mode
                * and after change of view-width mode, to normalize
                * view:
                if (screen_top_ptr > buffer + BUFFER_SIZE/2) {
                    screen_top_ptr = find_prev_line(screen_top_ptr);
                    screen_top_ptr = find_next_line(screen_top_ptr);
                }
                else {
                    screen_top_ptr = find_next_line(screen_top_ptr);
                    screen_top_ptr = find_prev_line(screen_top_ptr);
                }
                ***********/

#ifdef HAVE_LCD_BITMAP
                init_need_scrollbar();
#endif
                viewer_draw(col);

                rb->splash(HZ, true, "%s %s",
                           view_mode_str[view_mode],
                           view_mode_str[VIEW_MODES]);

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_UP:
            case BUTTON_UP | BUTTON_REPEAT:
#else
            case BUTTON_LEFT:
            case BUTTON_LEFT | BUTTON_REPEAT:
#endif
                /* Page up */
#ifdef HAVE_LCD_BITMAP
                for (i = page_mode==OVERLAP? 1:0; i < display_lines; i++)
#else
                for (i = 0; i < display_lines; i++)
#endif
                    viewer_scroll_up();

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_DOWN:
            case BUTTON_DOWN | BUTTON_REPEAT:
#else
            case BUTTON_RIGHT:
            case BUTTON_RIGHT | BUTTON_REPEAT:
#endif
                /* Page down */
                if (next_screen_ptr != NULL)
                    screen_top_ptr = next_screen_to_draw_ptr;

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_LEFT:
            case BUTTON_LEFT | BUTTON_REPEAT:
#else
            case BUTTON_MENU | BUTTON_LEFT:
            case BUTTON_MENU | BUTTON_LEFT | BUTTON_REPEAT:
#endif
                if (view_mode == WIDE) {
                    /* Screen left */
                    col -= display_columns;
                    col = col_limit(col);
                }
                else {   /* view_mode == NARROW */
                    /* Top of file */
                    viewer_top();
                }

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_RIGHT:
            case BUTTON_RIGHT | BUTTON_REPEAT:
#else
            case BUTTON_MENU | BUTTON_RIGHT:
            case BUTTON_MENU | BUTTON_RIGHT | BUTTON_REPEAT:
#endif
                if (view_mode == WIDE) {
                    /* Screen right */
                    col += display_columns;
                    col = col_limit(col);
                }
                else {   /* view_mode == NARROW */
                    /* Bottom of file */
                    viewer_bottom();
                }

                viewer_draw(col);
                break;

#if CONFIG_KEYPAD == RECORDER_PAD
            case BUTTON_ON:
                /*Go to On-btn combinations */
                col = viewer_recorder_on_button(col);
                break;
#endif

            case SYS_USB_CONNECTED:
                /* Release control to USB functions */
                rb->usb_screen();
                viewer_exit();
                return PLUGIN_USB_CONNECTED;
        }
    }
    return PLUGIN_OK;
}
