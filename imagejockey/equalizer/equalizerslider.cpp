#include "equalizerslider.h"
#include "ui_equalizerslider.h"

#include "../imagejockeyutils.h"

#include <qwt_slider.h>

EqualizerSlider::EqualizerSlider(double centralFrequency, bool showScale, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::EqualizerSlider),
    m_centralFrequency( centralFrequency ),
	m_previous_dB( 0.0 )
{
    ui->setupUi(this);

    //add the slider
    m_slider = new QwtSlider( Qt::Vertical );
	m_slider->setUpperBound( 100.0 );  //greatest amplification = +100dB
	m_slider->setLowerBound( -100.0 ); //greatest attenuation = -100dB
    if( ! showScale )
        m_slider->setScalePosition( QwtSlider::ScalePosition::NoScale );
    ui->frmSliderPlace->layout()->addWidget( m_slider );
    connect( m_slider, SIGNAL(valueChanged(double)), this, SLOT(setValue(double)) );

    //display the current center frequency
    ui->lblCenterFrequency->setText( ImageJockeyUtils::humanReadable( m_centralFrequency ) );
}

EqualizerSlider::~EqualizerSlider()
{
    delete ui;
}
double EqualizerSlider::centralFrequency() const
{
    return m_centralFrequency;
}

void EqualizerSlider::setCentralFrequency(double centralFrequency)
{
    m_centralFrequency = centralFrequency;
    ui->lblCenterFrequency->setText( ImageJockeyUtils::humanReadable( m_centralFrequency ) );
}

void EqualizerSlider::setValue(double value)
{
    double delta_dB = value - m_previous_dB;
    emit adjustmentMade( m_centralFrequency, delta_dB );
    m_previous_dB = value;
}

