/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Monitor (execute with tmonitor)
Author: Daniel Glinka

This tool combines parts of xbacklight, xrandr and redshift to control backlight, external screens and redshift (night mode). It only provides features necessary for my setup. It is not a full replacement for these tools.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#include <dirent.h> // dirent, readdir
#include <fcntl.h> // open, close
#include <unistd.h> // read, write
#include <stdio.h>

// TODO(dgl): Remove xcb deps?
#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "helpers/types.h"

typedef struct dirent dirent;

typedef struct
{
    char PathName[4096];
    size_t PathNameCount;
    real32 Brightness; // In percent (0 to 1)
    uint32 MaxBrightness;
} backlight_provider;

// TODO(dgl): Merge connection into screen resources or vice versa? Then we should check on each function if the necessary fields are initialized
typedef struct
{
    xcb_connection_t *Connection;
} xcb_context;

typedef struct
{
    xcb_randr_get_screen_resources_current_reply_t *Screen;
    xcb_randr_output_t *Outputs;
    int OutputCount;
} xcb_screen_resources;

typedef enum
{
    AUTOMATIC,
    OFF
} xcb_crtc_mode;

#define ACPI_BACKLIGHT_DIR "/sys/class/backlight/"
// NOTE(dgl): We prepent the / so we do not have to do another String concat and add it after we get the provider name.
// If this causes problems we will change it and add append the / to the provider name.
#define MAX_BRIGHTNESS_FILENAME "/max_brightness"
#define CURRENT_BRIGHTNESS_FILENAME "/brightness"

// TODO(dgl): Think about optimization if used in multiple tools
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

// TODO(dgl): Think about optimization if used in multiple tools
internal void
StringCopy(char *Src, size_t SrcCount, char *Dest, size_t DestCount)
{
    // NOTE(dgl): Must be one larger for nullbyte
    Assert(DestSize > SrcSize);
    
    for(int index = 0; index < SrcCount; ++index)
    {
        *Dest++ = *Src++;
    }
    *Dest = '\0';
    
}

// TODO(dgl): Think about optimization if used in multiple tools
internal void
StringConcat(char *SrcA, size_t SrcACount, char *SrcB, size_t SrcBCount,
             char *Dest, size_t DestCount)
{
    Assert(DestCount > SourceACount + SourceBCount);
    StringCopy(SrcA, SrcACount, Dest, DestCount);
    StringCopy(SrcB, SrcBCount, Dest + SrcACount, DestCount - SrcACount);
}

// TODO(dgl): Think about optimization if used in multiple tools
internal int32
StringCompare(char *StringA, char *StringB, size_t StringCount)
{
    int32 Result = 0;
    int32 Cursor = 0;
    while((Result == 0) && (Cursor < StringCount))
    {
        Result = *StringA++ - *StringB++;
        Cursor++;
    }
    return Result;
}

internal uint32
AToUInt32(char *String, size_t StringSize)
{
    uint32 Result = 0;
    for(int Index = 0; Index < StringSize; ++Index)
    {
        char Digit = String[Index];
        Assert((Digit >= '0') && Digit <= '9');
        uint32 Normalized = (uint32)(Digit - '0');
        Result = Result * 10 + Normalized;
    }
    return Result;
}

internal void
UInt32ToA(uint32 Number, char*String, size_t StringSize)
{
    snprintf(String, StringSize, "%d", Number);
}

internal void
WriteBrightnessToFile(const char*Path, uint32 Brightness)
{
    int FileHandle = 0;
    FileHandle = open(Path, O_WRONLY);
    if(FileHandle > 0)
    {
        ssize_t WriteCount = 0;
        
        // NOTE(dgl): Overkill but who cares
        char Buffer[128] = {};
        UInt32ToA(Brightness, Buffer, ArrayCount(Buffer));
        WriteCount = write(FileHandle, Buffer, ArrayCount(Buffer));
        if(WriteCount <= 0)
        {
            // TODO(dgl): logging
        }
        close(FileHandle);
    }
    else
    {
        fprintf(stderr, "Could not write to file %s\n Try using \"sudo\"!\n", Path);
        // TODO(dgl): logging
    }
}

internal uint32
ReadBrightnessFromFile(const char* Path)
{
    uint32 Result = 0;
    int FileHandle = 0;
    FileHandle = open(Path, O_RDONLY);
    if(FileHandle > 0)
    {
        // NOTE(dgl): Overkill but who cares
        char Buffer[128] = {};
        size_t ReadCount = 0;
        read(FileHandle, Buffer, ArrayCount(Buffer));
        if(ReadCount >= 0)
        {
            size_t ReadCount = StringLength(Buffer);
            
            // NOTE(dgl): Remove the \n at the end of the value with -1
            if(Buffer[ReadCount - 1] < '0' || Buffer[ReadCount - 1] > '9')
            {
                ReadCount--;
            }
            
            Result = AToUInt32(Buffer, ReadCount);
        }
        else
        {
            fprintf(stderr, "Could not read file %s\n", Path);
            // TODO(dgl): logging
        }
        close(FileHandle);
    }
    else
    {
        fprintf(stderr, "Could not read file %s\n", Path);
        // TODO(dgl): logging
    }
    return(Result);
}

