// This file is part of MatrixPilot.
//
//    http://code.google.com/p/gentlenav/
//
// Copyright 2009-2011 MatrixPilot Team
// See the AUTHORS.TXT file for a list of authors of MatrixPilot.
//
// MatrixPilot is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MatrixPilot is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with MatrixPilot.  If not, see <http://www.gnu.org/licenses/>.


#include "libUDB_internal.h"

#if (BOARD_TYPE == UDB4_BOARD)

//	Variables.

struct ADchannel udb_xaccel, udb_yaccel , udb_zaccel ; // x, y, and z accelerometer channels
struct ADchannel udb_xrate , udb_yrate, udb_zrate ;  // x, y, and z gyro channels
struct ADchannel udb_vref ; // reference voltage

#if (NUM_ANALOG_INPUTS >= 1)
struct ADchannel udb_analogInputs[NUM_ANALOG_INPUTS] ; // 0-indexed, unlike servo pwIn/Out/Trim arrays
#endif

int vref_adj ;
int sample_count ;

#if (RECORD_FREE_STACK_SPACE == 1)
unsigned int maxstack = 0 ;
#endif

/*<GUIOTT>
 * Analog to digital processing.
 * Sampling and conversion is done automatically, all channels are read by the
 * DMA before generating the interrupt.
 * At the end of a full acquisition cycle, the single readings are summated in a
 * variable and averaged after ALMOST_ENOUGH_SAMPLES cycles. This correspond to
 * a first order digital lowpass filter with a time constant of about
 * 23 milliseconds, applied to improve signal to noise.
 * With an hearthbeat of 40Hz (25ms) this guarantee at least one new set of
 * values for each DCM computation.
 *
 * Below constants to make the ADC configuration parametrizable following
 * Mark Whitehorn branch for 40Mips clock
 *
 * dsPIC33FJXXXGPX06A/X08A/X10A
 * minimum allowed 12-bit ADC clock period is 118ns or 8.47MHz
 * TAD is 1/ADC_CLK
 * 12 bit mode conversion time is 14 Tad cycles
 * total sample/conversion time is (14+SAMC) * Tad
 * for automatic sequential sampling and conversion, the sampling time is
 *   SAMC * Tad
 * 
 * With:
 * FREQOSC = 80MHz
 * CLK_PHASES = 2
 * ADCLK_DIV_N_MINUS_1 = 63 (ADCS)
 * ADSAMP_TIME_N = 10 (SAMC)
 * NUM_ANALOG_INPUTS = 0
 * NUM_AD_CHAN = NUM_ANALOG_INPUTS + 6 (only 3 axys accel. + 3 axys gyro)

 * Fcy = FREQOSC / CLK_PHASES = 40MHz
 * Tcy = 1 / Fcy = 25ns
 * ADC_CLK = Fcy / (ADCLK_DIV_N_MINUS_1 + 1) = 625kHz
 * Tad = 1 / ADC_CLK = 1.6us
 * ADC Conversion Time for 12-bit Tconv = 14*Tad = 22.4us
 * ADC Total sample/conversion time = (14 + ADSAMP_TIME_N) * 1.6us = 38.4us
 * ADC_RATE = 1 / (Total sample/conversion time) = 26kHz
 * Cycle time = (Total sample/conversion time) * NUM_AD_CHAN = 230.4us
 * DMA interrupt rate = 1 / Cycle time = 4.3kHz
 * Average time=Cycle time*ALMOST_ENOUGH_SAMPLES=23ms = PB filter time constant
*/

// Number of possible locations for ADC buffer 10
// (AN1,4,6,9,10,11,15,16,17,18) x 1 = 10 words
// Align the buffer to 10 words or 20 bytes. This is needed for peripheral indirect mode
// 3 analog inputs for accell + 3 for gyro
#define NUM_AD_CHAN (NUM_ANALOG_INPUTS + 6)
int  BufferA[NUM_AD_CHAN] __attribute__((space(dma),aligned(32))) ;
int  BufferB[NUM_AD_CHAN] __attribute__((space(dma),aligned(32))) ;

