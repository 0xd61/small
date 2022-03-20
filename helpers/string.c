typedef struct {
    usize length;
    usize cap;
    union {
        char *text;
        void *data;
    };
} String;

internal usize
string_length(char *string) {
    usize count = 0;
    while(*string++) {
        count++;
    }

    return count;
}

internal String
string_from_c_str(char *s) {
    String result = {};
    result.length = string_length(s);
    result.cap = result.length;
    result.text = s;

    return result;
}

internal char *
string_to_c_str(String s) {
    s.text[s.cap] = 0;
    return s.text;
}

internal void
string_copy(char *src, size_t src_count, char *dest, size_t dest_count) {
    // NOTE(dgl): Must be one larger for nullbyte
    assert(dest_count > src_count, "String overflow. Increase string size");

    for(int index = 0; index < src_count; ++index) {
        *dest++ = *src++;
    }
    *dest = '\0';
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
