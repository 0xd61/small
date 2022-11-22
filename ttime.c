/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Time (execute with ttime)
Author: Daniel Glinka

This tool uses a time.txt file to track the time and run reports on it. In the future this should work with todo.sh

Time is written in ISO 8601 format (2022-03-08T01:38:00+00:00)
We can have one line per time entry and multiple columns devided by |. After the taskID everything is considered as annotation.

Example:
2022-03-08T01:38:00+00:00 | 2022-03-08T01:38:00+00:00 | taskID (-1 if no task specified) | other annotations

TODO(dgl):
    - Better printing (log vs output)
    - Add one newline too much on start/cont after a comment

Usage:
    ttime <flags> [command] [command args]

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#define DEBUG_TOKENIZER_PREVIEW 20
#define MAX_FILENAME_SIZE 4096
// NOTE(dgl): used to calculate about how much memory we have to allocate for our
// entry buffer.
#define AVERAGE_CHARS_PER_LINE 120

// TODO(dgl): @temporary
#define MAX_TAGS 5

#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <x86intrin.h>
#include <stdarg.h>

#include "helpers/types.h"
#define STRING_IMPLEMENTATION
#include "helpers/string.h"
#define MEMORY_IMPLEMENTATION
#include "helpers/memory.h"


// NOTE(dgl): Disable compiler warnings for stb includes
#if defined(__clang__)
#pragma clang diagnostic push
#if __clang_major__ > 7
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#endif
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"


#define STB_SPRINTF_IMPLEMENTATION
#include "helpers/stb_sprintf.h"

#pragma clang diagnostic pop
#endif


typedef struct Sort_Entry {
    uint32 sort_key;
    int32 index;
} Sort_Entry;

global const int32 days_in_month[] = {31,30,31,30,31,31,30,31,30,31,31,29};

typedef enum {
    Datetime_Duration_Invalid = 0x1 << 0,
    Datetime_Duration_Second  = 0x1 << 1,
    Datetime_Duration_Minute  = 0x1 << 2,
    Datetime_Duration_Hour    = 0x1 << 3,
    Datetime_Duration_Day     = 0x1 << 4,
    Datetime_Duration_Week    = 0x1 << 5,
    Datetime_Duration_Month   = 0x1 << 6,
    Datetime_Duration_Year    = 0x1 << 7,
} Datetime_Duration;

typedef struct {
    int32 year;
    int32 month;
    int32 day;
    int32 hour;
    int32 minute;
    int32 second;
    int32 offset_hour;
    int32 offset_minute;
    int32 offset_second;
    int32 offset_sign;
} Datetime;

typedef struct {
    String  input;

    int32   column;
    int32   line;

    bool32  has_error;
    char   error_msg[256];
} Tokenizer;

typedef struct {
    usize   begin; // NOTE(dgl): epoch of begin
    uintptr buffer_pos;
    int32   line;
    usize   length;
} EntryMeta;

typedef struct {
    Datetime   begin;
    Datetime   end;
    int32      task_id;
    int32      line;
    String     annotation;
} Entry;

typedef struct {
    String  filename;
    usize   filesize;
    bool32  exists;
} File_Stats;


typedef enum {
    Command_Type_Noop,
    Command_Type_Start,
    Command_Type_Stop,
    Command_Type_Continue,
    Command_Type_Report,
    Command_Type_CSV,
#if DEBUG
    Command_Type_Generate,
    Command_Type_Test,
#endif
} Command_Type;

typedef struct {
    int32   task_id;
    String  annotation;
} Command_Start;

typedef enum {
    Report_Type_Today,
    Report_Type_Week,
    Report_Type_Month,
    Report_Type_Year,
    Report_Type_Set_End_Date, // NOTE(dgl): Do not use as type. This is only a divider!
    Report_Type_Yesterday,
    Report_Type_Last_Week,
    Report_Type_Last_Month,
    Report_Type_Last_Year,
    Report_Type_Custom,
} Report_Type;

typedef struct {
    Report_Type type;
    Datetime    from;
    Datetime    to;
    String      filter[MAX_TAGS];
    int         filter_count;
} Command_Report;

typedef struct {
    Command_Report report;
    bool32         heading;
} Command_CSV;

typedef struct {
    Mem_Arena     *arena;
    Command_Type  command_type;
    bool32        is_valid;
    File_Stats    file;
    int32         window_columns;
    int32         window_rows;
    union {
        Command_Start  start;
        Command_Report report;
        Command_CSV    csv;
    };
} Commandline;

typedef enum {
    Print_Timezone = 0x1 << 0,
    Print_Seconds  = 0x1 << 1,
} Print_Flags;

//
//
//
internal usize max_entry_length(String annotation);

//
// Printing
//

internal void
print_date(String_Builder *builder, Datetime *datetime, int32 flags) {
    string_append(builder, "%04d-%02d-%02d", datetime->year, datetime->month, datetime->day);
}

internal void
print_time(String_Builder *builder, Datetime *datetime, int32 flags) {
    string_append(builder, "%02d:%02d", datetime->hour, datetime->minute);

    if ((flags & Print_Seconds) != 0) {
        string_append(builder, ":%02d", datetime->second);
    }

    if ((flags & Print_Timezone) != 0) {
        string_append(builder, " (%c%02d:%02d", datetime->offset_sign ? '-' : '+', datetime->offset_hour, datetime->offset_minute);
        if ((flags & Print_Seconds) != 0) {
            string_append(builder, ":%02d", datetime->offset_second);
        }
        string_append(builder, ")");
    }
}

internal void
print_hours(String_Builder *builder, usize seconds, int32 flags) {
    Datetime time = {};
    time.hour = cast(int32, seconds / 3600);
    seconds = seconds - (cast(usize, time.hour) * 3600);
    time.minute = cast(int32, seconds / 60);
    seconds = seconds - (cast(usize, time.minute) * 60);
    time.second = cast(int32, seconds);

    flags &= ~Print_Timezone;
    print_time(builder, &time, flags);
}

internal void
print_datetime(Mem_Arena *arena, int32 flags, char *fmt, ...) {
    Mem_Temp_Arena tmp_arena = mem_arena_begin_temp(arena);
    {
        usize fmt_len = string_length(fmt);
        char *cursor = mem_arena_push_array(tmp_arena.arena, char, fmt_len);
        string_copy(fmt, fmt_len, cursor, fmt_len);

        String_Builder builder = string_builder_init(tmp_arena.arena, 128);

        va_list args;
        va_start(args, fmt);
        while (*cursor) {
            if (string_compare(cursor, "%td", 3) == 0) {
                Datetime datetime = va_arg(args, Datetime);
                print_date(&builder, &datetime, flags);
                cursor += 3;
            } else if (string_compare(cursor, "%tt", 3) == 0) {
                Datetime datetime = va_arg(args, Datetime);
                print_time(&builder, &datetime, flags);
                cursor += 3;
            } else if (string_compare(cursor, "%th", 3) == 0) {
                usize seconds = va_arg(args, usize);
                print_hours(&builder, seconds, flags);
                cursor += 3;
            } else if (string_compare(cursor, "%ts", 3) == 0) {
                String text = va_arg(args, String);
                string_append(&builder, "%.*s", cast(int32, text.length), text.text);
                cursor += 3;
            } else {
                char *pos = cursor;
                ++cursor;
                while (*cursor != 0 && *cursor != '%') {
                    ++cursor;
                }
                char tmp = *cursor;
                *cursor = 0;
                string_append(&builder, pos, args);
                *cursor = tmp;
            }
        }

        va_end(args);

        String result = string_builder_to_string(&builder);
        fwrite(result.data, result.length, sizeof(char), stdout);
#if DEBUG
        fflush(stdout);
#endif

    }
    mem_arena_end_temp(tmp_arena);
}