#define ADCLK_DIV_N_MINUS_1 63  // ADCS max divisor is 64
#define ADC_CLK (FREQOSC / (CLK_PHASES * (ADCLK_DIV_N_MINUS_1 + 1)))
#if (ADC_CLK > 8470000)
#error ADC_CLK too fast
#endif

#define ADSAMP_TIME_N 10

#define ADC_RATE (1.0 * ADC_CLK / (ADSAMP_TIME_N + 14))

#define ALMOST_ENOUGH_SAMPLES 100
//</GUIOTT>


void udb_init_gyros( void )
{
	// turn off auto zeroing 
	_TRISC4 = _TRISB14 = 0 ;
	_LATC4 = _LATB14 = 0 ;
	
	return ;
}


void udb_init_accelerometer(void)
{
	_TRISA6 = 0 ;  // GSELECT is an output
	_LATA6 = 1 ;   // 6 G setting
	
	//	set as inputs:
	_TRISB9 = 1 ;
	_TRISB10 = 1 ;
	_TRISB11 = 1 ;
	
	return ;
}


void udb_init_ADC( void )
{
	udb_init_gyros() ;
	udb_init_accelerometer() ;
	sample_count = 0 ;
	
	AD1CON1bits.FORM   = 3 ;	// Data Output Format: Signed Fraction (Q15 format)
	AD1CON1bits.SSRC   = 7 ;	// Sample Clock Source: Auto-conversion
	AD1CON1bits.ASAM   = 1 ;	// ADC Sample Control: Sampling begins immediately after conversion
	AD1CON1bits.AD12B  = 1 ;	// 12-bit ADC operation
	
	AD1CON2bits.CSCNA = 1 ;		// Scan Input Selections for CH0+ during Sample A bit
	AD1CON2bits.CHPS  = 0 ;		// Converts CH0
	
	AD1CON3bits.ADRC = 0 ;		// ADC Clock is derived from Systems Clock
	AD1CON3bits.ADCS = ADCLK_DIV_N_MINUS_1;
	AD1CON3bits.SAMC = ADSAMP_TIME_N  ;
	
	AD1CON2bits.VCFG = 0 ;		// use supply as reference voltage
	
	AD1CON1bits.ADDMABM = 1 ; 	// DMA buffers are built in sequential mode
	AD1CON2bits.SMPI    = (NUM_AD_CHAN-1) ;
	AD1CON4bits.DMABL   = 0 ;	// Each buffer contains 1 word
	
	
	AD1CSSL = 0x0000 ;
	AD1CSSH = 0x0000 ;
	AD1PCFGL= 0xFFFF ;
	AD1PCFGH= 0xFFFF ;
	
//	include the 110 degree/second scale, gyro
	/*
	AD1CSSLbits.CSS0 = 1 ;
	AD1CSSLbits.CSS3 = 1 ;
	AD1CSSLbits.CSS5 = 1 ;
	
	AD1PCFGLbits.PCFG0 = 0 ;
	AD1PCFGLbits.PCFG3 = 0 ;
	AD1PCFGLbits.PCFG5 = 0 ;
	*/
	
//	include the 500 degree/second scale, gyro
	AD1CSSLbits.CSS1 = 1 ;
	AD1CSSLbits.CSS4 = 1 ;
	AD1CSSLbits.CSS6 = 1 ;
	
	AD1PCFGLbits.PCFG1 = 0 ;
	AD1PCFGLbits.PCFG4 = 0 ;
	AD1PCFGLbits.PCFG6 = 0 ;
	
//	include the accelerometer in the scan:
	AD1CSSLbits.CSS9 = 1 ;
	AD1CSSLbits.CSS10 = 1 ;
	AD1CSSLbits.CSS11 = 1 ;
	
	AD1PCFGLbits.PCFG9 = 0 ;
	AD1PCFGLbits.PCFG10 = 0 ;
	AD1PCFGLbits.PCFG11 = 0 ;
	
//  include the extra analog input pins
	AD1CSSLbits.CSS15 = 1 ;		// Enable AN15 for channel scan
	AD1CSSHbits.CSS16 = 1 ;		// Enable AN16 for channel scan
	AD1CSSHbits.CSS17 = 1 ;		// Enable AN17 for channel scan
	AD1CSSHbits.CSS18 = 1 ;		// Enable AN18 for channel scan
 	
	AD1PCFGLbits.PCFG15 = 0 ;	// AN15 as Analog Input 
	AD1PCFGHbits.PCFG16 = 0 ;	// AN16 as Analog Input 
	AD1PCFGHbits.PCFG17 = 0 ;	// AN17 as Analog Input 
	AD1PCFGHbits.PCFG18 = 0 ;	// AN18 as Analog Input 
	
	_AD1IF = 0 ;				// Clear the A/D interrupt flag bit
	_AD1IP = 5 ;				// priority 5
	AD1CON1bits.ADON = 1 ;		// Turn on the A/D converter
	_AD1IE = 0 ;				// Do Not Enable A/D interrupt
	
	
//  DMA Setup
	DMA0CONbits.AMODE = 2 ;		// Configure DMA for Peripheral indirect mode
	DMA0CONbits.MODE  = 2 ;		// Configure DMA for Continuous Ping-Pong mode
	DMA0PAD=(int)&ADC1BUF0 ;
	DMA0CNT = NUM_AD_CHAN-1 ;					
	DMA0REQ = 13 ;				// Select ADC1 as DMA Request source
	
	DMA0STA = __builtin_dmaoffset(BufferA) ;
	DMA0STB = __builtin_dmaoffset(BufferB) ;
	
	IFS0bits.DMA0IF = 0 ;		//Clear the DMA interrupt flag bit
    IEC0bits.DMA0IE = 1 ;		//Set the DMA interrupt enable bit
	_DMA0IP = 5 ;				//Set the DMA ISR priority
	
	DMA0CONbits.CHEN = 1 ;		// Enable DMA
	
	return ;
}


