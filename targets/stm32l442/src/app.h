#ifndef _APP_H_
#define _APP_H_
#include <stdint.h>

#define DEBUG_UART      USART1




#define DEBUG_LEVEL 1

#define NON_BLOCK_PRINTING 1

//#define PRINTING_USE_VCOM

//#define USING_DEV_BOARD

//#define ENABLE_U2F_EXTENSIONS

#define ENABLE_U2F

//#define DISABLE_CTAPHID_PING
//#define DISABLE_CTAPHID_WINK
//#define DISABLE_CTAPHID_CBOR

void printing_init();
void hw_init(void);

//#define TEST
//#define TEST_POWER

#define LED_INIT_VALUE			0x001000

#endif