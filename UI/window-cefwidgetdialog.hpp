#pragma once

#include <QDialog>
#include <string>
#include <memory>

#include "ui_cefwidgetdialog.h"

class CefWidgetDialog : public QDialog
{
	Q_OBJECT

private:
	std::unique_ptr<Ui::CefWidgetDialog> ui;

public:
	explicit CefWidgetDialog(QWidget *parent = 0);
	~CefWidgetDialog();

	static bool AskForUrl(QWidget *parent, const QString title,
		const QString &text, std::string &str,
		const QString &placeHolder = QString(""));
};
