#pragma once
using namespace System;
namespace ScreenRecorderLibNew {
	public ref class AudioDevice {
	public:
		AudioDevice() {};
		AudioDevice(String^ deviceName, String^ friendlyName) {
			DeviceName = deviceName;
			FriendlyName = friendlyName;
		}
		virtual property String^ DeviceName;
		virtual property String^ FriendlyName;
	};
}