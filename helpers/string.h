#ifndef STRING_H_INCLUDE
#define STRING_H_INCLUDE

#include <stdarg.h>
#include "stb_sprintf.h"
#include "memory.h"

typedef struct {
    usize length;
    usize cap;
    union {
        char *text;
        void *data;
    };
} String;

typedef struct String_Builder {
    Mem_Arena *arena;
    String string;
} String_Builder;

internal String string_from_c_str(char *s);
internal void string_copy(char *src, size_t src_count, char *dest, size_t dest_count);
internal void string_concat(char *src_a, size_t src_a_count, char *src_b, size_t src_b_count, char *dest, size_t dest_count);
internal int32 string_compare(char *string_a, char *string_b, size_t string_count);
internal int32 string_to_int32(char *string, int32 count);

internal String_Builder string_builder_init(Mem_Arena *arena, usize capacity);
internal void string_append(String_Builder *builder, char *fmt, ...);
internal String string_builder_to_string(String_Builder *builder);

internal inline usize
string_length(char *string) {
    char *begin = string;
    usize count = 0;
    while(*string++) {
        count++;
    }

    count -= 1;

    // NOTE(dgl): should return one char before nullbyte
    assert(begin[count + 1] == '\0', "Not a valid cstring");
    return count;
}

internal inline char *
string_to_c_str(Mem_Arena *arena, String s) {
    assert(s.text != 0, "String text is not defined");

    char *text = mem_arena_push_array(arena, char, s.length + 1);
    string_copy(s.text, s.length, text, s.length + 1);
    text[s.length + 1] = 0;

    return text;
}

#endif // STRING_H_INCLUDE

#ifdef STRING_IMPLEMENTATION

internal void
string_copy(char *src, size_t src_count, char *dest, size_t dest_count) {
    // NOTE(dgl): Must be one larger for nullbyte
    assert(dest_count > src_count, "String overflow. Increase string size");

    for(int index = 0; index < src_count; ++index) {
        *dest++ = *src++;
    }
    // TODO(dgl): is this necessary?
    // This should only be done when using cstrings. We should replace the string copy parameter
    // by strings and create a mem_copy function for everything else.
    *dest = '\0';
}

internal String
string_from_c_str(char *s) {
    String result = {};
    result.length = string_length(s);
    result.cap = result.length + 1;
    result.text = s;

    assert(result.text[result.cap] == '\0', "Not a valid cstring");

    return result;
}

internal void
string_concat(char *src_a, size_t src_a_count, char *src_b, size_t src_b_count, char *dest, size_t dest_count) {
    assert(dest_count > src_a_count + src_b_count, "String overflow. Increase string size");
    string_copy(src_a, src_a_count, dest, dest_count);
    string_copy(src_b, src_b_count, dest + src_a_count, dest_count - src_a_count);
}

internal int32
string_compare(char *string_a, char *string_b, size_t string_count) {
    int32 result = 0;
    int32 cursor = 0;
    while((result == 0) && (cursor < string_count)) {
        result = *string_a++ - *string_b++;
        cursor++;
    }
    return result;
}

internal int32
string_to_int32(char *string, int32 count) {
    int32 result = 0;
    bool32 sign = false;

    if(*string == '-') {
        sign = true;
        string++;
        count--;
    }

    for(int index = 0; index < count; index++) {
        if (index > 0) {
            result *= 10;
        }

        assert(string[index] >= '0' && string[index] <= '9', "Character %c (%d) is not a number", string[index], string[index]);
        result += string[index] - '0';
    }

    if (sign) {
        result = -result;
    }

    return result;
}

internal String_Builder
string_builder_init(Mem_Arena *arena, usize default_cap) {
    String_Builder result = {};
    result.arena = arena;
    result.string.text = mem_arena_push_array(arena, char, default_cap);
    result.string.length = 0;
    result.string.cap = default_cap;
    return result;
}

internal void
string_append(String_Builder *builder, char *fmt, ...) {
    Mem_Arena *a = builder->arena;
    va_list ap;
retry:
    va_start(ap, fmt);
    assert(builder->string.cap > builder->string.length, "String length cannot be larger than its capacity");
    int32 size = stbsp_vsnprintf(cast(char *, builder->string.data + builder->string.length), cast(int, builder->string.cap - builder->string.length), fmt, ap);
    if(size < 0) {
        LOG("Failed to append string to %p", builder->string.data);
    } else {
        usize required_cap = builder->string.length + cast(usize, size);
        if(builder->string.cap <= required_cap) {
            usize new_capacity = builder->string.cap;
            while(new_capacity <= required_cap) {
                new_capacity *= 2;
            }

            // NOTE(dgl): If the current buffer size is too small,
            // we resize the buffer with double the size.
            builder->string.data = mem_arena_resize(a, builder->string.data, builder->string.cap, new_capacity);
            builder->string.cap = new_capacity;
            goto retry;
        }
        builder->string.length += cast(usize, size);
        builder->string.text[builder->string.length] = '\0';
    }
}

internal String
string_builder_to_string(String_Builder *builder) {
   String result = builder->string;
   return result;
}

internal Buffer
string_to_buffer(String *string) {
    Buffer result = {};
    result.data = string->data;
    result.data_count = string->length;
    result.cap = string->cap;

    return result;
}

#endif // STRING_IMPLEMENTATION