//
// Timing
//

internal inline struct timespec
get_wall_clock()
{
    struct timespec result;
    clock_gettime(CLOCK_MONOTONIC, &result);
    return(result);
}

internal inline real64
get_ms_elapsed(struct timespec start, struct timespec end)
{
    int64 sec = end.tv_sec - start.tv_sec;
    real64 result = (real64)(sec * 1000) +
                    ((real64)(end.tv_nsec - start.tv_nsec) * 1e-6);
    return(result);
}

usize get_rdtsc(){
    return __rdtsc();
}

//
// File/Buffer
//

internal Buffer
allocate_filebuffer(Mem_Arena *arena, File_Stats *file, usize padding) {
    Buffer result = {};
    if (file->exists) {
        result.data_count = 0;
        result.cap = file->filesize + padding;

        // TODO(dgl): push padding separately
        LOG_DEBUG("Allocating memory for %lu bytes (filesize with padding)", result.cap);

        // TODO(dgl): reallocate memory on arena overflow
        result.data = mem_arena_push_array(arena, uint8, result.cap);
    }

    return result;
}


internal File_Stats
get_file_stats(Mem_Arena *arena, String filename) {
    File_Stats result = {};
    result.filename = filename;

    struct stat file_stat = {};
    int err = stat(filename.text, &file_stat);
    if (err == 0) {
        result.filesize = cast(usize, file_stat.st_size);
        result.exists = true;
    } else {
        LOG_DEBUG("Failed to get file stats for file %s: err %d", string_to_c_str(arena, filename), err);
    }

    return result;
}

internal void
read_entire_file(Mem_Arena *temp_arena, File_Stats *file, Buffer *buffer) {
    int fd = open(file->filename.text, O_RDONLY);
    if (fd) {
        if (file->filesize > 0) {
            lseek(fd, 0, SEEK_SET);
            ssize_t res = read(fd, buffer->data, buffer->cap);
            if (res >= 0) {
                buffer->data_count = cast(usize, res);
            } else {
                buffer->data_count = 0;
                LOG("Failed to read file %s", string_to_c_str(temp_arena, file->filename));
            }
        } else {
            LOG_DEBUG("File is empty: %s", string_to_c_str(temp_arena, file->filename));
        }
        close(fd);
    } else {
        LOG("Could not open file: %s", string_to_c_str(temp_arena, file->filename));
    }
}

// NOTE(dgl): offset after last text character of last line of file
internal usize
get_end_of_file_offset(Buffer *buffer) {
    usize result = 0;
    char *input = cast(char *, buffer->data);
    if (input) {
        char *cursor = input + buffer->data_count;
        while (cursor > input &&
              (*cursor <= ' ')) {
            cursor--;
        }

        assert(cursor > cast(char *, buffer->data), "Buffer overflow. Cursor cannot be larger than the data buffer");
        result = cast(usize, cursor - cast(char *, buffer->data)) + 1; // +1 to go one byte after last text character
    }

    return result;
}

// NOTE(dgl): appends the different buffers.
internal void
write_entire_file(Mem_Arena *arena, File_Stats *file, int buffer_count, ...) {
    char tmp_filename[MAX_FILENAME_SIZE];
    assert(file->filename.length + 2 < MAX_FILENAME_SIZE, "Filename too long. Increase MAX_FILENAME_SIZE.");
    string_copy(file->filename.text, file->filename.length, tmp_filename, MAX_FILENAME_SIZE);

    tmp_filename[file->filename.length] = '~';
    tmp_filename[file->filename.length + 1] = 0;

    int fd = open(tmp_filename, O_WRONLY | O_CREAT, 0644);
    if (fd) {
        va_list buffers;
        va_start(buffers, buffer_count);

        for (int index = 0; index < buffer_count; ++index) {
            Buffer *buffer = va_arg(buffers, Buffer *);
            char *test = cast(char *, buffer->data);
            ssize_t res = write(fd, buffer->data, buffer->data_count);
            if (res < 0) {
                LOG("Failed writing to file %s with error: %d", tmp_filename, errno);
            } else {
                LOG_DEBUG("Written %ld bytes of %ld bytes to %s", res, buffer->data_count, tmp_filename);
            }
        }

        va_end(buffers);
    }
    close(fd);

    if(rename(tmp_filename, file->filename.text) != 0) {
        LOG("Failed to move content from temporary file %s to %s", tmp_filename, string_to_c_str(arena, file->filename));
    }
}

internal Buffer
entry_to_buffer(Mem_Arena *arena, Entry *entry) {
    String_Builder builder = string_builder_init(arena, sizeof(char) * 80);
    string_append(&builder, "\n");
    string_append(&builder, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d | ",
                           entry->begin.year,
                           entry->begin.month,
                           entry->begin.day,
                           entry->begin.hour,
                           entry->begin.minute,
                           entry->begin.second,
                           entry->begin.offset_sign ? '-' : '+',
                           entry->begin.offset_hour,
                           entry->begin.offset_minute,
                           entry->begin.offset_second);

    if (entry->end.year > 0) {
        string_append(&builder, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d | ",
                               entry->end.year,
                               entry->end.month,
                               entry->end.day,
                               entry->end.hour,
                               entry->end.minute,
                               entry->end.second,
                               entry->end.offset_sign ? '-' : '+',
                               entry->end.offset_hour,
                               entry->end.offset_minute,
                               entry->end.offset_second);
    } else {
        string_append(&builder, " | ");
    }

    string_append(&builder, "%d | ", entry->task_id);

    if (entry->annotation.data) {
        string_append(&builder, "%s\n", string_to_c_str(arena, entry->annotation));
    } else {
        string_append(&builder, "\n");
    }

    String string = string_builder_to_string(&builder);
    Buffer result = string_to_buffer(&string);

    return (result);
}

// NOTE(dgl): Returns 0 if the complete buffer b was merged into buffer a.
// It buffer b is larger than a, we return the number of the missing bytes.
internal usize
buffer_merge_at(Buffer *a, Buffer *b, usize pos) {
    assert(pos < a->cap, "Position outside of buffer");
    usize bytes_to_write = b->data_count;

    if (a->cap - pos < bytes_to_write) {
        bytes_to_write = cast(usize, a->cap - pos - 1);
    }

    assert(b->data_count >= bytes_to_write, "Cannot write more bytes than available");

    usize missing_bytes = b->data_count - bytes_to_write;
    string_copy(b->data, bytes_to_write, a->data + pos, a->cap - pos);
    a->data_count = pos + bytes_to_write;

    LOG_DEBUG("Buffer does not have enough space. We will only merge partially - left %lu, required %lu, missing %lu", a->cap - pos, b->data_count, missing_bytes);

    return missing_bytes;
}

//
// Tokenizer/Parser
//
internal inline usize datetime_to_epoch(Datetime *datetime);
internal void print_timestamp(Datetime *timestamp);

internal void
fill_tokenizer(Tokenizer *tokenizer, Buffer *buffer) {
    tokenizer->input.data = buffer->data;
    tokenizer->input.length = buffer->data_count;
    tokenizer->input.cap = buffer->cap;

    tokenizer->line = 1;
    tokenizer->column = 0;
}

internal void
set_tokenizer(Tokenizer *tokenizer, usize offset) {
    assert(offset < tokenizer->input.cap, "Offset cannot be larger than tokenizer input. Max value: %zu, got: %zu", tokenizer->input.cap, offset);
    tokenizer->input.data += offset;
    tokenizer->input.length -= offset;
}

