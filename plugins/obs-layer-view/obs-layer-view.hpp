#include <obs.hpp>
#include <obs-module.h>
#include <util/darray.h>
#include <obs-frontend-api.h>

#include <QWidget>
#include <QDockWidget>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialog>
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QCursor>

struct obs_layeritem_t {
	DARRAY(obs_sceneitem_t*) items;
};

struct obs_layers_t {
	DARRAY(obs_layeritem_t*) layers;
};

static void saveload_callback(obs_data_t *save_data, bool saving, void *vptr);
static void signal_callback_add(void *vptr, calldata_t *cbdata);
static void signal_callback_remove(void *vptr, calldata_t *cbdata);
static bool listSceneItems(obs_scene_t *scene, obs_sceneitem_t *item, void *vptr);

const char *sceneitem_get_name(obs_sceneitem_t *item)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	return obs_source_get_name(source);
}

class OBSLayerView : public QDockWidget {
private:
	QWidget *_customgui = nullptr;
	obs_source_t *_currentSceneSource = nullptr;
	obs_scene_t *_currentScene = nullptr;
	std::vector<obs_sceneitem_t *> _currentItems;

	std::vector<std::vector<obs_sceneitem_t*>> _layers;
	
	QFormLayout *_form = nullptr;
	std::vector<size_t> _used;
	QHBoxLayout *toplayout = nullptr;

	obs_data_t *saved = nullptr;
	std::vector<std::vector<std::string>> _layersinfo;
	bool loaded = false;
public:
	void CompleteLoad(std::vector<obs_sceneitem_t*> items)
	{
		for (obs_sceneitem_t *item : items) {
			size_t index = ItemInLayer(item);
			std::string name = sceneitem_get_name(item);
			if (name.empty())
				continue;
			if (index < _layers.size()) {
				blog(LOG_INFO, "%s already in layer %zu", name.c_str(), index);
				continue;
			}
			for (size_t i = 0; i < _layersinfo.size(); i++) {
				std::vector<std::string> names = _layersinfo[i];
				for (size_t j = 0; j < names.size(); j++) {
					if (name == names[j]) {
						while (_layers.size() < (i+1))
							_layers.push_back({});
						_layers[i].push_back(item);
						blog(LOG_INFO, "%s in new layer", name.c_str(), _layers.size());
					}
				}
			}
		}
		blog(LOG_INFO, "Finished Loading");
	}
	/*
	void Load()
	{
		if (!saved) {
			std::string file = obs_get_module_data_path(obs_current_module());
			file += "/layers.json";
			saved = obs_data_create_from_json_file_safe(file.c_str(),
					"bak");
			obs_data_array_t *layers = obs_data_get_array(saved, "layers");
			size_t count = obs_data_array_count(layers);
			_layersinfo.clear();
			_layersinfo.reserve(count);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *layeritem = obs_data_array_item(layers, i);
				obs_data_array_t *layer = obs_data_get_array(layeritem, "layer");
				size_t count2 = obs_data_array_count(layer);
				std::vector<std::string> names;
				names.reserve(count2);
				for (size_t j = 0; j < count; j++) {
					obs_data_t *itemdata = obs_data_array_item(layer, j);
					names.push_back(obs_data_get_string(itemdata, "name"));
					obs_data_release(itemdata);
				}
				_layersinfo.push_back(names);
				obs_data_array_release(layer);
				obs_data_release(layeritem);
			}
			obs_data_array_release(layers);
		}
	}

	void Save()
	{
		obs_data_t *data = obs_data_create();
		obs_data_array_t *layerarray = obs_data_array_create();
		for (size_t i = 0; i < _layers.size(); i++) {
			std::vector<obs_sceneitem_t*> layer = _layers[i];
			obs_data_t *layerdata = obs_data_create();
			obs_data_array_t *itemarray = obs_data_array_create();

			for (obs_sceneitem_t *item : layer) {
				obs_data_t *iteminfo = obs_data_create();
				obs_source_t *source = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(source);
				obs_data_set_string(iteminfo, "name", name);
				obs_data_array_push_back(itemarray, iteminfo);
				obs_data_release(iteminfo);
			}

			obs_data_set_array(layerdata, "layer", itemarray);
			obs_data_array_push_back(layerarray, layerdata);
			obs_data_array_release(itemarray);
		}
		obs_data_set_array(data, "layers", layerarray);
		obs_data_array_release(layerarray);

		std::string file = obs_get_module_data_path(obs_current_module());
		file += "/layers.json";
		//char *file = obs_module_file("layers.json");
		obs_data_save_json_safe(data, file.c_str(), "tmp", "bak");
		obs_data_release(data);
		//bfree(file);
	}
	*/

