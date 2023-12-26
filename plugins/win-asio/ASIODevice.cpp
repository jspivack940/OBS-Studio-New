/*  Copyright (c) 2022 pkv <pkv@obsproject.com>
 *
 * This implementation is jacked from JUCE (juce_win32_ASIO.cpp)
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

#include "ASIODevice.hpp"

os_event_t *shutting_down;
std::atomic<bool> shutting_down_atomic = false;

void OBSEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		shutting_down_atomic = true;
	}
}

ASIODevice::~ASIODevice()
{
	free(inputFormat);
	free(outputFormat);
	free(ioBufferSpace);
	free(bufferInfos);
	if (bComInitialized) {
		CoUninitialize();
	}
	for (int i = 0; i < maxNumASIODevices; ++i)
		if (currentASIODev[i] == this)
			currentASIODev[i] = nullptr;

	close();
	debug(" driver deleted.");

	if (!removeCurrentDriver())
		info("** Driver crashed while being closed");
}

ASIODevice::ASIODevice(const std::string &devName, CLSID clsID, int slotNumber) : classId(clsID)
{
	if (!bComInitialized)
		bComInitialized = SUCCEEDED(CoInitialize(nullptr));

	deviceName = devName;
	assert(currentASIODev[slotNumber] == nullptr);
	currentASIODev[slotNumber] = this;

	openDevice();
	current_nb_clients = 0;
}

void ASIODevice::updateSampleRates()
{
	// find a list of sample rates..
	std::vector<double> newRates;

	if (asioObject != nullptr) {
		for (auto rate : {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000,
				  352800, 384000, 705600, 768000})
			if (asioObject->canSampleRate((double)rate) == 0)
				newRates.push_back((double)rate);
	}

	if (newRates.empty()) {
		auto cr = getSampleRate();
		blog(LOG_INFO, "[asio source:] No sample rates supported - current rate: %i", cr);
		if (cr > 0)
			newRates.push_back(cr);
	}

	if (sampleRates != newRates) {
		sampleRates.swap(newRates);
		String samplelist;
		for (double r : sampleRates)
			samplelist = samplelist.append(std::to_string(r)).append(", ");
		debug("Supported rates: %s", samplelist.c_str());
	}
}

String ASIODevice::getName()
{
	return deviceName;
}

std::vector<String> ASIODevice::getOutputChannelNames()
{
	return outputChannelNames;
}

std::vector<String> ASIODevice::getInputChannelNames()
{
	return inputChannelNames;
}

std::vector<double> ASIODevice::getAvailableSampleRates()
{
	return sampleRates;
}

std::vector<int> ASIODevice::getAvailableBufferSizes()
{
	return bufferSizes;
}

int ASIODevice::getDefaultBufferSize()
{
	return preferredBufferSize;
}

int ASIODevice::getXRunCount() const noexcept {	return xruns;}

String ASIODevice::get_sample_format(int type)
{
	String message = "24 bit int";
	switch (type) {
	case 17:
		break;
	case 18:
		message = "32 bit int";
		break;
	case 19:
		message = "32 bit float";
		break;
	default:
		message = "uncommon format number " + std::to_string(type);
	}
	return message;
}

String ASIODevice::open(double sr, int bufferSizeSamples)
{
	if (isOpen())
		close();

	if (bufferSizeSamples < 8 || bufferSizeSamples > 32768)
		shouldUsePreferredSize = true;

	if (asioObject == nullptr) {
		auto openingError = openDevice();

		if (asioObject == nullptr) {
			error("%s", openingError.c_str());
			return openingError;
		}
	}
	isStarted = false;

	auto err = asioObject->getChannels(&totalNumInputChans, &totalNumOutputChans);
	assert(err == ASE_OK);
	if (totalNumInputChans > 32 || totalNumOutputChans > 32)
		info("Only up to 32 input + 32 output channels are enabled. Higher channel counts are disabled.");
	totalNumInputChans = min(totalNumInputChans, 32);
	totalNumOutputChans = min(totalNumOutputChans, 32);

	auto sampleRate = sr;
	currentSampleRate = sampleRate;

	updateSampleRates();
	bool isListed = std::find(sampleRates.begin(), sampleRates.end(), sampleRate) != sampleRates.end();
	if (sampleRate == 0 || (sampleRates.size() > 0 && !isListed))
		sampleRate = sampleRates[0];

	if (sampleRate == 0) {
		__assume(false);
		sampleRate = 48000.0;
	}

	updateClockSources();
	currentSampleRate = getSampleRate();

	errorstring.clear();
	buffersCreated = false;

	setSampleRate(sampleRate);
	currentBlockSizeSamples = bufferSizeSamples = readBufferSizes(bufferSizeSamples);

	// (need to get this again in case a sample rate change affected the channel count)
	err = asioObject->getChannels(&totalNumInputChans, &totalNumOutputChans);
	assert(err == ASE_OK);
	if (totalNumInputChans > 32 || totalNumOutputChans > 32)
		info("Only up to 32 input + 32 output channels are enabled. Higher channel counts are disabled.");
	totalNumInputChans = min(totalNumInputChans, 32);
	totalNumOutputChans = min(totalNumOutputChans, 32);

	if (asioObject->future(kAsioCanReportOverload, nullptr) != ASE_OK)
		xruns = -1;

	if (needToReset) {
		info(" Resetting");

		if (!removeCurrentDriver())
			error("** Driver crashed while being closed");

		loadDriver();
		String initError = initDriver();

		if (!initError.empty())
			error("ASIOInit: %s", initError);
		else
			setSampleRate(getSampleRate());

		needToReset = false;
	}
	/* buffers creation; if this fails, try a second time with preferredBufferSize*/
	auto totalBuffers = totalNumInputChans + totalNumOutputChans;
	resetBuffers();

	setCallbackFunctions();

	info("disposing buffers");
	err = asioObject->disposeBuffers();

	info("creating buffers: %i in-out channels, size: %i samples", totalBuffers, currentBlockSizeSamples);
	err = asioObject->createBuffers(bufferInfos, totalBuffers, currentBlockSizeSamples, &callbacks);

	if (err != ASE_OK) {
		currentBlockSizeSamples = preferredBufferSize;
		asioErrorLog("create buffers 2nd attempt", err);

		asioObject->disposeBuffers();
		err = asioObject->createBuffers(bufferInfos, totalBuffers, currentBlockSizeSamples, &callbacks);
	}

	if (err == ASE_OK) {
		buffersCreated = true;
		// This is where input and output buffers are allocated
		ioBufferSpace = (float *)calloc(totalBuffers * currentBlockSizeSamples + 32, sizeof(float));
		//			silentBuffers = (float *)calloc(currentBlockSizeSamples, sizeof(float));

		std::vector<int> types;
		currentBitDepth = 16;

		for (int n = 0; n < (int)totalNumInputChans; ++n) {
			inBuffers[n] = ioBufferSpace + (currentBlockSizeSamples * n);
			ASIOChannelInfo channelInfo = {};
			channelInfo.channel = n;
			channelInfo.isInput = 1;
			asioObject->getChannelInfo(&channelInfo);
			if (n == 0)
				types.push_back(channelInfo.type);
			inputFormat[n] = ASIOSampleFormat(channelInfo.type);
			currentBitDepth = max(currentBitDepth, inputFormat[n].bitDepth);
		}
		// this sets the outputFormat for the device, so that we can convert from obs float
		for (int n = 0; n < (int)totalNumOutputChans; ++n) {
			outBuffers[n] = ioBufferSpace + (currentBlockSizeSamples * (totalNumInputChans + n));
			ASIOChannelInfo channelInfo = {};
			channelInfo.channel = n;
			channelInfo.isInput = 0;
			asioObject->getChannelInfo(&channelInfo);
			if (n == 0)
				types.push_back(channelInfo.type);
			outputFormat[n] = ASIOSampleFormat(channelInfo.type);
			currentBitDepth = max(currentBitDepth, outputFormat[n].bitDepth);
		}
		info("input sample format: %s, output sample format: %s\n ", get_sample_format(types[0]).c_str(),
		     get_sample_format(types[1]).c_str());
		// outputFormat buffers are initially silent
		for (int i = 0; i < totalNumOutputChans; ++i) {
			//outputFormat[i].clear(bufferInfos[totalNumInputChans + i].buffers[0],
			//		      currentBlockSizeSamples);
			//outputFormat[i].clear(bufferInfos[totalNumInputChans + i].buffers[1],
			//		      currentBlockSizeSamples);
			bufferInfos[totalNumInputChans + i].buffers[0] = temp0[i];
			bufferInfos[totalNumInputChans + i].buffers[1] = temp1[i];
		}

		readLatencies();
		refreshBufferSizes();
		deviceIsOpen = true;

		info("starting");
		calledback = false;
		err = asioObject->start();

		if (err != 0) {
			deviceIsOpen = false;
			error("stop on failure");
			Sleep(10);
			asioObject->stop();
			errorstring = "Can't start device";
			Sleep(10);
		} else {
			int count = 300;
			while (--count > 0 && !calledback)
				Sleep(10);

			isStarted = true;

			if (!calledback) {
				errorstring = "Device didn't start correctly";
				error("no callbacks - stopping..");
				asioObject->stop();
			}
		}
	} else {
		errorstring = "Can't create i/o buffers";
	}

	if (!errorstring.empty()) {
		asioErrorLog(errorstring, err);
		disposeBuffers();

		Sleep(20);
		isStarted = false;
		deviceIsOpen = false;

		auto errorCopy = errorstring;
		close(); // (this resets the error string)
		errorstring = errorCopy;
	}

	needToReset = false;
	return errorstring;
}