internal void
token_error(Tokenizer *tokenizer, char *msg) {
    // NOTE(dgl): Only report first error.
    // During parsing after an error occured there may be more
    // because after an error all tokens become invalid.
    if (!tokenizer->has_error) {
        tokenizer->has_error = true;
        LOG_DEBUG("Parsing error at line %d, column %d: %s", tokenizer->line, tokenizer->column + 1, msg);
        // TODO(dgl): can we use the permanent_arena to store this error?
        stbsp_snprintf(tokenizer->error_msg, array_count(tokenizer->error_msg), "Parsing error at line %d, column %d: %s", tokenizer->line, tokenizer->column + 1, msg);
    }
}

// NOTE(dgl): does not support unicode currently
internal inline void
eat_next_character(Tokenizer *tokenizer) {
    if (!tokenizer->has_error && tokenizer->input.length > 0) {
        // LOG_DEBUG("Eaten character %c (%d)", *tokenizer->input.text, *tokenizer->input.text);
        ++tokenizer->input.data;
        --tokenizer->input.length;
        ++tokenizer->column;
        if (*tokenizer->input.text == '\n') {
            ++tokenizer->line;
            tokenizer->column = 0;
        }
    }
}

// NOTE(dgl): negative lookahead is possible but be careful!
internal inline char
peek_character(Tokenizer *tokenizer, int32 lookahead) {
    char result = 0;
    if (!tokenizer->has_error && tokenizer->input.length > 0) {
        assert(abs(lookahead) < tokenizer->input.length, "Lookahead cannot be larger than input");
        result = *(tokenizer->input.text + lookahead);
    }

    // LOG_DEBUG("Peeked character %c (%d) - Buffered: %.*s", result, result, DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);

    return result;
}

internal inline char
peek_next_character(Tokenizer *tokenizer) {
    char result = peek_character(tokenizer, 0);
    return result;
}

// NOTE(dgl): only checks for spaces and tabs
internal inline bool32
is_whitespace(char c) {
    bool32 result = false;

    if (c == ' ' || c == '\t') {
        result = true;
    }

    return result;
}

// NOTE(dgl): eats all whitespace newlines and comments
internal inline void
eat_all_whitespace(Tokenizer *tokenizer) {
    char c = peek_next_character(tokenizer);

    while(is_whitespace(c) || c == '/' || c == '\n') {
        if (c == '/') {
            char next_c = peek_character(tokenizer, 1);
            if (next_c == '/') {
                while(peek_next_character(tokenizer) != '\n' && tokenizer->input.length > 0) {
                    eat_next_character(tokenizer);
                }
            } else {
                return;
            }
        } else if (c == '\n') {
            char next_c = peek_character(tokenizer, 1);
            while(peek_next_character(tokenizer) == '\n' && tokenizer->input.length > 0) {
                eat_next_character(tokenizer);
            }
            return;
        }

        eat_next_character(tokenizer);
        c = peek_next_character(tokenizer);
    }
}

internal String
parse_string_line(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing string line from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    String result = {};
    if (!tokenizer->has_error) {
        char *begin = tokenizer->input.text;
        char next = peek_next_character(tokenizer);
        int32 length = 0;
        while(!(next == '\n' || next == 0)) {
            eat_next_character(tokenizer);
            next = peek_next_character(tokenizer);
            ++length;
        }

        result.length = length;
        result.cap = length;
        result.text = begin;
    }

    return result;
}

internal int32
parse_integer(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing integer from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    int32 result = 0;

    char c = peek_next_character(tokenizer);
    bool32 is_negative = false;
    if (c == '-') {
        is_negative = true;
        eat_next_character(tokenizer);
        c = peek_next_character(tokenizer);
    }

    if (c >= '0' && c <= '9') {
        result = c - '0';
        eat_next_character(tokenizer);
        c = peek_next_character(tokenizer);
        while (c >= '0' && c <= '9') {
            result *= 10;
            result += c - '0';
            eat_next_character(tokenizer);
            c = peek_next_character(tokenizer);
        }
    } else {
        token_error(tokenizer, "Failed to parse integer. Expected a numeric value");
    }

    if (is_negative) {
        result = -result;
    }

    return result;
}

internal Datetime
parse_date(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing date from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Datetime result = {};
    char c = 0;

    result.year = parse_integer(tokenizer);
    if (result.year < 1900 || result.year >= 10000) {
        token_error(tokenizer, "Failed to parse date: Invalid year - expected yyyy-mm-dd");
    }

    c = peek_next_character(tokenizer);
    if (c == '-') {
        eat_next_character(tokenizer);
    } else {
        token_error(tokenizer, "Could not parse date: Invalid format - expected yyyy-mm-dd");
    }

    result.month = parse_integer(tokenizer);
    if (result.month < 1 || result.month > 12) {
        token_error(tokenizer, "Failed to parse date: Invalid month - expected yyyy-mm-dd");
    }

    c = peek_next_character(tokenizer);
    if (c == '-') {
        eat_next_character(tokenizer);
    } else {
        token_error(tokenizer, "Could not parse date: Invalid format - expected yyyy-mm-dd");
    }

    result.day = parse_integer(tokenizer);
    if (result.day < 1 || result.day > 31) {
        token_error(tokenizer, "Failed to parse date: Invalid day - expected yyyy-mm-dd");
    }

    return result;
}

internal Datetime
parse_time(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing time from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Datetime result = {};

    result.hour = parse_integer(tokenizer);
    if (result.hour < 0 || result.hour > 23) {
        token_error(tokenizer, "Failed to parse time: Invalid hour - expected hh[:mm[:ss]]");
    }

    char c = peek_next_character(tokenizer);
    if (c == ':') {
        eat_next_character(tokenizer);
        result.minute = parse_integer(tokenizer);
        if (result.minute < 0 || result.minute > 59) {
            token_error(tokenizer, "Failed to parse time: Invalid minute - expected hh:mm[:ss]");
        }
    }

    c = peek_next_character(tokenizer);
    if (c == ':') {
        eat_next_character(tokenizer);
        result.second = parse_integer(tokenizer);
        if (result.second < 0 || result.second > 59) {
            token_error(tokenizer, "Failed to parse time: Invalid second - expected hh:mm:ss");
        }
    }

    return result;
}

internal Datetime
parse_timezone(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing timezone from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Datetime result = {};
    char c = peek_next_character(tokenizer);

    if (c == '-' || c == '+') {
        if (c == '-') {
            result.offset_sign = true;
        }
        eat_next_character(tokenizer);
    } else {
        token_error(tokenizer, "Failed to parse timezone: Invalid format - expected [+|-]hh[:mm[:ss]]");
    }

    Datetime offset = parse_time(tokenizer);
    result.offset_hour = offset.hour;
    result.offset_minute = offset.minute;
    result.offset_second = offset.second;

    return result;
}

internal Datetime
parse_datetime(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing datetime from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Datetime result = {};
    Datetime date = parse_date(tokenizer);

    char c = peek_next_character(tokenizer);
    if (c == 'T') {
        eat_next_character(tokenizer);
    } else {
        token_error(tokenizer, "Failed to parse datetime: missing date-time divider T - expected yyyy-mm-ddThh[:mm[:ss]][+|-]hh[:mm[:ss]]");
    }

    Datetime time = parse_time(tokenizer);
    Datetime timezone = parse_timezone(tokenizer);

    result.year = date.year;
    result.month = date.month;
    result.day = date.day;
    result.hour = time.hour;
    result.minute = time.minute;
    result.second = time.second;
    result.offset_sign = timezone.offset_sign;
    result.offset_hour = timezone.offset_hour;
    result.offset_minute = timezone.offset_minute;
    result.offset_second = timezone.offset_second;

    return result;
}

