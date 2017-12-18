/**
    =========
    METERMAN METERGATEWAY
    metergateway.cpp
    =========
    Operates a 'meter gateway' that communicates with one or more 'meter nodes'
    over packet radio.  It acts as a radio gateway between these and logic
    running on a local server.  Hardware consists of a ATMEGA 328P (with RFM69
    radio), and a daughterboard to mate to a Raspberry Pi for Serial TX/RX and
    power (AKA a Pi Hat).

    This gateway could aggregate multiple nodes. These are part of a basic
    monitoring system as described at http://leehonan.com/meterman.

    Uses following libraries (with licencing):  RadioHead (GPL Version 2
    Licence), and Time (GPL 2.1).

    This program is licenced as follows.

    -----------
    MIT License
    -----------

    Copyright (c) 2017 Lee Honan (lee at leehonan dot com).

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this perMNOIion notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
 */


// *****************************************************************************
//    Main Config Parameters
//    'DEF_*' constants are defaults for configuration variables of the same
//    name. Most of these will be stored to EEPROM and can be updated by
//    serial terminal commands.

//    Have minimised use of #DEFINE, #IFDEF etc for easier debugging.
// *****************************************************************************

#include <Arduino.h>
#include <avr/wdt.h>
#include <EEPROM.h>

// http://www.airspayce.com/mikem/arduino/RadioHead/
#include <RH_RF69.h>
#include <RHReliableDatagram.h>

// https://github.com/PaulStoffregen/Time
#include <TimeLib.h>


static const int8_t FW_VERSION = 6;

// Log Levels
typedef enum {
    logNull = 0,        // outputs to serial regardless of runtime log level
    logError = 1,
    logWarn = 2,
    logInfo = 3,
    logDebug = 4
} LogLev;

// Default runtime log level.
// Can control through serial command, save to EEPROM.
static const LogLev DEF_LOG_LEVEL = logDebug;

// if using HW/HCW module must set to true:
static const bool RADIO_HIGH_POWER = true;

// Initial power level in dBm.  Use -18 to +13 for W/CW, -14 to +20 for HW/HCW:
static const int8_t DEF_TX_POWER = 20;

// Gateway ID.  Gateway is usually 1.  Nodes between 2 and 254.
// 255 is broadcast (RH_GMSG_ADDRESS).
// Nodes are added dynamically, no pre-registration.
// Implicitly trusted if they have the same network Id and key.
static const uint8_t DEF_GATEWAY_ID = 1;

// Network octets, akin to IP address but with an extra subnet (as the 4 octets
// define a subnet, with node addressing within this).
// Need at least 2 octets to be non-zero, so will have 3rd and 4th begin at 1.
static const uint8_t DEF_NETWORK_ID_O1 = 0;  //0 to 254
static const uint8_t DEF_NETWORK_ID_O2 = 0;  //0 to 254
static const uint8_t DEF_NETWORK_ID_O3 = 1;  //1 to 254 by convention
static const uint8_t DEF_NETWORK_ID_O4 = 1;  //1 to 254 by convention

// AES 128b encryption key, shared amongst nodes.  Must be 16 chars/bytes,
// chars ASCII 32 - 126 (space, symbols, letters, numbers).
static const uint8_t KEY_LENGTH = 16;
static const uint8_t DEF_ENCRYPT_KEY[KEY_LENGTH] =
        {'C','H','A','N','G','E','_','M','E','_','P','L','E','A','S','E'};

static const uint32_t SERIAL_BAUD = 115200;

// whether to align node entries to mm:00 (begin at top of minute)
static const bool DEF_ALIGN_ENTRIES = 1;

// *****************************************************************************
//    General Init - Pins
// *****************************************************************************

static const uint8_t RADIO_INTERRUPT_PIN = 2;   // IC4
                                                // uses 328P interrupt 0

static const uint8_t RADIO_SS_PIN = 10;         // IC16
                                                // HIGH is off, LOW on

static const uint8_t LED_PIN = 4;               // IC6

static const uint8_t BUTTON_PIN = 6;            // IC12
                                                // auxiliary button

// *****************************************************************************
//    General Init - General Variables
// *****************************************************************************

// variable to hold MCU reset cause
uint8_t resetFlags __attribute__ ((section(".noinit")));

uint32_t btnEventStartMillis = 0;    // button on start time in millis

// global temporary variables, used somewhat arbitrarily vs local static vars
char tmpStr[60] = "";
uint32_t tmpInt = 0ul;

static const uint8_t SERIAL_IN_BUFFER_SIZE = 40;
char serInBuff[SERIAL_IN_BUFFER_SIZE] = "";
uint8_t serialBuffPos = 0;

// *****************************************************************************
//    General Init - Config Vars
// *****************************************************************************

// Config Variables (corresponding defaults are defined above with DEF_*).  Can
// be changed with serial commands and are saved to EEPROM.
LogLev cfgLogLevel = DEF_LOG_LEVEL;
uint8_t cfgTXPower = 0;
uint8_t cfgGatewayId = 0;
uint8_t cfgNetworkId1 = 0;
uint8_t cfgNetworkId2 = 0;
uint8_t cfgNetworkId3 = 0;
uint8_t cfgNetworkId4 = 0;
uint8_t cfgEncryptKey[KEY_LENGTH] =
        {'\0','\0','\0','\0','\0','\0','\0','\0','\0','\0','\0','\0','\0',
        '\0','\0','\0'};
uint8_t cfgAlignEntries = 0;

// *****************************************************************************
//    General Init - Logging
// *****************************************************************************

// Serial event/status logging output prefixes
static const char LOG_ERROR_LBL[] PROGMEM = "ERROR";
static const char LOG_WARN_LBL[] PROGMEM = "WARN";
static const char LOG_INFO_LBL[] PROGMEM = "INFO";
static const char LOG_DEBUG_LBL[] PROGMEM = "DEBUG";

// *****************************************************************************
//    General Init - Commands (Serial)
// *****************************************************************************

// Serial command strings.
static const char SMSG_FS PROGMEM = ',';
static const char SMSG_RS PROGMEM = ';';

// Serial message (TX) string prefixes.
static const char SMSG_TX_PREFIX[] PROGMEM = "G>S:";
static const char SMSG_GTIME[] PROGMEM = "GTIME";
static const char SMSG_STIME_ACK[] PROGMEM = "STIME_ACK";
static const char SMSG_STIME_NACK[] PROGMEM = "STIME_NACK";
static const char SMSG_GWSNAP[] PROGMEM = "GWSNAP";   // dump of gateway status
static const char SMSG_NOSNAP[] PROGMEM = "NOSNAP";   // one or many nodes
static const char SMSG_GNOSNAP_NACK[] PROGMEM = "GNOSNAP_NACK";
static const char SMSG_MUPC[] PROGMEM = "MUPC";
static const char SMSG_MUP_[] PROGMEM = "MUP_";
static const char SMSG_MREB[] PROGMEM = "MREB";
static const char SMSG_GMSG[] PROGMEM = "GMSG";
static const char SMSG_SMVAL_ACK[] PROGMEM = "SMVAL_ACK";
static const char SMSG_SMVAL_NACK[] PROGMEM = "SMVAL_NACK";
static const char SMSG_SPLED_ACK[] PROGMEM = "SPLED_ACK";
static const char SMSG_SPLED_NACK[] PROGMEM = "SPLED_NACK";
static const char SMSG_SMINT_ACK[] PROGMEM = "SMINT_ACK";
static const char SMSG_SMINT_NACK[] PROGMEM = "SMINT_NACK";
static const char SMSG_SGITR_ACK[] PROGMEM = "SGITR_ACK";
static const char SMSG_SGITR_NACK[] PROGMEM = "SGITR_NACK";
static const char SMSG_NDARK[] PROGMEM = "NDARK";

// Serial message (RX) string prefixes.
static const char SMSG_RX_PREFIX[] PROGMEM = "S>G:";
static const char SMSG_STIME[] PROGMEM = "STIME";
static const char SMSG_GGWSNAP[] PROGMEM = "GGWSNAP";
static const char SMSG_GNOSNAP[] PROGMEM = "GNOSNAP";
static const char SMSG_SMVAL[] PROGMEM = "SMVAL";
static const char SMSG_SPLED[] PROGMEM = "SPLED";
static const char SMSG_SMINT[] PROGMEM = "SMINT";
static const char SMSG_SGITR[] PROGMEM = "SGITR";

// Serial command (RX) strings.

// help
static const char SER_CMD_HELP[] PROGMEM = "HELP";

// dump gateway state to console
static const char SER_CMD_DUMPGW[] PROGMEM = "DUMPG";

// dump node state to console
static const char SER_CMD_DUMPNO[] PROGMEM = "DUMPN";

// reset EEPROM config to defaults
static const char SER_CMD_RCFG[] PROGMEM = "RCFG";

// print/set time (set with TIME=[time as epoch])
static const char SER_CMD_TIME[] PROGMEM = "TIME";

// print/set loglevel (set with LOGL=[log level])
static const char SER_CMD_LOGL[] PROGMEM = "LOGL";

// print/set encryption key (set with EKEY=[key])
static const char SER_CMD_EKEY[] PROGMEM = "EKEY";

// print/set network id (set with NETI=[network id])
static const char SER_CMD_NETI[] PROGMEM = "NETI";

// print/set gateway id (set with GWID=[gateway id])
static const char SER_CMD_GWID[] PROGMEM = "GWID";

// print/set transmission power (set with TXPW=[tx power in dBi])
static const char SER_CMD_TXPW[] PROGMEM = "TXPW";

// print/set entry alignment (set with ENTA=[0,1])
static const char SER_CMD_ENTA[] PROGMEM = "ENTA";