void ASIODevice::close()
{
	errorstring.clear();
	timerstop = true;
	// stop(); this stops the callbacks, but we're not using explictily callbacks, though it'd be cleaner to do so.

	if (asioObject != nullptr && deviceIsOpen) {

		deviceIsOpen = false;
		isStarted = false;
		needToReset = false;

		info("stopping");

		if (asioObject != nullptr) {
			Sleep(20);
			asioObject->stop();
			Sleep(10);
			disposeBuffers();
		}
		// this resets the "pseudo callbacks"
		current_nb_clients = 0;
		obs_clients.clear();

		Sleep(10);
	}
}

bool ASIODevice::isOpen()
{
	return deviceIsOpen || insideControlPanelModalLoop;
}

bool ASIODevice::isPlaying()
{
	return asioObject != nullptr && current_nb_clients != 0;
};

int ASIODevice::getCurrentBufferSizeSamples()
{
	return currentBlockSizeSamples;
}

double ASIODevice::getCurrentSampleRate()
{
	return currentSampleRate;
}

int ASIODevice::getCurrentBitDepth()
{
	return currentBitDepth;
}

int ASIODevice::getOutputLatencyInSamples()
{
	return outputLatency;
}

int ASIODevice::getInputLatencyInSamples()
{
	return inputLatency;
}

