#include "pythonplugins.h"
#include "domain/application.h"
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>

PythonPlugins::PythonPlugins()
{
}

bool PythonPlugins::test()
{
	QString fullVersion = getPythonVersion();

	//finding the major version number of Python
	QRegularExpression re_version("(\\d+)(\\.\\d+)+");
	QRegularExpressionMatch match = re_version.match( fullVersion );
	int majorVersion = 0;
	if( match.hasMatch() ){
		bool ok;
		majorVersion = match.captured(1).toInt( &ok );
		if ( ! ok ){
			Application::instance()->logError("PythonPlugins::test(): Unable to determine Python major version.");
			return false;
		}
	}

	if( majorVersion < 3 ){
		Application::instance()->logError("PythonPlugins::test(): Python version 3 or higher is required.");
		return false;
	}

	return true;
}

QString PythonPlugins::getPythonVersion()
{
	QString pythonHome = Application::instance()->getPythonPathSetting();
	QString pythonPath;
	if( ! pythonHome.trimmed().isEmpty() )
		pythonPath += pythonHome + "/bin/";

	//execute "python --version"
	QProcess process;
	QString command = pythonPath + "python --version";
	process.start( command );
	if(! process.waitForFinished(-1) ){
		Application::instance()->logError(QString("PythonPlugins::getPythonVersion(): call to external command abnormally ended."));
		QString errorCause;
		switch( process.error() ){
			case QProcess::FailedToStart:
				errorCause = "Command \"" + command + "\" is missing or you lack execution permission on it.";
				break;
			case QProcess::Crashed:
				errorCause = "Command \"" + command + "\" crashed.";
				break;
			case QProcess::Timedout:
				errorCause = "Command execution timed out.";
				break;
			case QProcess::WriteError:
				errorCause = "Could not write to process' input stream.";
				break;
			case QProcess::ReadError:
				errorCause = "Could not read from process' output stream.";
				break;
			default:
				errorCause = "Unknown.";
		}
		Application::instance()->logError(QString("      Cause: ").append(errorCause));
	}

	//read output (strangely, python --version can output to std::err
	QString stdoutput( process.readAllStandardOutput() );
	QString erroutput( process.readAllStandardError() );
	QString output = stdoutput;
	if( stdoutput.trimmed().isEmpty() )
		output = erroutput;

	//finding the version number in command's output
	QRegularExpression re_version("\\d+(\\.\\d+)+");
	QRegularExpressionMatch match = re_version.match( output );
	if( match.hasMatch() ){
		return match.captured(0); //group 0 is the entire matched expression
	} else {
		return "0.0.0";
	}
}
