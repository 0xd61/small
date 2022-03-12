// TODO(dgl): Think about optimization if used in multiple tools
internal usize
string_length(char *string) {
    usize count = 0;
    while(*string++) {
        count++;
    }

    return(count);
}

// TODO(dgl): Think about optimization if used in multiple tools
internal void
string_copy(char *src, size_t src_count, char *dest, size_t dest_count) {
    // NOTE(dgl): Must be one larger for nullbyte
    assert(dest_count > src_count, "String overflow. Increase string size");

    for(int index = 0; index < src_count; ++index) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// TODO(dgl): Think about optimization if used in multiple tools
internal void
string_concat(char *src_a, size_t src_a_count, char *src_b, size_t src_b_count, char *dest, size_t dest_count) {
    assert(dest_count > src_a_count + src_b_count, "String overflow. Increase string size");
    string_copy(src_a, src_a_count, dest, dest_count);
    string_copy(src_b, src_b_count, dest + src_a_count, dest_count - src_a_count);
}

// TODO(dgl): Think about optimization if used in multiple tools
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

        assert(string[index] >= '0' && string[index] <= '9', "Character %c is not a number", string[index]);
        result += string[index] - '0';
    }

    if (sign) {
        result = -result;
    }

    return result;
}