	void LoadData(obs_data_t *save_data)
	{
		obs_data_array_t *layers = obs_data_get_array(save_data, "layers");
		size_t count = obs_data_array_count(layers);
		_layersinfo.clear();
		_layersinfo.reserve(count);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *layeritem = obs_data_array_item(layers, i);
			obs_data_array_t *layer = obs_data_get_array(layeritem, "layer");
			size_t count2 = obs_data_array_count(layer);
			std::vector<std::string> names;
			names.reserve(count2);
			for (size_t j = 0; j < count; j++) {
				obs_data_t *itemdata = obs_data_array_item(layer, j);
				std::string name = obs_data_get_string(itemdata, "name");
				if (!name.empty())
					names.push_back(name);
				obs_data_release(itemdata);
			}
			_layersinfo.push_back(names);
			obs_data_array_release(layer);
			obs_data_release(layeritem);
		}
		obs_data_array_release(layers);
	}

	void SaveData(obs_data_t *save_data)
	{
		obs_data_array_t *layerarray = obs_data_array_create();
		for (size_t i = 0; i < _layers.size(); i++) {
			std::vector<obs_sceneitem_t*> layer = _layers[i];
			obs_data_t *layerdata = obs_data_create();
			obs_data_array_t *itemarray = obs_data_array_create();

			for (obs_sceneitem_t *item : layer) {
				obs_data_t *iteminfo = obs_data_create();
				obs_source_t *source = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(source);
				obs_data_set_string(iteminfo, "name", name);
				obs_data_array_push_back(itemarray, iteminfo);
				obs_data_release(iteminfo);
			}

			obs_data_set_array(layerdata, "layer", itemarray);
			obs_data_array_push_back(layerarray, layerdata);
			obs_data_array_release(itemarray);
		}
		obs_data_set_array(save_data, "layers", layerarray);
		obs_data_array_release(layerarray);
	}

	void AddSceneItem(OBSSceneItem item)
	{
		//CompleteLoad({item});
		obs_sceneitem_t *itm = item;
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = obs_source_get_name(source);
		blog(LOG_INFO, "Adding: %s", name);
		if (toplayout) {
			QString qname(name);
			QPushButton *button = new QPushButton(qname);
			QVariant val((uint64_t)itm);
			button->setProperty("scene_item", val);
			connect(button, &QPushButton::clicked, this, &OBSLayerView::MenuDropDown);
			/*Connect Button*/
			toplayout->addWidget(button);
			blog(LOG_INFO, "Adding Button: %s", name);
			update();
			repaint();
		}
	}

	void RemoveSceneItem(OBSSceneItem item)
	{
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = obs_source_get_name(source);
		blog(LOG_INFO, "Removing: %s", name);
		if (toplayout) {
			obs_sceneitem_t *test = item;
			for (size_t i = 0; i < toplayout->count(); i++) {
				QWidget *button = toplayout->itemAt(i)->widget();
				QVariant val = button->property("scene_item");
				obs_sceneitem_t *buttonitem = (obs_sceneitem_t*)val.toULongLong();
				if (buttonitem == test) {
					delete button;
					break;
				}
			}
			blog(LOG_INFO, "Removing Button: %s", name);
		}
		RemoveFromAllLayers(item);

		update();
		repaint();
	}

	void RemoveFromAllLayers(OBSSceneItem item)
	{
		obs_sceneitem_t *test = item;
		for (size_t i = 0; i < _layers.size(); i++) {
			_layers[i].erase(std::remove_if(_layers[i].begin(), _layers[i].end(), [=](obs_sceneitem_t *it) {
				return item == it;
			}), _layers[i].end());

			for (size_t k = 1; k < _form->count(); k++) {
				QLayoutItem *layoutitem = _form->itemAt(k);
				if (!layoutitem)
					continue;
				QHBoxLayout *layerlayout = (QHBoxLayout*)layoutitem->layout();

				for (size_t j = 0; j < layerlayout->count(); j++) {
					QWidget *widget = layerlayout->itemAt(j)->widget();
					QVariant val = widget->property("scene_item");
					obs_sceneitem_t *buttonitem = (obs_sceneitem_t*)val.toULongLong();
					if (buttonitem == test) {
						delete widget;
						j--;
					}
				}

				if (layerlayout->count() == 0) {
					QVariant val = layerlayout->property("layer");
					size_t index = val.toULongLong();
					_used.erase(std::remove_if(_used.begin(), _used.end(),
						[=](size_t idx) {
						return idx == index;
					}), _used.end());
					_form->removeRow(k);
					k--;
				}
			}
		}
		/*
		_layers.erase(std::remove_if(_layers.begin(), _layers.end(), [=](std::vector<obs_sceneitem_t*> its) {
			return its.size() == 0;
		}), _layers.end());
		*/
	}
