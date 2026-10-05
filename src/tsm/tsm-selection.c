/*
 * libtsm - Screen Selections
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Screen Selections
 * If a running pty-client does not support mouse-tracking extensions, a
 * terminal can manually mark selected areas if it does mouse-tracking itself.
 * This tracking is slightly different than the integrated client-tracking:
 *
 * Initial state is no-selection. At any time selection_reset() can be called to
 * clear the selection and go back to initial state.
 * If the user presses a mouse-button, the terminal can calculate the selected
 * cell and call selection_start() to notify the terminal that the user started
 * the selection. While the mouse-button is held down, the terminal should call
 * selection_target() whenever a mouse-event occurs. This will tell the screen
 * layer to draw the selection from the initial start up to the last given
 * target.
 * Please note that the selection-start cannot be modified by the terminal
 * during a selection. Instead, the screen-layer automatically moves it along
 * with any scroll-operations or inserts/deletes. This also means, the terminal
 * must _not_ cache the start-position itself as it may change under the hood.
 * This selection takes also care of scrollback-buffer selections and correctly
 * moves selection state along.
 *
 * Please note that this is not the kind of selection that some PTY applications
 * support. If the client supports the mouse-protocol, then it can also control
 * a separate screen-selection which is always inside of the actual screen. This
 * is a totally different selection.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "libtsm.h"
#include "libtsm-int.h"
#include "shl-llog.h"
#include "shl_dlist.h"

#define LLOG_SUBSYSTEM "tsm-selection"

static void selection_set(struct tsm_screen *con, struct selection_pos *sel,
			  unsigned int x, unsigned int y)
{
	struct line *line;

	sel->x = x;

	if (!con->sb.pos) {
		sel->line = con->lines[y];
	} else if (con->sb.pos_num + y >= con->sb.count) {
		y -= con->sb.count - con->sb.pos_num;
		sel->line = con->lines[y];
	} else {
		line = con->sb.pos;
		while (y--)
			line = shl_dlist_next(line, &con->sb.list, struct line, list);
		sel->line = line;
	}
	/* A wide continuation belongs to the preceding glyph, including when it
	 * is the only selected cell or the start of a reversed selection. */
	while (sel->x > 0 && sel->x < sel->line->size &&
	       !sel->line->cells[sel->x].width)
		--sel->x;
}

static void word_select(struct tsm_screen *con,
			unsigned int posx,
			unsigned int posy)
{
	int start, end;
	struct line *line;

	selection_set(con, &con->sel_start, posx, posy);

	line = con->sel_start.line;

	if (!line || line->cells[posx].ch == ' ')
		return;

	for (start = posx; start >= 0; start--) {
		if (line->cells[start].ch == ' ') {
			start++;
			break;
		}
	}
	if (start < 0)
		start = 0;

	for (end = posx; end < line->size; end++) {
		if (line->cells[end].ch == ' ' || line->cells[end].ch == '\n' ||
		    line->cells[end].ch == '\0') {
			end--;
			break;
		}
	}
	con->sel_start.x = start;
	con->sel_end.x = end;
	con->sel_end.line = line;
	con->sel_active = true;
}

static void age_selection_visible(struct tsm_screen *con,
				  struct selection_pos *start,
				  struct selection_pos *end,
				  tsm_age_t age);

SHL_EXPORT
void tsm_screen_selection_reset(struct tsm_screen *con)
{
	struct selection_pos old_start, old_end;

	if (!con)
		return;

	old_start = con->sel_start;
	old_end = con->sel_end;
	screen_inc_age(con);
	age_selection_visible(con, &old_start, &old_end, con->age_cnt);

	con->sel_active = false;
	con->sel_start.line = NULL;
	con->sel_end.line = NULL;
}

/* calculates the line length from the beginning to the last non zero character */
static unsigned int calc_line_len(struct line *line)
{
	int i;

	for (i = line->size - 1; i >= 0; i--)
		if (line->cells[i].ch != 0)
			return i + 1;
	return 0;
}