// Array of commands, used to print list on help or invalid input
const char* const SER_CMDS[] PROGMEM = {
                SER_CMD_HELP, SER_CMD_DUMPGW, SER_CMD_DUMPNO, SER_CMD_RCFG,
                SER_CMD_TIME, SER_CMD_LOGL, SER_CMD_EKEY, SER_CMD_NETI,
                SER_CMD_GWID, SER_CMD_TXPW, SER_CMD_ENTA};

// *****************************************************************************
//    General Init - Radio Message Types
// *****************************************************************************

// Meter Rebase
static const char RMSG_MREBASE[] PROGMEM = "MREB";

// Meter Update with current
static const char RMSG_MUPC[] PROGMEM = "MUPC";

// Meter Update without current
static const char RMSG_MUP_[] PROGMEM = "MUP_";

// Instruction Request from Node to Gateway
static const char RMSG_GINR[] PROGMEM = "GINR";

// Request from Gateway to Node to temporarily increase GINR polling rate
static const char RMSG_GITR[] PROGMEM = "GITR";

// Clock Sync Ping Request from Node to Gateway
static const char RMSG_PREQ[] PROGMEM = "PREQ";

// Clock Sync Ping Response from Gateway to Node
static const char RMSG_PRSP[] PROGMEM = "PRSP";

// Meter instruction (from gateway to node) to set meter value
static const char RMSG_MVAI[] PROGMEM = "MVAI";

// Meter instruction (from gateway to node) to set meter interval
static const char RMSG_MINI[] PROGMEM = "MINI";

// Meter instruction (from gateway to node) to set puck LED
static const char RMSG_MPLI[] PROGMEM = "MPLI";

// 'no op' Meter instruction (from gateway to node)
static const char RMSG_MNOI[] PROGMEM = "MNOI";

// General purpose message (can broadcast)
static const char RMSG_GMSG[] PROGMEM = "GMSG";

// Number of seconds to wait for 'proof of life' before regarding a node as MIA,
// and alerting.  Longer than 5m usually best.
static const uint16_t POL_MSG_TIMEOUT_SEC = 600;        //10m

// *****************************************************************************
//    Radio Init
//
//    All message-driven request/response interactions are logically
//    asynchronous; but implemented by RadioHead via synchronous send/ACK
//    message pairs. Messaging initiated by client node to facilitate long sleep
//    periods.  As such, the gateway needs to queue any commands or updates for
//    meter nodes.  Max message data length is 60 *bytes*, with no
//    recognition of payload elements as anything other than a big 'string'.
//    E.g. an uint32_t of 2,000,000,000 will be 80b/10B not 32b/4B.
//
// *****************************************************************************

static const float RADIO_FREQ = 915.0f;

// Modem config per RadioHead docs.  FSK seems most reliable.
//
// From FSK_Rb4_8Fd9_6 through to FSK_Rb125Fd125 work well (could go higher).
// Use fastest rate that yields acceptable range and reasonably low TX power
// (unless running on DC adapter and not concerned with RF 'noise').

// Using FSK, Whitening, bit rate = 125kbps, modulation frequency = 125kHz.
static const RH_RF69::ModemConfigChoice MODEM_CONFIG = RH_RF69::FSK_Rb125Fd125;

// Transmit and Receive timeouts (millis).  Long timeouts can make serial
// communication etc laggy if gateway not up.
static const uint16_t TX_TIMEOUT = 800;
static const uint16_t RX_TIMEOUT = 800;

// Radio Driver and Message Manager
RH_RF69 radio(RADIO_SS_PIN, RADIO_INTERRUPT_PIN);
RHReliableDatagram msgManager(radio, cfgGatewayId);

// Last message RSSI (received signal strength indicator).  0 is highest, -100
// is lowest.
int8_t lastRSSIAtGateway = 0;
int8_t lastRSSIAtNode = 0;

// radio/node Id of last message sender
uint8_t lastMsgFrom = 0;

// Intermediate string buffer for message contents.  Used as easier to work with
// than byte buffer.  KEY_LENGTH 'fudge factor' added as while length is fixed,
// an accidental overflow can be caught and handled through validation instead
// of creating an actual overflow.
char msgBuffStr[RH_RF69_MAX_MESSAGE_LEN + KEY_LENGTH] = "";

// Actual buffer used to pass payload to/from the RadioHead library.
uint8_t radioMsgBuff[RH_RF69_MAX_MESSAGE_LEN] = "";

// *****************************************************************************
//    Meter Nodes
//
// *****************************************************************************

// Maximum meter value => 4 billion entries (which may be greater than number of
// pulses depending on interval setting).  UL type will overflow at ~4.3
// billion. Could use long long but constrained to limit message size.

// Assume all meter nodes are measuring with same unit of measure (Wh).

static const uint32_t MTR_MAX_VALUE = 4000000000ul;

struct MeterNode {
    uint8_t nodeId = 0;

    // voltage in millivolts
    uint16_t battVoltageMV = 0;
    uint32_t secondsUptime = 0ul;
    uint32_t secondsSlept = 0ul;
    uint16_t freeRAM = 0;
    uint32_t lastSeenTime = 0ul;

    // crude, does not take message latency into account
    int32_t lastClockDriftSecs = 0;

    // interval in seconds at which read entries are created (resolution)
    uint8_t meterInterval = 0;
    uint32_t lastEntryFinishTime = 0ul;

    uint16_t meterImpPerKwh = 0;

    // accumulated meter count
    uint32_t lastMeterValue = 0ul;
    double lastCurrentRMS = 0.0;

    // Puck LED rate vs watched meter LED.  0=off.
    uint8_t puckLEDRate = 0;

    // Max pulse time in millis.  Ignored if longer than meter's pulse.
    uint16_t puckLEDTime = 0;

    // new values to set.  if not vals below will reset meter node...
    uint8_t newMeterInterval = 0;
    uint32_t newMeterValue = 0ul;
    uint8_t newPuckLEDRate = UINT8_MAX;
    uint16_t newPuckLEDTime = UINT16_MAX;
    uint16_t tmpGinrPollRate = 0;
    uint16_t tmpGinrPollPeriod = 0;

    // last RSSI from node
    int8_t lastNodeRSSI = 0;
};

static const uint8_t MAX_MTR_NODES = 5;       // ~50B per node

struct MeterNode meterNodes[MAX_MTR_NODES];


// *****************************************************************************
//    Timers
// *****************************************************************************

// Basetime is UNIX epoch in seconds (since 1 Jan 1970 00:00:00).
// Set from local server.
uint32_t baseTime = 0ul;

// Default UNIX epoch time.
static const uint32_t INIT_TIME = 1483228800ul;        //  1 Jan 2017 00:00:00

// Millis() value converted to seconds taken when basetime set.
uint32_t baseTimeAsLocalSecs = 0ul;

// Set using time from local Server.
uint32_t whenBooted = 0ul;


// *****************************************************************************
//    Runtime Logging
//
// *****************************************************************************

// used to detect when a new line is started, to write log level
bool newLogLine = true;


void printNewLine(LogLev logLevel){
    if (cfgLogLevel >= logLevel){
        Serial.write("\r\n");
        newLogLine = true;
    }
}


void print_P(const char* flashStr){
    strcpy_P(tmpStr, flashStr);
    Serial.write(tmpStr);
}


void println_P(const char* flashStr){
    print_P(flashStr);
    printNewLine(logNull);
}


void printLogLevel(LogLev logLevel, bool printColon){
    if (logLevel == logNull)
        return;

    if (logLevel == logError)
        print_P(LOG_ERROR_LBL);
    else if (logLevel == logWarn)
        print_P(LOG_WARN_LBL);
    else if (logLevel == logInfo)
        print_P(LOG_INFO_LBL);
    else if (logLevel == logDebug)
        print_P(LOG_DEBUG_LBL);

    if (printColon)
        Serial.write(": ");
}


void writeLog(char* debugText, LogLev logLevel){
    if (cfgLogLevel >= logLevel){
        if (newLogLine)
            printLogLevel(logLevel, true);
        Serial.write(debugText);
        newLogLine = false;
    }
}


void writeLogLn(char* debugText, LogLev logLevel){
    if (cfgLogLevel >= logLevel){
        if (newLogLine)
            printLogLevel(logLevel, true);
        Serial.write(debugText);
        printNewLine(logLevel);
    }
}


void writeLog(const char* debugText, LogLev logLevel){
    writeLog((char*)debugText, logLevel);
}


void writeLogLn(const char* debugText, LogLev logLevel){
    writeLog(debugText, logLevel);
    printNewLine(logLevel);
}


void writeLog(uint32_t debugText, LogLev logLevel){
    static char printStr[12] = "";
    sprintf(printStr, "%lu", debugText);
    writeLog(printStr, logLevel);
}


void writeLogLn(uint32_t debugText, LogLev logLevel){
    writeLog(debugText, logLevel);
    printNewLine(logLevel);
}


void writeLog(uint16_t debugText, LogLev logLevel){
    writeLog((uint32_t)debugText, logLevel);
}


void writeLogLn(uint16_t debugText, LogLev logLevel){
    writeLog((uint32_t)debugText, logLevel);
    printNewLine(logLevel);
}


void writeLog(int32_t debugText, LogLev logLevel){
    static char printStr[12] = "";
    sprintf(printStr, "%ld", debugText);
    writeLog(printStr, logLevel);
}


void writeLogLn(int32_t debugText, LogLev logLevel){
    writeLog(debugText, logLevel);
    printNewLine(logLevel);
}


void writeLog(int16_t debugText, LogLev logLevel){
    writeLog((int32_t)debugText, logLevel);
}