unsigned char DmaBuffer = 0 ;

void __attribute__((__interrupt__,__no_auto_psv__)) _DMA0Interrupt(void)
{
	indicate_loading_inter ;
	interrupt_save_set_corcon ;
	
#if (RECORD_FREE_STACK_SPACE == 1)
	unsigned int stack = WREG15 ;
	if ( stack > maxstack )
	{
		maxstack = stack ;
	}
#endif
	
#if (HILSIM != 1)
	int *CurBuffer = (DmaBuffer == 0) ? BufferA : BufferB ;
	
	udb_xrate.input = CurBuffer[xrateBUFF-1] ;
	udb_yrate.input = CurBuffer[yrateBUFF-1] ;
	udb_zrate.input = CurBuffer[zrateBUFF-1] ;
	udb_xaccel.input = CurBuffer[xaccelBUFF-1] ;
	udb_yaccel.input = CurBuffer[yaccelBUFF-1] ;
	udb_zaccel.input = CurBuffer[zaccelBUFF-1] ;
#if (NUM_ANALOG_INPUTS >= 1)
	udb_analogInputs[0].input = CurBuffer[analogInput1BUFF-1] ;
#endif
#if (NUM_ANALOG_INPUTS >= 2)
	udb_analogInputs[1].input = CurBuffer[analogInput2BUFF-1] ;
#endif
#if (NUM_ANALOG_INPUTS >= 3)
	udb_analogInputs[2].input = CurBuffer[analogInput3BUFF-1] ;
#endif
#if (NUM_ANALOG_INPUTS >= 4)
	udb_analogInputs[3].input = CurBuffer[analogInput4BUFF-1] ;
#endif
	
#endif
	
	DmaBuffer ^= 1 ;			// Switch buffers
	IFS0bits.DMA0IF = 0 ;		// Clear the DMA0 Interrupt Flag
	
	
	if ( udb_flags._.a2d_read == 1 ) // prepare for the next reading
	{
		udb_flags._.a2d_read = 0 ;
		udb_xrate.sum = udb_yrate.sum = udb_zrate.sum = 0 ;
		udb_xaccel.sum = udb_yaccel.sum = udb_zaccel.sum = 0 ;
#ifdef VREF
		udb_vref.sum = 0 ;
#endif
#if (NUM_ANALOG_INPUTS >= 1)
		udb_analogInputs[0].sum = 0;
#endif
#if (NUM_ANALOG_INPUTS >= 2)
		udb_analogInputs[1].sum = 0 ;
#endif
#if (NUM_ANALOG_INPUTS >= 3)
		udb_analogInputs[2].sum = 0;
#endif
#if (NUM_ANALOG_INPUTS >= 4)
		udb_analogInputs[3].sum = 0 ;
#endif
		sample_count = 0 ;
	}
	
	//	perform the integration:
	udb_xrate.sum += udb_xrate.input ;
	udb_yrate.sum += udb_yrate.input ;
	udb_zrate.sum += udb_zrate.input ;
#ifdef VREF
	udb_vref.sum  += udb_vref.input ;
#endif
	udb_xaccel.sum += udb_xaccel.input ;
	udb_yaccel.sum += udb_yaccel.input ;
	udb_zaccel.sum += udb_zaccel.input ;
#if (NUM_ANALOG_INPUTS >= 1)
	udb_analogInputs[0].sum += udb_analogInputs[0].input ;
#endif
#if (NUM_ANALOG_INPUTS >= 2)
	udb_analogInputs[1].sum += udb_analogInputs[1].input ;
#endif
#if (NUM_ANALOG_INPUTS >= 3)
	udb_analogInputs[2].sum += udb_analogInputs[2].input ;
#endif
#if (NUM_ANALOG_INPUTS >= 4)
	udb_analogInputs[3].sum += udb_analogInputs[3].input ;
#endif
	sample_count ++ ;
	
	//When there is a chance that read_gyros() and read_accel() will execute soon,
	// have the new average values ready.
	if ( sample_count > ALMOST_ENOUGH_SAMPLES )
	{	
		udb_xrate.value = __builtin_divsd( udb_xrate.sum , sample_count ) ;
		udb_yrate.value = __builtin_divsd( udb_yrate.sum , sample_count ) ;
		udb_zrate.value = __builtin_divsd( udb_zrate.sum , sample_count ) ;
#ifdef VREF
		udb_vref.value = __builtin_divsd( udb_vref.sum , sample_count ) ;
#endif
		udb_xaccel.value =  __builtin_divsd( udb_xaccel.sum , sample_count ) ;
		udb_yaccel.value =  __builtin_divsd( udb_yaccel.sum , sample_count ) ;
		udb_zaccel.value =  __builtin_divsd( udb_zaccel.sum , sample_count ) ;
		
#if (NUM_ANALOG_INPUTS >= 1)
		udb_analogInputs[0].value = __builtin_divsd( udb_analogInputs[0].sum, sample_count ) ;
#endif
#if (NUM_ANALOG_INPUTS >= 2)
		udb_analogInputs[1].value = __builtin_divsd( udb_analogInputs[1].sum, sample_count ) ;
#endif
#if (NUM_ANALOG_INPUTS >= 3)
		udb_analogInputs[2].value = __builtin_divsd( udb_analogInputs[2].sum, sample_count ) ;
#endif
#if (NUM_ANALOG_INPUTS >= 4)
		udb_analogInputs[3].value = __builtin_divsd( udb_analogInputs[3].sum, sample_count ) ;
#endif
	}
	
	interrupt_restore_corcon ;
	return ;
}

#endif
