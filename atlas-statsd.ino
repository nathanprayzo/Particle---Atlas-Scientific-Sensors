#include "math.h"

//Uncomment the line below to enable serial debugging
//#define DEBUG_ENABLED

// Provides easy enable/disable debugging mechanism
#ifdef DEBUG_ENABLED
	#define DEBUG_PRINT(str)    \
	Serial.print(millis());     \
	Serial.print(": ");    \
	Serial.print(__PRETTY_FUNCTION__); \
	Serial.print(' ');      \
	Serial.print(__LINE__);     \
	Serial.print(' ');      \
	Serial.println(str);
#else
	#define DEBUG_PRINT(str)
#endif

// enable SRAM feature for preserving variables between deep sleep modes
STARTUP(System.enableFeature(FEATURE_RETAINED_MEMORY));

// An UDP instance to let us send and receive packets over UDP
UDP udp_client;

// UDP Port used for two way communication
unsigned int udp_local_port = 8888;

// Destination IP for UDP Packets
IPAddress remoteIP(159, 203, 144, 95);

// don't publish trivial changes to conserve data
float delta_to_publish = 0.05;

// atlas scientific chip types
typedef enum {
	ph,
	ec,
	orp,
	dissolved_oxygen
} chip_type;

// chip commmunication mode {i2c, serial}
typedef enum {
	i2c,
	serial
} chip_mode;

// struct for abstracing atlas chips 
struct atlas_chip {
	chip_type type;
	chip_mode mode;
	byte address;
	String data;
};

// initialize struct for ph chip
struct atlas_chip ph_chip = { .type = ph, .mode = i2c, .address = 0x63 };

// retain last reading in SRAM to preserve between deep sleeps
// NOTE: Storing as float due to firmware bug preventing use of Strings from being preserved
retained float last_reading;
// cache Device ID in SRAM to preserve between deep sleeps
retained String device_id;

// chip_command sends a command to an atlas chip and responds with the chip response_
String chip_command(String chip_command, byte chip_address, int read_delay, int response_size) 
{
	int response_code;			// store response code separate from response data
	String response_data;			// store data as string
	char reading[response_size];		// create array to store sensor data

	Wire.beginTransmission(chip_address); 	// signal beginning of transmit
	Wire.write(chip_command);      	 	// transmit command
	Wire.endTransmission();    		// signal end of transmission
	delay(read_delay);			// wait for chip to process command

	// if response size is zero, no response is expected so return with no data
	if (response_size == 0) {
		return response_data;
	}

	Wire.requestFrom(chip_address, response_size); // request data from sensor
	response_code = Wire.read();	// get first byte which contains response code from chip
	switch (response_code) {
		case 1: 			// if first byte is 1, command was successful
			for (int byte_num = 0; byte_num < response_size - 1; byte_num = byte_num + 1) {
				// and pull data off the bus one byte at a time
				reading[byte_num] = Wire.read();
			}
			response_data = String(reading);
			break;
		case 2:			// if first byte is 2, the request failed
			response_data = "Request Failed";
			break;
		case 254:		// if first byte is 254, the request is still being processed
			response_data = "Pending - the request is still being processed. Ensure that you have waited the minimum time to guarantee a repsonse";
			break;
		case 255:		// if first byte is 255, there is no pending request/data available.
			response_data = "No Data - there is no pending request, so there is no data to return from the circuit";
			break;
	}
	return response_data;
}

// get_reading takes a point to an atlas_chip and populates its data field
void get_reading(struct atlas_chip *chip) {
	switch (chip->type) {
		case ph:
			// ph chip will response with max 7 bytes after 1 second
			chip->data=chip_command("R", chip->address, 1000, 7);
			break;
		case ec:
			// ec chip will response with max 32 bytes after 1 second
			chip->data=chip_command("R", chip->address, 1000, 32);
			break;
		case orp:
			// orp chip will response with max 8 bytes after 1 second
			chip->data=chip_command("R", chip->address, 1000,8);
			break;
		case dissolved_oxygen:
			// dissolved oxygen chip will response with max 14 bytes after 1 second
			chip->data=chip_command("R", chip->address, 1000,14);
			break;
	}
}

// publishes data via udp to statsd->graphite engine
void publish_data(String data)
{
  	udp_client.beginPacket(remoteIP, udp_local_port);
	udp_client.write("aqpncs." + device_id + ":" + data + "|g");
	udp_client.endPacket();

}

// put chip into low power sstate
void chip_sleep(struct atlas_chip chip)
{
	String sleep_command = "SLEEP";
	int processing_delay = 300;
	int expected_response = 0;
	chip_command(sleep_command, chip.address, processing_delay, expected_response); 
}

// wake chip up from sleep
void chip_wake(struct atlas_chip chip)
{
	DEBUG_PRINT("Waking up " + String(chip.type) + " chip");
	// After chip is woken up, 4 consecutive readings should be taken before
	//    the readings are considered valid.
	int required_readings = 4;

	// 16 consecutive readings are required for dissolved oxygen chip
	if (chip.type == dissolved_oxygen) {
		required_readings = 16;
	}

	DEBUG_PRINT("Dumping " + String(required_readings) + " readings");
	for (int reading = 0; reading < required_readings; reading++)
	{
		chip_command("R", chip.address, 1000, 0);
	}
}

// Setup
void setup()
{
	DEBUG_PRINT("Start of setup()");
	if (device_id == "") {
		device_id = Particle.deviceID();  // get device id from particle cloud
		DEBUG_PRINT("Did not find device id in sram so got it from cloud: " + device_id);
	}else {
		DEBUG_PRINT("found device id in sram: " + device_id);
	}
  	udp_client.begin(udp_local_port); // start udp client on specified port (NEED2: extract port to cloud data point)
	Wire.begin(); 			// Initiate the Wire library and join the I2C bus as a master or slave 

	#ifdef DEBUG_ENABLED		// if debugging is enabled start serial and turn on atlas chip debug led
		Serial.begin(9600);			// This channel communicates through the USB port and when connected to a computer, will show up as a virtual COM port.
		Wire.beginTransmission(ph_chip.address);   // signal beginning of transmit
		Wire.write("L,1");               		// turn on atlas debug led
		Wire.endTransmission();                 	// signal end of transmission
	#else
		Wire.beginTransmission(ph_chip.address);   // signal beginning of transmit
		Wire.write("L,0");               		// turn off atlas debug led
		Wire.endTransmission();                 	// signal end of transmission
	#endif
}

// Main loop
void loop()
{
	DEBUG_PRINT("Start of loop()");
	DEBUG_PRINT("Waking up atlas chip from low power state");
	chip_wake(ph_chip);
	DEBUG_PRINT("Last reading: " + String(last_reading));
	get_reading(&ph_chip);
	DEBUG_PRINT("This reading: " + ph_chip.data);

	// only update last reading and publish data if change is > 
	if ( fabs ( atof(ph_chip.data) - last_reading ) > delta_to_publish ) {
		last_reading = atof(ph_chip.data);
		DEBUG_PRINT("New sensor reading differs from last reading more than configured delta");
		publish_data(ph_chip.data);
	}

	DEBUG_PRINT("Putting atlas chip into low power mode");
	chip_sleep(ph_chip);
	DEBUG_PRINT("Putting electron into deep sleep");
	System.sleep(SLEEP_MODE_DEEP,900, SLEEP_NETWORK_STANDBY);
}

