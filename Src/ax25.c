/*
This file is part of VP-Digi.

VP-Digi is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

VP-Digi is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with VP-Digi.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ax25.h"
#include <stdlib.h>
#include "drivers/modem.h"
#include "common.h"
#include "drivers/systick.h"
#include <stdbool.h>
#include "digipeater.h"
#include "fx25.h"

struct Ax25ProtoConfig Ax25Config;

//values below must be kept consistent so that FRAME_BUFFER_SIZE >= FRAME_MAX_SIZE * FRAME_MAX_COUNT
#ifndef ENABLE_FX25
//for AX.25 308 bytes is the theoretical max size assuming 2-byte Control, 256-byte info field and 5 digi address fields
#define FRAME_MAX_SIZE (308) //single frame max length
#else
//in FX.25 mode the block can be 255 bytes long at most, and the AX.25 frame itself must be even smaller
//frames that are too long are sent as standard AX.25 frames
//Reed-Solomon library needs a bit of memory and the frame buffer must be smaller
//otherwise we run out of RAM
#define FRAME_MAX_SIZE (280) //single frame max length
#endif
#define FRAME_MAX_COUNT (10) //max count of frames in buffer
#define FRAME_BUFFER_SIZE (FRAME_MAX_COUNT * FRAME_MAX_SIZE) //circular frame buffer length

#define STATIC_HEADER_FLAG_COUNT 4 //number of flags sent before each frame
#define STATIC_FOOTER_FLAG_COUNT 8 //number of flags sent after each frame

#define MAX_TRANSMIT_RETRY_COUNT 8 //max number of retries if channel is busy

struct FrameHandle
{
	uint16_t start;
	uint16_t size;
	uint16_t signalLevel;
#ifdef ENABLE_FX25
	struct Fx25Mode *fx25Mode;
#endif
};

static uint8_t rxBuffer[FRAME_BUFFER_SIZE]; //circular buffer for received frames
static uint16_t rxBufferHead = 0; //circular RX buffer write index
static struct FrameHandle rxFrame[FRAME_MAX_COUNT];
static uint8_t rxFrameHead = 0;
static uint8_t rxFrameTail = 0;
static bool rxFrameBufferFull = false;

static uint8_t txBuffer[FRAME_BUFFER_SIZE];  //circular TX frame buffer
static uint16_t txBufferHead = 0; //circular TX buffer write index
static uint16_t txBufferTail = 0;
static struct FrameHandle txFrame[FRAME_MAX_COUNT];
static uint8_t txFrameHead = 0;
static uint8_t txFrameTail = 0;
static bool txFrameBufferFull = false;

#ifdef ENABLE_FX25
static uint8_t txFx25Buffer[FX25_MAX_BLOCK_SIZE];
static uint8_t txTagByteIdx = 0;
#endif

static uint8_t frameReceived; //a bitmap of receivers that received the frame


enum TxStage
{
	TX_STAGE_IDLE = 0,
	TX_STAGE_PREAMBLE,
	TX_STAGE_HEADER_FLAGS,
	TX_STAGE_DATA,
	TX_STAGE_CRC,
	TX_STAGE_FOOTER_FLAGS,
	TX_STAGE_TAIL,

#ifdef ENABLE_FX25
	//stages used in FX.25 mode additionally
	TX_STAGE_CORRELATION_TAG,
#endif
};

enum TxInitStage
{
	TX_INIT_OFF,
	TX_INIT_WAITING,
	TX_INIT_TRANSMITTING
};

static uint8_t txByte = 0; //current TX byte
static uint16_t txByteIdx = 0; //current TX byte index
static int8_t txBitIdx = 0; //current bit index in txByte
static uint16_t txDelayElapsed = 0; //counter of TXDelay bytes already sent
static uint8_t txFlagsElapsed = 0; //counter of flag bytes already sent
static uint8_t txCrcByteIdx = 0; //currently transmitted byte of CRC
static uint8_t txBitstuff = 0; //bit-stuffing counter
static uint16_t txTailElapsed; //counter of TXTail bytes already sent
static uint16_t txCrc = 0xFFFF; //current CRC
static uint32_t txQuiet = 0; //quit time + current tick value
static uint8_t txRetries = 0; //number of TX retries
static enum TxInitStage txInitStage; //current TX initialization stage
static enum TxStage txStage; //current TX stage

struct RxState
{
	uint16_t crc; //current CRC
	uint8_t frame[FRAME_MAX_SIZE]; //raw frame buffer
	uint16_t frameIdx; //index for raw frame buffer
	uint8_t receivedByte; //byte being currently received
	uint8_t receivedBitIdx; //bit index for recByte
	uint8_t rawData; //raw data being currently received
	enum Ax25RxStage rx; //current RX stage
	uint8_t frameReceived; //frame received flag
};

static volatile struct RxState rxState[MODEM_MAX_DEMODULATOR_COUNT];

static uint16_t lastCrc = 0; //CRC of the last received frame. If not 0, a frame was successfully received
static uint16_t rxMultiplexDelay = 0; //simple delay for decoder multiplexer to avoid receiving the same frame twice

static uint16_t txDelay; //number of TXDelay bytes to send
static uint16_t txTail; //number of TXTail bytes to send

static uint8_t outputFrameBuffer[FRAME_MAX_SIZE];

#define GET_FREE_SIZE(max, head, tail) (((head) < (tail)) ? ((tail) - (head)) : ((max) - (head) + (tail)))
#define GET_USED_SIZE(max, head, tail) (max - GET_FREE_SIZE(max, head, tail))

/**
 * @brief Recalculate CRC for one bit
 * @param bit Input bit
 * @param *crc CRC pointer
 */
