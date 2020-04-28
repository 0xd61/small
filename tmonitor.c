/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Monitor (execute with tmonitor)
Author  : Daniel Glinka

This tools combines parts of xbacklight and xrandr to control backlight and external screens. It is not a full replacement for these tools.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#include "helpers/types.h"

#include <sys/socket.h>				//Socket related constants
#include <sys/un.h>					//Unix domain constants
#include <netinet/in.h>				//Socket related constants
#include <stdio.h>
#include <unistd.h>

#define false 0;
#define true 1;

typedef struct sockaddr sockaddr;
typedef struct sockaddr_un sockaddr_un;
typedef struct sockaddr_in sockaddr_in;

typedef struct X11ConnRequest
{
    uint8  ByteOrder;
    uint8  _Pad0;
	uint16 MajorVersion;
	uint16 MinorVersion;
	uint16 AuthProtocol;
	uint16 AuthData;
	uint8  _Pad1[2];
} X11ConnRequest;

typedef struct X11ConnResponse
{
    uint8  Status;
    uint8  ReasonCount;
    uint16 MajorVersion;
    uint16 MinorVersion;
    uint16 Length;
} X11ConnResponse;

typedef struct X11Setup
{
    uint8  Status;
    uint8  _Pad0;
    uint16 MajorVersion;
    uint16 MinorVersion;
    uint16 Length;
    uint32 ReleaseNumber;
    uint32 ResourceIdBase;
    uint32 ResourceIdMask;
    uint32 MotionBufferSize;
    uint16 VendorLength;
    uint16 MaximumRequestLength;
    uint8  RootsLength;
    uint8  PixmapFormatsLength;
    uint8  ImageByteOrder;
    uint8  BitmapFormatBitOrder;
    uint8  BitmapFormatScanlineUnit;
    uint8  BitmapFormatScanlinePad;
    uint8  MinKeycode;
    uint8  MaxKeycode;
    uint8  _Pad1[4];
} X11Setup;

typedef struct X11Connection
{
    int    Handle;
    bool32 Established;
} X11Connection;

internal size_t
StringLength(char *String)
{
    size_t Count = 0;
    while(*String++)
    {
        Count++;
    }
    
    return(Count);
}

internal void
StringCopy(char *Src, size_t SrcCount, char *Dest, size_t DestCount)
{
    Assert(SrcSize >= DestSize);
    
    for(int index = 0; index < SrcCount; ++index)
    {
        *Dest++ = *Src++;
    }
}

int main(int argc, char ** argv)
{
    X11Connection Conn = {};
    
#if 1
    sockaddr_in Socket = {};
    
    //Create the socket
    Conn.Handle = socket(AF_INET, SOCK_STREAM, 0);
    if (Conn.Handle < 0)
    {
        printf("Error opening socket\n");
        return -1;
    }
    
    Socket.sin_family = AF_INET;
    Socket.sin_addr.s_addr=0x0100007f; // localhost (127.0.0.1) in network order (big endian)
    Socket.sin_port = htons(6001);
    
    if(connect(Conn.Handle, (sockaddr *)&Socket, sizeof(Socket)) >= 0)
#else
    sockaddr_un Socket = {};
    
    //Create the socket
    Conn.Handle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (Conn.Handle < 0)
    {
        printf("Error opening socket\n");
        return -1;
    }
    
    Socket.sun_family = AF_UNIX;
    char *SocketPath = "/tmp/.X11-unix/X0";
    StringCopy(SocketPath, StringLength(SocketPath), Socket.sun_path, ArrayCount(Socket.sun_path));
    
    if(connect(Conn.Handle, (sockaddr *)&Socket, sizeof(Socket)) >= 0)
#endif
    {
        X11ConnRequest Request = {};
        Request.ByteOrder = 'l'; // Little Endian
        Request.MajorVersion = 11;
        Request.MinorVersion = 0;
        
        if(write(Conn.Handle, &Request, sizeof(X11ConnRequest)) >= 0)
        {
            X11ConnResponse Response = {};
            if(read(Conn.Handle, &Response, sizeof(X11ConnResponse)) >= 0)
            {
                Conn.Established = Response.Status;
                X11Setup Setup = {};
                
                read(Conn.Handle, &Setup, sizeof(X11Setup));
                
            }
            else
            {
                // TODO(dgl): logging
            }
        }
        else
        {
            // TODO(dgl): logging
            printf("Could not write to socket\n");
        }
    }
    else
    {
        // TODO(dgl): logging
        printf("Could not connect to socket\n");
    }
    
    if(Conn.Established)
    {
        
    }
    
    return 0;
}