void writeLogLn(int16_t debugText, LogLev logLevel){
    writeLog((int32_t)debugText, logLevel);
    printNewLine(logLevel);
}


void writeLog(double debugText, LogLev logLevel){
    static char printStr[12] = "";
    // use int hack as dtostr takes up too much memory, sprintf floats
    // not supported...
    sprintf(printStr, "%d.%02d", (int)debugText, (int)(debugText*100)%100);
    writeLog(printStr, logLevel);
}


void writeLogLn(double debugText, LogLev logLevel){
    writeLog(debugText, logLevel);
    printNewLine(logLevel);
}


void writeLogF(const __FlashStringHelper* debugText, LogLev logLevel){
    // easiest to do redundant writeLog as can't simply cast FlashString

    if (cfgLogLevel >= logLevel){
        if (newLogLine)
            printLogLevel(logLevel, true);
        Serial.print(debugText);
        newLogLine = false;
    }
}


void writeLogLnF(const __FlashStringHelper* debugText, LogLev logLevel){

    if (cfgLogLevel >= logLevel){
        if (newLogLine)
            printLogLevel(logLevel, true);
        Serial.print(debugText);
    }
    printNewLine(logLevel);
}


// *****************************************************************************
//
//    Functions & Main Loop...
//
// *****************************************************************************

uint32_t getNowTimestampSec();
void resetConfig();
void printResetVal(uint8_t resetVal);

void sendRadioMsg(uint8_t recipient, bool checkReply);


void print2Digits(int digits){
    if(digits < 10)
        writeLogF(F("0"), logNull);
    writeLog(digits, logNull);
}


void printPrompt(){
    writeLogF(F(" > "), logNull);
}


void printNetworkId(){
    writeLog(cfgNetworkId1, logNull);
    writeLogF(F("."), logNull);
    writeLog(cfgNetworkId2, logNull);
    writeLogF(F("."), logNull);
    writeLog(cfgNetworkId3, logNull);
    writeLogF(F("."), logNull);
    writeLog(cfgNetworkId4, logNull);
}


void printWhValue(uint32_t whValue, LogLev logLevel){
    if (cfgLogLevel >= logLevel){
        writeLog(whValue, logNull);
        writeLogF(F(" Wh"), logNull);
    }
}


void printCmdHelp(){
    printPrompt();
    writeLogF(F("Cmds: "), logNull);
    for (uint8_t i = 0; i <= 10; i++){
        print_P((char*)pgm_read_word(&(SER_CMDS[i])));
        writeLogF(F(" "), logNull);
    }
    printNewLine(logNull);
}


bool strStartsWith(const char *strBody, const char *strPrefix){
    /*
        Tests if string starts with given prefix.  Case insensitive.
     */

    return (strncasecmp(strBody, strPrefix, strlen(strPrefix)) == 0);
}


uint8_t strStartsWithP(const char *strBody, const char *strPrefix){
    /*
        Variant of strStartsWith for PROGMEM prefix, test for presence of
        '=' (used to distinguish a setter from getter, e.g. 'TIME=').
     */

    static char withEq [] = "";
    strcpy_P(withEq, strPrefix);
    strcat_P(withEq, PSTR("="));
    if(strStartsWith(strBody, withEq))
        return 2;   // setter
    else if(strncasecmp_P(strBody, strPrefix, strlen_P(strPrefix)) == 0)
        return 1;   // getter/normal
    else
        return 0;   // no match
}


uint8_t strStartsWithP(const char *strBody, const char *strPrefix1,
            const char *strPrefix2){
    /*
        Variant of strStartsWithP for two PROGMEM prefixes.
     */
    strcpy_P(tmpStr, strPrefix1);
    strcat_P(tmpStr, strPrefix2);
    if (strStartsWith(strBody, tmpStr))
        return 1;
    else
        return 0;
}


uint16_t freeRAM(){
    /*
        Returns free SRAM in bytes (328P has 2kB total)
     */
    extern int __heap_start, *__brkval;
    int v;
    return (uint16_t) &v - (__brkval == 0 ? (int) &__heap_start :
            (int) __brkval);
}


void printTime(uint32_t timestampSec, LogLev logLevel) {
    /*
       Print formatted timestamp to serial out if runtime log level is >=
       logLevel
    */
    if (cfgLogLevel < logLevel)
        return;

    tmElements_t timeTME;
    breakTime(timestampSec, timeTME);

    print2Digits(1970 + timeTME.Year);
    writeLogF(F("-"), logNull);
    print2Digits(timeTME.Month);
    writeLogF(F("-"), logNull);
    print2Digits(timeTME.Day);
    writeLogF(F(" "), logNull);
    print2Digits(timeTME.Hour);
    writeLogF(F(":"), logNull);
    print2Digits(timeTME.Minute);
    writeLogF(F(":"), logNull);
    print2Digits(timeTME.Second);
}


uint32_t getNowTimestampSec(){
    /*
       Returns synthesized timestamp given sync with server and local millis
       timer.  Accuracy will require frequent sync and no use of sleep.
    */

    static uint32_t secsFromMillis = 0ul;
    secsFromMillis = millis() / 1000;

    // Check for millis overflow (every ~49d), use different method.
    //
    // If millis (as sec) is less than base time seconds (when clock last set),
    // then the time elapsed since basetime will be (UINT32_MAX - base time
    // seconds + millisAsSeconds).
    //
    // Otherwise it is millisAsSeconds - base time seconds.
    //
    // Assumes frequent RTC sync and rebase will make such adjustments small
    // enough to fit an uint32_t.

    if (secsFromMillis < baseTimeAsLocalSecs)
        return (baseTime + secsFromMillis + (UINT32_MAX - baseTimeAsLocalSecs));
    else
        return (baseTime + secsFromMillis - baseTimeAsLocalSecs);
}


void adjustTSVar(uint32_t * timestampVar, uint32_t timeSecs){
    /*
       Adjust global timestamps to avoid negative/wild results from duration
       calcs
    */
    static uint64_t adjTime = 0ul;

    adjTime = timeSecs - (getNowTimestampSec() - (*timestampVar));
    *timestampVar = adjTime >= 0 ? (uint32_t)adjTime : 0ul;
}


void setNowTimestampSec(uint32_t timeSecs){
    /*
       Sets UTC time in seconds since UNIX Epoch @ midnight Jan 1 1970
    */

     baseTime = timeSecs;
     baseTimeAsLocalSecs = millis() / 1000;

     if (whenBooted <= INIT_TIME)
        whenBooted = timeSecs;
     else
        adjustTSVar(&whenBooted, timeSecs);

    // reset when last seen times for nodes
    for (uint8_t i = 0; i < MAX_MTR_NODES; i++)
        meterNodes[i].lastSeenTime = UINT32_MAX;

     // push out 0 value read to update time, force rebase
     writeLogF(F("Time="), logDebug);
     printTime(getNowTimestampSec(), logDebug);
     printNewLine(logDebug);
}


int8_t getTXPowMin(){
    if (RADIO_HIGH_POWER)
        return -2;
    else
        return -18;
}


int8_t getTXPowMax(){
    if (RADIO_HIGH_POWER)
        return 20;
    else
        return 13;
}


bool isTXPowValid(int8_t TXPowValue){
    return (TXPowValue >= getTXPowMin() && TXPowValue <= getTXPowMax());
}


void putConfigToMem(){
    /*
       Uses EEPROM Put function to write current config to EEPROM.  Only writes
       to chip if value differs (i.e. update).
    */

    uint8_t eeAddress = 0;
    writeLogLnF(F("Updt ROM"), logInfo);
    wdt_reset();
    EEPROM.put(eeAddress, (uint8_t)cfgLogLevel);
    eeAddress += sizeof((uint8_t)cfgLogLevel);
    EEPROM.put(eeAddress, cfgTXPower);
    eeAddress += sizeof(cfgTXPower);
    EEPROM.put(eeAddress, cfgGatewayId);
    eeAddress += sizeof(cfgGatewayId);
    EEPROM.put(eeAddress, cfgNetworkId1);
    eeAddress += sizeof(cfgNetworkId1);
    EEPROM.put(eeAddress, cfgNetworkId2);
    eeAddress += sizeof(cfgNetworkId2);
    EEPROM.put(eeAddress, cfgNetworkId3);
    eeAddress += sizeof(cfgNetworkId3);
    EEPROM.put(eeAddress, cfgNetworkId4);
    eeAddress += sizeof(cfgNetworkId4);
    wdt_reset();
    for (uint8_t i = 0; i < KEY_LENGTH; i++){
        EEPROM.put(eeAddress, cfgEncryptKey[i]);
        eeAddress++;
    }
    EEPROM.put(eeAddress, cfgAlignEntries);
}


