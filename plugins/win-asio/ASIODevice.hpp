/*  Copyright (c) 2022 pkv <pkv@obsproject.com>
 *
 * This implementation is jacked from JUCE (juce_ASIO.cpp). I've reused their
 * classes and adapted them to obs.
 * Credits to their authors.
 * I also reused some stuff that I co-authored when writing the obs-asio plugin
 * based on RTAudio, Portaudio, Bassasio & Juce.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

/* juce_ASIO.cpp license
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/
#pragma once
// clang-format off
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "iasiodrv.h"
#include <util/base.h>
#include "byteorder.h"
#include <util/threading.h>
#include <util/deque.h>
// clang-format on
#define ASIOCALLBACK __cdecl
#define ASIO_LOG(level, format, ...) blog(level, "[asio_device '%s']: " format, this->getName().c_str(), ##__VA_ARGS__)
#define ASIO_LOG2(level, format, ...) blog(level, "[asio_device_list]: " format, ##__VA_ARGS__)
#define debug(format, ...) ASIO_LOG(LOG_DEBUG, format, ##__VA_ARGS__)
#define warn(format, ...) ASIO_LOG(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) ASIO_LOG(LOG_INFO, format, ##__VA_ARGS__)
#define info2(format, ...) ASIO_LOG2(LOG_INFO, format, ##__VA_ARGS__)
#define error(format, ...) ASIO_LOG(LOG_ERROR, format, ##__VA_ARGS__)
#define error2(format, ...) ASIO_LOG2(LOG_ERROR, format, ##__VA_ARGS__)

#define String std::string

constexpr int maxNumASIODevices = 16;
class ASIODevice;
class ASIODeviceList;
static ASIODevice *currentASIODev[maxNumASIODevices] = {};

#define MAX_DEVICE_CHANNELS 32

struct asio_data {
  /* common */
  ASIODevice *asio_device;                  // asio device (source plugin: input; output plugin: output)
  int asio_client_index[maxNumASIODevices]; // index of obs source in device client list
  const char *device;                       // device name
  uint8_t device_index;                     // device index in the driver list
  enum speaker_layout speakers;             // speaker layout
  int sample_rate;                          // 44100 or 48000 Hz
  uint8_t in_channels;                      // number of device input channels
  uint8_t out_channels; // output :number of device output channels; source: number of channels selected from layout to
                        // stream to obs (ex: 5.1 ==> 6 output channels from source to obs)
  std::atomic<bool> stopping; // signals the source is stopping
  /* source */
  obs_source_t *source;
  int route[MAX_AUDIO_CHANNELS]; // stores the channel re-ordering info
  std::atomic<bool> active;      // tracks whether the device is streaming
  /* output*/
  obs_output_t *output;
  uint8_t obs_track_channels;         // number of obs output channels
  int out_route[MAX_DEVICE_CHANNELS]; // stores the channel re-ordering info for outputs
};

static int get_obs_output_channels() {
  struct obs_audio_info aoi;
  obs_get_audio_info(&aoi);
  return (int)get_audio_channels(aoi.speakers);
}

//============================================================================
struct ASIOSampleFormat {
  ASIOSampleFormat() noexcept {}

  ASIOSampleFormat(long type) noexcept {
    switch (type) {
    case ASIOSTInt16MSB:
      byteStride = 2;
      littleEndian = false;
      bitDepth = 16;
      break;
    case ASIOSTInt24MSB:
      byteStride = 3;
      littleEndian = false;
      break;
    case ASIOSTInt32MSB:
      bitDepth = 32;
      littleEndian = false;
      break;
    case ASIOSTFloat32MSB:
      bitDepth = 32;
      littleEndian = false;
      formatIsFloat = true;
      break;
    case ASIOSTFloat64MSB:
      bitDepth = 64;
      byteStride = 8;
      littleEndian = false;
      break;
    case ASIOSTInt32MSB16:
      bitDepth = 16;
      littleEndian = false;
      break;
    case ASIOSTInt32MSB18:
      littleEndian = false;
      break;
    case ASIOSTInt32MSB20:
      littleEndian = false;
      break;
    case ASIOSTInt32MSB24:
      littleEndian = false;
      break;
    case ASIOSTInt16LSB:
      byteStride = 2;
      bitDepth = 16;
      break;
    case ASIOSTInt24LSB:
      byteStride = 3;
      break;
    case ASIOSTInt32LSB:
      bitDepth = 32;
      break;
    case ASIOSTFloat32LSB:
      bitDepth = 32;
      formatIsFloat = true;
      break;
    case ASIOSTFloat64LSB:
      bitDepth = 64;
      byteStride = 8;
      break;
    case ASIOSTInt32LSB16:
      bitDepth = 16;
      break;
    case ASIOSTInt32LSB18:
      break; // (unhandled)
    case ASIOSTInt32LSB20:
      break; // (unhandled)
    case ASIOSTInt32LSB24:
      break;

    case ASIOSTDSDInt8LSB1:
      break; // (unhandled)
    case ASIOSTDSDInt8MSB1:
      break; // (unhandled)
    case ASIOSTDSDInt8NER8:
      break; // (unhandled)

    default:
      break;
    }
  }

