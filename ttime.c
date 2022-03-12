/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Time (execute with ttime)
Author: Daniel Glinka

This tool uses a time.txt file to track the time and run reports on it. In the future this should work with todo.sh

Time is written in ISO 8601 format (2022-03-08T01:38:00+00:00)
We can have one line per time entry and multiple columns devided by |. After the taskID everything is considered as annotation. Multiple annotations are also devided by |.

Example:
2022-03-08T01:38:00+00:00 | 2022-03-08T01:38:00+00:00 | taskID (-1 if no task specified) | other annotations

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#define DEBUG 1

#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>


#include "helpers/types.h"
#include "helpers/string.c"

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

typedef enum {
    Token_Type_Invalid,
    Token_Type_Divider,
    Token_Type_Date,
    Token_Type_Time,
    Token_Type_DateTimeDivider,
    Token_Type_Sign,
    Token_Type_Integer,
    Token_Type_String,
    Token_Type_Newline,
    Token_Type_EndOfFile,
} Token_Type;

typedef struct {
    char       *text;
    int32       text_length;
    int32       line;
    Token_Type  type;
} Token;

typedef struct {
    char  *input;
    int32  input_count;
    char   at[2];
    int32  line;

    Token_Type last_token_type;

    bool32 has_error;
    char  *error_msg;
} Tokenizer;

typedef struct {
    Datetime   begin;
    Datetime   end;
    int32      task_id;
    int32      line;
    char      *annotation; /* TODO(dgl): only load those on demand? */
} Entry;

typedef struct {
    Command_Type  command_type;
    char         *params; // TODO(dgl): @temp
    bool32        is_valid;
} Commandline;

internal void *
read_whole_file(char *filename) {
    void *result = 0;
    int fd = open(filename, O_RDONLY);
    if (fd) {
        int64 tmp_filesize = lseek(fd, 0, SEEK_END);
        if (tmp_filesize > 0) {
            usize filesize = cast(usize, tmp_filesize);
            void *buffer = malloc(filesize);
            if (buffer) {
                lseek(fd, 0, SEEK_SET);
                ssize_t res = read(fd, buffer, filesize);
                if (res == filesize) {
                    result = buffer;
                } else {
                    LOG("Failed to read file %s", filename);
                }
            } else {
                LOG("Could not allocate memory");
            }
        } else {
            LOG_DEBUG("File is empty: %s", filename);
        }
        close(fd);
    } else {
        LOG("Could not open file: %s", filename);
    }

    return result;
}

internal bool32
is_numeric(char c) {
    bool32 result = true;

    if (c < '0' || c >= '9') {
        result = false;
    }

    return result;
}

internal char *
read_last_line_of_file(char *filename) {
    char *result = 0;
    int fd = open(filename, O_RDONLY);
    if (fd) {
        int64 filesize = lseek(fd, -1, SEEK_END);

        if (filesize > 0) {
            int64 file_offset = 0;
            char cursor;
            read(fd, &cursor, 1);\
            bool32 found_text = false;

            while (cursor != '\n' || !found_text) {
                if (cursor > ' ') {
                    found_text = true;
                }
                file_offset = lseek(fd, -2, SEEK_CUR);
                read(fd, &cursor, 1);
            }

            assert(filesize >= file_offset, "File offset %lld cannot be larger than filesize %lld", file_offset, filesize);
            usize line_byte_count = cast(usize, filesize - file_offset);
            void *buffer = malloc(line_byte_count + 1);
            if (buffer) {
                ssize_t res = read(fd, buffer, line_byte_count);
                if (res == line_byte_count) {
                    result = cast(char *, buffer);
                    // TODO(dgl): should we replace the newline with a nullbyte?
                    result[line_byte_count + 1] = 0;
                    LOG_DEBUG("Read string %s", result);
                } else {
                    LOG_DEBUG("Wanted to read %zu bytes but read %zd", line_byte_count, res);
                    LOG("Failed to read file %s", filename);
                }
            } else {
                LOG("Could not allocate memory");
            }
        } else {
            LOG_DEBUG("File is empty: %s", filename);
        }
        close(fd);
    } else {
        LOG("Could not open file: %s", filename);
    }

    return result;
}

