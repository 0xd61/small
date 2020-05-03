#ifndef TYPES_H
#define TYPES_H

#if DEBUG
#define Assert(Expression) if(!(Expression)) {__builtin_trap();}
#else
#define Assert(Expression)
#endif

#define Kilobytes(Value) ((Value)*1024LL)
#define Megabytes(Value) (Kilobytes(Value)*1024LL)
#define Gigabytes(Value) (Megabytes(Value)*1024LL)
#define Terabytes(Value) (Gigabytes(Value)*1024LL)

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))

typedef unsigned char  uint8 ;
typedef   signed char   int8 ;
typedef unsigned short uint16;
typedef   signed short  int16;
typedef unsigned int   uint32;
typedef   signed int    int32;
typedef unsigned long  uint64;
typedef   signed long   int64;

typedef float  real32;
typedef double real64;
typedef int32  bool32;
#define false  0;
#define true   1;

#define internal      static
#define local_persist static
#define global        static

inline uint32
SafeTruncateSize32(uint64 Value)
{
    Assert(Value <= 0xFFFFFFFF)
        uint32 Result = (uint32)Value;
    return (Result);
}



#endif //TYPES_H