// TODO(dgl): Split into GetProviderInfo and GetProviders
internal uint32
GetProviderInfo(backlight_provider *Providers, uint32 ProviderCount)
{
    uint32 Result = 0;
    DIR *BacklightDir = opendir(ACPI_BACKLIGHT_DIR);
    if(BacklightDir)
    {
        dirent *Entry;
        uint32 CurrentProvider = 0;
        size_t BasePathCount = StringLength(ACPI_BACKLIGHT_DIR);
        while((Entry = readdir(BacklightDir)))
        {
            if(!(Entry->d_name[0] == '.' && Entry->d_name[1] == '\0') &&
               !(Entry->d_name[0] == '.' && Entry->d_name[1] == '.' && Entry->d_name[2] == '\0'))
            {
                backlight_provider *Provider = &Providers[CurrentProvider++];
                Assert(CurrentProvider < ProviderCount);
                
                size_t NameCount = StringLength(Entry->d_name);
                
                StringConcat(ACPI_BACKLIGHT_DIR, BasePathCount, Entry->d_name, NameCount, Provider->PathName, ArrayCount(Provider->PathName));
                
                Provider->PathNameCount = BasePathCount + NameCount;
                
                // NOTE(dgl): Getting max brightness for provider
                char MaxBrightnessPath[4096];
                size_t MaxBrightnessFileNameCount = StringLength(MAX_BRIGHTNESS_FILENAME);
                StringConcat(Provider->PathName, Provider->PathNameCount,
                             MAX_BRIGHTNESS_FILENAME, MaxBrightnessFileNameCount,
                             MaxBrightnessPath, ArrayCount(MaxBrightnessPath));
                
                Provider->MaxBrightness = ReadBrightnessFromFile(MaxBrightnessPath);
                
                // NOTE(dgl): Getting current brightness for provider
                char CurrentBrightnessPath[4096];
                size_t CurrentBrightnessFileNameCount = StringLength(CURRENT_BRIGHTNESS_FILENAME);
                StringConcat(Provider->PathName, Provider->PathNameCount,
                             CURRENT_BRIGHTNESS_FILENAME, CurrentBrightnessFileNameCount,
                             CurrentBrightnessPath, ArrayCount(CurrentBrightnessPath));
                
                uint32 Brightness = ReadBrightnessFromFile(CurrentBrightnessPath);
                Provider->Brightness = ((real32)Brightness / (real32)Provider->MaxBrightness);
            }
        }
        closedir(BacklightDir);
        Result = CurrentProvider;
    }
    else
    {
        // TODO(dgl): logging
    }
    
    return(Result);
}

internal void
SetOutputProperty(xcb_context *Context, xcb_screen_resources *Resources, xcb_randr_output_t Output, char *Propery)
{
    
}

internal void
SetOutputCrtcMode(xcb_context *Context, xcb_screen_resources *Resources, xcb_randr_output_t Output, xcb_crtc_mode Mode)
{
    
    //xcb_randr_get_crtc_cookie_t CrtcCookie = xcb_randr_get_crtc_info(Context->Connection, OutputInfo->crtc, Resources->timestamp);
    //xcb_randr_get_crtc_info_reply_t *Crtc = xcb_randr_get_crtc_info_reply(Context->Connection, CrtcCookie, NULL);
}

internal xcb_randr_output_t
GetScreenOutputByName(xcb_context *Context, xcb_screen_resources *Resources, char *Name)
{
    xcb_randr_output_t Result = 0;
    
    for (int Index = 0; 
         Index < Resources->OutputCount; 
         ++Index) 
    {
        xcb_randr_get_output_info_cookie_t OutputInfoCookie = xcb_randr_get_output_info(Context->Connection, Resources->Outputs[Index], Resources->Screen->config_timestamp);
        
        xcb_randr_get_output_info_reply_t *OutputInfo = 
            xcb_randr_get_output_info_reply(Context->Connection, OutputInfoCookie, NULL);
        
        if(OutputInfo)
        {
            int OutputNameCount = xcb_randr_get_output_info_name_length(OutputInfo);
            
            if(OutputNameCount > 0)
            {
                if(OutputNameCount == StringLength(Name))
                {
                    char *OutputName = (char *)xcb_randr_get_output_info_name(OutputInfo);
                    if(StringCompare(OutputName, Name,(size_t) OutputNameCount) == 0)
                    {
                        Result = Resources->Outputs[Index];
                        printf("Outputname: %s\n", OutputName);
                    }
                }
            }
            else
            {
                fprintf(stderr, "Could not fetch name of screen output.\n");
            }
        }
        else
        {
            // TODO(dgl): logging
        }
    }
    
    return(Result);
}

