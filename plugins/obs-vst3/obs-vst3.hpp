#pragma once
#include <JuceHeader.h>

static Image windowIcon = Image();

template<class pluginformat> class VSTWindow;
template<class pluginformat> class PluginHost;

template<class pluginformat>
class VSTWindow : public DialogWindow, public std::enable_shared_from_this<VSTWindow<pluginformat>> {
	std::weak_ptr<PluginHost<pluginformat>> _owner;

public:
	VSTWindow(const String &name, Colour backgroundColour, bool escapeKeyTriggersCloseButton,
			PluginHost<pluginformat> *owner, bool addToDesktop = false)
		: DialogWindow(name, backgroundColour, escapeKeyTriggersCloseButton, addToDesktop)
	{
		_owner = owner->shared_from_this();
		setIcon(windowIcon);
		setVisible(false);
		setOpaque(true);
		setUsingNativeTitleBar(true);
	}
	~VSTWindow()
	{
	}
	std::shared_ptr<DialogWindow> get()
	{
		return shared_from_this();
	}
	void closeButtonPressed()
	{
		auto o = _owner.lock();
		if (o)
			o->host_close();
		else
			delete this;
	}
};

template<class pluginformat> using SharedPluginHost = std::shared_ptr<PluginHost<pluginformat>>;

template<class pluginformat>
class PluginHost : private AudioProcessorListener, public std::enable_shared_from_this<PluginHost<pluginformat>> {
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
	MidiInput *          midi_input = nullptr;

	// std::weak_ptr<DialogWindow> dialog;
	std::shared_ptr<VSTWindow<pluginformat>> dialog;

	juce::AudioProcessorParameter *param = nullptr;

	bool swap         = false;
	bool updating     = false;
	bool asynchronous = true;

	// std::shared_ptr<PluginHost<pluginformat>> self;
	pluginformat plugin_format;

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

	void set_enabled(AudioProcessor *processor, bool value)
	{
		juce::AudioProcessorParameter *nparam = processor->getBypassParameter();
		if (nparam) {
			nparam->beginChangeGesture();
			nparam->setValueNotifyingHost(((float)!value) * 1.0f);
			nparam->endChangeGesture();
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

			if (!vst_settings && new_vst_processor.compare(vst_processor) == 0) {
				juce::MemoryBlock m;
				m.fromBase64Encoding(state);
				new_vst_instance->setStateInformation(m.getData(), m.getSize());
				new_vst_instance->refreshParameterList();
				int size = new_vst_instance->getParameters().size();
				for (int i = 0; i < vstsaved.size(); i++) {
					int   idx = vstsaved[i].first;
					float f   = vstsaved[i].second;
					if (idx < size) {
						AudioProcessorParameter *param = new_vst_instance->getParameters()[idx];
						param->beginChangeGesture();
						param->setValueNotifyingHost(f);
						param->endChangeGesture();
					}
				}

				vst_settings = obs_data_create();
			} else {
				// obs_data_clear(vst_settings);
			}

			/*
			juce::AudioProcessorParameter *nparam = new_vst_instance->getBypassParameter();
			if (nparam) {
				nparam->beginChangeGesture();
				nparam->setValueNotifyingHost(((float)!enabled) * 1.0f);
				nparam->endChangeGesture();
			}
			*/
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
		updating                                         = true;
		std::weak_ptr<PluginHost<pluginformat>> tmp_self = weak_from_this();
		/*
		std::weak_ptr<PluginHost<pluginformat>> tmp_self = self;
		auto                                    tmp_keep = tmp_self.lock();
		if (!tmp_keep) {
			blog(LOG_INFO, "?");
			return;
		}
		*/

		close_vst(old_vst_instance);
		old_vst_instance = nullptr;

		obs_audio_info aoi;
		aoi.samples_per_sec = 48000;
		bool got_audio      = obs_get_audio_info(&aoi);

		juce::String file       = obs_data_get_string(settings, "effect");
		juce::String plugin     = obs_data_get_string(settings, "desc");
		juce::String mididevice = obs_data_get_string(settings, "midi");
		//		enabled = obs_data_get_bool(settings, "enable");

		if (mididevice.compare("") == 0) {
			if (midi_input) {
				midi_input->stop();
				delete midi_input;
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
				delete midi_input;
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

					std::vector<std::pair<int, float>> vst_saved;
					vst_saved.reserve(64);

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
									vst_saved.push_back({idx, f});
								} else {
									f = (float)obs_data_item_get_int(item);
									vst_saved.push_back({idx, f});
								}
							} catch (...) {
							}
						}
					}