void getConfigFromMem(){
    /*
       Reads from EEPROM, testing each value's validity.  Any failure will
       result in EEPROM being re-written.
    */

    uint8_t eeAddress = 0;
    uint8_t byteVal = 0;
    int8_t intVal = 0;
    uint8_t byteValArray[KEY_LENGTH] = {0};
    bool EEPROMValid = true;

    writeLogLnF(F("Read ROM"), logInfo);
    wdt_reset();
    EEPROM.get(eeAddress, byteVal);
    if (byteVal >= logNull && byteVal <= logDebug)
        cfgLogLevel = (LogLev)byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    EEPROM.get(eeAddress, intVal);
    if (isTXPowValid(intVal))
        cfgTXPower = intVal;
    else
        EEPROMValid = false;
    eeAddress++;

    EEPROM.get(eeAddress, byteVal);
    if (byteVal > 0 && byteVal < 255)
        cfgGatewayId = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    EEPROM.get(eeAddress, byteVal);
    if (byteVal >= 0 && byteVal < 255)
        cfgNetworkId1 = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    wdt_reset();

    EEPROM.get(eeAddress, byteVal);
    if (byteVal >= 0 && byteVal < 255)
        cfgNetworkId2 = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    EEPROM.get(eeAddress, byteVal);
    if (byteVal > 0 && byteVal < 255)
        cfgNetworkId3 = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    EEPROM.get(eeAddress, byteVal);
    if (byteVal > 0 && byteVal < 255)
        cfgNetworkId4 = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    memset(byteValArray, '\0', sizeof(byteValArray));
    for (uint8_t i = 0; i < KEY_LENGTH; i++){
        EEPROM.get(eeAddress, byteValArray[i]);
        if (byteValArray[i] < 32 || byteValArray[i] > 126)
            EEPROMValid = false;
        eeAddress++;
    }

    if (EEPROMValid){
        memset(cfgEncryptKey, '\0', sizeof(cfgEncryptKey));
        memcpy(cfgEncryptKey, byteValArray, sizeof(byteValArray));
    }

    EEPROM.get(eeAddress, byteVal);
    if (byteVal >= 0 && byteVal <= 1)
        cfgAlignEntries = byteVal;
    else
        EEPROMValid = false;
    eeAddress++;

    if (! EEPROMValid){
        writeLogLnF(F("ROM Bad"), logError);
        resetConfig();
        putConfigToMem();
    }
}


void applyRadioConfig(){
    /*
       Applies current radio config parameters.  May be invoked from changes to
       config through serial commands.
    */

    writeLogLnF(F("Radio Init"), logDebug);

    if (!msgManager.init())         // also intialises radio driver
      writeLogLnF(F("MsgMgr fail"), logError);

    msgManager.setThisAddress(cfgGatewayId);
    msgManager.setTimeout(TX_TIMEOUT);

    if (!radio.setModemConfig(MODEM_CONFIG)) {
        writeLogLnF(F("ModemCfg fail"), logError);
    }

    if (!radio.setFrequency(RADIO_FREQ)) {
        writeLogLnF(F("SetFreq fail"), logError);
    }

    radio.setTxPower(cfgTXPower, RADIO_HIGH_POWER);

    uint8_t syncwords[] =
            {cfgNetworkId1, cfgNetworkId2, cfgNetworkId3, cfgNetworkId4};
    radio.setSyncWords(syncwords, sizeof(syncwords));

    radio.setEncryptionKey(cfgEncryptKey);
}


void resetConfig(){
    /*
       Sets config parameters to defaults
    */
    cfgLogLevel = DEF_LOG_LEVEL;
    cfgTXPower = DEF_TX_POWER;
    cfgGatewayId = DEF_GATEWAY_ID;
    cfgNetworkId1 = DEF_NETWORK_ID_O1;
    cfgNetworkId2 = DEF_NETWORK_ID_O2;
    cfgNetworkId3 = DEF_NETWORK_ID_O3;
    cfgNetworkId4 = DEF_NETWORK_ID_O4;
    memcpy(cfgEncryptKey, DEF_ENCRYPT_KEY, KEY_LENGTH);
    cfgAlignEntries = DEF_ALIGN_ENTRIES;
    putConfigToMem();
    applyRadioConfig();
}


uint8_t getNodeIxById(uint8_t nodeId){
    /*
       Returns node's index in meterNodes array given a nodeId
    */
    for (uint8_t i = 0; i < MAX_MTR_NODES; i++){
        if (meterNodes[i].nodeId == nodeId)
            return i;
    }
    return UINT8_MAX;   // to signify false, as 0 is first array element
}


uint8_t getNodeIxByIdWithCreate(uint8_t nodeId){
    /*
       If node can't be found find first empty element and insert it, returning
       the index. Returns UINT8_MAX if array is full.
    */
    static uint8_t nodeIx = 0;
    nodeIx = getNodeIxById(nodeId);

    if (nodeIx == UINT8_MAX){
        // not found, so create
        for (uint8_t i = 0; i < MAX_MTR_NODES; i++){
            if (meterNodes[i].nodeId == 0){
                meterNodes[i].nodeId = nodeId;
                return i;
            }
        }
        writeLogF(F("Can't add node "), logError);
        writeLog(nodeId, logError);
        writeLogLnF(F(": array full"), logError);

    }
    return nodeIx;      // UINT8_MAX if not found or created
}


void printNodeSnapByIx(uint8_t nodeIx, bool isMessage){
    /*
       Prints a node dump to serial out or message format.
   */
    char fieldDelimiter[3] = "";

    if (isMessage)
        strcpy(fieldDelimiter, ",");
    else
        strcpy(fieldDelimiter, "\r\n");

    if (not isMessage) {
        printPrompt();
        writeLogF(F("node_id="), logNull);
    }
    else
        Serial.write(SMSG_RS);

    writeLog(meterNodes[nodeIx].nodeId, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("batt_v="), logNull);
    }
    writeLog(meterNodes[nodeIx].battVoltageMV, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("up_time="), logNull);
    }
    writeLog(meterNodes[nodeIx].secondsUptime, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("sleep_time="), logNull);
    }
    writeLog(meterNodes[nodeIx].secondsSlept, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("free_ram="), logNull);
    }
    writeLog(meterNodes[nodeIx].freeRAM, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("when_last_seen="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastSeenTime, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("last_clock_drift="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastClockDriftSecs, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("mtr_interval="), logNull);
    }
    writeLog(meterNodes[nodeIx].meterInterval, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("mtr_imp_per_kwh="), logNull);
    }
    writeLog(meterNodes[nodeIx].meterImpPerKwh, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("last_meter_entry_finish="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastEntryFinishTime, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("last_mtr_val="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastMeterValue, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("last_curr_val="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastCurrentRMS, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("p_led_rate="), logNull);
    }
    writeLog(meterNodes[nodeIx].puckLEDRate, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("p_led_time="), logNull);
    }
    writeLog(meterNodes[nodeIx].puckLEDTime, logNull);
    Serial.write(fieldDelimiter);

    if (not isMessage) {
        printPrompt();
        writeLogF(F("last_rssi="), logNull);
    }
    writeLog(meterNodes[nodeIx].lastNodeRSSI, logNull);

    if (not isMessage)
        printNewLine(logNull);
}


void printNodes(bool isMessage){
    for (uint8_t i = 0; i < MAX_MTR_NODES; i++)
        if (meterNodes[i].nodeId != 0){
            printNodeSnapByIx(i, isMessage);
            printNewLine(logNull);
        }
}


void sendSerGetTime(){
    /*
       Sends a request message to the server to update time
   */
   wdt_reset();
   print_P(SMSG_TX_PREFIX);
   println_P(SMSG_GTIME);
}


void sendSerMeterUpdate(uint8_t nodeId, bool isWithCurrent){
    /*
       Pass through a meter update message (in message buffer) to the server
   */
    wdt_reset();
    print_P(SMSG_TX_PREFIX);
    print_P(isWithCurrent ? SMSG_MUPC: SMSG_MUP_);

    Serial.write(SMSG_RS);
    writeLog(nodeId, logNull);
    Serial.write(SMSG_FS);
    writeLogLn(msgBuffStr, logNull);
}


void sendSerMeterRebase(uint8_t nodeId){
    /*
       Pass through a meter rebase message (in message buffer) to the server
   */
    wdt_reset();
    print_P(SMSG_TX_PREFIX);
    print_P(SMSG_MREB);
    Serial.write(SMSG_RS);
    writeLog(nodeId, logNull);
    Serial.write(SMSG_FS);
    writeLogLn(msgBuffStr, logNull);
}


void sendSerNodeGenMsg(uint8_t nodeId){
    /*
       Pass through a node general purpose message (in message buffer) to the
       server
    */
    wdt_reset();
    print_P(SMSG_TX_PREFIX);
    print_P(SMSG_GMSG);
    Serial.write(SMSG_RS);
    writeLog(nodeId, logNull);
    Serial.write(SMSG_FS);
    writeLog(msgBuffStr, logNull);
    Serial.write(' ');
    if (strStartsWithP(msgBuffStr, PSTR("GMSG,BOOT"))){
        sscanf(msgBuffStr, "%*[^,],BOOT %lu", &tmpInt);
        printResetVal((uint8_t)tmpInt);
    }
    printNewLine(logNull);
}


int readLineSerial(int readChar, char *serialBuffer){
    /*
       Reads a CR-terminated line from serial RX.  Will reset state when a CR
       occurs.
   */

    int retPos;
    wdt_reset();

    // Check if invalid chars.  Ignore.
    if (readChar > 0 && (readChar < 32 || readChar > 127) &&
            readChar != '\r' && readChar != '\b')
        return -1;

    // Check if at maximum input length.  Ignore until deleted or return
    // pressed.
    else if (readChar > 0 && serialBuffPos >= (SERIAL_IN_BUFFER_SIZE -1) &&
            readChar != '\r' && readChar != '\b')
        return -1;

    else if (readChar > 0) {   // allow for null terminator
        switch (readChar) {
            case '\b':
                serialBuffPos--;
                serialBuffer[serialBuffPos] = '\0';

                // hacky way to realise working backspace -
                // backspace+space+backspace
                Serial.write("\b\x20\b");
                break;
            case '\r': // Return on CR
                printNewLine(logNull);
                retPos = serialBuffPos;
                serialBuffPos = 0;  // reset index
                return retPos;
            default:
                if (serialBuffPos < SERIAL_IN_BUFFER_SIZE - 1) {
                    serialBuffer[serialBuffPos++] = readChar;
                    serialBuffer[serialBuffPos] = '\0';
                    // echo the value that was read back to the serial port.
                    Serial.write(readChar);
                }
        }
    }
    // No end of line has been found, so return -1.
    return -1;
}


