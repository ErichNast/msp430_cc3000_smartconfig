/*****************************************************************************
*
*  demo.c - CC3000 Main Demo Application
*  Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*    Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
*
*    Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the   
*    distribution.
*
*    Neither the name of Texas Instruments Incorporated nor the names of
*    its contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
*  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
*  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
*  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
*  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
*  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
*  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
*  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
*  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*****************************************************************************/
#include <msp430.h>
#include "wlan.h" 
#include "evnt_handler.h"    // callback function declaration
#include "nvmem.h"
#include "socket.h"
#include "common.h"
#include "netapp.h"
#include "cc3000.h"
#include "sensors.h"
#include "board.h"
#include "string.h"
#include "utils.h"
#include "spi.h"
//#include "demo_config.h"
#include "uart.h"
#include "../exosite/exosite.h"


// local defines
#define ExositeAppVersion                  "  v0.1  "
#define WRITE_INTERVAL 5
#define EXO_BUFFER_SIZE 200 //reserve 200 bytes for packing all our write data into a FRAM buffer

#ifdef __MSP430FR5739__
#pragma DATA_SECTION(exo_buffer, ".exo_buffer") //create a buffer in FRAM
char exo_buffer[EXO_BUFFER_SIZE];
#elif __IAR_SYSTEMS_ICC__
#pragma location = "EXO_DATA"
__no_init char exo_buffer[EXO_BUFFER_SIZE];
#endif

// local functions
unsigned char checkWiFiConnected(void);
void show_status(void);

// exported functions

// externs
extern const char sensorNames[10][11];
extern char *itoa(int n, char *s, int b);
extern void busyWait(unsigned short delay);
extern void board_init(void);

// global variables
int cloud_status = -1;

/** \brief Flag indicating whether user requested to perform Smart Config */
volatile char runSmartConfig = 0;
//Flag to indicate Smart Config needs to be performed
unsigned char * ptrFtcAtStartup = (unsigned char *)0x1830;
//Flag to indicate Smart Config was performed in the past and CC3000 has a stored profile
unsigned char * SmartConfigProfilestored = (unsigned char *)0x1800;
unsigned char ConnectUsingSmartConfig = 0; 
/** \brief Flag indicating whether to print CC3000 Connection info */
static unsigned char obtainIpInfoFlag = FALSE;

//Device name - used for Smart config in order to stop the Smart phone configuration process
char DevServname[] = {'C','C','3','0','0','0'};

volatile unsigned long ulCC3000Connected;
volatile unsigned long SendmDNSAdvertisment;


//==========================================
long serverSocket;
sockaddr serverSocketAddr;

/** \brief Definition of data packet to be sent by server */
unsigned char dataPacket[] = { '\r', 0xBE, 128, 128, 128, 70, 36, 0xEF };

char serverErrorCode = 0;

