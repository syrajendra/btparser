/*
    backtrace.c

    Copyright (C) 2010  Red Hat, Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "backtrace.h"
#include "thread.h"
#include "frame.h"
#include "sharedlib.h"
#include "utils.h"
#include "strbuf.h"
#include "location.h"
#include "normalize.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

struct btp_backtrace *
btp_backtrace_new()
{
    struct btp_backtrace *backtrace = btp_malloc(sizeof(struct btp_backtrace));
    btp_backtrace_init(backtrace);
    return backtrace;
}

void
btp_backtrace_init(struct btp_backtrace *backtrace)
{
    backtrace->threads = NULL;
    backtrace->crash = NULL;
    backtrace->libs = NULL;
}

void
btp_backtrace_free(struct btp_backtrace *backtrace)
{
    if (!backtrace)
        return;

    while (backtrace->threads)
    {
        struct btp_thread *rm = backtrace->threads;
        backtrace->threads = rm->next;
        btp_thread_free(rm);
    }

    while (backtrace->libs)
    {
        struct btp_sharedlib *rm = backtrace->libs;
        backtrace->libs = rm->next;
        btp_sharedlib_free(rm);
    }

    if (backtrace->crash)
        btp_frame_free(backtrace->crash);

    free(backtrace);
}

struct btp_backtrace *
btp_backtrace_dup(struct btp_backtrace *backtrace)
{
    struct btp_backtrace *result = btp_backtrace_new();
    memcpy(result, backtrace, sizeof(struct btp_backtrace));

    if (backtrace->crash)
        result->crash = btp_frame_dup(backtrace->crash, false);
    if (backtrace->threads)
        result->threads = btp_thread_dup(backtrace->threads, true);
    if (backtrace->libs)
        result->libs = btp_sharedlib_dup(backtrace->libs, true);

    return result;
}

int
btp_backtrace_get_thread_count(struct btp_backtrace *backtrace)
{
    struct btp_thread *thread = backtrace->threads;
    int count = 0;
    while (thread)
    {
        thread = thread->next;
        ++count;
    }
    return count;
}

void
btp_backtrace_remove_threads_except_one(struct btp_backtrace *backtrace,
					struct btp_thread *thread)
{
    while (backtrace->threads)
    {
        struct btp_thread *delete_thread = backtrace->threads;
        backtrace->threads = delete_thread->next;
        if (delete_thread != thread)
            btp_thread_free(delete_thread);
    }

    thread->next = NULL;
    backtrace->threads = thread;
}

/* we do not care about thread numbers and btp_thread_cmp does not allow us to
 * compare them */
static int
thread_cmp_dont_compare_number(struct btp_thread *t1, struct btp_thread *t2)
{
    struct btp_frame *f1 = t1->frames, *f2 = t2->frames;
    do {
        if (f1 && !f2)
            return 1;
        else if (f2 && !f1)
            return -1;
        else if (f1 && f2)
        {
            int frames = btp_frame_cmp(f1, f2, true);
            if (frames != 0)
                return frames;
            f1 = f1->next;
            f2 = f2->next;
        }
    } while (f1 || f2);

    return 0;
}

enum requirement { ONE_MATCHING, ABORT_FUNCTION, FIRST_MATCHING };
/**
 * Loop through all threads and if a single one contains the crash
 * frame on the top, return it. Otherwise, return NULL.
 *
 * Parameter req controlls the criteria for matching thread:
 * ONE_MATCHING   - There is only one thread that contains the crash frame.
 * ABORT_FUNCTION - There may be multiple threads containing the crash frame
 *                  but only one contains some known "abort" function.
 * FIRST_MATCHING - If everything else fails, we sort the threads (in order to
 *                  get deterministic results) and return the first thread
 *                  containing the crash frame.
 */
static struct btp_thread *
btp_backtrace_find_crash_thread_from_crash_frame(struct btp_backtrace *backtrace,
                                                 enum requirement req)
{
    if (btp_debug_parser)
        printf("%s(backtrace, %s)\n", __FUNCTION__,
               (req == ABORT_FUNCTION) ? "true" : "false");