internal EntryMeta
parse_entry_meta(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing entry meta from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    EntryMeta result = {};

    eat_all_whitespace(tokenizer);
    char *pos = tokenizer->input.text;
    Datetime begin = parse_datetime(tokenizer);
    parse_string_line(tokenizer);

    char c = peek_next_character(tokenizer);
    if (c == '\n') {
        eat_next_character(tokenizer);
    }

    if (!tokenizer->has_error) {
        result.begin = datetime_to_epoch(&begin);
        result.buffer_pos = cast(uintptr, pos);
        result.line = tokenizer->line;
        result.length = tokenizer->input.text - pos;
    }

    return result;
}

internal Entry
parse_entry(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing entry from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Entry result = {};

    eat_all_whitespace(tokenizer);
    result.begin = parse_datetime(tokenizer);
    eat_all_whitespace(tokenizer);

    char c = peek_next_character(tokenizer);
    if (c == '|') {
        eat_next_character(tokenizer);
        eat_all_whitespace(tokenizer);
    } else {
        token_error(tokenizer, "Failed to parse entry. Expected a divider (|)");
    }

    // NOTE(dgl): maybe the end time, otherwise a |
    c = peek_next_character(tokenizer);
    if (c == '|') {
        eat_next_character(tokenizer);
        eat_all_whitespace(tokenizer);
    } else {
        result.end = parse_datetime(tokenizer);
        eat_all_whitespace(tokenizer);
        c = peek_next_character(tokenizer);
        if (c == '|') {
            eat_next_character(tokenizer);
            eat_all_whitespace(tokenizer);
        } else {
            token_error(tokenizer, "Failed to parse entry. Expected a divider (|)");
        }
    }

    c = peek_next_character(tokenizer);
    if (c == '|') {
        eat_next_character(tokenizer);
        eat_all_whitespace(tokenizer);
    } else {
        result.task_id = parse_integer(tokenizer);
        eat_all_whitespace(tokenizer);
        c = peek_next_character(tokenizer);
        if (c == '|') {
            eat_next_character(tokenizer);
            eat_all_whitespace(tokenizer);
        } else {
            token_error(tokenizer, "Failed to parse entry. Expected a divider (|)");
        }
    }

    result.annotation = parse_string_line(tokenizer);
    c = peek_next_character(tokenizer);
    if (c == '\n') {
        eat_next_character(tokenizer);
    }

    return result;
}

internal Entry
parse_entry_at(Tokenizer *tokenizer, usize offset) {
    set_tokenizer(tokenizer, offset);
    Entry result = parse_entry(tokenizer);
    return result;
}

// TODO(dgl): make this possible with parse_entry_at. We need to set the tokenizer to a buffer
// offset. But this offset can be before or after the current position. Maybe the best thing would
// be storing the buffer address somewhere and do the offset setting based on this address.
// This is a little tricky because new entries do not have meta data.
internal Entry
parse_entry_from_meta(Tokenizer *tokenizer, EntryMeta *meta) {
    tokenizer->input.data = cast(void *, meta->buffer_pos);
    tokenizer->input.length = meta->length;
    tokenizer->line = meta->line;

    Entry result = parse_entry(tokenizer);
    return result;
}

internal usize
max_entry_length(String annotation) {
    // example: 2022-03-08T01:38:00:00+00:00:00 | 2022-03-08T01:38:00:00+00:00:00 | taskID (-1 if no task specified) | other annotations
    usize result = 0;
    usize max_datetime_len = string_length("2022-03-08T01:38:00:00+00:00:00");
    usize max_task_id_len = string_length("-2147483648");
    usize max_annotation_len = annotation.length;

    usize max_divider_len = string_length(" | ");
    result = max_datetime_len + max_divider_len + max_datetime_len + max_divider_len + max_task_id_len + max_divider_len + max_annotation_len;

    return result + 1;  // +1 because of \n
}

// NOTE(dgl): offset before last line of file (line containing text)
// TODO(dgl): replace by tokenizer
// internal usize
// get_last_line_offset(Buffer *buffer) {
//     usize result = 0;
//     char *input = cast(char *, buffer->data);
//     if (input) {
//         char *cursor = input + buffer->data_count;
//         bool32 found_text = false;
//         while (cursor > input &&
//               (*cursor != '\n' || !found_text)) {
//             if (*cursor > ' ') {
//                 found_text = true;
//             }
//             cursor--;
//         }

//         assert(cursor > cast(char *, buffer->data), "Buffer overflow. Cursor cannot be larger than the data buffer");
//         result = cast(usize, cursor - cast(char *, buffer->data)) + 1; // +1 to go to character after the \n


//     }

//     return result;
// }

// NOTE(dgl): offset before last line of file (line containing text)
internal usize
get_last_line_offset(Tokenizer *tokenizer) {
    usize result = 0;

    char *orig_input = tokenizer->input.text;
    tokenizer->input.text += tokenizer->input.length;

    int32 offset = 0;
    while(result == 0 && abs(offset) < tokenizer->input.length) {
        char cursor = peek_character(tokenizer, offset);
        while (cursor != '\n' && abs(offset) < tokenizer->input.length) {
            cursor = peek_character(tokenizer, --offset);
        }

        // NOTE(dgl): ignore whitespace at the beginning of the line
        int32 whitespace_offset = 1;
        while(is_whitespace(peek_character(tokenizer, offset + whitespace_offset))) {
            whitespace_offset++;
        }

        char offset_cursor = peek_character(tokenizer, offset + whitespace_offset);
        char next_offset_cursor = peek_character(tokenizer, offset + whitespace_offset + 1);
        if (offset_cursor == '\n' ||
            offset_cursor == '\0' || (
                offset_cursor == '/' &&
                next_offset_cursor == '/')
            ) {
            offset--;
        } else {
            result = tokenizer->input.length + offset;
        }
    }

    assert(result < tokenizer->input.length, "Invalid offset - got: %lu expected: < %lu", result, tokenizer->input.length);

    tokenizer->input.text = orig_input;
    return result;
}

//
// Time/Datetime
// NOTE(dgl): internally everything is compared to UTC!
//

internal Datetime
get_timezone_offset() {
    void tzset(void);
    extern char *tzname[2];
    extern int daylight;
    tzset();
    LOG_DEBUG("The timezone is %s and %s", tzname[0], tzname[1]);

    Datetime result = {};

    char *tz = tzname[0];
    if (*tzname[1] && daylight) {
        tz = tzname[1];
    }

    if (string_compare("UTC", tz, 3) != 0) {
        Tokenizer tokenizer = {};
        tokenizer.input = string_from_c_str(tz);
        result = parse_timezone(&tokenizer);
    }

    return result;
}

internal Datetime
get_timestamp() {
    Datetime result = {};

    time_t epoch = time(NULL);
    struct tm local = *localtime(&epoch);
    Datetime tz = get_timezone_offset();

    result.second = local.tm_sec;
    result.minute = local.tm_min;
    result.hour = local.tm_hour;
    result.day = local.tm_mday;
    result.month = local.tm_mon + 1;
    result.year = local.tm_year + 1900;
    result.offset_second = tz.offset_second;
    result.offset_minute = tz.offset_minute;
    result.offset_hour = tz.offset_hour;
    result.offset_sign = tz.offset_sign;

    return result;
}

