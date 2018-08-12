#include "fkestimation.h"
#include "geostats/searchstrategy.h"
#include "domain/datafile.h"
#include "domain/cartesiangrid.h"

#include <QProgressDialog>

FKEstimation::FKEstimation() :
    m_searchStrategy( nullptr ),
    m_variogramModel( nullptr ),
    m_meanSK( 0.0 ),
    m_ktype( KrigingType::OK ),
    m_at_input( nullptr ),
    m_cg_estimation( nullptr )
{
}

void FKEstimation::setSearchStrategy(const SearchStrategy *searchStrategy)
{
    m_searchStrategy = searchStrategy;
}

void FKEstimation::setVariogramModel(const VariogramModel *variogramModel)
{
    m_variogramModel = variogramModel;
}

void FKEstimation::setMeanForSimpleKriging(double meanSK)
{
    m_meanSK = meanSK;
}

void FKEstimation::setKrigingType(KrigingType ktype)
{
    m_ktype = ktype;
}

void FKEstimation::setInputVariable(Attribute *at_input)
{
    m_at_input = at_input;
}

void FKEstimation::setEstimationGrid(CartesianGrid *cg_estimation)
{
    m_cg_estimation = cg_estimation;
}

std::vector<double> FKEstimation::run()
{
    if( ! m_variogramModel ){
        Application::instance()->logError("FKEstimation::run(): variogram model not specified. Aborted.", true);
        return std::vector<double>();
    } else {
        m_variogramModel->readFromFS();
    }

    //Get the data file containing the input variable.
    DataFile *input_datafile = static_cast<DataFile*>( m_at_input->getContainingFile());

    if( ! input_datafile->hasNoDataValue() ){
        Application::instance()->logError("FKEstimation::run(): No-data-value not set for the input dataset. Aborted.", true);
        return std::vector<double>();
    } else {
        bool ok;
        m_NDV_of_input = input_datafile->getNoDataValue().toDouble( &ok );
        if( ! ok ){
            Application::instance()->logError("FKEstimation::run(): No-data-value setting of the input dataset is not a valid number. Aborted.", true);
            return std::vector<double>();
        }
    }

    if( ! m_cg_estimation->hasNoDataValue() ){
        Application::instance()->logError("FKEstimation::run(): No-data-value not set for the estimation grid. Aborted.", true);
        return std::vector<double>();
    } else {
        bool ok;
        m_NDV_of_output = m_cg_estimation->getNoDataValue().toDouble( &ok );
        if( ! ok ){
            Application::instance()->logError("FKEstimation::run(): No-data-value setting of the output grid is not a valid number. Aborted.", true);
            return std::vector<double>();
        }
    }

    //loads data previously to prevent clash with the progress dialog of both data
    //loading and estimation running.
    input_datafile->loadData();
    m_cg_estimation->loadData();

    //get the estimation grid dimensions
    uint nI = m_cg_estimation->getNX();
    uint nJ = m_cg_estimation->getNY();
    uint nK = m_cg_estimation->getNZ();

    Application::instance()->logInfo("Factorial Kriging started...");

    //suspend message reporting as it tends to slow things down.
    Application::instance()->logWarningOff();
    Application::instance()->logErrorOff();

    //estimation takes place in another thread, so we can show and update a progress bar
    //////////////////////////////////
    QProgressDialog progressDialog;
    progressDialog.show();
    progressDialog.setLabelText("Running FK...");
    progressDialog.setMinimum( 0 );
    progressDialog.setValue( 0 );
    progressDialog.setMaximum( nI * nJ * nK );
    QThread* thread = new QThread();
    FKEstimationRunner* runner = new FKEstimationRunner( this, _at );
    runner->moveToThread(thread);
    runner->connect(thread, SIGNAL(finished()), runner, SLOT(deleteLater()));
    runner->connect(thread, SIGNAL(started()), runner, SLOT(doRun()));
    runner->connect(runner, SIGNAL(progress(int)), &progressDialog, SLOT(setValue(int)));
    runner->connect(runner, SIGNAL(setLabel(QString)), &progressDialog, SLOT(setLabelText(QString)));
    thread->start();
    /////////////////////////////////

    //wait for the kriging to finish
    //not very beautiful, but simple and effective
    while( ! runner->isFinished() ){
        thread->wait( 200 ); //reduces cpu usage, refreshes at each 200 milliseconds
        QCoreApplication::processEvents(); //let Qt repaint widgets
    }

    //flushes any messages that have been generated for logging.
    Application::instance()->logWarningOn();
    Application::instance()->logErrorOn();

    std::vector<double> results = runner->getResults();

    delete runner;

    Application::instance()->logInfo("Factorial Kriging completed.");

    return results;
}