    assert(backtrace->threads); /* checked by the caller */
    if (!backtrace->crash || !backtrace->crash->function_name)
        return NULL;

    struct btp_thread *result = NULL;
    struct btp_thread *thread = backtrace->threads;
    while (thread)
    {
        struct btp_frame *top_frame = thread->frames;
        bool same_name = top_frame &&
            top_frame->function_name &&
            0 == strcmp(top_frame->function_name, backtrace->crash->function_name);
        bool abort_requirement_satisfied = (req != ABORT_FUNCTION) ||
            btp_glibc_thread_find_exit_frame(thread);
        if (btp_debug_parser)
        {
            printf(" - thread #%d: same_name %s, abort_satisfied %s\n",
                   thread->number,
                   same_name ? "true" : "false",
                   abort_requirement_satisfied ? "true" : "false");
        }

        if (same_name && abort_requirement_satisfied)
        {
            if (NULL == result)
            {
                result = thread;
            }
            else if (req != FIRST_MATCHING)
            {
                /* Second frame with the same function. Failure. */
                return NULL;
            }
            else if (thread_cmp_dont_compare_number(thread, result) < 0)
            {
                /* We're just looking for the first thread that matches. */
                result = thread;
            }
        }

        thread = thread->next;
    }

    return result;
}

struct btp_thread *
btp_backtrace_find_crash_thread(struct btp_backtrace *backtrace)
{
    /* If there is no thread, be silent and report NULL. */
    if (!backtrace->threads)
        return NULL;

    /* If there is just one thread, it is simple. */
    if (!backtrace->threads->next)
        return backtrace->threads;

    /* If we have a crash frame *and* there is just one thread which has
     * this frame on the top, it is also simple.
     */
    struct btp_thread *thread;
    thread = btp_backtrace_find_crash_thread_from_crash_frame(backtrace, ONE_MATCHING);
    if (thread)
        return thread;

    /* There are multiple threads with a frame indistinguishable from
     * the crash frame on the top of stack.
     * Try to search for known abort functions.
     */
    thread = btp_backtrace_find_crash_thread_from_crash_frame(backtrace, ABORT_FUNCTION);
    if (thread)
        return thread;

    /* There are multiple threads with the same crash frame and none of them
     * contains known abort function.
     * Take a guess and return the first one (from sorted threads).
     */
    thread = btp_backtrace_find_crash_thread_from_crash_frame(backtrace, FIRST_MATCHING);

    /* We might want to search a thread with known abort function, and
     * without the crash frame here. However, it hasn't been needed so
     * far.
     */
    return thread; /* result or null */
}


void
btp_backtrace_limit_frame_depth(struct btp_backtrace *backtrace,
				int depth)
{
    assert(depth > 0);
    struct btp_thread *thread = backtrace->threads;
    while (thread)
    {
        btp_thread_remove_frames_below_n(thread, depth);
        thread = thread->next;
    }
}

float
btp_backtrace_quality_simple(struct btp_backtrace *backtrace)
{
    int ok_count = 0, all_count = 0;
    struct btp_thread *thread = backtrace->threads;
    while (thread)
    {
        btp_thread_quality_counts(thread, &ok_count, &all_count);
        thread = thread->next;
    }

    if (all_count == 0)
        return 0;

    return ok_count / (float)all_count;
}

