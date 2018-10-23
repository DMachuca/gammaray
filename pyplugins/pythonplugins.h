#ifndef PYTHONPLUGINS_H
#define PYTHONPLUGINS_H

#include <QString>

/**
 * This is the part of the plug-ins infrastructure that is linked against the
 * main executable.
 */

class PythonPlugins
{
public:
	PythonPlugins();

	/** Returns whether Python is available, working and of supported version.
	 * The Python sought after is the one residing in the path returned by
	 * Application::getPythonPathSetting() which is configured by the user and can be blank.
	 */
	static bool test();

	/**
	 * Returns the Python version string contained in the output of the "<Python directory>/python --version" command.
	 */
	static QString getPythonVersion();
};

#endif // PYTHONPLUGINS_H