static void calculateCRC(uint8_t bit, uint16_t *crc)
{
    uint16_t xor_result;
    xor_result = *crc ^ bit;
    *crc >>= 1;
    if (xor_result & 0x0001)
    {
    	*crc ^= 0x8408;
    }
}

uint8_t Ax25GetReceivedFrameBitmap(void)
{
	return frameReceived;
}

void Ax25ClearReceivedFrameBitmap(void)
{
	frameReceived = 0;
}

void Ax25TxKiss(uint8_t *buf, uint16_t len)
{
	if(len < 18) //frame is too small
	{
		return;
	}
	for(uint16_t i = 0; i < len; i++)
	{
		if(buf[i] == 0xC0) //frame start marker
		{
			uint16_t end = i + 1;
			while(end < len)
			{
				if(buf[end] == 0xC0)
					break;
				end++;
			}
			if(end == len) //no frame end marker found
				return;
			Ax25WriteTxFrame(&buf[i + 2], end - (i + 2)); //skip modem number and send frame
			DigiStoreDeDupe(&buf[i + 2], end - (i + 2));
			i = end; //move pointer to the next byte if there are more consecutive frames
		}
	}
}

#ifdef ENABLE_FX25
static void *writeFx25Frame(uint8_t *data, uint16_t size)
{
	//first calculate how big the frame can be
	//this includes 2 flags, 2 CRC bytes and all bits added by bitstuffing
	//bitstuffing occurs after 5 consecutive ones, so in worst scenario
	//bits inserted by bitstuffing can occupy up to frame size / 5 additional bytes
	//also add 1 in case there is a remainder when dividing
	const struct Fx25Mode *fx25Mode = fx25Mode = Fx25GetMode(size + 4 + (size / 5) + 1);
	uint16_t requiredSize = size;
	if(NULL != fx25Mode)
		requiredSize = fx25Mode->K + fx25Mode->T;
	else
		return NULL; //frame will not fit in FX.25

	uint16_t freeSize = GET_FREE_SIZE(FRAME_BUFFER_SIZE, txBufferHead, txBufferTail);
	if(freeSize < requiredSize) //check if there is enough size to store full FX.25 (or AX.25) frame
	{
		return NULL; //if not, it may fit in standard AX.25
	}

	txFrame[txFrameHead].size = requiredSize;
	txFrame[txFrameHead].start = txBufferHead;
	txFrame[txFrameHead].fx25Mode = (struct Fx25Mode*)fx25Mode;

	uint16_t index = 0;
	//header flag
	txFx25Buffer[index++] = 0x7E;

	uint16_t crc = 0xFFFF;

	uint8_t bits = 0; //bit counter within a byte
	uint8_t bitstuff = 0;
	for(uint16_t i = 0; i < size + 2; i++)
	{
		for(uint8_t k = 0; k < 8; k++)
		{
			txFx25Buffer[index] >>= 1;
			bits++;
			if(i < size) //frame data
			{
				if(data[i] >> k)
				{
					calculateCRC(1, &crc);
					bitstuff++;
					txFx25Buffer[index] |= 0x80;
				}
				else
				{
					calculateCRC(0, &crc);
					bitstuff = 0;
				}
			}
			else //crc
			{
				uint8_t c = 0;
				if(i == size)
					c = (crc & 0xFF) ^ 0xFF;
				else
					c = (crc >> 8) ^ 0xFF;

				if(c >> k)
				{
					bitstuff++;
					txFx25Buffer[index] |= 0x80;
				}
				else
				{
					bitstuff = 0;
				}
			}

			if(bits == 8)
			{
				bits = 0;
				index++;
			}
			if(bitstuff == 5)
			{
				bits++;
				bitstuff = 0;
				txFx25Buffer[index] >>= 1;
				if(bits == 8)
				{
					bits = 0;
					index++;
				}
			}
		}
	}

	//pad with flags
	while(index < fx25Mode->K)
	{
		for(uint8_t k = 0; k < 8; k++)
		{
			txFx25Buffer[index] >>= 1;
			bits++;

			if(0x7E >> k)
			{
				txFx25Buffer[index] |= 0x80;
			}

			if(bits == 8)
			{
				bits = 0;
				index++;
			}
		}
	}

	Fx25AddParity(txFx25Buffer, fx25Mode);

	for(uint16_t i = 0; i < (fx25Mode->K + fx25Mode->T); i++)
	{
		txBuffer[txBufferHead++] = txFx25Buffer[i];
		txBufferHead %= FRAME_BUFFER_SIZE;
	}

	void *ret = &txFrame[txFrameHead];
	txFrameHead++;
	txFrameHead %= FRAME_MAX_COUNT;
	if(txFrameHead == txFrameTail)
		txFrameBufferFull = true;
	return ret;
}
#endif