internal void
print_timestamp(Datetime *timestamp) {
    LOG_DEBUG("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d", timestamp->year,
                                                              timestamp->month,
                                                              timestamp->day,
                                                              timestamp->hour,
                                                              timestamp->minute,
                                                              timestamp->second,
                                                              timestamp->offset_sign ? '-' : '+',
                                                              timestamp->offset_hour,
                                                              timestamp->offset_minute,
                                                              timestamp->offset_second);
}

internal inline usize
datetime_to_epoch(Datetime *datetime) {
    usize result = 0;
	assert(datetime->year >= 1900, "Year cannot be smaller than 1900");

	struct tm platform_datetime = { .tm_sec  = datetime->second,
                                    .tm_min  = datetime->minute,
                                    .tm_hour = datetime->hour,
                                    .tm_mday = datetime->day,
                                    .tm_mon  = datetime->month - 1,
                                    .tm_year = datetime->year - 1900};

	usize timestamp = mktime(&platform_datetime);
    usize timezone_offset = (((datetime->offset_hour * 60) + datetime->offset_minute) * 60) + datetime->offset_second;

    if(datetime->offset_sign) {
        assert(timezone_offset < timestamp, "Invalid timezone offset. Offset cannot be larger than the timestamp.");
        timestamp += timezone_offset;
    } else {
        timestamp -= timezone_offset;
    }

    result = timestamp;

    return result;
}

internal inline bool32
_is_leap_year(int32 year) {
    // 4th year test: year & 3 => is the same as year % 4 (only works for powers of 2).
    // 100th year test: year % 25 => 100 factors out to 2 x 2 x 5 x 5. Because the 4th year test already checks for factors of 4 we can eliminate that factor from 100, leaving 25.
    // 400th year test: year & 15 => same as year % 16. 400 factors out to 2 x 2 x 2 x 2 x 5 x 5. We can eliminate the factor of 25 which is tested by the 100th year test, leaving 16.
    bool32 result = ((year & 3) == 0 && ((year % 25) != 0 || (year & 15) == 0));

    return result;
}

internal inline int32
_get_weekday(int32 year, int32 month, int32 day) {
    // https://en.wikipedia.org/wiki/Determination_of_the_day_of_the_week#Keith
    int32 result = (day += month < 3 ? year-- : year - 2, 23 * month/9 + day + 4 + year/4- year/100 + year/400) % 7;
    return result;
}

// NOTE(dgl): returns overflow factor.
internal inline int32
datetime_wrap(int32 *value, int32 lo, int32 hi) {
    int32 result = 0;
    hi = hi + 1;

    if (*value < 0) {
        result = (*value / hi) - 1;
    } else {
        result = (*value / hi);
    }
    int32 rem = (lo + (*value - lo) % (hi - lo));
    *value = rem < 0 ? rem + hi : rem;

    return result;
}

// TODO(dgl): would this be more efficient converting everything to s, and back?
internal void
datetime_normalize(Datetime *datetime) {
//     LOG_DEBUG("Date before normalizing");
//     print_timestamp(datetime);

    datetime->minute += datetime_wrap(&datetime->second, 0, 59);
    datetime->hour += datetime_wrap(&datetime->minute, 0, 59);
    datetime->day += datetime_wrap(&datetime->hour, 0, 23);
    datetime->month += datetime_wrap(&datetime->day, 1, 31);
    datetime->year += datetime_wrap(&datetime->month, 1, 12);

    // NOTE(dgl): fixing month (month_index go optimized out)
    int32 month_index = (datetime->month - 3) % (array_count(days_in_month) - 1);
    int32 days = days_in_month[month_index];
    if (month_index == array_count(days_in_month) - 1 &&
        !_is_leap_year(datetime->year)) {
        days -= 1;
    }
    datetime->month += datetime_wrap(&datetime->day, 1, days);

    assert(datetime->year >= 1900, "Invalid year. Year must be greater then 1900, got: %d", datetime->year);
    assert(datetime->month > 0 && datetime->month <= 12, "Invalid month. Month must be between 1 and 12, got: %d", datetime->month);
    assert(datetime->day > 0 && datetime->day <= 31, "Invalid day. Day must be between 1 and 31, got: %d", datetime->day);
    assert(datetime->hour >= 0 && datetime->hour < 60, "Invalid hour. Hour must be between 0 and 59, got: %d", datetime->hour);
    assert(datetime->minute >= 0 && datetime->minute < 60, "Invalid minute. Minute must be between 0 and 59, got: %d", datetime->minute);
    assert(datetime->second >= 0 && datetime->month < 60, "Invalid second. Second must be between 0 and 59, got: %d", datetime->second);

//     LOG_DEBUG("Date after normalizing");
//     print_timestamp(datetime);
}

internal int32
datetime_report_ops_by_type(Report_Type type) {
    int32 result = 0;
    switch(type) {
        case Report_Type_Last_Year:
        case Report_Type_Year: {
            result |= Datetime_Duration_Second |
                   Datetime_Duration_Minute |
                   Datetime_Duration_Hour |
                   Datetime_Duration_Day |
                   Datetime_Duration_Month;
        } break;
        case Report_Type_Last_Month:
        case Report_Type_Month: {
            result |= Datetime_Duration_Second |
                   Datetime_Duration_Minute |
                   Datetime_Duration_Hour |
                   Datetime_Duration_Day;
        } break;
        case Report_Type_Yesterday:
        case Report_Type_Today: {
            result |= Datetime_Duration_Second |
                   Datetime_Duration_Minute |
                   Datetime_Duration_Hour;
        } break;
        case Report_Type_Last_Week:
        case Report_Type_Week: {
            result |= Datetime_Duration_Second |
                   Datetime_Duration_Minute |
                   Datetime_Duration_Hour |
                   Datetime_Duration_Week;
        } break;
        default: {
            // NOTE(dgl): no need of handling the other types
        }
    }

    return result;
}

internal Datetime
datetime_to_beginning_of(Report_Type type, Datetime *datetime) {
    Datetime result = *datetime;
    int ops = datetime_report_ops_by_type(type);

    switch(type) {
        case Report_Type_Last_Year: { result.year -= 1; } break;
        case Report_Type_Last_Month: { result.month -= 1; } break;
        case Report_Type_Yesterday: { result.day -= 1; } break;
        case Report_Type_Last_Week: { result.day -= 7; } break;
        default: {
            // NOTE(dgl): no need of handling the other types
        }
    }

    if (ops & Datetime_Duration_Second) {
        result.second = 0;
    }
    if (ops & Datetime_Duration_Minute) {
        result.minute = 0;
    }
    if (ops & Datetime_Duration_Hour) {
        result.hour = 0;
    }
    if (ops & Datetime_Duration_Day) {
        result.day = 1;
    }
    if (ops & Datetime_Duration_Month) {
        result.month = 1;
    }
    if (ops & Datetime_Duration_Year) {
        result.year = 1900;
    }
    if (ops & Datetime_Duration_Week) {
        datetime_normalize(&result);
        int32 weekday = _get_weekday(result.year, result.month, result.day);
        result.day -= weekday;
    }

    datetime_normalize(&result);

    return result;
}

