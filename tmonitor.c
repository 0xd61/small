/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Tool: Monitor (execute with tmonitor)
Author  : Daniel Glinka

This tool combines parts of xbacklight, xrandr and redshift to control backlight, external screens and redshift (night mode). It is not a full replacement for these tools.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
#include "helpers/types.h"
#include <dirent.h> // dirent, readdir
#include <fcntl.h> // open, close
#include <unistd.h> // read, write
#include <stdio.h>

typedef struct dirent dirent;

typedef struct backlight_provider
{
    char PathName[4096];
    size_t PathNameCount;
    real32 Brightness; // In percent (0 to 1)
    uint32 MaxBrightness;
} backlight_provider;

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
    while(Result == 0 && Cursor < StringCount)
    {
        Result = *StringA - *StringB;
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
        size_t WriteCount = 0;
        
        // NOTE(dgl): Overkill but who cares
        char Buffer[128] = {};
        UInt32ToA(Brightness, Buffer, ArrayCount(Buffer));
        write(FileHandle, Buffer, ArrayCount(Buffer));
        if(WriteCount >= 0)
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

int main(int argc, char** argv)
{
    if(argc < 2)
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
    
    // TODO(dgl): Use cursor to handle more than one arg @@cleanup
    if(argc >= 2)
    {
        char *Arg = argv[1];
        if(StringCompare(Arg, "-set", 4) == 0)
        {
            if(argc < 2)
            {
                fprintf(stderr, "Missing brightness percentage after \"-set\".\n");
                return -1;
            }
            
            backlight_provider Providers[8] = {};
            
            uint32 AvailableProviderCount = GetProviderInfo(Providers, ArrayCount(Providers));
            if(AvailableProviderCount <= 0)
            {
                fprintf(stderr, "No backlight provider found.");
                return -1;
            }
            // TODO(dgl): Allow choosing a specific provider
            backlight_provider *Provider = &Providers[0];
            
            char *BrightnessArg = argv[2];
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
        }
    }
    
    return 0;
}