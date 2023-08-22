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

#include "drivers/modem.h"
#include "drivers/systick.h"
#include "ax25.h"
#include "stm32f1xx.h"
#include <math.h>
#include <stdlib.h>
#include "common.h"


/*
 * Configuration for PLL-based data carrier detection
 * DCD_MAXPULSE is the maximum value of the DCD pulse counter
 * DCD_THRES is the threshold value of the DCD pulse counter. When reached the input signal is assumed to be valid
 * DCD_MAXPULSE and DCD_THRES difference sets the DCD "inertia" so that the DCD state won't change rapidly when a valid signal is present
 * DCD_DEC is the DCD pulse counter decrementation value when symbol changes too far from PLL counter zero
 * DCD_INC is the DCD pulse counter incrementation value when symbol changes near the PLL counter zero
 * DCD_PLLTUNE is the DCD timing coefficient when symbol changes, pll_counter = pll_counter * DCD_PLLTUNE
 * The DCD mechanism is described in afsk_demod().
 * All values were selected by trial and error
 */
#define DCD1200_MAXPULSE 100
#define DCD1200_THRES 30
#define DCD1200_DEC 1
#define DCD1200_INC 7
#define DCD1200_PLLTUNE 0

#define DCD9600_MAXPULSE 200
#define DCD9600_THRES 80
#define DCD9600_DEC 3
#define DCD9600_INC 6
#define DCD9600_PLLTUNE 0

#define DCD300_MAXPULSE 50
#define DCD300_THRES 6
#define DCD300_DEC 1
#define DCD300_INC 5
#define DCD300_PLLTUNE 0

#define N1200 8 //samples per symbol
#define N9600 4
#define N300 16
#define NMAX 16 //keep this value equal to the biggest Nx

#define PLL1200_STEP (((uint64_t)1 << 32) / N1200) //PLL tick increment value
#define PLL9600_STEP (((uint64_t)1 << 32) / N9600)
#define PLL300_STEP (((uint64_t)1 << 32) / N300)

#define PLL1200_LOCKED_TUNE 0.74f
#define PLL1200_NOT_LOCKED_TUNE 0.50f
#define PLL9600_LOCKED_TUNE 0.89f
#define PLL9600_NOT_LOCKED_TUNE 0.50f //I have 0.67 noted somewhere as possibly better value
#define PLL300_LOCKED_TUNE 0.74f
#define PLL300_NOT_LOCKED_TUNE 0.50f

#define DAC_SINE_SIZE 32 //DAC sine table size


#define PTT_ON GPIOB->BSRR = GPIO_BSRR_BS7
#define PTT_OFF GPIOB->BSRR = GPIO_BSRR_BR7
#define DCD_ON (GPIOC->BSRR = GPIO_BSRR_BR13)
#define DCD_OFF (GPIOC->BSRR = GPIO_BSRR_BS13)

struct ModemDemodConfig ModemConfig;

static uint8_t N; //samples per symbol
static enum ModemTxTestMode txTestState; //current TX test mode
static uint8_t demodCount; //actual number of parallel demodulators
static uint16_t dacSine[DAC_SINE_SIZE]; //sine samples for DAC
static uint8_t dacSineIdx; //current sine sample index
static uint16_t samples[4]; //very raw received samples, filled directly by DMA
static uint8_t currentSymbol; //current symbol for NRZI encoding
static float markFreq; //mark frequency
static float spaceFreq; //space frequency
static float baudRate; //baudrate
static uint8_t markStep; //mark timer step
static uint8_t spaceStep; //space timer step
static uint16_t baudRateStep; //baudrate timer step
static int32_t coeffHiI[NMAX], coeffLoI[NMAX], coeffHiQ[NMAX], coeffLoQ[NMAX]; //correlator IQ coefficients
static uint8_t dcd = 0; //multiplexed DCD state from both demodulators


/**
 * @brief BPF filter with 2200 Hz tone 6 dB preemphasis (it actually attenuates 1200 Hz tone by 6 dB)
 */
