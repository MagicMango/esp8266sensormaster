#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <Wire.h>
#include <FS.h>
#include <ArduinoJson.h>
#include "Sensor.h"
#include "SensorData.h"
#include "Configuration.h"
#include <EasyNTPClient.h>
#include <WiFiUdp.h>
#include "LinkedList.h"
extern "C" {
#include <user_interface.h>
#include <ctime>
}

bool parseSensorJSON();
String macToString(const unsigned char* mac);
void handleRoot();
void handleWebRequests();
bool loadFromSpiffs(String path);
String getTime();
String getTimeStamp(unsigned long unixTime, const char* format);
SensorInfo parseSensorInfo(String JSONMessage);
SensorData parseSensorData(String JSONMessage, String v);
void saveSensorSettings();
void handleFileUpload();
void connectedClients();
void disconnectedClients();

static Configuration conf;
//10 Seconds
static int updatePeriod = 10 * 1000;
static unsigned long time_now1 = 0;
static Sensor sensors[8];
const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "admin";
const char* dataFolder = "/sensordata/";
static StaticJsonBuffer<JSON_ARRAY_SIZE(8) + JSON_OBJECT_SIZE(8 * 4)> jsonBuffer;
static StaticJsonBuffer<JSON_ARRAY_SIZE(8) + JSON_OBJECT_SIZE(8 * 4)> sensorBuffer;
static unsigned long Timestamp;
static bool updateSensors;
static LinkedList<String> lastConnectedMACs = LinkedList<String>();
static LinkedList<String> lastDisconnectedMACs = LinkedList<String>();
static File fsUploadFile;

WiFiEventHandler stationConnectedHandler;
WiFiEventHandler stationDisconnectedHandler;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

