#include <string.h>

#include "stm32l4xx.h"

#include "nfc.h"
#include "ams.h"
#include "log.h"
#include "util.h"
#include "device.h"
#include "u2f.h"
#include "crypto.h"

#include "ctap_errors.h"

#define IS_IRQ_ACTIVE()         (1  == (LL_GPIO_ReadInputPort(SOLO_AMS_IRQ_PORT) & SOLO_AMS_IRQ_PIN))

// Capability container
const CAPABILITY_CONTAINER NFC_CC = {
    .cclen_hi = 0x00, .cclen_lo = 0x0f,
    .version = 0x20,
    .MLe_hi = 0x00, .MLe_lo = 0x7f,
    .MLc_hi = 0x00, .MLc_lo = 0x7f,
    .tlv = { 0x04,0x06,
            0xe1,0x04,
            0x00,0x7f,
            0x00,0x00 }
};

// 13 chars
uint8_t NDEF_SAMPLE[] = "\x00\x14\xd1\x01\x0eU\x04solokeys.com/";

// Poor way to get some info while in passive operation
#include <stdarg.h>
void nprintf(const char *format, ...)
{
    memmove((char*)NDEF_SAMPLE + sizeof(NDEF_SAMPLE) - 1 - 13,"             ", 13);
    va_list args;
    va_start (args, format);
    vsnprintf ((char*)NDEF_SAMPLE + sizeof(NDEF_SAMPLE) - 1 - 13, 13, format, args);
    va_end (args);
}

static struct
{
    uint8_t max_frame_size;
    uint8_t cid;
    uint8_t block_num;
    uint8_t selected_applet;
} NFC_STATE;

void nfc_state_init()
{
    memset(&NFC_STATE,0,sizeof(NFC_STATE));
    NFC_STATE.max_frame_size = 32;
    NFC_STATE.block_num = 1;
}

bool nfc_init()
{
    uint32_t t1;
    nfc_state_init();
    ams_init();

    // Detect if we are powered by NFC field by listening for a message for
    // first 10 ms.
    t1 = millis();
    while ((millis() - t1) < 10)
    {
        if (nfc_loop() > 0)
            return 1;
    }

    // Under USB power.  Configure AMS chip.
    ams_configure();

    return 0;
}

void process_int0(uint8_t int0)
{

}

bool ams_wait_for_tx(uint32_t timeout_ms)
{
	uint32_t tstart = millis();
	while (tstart + timeout_ms > millis())
	{
		uint8_t int0 = ams_read_reg(AMS_REG_INT0);
		if (int0) process_int0(int0);
		if (int0 & AMS_INT_TXE)
			return true;

		delay(1);
	}

	return false;
}

bool ams_receive_with_timeout(uint32_t timeout_ms, uint8_t * data, int maxlen, int *dlen)
{
	uint8_t buf[32];
	*dlen = 0;

	uint32_t tstart = millis();
	while (tstart + timeout_ms > millis())
	{
		uint8_t int0 = ams_read_reg(AMS_REG_INT0);
		uint8_t buffer_status2 = ams_read_reg(AMS_REG_BUF2);

        if (buffer_status2 && (int0 & AMS_INT_RXE))
        {
            if (buffer_status2 & AMS_BUF_INVALID)
            {
                printf1(TAG_NFC,"Buffer being updated!\r\n");
            }
            else
            {
                uint8_t len = buffer_status2 & AMS_BUF_LEN_MASK;
                ams_read_buffer(buf, len);
				printf1(TAG_NFC_APDU, ">> ");
				dump_hex1(TAG_NFC_APDU, buf, len);

				*dlen = MIN(32, MIN(maxlen, len));
				memcpy(data, buf, *dlen);

				return true;
            }
        }

		delay(1);
	}

	return false;
}

void nfc_write_frame(uint8_t * data, uint8_t len)
{
    if (len > 32)
    {
        len = 32;
    }
    ams_write_command(AMS_CMD_CLEAR_BUFFER);
    ams_write_buffer(data,len);
    ams_write_command(AMS_CMD_TRANSMIT_BUFFER);

    printf1(TAG_NFC_APDU, "<< ");
	dump_hex1(TAG_NFC_APDU, data, len);
}

