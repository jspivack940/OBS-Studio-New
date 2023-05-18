#pragma once
#include <obs-module.h>
#include "obs-ffmpeg-srt-stats.hpp"
#include <QAction>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScreen>
#include <QDockWidget>
#include <string>
#include <util/config-file.h>
#include <QGuiApplication>
#include <QStyle>
#include <QDialogButtonBox>

#define TIMER_INTERVAL 2000

#define QT_UTF8(str) QString::fromUtf8(str, -1)
#define QT_TO_UTF8(str) str.toUtf8().constData()

SRTStats *_statsDialog = nullptr;
QDockWidget *statsDock = nullptr;
bool WindowPositionValid(QRect rect)
{
	for (QScreen *screen : QGuiApplication::screens()) {
		if (screen->availableGeometry().intersects(rect))
			return true;
	}
	return false;
}
void setThemeID(QWidget *widget, const QString &themeID)
{
	if (widget->property("themeID").toString() != themeID) {
		widget->setProperty("themeID", themeID);

		/* force style sheet recalculation */
		QString qss = widget->styleSheet();
		widget->setStyleSheet("/* */");
		widget->setStyleSheet(qss);
	}
}

extern "C" void load_srt_stats()
{
	// initialize stats
	obs_frontend_push_ui_translation(obs_module_get_string);
	QMainWindow *mainWindow =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	_statsDialog = new SRTStats(mainWindow);
	obs_frontend_pop_ui_translation();

	// add the stats to the tools menu
	const char *menuActionText = obs_module_text("SRT.Stats");
	QAction *menuAction =
		(QAction *)obs_frontend_add_tools_menu_qaction(menuActionText);

	QObject::connect(menuAction, &QAction::triggered,
			 [] { _statsDialog->ToggleShowHide(); });
}

extern "C" void unload_srt_stats()
{
	//delete _statsDialog;
}

void SRTStats::OBSFrontendEvent(enum obs_frontend_event event, void *ptr)
{
	SRTStats *stats = reinterpret_cast<SRTStats *>(ptr);

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		stats->chart->connect();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stats->chart->disconnect();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		stats->Update();
	default:
		break;
	}
}