  void convertToFloat(const void *src, float *dst, int samps) const noexcept {
    if (formatIsFloat) {
      memcpy(dst, src, samps * sizeof(float));
    } else {
      switch (bitDepth) {
      case 16:
        convertInt16ToFloat(static_cast<const char *>(src), dst, byteStride, samps, littleEndian);
        break;
      case 24:
        convertInt24ToFloat(static_cast<const char *>(src), dst, byteStride, samps, littleEndian);
        break;
      case 32:
        convertInt32ToFloat(static_cast<const char *>(src), dst, byteStride, samps, littleEndian);
        break;
      default:
        break;
      }
    }
  }

  void convertFromFloat(const float *src, void *dst, int samps) const noexcept {
    if (formatIsFloat) {
      memcpy(dst, src, samps * sizeof(float));
    } else {
      switch (bitDepth) {
      case 16:
        convertFloatToInt16(src, static_cast<char *>(dst), byteStride, samps, littleEndian);
        break;
      case 24:
        convertFloatToInt24(src, static_cast<char *>(dst), byteStride, samps, littleEndian);
        break;
      case 32:
        convertFloatToInt32(src, static_cast<char *>(dst), byteStride, samps, littleEndian);
        break;
      default:
        break;
      }
    }
  }

  void clear(void *dst, int numSamps) noexcept {
    if (dst != nullptr)
      // dst = calloc(numSamps * byteStride, sizeof(float));
      memset(dst, 0, numSamps * byteStride);
  }

  int bitDepth = 24, byteStride = 4;
  bool formatIsFloat = false, littleEndian = true;

private:
  static double jlimit(double lowerLimit, double upperLimit, double valueToConstrain) noexcept {
    assert(lowerLimit <= upperLimit); // if these are in the wrong order, results are unpredictable..

    return valueToConstrain < lowerLimit ? lowerLimit : (upperLimit < valueToConstrain ? upperLimit : valueToConstrain);
  }

  static void convertInt16ToFloat(const char *src, float *dest, int srcStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double g = 1.0 / 32768.0;

    if (littleEndian) {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * (short)ByteOrder::littleEndianShort(src));
        src += srcStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * (short)ByteOrder::bigEndianShort(src));
        src += srcStrideBytes;
      }
    }
  }

  static void convertFloatToInt16(const float *src, char *dest, int dstStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double maxVal = (double)0x7fff;

    if (littleEndian) {
      while (--numSamples >= 0) {
        *(uint16_t *)dest = ByteOrder::swapIfBigEndian((uint16_t)(short)jlimit(-maxVal, maxVal, maxVal * *src++));
        dest += dstStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        *(uint16_t *)dest = ByteOrder::swapIfLittleEndian((uint16_t)(short)jlimit(-maxVal, maxVal, maxVal * *src++));
        dest += dstStrideBytes;
      }
    }
  }

  static void convertInt24ToFloat(const char *src, float *dest, int srcStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double g = 1.0 / 0x7fffff;

    if (littleEndian) {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * ByteOrder::littleEndian24Bit(src));
        src += srcStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * ByteOrder::bigEndian24Bit(src));
        src += srcStrideBytes;
      }
    }
  }

  static void convertFloatToInt24(const float *src, char *dest, int dstStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double maxVal = (double)0x7fffff;

    if (littleEndian) {
      while (--numSamples >= 0) {
        ByteOrder::littleEndian24BitToChars((uint32_t)(int32_t)jlimit(-maxVal, maxVal, maxVal * *src++), dest);
        dest += dstStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        ByteOrder::bigEndian24BitToChars((uint32_t)(int32_t)jlimit(-maxVal, maxVal, maxVal * *src++), dest);
        dest += dstStrideBytes;
      }
    }
  }

  static void convertInt32ToFloat(const char *src, float *dest, int srcStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double g = 1.0 / 0x7fffffff;

    if (littleEndian) {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * (int)ByteOrder::littleEndianInt(src));
        src += srcStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        *dest++ = (float)(g * (int)ByteOrder::bigEndianInt(src));
        src += srcStrideBytes;
      }
    }
  }

  static void convertFloatToInt32(const float *src, char *dest, int dstStrideBytes, int numSamples,
                                  bool littleEndian) noexcept {
    const double maxVal = (double)0x7fffffff;

    if (littleEndian) {
      while (--numSamples >= 0) {

        *(uint32_t *)dest = ByteOrder::swapIfBigEndian((uint32_t)(int32_t)jlimit(-maxVal, maxVal, maxVal * *src++));
        dest += dstStrideBytes;
      }
    } else {
      while (--numSamples >= 0) {
        *(uint32_t *)dest = ByteOrder::swapIfLittleEndian((uint32_t)(int32_t)jlimit(-maxVal, maxVal, maxVal * *src++));
        dest += dstStrideBytes;
      }
    }
  }
};