internal void
token_error(Tokenizer *tokenizer, char *msg) {
    // NOTE(dgl): Only report first error.
    // During parsing after an error occured there may be more
    // because after an error all tokens become invalid.
    if (!tokenizer->has_error) {
        tokenizer->has_error = true;
        LOG("Parsing Error at Line %d: %s", tokenizer->line, msg);
    }
}

internal void
refill(Tokenizer *tokenizer) {
    if(tokenizer->input_count == 0) {
        tokenizer->at[0] = 0;
        tokenizer->at[1] = 0;
    } else if(tokenizer->input_count == 1) {
        tokenizer->at[0] = tokenizer->input[0];
        tokenizer->at[1] = 0;
    } else {
        tokenizer->at[0] = tokenizer->input[0];
        tokenizer->at[1] = tokenizer->input[1];
    }
}

internal void
advance(Tokenizer *tokenizer, uint32 count) {
    tokenizer->input += count;
    tokenizer->input_count -= count;

    if (tokenizer->input_count < 0) {
        tokenizer->input_count = 0;
    }

    refill(tokenizer);
}

internal void
eat_all_whitespace(Tokenizer *tokenizer) {
    // TODO(dgl): allow comments?
    while(tokenizer->at[0] == ' ') {
        advance(tokenizer, 1);
    }
}

internal Token
get_token(Tokenizer *tokenizer) {
    Token result = {};

    if (!tokenizer->has_error) {
        eat_all_whitespace(tokenizer);

        result.text = tokenizer->input;
        result.text_length = 1;
        result.line = tokenizer->line;
        char c = tokenizer->at[0];
        advance(tokenizer, 1);

        switch(c) {
            // NOTE(dgl): decrement tokenizer to hit EOF again if someone calls it
            case 0   : { result.type = Token_Type_EndOfFile; } break;
            case '|' : { result.type = Token_Type_Divider; } break;
            case '\n': { result.type = Token_Type_Newline; ++tokenizer->line; } break;
            default: {
                 if (tokenizer->last_token_type == Token_Type_Time &&
                   (c == '+' || c == '-')) {
                    result.type = Token_Type_Sign;
                } else if (tokenizer->last_token_type == Token_Type_Date &&
                           c == 'T' &&
                           is_numeric(tokenizer->at[0])) {
                    result.type = Token_Type_DateTimeDivider;
                } else if ((tokenizer->last_token_type == Token_Type_DateTimeDivider || tokenizer->last_token_type == Token_Type_Sign) &&
                           (is_numeric(c) && is_numeric(tokenizer->at[0]))) {
                    advance(tokenizer, 1);
                    result.type = Token_Type_Time;
                    result.text_length = 2;

                    while(*tokenizer->at == ':') {
                        advance(tokenizer, 1);
                        if(is_numeric(tokenizer->at[0]) && is_numeric(tokenizer->at[1])) {
                            advance(tokenizer, 2);
                            result.text_length = cast(int32, tokenizer->input - result.text);
                        } else {
                            token_error(tokenizer, "Invalid time");
                        }
                    }

                    if (result.text_length > 8) {
                        token_error(tokenizer, "Invalid time");
                    }
                } else {
                    if (is_numeric(tokenizer->at[0]) || tokenizer->at[0] == '-') {
                        advance(tokenizer, 1);
                        result.type = Token_Type_Integer;
                        while (is_numeric(tokenizer->at[0]) ) {
                            advance(tokenizer, 1);

                            if (tokenizer->at[0] == '-') {
                                advance(tokenizer, 1);
                                result.type = Token_Type_Date;
                            }
                        }

                        result.text_length = cast(int32, tokenizer->input - result.text);
                        if (result.type == Token_Type_Date && result.text_length != 10) {
                            token_error(tokenizer, "Invalid date length.");
                        }
                    } else {
                        while (tokenizer->at[0] != '\n' && tokenizer->at[0] != 0) {
                            advance(tokenizer, 1);
                        }
                        result.type = Token_Type_String;
                        result.text_length = cast(int32, tokenizer->input - result.text);
                    }
                }
            }
        }

        tokenizer->last_token_type = result.type;
    }

    LOG_DEBUG("Parsed token: type %d, text %.*s", result.type, result.text_length, result.text);

    return result;
}