float
btp_backtrace_quality_complex(struct btp_backtrace *backtrace)
{
    backtrace = btp_backtrace_dup(backtrace);

    /* Find the crash thread, and then normalize the backtrace. It is
     * not possible to find the crash thread after the backtrace has
     * been normalized.
     */
    struct btp_thread *crash_thread = btp_backtrace_find_crash_thread(backtrace);
    btp_normalize_backtrace(backtrace);

    /* Get the quality q1 of the full backtrace. */
    float q1 = btp_backtrace_quality_simple(backtrace);

    if (!crash_thread)
    {
        btp_backtrace_free(backtrace);
        return q1;
    }

    /* Get the quality q2 of the crash thread. */
    float q2 = btp_thread_quality(crash_thread);

    /* Get the quality q3 of the frames around the crash.  First,
     * duplicate the crash thread so we can cut it. Then find an exit
     * frame, and remove it and everything above it
     * (__run_exit_handlers and such). Then remove all the redundant
     * frames (assert calls etc.) Then limit the frame count to 5.
     */
    btp_thread_remove_frames_below_n(crash_thread, 5);
    float q3 = btp_thread_quality(crash_thread);

    btp_backtrace_free(backtrace);

    /* Compute and return the final backtrace quality q. */
    return 0.25f * q1 + 0.35f * q2 + 0.4f * q3;
}

char *
btp_backtrace_to_text(struct btp_backtrace *backtrace, bool verbose)
{
    struct btp_strbuf *str = btp_strbuf_new();
    if (verbose)
    {
        btp_strbuf_append_strf(str, "Thread count: %d\n",
                               btp_backtrace_get_thread_count(backtrace));
    }

    if (backtrace->crash && verbose)
    {
        btp_strbuf_append_str(str, "Crash frame: ");
        btp_frame_append_to_str(backtrace->crash, str, verbose);
    }

    struct btp_thread *thread = backtrace->threads;
    while (thread)
    {
        btp_thread_append_to_str(thread, str, verbose);
        thread = thread->next;
    }

    return btp_strbuf_free_nobuf(str);
}

struct btp_frame *
btp_backtrace_get_crash_frame(struct btp_backtrace *backtrace)
{
    backtrace = btp_backtrace_dup(backtrace);

    struct btp_thread *crash_thread = btp_backtrace_find_crash_thread(backtrace);
    if (!crash_thread)
    {
        btp_backtrace_free(backtrace);
        return NULL;
    }

    btp_normalize_backtrace(backtrace);
    struct btp_frame *crash_frame = crash_thread->frames;
    crash_frame = btp_frame_dup(crash_frame, false);
    btp_backtrace_free(backtrace);
    return crash_frame;
}

char *
btp_backtrace_get_duplication_hash(struct btp_backtrace *backtrace)
{
    backtrace = btp_backtrace_dup(backtrace);
    struct btp_thread *crash_thread = btp_backtrace_find_crash_thread(backtrace);
    if (crash_thread)
        btp_backtrace_remove_threads_except_one(backtrace, crash_thread);

    btp_normalize_backtrace(backtrace);
    btp_backtrace_limit_frame_depth(backtrace, 3);
    char *hash = btp_backtrace_to_text(backtrace, false);
    btp_backtrace_free(backtrace);
    return hash;
}

struct btp_backtrace *
btp_backtrace_parse(const char **input,
                    struct btp_location *location)
{
    const char *local_input = *input;
    struct btp_backtrace *imbacktrace = btp_backtrace_new(); /* im - intermediate */
    imbacktrace->libs = btp_sharedlib_parse(*input);

    /* The header is mandatory, but it might contain no frame header,
     * in some broken backtraces. In that case, backtrace.crash value
     * is kept as NULL.
     */
    if (!btp_backtrace_parse_header(&local_input,
                                    &imbacktrace->crash,
                                    location))
    {
        btp_backtrace_free(imbacktrace);
        return NULL;
    }

    struct btp_thread *thread, *prevthread = NULL;
    while ((thread = btp_thread_parse(&local_input, location)))
    {
        if (prevthread)
        {
            btp_thread_add_sibling(prevthread, thread);
            prevthread = thread;
        }
        else
            imbacktrace->threads = prevthread = thread;
    }
    if (!imbacktrace->threads)
    {
        btp_backtrace_free(imbacktrace);
        return NULL;
    }

    *input = local_input;
    return imbacktrace;
}