					auto callback = [state, tmp_self, file, vst_processor, vst_saved](
									AudioPluginInstance *inst,
									const juce::String & err) {
						auto myself = tmp_self.lock();
						if (myself)
							myself->change_vst(inst, err, state, file, vst_processor,
									vst_saved);
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
		/*
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
		*/
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
	std::shared_ptr<PluginHost<pluginformat>> get()
	{
		return shared_from_this(); // self;
	}

	static pluginformat get_format()
	{
		return plugin_format;
	}

	PluginHost<pluginformat>(obs_data_t *settings, obs_source_t *source) : context(source)
	{
		// midi_input.openDevice(0, midi_collector.handleIncomingMidiMessage);
		// midi_input = new MidiInput("")
		// self.reset(this);
		update(settings);
	}

	~PluginHost<pluginformat>()
	{
		/*
		if (midi_input) {
			midi_input->stop();
			delete midi_input;
			midi_input = nullptr;
		}
		*/
		// obs_data_release(vst_settings);
		// host_close();
		/*
		if (old_vst_instance)
			delete old_vst_instance;
		if (vst_instance) {
			//delete vst_instance->getActiveEditor();
			//vst_instance->releaseResources();
			//delete vst_instance;
		}
		if (new_vst_instance)
			delete new_vst_instance;
		*/
	}

	void close_vsts()
	{
		// close_vst(old_vst_instance);
		// close_vst(vst_instance);
	}

	bool host_is_named(juce::String processor)
	{
		auto d = dialog; // dialog.lock();
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
			auto d = dialog; // dialog.lock();
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
				dialog = std::make_shared<VSTWindow<pluginformat>>(
						desc.name, juce::Colour(255, 255, 255), false, this);
				/*
				VSTWindow *w = new VSTWindow(desc.name, juce::Colour(255, 255, 255), false, false);
				dialog       = w->get();
				*/
			}
			if (dialog) {
				AudioProcessorEditor *e = vst_instance->getActiveEditor();
				if (e) {
					e->setOpaque(true);
					e->setVisible(true);
					dialog->setContentNonOwned(e, true);
					host_show();
				} else {
					e = vst_instance->createEditor();
					if (e) {
						e->setOpaque(true);
						e->setVisible(true);
						dialog->setContentNonOwned(e, true);
						host_show();
					}
				}
			}
			/*
			auto d = dialog.lock();
			if (d) {
				AudioProcessorEditor *e = vst_instance->getActiveEditor();
				if (e) {
					e->setOpaque(true);
					e->setVisible(true);
					d->setContentNonOwned(e, true);
					host_show();
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
			*/
		}
	}

	void host_show()
	{
		auto d = dialog; // dialog.lock();
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
		dialog.reset();
		/*
		auto d = dialog.lock();
		if (d)
			d->closeButtonPressed();
		dialog.reset();
		*/
	}

	void host_window_close()
	{
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
		// PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);
		// obs_property_t *effect = obs_properties_get(props, "effect");
		std::shared_ptr<PluginHost<pluginformat>> *plugin =
				static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
		if (plugin) {
			auto tmp = *plugin;
			if (tmp) {
				if (tmp->old_host())
					tmp->host_close();
				tmp->host_clicked();
			}
		}
		return false;
		/*
					if (plugin) {
				std::shared_ptr<PluginHost<pluginformat>> self = plugin->get();
				if (self->old_host())
					self->host_close();
				self->host_clicked();
			}
		*/
	}


		static bool midi_selected_modified(
				void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
		{
			obs_property_list_clear(property);
			juce::StringArray devices = MidiInput::getDevices();
			obs_property_list_add_string(property, "", "");
			for (int i = 0; i < devices.size(); i++)
				obs_property_list_add_string(property, devices[i].toRawUTF8(), devices[i].toRawUTF8());

			return false;
		}

		static bool vst_selected_modified(
				void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
		{
			static pluginformat f;

			// PluginHost<pluginformat> *plugin          = static_cast<PluginHost<pluginformat> *>(vptr);
			obs_property_t *vst_host_button = obs_properties_get(props, "vst_button");
			obs_property_t *desc_list       = obs_properties_get(props, "desc");
			juce::String    file            = obs_data_get_string(settings, "effect");
			/*
			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
			if (plugin && plugin->use_count()) {
				auto tmp = *plugin;
				if (tmp && (!tmp->current_file_is(file) || tmp->old_host())) {
					tmp->host_close();
				}
			}
			*/
			/*
			if (plugin) {
				std::shared_ptr<PluginHost<pluginformat>> self = plugin->get();
				if (self && (!self->current_file_is(file) || self->old_host()))
					self->host_close();
			}
			*/

			/*
			if (plugin && (!plugin->current_file_is(file) || plugin->old_host()))
				plugin->host_close();
			*/
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
			static pluginformat f;
			// PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);

			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);

			obs_properties_t *props;
			props = obs_properties_create();

			obs_property_t *vst_list;
			obs_property_t *desc_list;
			obs_property_t *midi_list;

			obs_property_t *vst_host_button;
			obs_property_t *bypass;
			vst_list  = obs_properties_add_list(props, "effect", obs_module_text("File"),
                                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
			desc_list = obs_properties_add_list(props, "desc", obs_module_text("Plugin"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
			obs_property_set_modified_callback2(vst_list, vst_selected_modified, nullptr);

			midi_list = obs_properties_add_list(props, "midi", obs_module_text("Midi"), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
			obs_property_set_modified_callback2(midi_list, midi_selected_modified, nullptr);

			vst_host_button = obs_properties_add_button2(
					props, "vst_button", obs_module_text("Show"), vst_host_clicked, plugin);

			// obs_properties_add_bool(props, "enable", "enable effect");

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
					obs_property_list_add_string(vst_list, p[i].toStdString().c_str(),
							name.toStdString().c_str());
				}
			}

			return props;
		}

		static void Update(void *vptr, obs_data_t *settings)
		{
			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
			if (plugin) {
				auto tmp = *plugin;
				if (tmp)
					tmp->update(settings);
			}
			/*
			PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);
			if (plugin) {
				std::shared_ptr<PluginHost<pluginformat>> self = plugin->get();
				self->update(settings);
			}
			*/
		}

		static void Defaults(obs_data_t * settings)
		{
			/*Setup Defaults*/
			obs_data_set_default_string(settings, "effect", "");
		}

		static const char *Name(void *unused)
		{
			UNUSED_PARAMETER(unused);
			static pluginformat f;
			static std::string  type_name = std::string("VSTPlugin.") + f.getName().toStdString();
			// std::string(typeid(PluginHost<pluginformat>).name());
			return obs_module_text(type_name.c_str());
		}

		static void *Create(obs_data_t * settings, obs_source_t * source)
		{
			std::shared_ptr<PluginHost<pluginformat>> *ptr = new std::shared_ptr<PluginHost<pluginformat>>;
			*ptr = std::make_shared<PluginHost<pluginformat>>(settings, source);
			return ptr;
			// new PluginHost<pluginformat>(settings, source);
		}

		static void Save(void *vptr, obs_data_t *settings)
		{
			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
			if (plugin) {
				auto tmp = *plugin;
				if (tmp)
					tmp->save(settings);
			}
			// PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);
			/*
			if (plugin) {
				std::shared_ptr<PluginHost<pluginformat>> self = plugin->get();
				self->save(settings);
			}
			*/
		}

		void destroy()
		{
			self.reset();
		}

		static void Destroy(void *vptr)
		{
			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
			if (plugin)
				plugin->reset();
			delete plugin;
			/*
			PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);
			if (plugin) {
				delete plugin;
				std::shared_ptr<PluginHost<pluginformat>> self = plugin->get();
				self->close_vsts();
				self->destroy();
				
			}
			*/
			plugin = nullptr;
		}

		static struct obs_audio_data *Filter_Audio(void *vptr, struct obs_audio_data *audio)
		{
			std::shared_ptr<PluginHost<pluginformat>> *plugin =
					static_cast<std::shared_ptr<PluginHost<pluginformat>> *>(vptr);
			if (plugin && plugin->use_count()) {
				plugin->get()->filter_audio(audio);
				/*
				auto tmp = *plugin;
				if (tmp) {
					tmp->filter_audio(audio);
				}
				*/
			}
			/*
			PluginHost<pluginformat> *plugin = static_cast<PluginHost<pluginformat> *>(vptr);
			plugin->filter_audio(audio);
			*/
			return audio;
		}
	};