void *Ax25WriteTxFrame(uint8_t *data, uint16_t size)
{
	while(txStage != TX_STAGE_IDLE)
		;

	if(txFrameBufferFull)
		return NULL;

#ifdef ENABLE_FX25
	if(Ax25Config.fx25)
	{
		void *ret = writeFx25Frame(data, size);
		if(ret)
			return ret;
	}
#endif

	if(GET_FREE_SIZE(FRAME_BUFFER_SIZE, txBufferHead, txBufferTail) < size)
	{
		return NULL;
	}

	txFrame[txFrameHead].size = size;
	txFrame[txFrameHead].start = txBufferHead;

#ifdef ENABLE_FX25
	txFrame[txFrameHead].fx25Mode = NULL;
#endif


	for(uint16_t i = 0; i < size; i++)
	{
		txBuffer[txBufferHead++] = data[i];
		txBufferHead %= FRAME_BUFFER_SIZE;
	}


	void *ret = &txFrame[txFrameHead];
	txFrameHead++;
	txFrameHead %= FRAME_MAX_COUNT;
	if(txFrameHead == txFrameTail)
		txFrameBufferFull = true;
	return ret;
}


bool Ax25ReadNextRxFrame(uint8_t **dst, uint16_t *size, uint16_t *signalLevel)
{
	if((rxFrameHead == rxFrameTail) && !rxFrameBufferFull)
		return false;

	*dst = outputFrameBuffer;

	for(uint16_t i = 0; i < rxFrame[rxFrameTail].size; i++)
	{
		(*dst)[i] = rxBuffer[(rxFrame[rxFrameTail].start + i) % FRAME_BUFFER_SIZE];
	}

	*signalLevel = rxFrame[rxFrameTail].signalLevel;
	*size = rxFrame[rxFrameTail].size;

	rxFrameBufferFull = false;
	rxFrameTail++;
	rxFrameTail %= FRAME_MAX_COUNT;
	return true;
}

enum Ax25RxStage Ax25GetRxStage(uint8_t modem)
{
	return rxState[modem].rx;
}


