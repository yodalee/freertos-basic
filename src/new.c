#include "FreeRTOS.h"
#include "fio.h"
#include "clib.h"
#include "task.h"

void vTask( void *pArg) {
  for (;;) {
  }
}

void new_command(int argc, char *argv[]) {
  int rtn;
  signed char name[128] = "Dummy Task";
  
  rtn = xTaskCreate(vTask, name, 128, NULL, 1, NULL);
  if (rtn == pdPASS) {
    fio_printf(1, "\r\nCreate a dummy task successfully\r\n");
  } else {
    fio_printf(1, "\r\nCreate a dummy task failed\r\n");
  }
}