//*****************************************************************************
//
//!  main
//!
//!  \param  None
//!
//!  \return none
//!
//!  \brief   The main loop is executed here
//
//*****************************************************************************
void main(void)
{
  unsigned char loopCount = 0;
  int loop_time = 2000;

  ulCC3000Connected = 0;
  SendmDNSAdvertisment = 0;

  // Initialize hardware and interfaces
  board_init();
  initUart();
  sendString("System init : \r\n");

  // Must initialize one time for MAC address prepare..
  if (!Exosite_Init("exosite", "cc3000wifismartconfig", IF_WIFI, 0))
  {
    show_status();
    while(1);
  }

  // Main Loop
  while (1)
  {
    // Perform Smart Config if button pressed in current run or if flag set in FRAM
    // from previous MSP430 Run.
    if(runSmartConfig == 1 || *ptrFtcAtStartup == SMART_CONFIG_SET)
    {
      // Clear flag
      ClearFTCflag();
      unsetCC3000MachineState(CC3000_ASSOC);

      // Start the Smart Config Process
      StartSmartConfig();
      runSmartConfig = 0;
    }

    WDTCTL = WDTPW + WDTHOLD;
    // If connectivity is good, run the primary functionality
    if(checkWiFiConnected())
    {
      char * pbuf = exo_buffer;

      //unsolicicted_events_timer_disable();

      if (0 == cloud_status)
      { //check to see if we have a valid connection
        loop_time = 2000;

        loopCount = 1;

        while (loopCount++ <= (WRITE_INTERVAL+1))
        {
//          WDTCTL = WDT_ARST_1000;
          if (Exosite_Read("led7_ctrl", pbuf, EXO_BUFFER_SIZE))
          {
        	// Read success
        	turnLedOn(CC3000_CLIENT_CONNECTED_IND);

            if (!strncmp(pbuf, "0", 1))
              turnLedOff(LED7);
            else if (!strncmp(pbuf, "1", 1))
              turnLedOn(LED7);
          }
          else
          {
        	if (EXO_STATUS_NOAUTH == Exosite_StatusCode())
        	{
        		turnLedOff(CC3000_CLIENT_CONNECTED_IND);
        		// Activate device again
        		cloud_status = Exosite_Activate();
        	}
          }

          hci_unsolicited_event_handler();
          unsolicicted_events_timer_init();
          //sendString("== Exosite Read==\r\n");
          WDTCTL = WDTPW + WDTHOLD;
          busyWait(loop_time);        //delay before looping again
        }

        unsolicicted_events_timer_init();
    	if (EXO_STATUS_NOAUTH != Exosite_StatusCode())
    	{
          unsigned char sensorCount = 0;
          int value;
          char strRead[6]; //largest value of an int in ascii is 5 + null terminate
          WDTCTL = WDT_ARST_1000;
          for (sensorCount = 0; sensorCount < SENSOR_END; sensorCount++)
          {
            value = getSensorResult(sensorCount);                                       //get the sensor reading
            itoa(value, strRead, 10);                           //convert to a string
            unsolicicted_events_timer_init();
            //for each reading / data source (alias), we need to build the string "alias=value" (must be URL encoded)
            //this is all just an iteration of, for example, Exosite_Write("mydata=hello_world",18);
            memcpy(pbuf,&sensorNames[sensorCount][0],strlen(&sensorNames[sensorCount][0]));  //copy alias name into buffer
            pbuf += strlen(&sensorNames[sensorCount][0]);
            *pbuf++ = 0x3d;                                             //put an '=' into buffer
            memcpy(pbuf,strRead, strlen(strRead));                      //copy value into buffer
            pbuf += strlen(strRead);
            *pbuf++ = 0x26;                                             //put an '&' into buffer, the '&' ties successive alias=val pairs together
          }
          pbuf--;                                                                       //back out the last '&'
          WDTCTL = WDT_ARST_1000;
          Exosite_Write(exo_buffer,(pbuf - exo_buffer - 1));    //write all sensor values to the cloud

          if (EXO_STATUS_OK == Exosite_StatusCode())
          { // Write success
    	    turnLedOn(CC3000_CLIENT_CONNECTED_IND);
          }
    	}
      } else {
          //we don't have a good connection yet - we keep retrying to authenticate
    	  WDTCTL = WDTPW + WDTHOLD;
          //sendString("== Exosite Activate==\r\n");
          cloud_status = Exosite_Activate();
          if (0 != cloud_status) loop_time = 30000; //delay 30 seconds before retrying...
      }

        unsolicicted_events_timer_init();
      }
      WDTCTL = WDTPW + WDTHOLD;
      // TODO - make this a sleep instead of busy wait
      busyWait(loop_time);        //delay before looping again
    }
}