void Ax25BitParse(uint8_t bit, uint8_t modem)
{
	if(lastCrc != 0) //there was a frame received
	{
		rxMultiplexDelay++;
		if(rxMultiplexDelay > (4 * MODEM_MAX_DEMODULATOR_COUNT)) //hold it for a while and wait for other decoders to receive the frame
		{
			lastCrc = 0;
			rxMultiplexDelay = 0;
			for(uint8_t i = 0; i < MODEM_MAX_DEMODULATOR_COUNT; i++)
			{
				frameReceived |= ((rxState[i].frameReceived > 0) << i);
				rxState[i].frameReceived = 0;
			}
		}

	}


	struct RxState *rx = (struct RxState*)&(rxState[modem]);

	rx->rawData <<= 1; //store incoming bit
	rx->rawData |= (bit > 0);


	if(rx->rawData == 0x7E) //HDLC flag received
	{
		if(rx->rx == RX_STAGE_FRAME) //if we are in frame, this is the end of the frame
		{
    		if((rx->frameIdx > 15)) //correct frame must be at least 16 bytes long
    		{
        		uint16_t i = 0;
				for(; i < rx->frameIdx - 2; i++) //look for path end bit
        		{
        			if(rx->frame[i] & 1)
        				break;
        		}

				//if non-APRS frames are not allowed, check if this frame has control=0x03 and PID=0xF0

				if(Ax25Config.allowNonAprs || (((rx->frame[i + 1] == 0x03) && (rx->frame[i + 2] == 0xf0))))
				{
					if((rx->frame[rx->frameIdx - 2] == ((rx->crc & 0xFF) ^ 0xFF)) && (rx->frame[rx->frameIdx - 1] == (((rx->crc >> 8) & 0xFF) ^ 0xFF))) //check CRC
					{
						rx->frameReceived = 1;
						rx->frameIdx -= 2; //remove CRC
						if(rx->crc != lastCrc) //the other decoder has not received this frame yet, so store it in main frame buffer
						{
							lastCrc = rx->crc; //store CRC of this frame

							if(!rxFrameBufferFull) //if enough space, store the frame
							{
								rxFrame[rxFrameHead].start = rxBufferHead;
								rxFrame[rxFrameHead].signalLevel = ModemGetRMS(modem);
								rxFrame[rxFrameHead++].size = rx->frameIdx;
								rxFrameHead %= FRAME_MAX_COUNT;
								if(rxFrameHead == txFrameHead)
									rxFrameBufferFull = true;

								for(uint16_t i = 0; i < rx->frameIdx; i++)
								{
									rxBuffer[rxBufferHead++] = rx->frame[i];
									rxBufferHead %= FRAME_BUFFER_SIZE;
								}

							}
						}
					}
				}

    		}

		}
		rx->rx = RX_STAGE_FLAG;
		ModemClearRMS(modem);
		rx->receivedByte = 0;
		rx->receivedBitIdx = 0;
		rx->frameIdx = 0;
		rx->crc = 0xFFFF;
		return;
	}


	if((rx->rawData & 0x7F) == 0x7F) //received 7 consecutive ones, this is an error (sometimes called "escape byte")
	{
		rx->rx = RX_STAGE_FLAG;
		ModemClearRMS(modem);
		rx->receivedByte = 0;
		rx->receivedBitIdx = 0;
		rx->frameIdx = 0;
		rx->crc = 0xFFFF;
		return;
	}



	if(rx->rx == RX_STAGE_IDLE) //not in a frame, don't go further
		return;


	if((rx->rawData & 0x3F) == 0x3E) //dismiss bit 0 added by bit stuffing
		return;

	if(rx->rawData & 0x01) //received bit 1
		rx->receivedByte |= 0x80; //store it

	if(++rx->receivedBitIdx >= 8) //received full byte
	{
		if(rx->frameIdx > FRAME_MAX_SIZE) //frame is too long
		{
			rx->rx = RX_STAGE_IDLE;
			ModemClearRMS(modem);
			rx->receivedByte = 0;
			rx->receivedBitIdx = 0;
			rx->frameIdx = 0;
			rx->crc = 0xFFFF;
			return;
		}
		if(rx->frameIdx >= 2) //more than 2 bytes received, calculate CRC
		{
			for(uint8_t i = 0; i < 8; i++)
			{
				calculateCRC((rx->frame[rx->frameIdx - 2] >> i) & 1, &(rx->crc));
			}
		}
		rx->rx = RX_STAGE_FRAME;
		rx->frame[rx->frameIdx++] = rx->receivedByte; //store received byte
		rx->receivedByte = 0;
		rx->receivedBitIdx = 0;
	}
	else
		rx->receivedByte >>= 1;
}