int ASIODevice::readBufferSizes(int bufferSizeSamples)
{
	minBufferSize = 0;
	maxBufferSize = 0;
	bufferGranularity = 0;
	long newPreferredSize = 0;

	if (asioObject->getBufferSize(&minBufferSize, &maxBufferSize, &newPreferredSize, &bufferGranularity) ==
	    ASE_OK) {
		if (preferredBufferSize != 0 && newPreferredSize != 0 && newPreferredSize != preferredBufferSize)
			shouldUsePreferredSize = true;

		if (bufferSizeSamples < minBufferSize || bufferSizeSamples > maxBufferSize)
			shouldUsePreferredSize = true;

		preferredBufferSize = newPreferredSize;
	}

	// unfortunate workaround for certain drivers which crash if you make
	// dynamic changes to the buffer size...
	bool isDigidesign = strstr(deviceName.c_str(), "Digidesign") != NULL;
	shouldUsePreferredSize = shouldUsePreferredSize || isDigidesign;

	if (shouldUsePreferredSize) {
		info("Using preferred size for buffer..");
		auto err = refreshBufferSizes();

		if (err == ASE_OK) {
			bufferSizeSamples = (int)preferredBufferSize;
		} else {
			bufferSizeSamples = 1024;
			asioErrorLog("getBufferSize1", err);
		}

		shouldUsePreferredSize = false;
	}

	return bufferSizeSamples;
}

String ASIODevice::getLastError()
{
	return errorstring;
}

bool ASIODevice::hasControlPanel()
{
	return true;
}

bool ASIODevice::showControlPanel()
{
	info("showing control panel");

	bool done = false;
	insideControlPanelModalLoop = true;
	auto started = os_gettime_ns() * 1000;

	if (asioObject != nullptr) {
		asioObject->controlPanel();

		auto spent = (int)(os_gettime_ns() * 1000 - started);
		debug("spent: %i", spent);

		if (spent > 300) {
			shouldUsePreferredSize = true;
			done = true;
		}
	}

	insideControlPanelModalLoop = false;
	return done;
}

void ASIODevice::resetRequest()
{
	int count = 500;
	while (--count > 0 && !timerstop)
		Sleep(1);
	if (!timerstop)
		timerCallback();
}

void ASIODevice::timerCallback()
{
	if (!insideControlPanelModalLoop) {
		timerstop = true;
		info("restart request!");

		close();
		needToReset = true;
		open(currentSampleRate, currentBlockSizeSamples);
		reloadChannelNames();

	} else {
		int count = 100;
		while (--count > 0 && !timerstop)
			Sleep(1);
		if (!timerstop)
			timerCallback();
	}
}