SRTStats::SRTStats(QWidget *parent) : timer(this)
{
	QVBoxLayout *mainLayout = new QVBoxLayout();
	outputLayout = new QGridLayout();

	/* --------------------------------------------- */
	QDialogButtonBox *closeButton = new QDialogButtonBox();
	closeButton->setStandardButtons(QDialogButtonBox::Close);
	QDialogButtonBox *resetButton = new QDialogButtonBox();
	resetButton->setStandardButtons(QDialogButtonBox::Reset);
	QHBoxLayout *buttonLayout = new QHBoxLayout;
	buttonLayout->addStretch();
	buttonLayout->addWidget(resetButton);
	buttonLayout->addWidget(closeButton);

	/* --------------------------------------------- */

	int col = 0;
	auto addOutputCol = [&](const char *loc) {
		QLabel *label = new QLabel(QT_UTF8(loc), this);
		label->setStyleSheet("font-weight: bold");
		outputLayout->addWidget(label, 0, col++);
	};

	addOutputCol(obs_module_text("SRT.Stats.Status"));
	addOutputCol(obs_module_text("SRT.Stats.DroppedFrames"));
	addOutputCol(obs_module_text("SRT.Stats.MegabytesSent"));
	addOutputCol(obs_module_text("SRT.Stats.Bitrate"));
	addOutputCol(obs_module_text("SRT.Stats.RTT"));
	addOutputCol(obs_module_text("SRT.Stats.Bandwidth"));

	col = 0;
	auto extraAddOutputCol = [&](const char *loc) {
		QLabel *label = new QLabel(QT_UTF8(loc), this);
		label->setStyleSheet("font-weight: bold");
		outputLayout->addWidget(label, 2, col++);
	};
	extraAddOutputCol(obs_module_text("SRT.Stats.Total.Pkts"));
	extraAddOutputCol(obs_module_text("SRT.Stats.Retransmitted.Pkts"));
	extraAddOutputCol(obs_module_text("SRT.Stats.Dropped.Pkts"));
	extraAddOutputCol(obs_module_text("SRT.Stats.Peer.Latency"));
	extraAddOutputCol(obs_module_text("SRT.Stats.Latency"));

	/* --------------------------------------------- */
	AddOutputLabels();

	chart = new Chart;
	chart->resize(780, 500);
	/* --------------------------------------------- */

	QVBoxLayout *outputContainerLayout = new QVBoxLayout();
	outputContainerLayout->addLayout(outputLayout);

	QChartView *chartView = new QChartView(chart);
	chartView->setRenderHint(QPainter::Antialiasing);
	outputContainerLayout->addWidget(chartView);

	outputContainerLayout->addStretch();

	QWidget *widget = new QWidget(this);
	widget->setLayout(outputContainerLayout);

	QScrollArea *scrollArea = new QScrollArea(this);
	scrollArea->setWidget(widget);
	scrollArea->setWidgetResizable(true);
	/* --------------------------------------------- */

	mainLayout->addWidget(scrollArea);
	mainLayout->addLayout(buttonLayout);
	setLayout(mainLayout);

	/* --------------------------------------------- */
	connect(closeButton, &QDialogButtonBox::clicked, [this]() { Close(); });
	connect(resetButton, &QDialogButtonBox::clicked, [this]() { Reset(); });

	//	resize(800, 500);

	setWindowTitle(obs_module_text("SRT.Stats"));
#ifdef __APPLE__
	setWindowIcon(
		QIcon::fromTheme("obs", QIcon(":/res/images/obs_256x256.png")));
#else
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs.png")));
#endif

	setWindowModality(Qt::NonModal);
	setAttribute(Qt::WA_DeleteOnClose, true);

	QObject::connect(&timer, &QTimer::timeout, this, &SRTStats::Update);
	timer.setInterval(TIMER_INTERVAL);

	if (isVisible())
		timer.start();

	obs_frontend_add_event_callback(OBSFrontendEvent, this);

	config_t *conf = obs_frontend_get_global_config();

	const char *geometry = config_get_string(conf, "SRTStats", "geometry");
	if (geometry != NULL) {
		QByteArray byteArray =
			QByteArray::fromBase64(QByteArray(geometry));
		restoreGeometry(byteArray);

		QRect windowGeometry = normalGeometry();
		if (!WindowPositionValid(windowGeometry)) {
			QRect rect =
				QGuiApplication::primaryScreen()->geometry();
			setGeometry(QStyle::alignedRect(Qt::LeftToRight,
							Qt::AlignCenter, size(),
							rect));
		}
	}
}

SRTStats::~SRTStats()
{
	obs_frontend_remove_event_callback(OBSFrontendEvent, this);
}
void SRTStats::showEvent(QShowEvent *)
{
	timer.start(TIMER_INTERVAL);
}

void SRTStats::hideEvent(QHideEvent *)
{
	timer.stop();
}

void SRTStats::closeEvent(QCloseEvent *event)
{
	QMainWindow *main =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	config_t *conf = obs_frontend_get_global_config();
	if (isVisible()) {
		config_set_string(conf, "SRTStats", "geometry",
				  saveGeometry().toBase64().constData());
		config_save_safe(conf, "tmp", nullptr);
	}

	QWidget::closeEvent(event);
}