uint8_t Ax25GetTxBit(void)
{
	if(txBitIdx == 8)
	{
		txBitIdx = 0;
		if(txStage == TX_STAGE_PREAMBLE) //transmitting preamble (TXDelay)
		{
			if(txDelayElapsed < txDelay)
			{
				txByte = 0x7E;
				txDelayElapsed++;
			}
			else
			{
				txDelayElapsed = 0;
				if(NULL != txFrame[txFrameTail].fx25Mode)
				{
					txStage = TX_STAGE_CORRELATION_TAG;
					txTagByteIdx = 0;
				}
				else
					txStage = TX_STAGE_HEADER_FLAGS;
			}
		}
#ifdef ENABLE_FX25
transmitTag:
		if(txStage == TX_STAGE_CORRELATION_TAG) //FX.25 correlation tag
		{
			if(txTagByteIdx < 8)
				txByte = (txFrame[txFrameTail].fx25Mode->tag >> (8 * txTagByteIdx)) & 0xFF;
			else
				txStage = TX_STAGE_DATA;
		}
#endif
		if(txStage == TX_STAGE_HEADER_FLAGS) //transmitting initial flags
		{
			if(txFlagsElapsed < STATIC_HEADER_FLAG_COUNT)
			{
				txByte = 0x7E;
				txFlagsElapsed++;
			}
			else
			{
				txFlagsElapsed = 0;
				txStage = TX_STAGE_DATA; //transmit data
			}
		}
		if(txStage == TX_STAGE_DATA) //transmitting normal data
		{
transmitNormalData:
			if((txFrameHead != txFrameTail) || txFrameBufferFull)
			{
				if(txByteIdx < txFrame[txFrameTail].size) //send buffer
				{
					txByte = txBuffer[(txFrame[txFrameTail].start + txByteIdx) % FRAME_BUFFER_SIZE];
					txByteIdx++;
				}
#ifdef ENABLE_FX25
				else if(txFrame[txFrameTail].fx25Mode != NULL)
				{
					txFrameBufferFull = false;
					txFrameTail++;
					txFrameTail %= FRAME_MAX_COUNT;
					txByteIdx = 0;
					if((txFrameHead != txFrameTail) || txFrameBufferFull)
					{
						if(txFrame[txFrameTail].fx25Mode != NULL)
						{
							txStage = TX_STAGE_CORRELATION_TAG;
							txTagByteIdx = 0;
							goto transmitTag;
						}
						else
							goto transmitNormalData;
					}
					else
						goto transmitTail;
				}
#endif
				else
				{
					txStage = TX_STAGE_CRC; //transmit CRC
					txCrcByteIdx = 0;
				}
			}
			else //no more frames
			{
transmitTail:
				txByteIdx = 0;
				txBitIdx = 0;
				txStage = TX_STAGE_TAIL;
			}
		}
		if(txStage == TX_STAGE_CRC) //transmitting CRC
		{
			if(txCrcByteIdx <= 1)
			{
				txByte = (txCrc & 0xFF) ^ 0xFF;
				txCrc >>= 8;
				txCrcByteIdx++;
			}
			else
			{
				txCrc = 0xFFFF;
				txStage = TX_STAGE_FOOTER_FLAGS; //now transmit flags
				txFlagsElapsed = 0;
			}

		}
		if(txStage == TX_STAGE_FOOTER_FLAGS)
		{
			if(txFlagsElapsed < STATIC_FOOTER_FLAG_COUNT)
			{
				txByte = 0x7E;
				txFlagsElapsed++;
			}
			else
			{
				txFlagsElapsed = 0;
				txFrameBufferFull = false;
				txFrameTail++;
				txFrameTail %= FRAME_MAX_COUNT;
				txByteIdx = 0;
#ifdef ENABLE_FX25
				if(((txFrameHead != txFrameTail) || txFrameBufferFull) && (txFrame[txFrameTail].fx25Mode != NULL))
				{
					txStage = TX_STAGE_CORRELATION_TAG;
					txTagByteIdx = 0;
					goto transmitTag;
				}
#endif
				txStage = TX_STAGE_DATA; //return to normal data transmission stage. There might be a next frame to transmit
				goto transmitNormalData;
			}
		}
		if(txStage == TX_STAGE_TAIL) //transmitting tail
		{
			if(txTailElapsed < txTail)
			{
				txByte = 0x7E;
				txTailElapsed++;
			}
			else //tail transmitted, stop transmission
			{
				txTailElapsed = 0;
				txStage = TX_STAGE_IDLE;
				txCrc = 0xFFFF;
				txBitstuff = 0;
				txByte = 0;
				txInitStage = TX_INIT_OFF;
				txBufferTail = txBufferHead;
				ModemTransmitStop();
				return 0;
			}
		}

	}

	uint8_t txBit = 0;
	//transmitting normal data or CRC in AX.25 mode
	if((NULL != txFrame[txFrameTail].fx25Mode) || (txStage == TX_STAGE_DATA) || (txStage == TX_STAGE_CRC))
	{
		if(txBitstuff == 5) //5 consecutive ones transmitted
		{
			txBit = 0; //transmit bit-stuffed 0
			txBitstuff = 0;
		}
		else
		{
			if(txByte & 1) //1 being transmitted
			{
				txBitstuff++; //increment bit stuffing counter
				txBit = 1;
			}
			else
			{
				txBit = 0;
				txBitstuff = 0; //0 being transmitted, reset bit stuffing counter
			}
			if(txStage == TX_STAGE_DATA) //calculate CRC only for normal data
				calculateCRC(txByte & 1, &txCrc);

			txByte >>= 1;
			txBitIdx++;
		}
	}
	//transmitting in FX.25 mode or in AX.25 mode, but these are preamble or flags, don't calculate CRC, don't use bit stuffing
	else
	{
		txBit = txByte & 1;
		txByte >>= 1;
		txBitIdx++;
	}
	return txBit;
}

