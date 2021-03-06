#define USE_STDPERIPH_DRIVER
#include "stm32f10x.h"
#include "stm32_p103.h"
/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

/* Filesystem includes */
#include "filesystem.h"
#include "fio.h"
#include "romfs.h"

#include "clib.h"
#include "shell.h"
#include "host.h"

/* _sromfs symbol can be found in main.ld linker script
 * it contains file system structure of test_romfs directory
 */
extern const unsigned char _sromfs;

//static void setup_hardware();

volatile xSemaphoreHandle serial_tx_wait_sem = NULL;
/* Add for serial input */
volatile xQueueHandle serial_rx_queue = NULL;

xQueueHandle xSyslogQueue = NULL;

/* IRQ handler to handle USART2 interruptss (both transmit and receive
 * interrupts). */
void USART2_IRQHandler()
{
	static signed portBASE_TYPE xHigherPriorityTaskWoken;

	/* If this interrupt is for a transmit... */
	if (USART_GetITStatus(USART2, USART_IT_TXE) != RESET) {
		/* "give" the serial_tx_wait_sem semaphore to notfiy processes
		 * that the buffer has a spot free for the next byte.
		 */
		xSemaphoreGiveFromISR(serial_tx_wait_sem, &xHigherPriorityTaskWoken);

		/* Diables the transmit interrupt. */
		USART_ITConfig(USART2, USART_IT_TXE, DISABLE);
		/* If this interrupt is for a receive... */
	}else if(USART_GetITStatus(USART2, USART_IT_RXNE) != RESET){
		char msg = USART_ReceiveData(USART2);

		/* If there is an error when queueing the received byte, freeze! */
		if(!xQueueSendToBackFromISR(serial_rx_queue, &msg, &xHigherPriorityTaskWoken))
			while(1);
	}
	else {
		/* Only transmit and receive interrupts should be enabled.
		 * If this is another type of interrupt, freeze.
		 */
		while(1);
	}

	if (xHigherPriorityTaskWoken) {
		taskYIELD();
	}
}

void send_byte(char ch)
{
	/* Wait until the RS232 port can receive another byte (this semaphore
	 * is "given" by the RS232 port interrupt when the buffer has room for
	 * another byte.
	 */
	while (!xSemaphoreTake(serial_tx_wait_sem, portMAX_DELAY));

	/* Send the byte and enable the transmit interrupt (it is disabled by
	 * the interrupt).
	 */
	USART_SendData(USART2, ch);
	USART_ITConfig(USART2, USART_IT_TXE, ENABLE);
}

char recv_byte()
{
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
	char msg;
	while(!xQueueReceive(serial_rx_queue, &msg, portMAX_DELAY));
	return msg;
}

void exec_command(void *pvParameters)
{
  fio_printf(1, "\r\n");
  xShellArg *arg = (xShellArg *)pvParameters;
  while(1) {

    if (strcmp(arg->argv[arg->n-1], "&") == 0) {
      //run in background
      (arg->n)--;
      vTaskResume(arg->parent);
      arg->fptr(arg->n, arg->argv);
    } else {
      //run in foreground
      arg->fptr(arg->n, arg->argv);
      vTaskResume(arg->parent);
    }
    vTaskDelete(NULL);
  }
}

void command_prompt(void *pvParameters)
{
  char buf[128];
  char hint[] = USER_NAME "@" USER_NAME "-STM32:~$ ";
  xShellArg arg;

  fio_printf(1, "\rWelcome to FreeRTOS Shell\r\n");

  while(1){
    fio_printf(1, "%s", hint);

    fio_read(0, buf, 127);

    arg.n=parse_command(buf, arg.argv);

    /* will return pointer to the command function */
    arg.fptr=do_command(arg.argv[0]);

    if (arg.fptr == NULL) {
      fio_printf(2, "\r\n\"%s\" command not found.\r\n", arg.argv[0]);
      continue;
    }
    arg.parent = xTaskGetCurrentTaskHandle();

    xTaskCreate(exec_command,
                (signed portCHAR *) "bg shell",
                512 /* stack size */, (void *)&arg,
                tskIDLE_PRIORITY + 2, NULL);

    //Suspend itself, wait child resume it
    vTaskSuspend(NULL);
  }
}

void system_monitor(void *pvParameters)
{
  signed char buf[128];
  char output[512] = {0};
  char *pc = output;
  char *tag = "\nName          State   Priority  Stack  Num\n*******************************************\n";
  portBASE_TYPE xStatus;

  portTickType xLastWakeTime = xTaskGetTickCount();
  int period = (int)pvParameters;

  while(1) {
    //Print ps message;
    memcpy(output, tag, strlen(tag));
    xStatus = xQueueSendToFront( xSyslogQueue, &pc, 0);
    if (xStatus != pdPASS) {
      fio_printf(1, "Write file error! \n\r");
      return;
    }

    vTaskList(buf);

    memcpy(output, (char *)(buf + 2), strlen((char *)buf) - 2);

    xStatus = xQueueSendToFront( xSyslogQueue, &pc, 0);
    if (xStatus != pdPASS) {
      fio_printf(1, "Write file error! \n\r");
      return;
    }
    vTaskDelayUntil( &xLastWakeTime, ( period / portTICK_RATE_MS));
  }
}

void system_logger(void *pvParameters)
{
  portBASE_TYPE xStatus;
  int handle, error;
  char *msg;
  const portTickType xTicksToWait = 100 / portTICK_RATE_MS;
  
  handle = host_action(SYS_SYSTEM, "mkdir -p output");
  handle = host_action(SYS_SYSTEM, "touch output/syslog");

  handle = host_action(SYS_OPEN, "output/syslog", 4);
  if(handle == -1) {
    fio_printf(1, "Open file error!\n");
    return;
  }

  while(1) {
    //Print data in queue;
    xStatus = xQueueReceive( xSyslogQueue, &msg, xTicksToWait );
    if (xStatus == pdPASS) {
      error = host_action(SYS_WRITE, handle, msg, strlen(msg));
      if (error != 0) {
        fio_printf(1, "Write file error! Remain %d bytes didn't write in the file.\n\r", error);
      }
    }

    taskYIELD();
  }
  
  host_action(SYS_CLOSE, handle);
}

int main()
{
	init_rs232();
	enable_rs232_interrupts();
	enable_rs232();
	
	fs_init();
	fio_init();
	
	register_romfs("romfs", &_sromfs);
	
	/* Create the queue used by the serial task.  Messages for write to
	 * the RS232. */
	vSemaphoreCreateBinary(serial_tx_wait_sem);
	/* Add for serial input 
	 * Reference: www.freertos.org/a00116.html */
	serial_rx_queue = xQueueCreate(1, sizeof(char));

  register_devfs();

  /* Create xSyslogQueue for system_logger message send/recv */
  xSyslogQueue = xQueueCreate(2, sizeof(char *));

	/* Create a task to output text read from romfs. */
	xTaskCreate(command_prompt,
	            (signed portCHAR *) "CLI",
	            512 /* stack size */, NULL, tskIDLE_PRIORITY + 2, NULL);

	/* Create a task to record system log. */
	xTaskCreate(system_logger,
	            (signed portCHAR *) "Logger",
	            128 /* stack size */, NULL, tskIDLE_PRIORITY + 3, NULL);

  /* Create a task to monitor system */
  xTaskCreate(system_monitor, 
              (signed portCHAR *) "Monitor",
              1024 /* stack size */, (void *)3000, tskIDLE_PRIORITY + 2, NULL);
	/* Start running the tasks. */
	vTaskStartScheduler();

	return 0;
}

void vApplicationTickHook()
{
}