bool nfc_write_response_ex(uint8_t req0, uint8_t * data, uint8_t len, uint16_t resp)
{
    uint8_t res[32];

	if (len > 32 - 3)
		return false;

	res[0] = NFC_CMD_IBLOCK | (req0 & 3);

	if (len && data)
		memcpy(&res[1], data, len);

	res[len + 1] = resp >> 8;
	res[len + 2] = resp & 0xff;
	nfc_write_frame(res, 3 + len);

	return true;
}

bool nfc_write_response(uint8_t req0, uint16_t resp)
{
	return nfc_write_response_ex(req0, NULL, 0, resp);
}

void nfc_write_response_chaining(uint8_t req0, uint8_t * data, int len)
{
    uint8_t res[32 + 2];
	int sendlen = 0;
	uint8_t iBlock = NFC_CMD_IBLOCK | (req0 & 3);

	if (len <= 31)
	{
		uint8_t res[32] = {0};
		res[0] = iBlock;
		if (len && data)
			memcpy(&res[1], data, len);
		nfc_write_frame(res, len + 1);
	} else {
		do {
			// transmit I block
			int vlen = MIN(31, len - sendlen);
			res[0] = iBlock;
			memcpy(&res[1], &data[sendlen], vlen);

			// if not a last block
			if (vlen + sendlen < len)
			{
				res[0] |= 0x10;
			}

			// send data
			nfc_write_frame(res, vlen + 1);
			sendlen += vlen;

			// wait for transmit (32 bytes aprox 2,5ms)
			// if (!ams_wait_for_tx(10))
			// {
			// 	printf1(TAG_NFC, "TX timeout. slen: %d \r\n", sendlen);
			// 	break;
			// }

			// if needs to receive R block (not a last block)
			if (res[0] & 0x10)
			{
				uint8_t recbuf[32] = {0};
				int reclen;
				if (!ams_receive_with_timeout(100, recbuf, sizeof(recbuf), &reclen))
				{
					printf1(TAG_NFC, "R block RX timeout %d/%d.\r\n",sendlen,len);
					break;
				}

				if (reclen != 1)
				{
					printf1(TAG_NFC, "R block length error. len: %d. %d/%d \r\n", reclen,sendlen,len);
                    dump_hex1(TAG_NFC, recbuf, reclen);
					break;
				}

				if (((recbuf[0] & 0x01) == (res[0] & 1)) && ((recbuf[0] & 0xf6) == 0xa2))
				{
					printf1(TAG_NFC, "R block error. txdata: %02x rxdata: %02x \r\n", res[0], recbuf[0]);
					break;
				}
			}

			iBlock ^= 0x01;
		} while (sendlen < len);
	}
}

// WTX on/off:
// sends/receives WTX frame to reader every `WTX_time` time in ms
// works via timer interrupts
// WTX: f2 01 91 40 === f2(S-block + WTX, frame without CID) 01(from iso - multiply WTX from ATS by 1) <2b crc16>
static bool WTX_sent;
static bool WTX_fail;
static uint32_t WTX_timer;

bool WTX_process(int read_timeout);

void WTX_clear()
{
	WTX_sent = false;
	WTX_fail = false;
	WTX_timer = 0;
}

bool WTX_on(int WTX_time)
{
	WTX_clear();
	WTX_timer = millis();

	return true;
}

bool WTX_off()
{
	WTX_timer = 0;

	// read data if we sent WTX
	if (WTX_sent)
	{
		if (!WTX_process(100))
		{
			printf1(TAG_NFC, "WTX-off get last WTX error\n");
			return false;
		}
	}

	if (WTX_fail)
	{
		printf1(TAG_NFC, "WTX-off fail\n");
		return false;
	}

	WTX_clear();
	return true;
}

void WTX_timer_exec()
{
	// condition: (timer on) or (not expired[300ms])
	if ((WTX_timer <= 0) || WTX_timer + 300 > millis())
		return;

	WTX_process(10);
	WTX_timer = millis();
}

