#pragma once
#include <util/platform.h>
#include <obs-frontend-api.h>
#include <QPointer>
#include <QWidget>
#include <QTimer>
#include <QLabel>
#include <QList>
#include <QChart>
#include <QChartView>
#include <QSplineSeries>
#include <QValueAxis>
#include <string>
#include <obs.hpp>
#include <QDialog>
#include <QDialogButtonBox>

class QGridLayout;
class QCloseEvent;

class Chart : public QChart {
	Q_OBJECT

public:
	Chart(QGraphicsItem *parent = nullptr, Qt::WindowFlags wFlags = {});
	virtual ~Chart();
	QValueAxis *m_axisX;
	QValueAxis *m_axisY;
	QValueAxis *m_axisZ; // vertical axis for RTT
	QLineSeries *retrans_pkt_series = nullptr;
	QLineSeries *dropped_pkt_series = nullptr;
	QSplineSeries *rtt_series = nullptr;

	obs_output_t *output;

public slots:
	void handleTimeout();
	void connect();
	void disconnect();

private:
	QTimer m_timer;
	QStringList m_titles;
	qreal m_x;
	qreal m_y;
	qreal m_step;
};

class SRTStats : public QDialog {
	Q_OBJECT

	QGridLayout *outputLayout = nullptr;
	QWidget *srtWidget = nullptr;
	Chart *chart = nullptr;
	QChartView *chartView = nullptr;
	QTimer timer;
	uint64_t num_bytes = 0;
	std::vector<long double> bitrates;

	struct OutputLabels {
		QPointer<QLabel> name;
		QPointer<QLabel> status;
		QPointer<QLabel> droppedFrames;
		QPointer<QLabel> megabytesSent;
		QPointer<QLabel> bitrate;

		/* SRT stats */
		QPointer<QLabel> srt_total_pkts;
		QPointer<QLabel> srt_retransmitted_pkts;
		QPointer<QLabel> srt_dropped_pkts;
		QPointer<QLabel> srt_rtt;
		QPointer<QLabel> srt_peer_latency;
		QPointer<QLabel> srt_latency;
		QPointer<QLabel> srt_bandwidth;
		qreal total_pkts;

		/*------------------------------------------*/

		uint64_t lastBytesSent = 0;
		uint64_t lastBytesSentTime = 0;

		int first_total = 0;
		int first_dropped = 0;

		void Update(obs_output_t *output, bool rec);
		void ExtraUpdate(obs_output_t *output);
		void Reset(obs_output_t *output);

		long double kbps = 0.0l;
	};
	OutputLabels ol;
	QList<OutputLabels> outputLabels;
	void AddOutputLabels();

	virtual void closeEvent(QCloseEvent *event) override;
	static void OBSFrontendEvent(enum obs_frontend_event event, void *ptr);
	void Update();
	void Reset();

public:
	SRTStats(QWidget *parent = nullptr);
	~SRTStats();
	void ToggleShowHide();
	QDialogButtonBox *buttonBox = nullptr;

private Q_SLOTS:
	void DialogButtonClicked(QAbstractButton *button);

protected:
	virtual void showEvent(QShowEvent *event) override;
	virtual void hideEvent(QHideEvent *event) override;
};