String ASIODevice::getChannelName(int index, bool isInput) const
{
	ASIOChannelInfo channelInfo = {};
	channelInfo.channel = index;
	channelInfo.isInput = isInput ? 1 : 0;
	asioObject->getChannelInfo(&channelInfo);
	String temp(channelInfo.name);
	return temp;
}

void ASIODevice::reloadChannelNames()
{
	long totalInChannels = 0, totalOutChannels = 0;

	if (asioObject != nullptr && asioObject->getChannels(&totalInChannels, &totalOutChannels) == ASE_OK) {
		if (totalInChannels > 32 || totalOutChannels > 32)
			info("Only up to 32 input + 32 output channels are enabled. Higher channel counts are disabled.");
		totalNumInputChans = min(totalInChannels, 32);
		totalNumOutputChans = min(totalOutChannels, 32);

		inputChannelNames.clear();
		outputChannelNames.clear();

		for (int i = 0; i < totalNumInputChans; ++i)
			inputChannelNames.push_back(getChannelName(i, true));

		for (int i = 0; i < totalNumOutputChans; ++i)
			outputChannelNames.push_back(getChannelName(i, false));
	}
}

long ASIODevice::refreshBufferSizes()
{
	const auto err =
		asioObject->getBufferSize(&minBufferSize, &maxBufferSize, &preferredBufferSize, &bufferGranularity);

	if (err == ASE_OK) {
		bufferSizes.clear();
		addBufferSizes(minBufferSize, maxBufferSize, preferredBufferSize, bufferGranularity);
	}

	return err;
}

void ASIODevice::resetBuffers()
{
	for (int i = 0; i < totalNumInputChans; ++i) {
		bufferInfos[i].isInput = 1;
		bufferInfos[i].channelNum = i;
		bufferInfos[i].buffers[0] = bufferInfos[i].buffers[1] = nullptr;
	}
	for (int j = 0; j < totalNumOutputChans; ++j) {
		bufferInfos[totalNumInputChans + j].isInput = 0;
		bufferInfos[totalNumInputChans + j].channelNum = j;
		bufferInfos[totalNumInputChans + j].buffers[0] = bufferInfos[totalNumInputChans + j].buffers[1] =
			nullptr;
	}
}

void ASIODevice::addBufferSizes(long minSize, long maxSize, long preferredSize, long granularity)
{
	// find a list of buffer sizes..
	info("Available buffer sizes: %i -> %i, preferred: %i, step: %i", minSize, maxSize, preferredSize, granularity);

	if (granularity >= 0) {
		granularity = max(16, (int)granularity);

		for (int i = max((int)(minSize + 15) & ~15, (int)granularity); i <= min(6400, (int)maxSize);
		     i += granularity)
			bufferSizes.push_back(granularity * (i / granularity));
	} else if (granularity < 0) {
		for (int i = 0; i < 18; ++i) {
			const int s = (1 << i);

			if (s >= minSize && s <= maxSize)
				bufferSizes.push_back(s);
		}
	}
}

double ASIODevice::getSampleRate() const
{
	double cr = 0;
	auto err = asioObject->getSampleRate(&cr);
	asioErrorLog("getSampleRate", err);
	return cr;
}

void ASIODevice::setSampleRate(double newRate)
{
	if (currentSampleRate != newRate) {
		info("rate change: %i to %i", currentSampleRate, newRate);
		auto err = asioObject->setSampleRate(newRate);
		asioErrorLog("setSampleRate", err);
		Sleep(10);

		if (err == ASE_NoClock && numClockSources > 0) {
			info("trying to set a clock source..");
			err = asioObject->setClockSource(clocks[0].index);
			asioErrorLog("setClockSource2", err);
			Sleep(10);
			err = asioObject->setSampleRate(newRate);
			asioErrorLog("setSampleRate", err);
			Sleep(10);
		}

		if (err == 0)
			currentSampleRate = newRate;

		// on fail, ignore the attempt to change rate, and run with the current one..
	}
}

void ASIODevice::updateClockSources()
{
	memset(clocks, 0, sizeof(clocks));
	long numSources = 32;
	//numElementsInArray(clocks);
	asioObject->getClockSources(clocks, &numSources);
	numClockSources = (int)numSources;

	bool isSourceSet = false;

	// careful not to remove this loop because it does more than just logging!
	for (int i = 0; i < numClockSources; ++i) {
		std::string s("clock: ");
		s += clocks[i].name;

		if (clocks[i].isCurrentSource) {
			isSourceSet = true;
			s += " (cur)";
		}

		info("%s", s.c_str());
	}

	if (numClockSources > 1 && !isSourceSet) {
		info("setting clock source");
		auto err = asioObject->setClockSource(clocks[0].index);
		asioErrorLog("setClockSource1", err);
		Sleep(20);
	} else {
		if (numClockSources == 0)
			info("no clock sources!");
	}
}