internal Datetime
datetime_to_end_of(Report_Type type, Datetime *datetime) {
  Datetime result = *datetime;
    int ops = datetime_report_ops_by_type(type);

    switch(type) {
        case Report_Type_Last_Year: { result.year -= 1; } break;
        case Report_Type_Last_Month: { result.month -= 1; } break;
        case Report_Type_Yesterday: { result.day -= 1; } break;
        case Report_Type_Last_Week: { result.day -= 7; } break;
        default: {
            // NOTE(dgl): no need of handling the other types
        }
    }

    if (ops & Datetime_Duration_Second) {
        result.second = 59;
    }
    if (ops & Datetime_Duration_Minute) {
        result.minute = 59;
    }
    if (ops & Datetime_Duration_Hour) {
        result.hour = 23;
    }
    if (ops & Datetime_Duration_Day) {
        int32 month_index = (result.month - 3) % 12;
        int32 days = days_in_month[month_index];
        if (month_index == array_count(days_in_month) - 1 &&
            !_is_leap_year(result.year)) {
            days -= 1;
        }
        result.day = days;
    }
    if (ops & Datetime_Duration_Month) {
        result.month = 12;
    }
    if (ops & Datetime_Duration_Year) {
        result.year = 3999;
    }
    if (ops & Datetime_Duration_Week) {
        datetime_normalize(&result);
        int32 weekday = _get_weekday(result.year, result.month, result.day);
        result.day += 6 - weekday;
    }

    datetime_normalize(&result);

    return result;
}

internal bool32
report_tag_matches(Commandline *ctx, Entry *entry) {
    bool32 result = false;

    if (ctx->report.filter_count > 0) {
        Tokenizer tokenizer = {};
        Buffer buffer = {};
        buffer.data = entry->annotation.data;
        buffer.data_count = entry->annotation.length;
        buffer.cap = entry->annotation.cap;
        fill_tokenizer(&tokenizer, &buffer);

        char *begin = 0;
        int32 length = 0;
        while(!tokenizer.has_error && tokenizer.input.length > 0) {
            char next = peek_next_character(&tokenizer);
            length++;

            if (next == '@' || next == '+') {
                char *begin = tokenizer.input.text;
                int32 length = 0;
                while (!is_whitespace(next) && tokenizer.input.length > 0) {
                    eat_next_character(&tokenizer);
                    next = peek_next_character(&tokenizer);
                    ++length;
                }

                for (int index = 0; index < ctx->report.filter_count; ++index) {
                    String tag = ctx->report.filter[index];

                    if (tag.length == length && string_compare(string_to_c_str(ctx->arena, tag), begin, length) == 0) {
                        result = true;
                        return result;
                    }
                }
            }
            eat_next_character(&tokenizer);
            eat_all_whitespace(&tokenizer);
        }
    } else {
        result = true;
    }

    return result;
}

//
// Commandline
//

internal void
commandline_parse_start_cmd(Commandline *ctx, char** args, int args_count) {
    ctx->start.task_id = -1;
    int32 cursor = 0;

    while(cursor < args_count &&
          string_compare("-", args[cursor], 1) == 0) {
        char *arg = args[cursor++];
        if (string_compare("-t", arg, 2) == 0) {
            char *number = args[cursor++];
            ctx->start.task_id = string_to_int32(number, cast(int32, string_length(number)));
        } else {
            ++cursor;
        }
    }

    // NOTE(dgl): everything after the flags is considered annotation
    if(cursor < args_count) {
        usize required_memory = 0;
        int32 start_count = cursor;

        while(cursor < args_count) {
            required_memory += string_length(args[cursor++]);
            required_memory += 1; // space or nullbyte
        }
        ctx->start.annotation.cap = required_memory;
        ctx->start.annotation.length = required_memory - 1;
        ctx->start.annotation.text = mem_arena_push_array(ctx->arena, char, required_memory);

        char *dest = ctx->start.annotation.text;
        cursor = start_count;
        // NOTE(dgl): each annotation is a separate arg therefore we concat them and add a space between
        while(cursor < args_count) {
            char *annotation = args[cursor++];
            usize length = string_length(annotation);
            string_copy(annotation, length, dest, length);
            dest += length;
            *dest++ = ' ';
        }

        ctx->start.annotation.text[required_memory] = 0;
    } else {
        ctx->start.annotation = string_from_c_str("");
    }
}

#if DEBUG
internal void
commandline_parse_test_cmd(Commandline *ctx, char** args, int args_count) {
    int32 cursor = 0;

    ctx->report.type = Report_Type_Today;
    while(cursor < args_count) {
        char *arg = args[cursor++];
        if ((*arg == '@') || (*arg == '+')) {
            if (ctx->report.filter_count < MAX_TAGS) {
                String *filter = ctx->report.filter + ctx->report.filter_count;
                usize length = string_length(arg);
                filter->length = length;
                filter->cap = length;
                filter->text = arg;
                ctx->report.filter_count++;
            } else {
                LOG("Max number of filter exceeded. Filter %s will be ignored", arg);
            }
        }
    }
}
#endif

internal void
commandline_parse_report_cmd(Commandline *ctx, char** args, int args_count) {
    int32 cursor = 0;

    ctx->report.type = Report_Type_Today;
    while(cursor < args_count) {
        char *arg = args[cursor++];
        if ((string_compare("yes", arg, 3) == 0)) {
            ctx->report.type = Report_Type_Yesterday;
        } else if ((string_compare("m", arg, 1) == 0)) {
            ctx->report.type = Report_Type_Month;
        } else if ((string_compare("lastm", arg, 5) == 0)) {
            ctx->report.type = Report_Type_Last_Month;
        } else if ((string_compare("w", arg, 1) == 0)) {
            ctx->report.type = Report_Type_Week;
        } else if ((string_compare("lastw", arg, 5) == 0)) {
            ctx->report.type = Report_Type_Last_Week;
        } else if ((string_compare("yea", arg, 3) == 0)) {
            ctx->report.type = Report_Type_Year;
        } else if ((string_compare("lasty", arg, 5) == 0)) {
            ctx->report.type = Report_Type_Last_Year;
        } else if ((*arg == '@') || (*arg == '+')) {
            if (ctx->report.filter_count < MAX_TAGS) {
                String *filter = ctx->report.filter + ctx->report.filter_count;
                usize length = string_length(arg);
                filter->length = length;
                filter->cap = length;
                filter->text = arg;
                ctx->report.filter_count++;
            } else {
                LOG("Max number of filter exceeded. Filter %s will be ignored", arg);
            }
        }
    }

    Datetime now = get_timestamp();
    if (ctx->report.type == Report_Type_Custom) {
        // NOTE(dgl): currently not supported
    } else {
        ctx->report.from = datetime_to_beginning_of(ctx->report.type, &now);
        ctx->report.to = now;
        if (ctx->report.type > Report_Type_Set_End_Date) {
            ctx->report.to = datetime_to_end_of(ctx->report.type, &now);
        }
    }

    LOG_DEBUG("Report type: %d", ctx->report.type);
    LOG_DEBUG("Report from: ");
    print_timestamp(&ctx->report.from);
    LOG_DEBUG("Report to: ");
    print_timestamp(&ctx->report.to);
}

internal void
commandline_parse_csv_cmd(Commandline *ctx, char** args, int args_count) {

}