public slots:
	void AddToLayer()
	{
		QObject *action = sender();
		QVariant val = action->property("scene_item");
		obs_sceneitem_t *buttonitem = (obs_sceneitem_t*)val.toULongLong();
		obs_source_t *src = obs_sceneitem_get_source(buttonitem);
		const char *n = obs_source_get_name(src);
		QVariant layerindex = action->property("layer");
		size_t index = layerindex.toULongLong();

		RemoveFromAllLayers(buttonitem);
		if (index >= _layers.size()) {
			index = _layers.size();
			_layers.push_back({ buttonitem });

			QHBoxLayout *layerlayout = new QHBoxLayout();
			layerlayout->setProperty("layer", index);
			obs_source_t *source = obs_sceneitem_get_source(buttonitem);
			const char *name = obs_source_get_name(source);
			QString qname(name);
			QPushButton *button = new QPushButton(qname);
			QVariant val((uint64_t)buttonitem);
			button->setProperty("scene_item", val);

			connect(button, &QPushButton::clicked, this, &OBSLayerView::SwitchViewable);
			/*Connect Button*/
			layerlayout->addWidget(button);
			_form->addRow(layerlayout);
		} else {
			_layers[index].push_back(buttonitem);
			QHBoxLayout *layerlayout = nullptr;
			for (size_t i = 0; i < _form->count(); i++) {
				QLayoutItem *layoutitem = _form->itemAt(i);
				if (layoutitem)
					continue;
				QHBoxLayout *hbox = (QHBoxLayout*)layoutitem->layout();
				QVariant val = hbox->property("layer");
				if (val.toULongLong() == index) {
					layerlayout = hbox;
					break;
				}
			}
			if (!layerlayout) {
				layerlayout = new QHBoxLayout();
				layerlayout->setProperty("layer", index);
				_form->addRow(layerlayout);
			}

			obs_source_t *source = obs_sceneitem_get_source(buttonitem);
			const char *name = obs_source_get_name(source);
			QString qname(name);
			QPushButton *button = new QPushButton(qname);
			QVariant val((uint64_t)buttonitem);
			button->setProperty("scene_item", val);

			connect(button, &QPushButton::clicked, this, &OBSLayerView::SwitchViewable);
			/*Connect Button*/
			layerlayout->addWidget(button);
		}

		update();
		repaint();
	}

	void MenuDropDown()
	{
		QObject *btn = sender();
		OBSLayerView *view = this;
		QVariant val = btn->property("scene_item");
		obs_sceneitem_t *buttonitem = (obs_sceneitem_t*)val.toULongLong();
		obs_source_t *src = obs_sceneitem_get_source(buttonitem);
		const char *n = obs_source_get_name(src);

		blog(LOG_INFO, "button clicked: %s", n);

		QMenu popup;

		for (size_t i = 0; i < _layers.size(); i++) {
			QAction *addToLayer = new QAction(QString::number(i + 1), this);
			addToLayer->setProperty("scene_item", val);
			addToLayer->setProperty("layer", i);
			connect(addToLayer, &QAction::triggered, this, &OBSLayerView::AddToLayer);
			popup.addAction(addToLayer);
		}
		QAction addToNew("+", this);
		addToNew.setProperty("scene_item", val);
		addToNew.setProperty("layer", -1);
		connect(&addToNew, &QAction::triggered, this, &OBSLayerView::AddToLayer);
		popup.addAction(&addToNew);
		
		popup.exec(QCursor::pos());
	}

	void SwitchViewable()
	{
		QObject *btn = sender();
		QVariant val = btn->property("scene_item");
		obs_sceneitem_t *buttonitem = (obs_sceneitem_t*)val.toULongLong();
		obs_source_t *src = obs_sceneitem_get_source(buttonitem);
		const char *n = obs_source_get_name(src);

		blog(LOG_INFO, "button clicked: %s", n);

		size_t index = ItemInLayer(buttonitem);
		QLayoutItem *layoutitem = _form->itemAt(index + 1);
		if (!layoutitem)
			return;
		QHBoxLayout *layout = (QHBoxLayout *)layoutitem->layout();
		for (size_t i = 0; i < layout->count(); i++) {
			QWidget *button = layout->itemAt(i)->widget();
			QVariant val = button->property("scene_item");
			obs_sceneitem_t *btnitem = (obs_sceneitem_t*)val.toULongLong();
			obs_source_t *src1 = obs_sceneitem_get_source(btnitem);
			const char *n1 = obs_source_get_name(src1);
			//blog(LOG_INFO, "item: %s vs %s", n, n1);
			obs_sceneitem_set_visible(btnitem, btnitem == buttonitem);
		}
	}