void ASIODevice::readLatencies()
{
	inputLatency = outputLatency = 0;

	if (asioObject->getLatencies(&inputLatency, &outputLatency) != 0)
		info("getLatencies() failed");
	else
		info("Latencies (samples): in = %i, out = %i", (int)inputLatency, (int)outputLatency);
}

void ASIODevice::createDummyBuffers(long preferredSize)
{

	for (int i = 0; i < min(2, (int)totalNumInputChans); ++i) {
		bufferInfos[i].isInput = 1;
		bufferInfos[i].channelNum = i;
		bufferInfos[i].buffers[0] = bufferInfos[i].buffers[1] = nullptr;
	}
	const int outputBufferIndex = min(2, (int)totalNumInputChans);
	for (int i = 0; i < min(2, (int)totalNumOutputChans); ++i) {
		bufferInfos[outputBufferIndex + i].isInput = 0;
		bufferInfos[outputBufferIndex + i].channelNum = i;
		bufferInfos[outputBufferIndex + i].buffers[0] = bufferInfos[i].buffers[1] = nullptr;
	}
	setCallbackFunctions();
	int numChans = outputBufferIndex + min(2, (int)totalNumOutputChans);
	info("creating buffers (dummy): %i channels, size: %i", numChans, (int)preferredSize);

	if (preferredSize > 0) {
		auto err = asioObject->createBuffers(&bufferInfos[0], numChans, preferredSize, &callbacks);
		asioErrorLog("dummy buffers", err);
	}

	long newInps = 0, newOuts = 0;
	asioObject->getChannels(&newInps, &newOuts);
	if (newInps > 32 || newOuts > 32)
		info("Only up to 32 input + 32 output channels are enabled. Higher channel counts are disabled.");
	totalNumInputChans = min(newInps, 32);
	totalNumOutputChans = min(newOuts, 32);
	if (totalNumInputChans != newInps || totalNumOutputChans != newOuts) {
		totalNumInputChans = newInps;
		totalNumOutputChans = newOuts;

		info("checking channel numbers after buffer creation: %i channels in, %i channels out",
		     (int)totalNumInputChans, (int)totalNumOutputChans);
	}

	updateSampleRates();
	reloadChannelNames();
	for (int i = 0; i < totalNumOutputChans; ++i) {
		ASIOChannelInfo channelInfo = {};
		channelInfo.channel = i;
		channelInfo.isInput = 0;
		asioObject->getChannelInfo(&channelInfo);

		outputFormat[i] = ASIOSampleFormat(channelInfo.type);

		if (i < 2) {
			// clear the channels that are used with the dummy stuff
			//outputFormat[i].clear(bufferInfos[outputBufferIndex + i].buffers[0],
			//		      preferredBufferSize);
			//outputFormat[i].clear(bufferInfos[outputBufferIndex + i].buffers[1],
			//		      preferredBufferSize);
			bufferInfos[outputBufferIndex + i].buffers[0] = temp0[i];
			bufferInfos[outputBufferIndex + i].buffers[1] = temp1[i];
		}
	}
}

bool ASIODevice::removeCurrentDriver()
{
	bool releasedOK = true;

	if (asioObject != nullptr) {
		{
			__try {
				asioObject->Release();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				releasedOK = false;
			}
		}
		asioObject = nullptr;
	}
	return releasedOK;
}

bool ASIODevice::loadDriver()
{
	if (!removeCurrentDriver())
		error("** Driver crashed while being closed");
	else
		info("Driver successfully removed");
	bool crashed = false;
	bool ok = tryCreatingDriver(crashed);

	if (crashed)
		error("** Driver crashed while being opened");
	else if (ok)
		info("driver com interface opened");
	return ok;
}

bool ASIODevice::tryCreatingDriver(bool &crashed)
{
	__try {
		return CoCreateInstance(classId, 0, CLSCTX_INPROC_SERVER, classId, (void **)&asioObject) == S_OK;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		crashed = true;
	}
	return false;
}

String ASIODevice::getLastDriverError() const
{
	assert(asioObject != nullptr);

	char buffer[512] = {};
	asioObject->getErrorMessage(buffer);
	String bufferString(buffer);
	return bufferString;
}

