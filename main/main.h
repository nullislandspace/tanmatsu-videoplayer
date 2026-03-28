#ifndef MAIN_H
#define MAIN_H

// Screen and Image Format Configuration
// This define controls both the screen initialization and all loaded image formats.
// All graphics are assumed to use the same format as the screen.
//
// Uncomment ONE of the following options:

// Option 1: RGB888 (24-bit color, 16.7M colors, 3 bytes per pixel)
#define SCREEN_FORMAT_RGB888

// Option 2: RGB565 (16-bit color, 65K colors, 2 bytes per pixel)
//#define SCREEN_FORMAT_RGB565

// Validation: Ensure only one format is defined
#if defined(SCREEN_FORMAT_RGB888) && defined(SCREEN_FORMAT_RGB565)
#error "Only one screen format can be defined at a time"
#endif

#if !defined(SCREEN_FORMAT_RGB888) && !defined(SCREEN_FORMAT_RGB565)
#error "Must define either SCREEN_FORMAT_RGB888 or SCREEN_FORMAT_RGB565"
#endif

#endif // MAIN_H
