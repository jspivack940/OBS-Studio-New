/*
Copyright (C) 2018 by pkv <pkv.stream@gmail.com>, andersama <anderson.john.alexander@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* For full GPL v2 compatibility it is required to build libs with
 * our open source sdk instead of steinberg sdk , see our fork:
 * https://github.com/pkviet/portaudio , branch : openasio
 * If you build with original asio sdk, you are free to do so to the
 * extent that you do not distribute your binaries.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <JuceHeader.h>
//#include <juce_audio_processors/juce_audio_processors.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vst3", "en-US")

#define blog(level, msg, ...) blog(level, "obs-vst3: " msg, ##__VA_ARGS__)

// get number of output channels (this is set in obs general audio settings
int get_obs_output_channels()
{
	// get channel number from output speaker layout set by obs
	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	return (int)get_audio_channels(aoi.speakers);
}

int get_max_obs_channels()
{
	static int channels = 0;
	if (channels > 0) {
		return channels;
	} else {
		for (int i = 0; i < 1024; i++) {
			int c = get_audio_channels((speaker_layout)i);
			if (c > channels)
				channels = c;
		}
		return channels;
	}
}

VSTPluginFormat  vst2format;
VST3PluginFormat vst3format;

const int obs_output_frames = AUDIO_OUTPUT_FRAMES;

static FileSearchPath search = vst3format.getDefaultLocationsToSearch();
StringArray           paths;

static FileSearchPath search_2x = vst2format.getDefaultLocationsToSearch();
StringArray           paths_2x;

const volatile int obs_max_channels = get_max_obs_channels();

class VSTWindow : public DialogWindow {
	std::shared_ptr<DialogWindow> self;

public:
	VSTWindow(const String &name, Colour backgroundColour, bool escapeKeyTriggersCloseButton,
			bool addToDesktop = true)
		: DialogWindow(name, backgroundColour, escapeKeyTriggersCloseButton, addToDesktop)
	{
		self.reset(this);
		setVisible(false);
		setOpaque(true);
		setUsingNativeTitleBar(true);
	}
	~VSTWindow()
	{
	}
	std::shared_ptr<DialogWindow> get()
	{
		return self;
	}
	void closeButtonPressed()
	{
		self.reset();
	}
};

StringArray get_paths(VSTPluginFormat &f)
{
	UNUSED_PARAMETER(f);
	return paths_2x;
}

StringArray get_paths(VST3PluginFormat &f)
{
	UNUSED_PARAMETER(f);
	return paths;
}

void set_paths(VSTPluginFormat &f, StringArray p)
{
	paths_2x = p;
}

void set_paths(VST3PluginFormat &f, StringArray p)
{
	paths = p;
}

template<class pluginformat> class VSTHost : private AudioProcessorListener {
private:
	juce::AudioBuffer<float> buffer;
	juce::MidiBuffer         midi;
	juce::MidiBuffer         empty_midi;
	AudioPluginInstance *    vst_instance     = nullptr;
	AudioPluginInstance *    new_vst_instance = nullptr;
	AudioPluginInstance *    old_vst_instance = nullptr;
	PluginDescription        desc;

	obs_source_t *    context = nullptr;
	juce::MemoryBlock vst_state;
	obs_data_t *      vst_settings = nullptr;
	juce::String      current_file = "";
	juce::String      current_name = "";

	MidiMessageCollector midi_collector;
	// std::unique_ptr<MidiInput> midi_input;
	MidiInput *midi_input = nullptr;

	std::weak_ptr<DialogWindow> dialog;
	// AudioProcessorEditor *editor;
	// juce::SharedResourcePointer<AudioProcessorEditor> editor;
	// std::unique_ptr<AudioProcessorEditor> editor;

	juce::AudioProcessorParameter *param = nullptr;

	bool enabled      = true;
	bool swap         = false;
	bool updating     = false;
	bool asynchronous = true;

	std::shared_ptr<VSTHost<pluginformat>> self;
	pluginformat                           plugin_format;

	void save_state(AudioProcessor *processor)
	{
		if (!vst_settings)
			vst_settings = obs_data_create();
		if (processor) {
			processor->getStateInformation(vst_state);
			String state = vst_state.toBase64Encoding();
			obs_data_set_string(vst_settings, "state", state.toStdString().c_str());
		} else {
			obs_data_set_string(vst_settings, "state", "");
		}
	}

	void load_state(AudioProcessor *processor)
	{
		processor->setStateInformation(vst_state.getData(), vst_state.getSize());
	}

	void audioProcessorParameterChanged(AudioProcessor *processor, int parameterIndex, float newValue)
	{
		if (!vst_settings)
			vst_settings = obs_data_create();
		
		std::string idx = std::to_string(parameterIndex);
		obs_data_set_double(vst_settings, idx.c_str(), newValue);
		
		String name = "";
		if (processor) {
			name = processor->getName();
			// save_state(processor);
		}
		obs_data_set_string(vst_settings, "vst_processor", name.toStdString().c_str());
	}

	void audioProcessorChanged(AudioProcessor *processor)
	{
		if (!vst_settings)
			vst_settings = obs_data_create();

		String name = "";
		if (processor) {
			name = processor->getName();
			// save_state(processor);
		}
		obs_data_set_string(vst_settings, "vst_processor", name.toStdString().c_str());
	}

	void close_vst(AudioPluginInstance *inst)
	{
		if (inst) {
			inst->removeListener(this);
			AudioProcessorEditor *e = inst->getActiveEditor();
			if (e)
				delete e;
			inst->releaseResources();
			delete inst;
			inst = nullptr;
		}
	}

	void change_vst(AudioPluginInstance *inst, juce::String err, juce::String state, juce::String file,
			juce::String vst_processor, std::vector<std::pair<int, float>> vstsaved)
	{
		updating         = true;
		new_vst_instance = inst;
		if (err.toStdString().length() > 0) {
			blog(LOG_WARNING, "failed to load! %s", err.toStdString().c_str());
		}
		if (new_vst_instance) {
			double sample_rate = new_vst_instance->getSampleRate();
			new_vst_instance->setNonRealtime(false);
			new_vst_instance->prepareToPlay(sample_rate, 2 * obs_output_frames);
			String new_vst_processor = new_vst_instance->getName();

			juce::AudioProcessorParameter *nparam = new_vst_instance->getBypassParameter();
			if (nparam) {
				nparam->beginChangeGesture();
				nparam->setValueNotifyingHost(((float)!enabled) * 1.0f);
				nparam->endChangeGesture();
			}

			if (!vst_settings && new_vst_processor.compare(vst_processor) == 0) {
				juce::MemoryBlock m;
				m.fromBase64Encoding(state);
				new_vst_instance->setStateInformation(m.getData(), m.getSize());

				for (int i = 0; i < vstsaved.size(); i++) {
					int idx = vstsaved[i].first;
					float f = vstsaved[i].second;
					AudioProcessorParameter *param = new_vst_instance->getParameters()[idx];
					param->beginChangeGesture();
					param->setValueNotifyingHost(f);
					param->endChangeGesture();
				}

				vst_settings = obs_data_create();
			} else {
				// obs_data_clear(vst_settings);
			}
			save_state(new_vst_instance);

			new_vst_instance->addListener(this);
			current_name = new_vst_processor;
		} else {
			current_name = "";
		}
		current_file = file;
		swap         = true;
		updating     = false;
	}

	void update(obs_data_t *settings)
	{
		if (swap || updating)
			return;
		updating                                      = true;
		std::weak_ptr<VSTHost<pluginformat>> tmp_self = self;
		auto                                 tmp_keep = tmp_self.lock();
		if (!tmp_keep) {
			blog(LOG_INFO, "?");
			return;
		}

		close_vst(old_vst_instance);
		old_vst_instance = nullptr;

		obs_audio_info aoi;
		aoi.samples_per_sec = 48000;
		bool got_audio      = obs_get_audio_info(&aoi);

		juce::String file       = obs_data_get_string(settings, "effect");
		juce::String plugin     = obs_data_get_string(settings, "desc");
		juce::String mididevice = obs_data_get_string(settings, "midi");
		enabled                 = obs_data_get_bool(settings, "enable");

		if (mididevice.compare("") == 0) {
			if (midi_input) {
				midi_input->stop();
				midi_input = nullptr;
			}
		} else {
			juce::StringArray devices     = MidiInput::getDevices();
			int               deviceindex = 0;
			for (; deviceindex < devices.size(); deviceindex++) {
				if (devices[deviceindex].compare(mididevice) == 0)
					break;
			}
			MidiInput *nextdevice = MidiInput::openDevice(deviceindex, &midi_collector);
			if (midi_input) {
				midi_input->stop();
				midi_input = nullptr;
			}
			midi_input = nextdevice;
			if (midi_input)
				midi_input->start();

			midi_collector.reset((double)aoi.samples_per_sec);
		}

		juce::String err;
		auto         clear_vst = [this]() {
                        close_vst(new_vst_instance);
                        new_vst_instance = nullptr;
                        desc             = PluginDescription();
                        swap             = true;
                        updating         = false;
		};

		if (file.compare("") == 0 || plugin.compare("") == 0) {
			host_close();
			clear_vst();
		} else if (file.compare(current_file) != 0 || plugin.compare(current_name)) {
			host_close();
			juce::OwnedArray<juce::PluginDescription> descs;
			plugin_format.findAllTypesForFile(descs, file);
			if (descs.size() > 0) {
				if (true) {
					String state         = obs_data_get_string(settings, "state");
					String vst_processor = obs_data_get_string(settings, "vst_processor");
					
					std::vector<std::pair<int, float>> vstsaved;
					vstsaved.reserve(64);

					obs_data_item_t *item = NULL;
					for (item = obs_data_first(settings); item; obs_data_item_next(&item)) {
						enum obs_data_type type  = obs_data_item_gettype(item);
						std::string        iname = obs_data_item_get_name(item);

						if (!obs_data_item_has_user_value(item))
							continue;
						if (type == OBS_DATA_NUMBER) {
							enum obs_data_number_type ntype = obs_data_item_numtype(item);
							try {
								int   idx = std::stoi(iname);
								float f   = 0;
								if (ntype == OBS_DATA_NUM_DOUBLE) {
									f = obs_data_item_get_double(item);
									vstsaved.push_back({idx, f});
								} else {
									f = obs_data_item_get_int(item);
									vstsaved.push_back({idx, f});
								}
							} catch (...) {
							}
						}
					}
					obs_data_item_release(&item);
					
					auto callback = [state, tmp_self, file, vst_processor](
									AudioPluginInstance *inst,
									const juce::String & err) {
						auto myself = tmp_self.lock();
						if (myself)
							myself->change_vst(inst, err, state, file, vst_processor, {});
					};

					bool found = false;
					for (int i = 0; i < descs.size(); i++) {
						if (plugin.compare(descs[i]->name) == 0) {
							desc  = *descs[i];
							found = true;
							break;
						}
					}

					if (found) {
						if (asynchronous) {
							plugin_format.createPluginInstanceAsync(desc,
									(double)aoi.samples_per_sec,
									2 * obs_output_frames, callback);
						} else {
							String               err;
							AudioPluginInstance *inst =
									plugin_format.createInstanceFromDescription(
											desc,
											(double)aoi.samples_per_sec,
											2 * obs_output_frames, err);
							callback(inst, err);
						}
					} else {
						clear_vst();
					}
				}
			} else {
				clear_vst();
			}
		}

		juce::AudioProcessorParameter *nparam;
		if (vst_instance)
			nparam = vst_instance->getBypassParameter();
		else
			nparam = nullptr;
		if (nparam) {
			nparam->beginChangeGesture();
			nparam->setValueNotifyingHost(((float)!enabled) * 1.0f);
			nparam->endChangeGesture();
		}
	}

	void save(obs_data_t *settings)
	{
		if (vst_instance)
			save_state(vst_instance);
		if (settings && vst_settings)
			obs_data_apply(settings, vst_settings);
	}

	void filter_audio(struct obs_audio_data *audio)
	{
		// static obs_audio_info laoi = {};
		if (swap) {
			old_vst_instance = vst_instance;
			vst_instance     = new_vst_instance;
			new_vst_instance = nullptr;
			if (old_vst_instance)
				old_vst_instance->removeListener(this);
			swap = false;
		}

		/*Process w/ VST*/
		if (vst_instance) {
			int chs = 0;
			for (; chs < obs_max_channels; chs++)
				if (!audio->data[chs])
					break;

			struct obs_audio_info aoi;
			bool                  audio_info = obs_get_audio_info(&aoi);
			double                sps        = (double)aoi.samples_per_sec;
			if (audio_info)
				vst_instance->prepareToPlay(sps, audio->frames);

			midi_collector.removeNextBlockOfMessages(midi, audio->frames);
			buffer.setDataToReferTo((float **)audio->data, chs, audio->frames);
			param = vst_instance->getBypassParameter();

			bool bypass = (param && param->getValue() != 0.0f);
			if (bypass) {
				vst_instance->processBlockBypassed(buffer, midi);
			} else {
				vst_instance->processBlock(buffer, midi);
			}
			midi.clear();

			if (audio_info) {
				midi_collector.reset(sps);
				// laoi.samples_per_sec = aoi.samples_per_sec;
			}
		}
	}