// executes twice a period. 1st for send WTX, 2nd for check the result
// read timeout must be 10 ms to call from interrupt
bool WTX_process(int read_timeout)
{
	uint8_t wtx[] = {0xf2, 0x01};
	if (WTX_fail)
		return false;

	if (!WTX_sent)
	{
		nfc_write_frame(wtx, sizeof(wtx));
		WTX_sent = true;
		return true;
	}
	else
	{
		uint8_t data[32];
		int len;
		if (!ams_receive_with_timeout(read_timeout, data, sizeof(data), &len))
		{
			WTX_fail = true;
			return false;
		}

		if (len != 2 || data[0] != 0xf2 || data[1] != 0x01)
		{
			WTX_fail = true;
			return false;
		}

		WTX_sent = false;
		return true;
	}
}

int answer_rats(uint8_t parameter)
{

    uint8_t fsdi = (parameter & 0xf0) >> 4;
    uint8_t cid = (parameter & 0x0f);

    NFC_STATE.cid = cid;

    if (fsdi == 0)
        NFC_STATE.max_frame_size = 16;
    else if (fsdi == 1)
        NFC_STATE.max_frame_size = 24;
    else
        NFC_STATE.max_frame_size = 32;

    uint8_t res[3 + 11];
    res[0] = sizeof(res);
    res[1] = 2 | (1<<5);     // 2 FSCI == 32 byte frame size, TB is enabled

    // frame wait time = (256 * 16 / 13.56MHz) * 2^FWI
    // FWI=0, FMT=0.3ms (min)
    // FWI=4, FMT=4.8ms (default)
    // FWI=10, FMT=309ms
    // FWI=12, FMT=1237ms
    // FWI=14, FMT=4949ms (max)
    res[2] = (12<<4) | (0);     // TB (FWI << 4) | (SGTI)

	// historical bytes
	memcpy(&res[3], (uint8_t *)"SoloKey tap", 11);


    nfc_write_frame(res, sizeof(res));
	ams_wait_for_tx(10);


    return 0;
}

void rblock_acknowledge()
{
    uint8_t buf[32];
    NFC_STATE.block_num = !NFC_STATE.block_num;
    buf[0] = NFC_CMD_RBLOCK | NFC_STATE.block_num;
    nfc_write_frame(buf,1);
}

// Selects application.  Returns 1 if success, 0 otherwise
int select_applet(uint8_t * aid, int len)
{
    if (memcmp(aid,AID_FIDO,sizeof(AID_FIDO)) == 0)
    {
        NFC_STATE.selected_applet = APP_FIDO;
        return APP_FIDO;
    }
    else if (memcmp(aid,AID_NDEF_TYPE_4,sizeof(AID_NDEF_TYPE_4)) == 0)
    {
        NFC_STATE.selected_applet = APP_NDEF_TYPE_4;
        return APP_NDEF_TYPE_4;
    }
    else if (memcmp(aid,AID_CAPABILITY_CONTAINER,sizeof(AID_CAPABILITY_CONTAINER)) == 0)
    {
        NFC_STATE.selected_applet = APP_CAPABILITY_CONTAINER;
        return APP_CAPABILITY_CONTAINER;
    }
    else if (memcmp(aid,AID_NDEF_TAG,sizeof(AID_NDEF_TAG)) == 0)
    {
        NFC_STATE.selected_applet = APP_NDEF_TAG;
        return APP_NDEF_TAG;
    }
    return APP_NOTHING;
}