static int copy_line(struct tsm_screen *con, struct line *line,
		     const struct selection_pos *first,
		     const struct selection_pos *last, char *buf, size_t capacity)
{
	unsigned int i, start, end;
	int pos = 0;
	unsigned int line_len = calc_line_len(line);
	start = first->line == line ? first->x : 0;
	end = last->line == line ? last->x + 1 : con->size_x;
	if (start > line_len)
		return 0;
	if (end > line_len)
		end = line_len;
	for (i = start; i < end; ++i) {
		tsm_symbol_t symbol = line->cells[i].ch;
		const uint32_t *codepoints;
		uint32_t space = ' ';
		size_t length = 1, j;
		if (!line->cells[i].width)
			continue;
		codepoints = symbol ? tsm_symbol_get(con->sym_table, &symbol, &length) : &space;
		for (j = 0; j < length; ++j) {
			char encoded[4];
			size_t bytes = tsm_ucs4_to_utf8(codepoints[j], encoded);
			if (bytes > (size_t)(INT_MAX - pos))
				return -EOVERFLOW;
			if (buf) memcpy(buf + pos, encoded, bytes);
			pos += (int)bytes;
		}
	}
	if (pos == INT_MAX)
		return -EOVERFLOW;
	if (buf && (size_t)pos < capacity) buf[pos] = '\n';
	return pos + 1;
}

/*
 * Returns true if a is before b in terminal order
 */
static bool selection_is_before(struct tsm_screen *con, struct selection_pos *a, struct selection_pos *b)
{
	int i;

	if (a->line == b->line)
		return (a->x < b->x);

	if (is_in_scrollback(a) != is_in_scrollback(b))
		return (is_in_scrollback(a));

	if (is_in_scrollback(a) && is_in_scrollback(b))
		return (a->line->sb_id < b->line->sb_id);

	/* so both are not in scroll back buffer and are not on the same line */
	for (i = 0; i < con->size_y; i++) {
		if (con->lines[i] == b->line)
			return false;

		if (con->lines[i] == a->line)
			return true;
	}
	// Should not happen
	return true;
}

/*
 * Selection highlighting is part of the rendering state, but it does not
 * change the cell contents. Age only the visible cells covered by a selection
 * so age-filtered renderers can repaint the old and new highlight ranges
 * without treating the complete screen as changed.
 */
static void age_selection_visible(struct tsm_screen *con,
				  struct selection_pos *start,
				  struct selection_pos *end,
				  tsm_age_t age)
{
	struct line *line, *next_line = NULL;
	unsigned int i, k = 0;

	if (!con || !start || !end || !start->line || !end->line)
		return;

	next_line = con->sb.pos;
	for (i = 0; i < con->size_y; ++i) {
		unsigned int from = 0, to = 0;
		struct selection_pos line_start, line_end;

		if (next_line) {
			line = next_line;
			next_line = shl_dlist_next(next_line, &con->sb.list,
							struct line, list);
		} else {
			line = con->lines[k++];
		}

		if (!line || !line->size)
			continue;

		if (line == start->line && line == end->line) {
			from = start->x < line->size ? start->x : line->size;
			to = end->x + 1 < line->size ? end->x + 1 : line->size;
		} else if (line == start->line) {
			from = start->x < line->size ? start->x : line->size;
			to = line->size;
		} else if (line == end->line) {
			from = 0;
			to = end->x + 1 < line->size ? end->x + 1 : line->size;
		} else {
			line_start.x = 0;
			line_start.line = line;
			line_end.x = line->size - 1;
			line_end.line = line;
			if (selection_is_before(con, start, &line_start) &&
			    selection_is_before(con, &line_end, end))
				to = line->size;
		}

		for ( ; from < to; ++from)
			line->cells[from].age = age;
	}
}

SHL_EXPORT
void tsm_screen_selection_start(struct tsm_screen *con,
				unsigned int posx,
				unsigned int posy)
{
	struct selection_pos old_start, old_end;

	if (!con || posx >= con->size_x || posy >= con->size_y)
		return;

	old_start = con->sel_start;
	old_end = con->sel_end;
	screen_inc_age(con);
	age_selection_visible(con, &old_start, &old_end, con->age_cnt);

	con->sel_active = true;
	selection_set(con, &con->sel_begin, posx, posy);
	con->sel_start = con->sel_begin;
	con->sel_end = con->sel_begin;
	age_selection_visible(con, &con->sel_start, &con->sel_end,
			      con->age_cnt);
}

SHL_EXPORT
void tsm_screen_selection_target(struct tsm_screen *con,
				 unsigned int posx,
				 unsigned int posy)
{
	struct selection_pos target;
	struct selection_pos old_start, old_end;

	if (!con || !con->sel_active || posx >= con->size_x || posy >= con->size_y)
		return;

	old_start = con->sel_start;
	old_end = con->sel_end;
	screen_inc_age(con);
	age_selection_visible(con, &old_start, &old_end, con->age_cnt);

	selection_set(con, &target, posx, posy);
	if (selection_is_before(con, &con->sel_begin, &target)) {
		con->sel_start = con->sel_begin;
		con->sel_end = target;
	} else {
		con->sel_start = target;
		con->sel_end = con->sel_begin;
	}
	age_selection_visible(con, &con->sel_start, &con->sel_end,
			      con->age_cnt);
}