void SRTStats::AddOutputLabels()
{
	ol.status = new QLabel(this);
	ol.droppedFrames = new QLabel(this);
	ol.megabytesSent = new QLabel(this);
	ol.bitrate = new QLabel(this);
	ol.srt_total_pkts = new QLabel(this);
	ol.srt_retransmitted_pkts = new QLabel(this);
	ol.srt_dropped_pkts = new QLabel(this);
	ol.srt_rtt = new QLabel(this);
	ol.srt_peer_latency = new QLabel(this);
	ol.srt_latency = new QLabel(this);
	ol.srt_bandwidth = new QLabel(this);

	int col = 0;
	int row = outputLabels.size() + 1;
	outputLayout->addWidget(ol.status, 1, col++);
	outputLayout->addWidget(ol.droppedFrames, 1, col++);
	outputLayout->addWidget(ol.megabytesSent, 1, col++);
	outputLayout->addWidget(ol.bitrate, 1, col++);
	outputLayout->addWidget(ol.srt_rtt, 1, col++);
	outputLayout->addWidget(ol.srt_bandwidth, 1, col++);
	col = 0;
	outputLayout->addWidget(ol.srt_total_pkts, 3, col++);
	outputLayout->addWidget(ol.srt_retransmitted_pkts, 3, col++);
	outputLayout->addWidget(ol.srt_dropped_pkts, 3, col++);
	outputLayout->addWidget(ol.srt_peer_latency, 3, col++);
	outputLayout->addWidget(ol.srt_latency, 3, col++);

	outputLabels.push_back(ol);
}

void SRTStats::Update()
{

	OBSOutputAutoRelease strOutput = obs_frontend_get_streaming_output();

	if (!strOutput)
		return;

	/* ------------------------------------------- */
	/* recording/streaming stats                   */

	outputLabels[0].Update(strOutput, false);

	outputLabels[0].ExtraUpdate(strOutput);

	chart->output = strOutput;
}

void SRTStats::Close()
{
	if (!close())
		blog(LOG_ERROR, "shit");
	blog(LOG_INFO, "isVisible is %i", isVisible());
}

void SRTStats::Reset()
{
	timer.start();

	OBSOutputAutoRelease strOutput = obs_frontend_get_streaming_output();

	outputLabels[0].Reset(strOutput);
	Update();
	if (obs_frontend_streaming_active()) {
		chart->disconnect();
		chart->connect();
	}
}

void SRTStats::ToggleShowHide()
{
	//if (!isVisible())
	//	setVisible(true);
	//else
	//	setVisible(false);
	blog(LOG_INFO, "isVisible at toggle is %i", isVisible());
	setVisible(!isVisible());
}

void SRTStats::OutputLabels::Update(obs_output_t *output, bool rec)
{
	uint64_t totalBytes = output ? obs_output_get_total_bytes(output) : 0;
	uint64_t curTime = os_gettime_ns();
	uint64_t bytesSent = totalBytes;

	if (bytesSent < lastBytesSent)
		bytesSent = 0;
	if (bytesSent == 0)
		lastBytesSent = 0;

	uint64_t bitsBetween = (bytesSent - lastBytesSent) * 8;
	long double timePassed =
		(long double)(curTime - lastBytesSentTime) / 1000000000.0l;
	kbps = (long double)bitsBetween / timePassed / 1000.0l;

	if (timePassed < 0.01l)
		kbps = 0.0l;

	QString str = obs_module_text("SRT.Stats.Status.Inactive");
	QString themeID;
	bool active = output ? obs_output_active(output) : false;

	if (active) {
		bool reconnecting = output ? obs_output_reconnecting(output)
					   : false;

		if (reconnecting) {
			str = obs_module_text("SRT.Stats.Status.Reconnecting");
			themeID = "error";
		} else {
			str = obs_module_text("SRT.Stats.Status.Live");
			themeID = "good";
		}
	}

	status->setText(str);
	setThemeID(status, themeID);

	long double num = (long double)totalBytes / (1024.0l * 1024.0l);

	megabytesSent->setText(
		QString("%1 MB").arg(QString::number(num, 'f', 1)));
	bitrate->setText(QString("%1 kb/s").arg(QString::number(kbps, 'f', 0)));

	int total = output ? obs_output_get_total_frames(output) : 0;
	int dropped = output ? obs_output_get_frames_dropped(output) : 0;

	if (total < first_total || dropped < first_dropped) {
		first_total = 0;
		first_dropped = 0;
	}

	total -= first_total;
	dropped -= first_dropped;

	num = total ? (long double)dropped / (long double)total * 100.0l : 0.0l;

	str = QString("%1 / %2 (%3%)")
		      .arg(QString::number(dropped), QString::number(total),
			   QString::number(num, 'f', 1));
	droppedFrames->setText(str);

	if (num > 5.0l)
		setThemeID(droppedFrames, "error");
	else if (num > 1.0l)
		setThemeID(droppedFrames, "warning");
	else
		setThemeID(droppedFrames, "");

	lastBytesSent = bytesSent;
	lastBytesSentTime = curTime;
}

