/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Time (execute with ttime)
Author: Daniel Glinka

This tool uses a time.txt file to track the time and run reports on it. In the future this should work with todo.sh

Time is written in ISO 8601 format (2022-03-08T01:38:00+00:00)
We can have one line per time entry and multiple columns devided by |. After the taskID everything is considered as annotation.

Example:
2022-03-08T01:38:00+00:00 | 2022-03-08T01:38:00+00:00 | taskID (-1 if no task specified) | other annotations

TODO(dgl):
    - segfault if no annotation at start
    - Better parsing:
        - Only parse begin date and buffer pos and continue to next line.
        - If begin is in our range, put the begin date and pos in an array.
        - Sort the array
        - go through the array and show the datetime differences
    - Better commandline errors (currently we get segfaults)

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#define DEBUG 1
#define DEBUG_TOKENIZER_PREVIEW 20
#define MAX_FILENAME_SIZE 4096

#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <x86intrin.h>


#include "helpers/types.h"
#include "helpers/string.c"

typedef struct {
    void  *data;
    usize  data_count;
    usize  cap;
} Buffer;

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
    char   *error_msg;
} Tokenizer;

typedef struct {
    Datetime   begin;
    Datetime   end;
    int32      task_id;
    int32      line;
    String     annotation; /* TODO(dgl): only load those on demand? */
    usize      buffer_offset;
} Entry;

typedef enum {
    Command_Type_Noop,
    Command_Type_Start,
    Command_Type_Stop,
    Command_Type_Continue,
    Command_Type_Report,
    Command_Type_CSV,
#if DEBUG
    Command_Type_Generate,
#endif
} Command_Type;

typedef struct {
    int32   task_id;
    String  annotation;
} Command_Start;

typedef struct {
} Command_Stop;

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
    String     *filter;
} Command_Report;

typedef struct {
    Command_Report report;
    bool32         heading;
} Command_CSV;

typedef struct {
    Command_Type  command_type;
    bool32        is_valid;
    String        filename;
    union {
        Command_Start  start;
        Command_Stop   stop;
        Command_Report report;
        Command_CSV    csv;
    };
} Commandline;

typedef struct {
    String  filename;
    usize   filesize;
} File_Stats;

//
//
//
internal usize max_entry_length(String annotation);

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

internal inline real32
get_ms_elapsed(struct timespec start, struct timespec end)
{
    real32 result = (real32)(end.tv_sec - start.tv_sec) +
                    ((real32)(end.tv_nsec - start.tv_nsec) * 1e-6f);
    return(result);
}

usize get_rdtsc(){
    return __rdtsc();
}

//
// File/Buffer
//

internal Buffer
allocate_filebuffer(File_Stats *file, usize padding) {
    Buffer result = {};
    result.data_count = 0;
    result.cap = file->filesize + padding;

#if DEBUG
    void *base_address = cast(void *, terabytes(2));
#else
    void *base_address = 0;
#endif
    result.data = mmap(base_address, result.cap, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    return result;
}


internal File_Stats
get_file_stats(String filename) {
    File_Stats result = {};

    struct stat file_stat = {};
    int err = stat(filename.text, &file_stat);
    if (err == 0) {
        result.filename = filename;
        result.filesize = cast(usize, file_stat.st_size);
    } else {
        LOG("Failed to get file stats for file %s: err %d", string_to_c_str(filename), err);
    }

    return result;
}

internal void
read_entire_file(File_Stats *file, Buffer *buffer) {
    int fd = open(file->filename.text, O_RDONLY);
    if (fd) {
        if (file->filesize > 0) {
            lseek(fd, 0, SEEK_SET);
            ssize_t res = read(fd, buffer->data, buffer->cap);
            if (res >= 0) {
                buffer->data_count = cast(usize, res);
            } else {
                buffer->data_count = 0;
                LOG("Failed to read file %s", string_to_c_str(file->filename));
            }
        } else {
            LOG_DEBUG("File is empty: %s", string_to_c_str(file->filename));
        }
        close(fd);
    } else {
        LOG("Could not open file: %s", string_to_c_str(file->filename));
    }
}

// NOTE(dgl): offset before last line of file (line containing text)
// TODO(dgl): replace by tokenizer
internal usize
get_last_line_offset(Buffer *buffer) {
    usize result = 0;
    char *input = cast(char *, buffer->data);
    if (input) {
        char *cursor = input + buffer->data_count;
        bool32 found_text = false;
        while (cursor > input &&
              (*cursor != '\n' || !found_text)) {
            if (*cursor > ' ') {
                found_text = true;
            }
            cursor--;
        }

        assert(cursor > cast(char *, buffer->data), "Buffer overflow. Cursor cannot be larger than the data buffer");
        result = cast(usize, cursor - cast(char *, buffer->data)) + 1; // +1 to go to character after the \n
    }

    return result;
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
        ++cursor; // TODO(dgl): is there a better way for this loop? We go back one character too much...

        assert(cursor > cast(char *, buffer->data), "Buffer overflow. Cursor cannot be larger than the data buffer");
        result = cast(usize, cursor - cast(char *, buffer->data)) + 1; // +1 to go one byte after last text character
    }

    return result;
}