//*****************************************************************************
//
//!  checkWiFiConnected
//!
//!  \param  None
//!
//!  \return TRUE if connected, FALSE if not
//!
//!  \brief  Checks to see that WiFi is still connected.  If not associated
//!          with an AP for 5 consecutive retries, it will reset the board.
//
//*****************************************************************************
unsigned char
checkWiFiConnected(void)
{
  unsigned char ipInfoFlagSet = 0;

  if(!(currentCC3000State() & CC3000_ASSOC)) //try to associate with an Access Point
  {
    //
    // Check whether Smart Config was run previously. If it was, we
    // use it to connect to an access point. Otherwise, we connect to the
    // default.
    //

    if((isFTCSet() == 0)&&(ConnectUsingSmartConfig==0)&&(*SmartConfigProfilestored != SMART_CONFIG_SET))
    {
      // Smart Config not set, check whether we have an SSID
      // from the assoc terminal command. If not, use fixed SSID.
        //sendString("== ConnectUsingSSID==\r\n");
        ConnectUsingSSID(SSID);
    }
    unsolicicted_events_timer_init();
    // Wait until connection is finished
    //sendString("== Wait until connection is finished==\r\n");
    while (!(currentCC3000State() & CC3000_ASSOC))
    {
      WDTCTL = WDT_ARST_1000;
      __delay_cycles(100);

      // Handle any un-solicited event if required - the function will get triggered
      // few times in a second
      hci_unsolicited_event_handler();

      // Check if user pressed button to do Smart Config
      if(runSmartConfig == 1)
          break;
    }
  }

  // Handle un-solicited events - will be triggered few times per second
  hci_unsolicited_event_handler();
  WDTCTL = WDTPW + WDTHOLD;
  // Check if we are in a connected state.  If so, set flags and LED
  if(currentCC3000State() & CC3000_IP_ALLOC)
  {
    unsolicicted_events_timer_disable(); // Turn our timer off since isr-driven routines set LEDs too...

    if (obtainIpInfoFlag == FALSE)
    {
      //sendString("== CC3000_IP_ALLOC_IND==\r\n");
      obtainIpInfoFlag = TRUE;             // Set flag so we don't constantly turn the LED on
      turnLedOn(CC3000_IP_ALLOC_IND);
      ipInfoFlagSet = 1;
      unsolicicted_events_timer_init();
    }

    if (obtainIpInfoFlag == TRUE)
    {
    	WDTCTL = WDT_ARST_1000;
      //If Smart Config was performed, we need to send complete notification to the configure (Smart Phone App)
      if (ConnectUsingSmartConfig==1)
      {
        mdnsAdvertiser(1,DevServname, sizeof(DevServname));
        ConnectUsingSmartConfig = 0;
        *SmartConfigProfilestored = SMART_CONFIG_SET;
      }
      //Start mDNS timer in order to send mDNS Advertisement every 30 seconds
      mDNS_packet_trigger_timer_enable();

      unsolicicted_events_timer_init();
    }
    WDTCTL = WDTPW + WDTHOLD;
    if( ipInfoFlagSet == 1)
    {
      // Initialize an Exosite connection
      //sendString("== Exosite Activate ==\r\n");
      cloud_status = Exosite_Activate();
      ipInfoFlagSet = 0;
    }

    return TRUE;
  }

  return FALSE;

}



/*****************************************************************************
*
*  show_status
*
*  \param  None
*
*  \return None
*
*  \brief  Shows the status message on the LCD display
*
*****************************************************************************/
void show_status(void)
{
  int code = Exosite_StatusCode();

  switch (code)
  {
    case EXO_STATUS_BAD_TCP:
      break;
    case EXO_STATUS_BAD_UUID:
      break;
    case EXO_STATUS_BAD_VENDOR:
      break;
    case EXO_STATUS_BAD_MODEL:
      break;
    case EXO_STATUS_BAD_INIT:
      break;
    case EXO_STATUS_BAD_SN:
      break;
    case EXO_STATUS_CONFLICT:
      break;
    case EXO_STATUS_BAD_CIK:
      break;
    case EXO_STATUS_NOAUTH:
      break;

  }

  return;
}

#pragma vector=PORT4_VECTOR
__interrupt void Port_4(void)
{
  switch(__even_in_range(P4IV,P4IV_P4IFG1))
  {
    case P4IV_P4IFG0:
      StartDebounceTimer();
      // disable switch interrupt
      DissableSwitch();
      break;

    default:
      break;
  }
    P4IFG = 0;
}


// Timer B0 interrupt service routine
#pragma vector = TIMER0_B0_VECTOR
__interrupt void Timer_B (void)
{
  // Check whether button is still pressed. If so, Smart Config
  // should be performed.

  if(!(switchIsPressed()))
  {
    // Button is still pressed, so Smart Config should be done
    runSmartConfig = 1;

    if(currentCC3000State() & CC3000_IP_ALLOC)
    {
      // Since accept and the server is blocking,
      // we will indicate in non-volatile FRAM that
      // Smart Config should be run at startup.
      SetFTCflag();
      // Clear Smart Config profile stored flag until connection established. To use the default SSID for connection, press S1 and then the reset button.
      *SmartConfigProfilestored = 0xFF;
      restartMSP430();
     }
  }

  // Restore Switch Interrupt
  RestoreSwitch();

  StopDebounceTimer();
}

