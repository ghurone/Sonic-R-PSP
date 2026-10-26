#ifndef SONICR_FILEIO_H
#define SONICR_FILEIO_H

#include <stdio.h>

#ifdef SONICR_DC
#include <kos.h>

/* extern mutex_t io_lock; */

/* Save files (SONICR.INF, JOYSTICK.INF, SAVE/R0N.SAV) route to the VMU;
 * all other paths fall through to fopen with the /pc dcload fallback.
 * Implemented in dc/src/save_vmu.c. */
FILE  *sr_fOpen(const char *path, const char *mode);
size_t sr_fRead(void *buffer, size_t elementSize, size_t elementCount, FILE *file);
size_t sr_fWrite(const void *buffer, size_t elementSize, size_t elementCount, FILE *file);
int    sr_fClose(FILE *file);
int    sr_fSeek(FILE *file, long offset, int whence);
long   sr_fTell(FILE *file);
int    sr_fError(FILE *file);

#define fOpen(path, mode)                               sr_fOpen((path), (mode))
#define fRead(buffer, elementSize, elementCount, file)  sr_fRead((buffer), (elementSize), (elementCount), (file))
#define fSeek(file, offset, whence)                     sr_fSeek((file), (offset), (whence))
#define fTell(file)                                     sr_fTell(file)
#define fClose(file)                                    sr_fClose(file)
#define fWrite(buffer, elementSize, elementCount, file) sr_fWrite((buffer), (elementSize), (elementCount), (file))
#define fError(file)                                    sr_fError(file)
#elif defined(SONICR_PSP)
/* fopen with a 32KB stdio buffer, so the loaders' small reads come out of
 * RAM instead of each reaching the Memory Stick (psp/src/platform_psp.c). */
FILE  *sr_fOpen(const char *path, const char *mode);
int    sr_fClose(FILE *file);

/* What a read wants already in the stream's buffer: copied from it as
 * newlib's own getc_unlocked does (its _p/_r), no lock or call — the
 * loaders read a byte or two at a time (ReadShort_LT, LoadLightingTable),
 * hundreds of thousands a track, and each fread's fixed cost was half its
 * load time. Anything else through fread. */
#include <string.h>
static inline size_t sr_fRead(void *buffer, size_t elementSize, size_t elementCount, FILE *file)
{
    size_t total = elementSize * elementCount;
    if (total != 0 && file->_r > 0 && (size_t)file->_r >= total) {
        memcpy(buffer, file->_p, total);
        file->_p += total;
        file->_r -= (int)total;
        return elementCount;
    }
    return fread(buffer, elementSize, elementCount, file);
}

#define fOpen(path, mode)                               sr_fOpen((path), (mode))
#define fRead(buffer, elementSize, elementCount, file)  sr_fRead(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    sr_fClose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fError(file)                                    ferror(file)
#else
#define fOpen(path, mode)                               fopen(path, mode)
#define fRead(buffer, elementSize, elementCount, file)  fread(buffer, elementSize, elementCount, file)
#define fSeek(file, offset, whence)                     fseek(file, offset, whence)
#define fTell(file)                                     ftell(file)
#define fClose(file)                                    fclose(file)
#define fWrite(buffer, elementSize, elementCount, file) fwrite(buffer, elementSize, elementCount, file)
#define fError(file)                                    ferror(file)
#endif

/* Consoles read image and track data with one fRead per block instead of one
 * per byte: on the DC each call takes io_lock against the music stream, on the
 * PSP each refill of the stdio buffer is a Memory Stick read. */
#if defined(SONICR_DC) || defined(SONICR_PSP)
#define SONICR_BULK_READ 1
#endif

#endif