static int16_t bpf1200[8] =
{
      728,
    -13418,
     -554,
    19493,
     -554,
    -13418,
      728,
     2104
};

/**
 * @brief BPF filter with 2200 Hz tone 6 dB deemphasis
 */
static int16_t bpf1200Inv[8] =
{
        -10513,
        -10854,
         9589,
        23884,
         9589,
        -10854,
        -10513,
         -879
};

//fs=4800, rectangular, fc1=1400, fc2=2000, 0 dB @ 1600 Hz and 1800 Hz, N = 15, gain 65536
static int16_t bpf300[15] =
{
	-2327, 3557, 1048, -9284, 12210, -3989, -9849, 17268, -9849, -3989, 12210, -9284, 1048, 3557, -2327,
};

#define BPF_MAX_TAPS 15

//fs=4800 Hz, raised cosine, fc=300 Hz (BR=600 Bd), beta=0.8, N=14, gain=65536
static int16_t lpf300[14] =
{
	741, 1834, 3216, 4756, 6268, 7547, 8402, 8402, 7547, 6268, 4756, 3216, 1834, 741,
};

#ifdef EXPERIMENTAL_LPF1200
//fs=9600 Hz, raised cosine, fc=600Hz (BR=1200 Bd), beta=0.8, N=14, gain=65536
static const int16_t lpf1200[14] =
{
	741, 1834, 3216, 4756, 6268, 7547, 8402, 8402, 7547, 6268, 4756, 3216, 1834, 741,
};
#define LPF_MAX_TAPS 14
#else
static int16_t lpf1200[15] =
{
        -6128,
        -5974,
        -2503,
         4125,
        12679,
        21152,
        27364,
        29643,
        27364,
        21152,
        12679,
         4125,
        -2503,
        -5974,
        -6128
};

#define LPF_MAX_TAPS 15

#endif

//fs=38400 Hz, Gaussian, fc=4800 Hz (9600 Bd), N=9, gain=65536
//seems like there is no difference between N=9 and any higher order
static int16_t lpf9600[9] = {497, 2360, 7178, 13992, 17478, 13992, 7178, 2360, 497};


#define FILTER_MAX_TAPS ((LPF_MAX_TAPS > BPF_MAX_TAPS) ? LPF_MAX_TAPS : BPF_MAX_TAPS)

struct Filter
{
	int16_t *coeffs;
	uint8_t taps;
	int32_t samples[FILTER_MAX_TAPS];
	uint8_t gainShift;
};

struct DemodState
{
	uint8_t rawSymbols; //raw, unsynchronized symbols
	uint8_t syncSymbols; //synchronized symbols

	enum ModemPrefilter prefilter;
	struct Filter bpf;
	int32_t correlatorSamples[NMAX];
	uint8_t correlatorSamplesIdx;
	struct Filter lpf;

	uint8_t dcd : 1; //DCD state
	uint64_t RMSenergy; //frame energy counter (sum of samples squared)
	uint32_t RMSsampleCount; //number of samples for RMS

	int32_t pll; //bit recovery PLL counter
	int32_t pllStep;
	float pllLockedAdjust;
	float pllNotLockedAdjust;

	int32_t dcdPll; //DCD PLL main counter
	uint8_t dcdLastSymbol; //last symbol for DCD
	uint8_t dcdCounter; //DCD "pulse" counter (incremented when RX signal is correct)
	int32_t dcdMax;
	int32_t dcdThres;
	int32_t dcdInc;
	int32_t dcdDec;
	float dcdAdjust;
};

static struct DemodState demodState[MODEM_MAX_DEMODULATOR_COUNT];

static void decode(uint8_t symbol, uint8_t demod);
static int32_t demodulate(int16_t sample, struct DemodState *dem);
static void setPtt(uint8_t state);

static int32_t filter(struct Filter *filter, int32_t input)
{
	int32_t out = 0;

	for(uint8_t i = filter->taps - 1; i > 0; i--)
		filter->samples[i] = filter->samples[i - 1]; //shift old samples

	filter->samples[0] = input; //store new sample
	for(uint8_t i = 0; i < filter->taps; i++)
	{
		out += filter->coeffs[i] * filter->samples[i];
	}
	return out >> filter->gainShift;
}