public:
	std::shared_ptr<VSTHost<pluginformat>> get()
	{
		return self;
	}

	static pluginformat get_format()
	{
		return plugin_format;
	}

	VSTHost<pluginformat>(obs_data_t *settings, obs_source_t *source) : context(source)
	{
		// midi_input.openDevice(0, midi_collector.handleIncomingMidiMessage);
		// midi_input = new MidiInput("")
		self.reset(this);
		update(settings);
	}

	~VSTHost<pluginformat>()
	{
		if (midi_input) {
			midi_input->stop();
			delete midi_input;
			midi_input = nullptr;
		}

		obs_data_release(vst_settings);
		host_close();
		close_vst(old_vst_instance);
		close_vst(vst_instance);
		close_vst(new_vst_instance);
	}

	void close_vsts()
	{
		// close_vst(old_vst_instance);
		// close_vst(vst_instance);
	}

	bool host_is_named(juce::String processor)
	{
		auto d = dialog.lock();
		if (d) {
			juce::String n = d->getName();
			return n.compare(processor) == 0;
		}
		return false;
	}

	bool current_file_is(juce::String file)
	{
		return file.compare(current_file) == 0;
	}

	bool old_host()
	{
		if (vst_instance) {
			auto d = dialog.lock();
			if (d)
				return current_name.compare(d->getName()) != 0;
		} else {
			return current_name.compare("") != 0;
		}
	}

	void host_clicked()
	{
		if (has_gui()) {
			if (!host_open()) {
				VSTWindow *w = new VSTWindow(desc.name, juce::Colour(255, 255, 255), false, false);
				dialog       = w->get();
			}
			auto d = dialog.lock();
			if (d) {
				AudioProcessorEditor *e = vst_instance->getActiveEditor();
				if (e) {
					e->setOpaque(true);
					e->setVisible(true);
					d->setContentNonOwned(e, true);
					host_show();
					/*
					editor->setOpaque(true);
					if (!editor->isVisible())
						editor->setVisible(true);
						*/
				} else {
					e = vst_instance->createEditor();
					if (e) {
						e->setOpaque(true);
						e->setVisible(true);
						d->setContentNonOwned(e, true);
						host_show();
					}
				}
			}
		}
	}

	void host_show()
	{
		auto d = dialog.lock();
		if (d) {
			void *h = obs_frontend_get_main_window_handle();
			d->setOpaque(true);
			d->setName(current_name);
			if (!d->isOnDesktop()) {
				int f = d->getDesktopWindowStyleFlags();
				f |= (ComponentPeer::StyleFlags::windowHasCloseButton |
						ComponentPeer::StyleFlags::windowHasTitleBar |
						ComponentPeer::StyleFlags::windowIsResizable);
				d->addToDesktop(f, h);
				d->setTopLeftPosition(40, 40);
			}
			if (!d->isVisible())
				d->setVisible(true);
		}
	}

	void host_close()
	{
		auto d = dialog.lock();
		if (d)
			d->closeButtonPressed();
		dialog.reset();
	}

	bool has_gui()
	{
		return vst_instance && vst_instance->hasEditor();
	}

	bool host_open()
	{
		return dialog.use_count();
	}

	static bool vst_host_clicked(obs_properties_t *props, obs_property_t *property, void *vptr)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		// obs_property_t *effect = obs_properties_get(props, "effect");
		if (plugin) {
			if (plugin->old_host()) {
				plugin->host_close();
			}
			plugin->host_clicked();
		}
		return false;
	}

	bool midi_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
	{
		obs_property_list_clear(prop);
		juce::StringArray devices = MidiInput::getDevices();
		obs_property_list_add_string(prop, "", "");
		for (int i = 0; i < devices.size(); i++)
			obs_property_list_add_string(prop, devices[i].toRawUTF8(), devices[i].toRawUTF8());
		return false;
	}

	static bool midi_selected_modified(
			void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		if (plugin)
			return plugin->midi_modified(props, property, settings);

		return false;
	}

	static bool vst_selected_modified(
			void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
	{
		static pluginformat f;

		VSTHost<pluginformat> *plugin          = static_cast<VSTHost<pluginformat> *>(vptr);
		obs_property_t *       vst_host_button = obs_properties_get(props, "vst_button");
		obs_property_t *       desc_list       = obs_properties_get(props, "desc");
		juce::String           file            = obs_data_get_string(settings, "effect");
		if (plugin && (!plugin->current_file_is(file) || plugin->old_host()))
			plugin->host_close();

		obs_property_list_clear(desc_list);

		juce::OwnedArray<juce::PluginDescription> descs;
		f.findAllTypesForFile(descs, file);
		bool has_options = descs.size() > 1;
		if (has_options)
			obs_property_list_add_string(desc_list, "", "");

		for (int i = 0; i < descs.size(); i++) {
			std::string n = descs[i]->name.toStdString();
			obs_property_list_add_string(desc_list, n.c_str(), n.c_str());
		}

		obs_property_set_enabled(desc_list, has_options);

		return true;
	}

	static obs_properties_t *Properties(void *vptr)
	{
		static pluginformat    f;
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);

		obs_properties_t *props;
		props = obs_properties_create();

		obs_property_t *vst_list;
		obs_property_t *desc_list;
		obs_property_t *midi_list;

		obs_property_t *vst_host_button;
		obs_property_t *bypass;
		vst_list = obs_properties_add_list(
				props, "effect", "File", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		desc_list = obs_properties_add_list(
				props, "desc", "Plugin", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback2(vst_list, vst_selected_modified, plugin);

		midi_list = obs_properties_add_list(
				props, "midi", "Midi Input", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback2(midi_list, midi_selected_modified, plugin);

		vst_host_button = obs_properties_add_button2(props, "vst_button", "Show", vst_host_clicked, plugin);

		obs_properties_add_bool(props, "enable", "enable effect");

		obs_property_list_add_string(vst_list, "", "");
		/*Add VSTs to list*/
		bool scannable = f.canScanForPlugins();
		if (scannable) {
			StringArray p = get_paths(f);
			if (p.size() < 1) {
				p = f.searchPathsForPlugins(search, true, true);
				set_paths(f, p);
			}
			for (int i = 0; i < p.size(); i++) {
				juce::String name = f.getNameOfPluginFromIdentifier(p[i]);
				obs_property_list_add_string(
						vst_list, p[i].toStdString().c_str(), name.toStdString().c_str());
			}
		}

		return props;
	}

	static void Update(void *vptr, obs_data_t *settings)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		if (plugin)
			plugin->update(settings);
	}

	static void Defaults(obs_data_t *settings)
	{
		/*Setup Defaults*/
		obs_data_set_default_string(settings, "effect", "");
		obs_data_set_default_double(settings, "enable", true);
	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		static std::string type_name = std::string("VSTPlugin.") + std::string(typeid(pluginformat).name());
		return obs_module_text(type_name.c_str());
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		return new VSTHost<pluginformat>(settings, source);
	}

	static void Save(void *vptr, obs_data_t *settings)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		if (plugin)
			plugin->save(settings);
	}

	void destroy()
	{
		self.reset();
	}

	static void Destroy(void *vptr)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		if (plugin) {
			plugin->close_vsts();
			plugin->destroy();
		}
		plugin = nullptr;
	}

	static struct obs_audio_data *Filter_Audio(void *vptr, struct obs_audio_data *audio)
	{
		VSTHost<pluginformat> *plugin = static_cast<VSTHost<pluginformat> *>(vptr);
		plugin->filter_audio(audio);

		return audio;
	}
};

