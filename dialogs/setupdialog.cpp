#include "setupdialog.h"
#include "ui_setupdialog.h"
#include <QFileDialog>
#include "domain/application.h"
#include "util.h"
#include <cstdlib>

SetupDialog::SetupDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SetupDialog)
{
    ui->setupUi(this);
    //read settings from OS registry.
    ui->txtGSLibPath->setText( Application::instance()->getGSLibPathSetting() );
    ui->txtGSPath->setText( Application::instance()->getGhostscriptPathSetting() );
	ui->txtPythonPath->setText( Application::instance()->getPythonPathSetting() );
	ui->spinMaxGridCells3DView->setValue( Application::instance()->getMaxGridCellCountFor3DVisualizationSetting() );
    adjustSize();
}

SetupDialog::~SetupDialog()
{
    delete ui;
}

void SetupDialog::showGSLibPathSearch()
{
    QString dir = QFileDialog::getExistingDirectory(this, "select GSLib executables location", Util::getProgramInstallDir());
    if( ! dir.isEmpty() )
        ui->txtGSLibPath->setText( dir );
}

void SetupDialog::showGSPathSearch()
{
    QString dir = QFileDialog::getExistingDirectory(this, "select Ghostscript installation directory", Util::getProgramInstallDir());
    if( ! dir.isEmpty() )
		ui->txtGSPath->setText( dir );
}

void SetupDialog::showPythonPathSearch()
{
	char * szPythonHome = std::getenv( "PYTHONHOME" );
	std::string pythonHome = Util::getProgramInstallDir().toStdString();
	if( szPythonHome )
		pythonHome = szPythonHome;

	QString dir = QFileDialog::getExistingDirectory(this, "select Python installation directory", QString( pythonHome.c_str() ) );
	if( ! dir.isEmpty() )
		ui->txtPythonPath->setText( dir );
}

void SetupDialog::accept()
{
    //save settings to OS registry.
    Application::instance()->setGSLibPathSetting( ui->txtGSLibPath->text() );
    Application::instance()->setGhostscriptPathSetting( ui->txtGSPath->text() );
	Application::instance()->setPythonPathSetting( ui->txtPythonPath->text() );
	Application::instance()->setMaxGridCellCountFor3DVisualizationSetting( ui->spinMaxGridCells3DView->value() );
    //make dialog close.
    this->reject();
}
