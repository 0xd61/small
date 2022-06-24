#ifndef MEMORY_H_INCLUDE
#define MEMORY_H_INCLUDE

#include <string.h>

#define DEFAULT_ALIGNMENT (2*sizeof(void *))

typedef usize Mem_Index;

typedef struct Mem_Arena {
    uint8 *base;
    Mem_Index size;
    Mem_Index curr_offset;
    Mem_Index prev_offset;
    char *dbg_name;
} Mem_Arena;

typedef struct Mem_Temp_Arena {
    Mem_Arena *arena;
    Mem_Index curr_offset;
    Mem_Index prev_offset;
} Mem_Temp_Arena;

internal void mem_arena_init(Mem_Arena *arena, uint8 *base, Mem_Index size, char *dbg_name);
#define mem_arena_push_struct(arena, type) (type *)mem_arena_alloc_align(arena, sizeof(type), DEFAULT_ALIGNMENT)
#define mem_arena_push_array(arena, type, count) (type *)mem_arena_alloc_align(arena, (count)*sizeof(type), DEFAULT_ALIGNMENT)
#define mem_arena_push(arena, size) mem_arena_alloc_align(arena, size, DEFAULT_ALIGNMENT)
internal void * mem_arena_alloc_align(Mem_Arena *arena, Mem_Index size, Mem_Index align);
#define mem_arena_resize_array(arena, type, current_base, current_size, new_size) (type *) mem_arena_resize_align(arena, cast(uint8 *, current_base), (current_size)*sizeof(type), (new_size)*sizeof(type), DEFAULT_ALIGNMENT)
#define mem_arena_resize(arena, current_base, current_size, new_size) mem_arena_resize_align(arena, current_base, current_size, new_size, DEFAULT_ALIGNMENT)
internal void * mem_arena_resize_align(Mem_Arena *arena, uint8 *current_base, Mem_Index current_size, Mem_Index new_size, usize align);
internal void mem_arena_free_all(Mem_Arena *arena);
internal Mem_Temp_Arena mem_arena_begin_temp(Mem_Arena *arena);
internal void mem_arena_end_temp(Mem_Temp_Arena temp);

#endif // MEMORY_H_INCLUDE

#ifdef MEMORY_IMPLEMENTATION

internal uintptr
_align_forward_uintptr(uintptr base, usize align) {
    uintptr result = base;

    assert((align & (align - 1)) == 0, "Alignment has to be a power of two");
    // Same as (base % align) but faster as 'align' is a power of two
    uintptr modulo = base & (cast(uintptr, align) - 1);

    if(modulo != 0) {
        result += (align - modulo);
    }

    return(result);
}

internal void
mem_arena_init(Mem_Arena *arena, uint8 *base, Mem_Index size, char *dbg_name) {
    arena->size = size;
    arena->base = base;
    arena->curr_offset = 0;
    arena->prev_offset = 0;
    arena->dbg_name = dbg_name;
}

internal void *
mem_arena_alloc_align(Mem_Arena *arena, Mem_Index size, usize align) {
    uintptr curr_ptr = cast(uintptr, arena->base + arena->curr_offset);
    uintptr new_ptr = _align_forward_uintptr(curr_ptr, align);

    Mem_Index offset = cast(Mem_Index, new_ptr - cast(uintptr, arena->base)); // revert back to relative offset

    LOG_DEBUG("%s: allocating memory %lu bytes (%lu left)", arena->dbg_name, size, arena->size - (offset+size));
    // TODO(dgl): make a proper memory check here and allocate more memory from the os
    assert((offset + size) <= arena->size, "Arena overflow. Cannot allocate size");

    void *result = arena->base + offset;
    arena->prev_offset = offset;
    arena->curr_offset = offset + size;


    // Zero new memory by default (we do not zero the memory on init or free_all)
    memset(result, 0, size);

    return(result);
}

internal void *
mem_arena_resize_align(Mem_Arena *arena, uint8 *current_base, Mem_Index current_size, Mem_Index new_size, usize align) {
    void *result = 0;
    assert(arena->base <= current_base && current_base < arena->base + arena->size, "This allocation does not belong to the arena");

    if(current_size == new_size) {
        result = current_base;
    }
    else if(arena->base + arena->prev_offset == current_base) {
        // TODO(dgl): make a proper memory check here and allocate more memory from the os
        assert((arena->prev_offset + new_size) <= arena->size, "Arena overflow. Cannot allocate new size");
        arena->curr_offset = arena->prev_offset + new_size;
        if (new_size > current_size) {
            // Zero the newly allocated memory
            memset(arena->base + arena->prev_offset + current_size, 0, new_size - current_size);
        }
        result = current_base;
    } else {
        void *new_base = mem_arena_alloc_align(arena, new_size, align);
        // NOTE(dgl): copy the existing data to the new location
        usize copy_size = new_size < current_size ? new_size : current_size;
        memcpy(new_base, current_base, copy_size);
        result = new_base;
    }

    return(result);
}

internal void
mem_arena_free_all(Mem_Arena *arena) {
    arena->curr_offset = 0;
    arena->prev_offset = 0;
}

internal Mem_Temp_Arena
mem_arena_begin_temp(Mem_Arena *arena) {
    Mem_Temp_Arena result;
    result.arena = arena;
    result.prev_offset = arena->prev_offset;
    result.curr_offset = arena->curr_offset;
    return(result);
}

internal void
mem_arena_end_temp(Mem_Temp_Arena temp) {
    temp.arena->prev_offset = temp.prev_offset;
    temp.arena->curr_offset = temp.curr_offset;
}

#endif // MEMORY_IMPLEMENTATION
