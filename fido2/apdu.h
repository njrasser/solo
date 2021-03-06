#ifndef _APDU_H_
#define _APDU_H_

#include <stdint.h>

typedef struct
{
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    uint8_t lc;
} __attribute__((packed)) APDU_HEADER;

#define APDU_FIDO_U2F_REGISTER        0x01
#define APDU_FIDO_U2F_AUTHENTICATE    0x02
#define APDU_FIDO_U2F_VERSION         0x03
#define APDU_FIDO_NFCCTAP_MSG         0x10
#define APDU_INS_SELECT               0xA4
#define APDU_INS_READ_BINARY          0xB0

#define SW_SUCCESS                    0x9000
#define SW_GET_RESPONSE               0x6100  // Command successfully executed; 'XX' bytes of data are available and can be requested using GET RESPONSE.
#define SW_WRONG_LENGTH               0x6700
#define SW_COND_USE_NOT_SATISFIED     0x6985
#define SW_FILE_NOT_FOUND             0x6a82
#define SW_INS_INVALID                0x6d00  // Instruction code not supported or invalid
#define SW_INTERNAL_EXCEPTION         0x6f00

#endif //_APDU_H_