void processSerialCommand(){
    /*
       Processes a serial command from the serial buffer
   */

    enum cmdValid {
        invalid = 0,
        valid = 1,
        dump = 2
    } cmdStatus = invalid;

    char cmdVal[20];
    memset(cmdVal, '\0', sizeof(cmdVal));
    tmpInt = 0ul;

    wdt_reset();

    // help
    if (strStartsWithP(serInBuff, SER_CMD_HELP) == 1){
        printCmdHelp();
        cmdStatus = valid;
    }

    // Dump gateway - will trigger all other 'query' commands
    if (strStartsWithP(serInBuff, SER_CMD_DUMPGW) == 1){
        printPrompt();
        writeLogF(F("Booted="), logNull);
        printTime(whenBooted, logNull);
        printNewLine(logNull);

        printPrompt();
        writeLogF(F("Free RAM (B)="), logNull);
        writeLogLn(freeRAM(), logNull);

        cmdStatus = dump;
    }

    // reset config
    if (strStartsWithP(serInBuff, SER_CMD_RCFG) == 1){
        resetConfig();
        cmdStatus = valid;
    }

    // set time
    if (strStartsWithP(serInBuff, SER_CMD_TIME) == 2){
        strncpy(cmdVal, serInBuff + strlen_P(SER_CMD_TIME) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_TIME) -1));
        tmpInt = strtoul(cmdVal,NULL,0);
        if (tmpInt > 0){
            setNowTimestampSec(tmpInt);
            cmdStatus = valid;
        }
        else{
            printPrompt();
            writeLogF(F("Bad Time"), logNull);
        }
    }

    // print time, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_TIME) >= 1){
        printPrompt();
        writeLogF(F("Time="), logNull);
        printTime(getNowTimestampSec(), logNull);
        writeLogF(F(" / "), logNull);
        writeLog(getNowTimestampSec(), logNull);
        printNewLine(logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set log level
    if (strStartsWithP(serInBuff, SER_CMD_LOGL) == 2){
        strncpy(tmpStr, serInBuff + strlen_P(SER_CMD_LOGL) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_LOGL) -1));
        if (strStartsWithP(tmpStr, LOG_ERROR_LBL) == 1)
            cfgLogLevel = logError;
        else if (strStartsWithP(tmpStr, LOG_WARN_LBL) == 1)
            cfgLogLevel = logWarn;
        else if (strStartsWithP(tmpStr, LOG_INFO_LBL) == 1)
            cfgLogLevel = logInfo;
        else if (strStartsWithP(tmpStr, LOG_DEBUG_LBL) == 1)
            cfgLogLevel = logDebug;
        else{
            printPrompt();
            writeLogLnF(F("Bad LogLev"), logNull);
            return;
        }
        putConfigToMem();  // apply config and write to EEPROM
        cmdStatus = valid;
    }

    // print log level, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_LOGL) >= 1){
        printPrompt();
        writeLogF(F("LogLev="), logNull);
        printLogLevel(cfgLogLevel, false);
        printNewLine(logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set radio encryption key
    if (strStartsWithP(serInBuff, SER_CMD_EKEY) == 2){
        strncpy(tmpStr, serInBuff + strlen_P(SER_CMDS[6]) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMDS[6]) -1));
        if (strlen(tmpStr) != KEY_LENGTH){
            printPrompt();
            writeLogLnF(F("Bad Key"), logNull);
        }
        else{
            memcpy(cfgEncryptKey, tmpStr, strlen(tmpStr));
            putConfigToMem();
            applyRadioConfig();
            cmdStatus = valid;
        }
    }

    // print radio encryption key, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_EKEY) >= 1){
        printPrompt();
        writeLogF(F("Key="), logNull);
        Serial.write(cfgEncryptKey, sizeof(cfgEncryptKey));
        printNewLine(logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set radio net id
    if (strStartsWithP(serInBuff, SER_CMD_NETI) == 2){
        strncpy(tmpStr, serInBuff + strlen_P(SER_CMD_NETI) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_NETI) -1));
        uint8_t addr1 = 0;
        uint8_t addr2 = 0;
        uint8_t addr3 = 0;
        uint8_t addr4 = 0;
        if(sscanf(tmpStr, "%" SCNd8 ".%" SCNd8 ".%" SCNd8 ".%" SCNd8,
                &addr1, &addr2, &addr3, &addr4) != 4){
            printPrompt();
            writeLogLnF(F("Bad Addr"), logNull);
        }
        else{
            cfgNetworkId1 = addr1;
            cfgNetworkId2 = addr2;
            cfgNetworkId3 = addr3;
            cfgNetworkId4 = addr4;
            putConfigToMem();
            applyRadioConfig();
            cmdStatus = valid;
        }
    }

    // print radio net id, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_NETI) >= 1){
        printPrompt();
        writeLogF(F("Net Id="), logNull);
        printNetworkId();
        printNewLine(logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set gateway id
    if (strStartsWithP(serInBuff, SER_CMD_GWID) == 2){
        strncpy(cmdVal, serInBuff + strlen_P(SER_CMD_GWID) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_GWID) -1));
        tmpInt = strtoul(cmdVal,NULL,0);
        if (tmpInt < 1 || tmpInt > 253){
            printPrompt();
            writeLogLnF(F("Bad Gway Id"), logNull);
        }
        else{
            cfgGatewayId = tmpInt;
            putConfigToMem();
            applyRadioConfig();
            cmdStatus = valid;
        }
    }

    // print gateway id, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_GWID) >= 1){
        printPrompt();
        writeLogF(F("Gway Id="), logNull);
        writeLogLn(cfgGatewayId, logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set TX power
    if (strStartsWithP(serInBuff, SER_CMD_TXPW) == 2){
        strncpy(cmdVal, serInBuff + strlen_P(SER_CMD_TXPW) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_TXPW) -1));
        static int16_t txPow = 0;
        txPow = strtol(cmdVal,NULL,0);

        if (! isTXPowValid(txPow)){
            printPrompt();
            writeLogLnF(F("Bad TX Power"), logNull);
        }
        else{
            cfgTXPower = txPow;
            putConfigToMem();
            applyRadioConfig();
            cmdStatus = valid;
        }
    }

    // print radio/rssi stats, done when target rssi or tx power read/set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_TXPW) >= 1){
        printPrompt();
        writeLogF(F("TX Pow="), logNull);
        writeLogLn(cfgTXPower, logNull);
        printPrompt();
        writeLogF(F("Last SSI @ Gway="), logNull);
        writeLog(lastRSSIAtGateway, logNull);
        writeLogF(F(" from node "), logNull);
        writeLogLn(lastMsgFrom, logNull);
        printPrompt();
        writeLogF(F("Last SSI @ Node "), logNull);
        writeLog(lastMsgFrom, logNull);
        writeLogF(F("="), logNull);
        writeLogLn(lastRSSIAtNode, logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    // set entry alignment
    if (strStartsWithP(serInBuff, SER_CMD_ENTA) == 2){
        strncpy(cmdVal, serInBuff + strlen_P(SER_CMD_ENTA) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_ENTA) -1));
        tmpInt = strtoul(cmdVal,NULL,0);
        if (tmpInt == 0 || tmpInt == 1){
            cfgAlignEntries = tmpInt;
            putConfigToMem();
            cmdStatus = valid;
        }
        else{
            printPrompt();
            writeLogLnF(F("Bad ENTA"), logNull);
        }
    }

    // print entry alignment, also echoes after being set
    if (cmdStatus == dump || strStartsWithP(serInBuff, SER_CMD_ENTA) >= 1){
        printPrompt();
        writeLogF(F("Entry Algn="), logNull);
        writeLogLn(cfgAlignEntries, logNull);
        if (cmdStatus != dump)
            cmdStatus = valid;
    }

    if (strStartsWithP(serInBuff, SER_CMD_DUMPNO) == 1){
        printNodes(false);
        cmdStatus = valid;
    }

    if (strStartsWithP(serInBuff, SER_CMD_DUMPNO) == 2){
        strncpy(tmpStr, serInBuff + strlen_P(SER_CMD_DUMPNO) + 1,
                (strlen(serInBuff) - strlen_P(SER_CMD_DUMPNO) -1));
        tmpInt = strtoul(tmpStr,NULL,0);
        if (tmpInt < 2 || tmpInt > 254){
            printPrompt();
            writeLogLnF(F("Bad Node Id (2-253, 254 for all)"), logNull);
        }
        else if (tmpInt == 254)
            printNodes(false);
        else {
            static uint8_t nodeIx = 0;
            nodeIx = getNodeIxById(tmpInt);
            if (nodeIx < UINT8_MAX){
                printNodeSnapByIx(nodeIx, false);
                printNewLine(logNull);
            }
            else {
                printPrompt();
                writeLogLnF(F("Node is unknown"), logNull);
            }
        }
        cmdStatus = valid;
    }

    if (cmdStatus == invalid){
        printPrompt();
        writeLogLnF(F("Bad Cmd"), logNull);
        printCmdHelp();
    }
}