SHL_EXPORT
void tsm_screen_selection_word(struct tsm_screen *con,
			       unsigned int posx,
			       unsigned int posy)
{
	struct selection_pos old_start, old_end;

	if (!con || posx >= con->size_x || posy >= con->size_y)
		return;

	old_start = con->sel_start;
	old_end = con->sel_end;
	screen_inc_age(con);
	age_selection_visible(con, &old_start, &old_end, con->age_cnt);

	word_select(con, posx, posy);
	if (con->sel_active)
		age_selection_visible(con, &con->sel_start, &con->sel_end,
				      con->age_cnt);
}

/*
 * Get the index of a line in the screen
 *
 * If the line is in the scroll back buffer, return 0
 * Otherwise, return the index of the line in the screen
 */
static unsigned int get_line_index(struct tsm_screen *con, struct line *line)
{
	unsigned int i = 0;

	if (line->sb_id)
		return 0;

	for (i = 0; i < con->size_y; i++) {
		if (con->lines[i] == line)
			return i;
	}
	return 0;
}

static struct line *get_next_line(struct tsm_screen *con, struct line *line, unsigned int *index)
{
	struct line *next;

	if (line->sb_id) {
		next = shl_dlist_next(line, &con->sb.list, struct line, list);
		if (next)
			return next;
		*index = 0;
		return con->lines[0];
	} else if (*index < con->size_y - 1) {
		(*index)++;
		return con->lines[*index];
	}
	return NULL;
}

static int selection_size(struct tsm_screen *con, struct selection_pos *start,
                      struct selection_pos *end)
{
	unsigned int index = get_line_index(con, start->line);
	struct line *iter = start->line;
	int pos = 0;
	while (iter) {
		int bytes = copy_line(con, iter, start, end, NULL, 0);
		if (bytes < 0 || bytes > INT_MAX - pos)
			return -EOVERFLOW;
		pos += bytes;
		if (iter == end->line)
			break;
		iter = get_next_line(con, iter, &index);
	}
	return pos;
}

SHL_EXPORT
int tsm_screen_selection_copy_into(struct tsm_screen *con, char *buffer,
                                  size_t capacity, size_t *written)
{
	struct selection_pos start, end;
	int bytes;
	if (!con || !written)
		return -EINVAL;
	*written = 0;
	if (!con->sel_active)
		return -ENOENT;
	start = con->sel_start;
	end = con->sel_end;
	if (!start.line && !end.line)
		return 0;
	if (!start.line) {
		start.line = !shl_dlist_empty(&con->sb.list) ?
			shl_dlist_first(&con->sb.list, struct line, list) : con->lines[0];
		start.x = 0;
	}
	bytes = selection_size(con, &start, &end);
	if (bytes < 0)
		return bytes;
	/* selection_size includes the final newline. The caller receives text only. */
	*written = bytes > 0 ? (size_t)bytes - 1 : 0;
	if (!buffer || capacity < *written)
		return *written ? 1 : 0;
	/* Keep the final newline out of caller storage, including exact-size buffers. */
	unsigned int index = get_line_index(con, start.line);
	struct line *iter = start.line;
	size_t pos = 0;
	while (iter) {
		int count = copy_line(con, iter, &start, &end, NULL, 0);
		if (count > 0) {
			/* Each line emits a newline; encode its text separately. */
			char *target = buffer + pos;
			int copied = copy_line(con, iter, &start, &end, target, *written - pos);
			pos += (size_t)copied;
		}
		if (iter == end.line) break;
		iter = get_next_line(con, iter, &index);
	}
	return 0;
}

SHL_EXPORT
int tsm_screen_selection_copy(struct tsm_screen *con, char **out)
{
	size_t length;
	int status;
	if (!out) return -EINVAL;
	*out = NULL;
	status = tsm_screen_selection_copy_into(con, NULL, 0, &length);
	if (status < 0) return status;
	char *text = malloc(length + 1);
	if (!text) return -ENOMEM;
	status = tsm_screen_selection_copy_into(con, text, length + 1, &length);
	if (status != 0) { free(text); return status < 0 ? status : -EOVERFLOW; }
	text[length] = 0;
	*out = text;
	return (int)length;
}