internal void
commandline_parse(Mem_Arena *arena, Commandline *ctx, char** args, int args_count) {
    ctx->arena = arena;
    ctx->is_valid = true;

    File_Stats home = get_file_stats(arena, string_from_c_str("~/time.txt"));
    File_Stats local = get_file_stats(arena, string_from_c_str("./time.txt"));
    ctx->file = local;
    if (!ctx->file.exists) {
        ctx->file = home;
    }

    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    ctx->window_rows = w.ws_row;
    ctx->window_columns = w.ws_col;

    // NOTE(dgl): default command type (if nothing else found)
    ctx->command_type = Command_Type_Report;

    int cursor = 1;
    // NOTE(dgl): without args
    if (args_count == 1) {
        // TODO(dgl): tbd
    } else {
        while (cursor < args_count) {
            char *arg = args[cursor++];

            if (string_compare("-f", arg, 2) == 0) {
                if (cursor < args_count) {
                    char *filename = args[cursor++];
                    ctx->file = get_file_stats(arena, string_from_c_str(filename));
                    // TODO(dgl): if a file flag was provided and the file
                    // does not exist, we should create it.
                } else {
                    ctx->is_valid = false;
                }
            } else if (string_compare("sta", arg, 3) == 0) {
                ctx->command_type = Command_Type_Start;
                break;
            } else if (string_compare("con", arg, 3) == 0) {
                ctx->command_type = Command_Type_Continue;
                break;
            } else if (string_compare("sto", arg, 3) == 0) {
                ctx->command_type = Command_Type_Stop;
                break;
            } else if (string_compare("rep", arg, 3) == 0) {
                ctx->command_type = Command_Type_Report;
                break;
            } else if (string_compare("csv", arg, 3) == 0) {
                ctx->command_type = Command_Type_CSV;
                break;
#if DEBUG
            } else if (string_compare("gen", arg, 3) == 0) {
                ctx->command_type = Command_Type_Generate;
                break;
            } else if (string_compare("test", arg, 4) == 0) {
                ctx->command_type = Command_Type_Test;
                break;
#endif
            }
        }

        args += cursor;
        args_count -= cursor;
        cursor = 0;
    }

    if (ctx->is_valid) {
        PRINT_DEBUG("Running ttime with these args:\n\tfilename=%s\n", string_to_c_str(ctx->arena, ctx->file.filename));
        switch (ctx->command_type) {
            case Command_Type_Start: {
                commandline_parse_start_cmd(ctx, args, args_count);
                PRINT_DEBUG("\tcommand=start\n\ttask_id=%d\n\tannotation=%s\n", ctx->start.task_id, string_to_c_str(ctx->arena, ctx->start.annotation));
            } break;
            case Command_Type_Continue: {
                PRINT_DEBUG("\tcommand=continue\n");
            } break;
            case Command_Type_Stop: {
                PRINT_DEBUG("\tcommand=stop\n");
            } break;
            case Command_Type_Report: {
                commandline_parse_report_cmd(ctx, args, args_count);
                PRINT_DEBUG("\tcommand=report\n");
            } break;
            case Command_Type_CSV: {
                commandline_parse_csv_cmd(ctx, args, args_count);
                PRINT_DEBUG("\tcommand=csv\n");
            } break;
#if DEBUG
            case Command_Type_Test: {
                commandline_parse_test_cmd(ctx, args, args_count);
                PRINT_DEBUG("\tcommand=test\n");
            } break;
#endif
            default:
                // NOTE(dgl): something went wrong
                ctx->is_valid = false;
        }
    }

    if (!ctx->file.exists) {
        ctx->is_valid = false;
        LOG("File %s does not exist. To create it automatically use the -f flag", string_to_c_str(ctx->arena, ctx->file.filename));
    }
}

//
// Sorting
//

internal void
sort_bubble(Sort_Entry *first, int32 entry_count) {
    //
    // NOTE(casey): This is the O(n^2) bubble sort
    //
    for(uint32 outer = 0; outer < entry_count; ++outer) {
        bool32 list_is_sorted = true;
        for(uint32 inner = 0; inner < (entry_count - 1); ++inner) {
            Sort_Entry *entry_a = first + inner;
            Sort_Entry *entry_b = entry_a + 1;

            if(entry_a->sort_key > entry_b->sort_key) {
                Sort_Entry temp = *entry_b;
                *entry_b = *entry_a;
                *entry_a = temp;
                list_is_sorted = false;
            }
        }

        if(list_is_sorted) {
            break;
        }
    }
}

internal void
sort_radix(Sort_Entry *first, Sort_Entry *temp, int32 entry_count) {
    Sort_Entry *source = first;
    Sort_Entry *dest = temp;
    for(uint32 byte_index = 0; byte_index < 32; byte_index += 8) {
        uint32 sort_key_offset[256] = {};

        // NOTE(casey): First pass - count how many of each key
        for(uint32 index = 0; index < entry_count; ++index) {
            usize radix_value = source[index].sort_key;
            usize radix_piece = (radix_value >> byte_index) & 0xFF;
            ++sort_key_offset[radix_piece];
        }

        // NOTE(casey): Change counts to offsets
        uint32 total = 0;
        for(uint32 sort_key_index = 0;
            sort_key_index < array_count(sort_key_offset);
            ++sort_key_index) {
            uint32 count = sort_key_offset[sort_key_index];
            sort_key_offset[sort_key_index] = total;
            total += count;
        }

        // NOTE(casey): Second pass - place elements into the right location
        for(uint32 index = 0; index < entry_count; ++index) {
            usize radix_value = source[index].sort_key;
            usize radix_piece = (radix_value >> byte_index) & 0xFF;
            dest[sort_key_offset[radix_piece]++] = source[index];
        }

        Sort_Entry *swap_temp = dest;
        dest = source;
        source = swap_temp;
    }
}

// TODO(dgl): Help command

//
// MAIN
//