float ModemGetBaudrate(void)
{
	return baudRate;
}

uint8_t ModemGetDemodulatorCount(void)
{
	return demodCount;
}

uint8_t ModemDcdState(void)
{
	return dcd;
}

uint8_t ModemIsTxTestOngoing(void)
{
	if(txTestState != TEST_DISABLED)
		return 1;

	return 0;
}

void ModemClearRMS(uint8_t modem)
{

	demodState[modem].RMSenergy = 0;
	demodState[modem].RMSsampleCount = 0;

}

uint16_t ModemGetRMS(uint8_t modem)
{
	return sqrtf((float)demodState[modem].RMSenergy / (float)demodState[modem].RMSsampleCount);
}

enum ModemPrefilter ModemGetFilterType(uint8_t modem)
{
	return demodState[modem].prefilter;
}

/**
 * @brief Set DCD LED
 * @param[in] state 0 - OFF, 1 - ON
 */
static void setDcd(uint8_t state)
{
	if(state)
	{
		GPIOC->BSRR = GPIO_BSRR_BR13;
		GPIOB->BSRR = GPIO_BSRR_BS5;
	}
	else
	{
		GPIOC->BSRR = GPIO_BSRR_BS13;
		GPIOB->BSRR = GPIO_BSRR_BR5;
	}
}


/**
 * @brief ISR for demodulator
 * Called at 9600 Hz by DMA
 */
void DMA1_Channel2_IRQHandler(void) __attribute__ ((interrupt));
void DMA1_Channel2_IRQHandler(void)
{
	if(DMA1->ISR & DMA_ISR_TCIF2)
	{
		DMA1->IFCR |= DMA_IFCR_CTCIF2;

		int32_t sample = ((samples[0] + samples[1] + samples[2] + samples[3]) >> 1) - 4095; //calculate input sample (decimation)

		bool partialDcd = false;

		for(uint8_t i = 0; i < demodCount; i++)
		{
			uint8_t symbol = (demodulate(sample, (struct DemodState*)&demodState[i]) > 0); //demodulate sample
			decode(symbol, i); //recover bits, decode NRZI and call higher level function
			if(demodState[i].dcd)
				partialDcd = true;
		}

		if(partialDcd) //DCD on any of the demodulators
		{
			dcd = 1;
			setDcd(1);
		}
		else //no DCD on both demodulators
		{
			dcd = 0;
			setDcd(0);
		}
	}
}



/**
 * @brief ISR for pushing DAC samples
 */
void TIM1_UP_IRQHandler(void) __attribute__ ((interrupt));
void TIM1_UP_IRQHandler(void)
{
	TIM1->SR &= ~TIM_SR_UIF;

	if(ModemConfig.usePWM)
	{
		TIM4->CCR1 = dacSine[dacSineIdx];
	}
	else
	{
		GPIOB->ODR &= ~0xF000; //zero 4 oldest bits
		GPIOB->ODR |= (dacSine[dacSineIdx] << 12); //write sample to 4 oldest bits
	}

	dacSineIdx++;
	dacSineIdx &= (DAC_SINE_SIZE - 1);
}


/**
 * @brief ISR for baudrate generator timer. NRZI encoding is done here.
 */
void TIM3_IRQHandler(void) __attribute__ ((interrupt));
void TIM3_IRQHandler(void)
{
	TIM3->SR &= ~TIM_SR_UIF;

	if(txTestState == TEST_DISABLED) //transmitting normal data
	{
		if(Ax25GetTxBit() == 0) //get next bit and check if it's 0
		{
			currentSymbol ^= 1; //change symbol - NRZI encoding
		}
		//if 1, no symbol change
	}
	else //transmit test mode
	{
		currentSymbol ^= 1; //change symbol
	}

	TIM1->CNT = 0;

	if(currentSymbol) //current symbol is space
		TIM1->ARR = spaceStep;
	else //mark
		TIM1->ARR = markStep;

}