String ASIODevice::initDriver()
{
	if (asioObject == nullptr)
		return "No Driver";

	HANDLE hwnd = GetDesktopWindow();
	bool initOk = asioObject->init(&hwnd) == ASIOTrue;
	String driverError;

	// Get error message if init() failed, or if it's a buggy Denon driver,
	// which returns true from init() even when it fails.
	bool isDenon = strstr(deviceName.c_str(), "denon dj asio") != NULL;
	if ((!initOk) || isDenon)
		driverError = getLastDriverError();

	if ((!initOk) && driverError.empty())
		driverError = "Driver failed to initialise";

	if (driverError.empty()) {
		char buffer[512] = {};
		asioObject->getDriverName(buffer); // just in case any flimsy drivers expect this to be called..
	}

	return driverError;
}

String ASIODevice::openDevice()
{
	// open the device and get its info..
	info("opening device: %s", getName().c_str());

	needToReset = false;
	outputChannelNames.clear();
	inputChannelNames.clear();
	bufferSizes.clear();
	sampleRates.clear();
	deviceIsOpen = false;
	totalNumInputChans = 0;
	totalNumOutputChans = 0;
	xruns = 0;
	errorstring.clear();

	if (getName().empty())
		return errorstring;

	long err = 0;

	if (loadDriver()) {
		errorstring = initDriver();
		if (errorstring.empty()) {
			totalNumInputChans = 0;
			totalNumOutputChans = 0;

			if (asioObject != nullptr &&
			    (err = asioObject->getChannels(&totalNumInputChans, &totalNumOutputChans)) == 0) {
				info("channels in: %i, channels out: %i", totalNumInputChans, totalNumOutputChans);
				if (totalNumInputChans > 32 || totalNumOutputChans > 32)
					info("Only up to 32 input + 32 output channels are enabled. Higher channel counts are disabled.");
				totalNumInputChans = min(totalNumInputChans, 32);
				totalNumOutputChans = min(totalNumOutputChans, 32);

				const int chansToAllocate = totalNumInputChans + totalNumOutputChans + 4;
				/* allocate buffers */
				bufferInfos = (ASIOBufferInfo *)calloc(chansToAllocate, sizeof(ASIOBufferInfo));
				inputFormat = (ASIOSampleFormat *)calloc(chansToAllocate, sizeof(ASIOSampleFormat));
				outputFormat = (ASIOSampleFormat *)calloc(chansToAllocate, sizeof(ASIOSampleFormat));
				if ((err = refreshBufferSizes()) == 0) {
					auto currentRate = getSampleRate();

					if (currentRate < 1.0 || currentRate > 192001.0) {
						info("setting default sample rate");
						err = asioObject->setSampleRate(48000.0);
						asioErrorLog("setting sample rate", err);
						// sanity check
						currentRate = getSampleRate();
					}

					currentSampleRate = currentRate;
					postOutput = (asioObject->outputReady() == 0);

					if (postOutput)
						info("outputReady true");

					updateSampleRates();
					// ..doing these steps because cubase does so at this stage
					// in initialisation, and some devices fail if we don't.
					readLatencies();
					createDummyBuffers(preferredBufferSize);
					readLatencies();

					// start and stop because cubase does it..
					err = asioObject->start();
					// ignore an error here, as it might start later after setting other stuff up
					asioErrorLog("start", err);

					Sleep(80);
					asioObject->stop();
				} else {
					errorstring = "Can't detect buffer sizes";
				}
			} else {
				errorstring = "Can't detect asio channels";
			}
		} else {
			info("Initialization failure reported by driver:\n %s\n"
			     "Your device is likely used concurrently "
			     "in another application, but ASIO usually "
			     "supports a single host.",
			     errorstring.c_str());
		}
	} else {
		errorstring = "No such device";
	}

	if (!errorstring.empty()) {
		asioErrorLog(errorstring, err);
		disposeBuffers();

		if (!removeCurrentDriver())
			info("** Driver crashed while being closed");
	} else {
		info("device opened but not started yet");
	}

	deviceIsOpen = false;
	needToReset = false;
	timerstop = true;
	return errorstring;
}

void ASIODevice::disposeBuffers()
{
	if (asioObject != nullptr && buffersCreated) {
		buffersCreated = false;
		asioObject->disposeBuffers();
	}
}