/**
 * @brief Initialize transmission and start when possible
 */
void Ax25TransmitBuffer(void)
{
	if(txInitStage == TX_INIT_WAITING)
		return;
	if(txInitStage == TX_INIT_TRANSMITTING)
		return;

	if((txFrameHead != txFrameTail) || txFrameBufferFull)
	{
		txQuiet = (SysTickGet() + (Ax25Config.quietTime / SYSTICK_INTERVAL) + Random(0, 200 / SYSTICK_INTERVAL)); //calculate required delay
		txInitStage = TX_INIT_WAITING;
	}
}



/**
 * @brief Start transmission immediately
 * @warning Transmission should be initialized using Ax25_transmitBuffer
 */
static void transmitStart(void)
{
	txCrc = 0xFFFF; //initial CRC value
	txStage = TX_STAGE_PREAMBLE;
	txByte = 0;
	txBitIdx = 0;
	txFlagsElapsed = 0;
	ModemTransmitStart();
}


/**
 * @brief Start transmitting when possible
 * @attention Must be continuously polled in main loop
 */
void Ax25TransmitCheck(void)
{
	if(txInitStage == TX_INIT_OFF) //TX not initialized at all, nothing to transmit
		return;
	if(txInitStage == TX_INIT_TRANSMITTING) //already transmitting
		return;

	if(ModemIsTxTestOngoing()) //TX test is enabled, wait for now
		return;

	if(txQuiet < SysTickGet()) //quit time has elapsed
	{
		if(!ModemDcdState()) //channel is free
		{
			txInitStage = TX_INIT_TRANSMITTING; //transmit right now
			txRetries = 0;
			transmitStart();
		}
		else //channel is busy
		{
			if(txRetries == MAX_TRANSMIT_RETRY_COUNT) //timeout
			{
				txInitStage = TX_INIT_TRANSMITTING; //transmit right now
				txRetries = 0;
				transmitStart();
			}
			else //still trying
			{
				txQuiet = SysTickGet() + Random(100 / SYSTICK_INTERVAL, 500 / SYSTICK_INTERVAL); //try again after some random time
				txRetries++;
			}
		}
	}
}

void Ax25Init(void)
{
	txCrc = 0xFFFF;

	memset((void*)rxState, 0, sizeof(rxState));
	for(uint8_t i = 0; i < (sizeof(rxState) / sizeof(rxState[0])); i++)
		rxState[i].crc = 0xFFFF;

	txDelay = ((float)Ax25Config.txDelayLength / (8.f * 1000.f / ModemGetBaudrate())); //change milliseconds to byte count
	txTail = ((float)Ax25Config.txTailLength / (8.f * 1000.f / ModemGetBaudrate()));
}