/* log asio sdk errors */
static void asioErrorLog(String context, long error) {
  const char *err = "Unknown error";

  switch (error) {
  case ASE_OK:
    return;
  case ASE_NotPresent:
    err = "Not Present";
    break;
  case ASE_HWMalfunction:
    err = "Hardware Malfunction";
    break;
  case ASE_InvalidParameter:
    err = "Invalid Parameter";
    break;
  case ASE_InvalidMode:
    err = "Invalid Mode";
    break;
  case ASE_SPNotAdvancing:
    err = "Sample position not advancing";
    break;
  case ASE_NoClock:
    err = "No Clock";
    break;
  case ASE_NoMemory:
    err = "Out of memory";
    break;
  default:
    break;
  }

  blog(LOG_ERROR, "ASIO SDK error %s - %s", context.c_str(), err);
}

class ASIODevice {
public:
  /* Capture Audio.
   * Each device will stream audio to a number of obs asio sources acting as audio clients.
   * The clients are listed in this public vector which stores the asio_data struct ptr.
   */
  std::vector<struct asio_data *> obs_clients;
  int current_nb_clients;

  /* Output Audio.*/
  // Each device can be a client to a single obs output which outputs audio to the device.
  struct asio_data *obs_output_client = nullptr;
  // Circular buffer to store the frames which are passed to asio devices.
  struct deque excess_frames[MAX_DEVICE_CHANNELS];
  // For each output channel, this sets a given track. -1 means no track.
  int obs_track[MAX_DEVICE_CHANNELS];
  // For each output channel, this sets a given channel from an obs track. -1 means a mute channel.
  int obs_track_channel[MAX_DEVICE_CHANNELS];
  ASIOSampleType output_type;
  uint8_t silentBuffers8[4096] = {0};
  std::atomic_bool capture_started;
  long totalNumInputChans = 0, totalNumOutputChans = 0;
  IASIO *asioObject = {};

public:
  ASIODevice(const std::string &devName, CLSID clsID, int slotNumber);
  ~ASIODevice();
  void updateSampleRates();
  String getName();
  std::vector<String> getOutputChannelNames();
  std::vector<String> getInputChannelNames();
  std::vector<double> getAvailableSampleRates();
  std::vector<int> getAvailableBufferSizes();
  int getDefaultBufferSize();
  int getXRunCount() const noexcept;
  String get_sample_format(int type);
  String open(double sr, int bufferSizeSamples);
  void close();
  bool isOpen();
  bool isPlaying();
  int getCurrentBufferSizeSamples();
  double getCurrentSampleRate();
  int getCurrentBitDepth();
  int getOutputLatencyInSamples();
  int getInputLatencyInSamples();
  int readBufferSizes(int bufferSizeSamples);
  String getLastError();
  bool hasControlPanel();
  bool showControlPanel();
  void resetRequest();
  void timerCallback();

private:
  //==============================================================================

  bool bComInitialized = false;

  ASIOCallbacks callbacks;

  CLSID classId;
  String errorstring;
  std::string deviceName;

  std::vector<std::string> inputChannelNames;
  std::vector<std::string> outputChannelNames;

  std::vector<double> sampleRates;
  std::vector<int> bufferSizes;
  long inputLatency = 0, outputLatency = 0;
  long minBufferSize = 0, maxBufferSize = 0, preferredBufferSize = 0, bufferGranularity = 0;
  ASIOClockSource clocks[MAX_DEVICE_CHANNELS] = {};
  int numClockSources = 0;

