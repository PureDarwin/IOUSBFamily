### IOUSBFamily
This repository contains the source code for IOUSBFamily, a kernel extension that provides support for USB devices in macOS. It was originally developed by Apple and released as open source software and is now being maintained by PureDarwin.
Features
 * Provides a high-level interface for communicating with USB devices
 * Supports a wide range of USB device classes
 * Implements USB standard compliant protocols
 * Handles USB device enumeration and configuration
 * Manages USB bandwidth allocation and power management

### Supported Devices
IOUSBFamily supports a wide variety of USB devices, including:
 * Human Interface Devices (HID): keyboards, mice, joysticks
 * Mass Storage Devices: hard drives, flash drives
 * Communication Devices: modems, network adapters
 * Audio Devices: speakers, microphone

### Building and Installation
To build IOUSBFamily, you will need the following:
 * A Mac running macOS 10.15 or higher. 
 * Xcode with the macOS SDK
 * The kernel source code for the version of macOS you are targeting
Once you have these prerequisites, you can build IOUSBFamily by following these steps:
 * Clone this repository
 * Open the IOUSBFamily.xcodeproj project in Xcode
 * Select the appropriate target for your macOS version
 * Build the project
The resulting kernel extension will be located in the build directory. You can install it by copying it to the /System/Library/Extensions directory.
This is for PureDarwin or other Darwin OS's shouldn't be used in macOS/ios. 

### Contributing
Contributions to IOUSBFamily are welcome. Please submit pull requests with your changes.
Also no reversed engineered code to be added to this Repository given 

### License
IOUSBFamily is released under the Apple Public Source License.

### Disclaimer
This repository is provided for informational purposes only. PureDarwin is not responsible for any damages caused by the use of this software.