bool
btp_backtrace_parse_header(const char **input,
                           struct btp_frame **frame,
                           struct btp_location *location)
{
    int first_thread_line, first_thread_column;
    const char *first_thread = btp_strstr_location(*input,
                                                   "\nThread ",
                                                   &first_thread_line,
                                                   &first_thread_column);

    /* Skip the newline. */
    if (first_thread)
    {
        ++first_thread;
        first_thread_line += 1;
        first_thread_column = 0;
    }

    int first_frame_line, first_frame_column;
    const char *first_frame = btp_strstr_location(*input,
                                                  "\n#",
                                                  &first_frame_line,
                                                  &first_frame_column);

    /* Skip the newline. */
    if (first_frame)
    {
        ++first_frame;
        first_frame_line += 1;
        first_frame_column = 0;
    }

    if (first_thread)
    {
        if (first_frame && first_frame < first_thread)
        {
            /* Common case. The crash frame is present in the input
             * before the list of threads begins.
             */
            *input = first_frame;
            btp_location_add(location, first_frame_line, first_frame_column);
        }
        else
        {
	    /* Uncommon case (caused by some kernel bug) where the
             * frame is missing from the header.  The backtrace
             * contains just threads.  We silently skip the header and
             * return true.
             */
  	    *input = first_thread;
            btp_location_add(location,
                             first_thread_line,
                             first_thread_column);
            *frame = NULL;
	    return true;
        }
    }
    else if (first_frame)
    {
        /* Degenerate case when the backtrace contains no thread, but
         * the frame is there.
         */
        *input = first_frame;
        btp_location_add(location, first_frame_line, first_frame_column);
    }
    else
    {
        /* Degenerate case where the input is empty or completely
         * meaningless. Report a failure.
         */
        location->message = "No frame and no thread found.";
        return false;
    }

    /* Parse the frame header. */
    *frame = btp_frame_parse(input, location);
    return *frame;
}

void
btp_backtrace_set_libnames(struct btp_backtrace *backtrace)
{
    struct btp_thread *thread;
    struct btp_frame *frame;
    struct btp_sharedlib *lib;

    thread = backtrace->threads;

    while (thread)
    {
        frame = thread->frames;
        while (frame)
        {
            lib = btp_sharedlib_find_address(backtrace->libs, frame->address);
            if (lib)
            {
                char *s1, *s2;

                /* Strip directory and version after the .so suffix. */
                s1 = strrchr(lib->soname, '/');
                if (!s1)
                    s1 = lib->soname;
                else
                    s1++;
                s2 = strstr(s1, ".so");
                if (!s2)
                    s2 = s1 + strlen(s1);
                else
                    s2 += strlen(".so");

                if (frame->library_name)
                    free(frame->library_name);
                frame->library_name = btp_strndup(s1, s2 - s1);
            }
            frame = frame->next;
        }
        thread = thread->next;
    }
}

struct btp_thread *
btp_backtrace_get_optimized_thread(struct btp_backtrace *backtrace, int max_frames)
{
    struct btp_thread *crash_thread;

    backtrace = btp_backtrace_dup(backtrace);
    crash_thread = btp_backtrace_find_crash_thread(backtrace);

    if (!crash_thread) {
        btp_backtrace_free(backtrace);
        return NULL;
    }

    btp_backtrace_remove_threads_except_one(backtrace, crash_thread);
    btp_backtrace_set_libnames(backtrace);
    btp_normalize_thread(crash_thread);
    btp_normalize_optimize_thread(crash_thread);

    /* Remove frames with no function name (i.e. signal handlers). */
    struct btp_frame *frame = crash_thread->frames, *frame_next;
    while (frame)
    {
        frame_next = frame->next;
        if (!frame->function_name)
            btp_thread_remove_frame(crash_thread, frame);
        frame = frame_next;
    }

    if (max_frames > 0)
        btp_thread_remove_frames_below_n(crash_thread, max_frames);

    crash_thread = btp_thread_dup(crash_thread, false);
    btp_backtrace_free(backtrace);

    return crash_thread;
}
