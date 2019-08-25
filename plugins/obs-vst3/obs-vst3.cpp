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

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <vector>
#include <stdio.h>
#include <JuceHeader.h>
#include <juce_audio_processors/juce_audio_processors.h>

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

VST3PluginFormat     vst3format;
const int            obs_output_frames = AUDIO_OUTPUT_FRAMES;
const FileSearchPath search            = vst3format.getDefaultLocationsToSearch();
StringArray          paths             = vst3format.searchPathsForPlugins(search, true, true);
const volatile int   obs_max_channels  = get_max_obs_channels();

class VSTWindow : public DialogWindow {
	std::shared_ptr<DialogWindow> self;

public:
	VSTWindow(const String &name, Colour backgroundColour, bool escapeKeyTriggersCloseButton,
			bool addToDesktop = true)
		: DialogWindow(name, backgroundColour, escapeKeyTriggersCloseButton, addToDesktop)
	{
		self.reset(this);
		// self = std::make_shared<DialogWindow>(static_cast<DialogWindow*>(this));
	}
	~VSTWindow()
	{
		self.~shared_ptr();
	}

	std::shared_ptr<DialogWindow> get()
	{
		return self;
	}
	void closeButtonPressed()
	{
		self.~shared_ptr();
		// self.reset(nullptr);
		// delete this;
	}
};

class VST3Host : private AudioProcessorListener {
private:
	PluginDescription     desc;
	AudioPluginInstance * vst_instance     = nullptr;
	AudioPluginInstance * new_vst_instance = nullptr;
	AudioPluginInstance * old_vst_instance = nullptr;
	
	obs_source_t *        context          = nullptr;
	juce::MemoryBlock     vst_state;
	obs_data_t *          vst_settings = nullptr;
	juce::String          current_file = "";

	// DialogWindow *dialog = nullptr;
	std::weak_ptr<DialogWindow> dialog;
	std::unique_ptr<AudioProcessorEditor> editor;

	juce::AudioProcessorParameter *param = nullptr;

	juce::MidiBuffer         midi;
	juce::AudioBuffer<float> buffer;

	bool enabled = true;
	bool swap    = false;

	void audioProcessorParameterChanged(AudioProcessor *processor, int parameterIndex, float newValue)
	{
		std::string idx = std::to_string(parameterIndex);
		if (!vst_settings)
			vst_settings = obs_data_create();

		obs_data_set_double(vst_settings, idx.c_str(), newValue);
		String name = "";
		if (processor) {
			name = processor->getName();
			obs_data_set_string(vst_settings, "vst_processor", name.toStdString().c_str());
		}
		/*
		processor->getStateInformation(vst_state);
		String state = vst_state.toBase64Encoding();
		obs_data_set_string(vst_settings, "state", state.toStdString().c_str());
		*/
	}