void nfc_process_iblock(uint8_t * buf, int len)
{
    APDU_HEADER * apdu = (APDU_HEADER *)(buf + 1);
    uint8_t * payload = buf + 1 + 5;
    uint8_t plen = apdu->lc;
    int selected;
    CTAP_RESPONSE ctap_resp;
    int status;

    printf1(TAG_NFC,"Iblock: ");
	dump_hex1(TAG_NFC, buf, len);

    // TODO this needs to be organized better
    switch(apdu->ins)
    {
        case APDU_INS_SELECT:
            if (plen > len - 6)
            {
                printf1(TAG_ERR, "Truncating APDU length %d\r\n", apdu->lc);
                plen = len-6;
            }
            // if (apdu->p1 == 0 && apdu->p2 == 0x0c)
            // {
            //     printf1(TAG_NFC,"Select NDEF\r\n");
            //
            //     NFC_STATE.selected_applet = APP_NDEF_TAG;
            //     // Select NDEF file!
            //     res[0] = NFC_CMD_IBLOCK | (buf[0] & 1);
            //     res[1] = SW_SUCCESS>>8;
            //     res[2] = SW_SUCCESS & 0xff;
            //     nfc_write_frame(res, 3);
            //     printf1(TAG_NFC,"<< "); dump_hex1(TAG_NFC,res, 3);
            // }
            // else
            {
                selected = select_applet(payload, plen);
                if (selected == APP_FIDO)
                {
                    // block = buf[0] & 1;
                    // block = NFC_STATE.block_num;
                    // block = !block;
                    // NFC_STATE.block_num = block;
                    // NFC_STATE.block_num = block;
					nfc_write_response_ex(buf[0], (uint8_t *)"U2F_V2", 6, SW_SUCCESS);
					printf1(TAG_NFC, "FIDO applet selected.\r\n");
               }
               else if (selected != APP_NOTHING)
               {
                   nfc_write_response(buf[0], SW_SUCCESS);
                   printf1(TAG_NFC, "SELECTED %d\r\n", selected);
               }
                else
                {
					nfc_write_response(buf[0], SW_FILE_NOT_FOUND);
                    printf1(TAG_NFC, "NOT selected\r\n"); dump_hex1(TAG_NFC,payload, plen);
                }
            }
        break;

        case APDU_FIDO_U2F_VERSION:
			if (NFC_STATE.selected_applet != APP_FIDO) {
				nfc_write_response(buf[0], SW_INS_INVALID);
				break;
			}

			printf1(TAG_NFC, "U2F GetVersion command.\r\n");

			nfc_write_response_ex(buf[0], (uint8_t *)"U2F_V2", 6, SW_SUCCESS);
        break;

        case APDU_FIDO_U2F_REGISTER:
			if (NFC_STATE.selected_applet != APP_FIDO) {
				nfc_write_response(buf[0], SW_INS_INVALID);
				break;
			}

			printf1(TAG_NFC, "U2F Register command.\r\n");

			if (plen != 64)
			{
				printf1(TAG_NFC, "U2F Register request length error. len=%d.\r\n", plen);
				nfc_write_response(buf[0], SW_WRONG_LENGTH);
				return;
			}

			timestamp();


			// WTX_on(WTX_TIME_DEFAULT);
            // SystemClock_Config_LF32();
            // delay(300);
            device_set_clock_rate(DEVICE_LOW_POWER_FAST);;
			u2f_request_nfc(&buf[1], len, &ctap_resp);
            device_set_clock_rate(DEVICE_LOW_POWER_IDLE);;
			// if (!WTX_off())
			// 	return;

            printf1(TAG_NFC,"U2F Register P2 took %d\r\n", timestamp());
            nfc_write_response_chaining(buf[0], ctap_resp.data, ctap_resp.length);

			// printf1(TAG_NFC, "U2F resp len: %d\r\n", ctap_resp.length);




            printf1(TAG_NFC,"U2F Register answered %d (took %d)\r\n", millis(), timestamp());
       break;

        case APDU_FIDO_U2F_AUTHENTICATE:
			if (NFC_STATE.selected_applet != APP_FIDO) {
				nfc_write_response(buf[0], SW_INS_INVALID);
				break;
			}

			printf1(TAG_NFC, "U2F Authenticate command.\r\n");

			if (plen != 64 + 1 + buf[6 + 64])
			{
				delay(5);
				printf1(TAG_NFC, "U2F Authenticate request length error. len=%d keyhlen=%d.\r\n", plen, buf[6 + 64]);
				nfc_write_response(buf[0], SW_WRONG_LENGTH);
				return;
			}

			timestamp();
			// WTX_on(WTX_TIME_DEFAULT);
			u2f_request_nfc(&buf[1], len, &ctap_resp);
			// if (!WTX_off())
			// 	return;

			printf1(TAG_NFC, "U2F resp len: %d\r\n", ctap_resp.length);
            printf1(TAG_NFC,"U2F Authenticate processing %d (took %d)\r\n", millis(), timestamp());
			nfc_write_response_chaining(buf[0], ctap_resp.data, ctap_resp.length);
            printf1(TAG_NFC,"U2F Authenticate answered %d (took %d)\r\n", millis(), timestamp);
        break;

        case APDU_FIDO_NFCCTAP_MSG:
			if (NFC_STATE.selected_applet != APP_FIDO) {
				nfc_write_response(buf[0], SW_INS_INVALID);
				break;
			}

			printf1(TAG_NFC, "FIDO2 CTAP message. %d\r\n", timestamp());

			WTX_on(WTX_TIME_DEFAULT);
            ctap_response_init(&ctap_resp);
            status = ctap_request(payload, plen, &ctap_resp);
			if (!WTX_off())
				return;

			printf1(TAG_NFC, "CTAP resp: 0x%02�  len: %d\r\n", status, ctap_resp.length);

			if (status == CTAP1_ERR_SUCCESS)
			{
				memmove(&ctap_resp.data[1], &ctap_resp.data[0], ctap_resp.length);
				ctap_resp.length += 3;
			} else {
				ctap_resp.length = 3;
			}
			ctap_resp.data[0] = status;
			ctap_resp.data[ctap_resp.length - 2] = SW_SUCCESS >> 8;
			ctap_resp.data[ctap_resp.length - 1] = SW_SUCCESS & 0xff;

            printf1(TAG_NFC,"CTAP processing %d (took %d)\r\n", millis(), timestamp());
			nfc_write_response_chaining(buf[0], ctap_resp.data, ctap_resp.length);
            printf1(TAG_NFC,"CTAP answered %d (took %d)\r\n", millis(), timestamp());
        break;

        case APDU_INS_READ_BINARY:


            switch(NFC_STATE.selected_applet)
            {
                case APP_CAPABILITY_CONTAINER:
                    printf1(TAG_NFC,"APP_CAPABILITY_CONTAINER\r\n");
                    if (plen > 15)
                    {
                        printf1(TAG_ERR, "Truncating requested CC length %d\r\n", apdu->lc);
                        plen = 15;
                    }
                    nfc_write_response_ex(buf[0], (uint8_t *)&NFC_CC, plen, SW_SUCCESS);
                    ams_wait_for_tx(10);
                break;
                case APP_NDEF_TAG:
                    printf1(TAG_NFC,"APP_NDEF_TAG\r\n");
                    if (plen > (sizeof(NDEF_SAMPLE) -  1))
                    {
                        printf1(TAG_ERR, "Truncating requested CC length %d\r\n", apdu->lc);
                        plen = sizeof(NDEF_SAMPLE) -  1;
                    }
                    nfc_write_response_ex(buf[0], NDEF_SAMPLE, plen, SW_SUCCESS);
                    ams_wait_for_tx(10);
                break;
                default:
                    printf1(TAG_ERR, "No binary applet selected!\r\n");
                    return;
                break;
            }

        break;
        default:
            printf1(TAG_NFC, "Unknown INS %02x\r\n", apdu->ins);
			nfc_write_response(buf[0], SW_INS_INVALID);
        break;
    }


}