static void free_type_data(void *type_data)
{
	type_data = 0;
}

bool obs_module_load(void)
{
	MessageManager::getInstance();

	struct obs_source_info vst3_filter = {};
	vst3_filter.id                     = "vst_filter_juce_3x";
	vst3_filter.type                   = OBS_SOURCE_TYPE_FILTER;
	vst3_filter.output_flags           = OBS_SOURCE_AUDIO;
	vst3_filter.get_name               = VSTHost<VST3PluginFormat>::Name;
	vst3_filter.create                 = VSTHost<VST3PluginFormat>::Create;
	vst3_filter.destroy                = VSTHost<VST3PluginFormat>::Destroy;
	vst3_filter.update                 = VSTHost<VST3PluginFormat>::Update;
	vst3_filter.filter_audio           = VSTHost<VST3PluginFormat>::Filter_Audio;
	vst3_filter.get_properties         = VSTHost<VST3PluginFormat>::Properties;
	vst3_filter.save                   = VSTHost<VST3PluginFormat>::Save;
	vst3_filter.type_data              = (void *)true;
	vst3_filter.free_type_data         = free_type_data;

	struct obs_source_info vst_filter = {};
	vst_filter.id                     = "vst_filter_juce_2x";
	vst_filter.type                   = OBS_SOURCE_TYPE_FILTER;
	vst_filter.output_flags           = OBS_SOURCE_AUDIO;
	vst_filter.get_name               = VSTHost<VSTPluginFormat>::Name;
	vst_filter.create                 = VSTHost<VSTPluginFormat>::Create;
	vst_filter.destroy                = VSTHost<VSTPluginFormat>::Destroy;
	vst_filter.update                 = VSTHost<VSTPluginFormat>::Update;
	vst_filter.filter_audio           = VSTHost<VSTPluginFormat>::Filter_Audio;
	vst_filter.get_properties         = VSTHost<VSTPluginFormat>::Properties;
	vst_filter.save                   = VSTHost<VSTPluginFormat>::Save;
	vst_filter.type_data              = (void *)true;
	vst_filter.free_type_data         = free_type_data;

	int version = (JUCE_MAJOR_VERSION << 16) | (JUCE_MINOR_VERSION << 8) | JUCE_BUILDNUMBER;
	blog(LOG_INFO, "JUCE Version: (%i) %i.%i.%i", version, JUCE_MAJOR_VERSION, JUCE_MINOR_VERSION,
			JUCE_BUILDNUMBER);

	obs_register_source(&vst3_filter);
	obs_register_source(&vst_filter);
	//#define DEBUG_JUCE_VST 1
	if (vst3_filter.type_data) {
		auto rescan_vst3 = [](void * = nullptr) {
			if (vst3format.canScanForPlugins())
				paths = vst3format.searchPathsForPlugins(search, true, true);
		};
		obs_frontend_add_tools_menu_item("Rescan VST3", rescan_vst3, nullptr);
		rescan_vst3();
#if DEBUG_JUCE_VST
		for (int i = 0; i < paths.size(); i++) {
			juce::OwnedArray<juce::PluginDescription> descs;
			vst3format.findAllTypesForFile(descs, paths[i]);
			for (int j = 0; j < descs.size(); j++) {
				std::string n = descs[j]->name.toStdString();
				blog(LOG_INFO, "[%i][%i]: %s", i, j, n.c_str());
			}
		}
#endif
	}

	if (vst_filter.type_data) {
		auto rescan_vst2 = [](void * = nullptr) {
			if (vst2format.canScanForPlugins())
				paths_2x = vst2format.searchPathsForPlugins(search_2x, true, true);
		};
		obs_frontend_add_tools_menu_item("Rescan VST", rescan_vst2, nullptr);
		rescan_vst2();
#if DEBUG_JUCE_VST
		for (int i = 0; i < paths_2x.size(); i++) {
			juce::OwnedArray<juce::PluginDescription> descs;
			vst2format.findAllTypesForFile(descs, paths[i]);
			for (int j = 0; j < descs.size(); j++) {
				std::string n = descs[j]->name.toStdString();
				blog(LOG_INFO, "[%i][%i]: %s", i, j, n.c_str());
			}
		}
#endif
	}

	return vst3_filter.type_data || vst_filter.type_data;
}