	void audioProcessorChanged(AudioProcessor *processor)
	{
		if (!vst_settings)
			vst_settings = obs_data_create();
		String name = "";
		if (processor) {
			name = processor->getName();
			obs_data_set_string(vst_settings, "vst_processor", name.toStdString().c_str());
		}
		/*
		processor->getStateInformation(vst_state);
		String state = vst_state.toBase64Encoding();
		obs_data_set_string(vst_settings, "state", state.toStdString().c_str());
		*/
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

	void update(obs_data_t *settings)
	{
		if (swap)
			return;
		close_vst(old_vst_instance);
		old_vst_instance = nullptr;

		obs_audio_info aoi;
		juce::String   file = obs_data_get_string(settings, "effect");
		enabled             = obs_data_get_bool(settings, "enable");

		juce::String err;
		if (file.compare(current_file) != 0 && file.compare("None") != 0) {
			host_close();
			juce::OwnedArray<juce::PluginDescription> descs;
			vst3format.findAllTypesForFile(descs, file);
			if (descs.size() > 0) {
				blog(LOG_INFO, "%s", descs[0]->name.toStdString().c_str());
				desc = *descs[0];

				if (obs_get_audio_info(&aoi)) {
					String state = obs_data_get_string(settings, "state");
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

					auto callback = [state, this, &aoi, file, vst_processor, vstsaved](AudioPluginInstance *inst,
									const juce::String &                     err) {
						new_vst_instance = inst;
						if (err.toStdString().length() > 0) {
							blog(LOG_WARNING, "failed to load! %s",
									err.toStdString().c_str());
						}
						if (new_vst_instance) {
							new_vst_instance->setNonRealtime(false);
							new_vst_instance->prepareToPlay((double)aoi.samples_per_sec,
									2 * obs_output_frames);
							String new_vst_processor = new_vst_instance->getName();

							juce::AudioProcessorParameter *nparam =
									new_vst_instance->getBypassParameter();
							if (nparam) {
								nparam->beginChangeGesture();
								nparam->setValueNotifyingHost(((float)!enabled) * 1.0f);
								nparam->endChangeGesture();
							}

							new_vst_instance->addListener(this);

							if (!vst_settings &&
									new_vst_processor.compare(vst_processor) == 0) {
								/*
								juce::MemoryBlock m;
								m.fromBase64Encoding(state);
								new_vst_instance->setStateInformation(
										m.getData(), m.getSize());
								*/
								for (int i = 0; i < vstsaved.size(); i++) {
									int   p = vstsaved[i].first;
									float v = vstsaved[i].second;
									new_vst_instance->beginParameterChangeGesture(
											p);

									new_vst_instance->setParameter(p, v);
									new_vst_instance->endParameterChangeGesture(p);
								}
								vst_settings = obs_data_create();
							} else {
								// obs_data_clear(vst_settings);
							}

						}
						current_file = file;
						swap         = true;
					};
					vst3format.createPluginInstanceAsync(desc, (double)aoi.samples_per_sec,
							2 * obs_output_frames, callback);
				}
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
		if (settings && vst_settings)
			obs_data_apply(settings, vst_settings);
		// obs_data_set_string(settings, "state", obs_data_get_string(vst_settings, "state"));
	}

	void filter_audio(struct obs_audio_data *audio)
	{
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
			if (obs_get_audio_info(&aoi))
				vst_instance->prepareToPlay((double)aoi.samples_per_sec, audio->frames);

			buffer.setDataToReferTo((float **)audio->data, chs, audio->frames);
			param       = vst_instance->getBypassParameter();
			bool bypass = (param && param->getValue() != 0.0f);
			if (bypass) {
				vst_instance->processBlockBypassed(buffer, midi);
			} else {
				vst_instance->processBlock(buffer, midi);
			}
		}
	}

public:
	VST3Host(obs_data_t *settings, obs_source_t *source) : context(source)
	{
		update(settings);
	}

	~VST3Host()
	{
		obs_data_release(vst_settings);
		host_close();
		close_vst(old_vst_instance);
		close_vst(vst_instance);
		close_vst(new_vst_instance);
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

	void host_clicked()
	{
		if (has_gui()) {
			if (dialog.expired()) {
				VSTWindow *w = new VSTWindow(desc.name, juce::Colour(255, 255, 255), false, false);
				dialog       = w->get();
				editor.reset(vst_instance->createEditorIfNeeded());

				auto d = dialog.lock();
				if (d && editor) {
					// d->setContentNonOwned(editor, true);
					d->setContentNonOwned(editor.get(), true);
					host_show();
					editor->setOpaque(true);
					if (!editor->isVisible())
						editor->setVisible(true);
				}
			} else {
				auto d = dialog.lock();
				if (!editor)
					editor.reset(vst_instance->createEditorIfNeeded());
				if (d && editor) {
					// d->setContentNonOwned(editor, true);
					d->setContentNonOwned(editor.get(), true);
					host_show();
					editor->setOpaque(true);
					if (!editor->isVisible())
						editor->setVisible(true);
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
			/*
			(ComponentPeer::StyleFlags::windowHasCloseButton |
						     ComponentPeer::StyleFlags::windowHasTitleBar |
						     ComponentPeer::StyleFlags::windowHasMinimiseButton)
			*/
			if (!d->isOnDesktop()) {
				int f = d->getDesktopWindowStyleFlags();
				f |= (ComponentPeer::StyleFlags::windowHasCloseButton |
						ComponentPeer::StyleFlags::windowHasTitleBar |
						ComponentPeer::StyleFlags::windowIsResizable);
				d->addToDesktop(f, h);
			}
			if (!d->isVisible())
				d->setVisible(true);
		}
	}

	void host_hide()
	{
		auto d = dialog.lock();
		if (d)
			d->removeFromDesktop();
	}

	void host_close()
	{
		if(!dialog.expired()) {
			auto d = dialog.lock();
			d->closeButtonPressed();
			if (!dialog.expired())
				dialog.reset();
		}
		editor.reset();
		/*
		if (editor) {
			delete editor;
			editor = nullptr;
		}
		*/
	}

	bool has_gui()
	{
		return !swap && vst_instance && vst_instance->hasEditor();
	}

	bool host_open()
	{
		return !dialog.expired();
	}

	static bool vst_host_clicked(obs_properties_t *props, obs_property_t *property, void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		bool      s      = true;
		if (plugin) {
			s = !plugin->host_open();
			if (s) {
				plugin->host_clicked();
			} else {
				plugin->host_close();
			}
		}
		obs_property_set_description(property, s ? "Hide" : "Show");
		return true;
	}

	static bool vst_selected_modified(
			void *vptr, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
	{
		VST3Host *      plugin          = static_cast<VST3Host *>(vptr);
		obs_property_t *vst_host_button = obs_properties_get(props, "vst_button");
		juce::String    file            = obs_data_get_string(settings, "effect");
		if (plugin && !plugin->current_file_is(file))
			plugin->host_close();
		obs_property_set_enabled(vst_host_button, plugin && plugin->has_gui());
		return true;
	}

	static obs_properties_t *Properties(void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);

		obs_properties_t *props;
		props = obs_properties_create();

		obs_property_t *vst_list;
		obs_property_t *vst_host_button;
		obs_property_t *bypass;
		vst_list = obs_properties_add_list(
				props, "effect", "vsts", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_set_modified_callback2(vst_list, vst_selected_modified, plugin);

		vst_host_button = obs_properties_add_button2(props, "vst_button", "Show", vst_host_clicked, plugin);
		// obs_property_set_enabled(vst_host_button, plugin->has_gui());
		obs_properties_add_bool(props, "enable", "enable effect");

		/*Add VSTs to list*/
		bool scannable = vst3format.canScanForPlugins();
		if (scannable) {
			if (paths.size() < 1)
				paths = vst3format.searchPathsForPlugins(search, true, true);
			for (int i = 0; i < paths.size(); i++) {
				juce::String name = vst3format.getNameOfPluginFromIdentifier(paths[i]);
				obs_property_list_add_string(
						vst_list, paths[i].toStdString().c_str(), name.toStdString().c_str());
			}
		}

		return props;
	}

	static void Update(void *vptr, obs_data_t *settings)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		if (plugin)
			plugin->update(settings);
	}

	static void Defaults(obs_data_t *settings)
	{
		/*Setup Defaults*/
		obs_data_set_default_string(settings, "effect", "None");
		obs_data_set_default_double(settings, "enable", true);
	}

	static const char *Name(void *unused)
	{
		UNUSED_PARAMETER(unused);
		return obs_module_text("Vst3Plugin");
	}

	static void *Create(obs_data_t *settings, obs_source_t *source)
	{
		return new VST3Host(settings, source);
	}

	static void Save(void *vptr, obs_data_t *settings)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		if (plugin)
			plugin->save(settings);
	}

	static void Destroy(void *vptr)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
		delete plugin;
		plugin = nullptr;
	}

	static struct obs_audio_data *Filter_Audio(void *vptr, struct obs_audio_data *audio)
	{
		VST3Host *plugin = static_cast<VST3Host *>(vptr);
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
	vst3_filter.id                     = "vst_filter3";
	vst3_filter.type                   = OBS_SOURCE_TYPE_FILTER;
	vst3_filter.output_flags           = OBS_SOURCE_AUDIO;
	vst3_filter.get_name               = VST3Host::Name;
	vst3_filter.create                 = VST3Host::Create;
	vst3_filter.destroy                = VST3Host::Destroy;
	vst3_filter.update                 = VST3Host::Update;
	vst3_filter.filter_audio           = VST3Host::Filter_Audio;
	vst3_filter.get_properties         = VST3Host::Properties;
	vst3_filter.save                   = VST3Host::Save;
	vst3_filter.type_data              = (void *)true;
	vst3_filter.free_type_data         = free_type_data;

	int version = (JUCE_MAJOR_VERSION << 16) | (JUCE_MINOR_VERSION << 8) | JUCE_BUILDNUMBER;
	blog(LOG_INFO, "JUCE Version: (%i) %i.%i.%i", version, JUCE_MAJOR_VERSION, JUCE_MINOR_VERSION,
			JUCE_BUILDNUMBER);

	obs_register_source(&vst3_filter);

	if (vst3_filter.type_data) {
		auto rescan = [](void *) {
			if (vst3format.canScanForPlugins())
				paths = vst3format.searchPathsForPlugins(search, true, true);
		};
		obs_frontend_add_tools_menu_item("Rescan", rescan, nullptr);
	}

	return vst3_filter.type_data;
}