static uint8_t ibuf[1024];
static int ibuflen = 0;

void clear_ibuf()
{
	ibuflen = 0;
	memset(ibuf, 0, sizeof(ibuf));
}

void nfc_process_block(uint8_t * buf, unsigned int len)
{

	if (!len)
		return;

    if (IS_PPSS_CMD(buf[0]))
    {
        printf1(TAG_NFC, "NFC_CMD_PPSS\r\n");
    }
    else if (IS_IBLOCK(buf[0]))
    {
		if (buf[0] & 0x10)
		{
			printf1(TAG_NFC_APDU, "NFC_CMD_IBLOCK chaining blen=%d len=%d\r\n", ibuflen, len);
			if (ibuflen + len > sizeof(ibuf))
			{
				printf1(TAG_NFC, "I block memory error! must have %d but have only %d\r\n", ibuflen + len, sizeof(ibuf));
				nfc_write_response(buf[0], SW_INTERNAL_EXCEPTION);
				return;
			}

			printf1(TAG_NFC_APDU,"i> ");
			dump_hex1(TAG_NFC_APDU, buf, len);

			if (len)
			{
				memcpy(&ibuf[ibuflen], &buf[1], len - 1);
				ibuflen += len - 1;
			}

			// send R block
			uint8_t rb = NFC_CMD_RBLOCK | NFC_CMD_RBLOCK_ACK | (buf[0] & 3);
			nfc_write_frame(&rb, 1);
		} else {
			if (ibuflen)
			{
				if (len)
				{
					memcpy(&ibuf[ibuflen], &buf[1], len - 1);
					ibuflen += len - 1;
				}

				memmove(&ibuf[1], ibuf, ibuflen);
				ibuf[0] = buf[0];
				ibuflen++;

				printf1(TAG_NFC_APDU, "NFC_CMD_IBLOCK chaining last block. blen=%d len=%d\r\n", ibuflen, len);

				printf1(TAG_NFC_APDU,"i> ");
				dump_hex1(TAG_NFC_APDU, buf, len);

				nfc_process_iblock(ibuf, ibuflen);
			} else {
				// printf1(TAG_NFC, "NFC_CMD_IBLOCK\r\n");
				nfc_process_iblock(buf, len);
			}
			clear_ibuf();
		}
    }
    else if (IS_RBLOCK(buf[0]))
    {
        rblock_acknowledge();
        printf1(TAG_NFC, "NFC_CMD_RBLOCK\r\n");
    }
    else if (IS_SBLOCK(buf[0]))
    {

        if ((buf[0] & NFC_SBLOCK_DESELECT) == 0)
        {
            printf1(TAG_NFC, "NFC_CMD_SBLOCK, DESELECTED\r\n");
            nfc_write_frame(buf, 1);
            ams_wait_for_tx(2);
            ams_write_command(AMS_CMD_SLEEP);
            nfc_state_init();
			clear_ibuf();
			WTX_clear();
        }
        else
        {
            printf1(TAG_NFC, "NFC_CMD_SBLOCK, Unknown. len[%d]\r\n", len);
        }
        dump_hex1(TAG_NFC, buf, len);
    }
    else
    {
        printf1(TAG_NFC, "unknown NFC request\r\n len[%d]:", len);
        dump_hex1(TAG_NFC, buf, len);
    }
}

