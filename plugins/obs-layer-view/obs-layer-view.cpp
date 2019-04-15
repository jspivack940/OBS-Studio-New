#include "obs-layer-view.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-layer-view", "en-US")
#define blog(level, msg, ...) blog(level, "obs-layer-view: " msg, ##__VA_ARGS__)

static bool listSceneItems(obs_scene_t *scene, obs_sceneitem_t *item, void *vptr)
{
	OBSLayerView *view = static_cast<OBSLayerView*>(vptr);
	view->AddItem(item);
	return true;
}

static void signal_callback_add(void *vptr, calldata_t *cbdata)
{
	//OBSLayerView *view = static_cast<OBSLayerView*>(vptr);
	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(cbdata, "item");
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(source);
	blog(LOG_INFO, "Scene Item Added: %s", name);
	if(layer_view)
		layer_view->AddSceneItem(item);
}

static void signal_callback_remove(void *vptr, calldata_t *cbdata)
{
	//OBSLayerView *view = static_cast<OBSLayerView*>(vptr);
	obs_sceneitem_t *item = (obs_sceneitem_t*)calldata_ptr(cbdata, "item");
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(source);
	blog(LOG_INFO, "Scene Item Removed: %s", name);
	if (layer_view)
		layer_view->RemoveSceneItem(item);
}

static void event_callback(enum obs_frontend_event event,
		void *private_data)
{
	OBSLayerView *view = static_cast<OBSLayerView*>(private_data);
	struct obs_frontend_source_list scenes = { 0 };
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		blog(LOG_INFO, "Scene Changed");

		obs_frontend_get_scenes(&scenes);

		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *source = scenes.sources.array[i];
			signal_handler_t *sh = obs_source_get_signal_handler(source);
			signal_handler_connect(sh, "item_add", signal_callback_add, view);
			signal_handler_connect(sh, "item_remove", signal_callback_remove, view);
		}

		obs_frontend_source_list_free(&scenes);

		if(layer_view)
			layer_view->UpdateScene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		blog(LOG_INFO, "Scene List Changed");

		obs_frontend_get_scenes(&scenes);

		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *source = scenes.sources.array[i];
			signal_handler_t *sh = obs_source_get_signal_handler(source);
			signal_handler_connect(sh, "item_add", signal_callback_add, view);
			signal_handler_connect(sh, "item_remove", signal_callback_remove, view);
		}

		obs_frontend_source_list_free(&scenes);
		if(layer_view)
			layer_view->UpdateScene();
		break;
	}
}

static void saveload_callback(obs_data_t *save_data, bool saving, void *vptr)
{
	//OBSLayerView *view = static_cast<OBSLayerView*>(vptr);
	if (layer_view) {
		if (saving)
			layer_view->SaveData(save_data);
		else
			layer_view->LoadData(save_data);
	}
}

bool obs_module_load()
{
	QMainWindow *mainWindow = static_cast<QMainWindow*>(obs_frontend_get_main_window());

	QAction *menu_action = (QAction*)obs_frontend_add_tools_menu_qaction(
		obs_module_text("OBSLayerView")
	);

	layer_view = new OBSLayerView();

	auto menu_cb = [](){
		if (layer_view)
			layer_view->show();
	};

	menu_action->connect(menu_action, &QAction::triggered, menu_cb);

	obs_frontend_add_event_callback(event_callback, layer_view);
	obs_frontend_add_save_callback(saveload_callback, layer_view);

	blog(LOG_INFO, "module loaded");
	return true;
}

void obs_module_unload()
{
	obs_frontend_remove_event_callback(event_callback, layer_view);
	obs_frontend_remove_save_callback(saveload_callback, layer_view);
}
