<div align="center">
 
# Simonulator<br />
An IBM Simon Emulator Based on MAME<br />

[![LICENSE](https://img.shields.io/badge/LICENSE-GPL2.0-blue.svg?style=for-the-badge)](https://github.com/Inter1006/PenPointOS_Vbox/blob/main/LICENSE )

Language  语言<br />
[简体中文](https://www.youtube.com/watch?v=dQw4w9WgXcQ)  |  ENGLISH<br />


</div>

## 📝What is Simonulator?
Simonulator is a MAME-based simulator designed to emulate IBM Simon and reproduce its functionality as closely as possible.<br />
For information about IBM Simon, please refer to [Wikipedia Page](https://en.wikipedia.org/wiki/IBM_Simon).

## 📚Current progress
As of August 24, 2026, the project has achieved the following progress.<br />
<br />
Completed:<br />

* VG239's two ROM mappings
* Correct aspect ratio and orientation
* Analog touchscreen input
* Backlighting system
* RTC clock (correct clock speed)

Initially achieved:<br />

* Phone functionality (mostly complete; now you can answer calls and stay on the line).
* Status indicator lights on the device(The amber indicator light currently only reflects the power status of the virtual phone hardware and is not yet linked to the working status of the phone hardware, nor is it consistent with the real phone.)
* PCMCIA Card Support
* Serial port on the bottom of the device
* The beeping sound (speed and pitch are inconsistent with the actual device)

Not yet realized:<br />
* Emergency call (will report an error)
* Physical buttons (power button and volume buttons)
* Standby mode (memory retention not implemented)
* Mail and Fax


## 📥How to use?
**Download** <br />
You can find the pre-compiled version at [Releases](https://github.com/INTERINIT/Simonulator/releases).<br />

**Preparations before starting** <br />
Before you begin, you will need a complete Simon dump. <br />
The dump should consist of the following parts: <br />
1.Simon's 1MB FlashROM, named "SIMONFlash.bin" <br />
2.Simon's 128KB BIOS chip, named "simonbios.bin" <br />
(The above two files should be placed in `\roms\ibmsimon`)<br />

*For some reason, this project does not provide any dump copies, but you can follow This [Project](https://github.com/INTERINIT/SimonDump).*

**Use the virtual AMPS switch** <br />
Run `\Start_Switch.bat` to start the switch<br />

Common Commands:

|Command      |function                |For example      |
|-------------|------------------------|-----------------|
|list         |List all online devices |                 |
|ring A B     |Call B as A             |ring 1001 1002   |
|hang A       |Set A to hang-up mode(equivalent to pressing End on side A to hang up).|hang 1001 |
set A [Service Status] [signal strength] [Operator Name] |Modify the network information obtained by A|set 1001 Home1 6 helloworld
|clear        |Clear screen            |                 |
|quit         |Quit the switch         |                 |
<br />

**How to compile?** <br />
Under Construction

## 🛠️Under Construction
under construction

## The Use of AI
The following parts of this project use **Codex**: <br />
1.Automatic capture and analysis of some logs <br />
2.Partial reverse engineering of C:\Phone.exe <br />

**All other code was written manually by real people,all tests were completed by me personally, and all code involving AI has been manually reviewed.**


## ❗Known problem
Because the MAME emulator is relatively inefficient, the overall running speed of the emulation instance will decrease after the phone hardware is started.<br />
We recommend that you experience the system functions with the phone turned off.<br />

You tell me

## ℹAbout
**Author:INTERINIT**
<table>
  <tr>
    <td align="center"><a href="https://github.com/INTERINIT"><img src="https://github.com/INTERINIT.png" width="100px;" alt=""/><br /><sub><b>Github Page</b></sub></a><br /></td>
    <td align="center"><a href="https://space.bilibili.com/1756824708"><img src="https://github.com/user-attachments/assets/48a9c033-8c87-4fa2-b5f9-1be5a9c9d665" width="100px;" alt=""/><br /><sub><b>bilibili</b></sub></a><br /></td>
  </tr>
</table>

**Special thanks(The rankings are in no particular order)**
<table>
  <tr>
    <td align="center"><a href="https://space.bilibili.com/484165196"><img src="https://github.com/WindowsNT351.png" width="100px;" alt=""/><br /><sub><b>351Workshop<br />bilibili</b></sub></a><br /></td>
    <td align="center"><a href="https://github.com/simoneer"><img src="https://github.com/simoneer.png" width="100px;" alt=""/><br /><sub><b>Simoneer<br />Github Page</b></sub></a><br /></td>
    <td align="center"><a href="https://space.bilibili.com/353176009"><img src="https://github.com/user-attachments/assets/079e9e4f-ce7a-4bce-8044-818f484f657e" width="100px;" alt=""/><br /><sub><b>Digitaalia<br />bilibili</b></sub></a><br /></td>
  </tr>
  
</table>
<br />

QQ Group:**981893945** <br />
Welcome to join us.

## 🤝Links from other websites
[351Workshop Offical Website](https://www.351workshop.top/)<br />
[Simon History](https://simoneer.github.io/history/)<br />