//==============================================================================
void ASIOCALLBACK ASIODevice::callback(long index)
{
	if (isStarted) {
		if (index >= 0)
			if (!shutting_down_atomic)
				processBuffer(index);
			else
				os_event_signal(shutting_down);
	} else {
		if (postOutput && (asioObject != nullptr))
			asioObject->outputReady();
	}
	calledback = true;
}

void ASIODevice::processBuffer(long bufferIndex)
{
	ASIOBufferInfo *infos = bufferInfos;
	int samps = currentBlockSizeSamples;
	// convert to float the samples retrieved from the device
	for (int i = 0; i < (int)totalNumInputChans; ++i) {
		if (inBuffers[i] != nullptr) {
			inputFormat[i].convertToFloat(infos[i].buffers[bufferIndex], inBuffers[i], samps);
		}
	}
	// pass audio to obs clients
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	int output_channels = get_obs_output_channels();

	obs_source_audio out;
	out.speakers = aoi.speakers;
	out.format = AUDIO_FORMAT_FLOAT_PLANAR;
	out.samples_per_sec = (uint32_t)getCurrentSampleRate();
	out.timestamp = os_gettime_ns();
	out.frames = samps;

	for (int idx = 0; idx < obs_clients.size(); idx++) {
		if (obs_clients[idx] != nullptr) {
			if (obs_clients[idx]->device) {
				output_channels = obs_clients[idx]->out_channels;
				out.speakers = (enum speaker_layout)output_channels;
				for (int j = 0; j < output_channels; j++) {
					if (obs_clients[idx]->route[j] >= 0 && !obs_clients[idx]->stopping)
						out.data[j] = (uint8_t *)inBuffers[obs_clients[idx]->route[j]];
					else
						out.data[j] = (uint8_t *)silentBuffers;
				}
				if (!obs_clients[idx]->stopping && obs_clients[idx]->source &&
				    obs_clients[idx]->active) {
					obs_source_output_audio(obs_clients[idx]->source, &out);
				}
			}
		}
	}
	// Writing silent audio : the outBuffers were calloc'd so they're silent.
	// The convertFromFloat could probably be just a cast ... but for the sake of streaming audio later, let's leave it like that.
	// We might be able to create an asio audio output like that.
	for (int i = 0; i < totalNumOutputChans; ++i) {
		if (outBuffers[i] != nullptr) {
			// if we have a ptr to the output, stream audio from obs to device
			if (obs_output_client != nullptr) {
				// consume circular buffer data
				for (size_t j = 0; j < obs_output_client->in_channels; j++)
					circlebuf_pop_front(&excess_frames[5][j], outBuffers[i], samps);
				// convert to device format and send to device
				outputFormat[i].convertFromFloat(
					outBuffers[i], infos[totalNumInputChans + i].buffers[bufferIndex], samps);
			} else {
				// stream silence to output
				infos[totalNumInputChans + i].buffers[bufferIndex] = temp0;
			}
		}
	}

	if (obs_clients.size() == 0) {
		for (int i = 0; i < totalNumOutputChans; ++i)
			infos[totalNumInputChans + i].buffers[bufferIndex] = temp0;
		// we assume bytestride is <= 32 if it's larger we're fucked since we allocated only 32 x 2048
	}

	if (postOutput)
		asioObject->outputReady();
}

long ASIODevice::asioMessagesCallback(long selector, long value)
{
	switch (selector) {
	case kAsioSelectorSupported:
		if (value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
		    value == kAsioLatenciesChanged || value == kAsioSupportsInputMonitor || value == kAsioOverload)
			return 1;
		break;

	case kAsioBufferSizeChange:
		info("kAsioBufferSizeChange");
		resetRequest();
		return 1;
	case kAsioResetRequest:
		info("kAsioResetRequest");
		resetRequest();
		return 1;
	case kAsioResyncRequest:
		info("kAsioResyncRequest");
		resetRequest();
		return 1;
	case kAsioLatenciesChanged:
		info("kAsioLatenciesChanged");
		return 1;
	case kAsioEngineVersion:
		return 2;

	case kAsioSupportsTimeInfo:
	case kAsioSupportsTimeCode:
		return 0;
	case kAsioOverload:
		++xruns;
		return 1;
	}

	return 0;
}

void ASIODevice::setCallbackFunctions() noexcept
{
	ASIOCallbackFunctions<0>::setCallbacksForDevice(callbacks, this);
}

/*===================================================================================*/

std::string TCHARToUTF8(const TCHAR *ptr)
{
#ifndef UNICODE
	std::string res(ptr);
	return res;
#else
	std::wstring wres(ptr);
	std::string res(wres.length(), 0);
	std::transform(wres.begin(), wres.end(), res.begin(), [](wchar_t c) { return (char)c; });
	return res;
#endif
}