/**
 * @brief Demodulate received sample (4x oversampling)
 * @param[in] sample Received sample
 * @param[in] *dem Demodulator state
 * @return Current tone (0 or 1)
 */
static int32_t demodulate(int16_t sample, struct DemodState *dem)
{
	dem->RMSenergy += ((sample >> 1) * (sample >> 1)); //square the sample and add it to the sum
	dem->RMSsampleCount++; //increment number of samples

	if(dem->prefilter != PREFILTER_NONE) //filter is used
	{
		dem->correlatorSamples[dem->correlatorSamplesIdx++] = filter(&dem->bpf, sample);
	}
	else //no pre/deemphasis
	{
		dem->correlatorSamples[dem->correlatorSamplesIdx++] = sample;
	}

	dem->correlatorSamplesIdx %= N;

	int64_t outLoI = 0, outLoQ = 0, outHiI = 0, outHiQ = 0; //output values after correlating

	for(uint8_t i = 0; i < N; i++)
	{
		int32_t t = dem->correlatorSamples[(dem->correlatorSamplesIdx + i) % N]; //read sample
		outLoI += t * coeffLoI[i]; //correlate sample
		outLoQ += t * coeffLoQ[i];
		outHiI += t * coeffHiI[i];
		outHiQ += t * coeffHiQ[i];
	}

	outHiI >>= 12;
	outHiQ >>= 12;
	outLoI >>= 12;
	outLoQ >>= 12;
	uint64_t hi = (outHiI * outHiI) + (outHiQ * outHiQ); //calculate output tone levels
	uint64_t lo = (outLoI * outLoI) + (outLoQ * outLoQ);

	//DCD using PLL
	//PLL is running nominally at 1200 Hz (= baudrate)
	//PLL timer is counting up and eventually overflows to a minimal negative value
	//so it crosses zero in the middle
	//tone change should happen somewhere near this zero-crossing (in ideal case of exactly same TX and RX baudrates)
	//nothing is ideal, so we need to have some region around zero where tone change is expected
	//if tone changed inside this region, then we add something to the DCD pulse counter (and adjust counter phase for the counter to be closer to 0)
	//if tone changes outside this region, then we subtract something from the DCD pulse counter
	//if some DCD pulse threshold is reached, then we claim that the incoming signal is correct and set DCD flag
	//when configured properly, it's generally immune to noise, as the detected tone changes much faster than 1200 baud
	//it's also important to set some maximum value for DCD counter, otherwise the DCD is "sticky"

	dem->dcdPll = (signed)((unsigned)(dem->dcdPll) + ((unsigned)dem->pll)); //keep PLL ticking at the frequency equal to baudrate

	uint8_t dcdSymbol = (hi > lo); //get current symbol

	if(dcdSymbol != dem->dcdLastSymbol) //tone changed
	{
		if(abs(dem->dcdPll) < dem->dcdInc) //tone change occurred near zero
			dem->dcdCounter += dem->dcdInc; //increase DCD counter
		else //tone change occurred far from zero
		{
			if(dem->dcdCounter >= dem->dcdDec) //avoid overflow
				dem->dcdCounter -= dem->dcdDec; //decrease DCD counter
		}
		dem->dcdPll = (int)(dem->dcdPll * dem->dcdAdjust); //adjust PLL
	}

	dem->dcdLastSymbol = dcdSymbol; //store last symbol for symbol change detection

	if(dem->dcdCounter > dem->dcdMax) //maximum DCD counter value reached
		dem->dcdCounter = dem->dcdMax; //avoid "sticky" DCD and counter overflow

	if(dem->dcdCounter > dem->dcdThres) //DCD threshold reached
		dem->dcd = 1; //DCD!
	else //below DCD threshold
		dem->dcd = 0; //no DCD

	return filter(&dem->lpf, hi - lo) > 0;
}




/**
 * @brief Decode received symbol: bit recovery, NRZI decoding and pass the decoded bit to higher level protocol
 * @param[in] symbol Received symbol
 * @param demod Demodulator index
 */