void SRTStats::OutputLabels::ExtraUpdate(obs_output_t *output)
{
	const char *id = obs_output_get_id(output);
	total_pkts = 0;
	int retransmitted_pkts = 0;
	int dropped_pkts = 0;
	float rtt = 0;
	float peer_latency = 0;
	float latency = 0;
	float bandwidth = 0;
	bool is_mpegts = id ? !strcmp(id, "ffmpeg_mpegts_muxer") : false;

	if (is_mpegts) {
		calldata_t cd = {0};
		proc_handler_t *ph = obs_output_get_proc_handler(output);
		proc_handler_call(ph, "get_srt_stats", &cd);
		total_pkts = (qreal)calldata_int(&cd, "srt_total_pkts");
		retransmitted_pkts =
			calldata_int(&cd, "srt_retransmitted_pkts");
		dropped_pkts = calldata_int(&cd, "srt_dropped_pkts");
		rtt = calldata_float(&cd, "srt_rtt");
		peer_latency = calldata_float(&cd, "srt_peer_latency");
		latency = calldata_float(&cd, "srt_latency");
		bandwidth = calldata_float(&cd, "srt_bandwidth");
	}

	srt_total_pkts->setText(QString::number(total_pkts).append(" pkts"));
	float num = (float)retransmitted_pkts / (float)total_pkts;
	QString str = QString::number(retransmitted_pkts);
	str += QString(" pkts (%1%)").arg(QString::number(num, 'f', 1));
	srt_retransmitted_pkts->setText(str);
	num = (float)dropped_pkts / (float)total_pkts;
	str = QString::number(dropped_pkts);
	str += QString(" pkts (%1%)").arg(QString::number(num, 'f', 1));
	srt_dropped_pkts->setText(str);
	srt_rtt->setText(QString::number(rtt, 'f', 1).append(" ms"));
	srt_peer_latency->setText(
		QString::number(peer_latency, 'f', 1).append(" ms"));
	srt_latency->setText(QString::number(latency, 'f', 1).append(" ms"));
	srt_bandwidth->setText(
		QString::number(bandwidth, 'f', 1).append(" Mbps"));
}

void SRTStats::OutputLabels::Reset(obs_output_t *output)
{
	if (!output)
		return;

	first_total = obs_output_get_total_frames(output);
	first_dropped = obs_output_get_frames_dropped(output);
}

