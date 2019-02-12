#include "window-cefwidgetdialog.hpp"
#include "qt-wrappers.hpp"
#include "obs-app.hpp"

#include "ui_cefwidgetdialog.h"

using namespace std;

CefWidgetDialog::CefWidgetDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::CefWidgetDialog)
{
    ui->setupUi(this);
}

CefWidgetDialog::~CefWidgetDialog()
{

}

bool CefWidgetDialog::AskForUrl(QWidget *parent, const QString title,
		const QString &text, std::string &str,
		const QString &placeHolder)
{
	CefWidgetDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.ui->label->setText(text);
	dialog.ui->urlEdit->setMaxLength(INT_MAX);
	dialog.ui->urlEdit->setText(placeHolder);
	dialog.ui->urlEdit->selectAll();

	bool accepted = (dialog.exec() == DialogCode::Accepted);
	if (accepted)
		str = QT_TO_UTF8(dialog.ui->urlEdit->text());

	return accepted;
}