static void decode(uint8_t symbol, uint8_t demod)
{
	struct DemodState *dem = (struct DemodState*)&demodState[demod];

	//This function provides bit/clock recovery and NRZI decoding
	//Bit recovery is based on PLL which is described in the function above (DCD PLL)
	//Current symbol is sampled at PLL counter overflow, so symbol transition should occur at PLL counter zero
	int32_t previous = dem->pll; //store last clock state

	dem->pll = (signed)((unsigned)(dem->pll) + (unsigned)dem->pllStep); //keep PLL running

	dem->rawSymbols <<= 1; //store received unsynchronized symbol
	dem->rawSymbols |= (symbol & 1);


	if ((dem->pll < 0) && (previous > 0)) //PLL counter overflow, sample symbol, decode NRZI and process in higher layer
	{
		dem->syncSymbols <<= 1;	//shift recovered (received, synchronized) bit register

		uint8_t t = dem->rawSymbols & 0x07; //take last three symbols for sampling. Seems that 1 symbol is not enough, but 3 symbols work well
		if(t == 0b111 || t == 0b110 || t == 0b101 || t == 0b011) //if there are 2 or 3 ones, then the received symbol is 1
		{
			dem->syncSymbols |= 1; //push to recovered symbols register
		}
		//if there 2 or 3 zeros, no need to add anything to the register

		//NRZI decoding
		if (((dem->syncSymbols & 0x03) == 0b11) || ((dem->syncSymbols & 0x03) == 0b00)) //two last symbols are the same - no symbol transition - decoded bit 1
		{
			Ax25BitParse(1, demod);
		}
		else //symbol transition - decoded bit 0
		{
			Ax25BitParse(0, demod);
		}
	}

	if(((dem->rawSymbols & 0x03) == 0b10) || ((dem->rawSymbols & 0x03) == 0b01)) //if there was a symbol transition, adjust PLL
	{

		if(Ax25GetRxStage(demod) != RX_STAGE_FRAME) //not in a frame
		{
			dem->pll = (int)(dem->pll * dem->pllNotLockedAdjust); //adjust PLL faster
		}
		else //in a frame
		{
			dem->pll = (int)(dem->pll * dem->pllLockedAdjust); //adjust PLL slower
		}
	}

}



void ModemTxTestStart(enum ModemTxTestMode type)
{
	if(txTestState != TEST_DISABLED) //TX test is already running
		ModemTxTestStop(); //stop this test

	setPtt(1); //PTT on
	txTestState = type;

	//DAC timer
	TIM1->PSC = 17; //72/18=4 MHz
	TIM1->DIER = TIM_DIER_UIE; //enable interrupt
	TIM1->CR1 |= TIM_CR1_CEN; //enable timer

	TIM2->CR1 &= ~TIM_CR1_CEN; //disable RX timer

	NVIC_DisableIRQ(DMA1_Channel2_IRQn); //disable RX DMA interrupt
	NVIC_EnableIRQ(TIM1_UP_IRQn); //enable timer 1 for PWM

	if(type == TEST_MARK)
	{
		TIM1->ARR = markStep;
	} else if(type == TEST_SPACE)
	{
		TIM1->ARR = spaceStep;
	}
	else //alternating tones
	{
		//enable baudrate generator
		TIM3->PSC = 71; //72/72=1 MHz
		TIM3->DIER = TIM_DIER_UIE; //enable interrupt
		TIM3->ARR = baudRateStep; //set timer interval
		TIM3->CR1 = TIM_CR1_CEN; //enable timer
		NVIC_EnableIRQ(TIM3_IRQn); //enable interrupt in NVIC
	}
}


void ModemTxTestStop(void)
{
	txTestState = TEST_DISABLED;

	TIM3->CR1 &= ~TIM_CR1_CEN; //turn off timers
	TIM1->CR1 &= ~TIM_CR1_CEN;
	TIM2->CR1 |= TIM_CR1_CEN; //enable RX timer

	NVIC_DisableIRQ(TIM3_IRQn);
	NVIC_DisableIRQ(TIM1_UP_IRQn);
	NVIC_EnableIRQ(DMA1_Channel2_IRQn);

	setPtt(0); //PTT off
}


