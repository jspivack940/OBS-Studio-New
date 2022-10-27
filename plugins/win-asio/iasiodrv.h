#pragma once
#include "asiosys.h"
#include "asio.h"
#define WIN32_MEAN_AND_LEAN
#include <stdio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <functional>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include "windows.h"
#include <locale>
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#include <atlbase.h>
#include <atlcom.h>
#include <atlstr.h>
#include <codecvt>
#include <cstddef>
#include <cstdint>

/* Forward Declarations */ 

#ifndef __ASIODRIVER_FWD_DEFINED__
#define __ASIODRIVER_FWD_DEFINED__
typedef interface IASIO IASIO;
#endif 	/* __ASIODRIVER_FWD_DEFINED__ */

interface IASIO : public IUnknown
{

	virtual ASIOBool init(void *sysHandle) = 0;
	virtual void getDriverName(char *name) = 0;	
	virtual long getDriverVersion() = 0;
	virtual void getErrorMessage(char *string) = 0;	
	virtual ASIOError start() = 0;
	virtual ASIOError stop() = 0;
	virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) = 0;
	virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) = 0;
	virtual ASIOError getBufferSize(long *minSize, long *maxSize,
		long *preferredSize, long *granularity) = 0;
	virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) = 0;
	virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) = 0;
	virtual ASIOError setClockSource(long reference) = 0;
	virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) = 0;
	virtual ASIOError getChannelInfo(ASIOChannelInfo *info) = 0;
	virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels,
		long bufferSize, ASIOCallbacks *callbacks) = 0;
	virtual ASIOError disposeBuffers() = 0;
	virtual ASIOError controlPanel() = 0;
	virtual ASIOError future(long selector,void *opt) = 0;
	virtual ASIOError outputReady() = 0;
};