Chart::Chart(QGraphicsItem *parent, Qt::WindowFlags wFlags)
	: QChart(QChart::ChartTypeCartesian, parent, wFlags),
	  retrans_pkt_series(new QLineSeries),
	  dropped_pkt_series(new QLineSeries),
	  rtt_series(new QSplineSeries),
	  m_axisX(new QValueAxis()),
	  m_axisY(new QValueAxis()),
	  m_axisZ(new QValueAxis()),
	  m_step(0),
	  m_x(0),
	  m_y(0),
	  output(nullptr)
{

	QPen red(Qt::red);
	QPen green(Qt::green);
	QPen blue(Qt::blue);
	green.setWidth(3);
	red.setWidth(3);
	blue.setWidth(3);
	retrans_pkt_series->setPen(red);
	dropped_pkt_series->setPen(green);
	rtt_series->setPen(blue);
	retrans_pkt_series->setName(
		obs_module_text("SRT.Stats.Retransmitted.Pkts"));
	dropped_pkt_series->setName(obs_module_text("SRT.Stats.Dropped.Pkts"));
	rtt_series->setName(obs_module_text("SRT.Stats.RTT"));
	addSeries(dropped_pkt_series);
	addSeries(retrans_pkt_series);
	addSeries(rtt_series);
	addAxis(m_axisX, Qt::AlignBottom);
	addAxis(m_axisY, Qt::AlignLeft);
	addAxis(m_axisZ, Qt::AlignRight);

	retrans_pkt_series->attachAxis(m_axisX);
	retrans_pkt_series->attachAxis(m_axisY);
	dropped_pkt_series->attachAxis(m_axisX);
	dropped_pkt_series->attachAxis(m_axisY);
	rtt_series->attachAxis(m_axisX);
	rtt_series->attachAxis(m_axisZ);

	m_axisX->setTickCount(5);
	m_axisX->setRange(0, 1);
	m_axisX->setTitleText(obs_module_text("SRT.Stats.Time"));
	m_axisY->setRange(0, 100);
	m_axisY->setTitleText(obs_module_text("SRT.Stats.Packets"));
	m_axisZ->setRange(0, 100);
	m_axisZ->setTitleText(obs_module_text("SRT.Stats.RTT.ms"));

	m_timer.start();
}

void Chart::connect()
{
	// reset what's displayed.
	// it's reset here so that the figs are still displayed at the end of the stream.
	m_axisX->setTickCount(5);
	m_axisX->setRange(0, 1);
	m_axisY->setRange(0, 100);
	m_axisZ->setRange(0, 100);
	m_x = 0;
	m_y = 0;
	retrans_pkt_series->clear();
	dropped_pkt_series->clear();
	rtt_series->clear();
	QObject::connect(&m_timer, &QTimer::timeout, this,
			 &Chart::handleTimeout);
	m_timer.setInterval(1000);
}

void Chart::disconnect()
{
	QObject::disconnect(&m_timer, &QTimer::timeout, this,
			    &Chart::handleTimeout);
}

Chart::~Chart() {}

void Chart::handleTimeout()
{
	qreal x = plotArea().width() / (12 * m_axisX->tickCount());
	// this slides to the right for 1/60 of the width ==> this sweeps everything in 60 sec
	qreal y =
		(m_axisX->max() - m_axisX->min()) / (12 * m_axisX->tickCount());
	calldata_t cd = {0};
	proc_handler_t *ph = obs_output_get_proc_handler(output);
	proc_handler_call(ph, "get_srt_stats", &cd);
	qreal total_pkts = (qreal)calldata_int(&cd, "srt_total_pkts");
	qreal retransmitted_pkts = calldata_int(&cd, "srt_retransmitted_pkts");
	qreal dropped_pkts = calldata_int(&cd, "srt_dropped_pkts");
	qreal rtt = calldata_float(&cd, "srt_rtt");
	m_x += y;
	m_y = retransmitted_pkts;
	if (m_y >= 0.8 * m_axisY->max())
		m_axisY->setRange(0, 2 * m_axisY->max());
	retrans_pkt_series->append(m_x, m_y);
	m_y = dropped_pkts;
	if (m_y >= 0.8 * m_axisY->max())
		m_axisY->setRange(0, 2 * m_axisY->max());
	dropped_pkt_series->append(m_x, m_y);
	m_y = rtt;
	if (m_y >= 0.8 * m_axisZ->max())
		m_axisZ->setRange(0, 2 * m_axisZ->max());
	rtt_series->append(m_x, m_y);
	if (m_x >= 1)
		scroll(x, 0);
}