int main(int argc, char** argv) {
    usize begin_cycles = get_rdtsc();

#if DEBUG
    void *base_address = cast(void *, terabytes(2));
#else
    void *base_address = 0;
#endif
    usize memory_size = megabytes(128);
    uint8 *memory_base = cast(uint8 *, mmap(base_address, memory_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0));
    Mem_Arena permanent_arena = {};
    Mem_Arena transient_arena = {};

    // TODO(dgl): optimize memory for large files
    mem_arena_init(&permanent_arena, memory_base, megabytes(64), "permanent_arena");
    mem_arena_init(&transient_arena, memory_base + permanent_arena.size, memory_size - permanent_arena.size, "transient_arena");

    struct timespec start = get_wall_clock();
    Commandline cmdline = {};
    commandline_parse(&permanent_arena, &cmdline, argv, argc);

    if (cmdline.is_valid) {
        switch(cmdline.command_type) {
            // TODO(dgl): almost equal with continue. Can be reduced and refactored
            case Command_Type_Start: {
                Buffer buffer = allocate_filebuffer(&permanent_arena, &cmdline.file, 0);
                read_entire_file(&transient_arena, &cmdline.file, &buffer);

                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);
                usize last_line_offset = get_last_line_offset(&tokenizer);
                Entry last_entry = parse_entry_at(&tokenizer, last_line_offset);

                if (!tokenizer.has_error) {
                    if (last_entry.end.year == 0) {
                        LOG("Time interval currently active with annotation: %s", string_to_c_str(&transient_arena, last_entry.annotation));
                    } else {
                        Entry new_entry = {};
                        new_entry.begin = get_timestamp();
                        new_entry.task_id = cmdline.start.task_id;
                        new_entry.annotation = cmdline.start.annotation;
                        Buffer entry_buffer = entry_to_buffer(&transient_arena, &new_entry);
                        write_entire_file(&transient_arena, &cmdline.file, 2, &buffer, &entry_buffer);
                    }
                } else {
                    LOG("Tokenizer error: %s", tokenizer.error_msg);
                }
            } break;
            case Command_Type_Continue: {
                Buffer buffer = allocate_filebuffer(&permanent_arena, &cmdline.file, 0);
                read_entire_file(&transient_arena, &cmdline.file, &buffer);

                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);
                usize last_line_offset = get_last_line_offset(&tokenizer);
                Entry last_entry = parse_entry_at(&tokenizer, last_line_offset);

                if (!tokenizer.has_error) {
                    if (last_entry.end.year == 0) {
                        LOG("Time interval currently active with annotation: %s", string_to_c_str(&transient_arena, last_entry.annotation));
                    } else {
                        Entry new_entry = {};
                        new_entry.begin = get_timestamp();
                        new_entry.task_id = last_entry.task_id;
                        new_entry.annotation = last_entry.annotation;

                        // NOTE(dgl): cleanup buffer end
                        //buffer.data_count = get_end_of_file_offset(&buffer);;

                        Buffer entry_buffer = entry_to_buffer(&transient_arena, &new_entry);
                        write_entire_file(&transient_arena, &cmdline.file, 2, &buffer, &entry_buffer);
                    }
                } else {
                    LOG("Tokenizer error: %s", tokenizer.error_msg);
                }
            } break;
            case Command_Type_Stop: {
                Buffer buffer = allocate_filebuffer(&permanent_arena, &cmdline.file, 0);
                read_entire_file(&transient_arena, &cmdline.file, &buffer);

                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);
                usize last_line_offset = get_last_line_offset(&tokenizer);
                Entry last_entry = parse_entry_at(&tokenizer, last_line_offset);

                if (!tokenizer.has_error) {
                    if (last_entry.end.year != 0) {
                        LOG("No time interval active");
                    } else {
                        last_entry.end = get_timestamp();
                        Buffer entry_buffer = entry_to_buffer(&transient_arena, &last_entry);
                        // NOTE(dgl): the last entry starts at the newline but to write the entry we must start one byte before.
                        // However what if we
                        if (*(cast(char *, buffer.data) + last_line_offset - 1) == '\n') {
                            last_line_offset--;
                        }

                        // NOTE(dgl): we try to overwrite the last entry in our buffer with the updated info.
                        // if the space in our buffer is too small we write the data that fits and
                        // set an offset in our entry_buffer so that we append only the missing bytes
                        // to the file.
                        usize missing_bytes = buffer_merge_at(&buffer, &entry_buffer, last_line_offset);
                        if (missing_bytes > 0) {
                            usize written = entry_buffer.data_count - missing_bytes;
                            entry_buffer.data += written;
                            entry_buffer.data_count -= written;
                            write_entire_file(&transient_arena, &cmdline.file, 2, &buffer, &entry_buffer);
                        } else {
                            write_entire_file(&transient_arena, &cmdline.file, 1, &buffer);
                        }
                    }
                } else {
                    LOG("Tokenizer error: %s", tokenizer.error_msg);
                }
            } break;
            case Command_Type_Report: {
                Buffer buffer = allocate_filebuffer(&permanent_arena, &cmdline.file, 0);
                read_entire_file(&transient_arena, &cmdline.file, &buffer);
                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);

                usize from_sentinel = datetime_to_epoch(&cmdline.report.from);
                LOG_DEBUG("From sentinel %lu", from_sentinel);
                usize to_sentinel = datetime_to_epoch(&cmdline.report.to);
                LOG_DEBUG("To sentinel %lu", to_sentinel);


                int32 entry_count = 0;
                int32 max_entry_count = 100;
                EntryMeta *entries = mem_arena_push_array(&transient_arena, EntryMeta, max_entry_count);
                while(!tokenizer.has_error && tokenizer.input.length > 0) {
                    EntryMeta meta = parse_entry_meta(&tokenizer);
                    eat_all_whitespace(&tokenizer);

                    if (meta.begin > from_sentinel && meta.begin < to_sentinel) {
                        if (entry_count == max_entry_count) {
                            int current_count = max_entry_count;
                            max_entry_count *= 2;
                            entries = mem_arena_resize_array(&transient_arena, EntryMeta, entries, current_count, max_entry_count);
                        }

                        entries[entry_count++] = meta;
                    }
                }

                if (tokenizer.has_error) {
                    LOG("Tokenizer error: %s", tokenizer.error_msg);
                }


                // NOTE(dgl): use different memory layout if too slow. @performance
                Sort_Entry *sort_entries = mem_arena_push_array(&permanent_arena, Sort_Entry, entry_count);
                for (int32 index = 0; index < entry_count; ++index) {
                    Sort_Entry *sort = sort_entries + index;
                    EntryMeta *meta = entries + index;

                    // NOTE(dgl): @performance we could also use an offset of now to be able to use 32bit integers.
                    // This would decrease the passes on the radix sort. It should be fine with the offset, because
                    // the seconds of one year do not surpass a 32bit integer

                    assert(from_sentinel < meta->begin, "begin cannot be in the future, for sorting");
                    sort->sort_key = cast(uint32, meta->begin - from_sentinel);
                    sort->index = index;
                }

                Mem_Temp_Arena tmp_arena = mem_arena_begin_temp(&transient_arena);
                {
                    Sort_Entry *sort_memory = mem_arena_push_array(tmp_arena.arena, Sort_Entry, entry_count);
                    sort_radix(sort_entries, sort_memory, entry_count);
                }
                mem_arena_end_temp(tmp_arena);

#if DEBUG
                 for (int32 index = 0; index < entry_count - 1; ++index) {
                    Sort_Entry *a = sort_entries + index;
                    Sort_Entry *b = a + 1;

                    assert(a->sort_key <= b->sort_key, "Array not correctly sorted at index %d - a: %d, b: %d", index, a->sort_key, b->sort_key);
                }
#endif

                usize total_seconds = 0;
                usize daily_seconds = 0;
                int32 last_day = 0;
                // TODO(dgl): use info from entry array to determine what is printed
                int32 print_flags = Print_Timezone;
                for (int32 index = 0; index < entry_count; ++index) {
                    EntryMeta *meta = entries + sort_entries[index].index;

                    Entry entry = parse_entry_from_meta(&tokenizer, meta);

                    if (report_tag_matches(&cmdline, &entry)) {
                        if (entry.end.year == 0) { entry.end = get_timestamp(); }
                        usize end = datetime_to_epoch(&entry.end);
                        assert(meta->begin < end, "End time cannot be larger than begin time");
                        usize difftime = end - meta->begin;

                        total_seconds += difftime;

                        if (entry.begin.day != last_day && last_day > 0) {
                            print_datetime(&transient_arena, print_flags, "\t\t%th hs\n", daily_seconds);
                            daily_seconds = 0;
                        }

                        if (daily_seconds == 0) {
                            print_datetime(&transient_arena, print_flags, "%td\t", entry.begin);
                        }

                        daily_seconds += difftime;
                        last_day = entry.begin.day;

                        print_datetime(&transient_arena, print_flags, "\n\t%tt - %tt => \t %th hs", entry.begin, entry.end, difftime, entry.annotation);
                    }
                }

                print_datetime(&transient_arena, print_flags, "\t\t%th hs\n\n", daily_seconds);
                print_datetime(&transient_arena, print_flags, "Total hours: %th hs\n", total_seconds);
            } break;
            case Command_Type_CSV: {
                LOG("Not yet implemented");
            } break;
    #if DEBUG
            case Command_Type_Generate: {
                LOG("Not yet implemented");
            } break;
            case Command_Type_Test: {
                String annotation = string_from_c_str("das ist ein test mit @test @test2 und @foobar");
                Entry entry = {};
                entry.annotation = annotation;

                LOG_DEBUG("Tag match: %d", report_tag_matches(&cmdline, &entry));
            } break;
    #endif
            default:
                LOG_DEBUG("Command type %d not implemented", cmdline.command_type);
        }
    } else {
        LOG("Invalid arguments");
    }

    usize end_cycles = get_rdtsc();
    struct timespec end = get_wall_clock();
    LOG("Executed in %f ms (%lu cycles)", get_ms_elapsed(start, end), end_cycles - begin_cycles);
    return 0;
}