  int currentBlockSizeSamples = 0;
  int currentBitDepth = 16;
  double currentSampleRate = 0;

  ASIOBufferInfo *bufferInfos = nullptr;
  float *inBuffers[MAX_DEVICE_CHANNELS];
  float *outBuffers[MAX_DEVICE_CHANNELS];
  float *ioBufferSpace = nullptr;
  ASIOSampleFormat *inputFormat = nullptr;
  ASIOSampleFormat *outputFormat = nullptr;

  bool deviceIsOpen = false, isStarted = false, buffersCreated = false;
  std::atomic<bool> calledback{false};
  bool postOutput = true, needToReset = false;
  bool insideControlPanelModalLoop = false;
  bool shouldUsePreferredSize = false;
  int xruns = 0;
  std::atomic<bool> timerstop = false;

  //==============================================================================

  String getChannelName(int index, bool isInput) const;
  void reloadChannelNames();
  long refreshBufferSizes();
  void resetBuffers();
  void addBufferSizes(long minSize, long maxSize, long preferredSize, long granularity);
  double getSampleRate() const;
  void setSampleRate(double newRate);
  void updateClockSources();
  void readLatencies();
  void createDummyBuffers(long preferredSize);
  bool removeCurrentDriver();
  bool loadDriver();
  bool tryCreatingDriver(bool &crashed);
  String getLastDriverError() const;
  String initDriver();
  String openDevice();
  void disposeBuffers();

  //==============================================================================
  void ASIOCALLBACK callback(long index);
  void processBuffer(long bufferIndex);
  long asioMessagesCallback(long selector, long value);
  //==============================================================================
  template <int deviceIndex> struct ASIOCallbackFunctions {
    static ASIOTime *ASIOCALLBACK bufferSwitchTimeInfoCallback(ASIOTime *, long index, long directProcess) {
      UNUSED_PARAMETER(directProcess);
      if (auto *d = currentASIODev[deviceIndex])
        d->callback(index);

      return {};
    }

    static void ASIOCALLBACK bufferSwitchCallback(long index, long directProcess) {
      UNUSED_PARAMETER(directProcess);
      if (auto *d = currentASIODev[deviceIndex])
        d->callback(index);
    }

    static long ASIOCALLBACK asioMessagesCallback(long selector, long value, void *, double *) {
      if (auto *d = currentASIODev[deviceIndex])
        return d->asioMessagesCallback(selector, value);

      return {};
    }

    static void ASIOCALLBACK sampleRateChangedCallback(ASIOSampleRate) {
      if (auto *d = currentASIODev[deviceIndex])
        d->resetRequest();
    }

    static void setCallbacks(ASIOCallbacks &callbacks) noexcept {
      callbacks.bufferSwitch = &bufferSwitchCallback;
      callbacks.asioMessage = &asioMessagesCallback;
      callbacks.bufferSwitchTimeInfo = &bufferSwitchTimeInfoCallback;
      callbacks.sampleRateDidChange = &sampleRateChangedCallback;
    }

    static void setCallbacksForDevice(ASIOCallbacks &callbacks, ASIODevice *device) noexcept {
      if (currentASIODev[deviceIndex] == device)
        setCallbacks(callbacks);
      else
        ASIOCallbackFunctions<deviceIndex + 1>::setCallbacksForDevice(callbacks, device);
    }
  };

  void setCallbackFunctions() noexcept;
};

template <> struct ASIODevice::ASIOCallbackFunctions<maxNumASIODevices> {
  static void setCallbacksForDevice(ASIOCallbacks &, ASIODevice *) noexcept {}
};

//=============================================================================
// driver struct to store name and CLSID | class to retrieve the driver list //
//=============================================================================

struct AsioDriver {
  std::string name;
  std::string clsid;
};

class ASIODeviceList {
private:
  bool hasScanned = false;
  std::vector<std::string> blacklisted = {"ASIO DirectX Full Duplex", "ASIO Multimedia Driver", "Realtek ASIO"};
  bool isBlacklistedDriver(const std::string &driverName);

public:
  std::vector<std::string> deviceNames;
  std::vector<CLSID> classIds;
  std::vector<struct AsioDriver> drivers;

  ASIODeviceList();

  ~ASIODeviceList();

  void scanForDevices();
  int findFreeSlot();
  int getIndexFromDeviceName(const std::string name);
  ASIODevice *attachDevice(const std::string inputDeviceName);
};
