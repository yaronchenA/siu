/* Firmware version — reported in FW_INFO and written into the image header. */
#ifndef VERSION_H
#define VERSION_H

#define FW_MAJOR 0u
#define FW_MINOR 5u
#define FW_PATCH 0u

#ifndef BUILD_ID
#define BUILD_ID 0u   /* set by the Makefile to the short git hash */
#endif

#endif /* VERSION_H */