internal void
write_entire_file(File_Stats *file, Buffer *buffer) {
    char tmp_filename[MAX_FILENAME_SIZE];
    assert(file->filename.length + 2 < MAX_FILENAME_SIZE, "Filename too long. Increase MAX_FILENAME_SIZE.");
    string_copy(file->filename.text, file->filename.length, tmp_filename, MAX_FILENAME_SIZE);

    tmp_filename[file->filename.length] = '~';
    tmp_filename[file->filename.length + 1] = 0;

    int fd = open(tmp_filename, O_WRONLY | O_CREAT, 0644);
    if (fd) {
        ssize_t res = write(fd, buffer->data, buffer->data_count);

        if (res < 0) {
            LOG("Failed writing to file %s with error: %d", tmp_filename, errno);
        } else {
            LOG_DEBUG("Written %ld bytes of %ld bytes to %s", res, buffer->data_count, tmp_filename);
        }
    }
    close(fd);

    if(rename(tmp_filename, file->filename.text) != 0) {
        LOG("Failed to move content from temporary file %s to %s", tmp_filename, string_to_c_str(file->filename));
    }
}

internal void
write_entry_to_buffer(Entry *entry, Buffer *buffer) {
    usize len = max_entry_length(entry->annotation);
    assert(entry->buffer_offset + len <= buffer->cap, "Buffer overflow. Please increase the buffer size before writing the entry");
    buffer->data_count = entry->buffer_offset;

    // TODO(dgl): we overwrite the buffer data, which is set in the annotation. We need to cache this annotation line before memset...
    char *annotation_cache = 0;
    if (entry->annotation.data) {
        annotation_cache = cast(char *, malloc(entry->annotation.length + 1));
        string_copy(entry->annotation.text, entry->annotation.length, annotation_cache, entry->annotation.length + 1);
        LOG_DEBUG("Annotation cache: %s", annotation_cache);
    }
    // NOTE(dgl): we set the dest one character behind the new token to ensure there is a newline
    char *dest = buffer->data + buffer->data_count - 1;
    LOG_DEBUG("%s", dest);
    memset(dest, 0, len + 1);
    *dest++ = '\n';

    int printed = sprintf(dest, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d | ",
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
    dest += printed;
    buffer->data_count += printed;
    if (entry->end.year > 0) {
        printed = sprintf(dest, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d | ",
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
       dest += printed;
       buffer->data_count += printed;
   } else {
       printed = sprintf(dest, " | ");
       dest += printed;
       buffer->data_count += printed;
   }

    printed = sprintf(dest, "%d | ", entry->task_id);
    dest += printed;
    buffer->data_count += printed;

    if (entry->annotation.data) {
        printed = sprintf(dest, "%s\n", annotation_cache);
        dest += printed;
        buffer->data_count += printed;
    } else {
        printed = sprintf(dest, "\n");
        dest += printed;
        buffer->data_count += printed;
    }

    assert(buffer->data_count <= buffer->cap, "Buffer overflow");
}

//
// Tokenizer/Parser
//

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
        LOG("Parsing error at line %d, column %d: %s", tokenizer->line, tokenizer->column + 1, msg);
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

internal inline char
peek_character(Tokenizer *tokenizer, uint32 lookahead) {
    char result = 0;
    if (!tokenizer->has_error && tokenizer->input.length > 0) {
        assert(lookahead < tokenizer->input.length, "Lookahead cannot be larger than input");
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

internal inline void
eat_all_whitespace(Tokenizer *tokenizer) {
    bool32 is_comment = false;
    char c = peek_next_character(tokenizer);
    char next_c = peek_character(tokenizer, 1);

    // TODO(dgl): also allow comments at the end of the line or with whitespace at the beginning...
    if (c == '/' && next_c == '/') {
        is_comment = true;
    }

    while(is_comment || c == ' ') {
        if (is_comment && c == '\n') { is_comment = false; }
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

internal Entry
parse_entry(Tokenizer *tokenizer) {
    // LOG_DEBUG("Parsing entry from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Entry result = {};

    // TODO(dgl): @temporary this file_pos is only valid if we use the get_last_line(). Otherwise
    // it is the position of the current file cursor. However this variable is the starting point
    // of the entry line we are currently parsing.
    //result.file_offset = file->file_offset + (tokenizer->input - file->content);
    //LOG_DEBUG("File offset %lu", result.file_offset);

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
    result.buffer_offset = offset;
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

    // NOTE(dgl): fixing month
    int32 month_index = (datetime->month - 3) % 12;
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


internal int32
datetime_compare(Datetime *a, Datetime *b) {
    int32 result = 0;
    assert(a->offset_hour == 0 && a->offset_minute == 0 && a->offset_second == 0, "Datetime is not UTC");
    assert(a->offset_hour == 0 && a->offset_minute == 0 && b->offset_second == 0, "Datetime is not UTC");

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
        ctx->start.annotation.data = mmap(0, required_memory, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

        char *dest = ctx->start.annotation.text;
        cursor = start_count;
        while(cursor < args_count) {
            char *annotation = args[cursor++];
            usize length = string_length(annotation);
            string_copy(annotation, length, dest, length + 1);
            dest += length;
            *dest++ = ' ';
        }

        ctx->start.annotation.text[required_memory] = 0;
    }
}

internal void
commandline_parse_stop_cmd(Commandline *ctx, char** args, int args_count) {

}

internal void
commandline_parse_report_cmd(Commandline *ctx, char** args, int args_count) {
    int32 cursor = 0;

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
        } else {
            ctx->report.type = Report_Type_Today;
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
commandline_parse(Commandline *ctx, char** args, int args_count) {
    ctx->is_valid = true;
    ctx->filename = string_from_c_str("~/time.txt");

    // NOTE(dgl): without args
    if (args_count == 1) {
        ctx->command_type = Command_Type_Report;
    } else {
        int cursor = 1;
        char *command = args[cursor++];

        if (string_compare("sta", command, 3) == 0) {
            ctx->command_type = Command_Type_Start;
        } else if (string_compare("sto", command, 3) == 0) {
            ctx->command_type = Command_Type_Stop;
        } else if (string_compare("rep", command, 3) == 0) {
            ctx->command_type = Command_Type_Report;
        } else if (string_compare("csv", command, 3) == 0) {
            ctx->command_type = Command_Type_CSV;
#if DEBUG
        } else if (string_compare("gen", command, 3) == 0) {
            ctx->command_type = Command_Type_Generate;
#endif
        } else {
            --cursor;
            ctx->command_type = Command_Type_Report;
            LOG_DEBUG("Unknown command: %s. Falling back to the report command", command);
        }

        args += cursor;
        args_count -= cursor;
        cursor = 0;
        if(ctx->is_valid) {
            while(cursor < args_count) {
                char *arg = args[cursor++];
                if (string_compare("-f", arg, 2) == 0) {
                    ctx->filename = string_from_c_str(args[cursor++]);
                }
            }

            PRINT_DEBUG("Running ttime with these args:\n\tfilename=%s\n", string_to_c_str(ctx->filename));
            switch (ctx->command_type) {
                case Command_Type_Start: {
                    commandline_parse_start_cmd(ctx, args, args_count);
                    PRINT_DEBUG("\tcommand=start\n\ttask_id=%d\n\tannotation=%s\n", ctx->start.task_id, string_to_c_str(ctx->start.annotation));
                } break;
                case Command_Type_Stop: {
                    commandline_parse_stop_cmd(ctx, args, args_count);
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
                default:
                    // NOTE(dgl): something went wrong
                    ctx->is_valid = false;
            }
        }
    }
}

// TODO(dgl): Help command

//
// MAIN
//

int main(int argc, char** argv) {
    usize begin_cycles = get_rdtsc();

    struct timespec start = get_wall_clock();
    Commandline cmdline = {};
    commandline_parse(&cmdline, argv, argc);

    if (cmdline.is_valid) {
        // TODO(dgl): check if the time.txt file exists!
        switch(cmdline.command_type) {
            case Command_Type_Start: {
                Entry new_entry = {};
                new_entry.begin = get_timestamp();
                new_entry.task_id = cmdline.start.task_id;
                new_entry.annotation = cmdline.start.annotation;
                usize entry_len = max_entry_length(new_entry.annotation);

                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = allocate_filebuffer(&file, entry_len);
                read_entire_file(&file, &buffer);

                new_entry.buffer_offset = get_end_of_file_offset(&buffer);

                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);
                usize last_line_offset = get_last_line_offset(&buffer);
                Entry last_entry = parse_entry_at(&tokenizer, last_line_offset);

                if (!tokenizer.has_error) {
                    if (last_entry.end.year == 0) {
                        LOG("Time interval currently active with annotation: %s", string_to_c_str(last_entry.annotation));
                    } else {
                        write_entry_to_buffer(&new_entry, &buffer);
                        write_entire_file(&file, &buffer);
                    }
                }
            } break;
            case Command_Type_Stop: {
                usize entry_len = max_entry_length(string_from_c_str(""));

                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = allocate_filebuffer(&file, entry_len);
                read_entire_file(&file, &buffer);

                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);
                usize last_line_offset = get_last_line_offset(&buffer);
                Entry last_entry = parse_entry_at(&tokenizer, last_line_offset);

                if (!tokenizer.has_error) {
                    if (last_entry.end.year != 0) {
                        LOG("No time interval active");
                    } else {
                        last_entry.end = get_timestamp();
                        write_entry_to_buffer(&last_entry, &buffer);
                        write_entire_file(&file, &buffer);
                    }
                }
            } break;
            case Command_Type_Report: {
                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = allocate_filebuffer(&file, 0);
                read_entire_file(&file, &buffer);
                Tokenizer tokenizer = {};
                fill_tokenizer(&tokenizer, &buffer);

                usize from_sentinel = datetime_to_epoch(&cmdline.report.from);
                LOG_DEBUG("From sentinel %lu", from_sentinel);
                usize to_sentinel = datetime_to_epoch(&cmdline.report.to);
                LOG_DEBUG("To sentinel %lu", to_sentinel);

                LOG_DEBUG("Matching Timestamps:");
                usize total_seconds = 0;
                while(!tokenizer.has_error && tokenizer.input.length > 0) {
                    Entry entry = parse_entry(&tokenizer);
                    eat_all_whitespace(&tokenizer);

                    usize begin = datetime_to_epoch(&entry.begin);

                    if (begin < from_sentinel || begin > to_sentinel) {
                        continue;
                    }

                    usize end = datetime_to_epoch(&entry.end);
                    assert(begin < end, "End time cannot be larger than begin time");
                    usize difftime = end - begin;
                    total_seconds += difftime;

                    int32 hours = cast(int32, difftime / 3600);
                    difftime = difftime - (hours * 3600);
                    int32 minutes = cast(int32, difftime / 60);
                    difftime = difftime - (minutes * 60);
                    int32 seconds = cast(int32, difftime);

                    printf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d - "
                           "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d => "
                           "%02d:%02d:%02d hs\n",
                          entry.begin.year,
                          entry.begin.month,
                          entry.begin.day,
                          entry.begin.hour,
                          entry.begin.minute,
                          entry.begin.second,
                          entry.begin.offset_sign ? '-' : '+',
                          entry.begin.offset_hour,
                          entry.begin.offset_minute,
                          entry.begin.offset_second,
                          entry.end.year,
                          entry.end.month,
                          entry.end.day,
                          entry.end.hour,
                          entry.end.minute,
                          entry.end.second,
                          entry.end.offset_sign ? '-' : '+',
                          entry.end.offset_hour,
                          entry.end.offset_minute,
                          entry.end.offset_second,
                          hours,
                          minutes,
                          seconds);
                }

                int32 hours = cast(int32, total_seconds / 3600);
                total_seconds = total_seconds - (hours * 3600);
                int32 minutes = cast(int32, total_seconds / 60);
                total_seconds = total_seconds - (minutes * 60);
                int32 seconds = cast(int32, total_seconds);

                printf("TOTAL HOURS: %02d:%02d:%02d hs\n", hours, minutes, seconds);

            } break;
            case Command_Type_CSV: {

            } break;
    #if DEBUG
            case Command_Type_Generate: {

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
