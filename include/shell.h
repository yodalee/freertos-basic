#ifndef SHELL_H
#define SHELL_H

#include "FreeRTOS.h"
#include "task.h"

int parse_command(char *str, char *argv[]);

typedef void cmdfunc(int, char *[]);

cmdfunc *do_command(const char *str);

typedef struct argument {
  cmdfunc *fptr;
  int n;
  char *argv[20];
  xTaskHandle parent;
} xShellArg;

#endif