internal xcb_screen_resources
GetScreenResources(xcb_context *Context)
{
    xcb_screen_resources Result = {};
    xcb_screen_t *Screen = xcb_setup_roots_iterator(xcb_get_setup(Context->Connection)).data;
    xcb_randr_get_screen_resources_current_cookie_t ScreenResourcesCookie = xcb_randr_get_screen_resources_current(Context->Connection, Screen->root);
    
    Result.Screen = xcb_randr_get_screen_resources_current_reply(Context->Connection, ScreenResourcesCookie, NULL);
    Result.OutputCount = xcb_randr_get_screen_resources_current_outputs_length(Result.Screen);
    Result.Outputs = xcb_randr_get_screen_resources_current_outputs(Result.Screen);
    
    return(Result);
}

int main(int argc, char** argv)
{
    if(argc == 1)
    {
        backlight_provider Providers[8] = {};
        
        uint32 AvailableProviderCount = GetProviderInfo(Providers, ArrayCount(Providers));
        if(AvailableProviderCount <= 0)
        {
            fprintf(stderr, "No backlight provider found.");
            return -1;
        }
        
        for(int Index = 0; Index < AvailableProviderCount; ++Index)
        {
            backlight_provider *Provider = &Providers[Index];
            printf("Provider.Path %s\nProvider.MaxBrightness %d\nProvider.Brightness %f (%d%%)\n", Provider->PathName, Provider->MaxBrightness, Provider->Brightness, (int)(Provider->Brightness * 100));
            
        }
    }
    else
    {
        // TODO(dgl): Use cursor to handle more than one arg @@cleanup
        int cursor = 1;
        while(cursor < argc)
        {
            char *Arg = argv[cursor];
            
            if(StringCompare("-set", Arg, 4) == 0)
            {
                if(cursor == argc - 1)
                {
                    fprintf(stderr, "Missing brightness percentage after \"-set\".\n");
                    return -1;
                }
                char *BrightnessArg = argv[cursor + 1];
                
                backlight_provider Providers[8] = {};
                
                uint32 AvailableProviderCount = GetProviderInfo(Providers, ArrayCount(Providers));
                if(AvailableProviderCount <= 0)
                {
                    fprintf(stderr, "No backlight provider found.");
                    return -1;
                }
                // TODO(dgl): Allow choosing a specific provider
                backlight_provider *Provider = &Providers[0];
                
                size_t ArgLength = StringLength(BrightnessArg);
                uint32 BrightnessPercent = AToUInt32(BrightnessArg, ArgLength);
                
                uint32 Brightness;
                if(BrightnessPercent < 100)
                {
                    Brightness = (uint32)(((real32)BrightnessPercent/100) * (real32)Provider->MaxBrightness);
                }
                else
                {
                    Brightness = Provider->MaxBrightness;
                }
                
                char CurrentBrightnessPath[4096];
                size_t CurrentBrightnessFileNameCount = StringLength(CURRENT_BRIGHTNESS_FILENAME);
                StringConcat(Provider->PathName, Provider->PathNameCount,
                             CURRENT_BRIGHTNESS_FILENAME, CurrentBrightnessFileNameCount,
                             CurrentBrightnessPath, ArrayCount(CurrentBrightnessPath));
                
                WriteBrightnessToFile(CurrentBrightnessPath, Brightness);
                
                cursor += 2;
            }
            else if(StringCompare("-mirror", Arg, 7) == 0)
            {
                // NOTE(dgl): we used https://github.com/Airblader/xedgewarp as reference
                // https://stackoverflow.com/questions/36966900/xcb-get-all-monitors-ands-their-x-y-coordinates
                
                xcb_context Context = {};
                Context.Connection = xcb_connect(NULL, NULL);
                
                xcb_screen_resources Resources = GetScreenResources(&Context);
                
                xcb_randr_output_t OutputeDP1 = GetScreenOutputByName(&Context, &Resources, "eDP-1");
                xcb_randr_output_t OutputHDMI1 = GetScreenOutputByName(&Context, &Resources, "HDMI-1");
                
                //OutputeDP1->connection == XCB_RANDR_CONNECTION_DISCONNECTED;
                
                //SetOutputProperty(OutputHDMI1, "Broadcast RGB");
                //SetOutputProperty(OutputHDMI1, "Limited 16:235");
                
                //SetOutputCRTC(OutputeDP1, AUTOMATIC);
                //SetOutputCRTC(OutputHDMI1, AUTOMATIC);
                
                // TODO(dgl): Somehow we have to do something with these outputs...
                // xcb_randr_get_output_property ('Broadcast RGB' 'Limited 16:235')
                // xcb_randr_get_output_info_modes + xcb_randr_get_output_info_modes_length
                // xcb_randr_set_crtc_config
                // -off -> crtc none and mode none?!
                // -auto -> crtc mode automatic?!
                
                // NOTE(dgl): We do not free because the programm terminates after executing the necessary commands.
                // This tool should not be used as long running service.
                
                cursor++;
            }
            else if(StringCompare("-laptop", Arg, 6) == 0)
            {
                cursor++;
            }
            else if(StringCompare("-hdmi", Arg, 5) == 0)
            {
                cursor++;
            }
            else
            {
                // TODO(dgl): Print help/usage and exit
                cursor++;
            }
        }
    }
    
    return 0;
}