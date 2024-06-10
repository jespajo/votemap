//
//  char *comment = Grab(/*
//      Hello! This is a stupid way of doing multi-line strings in C! A hack that only works if
//      the program runs from its own source directory.
//
//      #include "grab.h" in a single source file.
//
//      Grab() returns a pointer that the caller is responsible for freeing (though I can't imagine
//      you'd use this in a situation where you care about freeing memory).
//  */);
//
#ifndef GRAB_H_INCLUDED
#define GRAB_H_INCLUDED

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#define Grab()  grab_comment(__FILE__, __LINE__)

char *grab_comment(char *file_name, int start_line)
{
    FILE *file = fopen(file_name, "r");
    if (!file)  return NULL;

    int text_length =  0;
    int start_index = -1;
    int line_number =  1;

    int c;
    while ((c = fgetc(file)) != EOF) {
        text_length += 1;

        if (line_number < start_line) {
            // We're looking for the line.
            if (c == '\n')  line_number += 1;
            continue;
        }

        if (start_index < 0) {
            // We're looking for the start of the comment.
            if (c != '/')  continue;

            if ((c = fgetc(file)) == EOF)  break;
            text_length += 1;

            if (c == '*')  start_index = text_length;
        } else {
            // We're looking for the end of the comment.
            if (c != '*')  continue;

            if ((c = fgetc(file)) == EOF)  break;
            text_length += 1;

            if (c == '/')  break;
        }
    }

    if (c == EOF)  return NULL; // We couldn't find a complete comment after the specified line number.

    // text_length is now the index of the character after the backslash at the end of the comment.
    int comment_length = text_length - start_index - 2;

    rewind(file);
    for (int i = 0; i < start_index; i++)  fgetc(file);

    char *buffer = malloc(comment_length+1);

    for (int i = 0; i < comment_length; i++)  buffer[i] = (char)fgetc(file);
    buffer[comment_length] = '\0';

    fclose(file);

    return buffer;
}
#endif // GRAB_H_INCLUDED