void processSerialMessage(){
    /*
       Processes a serial message from the serial buffer.  Minimal validation.
    */
    static uint8_t nodeIx = UINT8_MAX;
    static uint8_t nodeId = 0;

    wdt_reset();

    // Time set instruction.  Form is [STIME,new_epoch_time_utc].
    if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_STIME) == 1){
        strncpy(tmpStr, serInBuff + strlen_P(SMSG_RX_PREFIX) +
                strlen_P(SMSG_STIME) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_STIME)));
        tmpInt = strtoul(tmpStr,NULL,0);
        if (tmpInt > 0){
            setNowTimestampSec(tmpInt);
            // write-back ACK
            print_P(SMSG_TX_PREFIX);
            println_P(SMSG_STIME_ACK);
            writeLogF(F("Set time on svr inst="), logDebug);
            printTime(getNowTimestampSec(), logDebug);
            writeLogLn("", logDebug);
        }
        else {
            print_P(SMSG_TX_PREFIX);
            println_P(SMSG_STIME_NACK);
            writeLogF(F("Bad STIME from server"), logWarn);
            writeLogLn("", logWarn);
        }
    }

    // Request for gateway status dump.  Form is [GGWSNAP].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_GGWSNAP) == 1){
        print_P(SMSG_TX_PREFIX);
        print_P(SMSG_GWSNAP);
        Serial.write(SMSG_RS);
        writeLog(cfgGatewayId, logNull);
        Serial.write(SMSG_FS);
        writeLog(whenBooted, logNull);
        Serial.write(SMSG_FS);
        writeLog(freeRAM(), logNull);
        Serial.write(SMSG_FS);
        writeLog(getNowTimestampSec(), logNull);
        Serial.write(SMSG_FS);
        printLogLevel(cfgLogLevel, false);
        Serial.write(SMSG_FS);
        Serial.write(cfgEncryptKey, sizeof(cfgEncryptKey));
        Serial.write(SMSG_FS);
        printNetworkId();
        Serial.write(SMSG_FS);
        writeLog(cfgTXPower, logNull);
        printNewLine(logNull);
    }

    // Request for node snapshot.  Form is [GNOSNAP,node_id].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_GNOSNAP) == 1){
        strncpy(tmpStr, serInBuff + strlen_P(SMSG_RX_PREFIX) +
                strlen_P(SMSG_GNOSNAP) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_GNOSNAP)));
        nodeId = strtoul(tmpStr,NULL,0);
        nodeIx = getNodeIxById(nodeId);
        print_P(SMSG_TX_PREFIX);
        if (nodeId == 254){
            // return all
            print_P(SMSG_NOSNAP);
            printNodes(true);
            printNewLine(logNull);
        }
        else if (nodeIx < UINT8_MAX){
            // node exists in array
            print_P(SMSG_NOSNAP);
            printNodeSnapByIx(nodeIx, true);
            printNewLine(logNull);
        }
        else{
            print_P(SMSG_GNOSNAP_NACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
        }
    }

    // Request to reset node meter value.
    // Form is [SMVAL,node_id,new_meter_value].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_SMVAL) == 1){
        strncpy(tmpStr, serInBuff + strlen_P(SMSG_RX_PREFIX) +
                strlen_P(SMSG_SMVAL) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_SMVAL)));
        static uint32_t newMeterValue = 0ul;
        sscanf(tmpStr, "%hhu,%lu", &nodeId, &newMeterValue);
        nodeIx = getNodeIxById(nodeId);
        print_P(SMSG_TX_PREFIX);
        if (nodeIx < UINT8_MAX &&
                (newMeterValue > 0 && newMeterValue < UINT32_MAX)){
            meterNodes[nodeIx].newMeterValue = newMeterValue;
            print_P(SMSG_SMVAL_ACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Set meter on svr inst"), logInfo);
        }
        else{
            print_P(SMSG_SMVAL_NACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Bad set meter inst"), logWarn);
        }
        writeLogF(F(". Node="), logInfo);
        writeLog(nodeId, logInfo);
        writeLogF(F(", New value="), logInfo);
        printWhValue(newMeterValue, logInfo);
        printNewLine(logInfo);
    }

    // Request to set node meter puck LED pulse rate and time.  Form is
    // [SPLED,node_id,new_puck_led_rate,new_puck_led_time].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_SPLED) == 1){
        strncpy(tmpStr, serInBuff + strlen_P(SMSG_RX_PREFIX) +
                strlen_P(SMSG_SPLED) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_SPLED)));
        static uint32_t newPuckLEDRate = 0ul;
        static uint32_t newPuckLEDTime = 0ul;
        sscanf(tmpStr, "%hhu,%lu,%lu",
                    &nodeId, &newPuckLEDRate, &newPuckLEDTime);
        nodeIx = getNodeIxById(nodeId);
        print_P(SMSG_TX_PREFIX);
        if (nodeIx < UINT8_MAX && newPuckLEDRate < UINT8_MAX
                && newPuckLEDTime <= 3000){
            meterNodes[nodeIx].newPuckLEDRate = newPuckLEDRate;
            meterNodes[nodeIx].newPuckLEDTime = newPuckLEDTime;
            print_P(SMSG_SPLED_ACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Set meter LED on svr inst"), logInfo);
        }
        else{
            print_P(SMSG_SPLED_NACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Bad set meter LED svr inst"), logWarn);
        }
        writeLogF(F(". Node="), logInfo);
        writeLog(nodeId, logInfo);
        writeLogF(F(", meter pulse per flash="), logInfo);
        writeLog(newPuckLEDRate, logInfo);
        writeLogF(F(", time (ms)="), logInfo);
        writeLogLn(newPuckLEDTime, logInfo);
    }

    // Request to set node meter interval rate (i.e. period of each interval
    // entry, e.g. 5s, which may be an aggregation of many reads).  Form is
    // [SMINT,node_id,new_meter_interval].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_SMINT) == 1){
        strncpy(tmpStr,
                serInBuff + strlen_P(SMSG_RX_PREFIX) + strlen_P(SMSG_SMINT) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_SMINT)));
        static uint32_t newMeterInterval = 0ul;
        sscanf(tmpStr, "%hhu,%lu", &nodeId, &newMeterInterval);
        nodeIx = getNodeIxById(nodeId);
        print_P(SMSG_TX_PREFIX);
        if (nodeIx < UINT8_MAX && newMeterInterval < UINT8_MAX){
            meterNodes[nodeIx].newMeterInterval = newMeterInterval;
            print_P(SMSG_SMINT_ACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Set meter interval on svr inst"), logInfo);
        }
        else{
            print_P(SMSG_SMINT_NACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Bad meter interval svr inst"), logWarn);
        }
        writeLogF(F(". Node="), logInfo);
        writeLog(nodeId, logInfo);
        writeLogF(F(", New value (s)="), logInfo);
        writeLogLn(newMeterInterval, logInfo);
    }

    // Request to set node GINR poll rate temporarily to more aggressive value
    // [SGITR;node_id,tmp_poll_rate,tmp_poll_period].
    else if (strStartsWithP(serInBuff, SMSG_RX_PREFIX, SMSG_SGITR) == 1){
        strncpy(tmpStr,
                serInBuff + strlen_P(SMSG_RX_PREFIX) + strlen_P(SMSG_SGITR) + 1,
                (strlen(serInBuff) - strlen_P(SMSG_SGITR)));
        static uint32_t tmpPollRate = 0ul;
        static uint32_t tmpPollPeriod = 0ul;
        sscanf(tmpStr, "%hhu,%lu,%lu", &nodeId, &tmpPollRate, &tmpPollPeriod);
        nodeIx = getNodeIxById(nodeId);
        print_P(SMSG_TX_PREFIX);
        if (nodeIx < UINT8_MAX && tmpPollRate >= 10 && tmpPollRate <= 600
               && tmpPollPeriod >= 10 && tmpPollPeriod <= 3000){
            meterNodes[nodeIx].tmpGinrPollRate = tmpPollRate;
            meterNodes[nodeIx].tmpGinrPollPeriod = tmpPollPeriod;
            print_P(SMSG_SGITR_ACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Set GINR fast poll svr inst"), logInfo);
        }
        else{
            print_P(SMSG_SGITR_NACK);
            Serial.write(SMSG_RS);
            writeLogLn(nodeId, logNull);
            writeLogF(F("Bad GINR fast poll svr inst"), logWarn);
        }
        writeLogF(F(". Node="), logInfo);
        writeLog(nodeId, logInfo);
        writeLogF(F(", "), logInfo);
        writeLog(tmpPollRate, logInfo);
        writeLogF(F("s for "), logInfo);
        writeLog(tmpPollPeriod, logInfo);
        writeLogLnF(F("s"), logInfo);
    }

    else {
        writeLogF(F("Bad Serial Message: "), logWarn);
        writeLogLn(serInBuff, logWarn);
    }
}


void checkSerialInput() {
    /*
       Checks for serial input and handles messages or interactive serial
       commands
    */
    if (readLineSerial(Serial.read(), serInBuff) > 0) {
        if (strStartsWithP(serInBuff, SMSG_RX_PREFIX) == 1)
            processSerialMessage();
        else
            processSerialCommand();
    }
}


void processMsgRecv(){
    /**
        Processes and dispatches a newly-received message from a meter node.
     */

    wdt_reset();
    lastRSSIAtGateway = radio.lastRssi();

    writeLogF(F("Got msg: "), logDebug);
    writeLog((char*)radioMsgBuff, logDebug);
    writeLogF(F(". RSSI = "), logDebug);
    writeLogLn(lastRSSIAtGateway, logDebug);

    strcpy(msgBuffStr, "");
    sprintf(msgBuffStr, "%s", (char*)radioMsgBuff);

    // ensure the sending node exists in the meternode array, create a new entry
    // if it doesnt
    static uint8_t nodeIx = 0;
    nodeIx = getNodeIxByIdWithCreate(lastMsgFrom);

    if (nodeIx == UINT8_MAX)
        return;     // abort if find/create failed

    // update when node last seen, RSSI from node at server
    meterNodes[nodeIx].lastSeenTime = getNowTimestampSec();
    meterNodes[nodeIx].lastNodeRSSI = lastRSSIAtGateway;

    // record and pass through rebase (MREB)
    // MREB:  meter rebase (to gateway) - a time and value baseline
    // format: MREB,<meter_time_start>,<meter_value_start>;
    // e.g.:   MREB,1496842913428,18829393;
    if (strStartsWithP(msgBuffStr, RMSG_MREBASE) == 1){
        sscanf(msgBuffStr, "MREB,%lu,%lu",
                &meterNodes[nodeIx].lastEntryFinishTime,
                &meterNodes[nodeIx].lastMeterValue);
        sendSerMeterRebase(lastMsgFrom);
    }

    // grab latest entry from meter update (MUPC) and pass through
    // MUPC:  meter update (to gateway) - a digest of timestamped accumulation
    //         meter entries with current reads
    // format: MUPC,1 of [<meter_time_start>, <meter_value_start>];
    //         2..n of
    //             [<interval_duration>, <interval_value>, <spot_rms_current>]
    // e.g.:
    //     MUPC,1496842913428,18829393;15,1,10.2;15,5,10.7;
    // should fit 2-3 entries
    else if (strStartsWithP(msgBuffStr, RMSG_MUPC) == 1){
        static uint32_t meterEntryFinishTime = 0ul;
        static uint32_t meterEntryValue = 0ul;
        static double currentRMS = 0.0;
        static char* token;
        static uint8_t i = 0;

        // get message value fields
        sscanf(msgBuffStr, "MUPC,%s[^\n]", tmpStr);
        // tokenise and add up times and entry values
        token = strtok(tmpStr, ";,");
        i = 0;
        meterEntryValue = 0;
        while (token != NULL){
            i++;
            if (i == 1)
                meterEntryFinishTime = strtoul(token, NULL, 0);
            else if (i == 2)
                meterEntryValue = strtoul(token, NULL, 0);
            else if (i % 3 == 0)        // a current entry
                currentRMS = strtod(token, NULL);       //TODO: replace strtod
            else if (i % 2 == 0)        // a meter entry as even field num
                meterEntryValue += strtoul(token, NULL, 0);
            else
                meterEntryFinishTime += strtoul(token, NULL, 0);
            token = strtok(NULL, ";,");     // get next token from strtok
        }
        meterNodes[nodeIx].lastEntryFinishTime = meterEntryFinishTime;
        meterNodes[nodeIx].lastMeterValue = meterEntryValue;
        meterNodes[nodeIx].lastCurrentRMS = currentRMS;
        sendSerMeterUpdate(lastMsgFrom, true);
    }

    // grab latest entry from meter update (MUP_) and pass through
    // MUP_:  meter update (to gateway) - a digest of timestamped accumulation
    //         meter entries without current reads
    // format: MUP_,1 of [<meter_time_start>, <meter_value_start>];
    //             2..n of [<interval_duration>, <interval_value>]
    // e.g.:   MUP_,1496842913428,18829393;15,1;15,5;15,2;16,3;
    // should fit 4 entries unless using > 999Wh per interval
    else if (strStartsWithP(msgBuffStr, RMSG_MUP_) == 1){
        static uint32_t meterEntryFinishTime = 0ul;
        static uint32_t meterEntryValue = 0ul;
        static char* token;
        static uint8_t i = 0;

        // get message value fields
        sscanf(msgBuffStr, "MUP_,%s[^\n]", tmpStr);
        // tokenise and add up times and entry values
        token = strtok(tmpStr, ";,");
        i = 0;
        meterEntryValue = 0;
        while (token != NULL){
            i++;
            if (i == 1)
                meterEntryFinishTime = strtoul(token, NULL, 0);
            else if (i == 2)
                meterEntryValue = strtoul(token, NULL, 0);
            else if (i % 2 == 0)        // a meter entry as even field num
                meterEntryValue += strtoul(token, NULL, 0);
            else
                meterEntryFinishTime += strtoul(token, NULL, 0);
            token = strtok(NULL, ";,");     // get next token from strtok
        }
        meterNodes[nodeIx].lastEntryFinishTime = meterEntryFinishTime;
        meterNodes[nodeIx].lastMeterValue = meterEntryValue;
        sendSerMeterUpdate(lastMsgFrom, false);
    }

    // process gateway instruction request (GINR) and respond
    // GINR: Instruction request (to gateway).  Also includes status data.
    // format: GINR;<battery_voltage>,<uptime_secs>,<sleeptime_secs>,<free_ram>,
    //                 <last_rssi>,<puck_led_rate>,<puck_led_time>,
    //                 <meter_interval_time>
    // e.g.:   GINR;4300,890000,555000,880,-80,10,100,5
    else if (strStartsWithP(msgBuffStr, RMSG_GINR) == 1){
        // get message value fields
        sscanf(msgBuffStr, "GINR,%d,%lu,%lu,%d,%hhd,%hhu,%d,%hhu,%d",
                &meterNodes[nodeIx].battVoltageMV,
                &meterNodes[nodeIx].secondsUptime,
                &meterNodes[nodeIx].secondsSlept,
                &meterNodes[nodeIx].freeRAM,
                &lastRSSIAtNode,
                &meterNodes[nodeIx].puckLEDRate,
                &meterNodes[nodeIx].puckLEDTime,
                &meterNodes[nodeIx].meterInterval,
                &meterNodes[nodeIx].meterImpPerKwh
            );

        writeLogF(F("Last RSSI at node="), logInfo);
        writeLogLn(lastRSSIAtNode, logInfo);

        // send request to temporarily increase GINR poll rate if queued
        // GITR:
        //  format: GITR;<new_rate>,<duration>,<last_node_rssi>
        //  e.g.: GITR;30,600,-70
        if (meterNodes[nodeIx].tmpGinrPollRate > 0 &&
                    meterNodes[nodeIx].tmpGinrPollPeriod > 0){
            sprintf_P(msgBuffStr, RMSG_GITR);
            sprintf(msgBuffStr, "%s,%d,%d,%hhd", msgBuffStr,
                    meterNodes[nodeIx].tmpGinrPollRate,
                    meterNodes[nodeIx].tmpGinrPollPeriod, lastRSSIAtGateway);
            writeLogF(F("Sent GINR poll rate increase (GITR) to node "),
                    logInfo);
            writeLogLn(lastMsgFrom, logInfo);
            meterNodes[nodeIx].tmpGinrPollRate = 0;
            meterNodes[nodeIx].tmpGinrPollPeriod = 0;
            sendRadioMsg(lastMsgFrom, false);
        }

        // send new meter value if one is queued
        //  MVAI:
        //  meter instruction (from gateway) to set accumulation meter value
        //  format: MVAI;<new_meter_value>,<last_node_rssi>
        //  e.g.: MVAI;120000,-70
        else if (meterNodes[nodeIx].newMeterValue > 0){
            sprintf_P(msgBuffStr, RMSG_MVAI);
            sprintf(msgBuffStr, "%s,%lu,%hhd", msgBuffStr,
                    meterNodes[nodeIx].newMeterValue, lastRSSIAtGateway);
            writeLogF(F("Sent meter val update inst (MVAI) to node "),
                    logInfo);
            writeLogLn(lastMsgFrom, logInfo);
            meterNodes[nodeIx].newMeterValue = 0ul;
            sendRadioMsg(lastMsgFrom, false);
        }

        // send new meter interval if one is queued
        // MINI:
        // meter instruction (from gateway) to set accumulation meter interval
        // format: MINI,<new_interval>,<last_node_rssi>
        // e.g.: MINI;5,-70
        else if (meterNodes[nodeIx].newMeterInterval > 0){
            sprintf_P(msgBuffStr, RMSG_MINI);
            sprintf(msgBuffStr, "%s,%d,%hhd", msgBuffStr,
                    meterNodes[nodeIx].newMeterInterval, lastRSSIAtGateway);
            writeLogF(F("Sent meter int update inst (MINI) to node "),
                    logInfo);
            writeLogLn(lastMsgFrom, logInfo);
            meterNodes[nodeIx].newMeterInterval = 0;
            sendRadioMsg(lastMsgFrom, false);
        }
        // send LED instruction if one is queued
        // MPLI:
        // meter instruction (from gateway) to set LED pulse rate and duration
        // format: MPLI,<led_pulse_rate>,<led_pulse_duration>,<last_node_rssi>
        // e.g.: MPLI;1,500,-70
        else if (meterNodes[nodeIx].newPuckLEDTime < UINT16_MAX &&
                    meterNodes[nodeIx].newPuckLEDRate < UINT8_MAX){
            sprintf_P(msgBuffStr, RMSG_MPLI);
            sprintf(msgBuffStr, "%s,%d,%d,%hhd", msgBuffStr,
                    meterNodes[nodeIx].newPuckLEDRate,
                    meterNodes[nodeIx].newPuckLEDTime, lastRSSIAtGateway);
            writeLogF(F("Sent meter update inst (MPLI) to node "),
                    logInfo);
            writeLogLn(lastMsgFrom, logInfo);
            meterNodes[nodeIx].newPuckLEDTime = UINT16_MAX;
            meterNodes[nodeIx].newPuckLEDRate = UINT8_MAX;
            sendRadioMsg(lastMsgFrom, false);
        }

        // send a MNOI if nothing to do
        // MNOI:
        // 'no op' ACK to GINR, provides RSSI for auto-tuning
        // format: MNOI,<last_node_rssi>
        // e.g.: MNOI;-70

        else {
            sprintf_P(msgBuffStr, RMSG_MNOI);
            sprintf(msgBuffStr, "%s,%hhd", msgBuffStr, lastRSSIAtGateway);
            writeLogF(F("Sent no-op (MNOI) to node "), logInfo);
            writeLogLn(lastMsgFrom, logInfo);
            sendRadioMsg(lastMsgFrom, false);
        }

    }

    // process time sync request (PREQ)
    // PREQ: ping request to gateway
    //    format: PREQ;<current_time_local>
    //    e.g.:   PREQ;1496842913428

    // Return PRSP:
    // ping response from gateway, used to sync clock
    // format: PRSP,<request_time_node>,<current_time_gateway>,<align_sec>
    // <last_node_rssi>
    // e.g.:   PRSP;14968429155328,1496842915428,1,-70
    else if (strStartsWithP(msgBuffStr, RMSG_PREQ) == 1){
        static uint32_t nodeTime = 0ul;
        static uint32_t gatewayTime = 0ul;

        gatewayTime = getNowTimestampSec();
        sscanf(msgBuffStr, "PREQ,%lu", &nodeTime);
        sprintf_P(msgBuffStr, RMSG_PRSP);
        sprintf(msgBuffStr, "%s,%lu,%lu,%hhu,%hhd", msgBuffStr, nodeTime,
                gatewayTime, cfgAlignEntries,
                lastRSSIAtGateway);  // 1=align to mm:00
        sendRadioMsg(lastMsgFrom, false);

        meterNodes[nodeIx].lastClockDriftSecs = gatewayTime - nodeTime;
    }

    // process gen purpose msg (GMSG)
    else if (strStartsWithP(msgBuffStr, RMSG_GMSG) == 1){
        writeLogF(F("Got bcast from node "), logInfo);
        writeLog(lastMsgFrom, logInfo);
        writeLogF(F(": "), logInfo);
        writeLogLn(msgBuffStr, logInfo);
        sendSerNodeGenMsg(lastMsgFrom);
    }

    else {
        writeLogF(F("Unknown msg from node "), logWarn);
        writeLog(lastMsgFrom, logWarn);
        writeLogF(F(": "), logWarn);
        writeLogLn(msgBuffStr, logWarn);
    }

    strcpy(msgBuffStr, "");
}


void checkRadioMsg(){
    /**
       Checks for newly received messages and calls processMsgRecv() - which is
       also called from within sendRadioMsg - hence separate function.
     */

    if (msgManager.available()){
        uint8_t lenBuff = sizeof(radioMsgBuff);
        memset(radioMsgBuff, 0, sizeof(radioMsgBuff));
        wdt_reset();
        // Check for newly arrived message and ACK.  No wait/timeout as not
        // assured of having one.
        if (msgManager.recvfromAck(radioMsgBuff, &lenBuff, &lastMsgFrom))
            processMsgRecv();
    }
    wdt_reset();
}


void sendRadioMsg(uint8_t recipient, bool checkReply){
    /*
        Sends whatever's in msgBuffStr to radio recipient
    */

    if (strlen(msgBuffStr) > RH_RF69_MAX_MESSAGE_LEN){
       writeLogF(F("Msg too long: "), logError);
       writeLogLn(msgBuffStr, logError);
       return;
    }

    uint8_t lenBuff = sizeof(radioMsgBuff);
    memset(radioMsgBuff, 0, sizeof(radioMsgBuff));

    writeLogF(F("Sending: "), logDebug);
    writeLogLn(msgBuffStr, logDebug);
    memcpy(radioMsgBuff, msgBuffStr, strlen(msgBuffStr));
    wdt_reset();

    // Send message with an ack timeout as specified by TX_TIMEOUT
    if (msgManager.sendtoWait(radioMsgBuff, lenBuff, recipient)){
        // Wait for a reply from the Gateway if instructed to
        if (checkReply && msgManager.recvfromAckTimeout(radioMsgBuff, &lenBuff,
                    RX_TIMEOUT, &lastMsgFrom))
            processMsgRecv();
        else if (checkReply)
            writeLogLnF(F("No ACK recv"), logInfo);
    }
    else {
        writeLogF(F("Send fail: "), logWarn);
        writeLogLn(msgBuffStr, logWarn);
    }
    wdt_reset();
}


void checkNodeLife(){
    /*
        Checks whether nodes have 'gone dark', and alerts
    */

    for (uint8_t i = 0; i < MAX_MTR_NODES; i++)
        if (meterNodes[i].nodeId > 0 && meterNodes[i].lastSeenTime < UINT32_MAX
                && (getNowTimestampSec() - meterNodes[i].lastSeenTime >
                        POL_MSG_TIMEOUT_SEC)){
            wdt_reset();
            print_P(SMSG_TX_PREFIX);
            print_P(SMSG_NDARK);
            Serial.write(SMSG_RS);
            writeLog(meterNodes[i].nodeId, logNull);
            Serial.write(SMSG_FS);
            writeLogLn(meterNodes[i].lastSeenTime, logNull);
            //set to max to avoid double reporting; interpret as 'node dark'
            meterNodes[i].lastSeenTime = UINT32_MAX;
        }
}

void blinkLED(uint8_t blinkTimes){
    for (uint8_t i = 1; i <= blinkTimes; i++){
        PORTD = PORTD | B00010000;  // on
        delay(500);
        PORTD = PORTD & B11101111;  // off
        delay(250);
        wdt_reset();
    }
}


void checkButton(){
    /*
        Place holder for button
    */
    static bool btnDown = false;
    // button is pin 6 with external pull-up (on is LOW)
    btnDown = ((PIND & B01000000) == B00000000);

    if (btnDown && btnEventStartMillis == 0)        // new event
        btnEventStartMillis = millis();

    else if (!btnDown && btnEventStartMillis > 0 &&
                (millis() - btnEventStartMillis <= 1000)){
        // button has been pressed and released in <= 1s
        // LOGIC...
        blinkLED(1);
        btnEventStartMillis = 0ul;
    }

    else if (!btnDown && btnEventStartMillis > 0 &&
                (millis() - btnEventStartMillis > 3000))
        // button has been pressed and released in >3s => ignore
        btnEventStartMillis = 0ul;

}

// save reset cause from bootloader
void resetFlagsInit(void) __attribute__ ((naked))
                          __attribute__ ((used))
                          __attribute__ ((section (".init0")));


void resetFlagsInit(void)
{
// save reset flags passed from bootloader
  __asm__ __volatile__ ("sts %0, r2\n" : "=m" (resetFlags) :);
}


void printResetVal(uint8_t resetVal){
    writeLogF(F("R_FLG 0x"), logNull);
    Serial.print(resetVal, HEX);

    // check for the reset bits.  Symbols are
    // bit flags, have to be shifted before comparison.

    if (resetVal & (1<<WDRF))
    {
        // watchdog
        writeLogF(F(" WD"), logNull);
        resetVal &= ~(1<<WDRF);
    }
    if (resetVal & (1<<BORF))
    {
        // brownout
        writeLogF(F(" BO"), logNull);
        resetVal &= ~(1<<BORF);
    }
    if (resetVal & (1<<EXTRF))
    {
        // external
        writeLogF(F(" EX"), logNull);
        resetVal &= ~(1<<EXTRF);
    }
    if (resetVal & (1<<PORF))
    {
        // power on
        writeLogF(F(" PO"), logNull);
        resetVal &= ~(1<<PORF);
    }
    if (resetVal != 0x00)
    // unknown - should not happen
        writeLogF(F(" UN"), logNull);

    printNewLine(logNull);
}


void setup() {
    /* initialise pins */
    pinMode(BUTTON_PIN, INPUT);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // disconnected pins 'pulled up' rather than left floating
    pinMode(3, INPUT_PULLUP);
    pinMode(5, INPUT_PULLUP);
    pinMode(7, INPUT_PULLUP);
    pinMode(8, INPUT_PULLUP);
    pinMode(9, INPUT_PULLUP);
    pinMode(A0, INPUT_PULLUP);
    pinMode(A1, INPUT_PULLUP);
    pinMode(A2, INPUT_PULLUP);
    pinMode(A3, INPUT_PULLUP);
    pinMode(A4, INPUT_PULLUP);
    pinMode(A5, INPUT_PULLUP);

    /* wait until connection is up */
    while (!Serial)
        delay(1);
    Serial.begin(SERIAL_BAUD);

    printNewLine(logNull);
    printNewLine(logNull);
    writeLogLnF(F("=BOOT="), logNull);
    printResetVal(resetFlags);

    /* get config from EEPROM */
    getConfigFromMem();
    applyRadioConfig();

    /* send boot message to server */
    sprintf_P(msgBuffStr, RMSG_GMSG);
    sprintf(msgBuffStr, "%s,BOOT v%hhu. Flags: %hhu", msgBuffStr, FW_VERSION,
            resetFlags);
    sendSerNodeGenMsg(cfgGatewayId);

    /* initialise Clock */
    writeLogLnF(F("RTC Init"), logDebug);
    setNowTimestampSec(INIT_TIME);  // in case get time fails
    sendSerGetTime();

    blinkLED(3);

    wdt_enable(WDTO_8S);    //Time for wait before autoreset
}


void loop() {
    /*
    Do processing, comms on a prioritised basis, ensuring at least 5 iterations
    before sleeping.
    */

    static uint8_t doEvery = 0;

    for (doEvery = 1; doEvery <=5; doEvery++){
        // do every time
        checkSerialInput();
        wdt_reset();
        checkButton();

        // do processing if not in middle of serial input
        if (serialBuffPos == 0 && doEvery % 2 == 0)
            checkRadioMsg();

        if (serialBuffPos == 0 && doEvery == 5)
            checkNodeLife();
    }
}