internal Token
peek_token(Tokenizer *tokenizer) {
    Tokenizer tokenizer2 = *tokenizer;

    LOG_DEBUG("Peeking next token");
    Token result = get_token(&tokenizer2);
    return result;
}

internal char *
parse_text(Tokenizer *tokenizer) {
    char *result = 0;
    Token token = {};
    token = get_token(tokenizer);

    if (token.type == Token_Type_String) {
        result = cast(char *, malloc(cast(usize, token.text_length + 1)));
        if (result) {
            string_copy(token.text, cast(usize, token.text_length), result, cast(usize, token.text_length + 1));
        } else {
            LOG("Could not allocate memory");
        }
    } else {
        token_error(tokenizer, "Could not parse text");
    }

    return result;
}

internal Datetime
parse_date(Tokenizer *tokenizer) {
    Datetime result = {};
    Token token = {};
    token = get_token(tokenizer);

    if (token.type == Token_Type_Date) {
        // NOTE(dgl): yyyy-mm-dd
        if (token.text_length == 10) {
            char *cursor = token.text;
            result.year = string_to_int32(cursor, 4);
            cursor += 4 + 1;
            result.month = string_to_int32(cursor, 2);
            cursor += 2 + 1;
            result.day = string_to_int32(cursor, 2);
        } else {
            token_error(tokenizer, "Invalid date");
        }
    } else {
        token_error(tokenizer, "Could not parse date");
    }
    return result;
}

internal Datetime
parse_time(Tokenizer *tokenizer) {
    Datetime result = {};
    Token token = {};
    token = get_token(tokenizer);

    if (token.type == Token_Type_Time) {
        // NOTE(dgl): hh:mm[:ss]
        if (token.text_length >= 5 && token.text_length <= 8) {
            int index = 0;
            char *cursor = token.text;
            while (index < token.text_length) {
                if (index < 3) {
                    result.hour = string_to_int32(cursor + index, 2);
                    index += 2 + 1;
                } else if (index < 5) {
                    result.minute = string_to_int32(cursor + index, 2);
                    index += 2 + 1;
                } else if (index < 8) {
                    result.second = string_to_int32(cursor + index, 2);
                    index += 2;
                }
            }
        } else {
            token_error(tokenizer, "Invalid time");
        }
    } else {
        token_error(tokenizer, "Could not parse time");
    }

    return result;
}

internal Datetime
parse_timezone(Tokenizer *tokenizer) {
    Datetime result = {};
    Token token = {};
    token = get_token(tokenizer);

    if (token.type == Token_Type_Sign) {
        assert(token.text_length == 1, "Invalid sign token");
        if (*token.text == '-') {
            result.offset_sign = true;
        }

        Datetime offset = parse_time(tokenizer);
        result.offset_hour = offset.hour;
        result.offset_minute = offset.minute;
        result.offset_second = offset.second;
    } else {
        token_error(tokenizer, "Could not parse timezone");
    }

    return result;
}