void ModemTransmitStart(void)
{
	setPtt(1); //PTT on

	TIM1->PSC = 17;
	TIM1->DIER |= TIM_DIER_UIE;


	TIM3->PSC = 71;
	TIM3->DIER |= TIM_DIER_UIE;
	TIM3->ARR = baudRateStep;

	TIM3->CR1 = TIM_CR1_CEN;
	TIM1->CR1 = TIM_CR1_CEN;
	TIM2->CR1 &= ~TIM_CR1_CEN;

	NVIC_DisableIRQ(DMA1_Channel2_IRQn);
	NVIC_EnableIRQ(TIM1_UP_IRQn);
	NVIC_EnableIRQ(TIM3_IRQn);
}


/**
 * @brief Stop TX and go back to RX
 */
void ModemTransmitStop(void)
{

	TIM2->CR1 |= TIM_CR1_CEN;
	TIM3->CR1 &= ~TIM_CR1_CEN;
	TIM1->CR1 &= ~TIM_CR1_CEN;

	NVIC_DisableIRQ(TIM1_UP_IRQn);
	NVIC_DisableIRQ(TIM3_IRQn);
	NVIC_EnableIRQ(DMA1_Channel2_IRQn);

	setPtt(0);

	TIM4->CCR1 = 44; //set around 50% duty cycle
}

/**
 * @brief Controls PTT output
 * @param[in] state 0 - PTT off, 1 - PTT on
 */
static void setPtt(uint8_t state)
{
	if(state)
		PTT_ON;
	else
		PTT_OFF;
}


/**
 * @brief Initialize AFSK module
 */
