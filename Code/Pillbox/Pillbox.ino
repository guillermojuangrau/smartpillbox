/***********************/
/*Constant definitions*/
/*********************/
#define dispButton 2
#define switchBT 3
#define dispLed 4
#define speaker 9


/*******************/
/*Libraries needed*/
/*****************/
#include <EEPROM.h>
#include <Servo.h>
#include <Wire.h>
#include "RTClib.h"


Servo upperServo; //Upper servo declaration
Servo lowerServo; //Lower servo declaration
RTC_DS1307 RTC; //Real-Time Clock module declaration
long debouncing_time = 15; //Debouncing Time in Milliseconds
volatile unsigned long last_micros; //Variable needed for the button debounce
int dispensingFlag = 0; //Flag to signal if a pill is requested by the user
DateTime now;
int day = 0;
int hour = 0;
int minute = 0;

//Array where the next dose to be dispensed will be stored
byte actualDose[4] = {255, 255, 255, 255};

int command = 0;

//Array where the dose timetable will be loaded during runtime
byte doses[120][4];

//Array where the logs will be kept
byte logs[120][4];
unsigned long timesinceon;


/**********************************/
/*Different upper servo positions*/
/********************************/
int pillAdisppos = 0; //Pill A dispensing position
int pillBdisppos = 0;//Pill B dispensing position
int pillCdisppos = 0;//Pill C dispensing position
int defaultpillstoragepos = 0;//Default pill storage position
int pillAloadpos = 0;//Pill A loading position
int pillBloadpos = 0;//Pill B loading position
int pillCloadpos = 0;//Pill C loading position


/**********************************/
/*Different lower servo positions*/
/********************************/
int loadpos = 0;//Pill chamber loading position
int releasepos = 0;//Pill chamber releasing position
int finishedrefilling = 120;

void setup() {
  pinMode(dispButton, INPUT);
  pinMode(switchBT, INPUT);
  pinMode(dispLed, OUTPUT);
  pinMode(speaker, OUTPUT);
  upperServo.attach(5);//Attach upper servo to pin 5
  lowerServo.attach(6);//Attach lower servo to pin 6
  digitalWrite(dispLed, LOW);
  attachInterrupt(2, debounceInterrupt, RISING);//Attach an interrupt to pin 2
  Serial.begin(9600);
  Wire.begin();
  RTC.begin();
  loadTimetable(doses);//Load the doses timetable from the EEPROM memory into the doses array
  now = RTC.now();//The actual date is gathered from the RTC module
  day = now.dayOfTheWeek();

  int i = 0;
 while (doses[i][0] != day) {
    doses[i][0] = 255;
    doses[i][1] = 255;
    doses[i][2] = 255;
    doses[i][3] = 255;

  }

}
void loop() {
  if (digitalRead(switchBT) == HIGH) {//Check if the Bluetooth switch is active
    if (Serial.available() > 0) {//Check if there is Serial data available
      command = Serial.read(); //Read the command sent by the device to the pillbox
      switch (command) {
        case 1: //Case 1 corresponds to sending the timetable to the device
          Serial.write("Sending the timetable now.");
          byte complete_timetable[256][4];//An array where the complete timetable will be loaded
          loadTimetable(complete_timetable);//Load the timetable from memory into the array
          for (int i = 0; i < 256; i++) {
            for (int j = 0; j < 4; j++) {
              Serial.write(complete_timetable[i][j]);//Writes the array in order to the device
            }
          }
          sendClosingSequence();
          break;
        case 2: //Case 2 corresponds to the device sending a new timetable for the pillbox
          Serial.write("Updating timetable procedure started.");
          byte new_timetable[256][4];
          for (int i = 0; i < 256; i++) {
            for (int j = 0; j < 4; j++) {
              new_timetable[i][j] = Serial.read(); //The new timetable is read byte by byte
            }
          }
          overwrite_timetable(new_timetable);//Overwrite the EEPROM memory with the new timetable
          loadTimetable(doses);//Load the new timetable from the recently overwritten EEPROM memory into the doses array
          sendConfirmationSequence();//Send a sequence so that the device knows the transference has been correctly done
          sendClosingSequence();
          break;
        case 3: //Case 3 sends how much time the device has been powered on for battery saving causes
          timesinceon = millis();
          Serial.write(timesinceon);
          sendClosingSequence();
          break;
        case 4: //Case 4 sends the logs of the times at which the pills were actually taken
          for (int i = 0; i < 256; i++) {
            for (int j = 0; j < 4; j++) {
              Serial.write(logs[i][j]);
            }
          }
          sendClosingSequence();

        case 5: //Case 5 activates the refilling sequence
          Serial.write("Refilling sequence activated.");
          int movement;
          //The device sends a number, this number will either be ond of the pill refilling position or a number interpreted as a command to stop the refilling sequence
          movement = Serial.read();      
          //While the number sent is not the one which stops the refilling sequence the pill storage will rotate to the sent positiom
          while (movement != finishedrefilling) {
            upperServo.write(movement);
            delay(1000);
            movement = Serial.read();
          }
          upperServo.write(defaultpillstoragepos);//After finishing the refilling sequence the pill storage will be rotated to its default positions
          delay(1000);
          sendClosingSequence();
          break;
        default:
          sendClosingSequence();
          break;
      }
    } else {
    }
  } else {
    now = RTC.now();//The actual date is gathered from the RTC module
    day = now.dayOfTheWeek();
    hour = now.hour();
    minute = now.minute();
    loadNextDose();//The next dose in line will be loaded into the actualDose variable.
    if (day == actualDose[0] && hour >= actualDose[1] && minute >= actualDose[2]) { //Checks the time of the next dose with the actual time
      if (minute - actualDose[3] < 5 ) {//If the next pill was to be taken less than five minutes ago visual and acoustic signals are sent
        digitalWrite(dispLed, HIGH);
        alarm();
      } else {
        digitalWrite(dispLed, HIGH);//Only visual notifications are activated if the pill had to be taken more than five minutes ago
      }
    }
    if (dispensingFlag == 1) {//If the flag to dispense a pill was pressed
      int pill = actualDose[3];
      dispensePill(actualDose[3]);//The dispensing procedure is activated for the pill type which has to be taken
      for (int i = 0; i < 256; i++) {//A for loop is created to log the pill dose time
        if (logs[i][0] == 255) {
          logs[i][0] = day;
          logs[i][1] = hour;
          logs[i][2] = minute;
          logs[i][3] = pill;
        }
      }
    }
  }
}