internal Datetime
parse_datetime(Tokenizer *tokenizer) {
    Datetime result = {};

    Datetime date = parse_date(tokenizer);

    Token token = get_token(tokenizer);
    if (token.type != Token_Type_DateTimeDivider) {
        token_error(tokenizer, "Could not parse datetime");
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

internal int32
parse_integer(Tokenizer *tokenizer) {
    int32 result = 0;
    Token token = get_token(tokenizer);

    if (token.type == Token_Type_Integer) {
        // TODO(dgl): overflow check
        result = string_to_int32(token.text, token.text_length);
    } else {
        token_error(tokenizer, "Could not parse integer");
    }

    return result;
}

internal Entry
parse_entry(Tokenizer *tokenizer) {
    Entry result = {};

    result.begin = parse_datetime(tokenizer);

    Token token = get_token(tokenizer);
    if (token.type != Token_Type_Divider) {
        token_error(tokenizer, "Could not parse entry");
    }

    token = peek_token(tokenizer);
    if (token.type == Token_Type_Date) {
        result.end = parse_datetime(tokenizer);
    }

    token = get_token(tokenizer);
    if (token.type != Token_Type_Divider) {
        token_error(tokenizer, "Could not parse entry");
    }

    token = peek_token(tokenizer);
    if (token.type == Token_Type_Integer) {
        result.task_id = parse_integer(tokenizer);
    } else {
        result.task_id = -1;
    }

    token = get_token(tokenizer);
    if (token.type != Token_Type_Divider) {
        token_error(tokenizer, "Could not parse entry");
    }

    token = peek_token(tokenizer);
    if (token.type == Token_Type_String) {
        result.annotation = parse_text(tokenizer);
    }

    token = get_token(tokenizer);
    if (token.type != Token_Type_Newline && token.type != Token_Type_EndOfFile) {
        token_error(tokenizer, "Could not parse entry");
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
        int index = 0;
        char *cursor = tz;

        // TODO(dgl): use tokenizer
        usize len = string_length(tz);
        while (index < len) {
            if (index < 1 && cursor[index] == '-') {
                // NOTE(dgl): sign
                result.offset_sign = true;
                index += 1;
            } else if (index < 3) {
            // NOTE(dgl): hours
                result.offset_hour = string_to_int32(cursor + index, 2);
                index += 2;
            } else if (index < 5) {
            // NOTE(dgl): minutes
                result.offset_minute = string_to_int32(cursor + index, 2);
                index += 2;
            } else if (index < 8) {
            // NOTE(dgl): seconds
                result.offset_second = string_to_int32(cursor + index, 2);
                index += 2;
            }
        }
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

internal void
commandline_parse(Commandline *ctx, char** args, int args_count) {
    ctx->is_valid = true;

    // NOTE(dgl): Call without args
    if (args_count == 1) {
        ctx->command_type = Command_Type_Report;
    } else {
        // NOTE(dgl): parse command
        int cursor = 1;
        char *arg = args[cursor++];

        if (string_compare("sta", arg, 3) == 0) {
            ctx->command_type = Command_Type_Start;
        } else if (string_compare("sto", arg, 3) == 0) {
            ctx->command_type = Command_Type_Stop;
        } else if (string_compare("rep", arg, 3) == 0) {
            ctx->command_type = Command_Type_Report;
        } else if (string_compare("csv", arg, 3) == 0) {
            ctx->command_type = Command_Type_CSV;
#if DEBUG
        } else if (string_compare("gen", arg, 3) == 0) {
            ctx->command_type = Command_Type_Generate;
#endif
        } else {
            ctx->is_valid = false;
            // TODO(dgl): logging
            LOG("Unknown command: %s", arg);
        }

        ctx->params = args[cursor++];
    }
}

int main(int argc, char** argv) {
    Commandline cmdline = {};
    commandline_parse(&cmdline, argv, argc);

    if (cmdline.is_valid) {
        switch(cmdline.command_type) {
            case Command_Type_Start: {
                Datetime now = get_timestamp();
                print_timestamp(&now);

                char *string = read_last_line_of_file(cmdline.params);
                assert(string, "Invalid last line");
                Tokenizer tokenizer = {};
                tokenizer.input = string;
                tokenizer.input_count = safe_size_to_int32(string_length(string));
                refill(&tokenizer);

                Entry entry = parse_entry(&tokenizer);

                if (!tokenizer.has_error) {
                    print_timestamp(&entry.begin);
                    print_timestamp(&entry.end);
                    printf("%d\n", entry.task_id);
                    printf("%s\n", entry.annotation);
                }
                // TODO(dgl): get first or last line in file
            } break;
            case Command_Type_Stop: {
                Datetime now = get_timestamp();
                print_timestamp(&now);
            } break;
            case Command_Type_Report: {

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

    return 0;
}
