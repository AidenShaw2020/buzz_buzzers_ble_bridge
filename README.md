It's creates bluetooth bridge between RPCSC3 on "Mac" and PS2 wired buzzers.

What do you need:
•	ESP32-S3 OTG
•	Some USB connectors for create powered USB-C to USB-A, or bought it

Process:
1.	Prepare ESP board for OTG using (some boards need solder two pads (in-out + usb-otg)
2.	Install Arduino IDE
3.	Install ESP32 boards
4.	Install library esp32beans/ESP32_USB_Host_HID
5.	Install library ESP32-BLE-Gamepad
6.	Select ESP32S3 Dev Module
7.	Open my code and flash it onto ESP
8.	Pair gamepad with Mac
9.	Connect buzzers via active USB-C to USB-A converter to OTG port
10.	Open terminal in folder with RPCS3
11.	Run this command:export SDL_GAMECONTROLLERCONFIG='0300000002e50000abbb000000000000,BUZZ_BLE_BRIDGE,platform:Windows,a:b15,b:b16,x:b17,y:b18,back:b19,guide:b10,start:b11,leftstick:b12,rightstick:b13,leftshoulder:b14,rightshoulder:b5,dpup:b6,dpdown:b7,dpleft:b8,dpright:b9,-leftx:b0,+leftx:b1,-lefty:b2,+lefty:b3,-rightx:b4,'
12.	In same terminal run this command RPCS3.app/Contents/MacOS/rpcs3
13.	In RPSC3 I/O settings select "Buzz! Emulated Controller" value: 1 controller (1-4 players)
14.	In RPCS3 right click on Buzz game and select "Create custom gamepad configuration"
15.	Select SDL as handler for players 1-4
16.	Clear mapping for this players
17.	Map your buzzers for this buttons (repeat for all 4 users):
R1 = RED
Triangle = BLUE
Square = ORANGE
Circle = GREEN
Cross = YELLOW
18.	Right click on Buzz game and select "Boot with custom configuration"
