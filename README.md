# cphw-esp32

This is an ESP32 app to unlock the Copenhagen Wheel. It uses BLE to connect to the wheel, authenticates with passcode, provides option to change ride mode. The this the ESP32 controller shuts off after 10 seconds of inactivity. 

The app uses a hard wired passcode. Before using it, the app must be run once to find the wheel identification number. With this, the wheel passcode can be found in passcode.cvs. 

See the wiki for more information: https://github.com/slviajero/cphw-esp32/wiki
