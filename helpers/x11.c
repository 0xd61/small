// TODO(dgl): This file is just a placeholder and will be used to communicate with the Xorg server.
// We will communicate with the server directly but first we have to figure out how the protocol works
// Because I have not found a documentation that works. But we will intercept XCB calls/packages from the Handmade
// Hero app and build the structures with this information.
// However we will first build the backlight part and afterwards the xrandr part for tmonitor. For the backlight part
// we will not use X11 because there only intel is supported and it is easier to directly change the files in
// /sys/class/backlight

#include <sys/socket.h>				//Socket related constants
#include <sys/un.h>					//Unix domain constants
#include <netinet/in.h>				//Socket related constants
#include <stdio.h>
#include <unistd.h>

typedef struct sockaddr sockaddr;
typedef struct sockaddr_un sockaddr_un;
typedef struct sockaddr_in sockaddr_in;

typedef struct X11ConnRequest
{
    uint8  ByteOrder;
    uint8  _Pad0;
	uint16 MajorVersion;
	uint16 MinorVersion;
	uint8 AuthProtocol;
	uint8 AuthData;
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

// TODO(dgl): New structure check how this works in debugger
typedef struct X11Setup
{
    uint32 Release;
    uint32 IdBase;
    uint32 IdMask;
    uint32 MotionBufferSize;
    uint16 VendorLength;
    uint16 RequestMax;
    uint8 RootsList; // Pointer
    uint8 Formats;
    uint8 ImageOrder;
    uint8 BitmapOrder;
    uint8 ScanlineUnit;
    uint8 ScanlinePad;
    uint8 KeycodeMin;
    uint8 KeycodeMax;
    uint8 _Pad[4];
} X11Setup;

typedef struct X11Connection
{
    int    Handle;
    bool32 Established;
} X11Connection;

int main(int argc, char ** argv)
{
    X11Connection Conn = {};
    
#if 1
    sockaddr_in Socket = {};
    
    //Create the socket
    Conn.Handle = socket(AF_INET, SOCK_STREAM, 0);
    if (Conn.Handle < 0)
    {
        fprintf(stderr, "Error opening socket\n");
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
        fprintf(stderr, "Error opening socket\n");
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
            read(Conn.Handle, &Response, sizeof(X11ConnResponse));
            
            Conn.Established = Response.Status;
            if(Conn.Established)
            {
                X11Setup Setup = {};
                read(Conn.Handle, &Setup, sizeof(X11Setup));
                printf("COOL");
            }
            else
            {
                // TODO(dgl): logging
                char Reason[255];
                Assert(Response.ReasonCount < ArrayCount(Reason));
                read(Conn.Handle, &Reason, Response.ReasonCount);
                fprintf(stderr, "Handshake error: %s\n", Reason);
            }
        }
        else
        {
            // TODO(dgl): logging
            fprintf(stderr, "Could not write to socket\n");
        }
    }
    else
    {
        // TODO(dgl): logging
        fprintf(stderr, "Could not connect to socket\n");
    }
    
    if(Conn.Established)
    {
        
    }
    
    return 0;
}