/***********************************************************/
/*loadTimetable: */
/*This function recieves an array of bytes as an argument*/
/*and loads the dose timetable from the EEPROM memory */
/*into the array */
/******************************************************/
void loadTimetable(byte array_to_fill[][4]) {
  int address = 0;
  for (int i = 0; i < 256; i++) {
    for (int j = 0; j < 4; j++) {
      array_to_fill[i][j] = EEPROM.read(address);
      address++;
    }
  }
}
/***************************************************************/
/*debounceInterrupt: */
/*The method which prevents the dispensing button press from */
/*debouncing and then activates the flag to dispense a pill.*/
/***********************************************************/
void debounceInterrupt() {
  if ((long)(micros() - last_micros) >= debouncing_time * 1000) {
    dispensingFlag = 1;
    last_micros = micros();
  }
}
/****************************************************************/
/*loadNextDose: */
/*This function retrieves the next dose to be taken from */
/*the doses array and stores it into the actualDose variable.*/
/************************************************************/
void loadNextDose() {
  int dosetocompare;
  for (int i = 0; i < 256; i++) {
    dosetocompare = (int)doses[i];
    if (dosetocompare != 255) {
      for (int j = 0; j < 4; j++) {
        actualDose[j] = doses[i][j];
        doses[i][j] = 255;
      }
      break; //para salirnos del for loop
    }
  }
}
/*****************************************************************************/
/*dispensePill: */
/*This function starts the pill dispensing procedure to */
/*dispense the pill type which is notified with the function's argument */
/*and rotates the servos in turn to dispense the pill */
/************************************************************************/
void dispensePill(int typeOfPill) {
  if (typeOfPill != 255) { //Esto comprueba que no haya fallos o que la pildora cargada sea correcta
    upperServo.write(typeOfPill);
    delay(200);
    lowerServo.write(loadpos); //posicion de carga
    delay(200);
    upperServo.write(defaultpillstoragepos);
    delay(200);
    lowerServo.write(releasepos);
    delay(200);
    digitalWrite(dispLed, LOW);
  }
}
/***********************************************************************/
/*sendConfirmationSequence: */
/*This function sends to the other device a */
/*preestablished confirmation sequence to indicate the new timetable*/
/*has been correctly received */
/******************************************************************/
void sendConfirmationSequence() {
  byte confirmationsequence[5] = {200, 200, 200};
  int i = 0;
  for (int i = 0; i < 3; i++)
    Serial.write(confirmationsequence[i]);
}
/************************************************************/
/*sendClosingSequence: */
/*This function sends to the other device a */
/*preestablished closing sequence to indicate the correct*/
/*termination of the bluetooth command */
/*******************************************************/
void sendClosingSequence() {
  byte closingsequence[5] = {255, 254, 255};
  int i = 0;
  for (int i = 0; i < 3; i++)
    Serial.write(closingsequence[i]);
}
/***********************************************************/
/*overwrite_timetable: */
/*This function recieves an array of bytes as an argument*/
/*and overwrites the timetable stored in the EEPROM */
/*memory with the one in the array */
/******************************************************/
void overwrite_timetable(byte new_timetable[][4]) {
  int address = 0;
  for (int i = 0; i < 256; i++) {
    for (int j = 0; j < 4; j++) {
      EEPROM.put(new_timetable[i][j], address);
      address++;
    }
  }
}
/******************************************************/
/*alarm: */
/*This function emits an alarm through the speaker */
/*to signal the acoustic notification of a new dose*/
/**************************************************/
void alarm() {
  tone(speaker, 440, 2000);
}