void setup() {
	Serial.begin(115200);
	//Initialize File System
	SPIFFS.begin();
	Serial.println();
	if (readConfig()) {
		if (parseSensorJSON()) {
			Serial.println(F("devices.json loaded."));
		}
		initWifi();
		initWebserver();
	}
	else {
		Serial.println(F("Config not readable"));
	}
}
void loop() {
	server.handleClient();
	if (millis() > time_now1 + updatePeriod) {
		time_now1 = millis();
		disconnectedClients();
		connectedClients();
	}
	if (updateSensors) {
		saveSensorSettings();
	}
	if (ESP.getFreeHeap() <= 10000) {
		ESP.restart();
	}
}
void initWifi() {
	char ssid[conf.ssid.length() + 1];
	char password[conf.password.length() + 1];
	conf.ssid.toCharArray(ssid, conf.ssid.length() + 1);
	conf.password.toCharArray(password, conf.password.length() + 1);
	WiFi.begin(ssid, password);     //Connect to your WiFi router
	WiFi.mode(WIFI_AP_STA);
	Serial.println("");
	// Wait for connection
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(F("."));
	}
	//If connection successful show IP address in serial monitor
	Serial.println("");
	Serial.print(F("Connected to "));
	Serial.println(ssid);
	Serial.print(F("IP address: "));
	Serial.println(WiFi.localIP());  //IP address assigned to your ESP
	WiFiUDP udp;
	EasyNTPClient ntpClient(udp, "de.pool.ntp.org", ((1 * 60 * 60))); // IST = GMT + 2 Summertime GMT + 1 Wintertime
	unsigned long tmptime = 0;
	while (tmptime == 0 || tmptime >= 4294960000) {
		tmptime = ntpClient.getUnixTime() - (millis() / 1000);
		Timestamp = tmptime;
		Serial.println("Unixtime: " + String(tmptime));
		delay(500);
	}
	Serial.println("Controller started at: " + getTime());
	if (WiFi.softAP("ESPMasterController", "aA03147062", 6, false, 8)) {
		Serial.println(F("AP Started."));
		Serial.print(F("APIP: "));
		Serial.print(WiFi.softAPIP());
		Serial.println();
		stationConnectedHandler = WiFi.onSoftAPModeStationConnected(&onStationConnected);
		stationDisconnectedHandler = WiFi.onSoftAPModeStationDisconnected(&onStationDisonnected);

	}
	else {
		Serial.println(F("AP not established."));
	}
}
void onStationConnected(const WiFiEventSoftAPModeStationConnected& evt) {
	Serial.print(F("Station connected. With MAC: "));
	Serial.print(macToString(evt.mac));
	Serial.println(F(""));
	lastConnectedMACs.add(macToString(evt.mac));
}
void onStationDisonnected(const WiFiEventSoftAPModeStationDisconnected& evt) {
	Serial.print(F("Station disconnected. With MAC: "));
	Serial.print(macToString(evt.mac));
	Serial.println(F(""));
	lastDisconnectedMACs.add(macToString(evt.mac));
}
void connectedClients() {
	struct station_info *stat_info;
	struct ip_addr *IPaddress;
	String lastMAC = lastConnectedMACs.pop();
	IPAddress address;
	SensorInfo info;

	stat_info = wifi_softap_get_station_info();
	while (stat_info != NULL) {
		IPaddress = &stat_info->ip;
		address = IPaddress->addr;
		bool found = false;
		if (macToString(stat_info->bssid) == lastMAC && macToString(stat_info->bssid) != "") {
			Serial.println(F("Found MAC, search for empty sensor space or known sensor."));
			for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
				if (!sensors[i].isInitialised()) {
					Serial.print(F("Will create new Sensor with IP: "));
					Serial.print(address.toString());
					Serial.println(F(""));
					info = getSensorInfo(address.toString());
					sensors[i] = { address.toString(), lastMAC, info.Function, info.Version };
					found = true;
					updateSensors = true;
					break;
				}
				else if (sensors[i].SensorMAC == lastMAC) {
					Serial.println(F("Found known Sensor updating values."));
					info = getSensorInfo(address.toString());
					sensors[i] = { address.toString(), lastMAC, info.Function, info.Version };
					found = true;
					updateSensors = true;
					break;
				}
			}
			if (!found)Serial.println(F("Maximum Sensors rechaed."));
		}
		stat_info = STAILQ_NEXT(stat_info, next);
		delay(0);
	}
	wifi_softap_free_station_info();
	periodicUpdate();
}
void disconnectedClients() {
	while (lastDisconnectedMACs.size() > 0) {
		String lastMAC = lastDisconnectedMACs.pop();
		if (lastMAC != "") {
			for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
				if (sensors[i].SensorMAC == lastMAC) {
					Serial.println("Sensor with MAC: " + lastMAC + " disconnected. Clearing IP");
					sensors[i].SensorIP = "";
					updateSensors = true;
				}
			}
		}
	}
}
bool readConfig() {
	size_t size = 0;
	File f = SPIFFS.open("/config.json", "r");
	if (!f) {
		Serial.println(F("Could not open config.json"));
		return false;
	}
	else
	{
		size = f.size();
		std::unique_ptr<char[]> buf(new char[size]);
		f.readBytes(buf.get(), size);
		JsonObject& parsed = jsonBuffer.parseObject(buf.get());
		if (!parsed.success()) {
			return false;
		}
		jsonBuffer.clear();
		conf = { parsed["ssid"], parsed["password"] };
		return true;
	}
}
void initWebserver() {
	//Initialize Webserver
	server.on("/", handleRoot);
	server.on("/getStatistics", getStatistics);
	server.onNotFound(handleWebRequests); //Set setver all paths are not found so we can handle as per URI
	server.on("/upload", HTTP_GET, []() {                 // if the client requests the upload page
		server.send(200, "text/html", "<form method=\"post\" enctype=\"multipart/form-data\"><input type=\"file\" name=\"name\"><input class=\"button\" type=\"submit\" value=\"Upload\"></form>");
	});

	server.on("/upload", HTTP_POST,                       // if the client posts to the upload page
		[]() { server.send(200); },                          // Send status 200 (OK) to tell the client we are ready to receive
		handleFileUpload                                    // Receive and save the file
	);
	httpUpdater.setup(&server, update_path, update_username, update_password);
	server.begin();
}
void periodicUpdate() {
	SensorData result;
	File f;
	for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
		if (sensors[i].SensorIP != "") {
			Serial.println("http://" + sensors[i].SensorIP + "/" + sensors[i].SensorFunction);
			result = getSensorData(sensors[i].SensorIP, sensors[i].SensorFunction, sensors[i].FunctionVersion);
			f = SPIFFS.open(dataFolder + sensors[i].SensorMAC, "a");
			if (!f) {
				Serial.println(F("Could not write sensor data."));
			}
			f.println(getTime() + " " + String(result.Temperature) + " " + String(result.Humidity) + " " + String(result.Pressure));
			Serial.println(getTime() + " " + String(result.Temperature) + " " + String(result.Humidity) + " " + String(result.Pressure));
			f.close();
			sensors[i].SensorIP = "";
			delay(0);
		}
	}
	Serial.println("Free Heap: "+String(ESP.getFreeHeap()));
}
String macToString(const unsigned char* mac) {
	char buf[20];
	snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return String(buf);
}
String getTime() {
	return getTimeStamp(Timestamp + (millis() / 1000), "%Y-%m-%d %H:%M:%S");
}
String getTimeStamp(unsigned long unixTime, const char* format)
{
	time_t epochTime = (time_t)(unixTime);
	char timestamp[64] = { 0 };
	strftime(timestamp, sizeof(timestamp), format, localtime(&epochTime));
	return timestamp;
}
/**
Json related Functions start
*/
SensorData getSensorData(String ip, String function, String versionf) {
	HTTPClient http;
	http.begin("http://" + ip + "/" + function);
	int httpCode = http.GET();
	SensorData result{ -1, -1, -2 };
	if (httpCode > 0) {
		String payload = http.getString();   //Get the request response payload
		result = parseSensorData(payload, versionf);
		delay(0);
	}
	http.end();
	return result;
}
SensorData parseSensorData(String JSONMessage, String v) {
	JsonObject& parsed = jsonBuffer.parseObject(JSONMessage); //Parse message
	if (!parsed.success()) {   //Check for errors in parsing
		return SensorData{ -1, -2, -2 };
	}
	jsonBuffer.clear();
	if (v == "v1") {
		return SensorData{ parsed["Intern"]["temperature"], parsed["Intern"]["humidity"], -1 };
	}
	else if (v == "v2") {
		return SensorData{ parsed["Intern"]["temperature"], parsed["Intern"]["humidity"], parsed["Intern"]["pressure"] };
	}
	else {
		return SensorData{ -2, -2, -2 };
	}
}
SensorInfo getSensorInfo(String ip) {
	HTTPClient http;
	http.begin("http://" + ip + "/getSensorInfo");
	int httpCode = http.GET();
	SensorInfo result;
	while (httpCode < 0) {
		httpCode = http.GET();
	}
	String payload = http.getString();   //Get the request response payload
	result = parseSensorInfo(payload);
	http.end();
	delay(0);
	Serial.println(payload);
	return result;
}
SensorInfo parseSensorInfo(String JSONMessage) {
	JsonObject& parsed = jsonBuffer.parseObject(JSONMessage); //Parse message
	if (!parsed.success()) {   //Check for errors in parsing
		return SensorInfo{ "", "" };
	}
	jsonBuffer.clear();
	return SensorInfo{ parsed["function"], parsed["version"] };
}
bool parseSensorJSON() {
	size_t size = 0;
	File f = SPIFFS.open("/devices.json", "r");
	if (!f) {
		Serial.println(F("Could not open devices.json"));
		return false;
	}
	else
	{
		size = f.size();
		std::unique_ptr<char[]> buf(new char[size]);
		f.readBytes(buf.get(), size);
		JsonArray& parsed = jsonBuffer.parseArray(buf.get()); //Parse message
		if (!parsed.success()) {   //Check for errors in parsing
			Serial.println(F("Error parsing sensors"));
			return false;
		}
		int i = 0;
		for (auto& sensor : parsed) {
			sensors[i] = { "", sensor["SensorMAC"], sensor["SensorFunction"], sensor["FunctionVersion"] };
			i++;
		}
		jsonBuffer.clear();
		delay(0);
		return true;
	}

}
void saveSensorSettings() {
	JsonArray& jsonSensors = sensorBuffer.createArray();
	File f = SPIFFS.open("/devices.json", "w");
	if (f) {
		for (int i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (sensors[i].isInitialised()) {
				JsonObject& jsonSensor = sensorBuffer.createObject();
				jsonSensor["SensorIP"] = sensors[i].SensorIP;
				jsonSensor["SensorMAC"] = sensors[i].SensorMAC;
				jsonSensor["SensorFunction"] = sensors[i].SensorFunction;
				jsonSensor["FunctionVersion"] = sensors[i].FunctionVersion;
				jsonSensors.add(jsonSensor);
				delay(0);
			}
		}
		jsonSensors.printTo(f);
		f.close();
	}
	sensorBuffer.clear();
	updateSensors = false;
}
/**
Json related Functions end
*/
/**
   Webserver Functions Start
*/
void handleRoot() {
	server.sendHeader("Location", "/index.html", true);  //Redirect to our html web page
	server.send(302, "text/plane", "");
}
void handleWebRequests() {
	if (loadFromSpiffs(server.uri())) return;
	server.send(404, "text/plain", "404 Not Found");
}
void getStatistics() {
	if (server.args() == 1) {
		if (server.argName(0) == "MAC") {
			if (!loadFromSpiffs(dataFolder + server.arg(0)))server.send(404, "text/plain", "404 Not Found");
		}
	}
}
void handleFileUpload() {
	// upload a new file to the SPIFFS
	HTTPUpload& upload = server.upload();
	if (upload.status == UPLOAD_FILE_START) {
		String filename = upload.filename;
		if (!filename.startsWith("/")) filename = "/" + filename;
		Serial.print("handleFileUpload Name: "); Serial.println(filename);
		fsUploadFile = SPIFFS.open(filename, "w");            // Open the file for writing in SPIFFS (create if it doesn't exist)
		filename = String();
	}
	else if (upload.status == UPLOAD_FILE_WRITE) {
		if (fsUploadFile)
			fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
	}
	else if (upload.status == UPLOAD_FILE_END) {
		if (fsUploadFile) {                                    // If the file was successfully created
			fsUploadFile.close();                               // Close the file again
			Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
			server.send(200, "text/html", "<html><body><h1>Upload Success</h1></body></html>");
		}
		else {
			server.send(500, "text/html", "<html><body><h1>Couldn't create file</h1></body></html>");
		}
	}
}
bool loadFromSpiffs(String path) {
	if (!SPIFFS.exists(path.c_str())) {
		return false;
	}
	String dataType = "text/plain";
	if (path.endsWith("/")) path += "index.html";
	if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
	else if (path.endsWith(".html")) dataType = "text/html";
	else if (path.endsWith(".htm")) dataType = "text/html";
	else if (path.endsWith(".css")) dataType = "text/css";
	else if (path.endsWith(".js")) dataType = "application/javascript";
	else if (path.endsWith(".png")) dataType = "image/png";
	else if (path.endsWith(".gif")) dataType = "image/gif";
	else if (path.endsWith(".jpg")) dataType = "image/jpeg";
	else if (path.endsWith(".ico")) dataType = "image/x-icon";
	else if (path.endsWith(".xml")) dataType = "text/xml";
	else if (path.endsWith(".pdf")) dataType = "application/pdf";
	else if (path.endsWith(".zip")) dataType = "application/zip";
	else if (path.endsWith(".json")) dataType = "application/json";
	File dataFile = SPIFFS.open(path.c_str(), "r");
	if (server.hasArg("download")) dataType = "application/octet-stream";
	if (server.streamFile(dataFile, dataType) != dataFile.size()) {
	}
	dataFile.close();
	return true;
}
/**
   Webserver Functions Ende
*/