#pragma once

#include <QList>
#include <QVector>
#include <QPointer>
#include <QListView>
#include <QCheckBox>
#include <QAbstractListModel>

class QLabel;
class QCheckBox;
class QLineEdit;
class SourceTree;
class QSpacerItem;
class QHBoxLayout;
class LockedCheckBox;
class VisibilityCheckBox;
class VisibilityItemWidget;

class SourceTreeSubItemCheckBox : public QCheckBox {
	Q_OBJECT
};

class SourceTreeItem : public QWidget {
	Q_OBJECT

	friend class SourceTree;
	friend class SourceTreeModel;

	void mouseDoubleClickEvent(QMouseEvent *event) override;

	virtual bool eventFilter(QObject *object, QEvent *event) override;

	void Update(bool force);

	enum class Type {
		Unknown,
		Item,
		Group,
		SubItem,
	};

	void DisconnectSignals();
	void ReconnectSignals();

	Type type = Type::Unknown;

public:
	explicit SourceTreeItem(SourceTree *tree, OBSSceneItem sceneitem);
	bool IsEditing();
protected:
	void SetLayerText(QString &text)
	{
		layerLabel->setText(text);
	}

private:
	QSpacerItem *spacer = nullptr;
	QCheckBox *expand = nullptr;
	QLabel *layerLabel = nullptr;
	VisibilityCheckBox *vis = nullptr;
	LockedCheckBox *lock = nullptr;
	QHBoxLayout *boxLayout = nullptr;
	QLabel *label = nullptr;

	QLineEdit *editor = nullptr;

	SourceTree *tree;
	OBSSceneItem sceneitem;
	OBSSignal sceneRemoveSignal;
	OBSSignal itemRemoveSignal;
	OBSSignal groupReorderSignal;
	OBSSignal deselectSignal;
	OBSSignal visibleSignal;
	OBSSignal renameSignal;
	OBSSignal removeSignal;

	virtual void paintEvent(QPaintEvent* event) override;

private slots:
	void Clear();

	void EnterEditMode();
	void ExitEditMode(bool save);

	void VisibilityChanged(bool visible);
	void Renamed(const QString &name);

	void ExpandClicked(bool checked);

	void Deselect();
	void ReLayer();
};

struct layer_info {
	std::string name;
	bool loaded;
};

class SourceTreeModel : public QAbstractListModel {
	Q_OBJECT

	friend class SourceTree;
	friend class SourceTreeItem;

	SourceTree *st;
	QVector<OBSSceneItem> items;
	bool hasGroups = false;

	static void OBSFrontendEvent(enum obs_frontend_event event, void *ptr);
	void Clear();
	void SceneChanged();
	void ReorderItems();

	void Add(obs_sceneitem_t *item);
	void Remove(obs_sceneitem_t *item);
	OBSSceneItem Get(int idx);
	QString GetNewGroupName();
	void AddGroup();

	void GroupSelectedItems(QModelIndexList &indices);
	void UngroupSelectedGroups(QModelIndexList &indices);

	void PrepLayerItems(std::vector<std::string> names);
	void LayerSelectedItems(QModelIndexList &indicies);
	void UnlayerSelectedItem(obs_sceneitem_t *item);
	void UnlayerSelectedItems(QModelIndexList &indicies);

	void ExpandGroup(obs_sceneitem_t *item);
	void CollapseGroup(obs_sceneitem_t *item);

	void UpdateGroupState(bool update);

	std::vector<std::vector<obs_sceneitem_t*>*> layers;
	std::vector<std::vector<layer_info>> savedLayers;
protected:
	bool ItemIsInLayer(obs_sceneitem_t *item);
	void ShowLayer(obs_sceneitem_t *item);
	void AddItemToLayer(obs_sceneitem_t *item, size_t index);
	QString ItemLayer(obs_sceneitem_t *item);
	std::vector<std::vector<obs_sceneitem_t*>*> GetLayers()
	{
		std::vector<std::vector<obs_sceneitem_t*>*> l;
		l.reserve(layers.size());
		for (size_t i = 0; i < layers.size(); i++) {
			std::vector<obs_sceneitem_t*> *layer =
				new std::vector<obs_sceneitem_t*>();
			layer->reserve(layers[i]->size());
			layer->assign(layers[i]->begin(), layers[i]->end());
			l.push_back(layer);
		}
		return l;
	}
public:
	explicit SourceTreeModel(SourceTree *st);
	~SourceTreeModel();

	virtual int rowCount(const QModelIndex &parent) const override;
	virtual QVariant data(const QModelIndex &index, int role) const override;

	virtual Qt::ItemFlags flags(const QModelIndex &index) const override;
	virtual Qt::DropActions supportedDropActions() const override;
};

class SourceTree : public QListView {
	Q_OBJECT

	bool ignoreReorder = false;
	bool layerMode = false;

	friend class SourceTreeModel;
	friend class SourceTreeItem;

	void ResetWidgets();
	void UpdateWidget(const QModelIndex &idx, obs_sceneitem_t *item);
	void UpdateWidgets(bool force = false);

	inline SourceTreeModel *GetStm() const
	{
		return reinterpret_cast<SourceTreeModel *>(model());
	}

public:
	void LoadLayers(obs_data_array_t *layers);
	bool LayerMode();
	QString ItemLayer(obs_sceneitem_t *item);

	inline SourceTreeItem *GetItemWidget(int idx)
	{
		QWidget *widget = indexWidget(GetStm()->createIndex(idx, 0));
		return reinterpret_cast<SourceTreeItem *>(widget);
	}

	bool ItemIsInLayer(obs_sceneitem_t *item);

	explicit SourceTree(QWidget *parent = nullptr);

	inline bool IgnoreReorder() const {return ignoreReorder;}
	inline void Clear() {GetStm()->Clear();}

	inline void Add(obs_sceneitem_t *item) {GetStm()->Add(item);}
	inline OBSSceneItem Get(int idx) {return GetStm()->Get(idx);}
	inline QString GetNewGroupName() {return GetStm()->GetNewGroupName();}
	inline std::vector<std::vector<obs_sceneitem_t*>*> GetLayers()
	{
		return GetStm()->GetLayers();
	}

	void SelectItem(obs_sceneitem_t *sceneitem, bool select);

	bool MultipleBaseSelected() const;
	bool GroupsSelected() const;
	bool GroupedItemsSelected() const;
	
public slots:
	void ReLayer();
	void SetMode(bool mode);
	void ToggleMode();
	inline void ReorderItems() {GetStm()->ReorderItems();}
	void ShowLayer(OBSSceneItem item);
	void Remove(OBSSceneItem item);
	void GroupSelectedItems();
	void LayerSelectedItems();
	void UngroupSelectedGroups();
	void AddGroup();
	void Edit(int idx);

protected:
	virtual void mouseDoubleClickEvent(QMouseEvent *event) override;
	virtual void dropEvent(QDropEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;
	virtual void leaveEvent(QEvent *event) override;

	virtual void selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) override;
};