public:
	void AddItem(obs_sceneitem_t *item)
	{
		obs_sceneitem_addref(item);
		_currentItems.push_back(item);
	}

	void UseLayer(size_t index)
	{
		blog(LOG_INFO, "Using Layer: %zu", index);
		for (size_t usedindex : _used) {
			if (index == usedindex)
				return;
		}
		_used.push_back(index);
		std::vector<obs_sceneitem_t*> items = _layers[index];
		QHBoxLayout *layerlayout = new QHBoxLayout();
		layerlayout->setProperty("layer", index);
		for (obs_sceneitem_t *item : items) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = obs_source_get_name(source);
			QString qname(name);
			QPushButton *button = new QPushButton(qname);
			QVariant val((uint64_t)item);
			button->setProperty("scene_item", val);

			connect(button, &QPushButton::clicked, this, &OBSLayerView::SwitchViewable);
			/*Connect Button*/
			layerlayout->addWidget(button);
		}
		_form->addRow(layerlayout);
	}

	size_t ItemInLayer(obs_sceneitem_t *item)
	{
		for (size_t i = 0; i < _layers.size(); i++) {
			for (size_t j = 0; j < _layers[i].size(); j++) {
				if (item == _layers[i][j])
					return i;
			}
		}
		return -1;
	}

	OBSLayerView(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags())
		: QDockWidget(parent, flags)
	{
		_customgui = new QWidget();
		setWidget(_customgui);
	}

	~OBSLayerView()
	{
		if (saved)
			obs_data_release(saved);
	}

	void UpdateScene()
	{
		blog(LOG_INFO, "Changed");
		/* Get the current scene we're working w/ */
		if (_currentSceneSource)
			obs_source_release(_currentSceneSource);
		_currentSceneSource = obs_frontend_get_current_scene();
		_currentScene = obs_scene_from_source(_currentSceneSource);
		for (obs_sceneitem_t *item : _currentItems)
			obs_sceneitem_release(item);

		_currentItems.clear();
		obs_scene_enum_items(_currentScene, listSceneItems, this);

		CompleteLoad(_currentItems);

		/* Update GUI */
		if (!_form) {
			_form = new QFormLayout(this);
			_customgui->setLayout(_form);
		}
		while (_form->rowCount())
			_form->removeRow(0);
		_used.clear();
		_used.reserve(_layers.size());

		toplayout = new QHBoxLayout();

		for (obs_sceneitem_t *item : _currentItems) {
			obs_source_t *source = obs_sceneitem_get_source(item);
			const char *name = obs_source_get_name(source);
			blog(LOG_INFO, "Adding Button: %s", name);
			QString qname(name);
			QPushButton *button = new QPushButton(qname);
			connect(button, &QPushButton::clicked, this, &OBSLayerView::MenuDropDown);

			QVariant val((uint64_t)item);
			button->setProperty("scene_item", val);
			button->setVisible(true);
			/*Connect Button*/
			toplayout->addWidget(button);
		}
		_form->addRow(toplayout);

		for (obs_sceneitem_t *item : _currentItems) {
			size_t index = ItemInLayer(item);
			if (index < _layers.size())
				UseLayer(index);
		}

		update();
		repaint();
	}
};