bool ASIODeviceList::isBlacklistedDriver(const std::string &driverName)
{
	bool result = false;
	for (int i = 0; i < (int)blacklisted.size(); i++)
		result = result || (blacklisted[i].find(driverName) != std::string::npos);
	return result;
}

ASIODeviceList::ASIODeviceList()
{
	// initialization code
	deviceNames = {};
	classIds = {};
	drivers = {};
}

ASIODeviceList::~ASIODeviceList()
{
	for (int i = 0; i < maxNumASIODevices; i++) {
		if (currentASIODev[i])
			delete currentASIODev[i];
	}
}

void ASIODeviceList::scanForDevices()
{
	hasScanned = true;
	deviceNames.clear();
	classIds.clear();

	HKEY asio;
	DWORD index = 0, nameSize = 256, valueSize = 256;
	LONG err;
	TCHAR name[256], value[256], value2[256];

	info2("Querying installed ASIO drivers.\n");

	if (!SUCCEEDED(err = RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\ASIO"), 0, KEY_READ, &asio))) {

		error("ASIO Error: Failed to open HKLM\\SOFTWARE\\ASIO: status %i", err);
		return;
	}

	while ((err = RegEnumKeyEx(asio, index++, name, &nameSize, nullptr, nullptr, nullptr, nullptr)) ==
	       ERROR_SUCCESS) {

		AsioDriver driver;

		nameSize = 256;
		valueSize = 256;
		/* blacklisted drivers */
		std::string nameString(TCHARToUTF8(name));
		if (isBlacklistedDriver(nameString))
			continue;
		/* Retrieve CLSID */
		if ((err = RegGetValue(asio, name, TEXT("CLSID"), RRF_RT_REG_SZ, nullptr, value, &valueSize)) !=
		    ERROR_SUCCESS) {

			error("Registry Error: Skipping key %s: Couldn't get CLSID, error %i\n", name, err);
			continue;
		}
		CLSID localclsid;
		if (CLSIDFromString((LPOLESTR)value, &localclsid) == S_OK)
			classIds.push_back(localclsid);

		driver.clsid = TCHARToUTF8(value);
		valueSize = 256;

		if ((err = RegGetValue(asio, name, TEXT("Description"), RRF_RT_REG_SZ, nullptr, value2, &valueSize)) !=
		    ERROR_SUCCESS) {

			// Workaround for drivers with incomplete ASIO registration.
			// Observed with M-Audio drivers: the main (64bit) registration is
			// fine but the Wow6432Node version is missing the description.
			driver.name = TCHARToUTF8(name);
			error("ASIO Error: Unable to get ASIO driver description for %s, using key name instead.\n",
			      driver.name.c_str());

		} else {
			driver.name = TCHARToUTF8(value2);
		}

		info2("Found ASIO driver: %s with CLSID %s\n", driver.name.c_str(), driver.clsid.c_str());
		drivers.push_back(driver);
		deviceNames.push_back(driver.name);
	}

	info2("ASIO Info: Done querying ASIO drivers.");

	RegCloseKey(asio);
}

int ASIODeviceList::findFreeSlot()
{
	for (int i = 0; i < maxNumASIODevices; ++i)
		if (currentASIODev[i] == nullptr)
			return i;

	error("You have more than 16 asio devices, that's too many !\nShip me some...");
	return -1;
}

int ASIODeviceList::getIndexFromDeviceName(const std::string name)
{
	// need to call scanForDevices() before doing this
	if (!hasScanned || name.size() == 0)
		return -1;

	for (int i = 0; i < deviceNames.size(); i++) {
		if (deviceNames[i] == name)
			return i;
	}
	return -1;
}

ASIODevice *ASIODeviceList::attachDevice(const std::string inputDeviceName)
{
	// need to call scanForDevices() before doing this & to have a driver name !
	if (inputDeviceName.size() == 0 || !hasScanned)
		return nullptr;

	std::string deviceName = inputDeviceName;
	int index = getIndexFromDeviceName(deviceName);

	if (index >= 0) {
		int freeSlot = findFreeSlot();

		if (freeSlot >= 0) {
			// check if the device has not already been created
			for (int j = 0; j < maxNumASIODevices; j++) {
				if (currentASIODev[j]) {
					if (deviceName == currentASIODev[j]->getName())
						return currentASIODev[j];
				}
			}
			return new ASIODevice(deviceName, classIds[index], freeSlot);
		}
	}
	return nullptr;
}

