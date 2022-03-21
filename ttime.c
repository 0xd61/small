/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Time (execute with ttime)
Author: Daniel Glinka

This tool uses a time.txt file to track the time and run reports on it. In the future this should work with todo.sh

Time is written in ISO 8601 format (2022-03-08T01:38:00+00:00)
We can have one line per time entry and multiple columns devided by |. After the taskID everything is considered as annotation.

Example:
2022-03-08T01:38:00+00:00 | 2022-03-08T01:38:00+00:00 | taskID (-1 if no task specified) | other annotations


// TODO(dgl): refactor tokenizer
create a function peek_next_character and eat_next_character
then use these directly in parse_integer. Currently we parse the whole file in 110ms with O3.
We always know what characters we are expecting.
Therefore we can parse integers etc. directly and don't need to create a token for them.
In theory this should be faster and simpler.
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

#include "helpers/types.h"
#include "helpers/string.c"

typedef struct {
    void  *data;
    usize  data_count;
    usize  cap;
} Buffer;

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

typedef struct {

} Command_Report;

typedef struct {

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
        // TODO(dgl): proper writing to file with atomic file save
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
internal void
eat_next_character(Tokenizer *tokenizer) {
    if (!tokenizer->has_error && tokenizer->input.length > 0) {
        LOG_DEBUG("Eaten character %c (%d)", *tokenizer->input.text, *tokenizer->input.text);
        ++tokenizer->input.data;
        --tokenizer->input.length;
        ++tokenizer->column;
        if (*tokenizer->input.text == '\n') {
            ++tokenizer->line;
            tokenizer->column = 0;
        }
    }
}

internal char
peek_next_character(Tokenizer *tokenizer) {
    char result = 0;
    if (!tokenizer->has_error && tokenizer->input.length > 0) {
        result = *tokenizer->input.text;
    }

    LOG_DEBUG("Peeked character %c (%d) - Buffered: %.*s", result, result, DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);

    return result;
}

internal void
eat_all_whitespace(Tokenizer *tokenizer) {
    while(peek_next_character(tokenizer) == ' ') {
        eat_next_character(tokenizer);
    }
}

internal bool32
is_numeric(char c) {
    bool32 result = false;

    if (c >= '0' && c <= '9') {
        result = true;
    }

    return result;
}

// internal Token
// get_token(Tokenizer *tokenizer) {
//     Token result = {};

//     if (!tokenizer->has_error) {
//         eat_all_whitespace(tokenizer);

//         result.text = tokenizer->input;
//         result.text_length = 1;
//         result.line = tokenizer->line;
//         char c = tokenizer->at[0];
//         advance(tokenizer, 1);

//         switch(c) {
//             // NOTE(dgl): decrement tokenizer to hit EOF again if someone calls it
//             case 0   : { result.type = Token_Type_EndOfFile; } break;
//             case '|' : { result.type = Token_Type_Divider; } break;
//             case '+' : { result.type = Token_Type_Plus; } break;
//             case '-' : { result.type = Token_Type_Minus; } break;
//             case ':' : { result.type = Token_Type_Colon; } break;
//             case '@' : { result.type = Token_Type_At; } break;
//             case '\n': { result.type = Token_Type_Newline; ++tokenizer->line; } break;
//             default: {
//                 if (is_numeric(c)) {
//                     result.type = Token_Type_Integer;
//                     while (is_numeric(tokenizer->at[0])) {
//                         advance(tokenizer, 1);
//                     }
//                     result.text_length = cast(int32, tokenizer->input - result.text);
//                 } else {
//                     result.type = Token_Type_String;
//                     while (tokenizer->at[0] > ' ' && !is_numeric(tokenizer->at[0])) {
//                         advance(tokenizer, 1);
//                     }
//                     result.text_length = cast(int32, tokenizer->input - result.text);
//                 }
//             }
//         }
//     }

//     LOG_DEBUG("Parsed token: type %d, text %.*s, length %d", result.type, result.text_length, result.text, result.text_length);

//     return result;
// }

// internal Token
// peek_token(Tokenizer *tokenizer) {
//     Tokenizer tokenizer2 = *tokenizer;

//     LOG_DEBUG("Peeking next token");
//     Token result = get_token(&tokenizer2);
//     return result;
// }

internal String
parse_string_line(Tokenizer *tokenizer) {
    LOG_DEBUG("Parsing string line from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    LOG_DEBUG("Parsing integer from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    LOG_DEBUG("Parsing date from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
    Datetime result = {};
    char c = 0;

    result.year = parse_integer(tokenizer);
    if (result.year < 1000 || result.year >= 10000) {
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
    LOG_DEBUG("Parsing time from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    LOG_DEBUG("Parsing timezone from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    LOG_DEBUG("Parsing datetime from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    LOG_DEBUG("Parsing entry from %.*s", DEBUG_TOKENIZER_PREVIEW, tokenizer->input.text);
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
    printf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d:%02d\n", timestamp->year,
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

internal usize
entry_length(Entry *entry) {
    // example: 2022-03-08T01:38:00:00+00:00:00 | 2022-03-08T01:38:00:00+00:00:00 | taskID (-1 if no task specified) | other annotations
    usize result = 0;
    usize max_datetime_len = string_length("2022-03-08T01:38:00:00+00:00:00");
    usize max_task_id_len = string_length("2147483647");
    usize max_annotation_len = entry->annotation.length;

    usize max_divider_len = string_length(" | ");
    result = max_datetime_len + max_divider_len + max_datetime_len + max_divider_len + max_task_id_len + max_divider_len + max_annotation_len;

    return result + 1;  // +1 because of \n
}


internal void
write_entry_to_buffer(Entry *entry, Buffer *buffer) {
    usize len = entry_length(entry);
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

int main(int argc, char** argv) {
    struct timespec start = get_wall_clock();
    Commandline cmdline = {};
    commandline_parse(&cmdline, argv, argc);

    // TODO(dgl): @temporary Cleanup the commands! This is currently exploration code and will get cleaned later.
    if (cmdline.is_valid) {
        switch(cmdline.command_type) {
            case Command_Type_Start: {
                Entry new_entry = {};
                new_entry.begin = get_timestamp();
                new_entry.task_id = cmdline.start.task_id;
                new_entry.annotation = cmdline.start.annotation;
                usize entry_len = entry_length(&new_entry);

                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = {};
                buffer.data_count = file.filesize;
                buffer.cap = file.filesize + entry_len;


#if DEBUG
    void *base_address = cast(void *, terabytes(2));
#else
    void *base_address = 0;
#endif
                buffer.data = mmap(base_address, buffer.cap, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

                read_entire_file(&file, &buffer);

                new_entry.buffer_offset = get_end_of_file_offset(&buffer);
                LOG_DEBUG("end_of_file_offset %ld, buffer.data_count %ld", new_entry.buffer_offset, buffer.data_count);
                assert(new_entry.buffer_offset <= buffer.data_count, "asdasd");

                usize last_line_offset = get_last_line_offset(&buffer);
                LOG_DEBUG("last_line_offset: %lu", last_line_offset);
                Tokenizer tokenizer = {};
                tokenizer.input.data = buffer.data + last_line_offset;
                tokenizer.input.length = buffer.data_count - last_line_offset;
                tokenizer.input.cap = tokenizer.input.length;

                Entry entry = parse_entry(&tokenizer);

                if (!tokenizer.has_error) {
                    print_timestamp(&entry.begin);
                    print_timestamp(&entry.end);
                    printf("%d\n", entry.task_id);
                    printf("%s\n", string_to_c_str(entry.annotation));

                    if (entry.end.year == 0) {
                        LOG("Time interval currently active with annotation: %s", string_to_c_str(entry.annotation));
                    } else {
                        write_entry_to_buffer(&new_entry, &buffer);
                        write_entire_file(&file, &buffer);
                    }
                }
            } break;
            case Command_Type_Stop: {
                usize entry_len = string_length("2022-03-08T01:38:00:00+00:00:00") * 2;

                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = {};
                buffer.data_count = file.filesize;
                buffer.cap = file.filesize + entry_len;

#if DEBUG
    void *base_address = cast(void *, terabytes(2));
#else
    void *base_address = 0;
#endif
                buffer.data = mmap(base_address, buffer.cap, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

                read_entire_file(&file, &buffer);

                usize last_line_offset = get_last_line_offset(&buffer);
                Tokenizer tokenizer = {};
                tokenizer.input.data = buffer.data + last_line_offset;
                tokenizer.input.length = buffer.data_count - last_line_offset;
                tokenizer.input.cap = tokenizer.input.length;

                Entry entry = parse_entry(&tokenizer);
                entry.buffer_offset = last_line_offset;

                if (!tokenizer.has_error) {
                    if (entry.end.year != 0) {
                        LOG("No time interval active");
                    } else {
                        entry.end = get_timestamp();
                        print_timestamp(&entry.begin);
                        print_timestamp(&entry.end);
                        PRINT_DEBUG("%d\n", entry.task_id);
                        PRINT_DEBUG("%s", string_to_c_str(entry.annotation));
                        write_entry_to_buffer(&entry, &buffer);
                        write_entire_file(&file, &buffer);
                    }
                }
            } break;
            case Command_Type_Report: {
                File_Stats file = get_file_stats(cmdline.filename);
                Buffer buffer = {};
                buffer.data_count = file.filesize;
                buffer.cap = file.filesize;
#if DEBUG
    void *base_address = cast(void *, terabytes(2));
#else
    void *base_address = 0;
#endif
                buffer.data = mmap(base_address, buffer.cap, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

                read_entire_file(&file, &buffer);
                Tokenizer tokenizer = {};
                tokenizer.input.data = buffer.data;
                tokenizer.input.length = buffer.data_count;
                tokenizer.input.cap = tokenizer.input.length;

                // TODO(dgl): YAY! 500.000 Lines get parsed in about 100ms on my machine.
                while(!tokenizer.has_error) {
                    Entry entry = parse_entry(&tokenizer);
                }

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

    struct timespec end = get_wall_clock();
    LOG("Executed in %f ms", get_ms_elapsed(start, end));
    return 0;
}
