#ifndef TYPES_H
#define TYPES_H

//
// Types
//

#define internal static
#define global static
#define local_persist static

#define kilobytes(value) ((value)*1024LL)
#define megabytes(value) (kilobytes(value)*1024LL)
#define gigabytes(value) (megabytes(value)*1024LL)
#define terabytes(value) (gigabytes(value)*1024LL)

typedef unsigned char      uint8 ;
typedef   signed char       int8 ;
typedef unsigned short     uint16;
typedef   signed short      int16;
typedef unsigned int       uint32;
typedef   signed int        int32;
typedef unsigned long long uint64;
typedef          long long  int64;
typedef          float     real32;
typedef          double    real64;
typedef          int32     bool32;
#define true 1
#define false 0

// TODO(dgl): could we define a intptr without stdint.h?
#include <stdint.h>
typedef uintptr_t uintptr;
#include <stddef.h>
typedef size_t usize;

//
// Defines
//

#if DEBUG
#define assert(cond, fmt, args...) do                                                   \
{                                                                              \
    if (!(cond))                                                               \
    {                                                                          \
      fprintf(stderr, "Fatal error: %s:%d: assertion '%s' failed with: " fmt "\n",   \
      __FILE__, __LINE__, #cond, ##args);                                        \
      __builtin_trap();                                                        \
    }                                                                          \
} while(0)
#else
#define assert(cond, msg, ...) __builtin_trap();
#endif

#if DEBUG
#define LOG(fmt, args...) printf(("%s:%d - " fmt "\n"), __FILE__, __LINE__, ##args)
#define LOG_DEBUG(fmt, args...) printf(("%s:%d - " fmt "\n"), __FILE__, __LINE__, ##args)
#else
#define LOG(fmt, args...) printf((fmt "\n"), ##args)
#define LOG_DEBUG(fmt, ...)
#endif

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define clamp(x,lo,hi) (min((hi), max((lo), (x))))
#define abs(x) ((x) < 0 ? (-x) : (x))

#define array_count(array) (sizeof(array) / sizeof((array)[0]))
#define cast(type, value) (type)(value)

//
// Utilities
//

internal inline uint16
safe_truncate_size_uint16(uint32 value) {
    assert(value <= 0xFFFF, "Failed to safely truncate value");
    uint16 result = cast(uint16, value);
	return (result);
}

internal inline int16
safe_truncate_size_int16(int32 value) {
    assert(value <= 0xFFFF, "Failed to safely truncate value");
    int16 result = cast(int16,value);
	return (result);
}

internal inline uint32
safe_truncate_size_uint32(uint64 value) {
    assert(value <= 0xFFFFFFFF, "Failed to safely truncate value");
    uint32 result = cast(uint32,value);
	return (result);
}

   internal inline int32
safe_truncate_size_int32(int64 value) {
    assert(value <= 0xFFFFFFFF, "Failed to safely truncate value");
    int32 result = cast(int32,value);
	return (result);
}

internal inline uint32
safe_size_to_uint32(usize value) {
    assert(value <= 0xFFFFFFFF, "Failed to safely truncate value");
    uint32 result = cast(uint32,value);
    return(result);
}

internal inline int32
safe_size_to_int32(usize value) {
    assert(value <= 0x7FFFFFFF, "Failed to safely cast value");
    int32 result = cast(int32,value);
    return(result);
}

#endif //TYPES_H