void ModemInit(void)
{
	memset(demodState, 0, sizeof(demodState));

	if((ModemConfig.modem == MODEM_1200) || (ModemConfig.modem == MODEM_1200_V23))
	{
		demodCount = 2;
		N = N1200;
		baudRate = 1200.f;

		demodState[0].pllStep = PLL1200_STEP;
		demodState[0].pllLockedAdjust = PLL1200_LOCKED_TUNE;
		demodState[0].pllNotLockedAdjust = PLL1200_NOT_LOCKED_TUNE;
		demodState[0].dcdMax = DCD1200_MAXPULSE;
		demodState[0].dcdThres = DCD1200_THRES;
		demodState[0].dcdInc = DCD1200_INC;
		demodState[0].dcdDec = DCD1200_DEC;
		demodState[0].dcdAdjust = DCD1200_PLLTUNE;

		demodState[1].pllStep = PLL1200_STEP;
		demodState[1].pllLockedAdjust = PLL1200_LOCKED_TUNE;
		demodState[1].pllNotLockedAdjust = PLL1200_NOT_LOCKED_TUNE;
		demodState[1].dcdMax = DCD1200_MAXPULSE;
		demodState[1].dcdThres = DCD1200_THRES;
		demodState[1].dcdInc = DCD1200_INC;
		demodState[1].dcdDec = DCD1200_DEC;
		demodState[1].dcdAdjust = DCD1200_PLLTUNE;

		demodState[0].prefilter = PREFILTER_NONE;
		demodState[0].lpf.coeffs = lpf1200;
		demodState[0].lpf.taps = sizeof(lpf1200) / sizeof(*lpf1200);
		demodState[0].lpf.gainShift = 0; //not important, output is always compared with 0
		demodState[1].lpf.coeffs = lpf1200;
		demodState[1].lpf.taps = sizeof(lpf1200) / sizeof(*lpf1200);
		demodState[1].lpf.gainShift = 0; //not important, output is always compared with 0

		if(ModemConfig.flatAudioIn) //when used with flat audio input, use deemphasis and flat modems
		{
			demodState[1].prefilter = PREFILTER_DEEMPHASIS;
			demodState[1].bpf.coeffs = bpf1200Inv;
			demodState[1].bpf.taps = sizeof(bpf1200Inv) / sizeof(*bpf1200Inv);
			demodState[1].bpf.gainShift = 15;
		}
		else //when used with normal (filtered) audio input, use flat and preemphasis modems
		{
			demodState[1].prefilter = PREFILTER_PREEMPHASIS;
			demodState[1].bpf.coeffs = bpf1200;
			demodState[1].bpf.taps = sizeof(bpf1200) / sizeof(*bpf1200);
			demodState[1].bpf.gainShift = 15;
		}

		if(ModemConfig.modem == MODEM_1200) //Bell 202
		{
			markFreq = 1200.f;
			spaceFreq = 2200.f;
		}
		else //V.23
		{
			markFreq = 1300.f;
			spaceFreq = 2100.f;
		}
	}
	else if(ModemConfig.modem == MODEM_300)
	{
		demodCount = 1;
		N = N300;
		baudRate = 300.f;
		markFreq = 1600.f;
		markFreq = 1800.f;

		demodState[0].pllStep = PLL300_STEP;
		demodState[0].pllLockedAdjust = PLL300_LOCKED_TUNE;
		demodState[0].pllNotLockedAdjust = PLL300_NOT_LOCKED_TUNE;
		demodState[0].dcdMax = DCD300_MAXPULSE;
		demodState[0].dcdThres = DCD300_THRES;
		demodState[0].dcdInc = DCD300_INC;
		demodState[0].dcdDec = DCD300_DEC;
		demodState[0].dcdAdjust = DCD300_PLLTUNE;

		demodState[0].prefilter = PREFILTER_FLAT;
		demodState[0].bpf.coeffs = bpf300;
		demodState[0].bpf.taps = sizeof(bpf300) / sizeof(*bpf300);
		demodState[0].bpf.gainShift = 16;
		demodState[0].lpf.coeffs = lpf300;
		demodState[0].lpf.taps = sizeof(lpf300) / sizeof(*lpf300);
		demodState[0].lpf.gainShift = 0; //not important, output is always compared with 0
	}

	markStep = 4000000 / (DAC_SINE_SIZE * markFreq) - 1;
	spaceStep = 4000000 / (DAC_SINE_SIZE * spaceFreq) - 1;
	baudRateStep = 1000000 / baudRate - 1;


	/**
	 * TIM1 is used for pushing samples to DAC (R2R or PWM) at 4 MHz
	 * TIM3 is the baudrate generator for TX running at 1 MHz
	 * TIM4 is the PWM generator with no software interrupt
	 * TIM2 is the RX sampling timer with no software interrupt, but it directly calls DMA
	 */

	RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
	RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
	RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
	RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
	RCC->AHBENR |= RCC_AHBENR_DMA1EN;


	GPIOC->CRH |= GPIO_CRH_MODE13_1; //DCD LED on PC13
	GPIOC->CRH &= ~GPIO_CRH_MODE13_0;
	GPIOC->CRH &= ~GPIO_CRH_CNF13;

	GPIOB->CRH &= ~0xFFFF0000; //R2R output on PB12-PB15
	GPIOB->CRH |= 0x22220000;

	GPIOA->CRL &= ~GPIO_CRL_CNF0; //ADC input on PA0
	GPIOA->CRL &= ~GPIO_CRL_MODE0;


	GPIOB->CRL |= GPIO_CRL_MODE7_1; //PTT output on PB7
	GPIOB->CRL &= ~GPIO_CRL_MODE7_0;
	GPIOB->CRL &= ~GPIO_CRL_CNF7;

	GPIOB->CRL |= GPIO_CRL_MODE5_1; //2nd DCD LED on PB5
	GPIOB->CRL &= ~GPIO_CRL_MODE5_0;
	GPIOB->CRL &= ~GPIO_CRL_CNF5;


	RCC->CFGR |= RCC_CFGR_ADCPRE_1; //ADC prescaler /6
	RCC->CFGR &= ~RCC_CFGR_ADCPRE_0;

	ADC1->CR2 |= ADC_CR2_CONT; //continuous conversion
	ADC1->CR2 |= ADC_CR2_EXTSEL;
	ADC1->SQR1 &= ~ADC_SQR1_L; //1 conversion
    ADC1->SMPR2 |= ADC_SMPR2_SMP0_2; //41.5 cycle sampling
	ADC1->SQR3 &= ~ADC_SQR3_SQ1; //channel 0 is first in the sequence
    ADC1->CR2 |= ADC_CR2_ADON; //ADC on

	ADC1->CR2 |= ADC_CR2_RSTCAL; //calibrate ADC
	while(ADC1->CR2 & ADC_CR2_RSTCAL)
		;
	ADC1->CR2 |= ADC_CR2_CAL;
	while(ADC1->CR2 & ADC_CR2_CAL)
		;

	ADC1->CR2 |= ADC_CR2_EXTTRIG;
	ADC1->CR2 |= ADC_CR2_SWSTART; //start ADC conversion

	//prepare DMA
	DMA1_Channel2->CCR |= DMA_CCR_MSIZE_0; //16 bit memory region
	DMA1_Channel2->CCR &= ~DMA_CCR_MSIZE_1;
    DMA1_Channel2->CCR |= DMA_CCR_PSIZE_0;
	DMA1_Channel2->CCR &= ~DMA_CCR_PSIZE_1;

	DMA1_Channel2->CCR |= DMA_CCR_MINC | DMA_CCR_CIRC| DMA_CCR_TCIE; //circular mode, memory increment and interrupt
    DMA1_Channel2->CNDTR = 4; //4 samples
	DMA1_Channel2->CPAR = (uint32_t)&(ADC1->DR); //ADC data register address
	DMA1_Channel2->CMAR = (uint32_t)samples; //sample buffer address
	DMA1_Channel2->CCR |= DMA_CCR_EN; //enable DMA

	NVIC_EnableIRQ(DMA1_Channel2_IRQn);

	TIM2->PSC = 17; //72/18=4 MHz
	TIM2->DIER |= TIM_DIER_UDE; //enable calling DMA on timer tick
	TIM2->ARR = 103; //4MHz / 104 =~38400 Hz (4*9600 Hz for 4x oversampling)
	TIM2->CR1 |= TIM_CR1_CEN; //enable timer

	for(uint8_t i = 0; i < N; i++) //calculate correlator coefficients
	{
		coeffLoI[i] = 4095.f * cosf(2.f * 3.1416f * (float)i / (float)N * markFreq / baudRate);
		coeffLoQ[i] = 4095.f * sinf(2.f * 3.1416f * (float)i / (float)N * markFreq / baudRate);
		coeffHiI[i] = 4095.f * cosf(2.f * 3.1416f * (float)i / (float)N * spaceFreq / baudRate);
		coeffHiQ[i] = 4095.f * sinf(2.f * 3.1416f * (float)i / (float)N * spaceFreq / baudRate);
	}

	for(uint8_t i = 0; i < DAC_SINE_SIZE; i++) //calculate DAC sine samples
	{
		if(ModemConfig.usePWM)
			dacSine[i] = ((sinf(2.f * 3.1416f * (float)i / (float)DAC_SINE_SIZE) + 1.f) * 45.f);
		else
			dacSine[i] = ((7.f * sinf(2.f * 3.1416f * (float)i / (float)DAC_SINE_SIZE)) + 8.f);
	}

	if(ModemConfig.usePWM)
	{
		RCC->APB1ENR |= RCC_APB1ENR_TIM4EN; //configure timer

		GPIOB->CRL |= GPIO_CRL_CNF6_1; //configure pin for PWM
		GPIOB->CRL |= GPIO_CRL_MODE6;
		GPIOB->CRL &= ~GPIO_CRL_CNF6_0;

		//set up PWM generation
		TIM4->PSC = 7; //72MHz/8=9MHz
		TIM4->ARR = 90; //9MHz/90=100kHz

		TIM4->CCMR1 |= TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2;
		TIM4->CCER |= TIM_CCER_CC1E;
		TIM4->CCR1 = 44; //initial duty cycle

		TIM4->CR1 |= TIM_CR1_CEN;
	}

}