int nfc_loop()
{
    uint8_t buf[32];
    AMS_DEVICE ams;
    int len = 0;


    read_reg_block(&ams);
    uint8_t state = AMS_STATE_MASK & ams.regs.rfid_status;

    if (state != AMS_STATE_SELECTED && state != AMS_STATE_SELECTEDX)
    {
        // delay(1);  // sleep ?
        return 0;
    }

    if (ams.regs.rfid_status)
    {
        // if (state != AMS_STATE_SENSE)
        //      printf1(TAG_NFC,"    %s  x%02x\r\n", ams_get_state_string(ams.regs.rfid_status), state);
    }
    if (ams.regs.int0 & AMS_INT_INIT)
    {
        nfc_state_init();
    }
    if (ams.regs.int1)
    {
        // ams_print_int1(ams.regs.int1);
    }

    if ((ams.regs.int0 & AMS_INT_RXE))
    {
        if (ams.regs.buffer_status2)
        {
            if (ams.regs.buffer_status2 & AMS_BUF_INVALID)
            {
                printf1(TAG_NFC,"Buffer being updated!\r\n");
            }
            else
            {
                len = ams.regs.buffer_status2 & AMS_BUF_LEN_MASK;
                ams_read_buffer(buf, len);
            }
        }
    }

    if (len)
    {

        // ISO 14443-3
        switch(buf[0])
        {
            case NFC_CMD_REQA:
                printf1(TAG_NFC, "NFC_CMD_REQA\r\n");
            break;
            case NFC_CMD_WUPA:
                printf1(TAG_NFC, "NFC_CMD_WUPA\r\n");
            break;
            case NFC_CMD_HLTA:
                printf1(TAG_NFC, "HLTA/Halt\r\n");
            break;
            case NFC_CMD_RATS:

                answer_rats(buf[1]);

                NFC_STATE.block_num = 1;
				clear_ibuf();
				WTX_clear();
            break;
            default:

                // ISO 14443-4
                nfc_process_block(buf,len);


            break;
        }

    }

    return len;

}
