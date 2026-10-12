#ifndef LOGVIEW_JSON_PRETTY_H
#define LOGVIEW_JSON_PRETTY_H

#include <stddef.h>
#include <stdio.h>

/* Validate and pretty-print JSON, stopping if output exceeds the given byte cap. */
int json_pretty_write(FILE *output, const unsigned char *input, size_t length,
                      size_t maximum_output_bytes);

#endif
