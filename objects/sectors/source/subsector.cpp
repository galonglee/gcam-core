/*! 
* \file subsector.cpp
* \ingroup CIAM
* \brief Subsector class source file.
* \author Sonny Kim
* \date $Date$
* \version $Revision$
*/

#include "util/base/include/definitions.h"
#include <string>
#include <iostream>
#include <cassert>
#include <vector>
#include <algorithm>
#include <xercesc/dom/DOMNode.hpp>
#include <xercesc/dom/DOMNodeList.hpp>

#include "util/base/include/configuration.h"
#include "sectors/include/subsector.h"
#include "technologies/include/technology.h"
#include "containers/include/scenario.h"
#include "sectors/include/sector.h"
#include "util/base/include/model_time.h"
#include "util/base/include/xml_helper.h"
#include "marketplace/include/marketplace.h"
#include "util/base/include/summary.h"
#include "emissions/include/indirect_emiss_coef.h"
#include "containers/include/world.h"
#include "containers/include/gdp.h"
#include "marketplace/include/market_info.h"

using namespace std;
using namespace xercesc;

extern Scenario* scenario;
// static initialize.
const string Subsector::XML_NAME = "subsector";

/*! \brief Default constructor.
*
* Constructor initializes member variables with default values, sets vector sizes, etc.
*
* \author Sonny Kim, Steve Smith, Josh Lurz
*/
const double LOGIT_EXP_DEFAULT = -3;

Subsector::Subsector( const string regionName, const string sectorName ){
    this->regionName = regionName;
    this->sectorName = sectorName;

    notech = 0;
    tax = 0;
    basesharewt = 0;
    Configuration* conf = Configuration::getInstance();
    debugChecking = conf->getBool( "debugChecking" );
	CO2EmFactor = 0;

    // resize vectors.
    const Modeltime* modeltime = scenario->getModeltime();
    const int maxper = modeltime->getmaxper();
    capLimit.resize( maxper, 1.0 );
    shrwts.resize( maxper, 1.0 ); // default 1.0, for sectors with one tech.
    lexp.resize( maxper, LOGIT_EXP_DEFAULT );
    share.resize(maxper); // subsector shares
    input.resize(maxper); // subsector energy input
    subsectorprice.resize(maxper); // subsector price for all periods
    fuelprice.resize(maxper); // subsector fuel price for all periods
    output.resize(maxper); // total amount of final output from subsector
    summary.resize(maxper); // object containing summaries
    fuelPrefElasticity.resize( maxper );
    summary.resize( maxper );
    calOutputValue.resize( maxper );
    doCalibration.resize( maxper, false );
    calibrationStatus.resize( maxper, false );
    fixedShare.resize( maxper );
    capLimited.resize( maxper, false );
    scaleYear = modeltime->getendyr(); // default year to scale share weight to after calibration
}

/*! \brief Default destructor.
*
* deletes all technology objects associated  with this sector.
*
* \author Josh Lurz
*/
Subsector::~Subsector() {
    clear();
}

//! Clear the Subsector member variables.
void Subsector::clear(){
    for ( vector< vector< technology* > >::iterator outerIter = techs.begin(); outerIter != techs.end(); outerIter++ ) {
        for( vector< technology* >::iterator innerIter = outerIter->begin(); innerIter != outerIter->end(); innerIter++ ) {
            delete *innerIter;
        }
    }
}

/*! \brief Returns sector name
*
* \author Sonny Kim
* \return sector name as a string
*/
const string Subsector::getName() const {
    return name;
}

//! Initialize Subsector with xml data
void Subsector::XMLParse( const DOMNode* node ) {	

    /*! \pre Make sure we were passed a valid node. */
    assert( node );

    // get the name attribute.
    name = XMLHelper<string>::getAttrString( node, "name" );

    // get all child nodes.
    DOMNodeList* nodeList = node->getChildNodes();

    const Modeltime* modeltime = scenario->getModeltime();

    // loop through the child nodes.
    for( unsigned int i = 0; i < nodeList->getLength(); i++ ){
        DOMNode* curr = nodeList->item( i );
        string nodeName = XMLHelper<string>::safeTranscode( curr->getNodeName() );

        if( nodeName == "#text" ) {
            continue;
        }
        else if( nodeName == "capacitylimit" ){
            XMLHelper<double>::insertValueIntoVector( curr, capLimit, modeltime );
        }
        else if( nodeName == "sharewt" ){
            XMLHelper<double>::insertValueIntoVector( curr, shrwts, modeltime );
        }
        else if( nodeName == "calOutputValue" ){
            XMLHelper<double>::insertValueIntoVector( curr, calOutputValue, modeltime );
            int thisPeriod = XMLHelper<double>::getNodePeriod( curr, modeltime );
            doCalibration[ thisPeriod ] = true;
        }
        else if( nodeName == "logitexp" ){
            XMLHelper<double>::insertValueIntoVector( curr, lexp, modeltime );
        }

        else if( nodeName == "fuelprefElasticity" ){
            XMLHelper<double>::insertValueIntoVector( curr, fuelPrefElasticity, modeltime );  
        }

        // basesharewt is not a vector but a single value
        else if( nodeName == "basesharewt" ){
            basesharewt = XMLHelper<double>::getValue( curr );
            share[0] = basesharewt;
        }
        else if( nodeName == "scaleYear" ){
            scaleYear = XMLHelper<int>::getValue( curr );
        }
        else if( nodeName == getChildXMLName() ){
            map<string,int>::const_iterator techMapIter = techNameMap.find( XMLHelper<string>::getAttrString( curr, "name" ) );

            if( techMapIter != techNameMap.end() ) {
                // technology already exists.
                // Check if we should delete. This is a hack.
                if( XMLHelper<bool>::getAttr( curr, "delete" ) ){
                    int vecSpot = techMapIter->second;
                    // Deallocate memory.
                    for( vector<technology*>::iterator iter = techs[ vecSpot ].begin(); iter != techs[ vecSpot ].end(); ++iter ){
                        delete *iter;
                    }
                    // Wipe the vector.
                    vector<vector<technology*> >::iterator delVecIter = techs.begin() + vecSpot;
                    techs.erase( delVecIter );

                    // Wipe out the map.
                    techNameMap.clear();

                    // Reset it.
                    int i = 0;
                    for( vector<vector<technology*> >::const_iterator iter = techs.begin(); iter != techs.end(); ++iter, i++ ){
                        assert( iter->begin() != iter->end() );
                        techNameMap[ (*iter)[ 0 ]->getName() ] = i;
                    }
                } // end hack.
                else {
                    DOMNodeList*childNodeList = curr->getChildNodes();

                    // loop through technologies children.
                    for( unsigned int j = 0; j < childNodeList->getLength(); j++ ){
                        DOMNode* currChild = childNodeList->item( j );
                        string childNodeName = XMLHelper<void>::safeTranscode( currChild->getNodeName() );

                        if( childNodeName == "#text" ){
                            continue;
                        }
                        else if( childNodeName == technology::getXMLNameStatic2D() ){
                            int thisPeriod = XMLHelper<void>::getNodePeriod( currChild, modeltime );
                            techs[ techMapIter->second ][ thisPeriod ]->XMLParse( currChild );
                        }
                    }
                }
            }
            else if( XMLHelper<bool>::getAttr( curr, "nocreate" ) ){
                ILogger& mainLog = ILogger::getLogger( "main_log" );
                mainLog.setLevel( ILogger::WARNING );
                mainLog << "Not creating technology " << XMLHelper<string>::getAttrString( curr, "name" ) 
                    << " in subsector " << name << " because nocreate flag is set." << endl;
            }
            else {
                // technology does not exist, create a new vector of techs.

                DOMNodeList* childNodeList = curr->getChildNodes();
                vector<technology*> techVec( modeltime->getmaxper() );

                // loop through technologies children.
                for( unsigned int j = 0; j < childNodeList->getLength(); j++ ){
                    DOMNode* currChild = childNodeList->item( j );
                    const string childNodeName = XMLHelper<void>::safeTranscode( currChild->getNodeName() );

                    if( childNodeName == "#text" ){
                        continue;
                    }

                    else if( childNodeName == technology::getXMLNameStatic2D() ){
                        auto_ptr<technology> tempTech( createChild() );
                        tempTech->XMLParse( currChild );
                        int thisPeriod = XMLHelper<void>::getNodePeriod( currChild, modeltime );

                        // Check that a technology does not already exist.
                        if( techVec[ thisPeriod ] ){
                            ILogger& mainLog = ILogger::getLogger( "main_log" );
                            mainLog.setLevel( ILogger::DEBUG );
                            mainLog << "Removing duplicate technology " << techVec[ thisPeriod ]->getName() 
                                << " in subsector " << name << " in sector " << sectorName << "." << endl;
                            delete techVec[ thisPeriod ];
                        }

                        techVec[ thisPeriod ] = tempTech.release();

                        // copy technology object for one period to all the periods
                        if ( XMLHelper<bool>::getAttr( currChild, "fillout" ) ) {
                            // will not do if period is already last period or maxperiod
                            for ( int i = thisPeriod + 1; i < modeltime->getmaxper(); i++ ) {
                                // Check that a technology does not already exist.
                                if( techVec[ i ] ){
                                    ILogger& mainLog = ILogger::getLogger( "main_log" );
                                    mainLog.setLevel( ILogger::DEBUG );
                                    mainLog << "Removing duplicate technology " << techVec[ i ]->getName() 
                                        << " in subsector " << name << " in sector " << sectorName << "." << endl;
                                    delete techVec[ i ];
                                }
                                techVec[ i ] = techVec[ thisPeriod ]->clone();
                                techVec[ i ]->setYear( modeltime->getper_to_yr( i ) );
                            } // end for
                        } // end if fillout
                    } // end else if
                } // end for
                techs.push_back( techVec );
                techNameMap[ techVec[ 0 ]->getName() ] = static_cast<int>( techs.size() ) - 1;
            }
        }
        // parsed derived classes
        else if( XMLDerivedClassParse( nodeName, curr ) ){
        }
        else {
            ILogger& mainLog = ILogger::getLogger( "main_log" );
            mainLog.setLevel( ILogger::ERROR );
            mainLog << "Unknown element " << nodeName << " encountered while parsing " << getXMLName() << endl;
        }
    }
}

//! Virtual function which specifies the XML name of the children of this class, the type of technology.
const string& Subsector::getChildXMLName() const {
    return technology::getXMLNameStatic1D();
}

//! Virtual function to generate a child element or construct the appropriate technology.
technology* Subsector::createChild() const {
    return new technology();
}

//! Parses any input variables specific to derived classes
bool Subsector::XMLDerivedClassParse( const string nodeName, const DOMNode* curr ) {
    // do nothing
    // defining method here even though it does nothing so that we do not
    // create an abstract class.
    return false;
}

//! Complete the initialization.
void Subsector::completeInit() {
    mSubsectorInfo.reset( new MarketInfo() );

    // Initialize any arrays that have non-zero default value
    notech = static_cast<int>( techs.size() );
    
    for ( vector< vector< technology* > >::iterator outerIter = techs.begin(); outerIter != techs.end(); outerIter++ ) {
        for( vector< technology* >::iterator innerIter = outerIter->begin(); innerIter != outerIter->end(); innerIter++ ) {
            assert( *innerIter ); // Make sure the technology has been defined.
            ( *innerIter )->completeInit();
        }
    }
}

//! Output the Subsector member variables in XML format.
void Subsector::toInputXML( ostream& out, Tabs* tabs ) const {
    const Modeltime* modeltime = scenario->getModeltime();
	XMLWriteOpeningTag( getXMLName(), out, tabs, name );
    
    // write the xml for the class members.
    for( unsigned i = 0; i < capLimit.size(); i++ ){
        XMLWriteElementCheckDefault( capLimit[ i ], "capacitylimit", out, tabs, 1.0, modeltime->getper_to_yr( i ) );
    }

    XMLWriteElementCheckDefault( scaleYear, "scaleYear", out, tabs, modeltime->getendyr() );

    for( unsigned i = 0; i < calOutputValue.size(); i++ ){
        if ( doCalibration[ i ] ) {
            XMLWriteElementCheckDefault( calOutputValue[ i ], "calOutputValue", out, tabs, 0.0, modeltime->getper_to_yr( i ) );
        }
    }
    
    for( unsigned i = 0; i < shrwts.size(); i++ ){
        XMLWriteElementCheckDefault( shrwts[ i ], "sharewt", out, tabs, 1.0, modeltime->getper_to_yr( i ) );
    }
    
    for( unsigned i = 0; i < lexp.size(); i++ ){
        XMLWriteElementCheckDefault( lexp[ i ], "logitexp", out, tabs, LOGIT_EXP_DEFAULT, modeltime->getper_to_yr( i ) );
    }
    
    for( unsigned i = 0; i < fuelPrefElasticity.size(); i++ ){
        XMLWriteElementCheckDefault( fuelPrefElasticity[ i ], "fuelprefElasticity", out, tabs, 0.0, modeltime->getper_to_yr( i ) );
    }
    
    XMLWriteElementCheckDefault( basesharewt, "basesharewt", out, tabs, 0.0, modeltime->getstartyr( ) );
    toInputXMLDerived( out, tabs );
    // write out the technology objects.
    for( vector< vector< technology* > >::const_iterator j = techs.begin(); j != techs.end(); j++ ){
        
        // If we have an empty vector this won't work, but that should never happen.
        assert( j->begin() != j->end() );
        const technology* firstTech = *( j->begin() ); // Get pointer to first element in row. 
		XMLWriteOpeningTag( firstTech->getXMLName1D(), out, tabs, firstTech->getName() );
        
        for( vector<technology*>::const_iterator k = j->begin(); k != j->end(); k++ ){
            ( *k )->toInputXML( out, tabs );
        }
        
		XMLWriteClosingTag( firstTech->getXMLName1D(), out, tabs );
    }
    
    // finished writing xml for the class members.
    
	XMLWriteClosingTag( getXMLName(), out, tabs );
}

//! XML output for viewing.
void Subsector::toOutputXML( ostream& out, Tabs* tabs ) const {
    const Modeltime* modeltime = scenario->getModeltime();
	XMLWriteOpeningTag( getXMLName(), out, tabs, name );
    
    // write the xml for the class members.
    for( unsigned i = 0; i < capLimit.size(); i++ ){
        XMLWriteElementCheckDefault( capLimit[ i ], "capacitylimit", out, tabs, 1.0, modeltime->getper_to_yr( i ) );
    }
    
    XMLWriteElementCheckDefault( scaleYear, "scaleYear", out, tabs, modeltime->getendyr() );

    for( unsigned i = 0; i < calOutputValue.size(); i++ ){
        if ( doCalibration[ i ] ) {
            XMLWriteElementCheckDefault( calOutputValue[ i ], "calOutputValue", out, tabs, 0.0, modeltime->getper_to_yr( i ) );
        }
    }
    
    for( unsigned i = 0; i < shrwts.size(); i++ ){
        XMLWriteElementCheckDefault( shrwts[ i ], "sharewt", out, tabs, 1.0, modeltime->getper_to_yr( i ) );
    }
    
    for( unsigned i = 0; i < lexp.size(); i++ ){
        XMLWriteElementCheckDefault( lexp[ i ], "logitexp", out, tabs, 0.0, modeltime->getper_to_yr( i ) );
    }
    
    for( unsigned int i = 0; i < fuelPrefElasticity.size(); i++ ){
        XMLWriteElementCheckDefault( fuelPrefElasticity[ i ], "fuelprefElasticity", out, tabs, 0.0, modeltime->getper_to_yr( i ) );
    }
    
    XMLWriteElementCheckDefault( basesharewt, "basesharewt", out, tabs, 0.0, modeltime->getstartyr( ) );
    toOutputXMLDerived( out, tabs );

    // write out the technology objects.
    for( vector< vector< technology* > >::const_iterator j = techs.begin(); j != techs.end(); j++ ){
        
        // If we have an empty vector this won't work, but that should never happen.
        assert( j->begin() != j->end() );
        const technology* firstTech = *( j->begin() ); // Get pointer to first element in row. 
		XMLWriteOpeningTag( firstTech->getXMLName1D(), out, tabs, firstTech->getName() );
        
        for( vector<technology*>::const_iterator k = j->begin(); k != j->end(); k++ ){
            ( *k )->toInputXML( out, tabs );
        }
      
		XMLWriteClosingTag( firstTech->getXMLName1D(), out, tabs );
    }
    
    // finished writing xml for the class members.
    
	XMLWriteClosingTag( getXMLName(), out, tabs );
}

/*! \brief Write information useful for debugging to XML output stream
*
* Function writes market and other useful info to XML. Useful for debugging.
*
* \author Josh Lurz
* \param period model period
* \param out reference to the output stream
*/
void Subsector::toDebugXML( const int period, ostream& out, Tabs* tabs ) const {
    
	XMLWriteOpeningTag( getXMLName(), out, tabs, name );
    
    // Write the xml for the class members.
    XMLWriteElement( unit, "unit", out, tabs );
    XMLWriteElement( fueltype, "fueltype", out, tabs );
    XMLWriteElement( notech, "notech", out, tabs );
    XMLWriteElement( tax, "tax", out, tabs );
    
    // Write the data for the current period within the vector.
    XMLWriteElement( capLimit[ period ], "capLimit", out, tabs );
    XMLWriteElement( shrwts[ period ], "sharewt", out, tabs );
    XMLWriteElement( lexp[ period ], "lexp", out, tabs );
    XMLWriteElement( fuelPrefElasticity[ period ], "fuelprefElasticity", out, tabs );
    XMLWriteElement( share[ period ], "share", out, tabs );
    XMLWriteElement( basesharewt, "basesharewt", out, tabs );
    XMLWriteElement( input[ period ], "input", out, tabs );
    XMLWriteElement( subsectorprice[ period ], "subsectorprice", out, tabs );
    XMLWriteElement( output[ period ], "output", out, tabs );
    toDebugXMLDerived( period, out, tabs );
    // Write out the summary object.
    // summary[ period ].toDebugXML( period, out );
    // write out the technology objects.
    
    for( int j = 0; j < static_cast<int>( techs.size() ); j++ ){
        techs[ j ][ period ]->toDebugXML( period, out, tabs );
    }
    
    // write out the hydrotech. Not yet implemented
    // hydro[ period ].toDebugXML( period, out );
    
    // finished writing xml for the class members.
    
	XMLWriteClosingTag( getXMLName(), out, tabs );
}

/*! \brief Get the XML node name for output to XML.
*
* This public function accesses the private constant string, XML_NAME.
* This way the tag is always consistent for both read-in and output and can be easily changed.
* This function may be virtual to be overriden by derived class pointers.
* \author Josh Lurz, James Blackwood
* \return The constant XML_NAME.
*/
const std::string& Subsector::getXMLName() const {
	return XML_NAME;
}

/*! \brief Get the XML node name in static form for comparison when parsing XML.
*
* This public function accesses the private constant string, XML_NAME.
* This way the tag is always consistent for both read-in and output and can be easily changed.
* The "==" operator that is used when parsing, required this second function to return static.
* \note A function cannot be static and virtual.
* \author Josh Lurz, James Blackwood
* \return The constant XML_NAME as a static.
*/
const std::string& Subsector::getXMLNameStatic() {
	return XML_NAME;
}

/*! \brief Perform any initializations needed for each period.
*
* Any initializations or calcuations that only need to be done once per period (instead of every iteration) should be placed in this function.
*
* \warning the ghg part of this routine assumes the existance of technologies in the previous and future periods
* \author Steve Smith
* \param period Model period
*/
void Subsector::initCalc( const int period ) {
   const Modeltime* modeltime = scenario->getModeltime();
    
    int i = 0;
    // Set any fixed demands
    for ( i=0 ;i<notech; i++ ) {        
        techs[i][ period ]->initCalc( );
        techs[i][ period ]->calcfixedOutput( period );
    }

    setCalibrationStatus( period );
    interpolateShareWeights( period ); 
    fixedShare[ period ] = 0;
    
    // Prevent pathological situation where share is zero where a fixed capacity is present.
    // This can happen at begining of an initialization. Share will be set properly within secotr::calcShare 
    if ( ( getFixedOutput( period ) > 0 ) && ( fixedShare[ period ] == 0 ) ) {
       fixedShare[ period ] = 0.1;
    }
   
    // Prevent pathological situation where a calibration value is present but a capacity limit is imposed. This will not work correctly.
    if ( ( getTotalCalOutputs( period ) > 0 ) && ( capLimit[ period ] < 1 ) ) {
       capLimit[ period ] = 1.0;
    }
   
   // check to see if input fuel has changed
    for ( i=0 ;i<notech && period > 0; i++ ) {
      string prevFuel = techs[i][ period-1 ]->getFuelName();
      if ( prevFuel != techs[i][ period ]->getFuelName() ) {
         cerr << "WARNING: Type of fuel "<< prevFuel << " changed in period " << period << ", tech: ";
         cerr << techs[i][ period ]->getName();
         cerr << ", sub-s: "<< name << ", sect: " << sectorName << ", region: " << regionName << endl;
      }
    }

   // Pass forward any emissions information
    for ( i=0 ;i<notech && period > 0 && period < modeltime->getmaxper() ; i++ ) {
		std::vector<std::string> ghgNames;
		ghgNames = techs[i][period]->getGHGNames();
		
		int numberOfGHGs =  techs[ i ][ period ]->getNumbGHGs();

		if ( numberOfGHGs != techs[i][ period - 1 ]->getNumbGHGs() ) {
			cerr << "WARNING: Number of GHG objects changed in period " << period;
         cerr << " to " << numberOfGHGs <<", tech: ";
			cerr << techs[i][ period ]->getName();
			cerr << ", sub-s: "<< name << ", sect: " << sectorName << ", region: " << regionName << endl;
		}
		// If number of GHG's decreased, then copy GHG objects
		if ( numberOfGHGs < techs[i][ period - 1 ]->getNumbGHGs() ) {
			// Not sure if to impliment this or not
		}
		
		// New method
		if ( period > 1 ) { // Note the hard coded base period
         for ( int j=0 ; j<numberOfGHGs; j++ ) {
            techs[i][ period ]->copyGHGParameters( techs[i][ period - 1]->getGHGPointer( ghgNames[j]  ) );
         } // End For
		}
      
	} // End For
}

/*! \brief Perform any sub-sector level calibration data consistancy checks
*
* \author Steve Smith
* \param period Model period
*/
void Subsector::checkSubSectorCalData( const int period ) {
}


/*! \brief check for fixed demands and set values to counter
*
* Routine flows down to technoogy and sets fixed demands to the appropriate marketplace to be counted
*
* \author Steve Smith
* \param period Model period
*/
void Subsector::tabulateFixedDemands( const int period ) {

    for ( int i=0 ;i<notech; i++ ) {        
        techs[i][ period ]->tabulateFixedDemands(  regionName, period );
   }
}

/*! \brief Computes weighted cost of all technologies in Subsector.
*
* Called from calcShare after technology shares are determined. Calculates share-weighted total price (subsectorprice) and cost of fuel (fuelprice). 
*
* Price function separated to allow different weighting for Subsector price
* changed to void return maw
*
* \author Sonny Kim, Marshall Wise
* \param regionName region name
* \param period Model period
*/
void Subsector::calcPrice( const int period ) {
    const World* world = scenario->getWorld();
	int i=0;
    subsectorprice[period] = 0; // initialize to 0 for summing
    fuelprice[period] = 0; // initialize to 0 for summing
	CO2EmFactor = 0; // initialize to 0 for summing

    for (i=0;i<notech;i++) {
        // calculate weighted average price for Subsector
        subsectorprice[period] += techs[i][period]->getShare()*
            techs[i][period]->getTechcost();
        // calculate weighted average price of fuel only
        // technology shares are based on total cost
        fuelprice[period] += techs[i][period]->getShare()*
            techs[i][period]->getFuelcost();
        // calculate share weighted average CO2 emissions factor
        CO2EmFactor += techs[i][period]->getShare()*
			world->getPrimaryFuelCO2Coef(regionName, techs[i][period]->getFuelName());
    }
}

/*! \brief returns the sector price.
*
* Returns the weighted price from sectorprice variable. See also price method.
*
* \author Sonny Kim
* \param period Model period
*/
double Subsector::getPrice( const int period ) const {
    return subsectorprice[ period ];
}

/*! \brief Returns calibration status.
*
* Since this information in needed often, this is stored in a variable. 
* Can be set just once, since this never changes during an interation.
* See setCalibrationStatus
*
* \author Steve Smith
* \param period Model period
* \pre must be set with setCalibrationStatus
* \return Boolean that is true if sub-sector is calibrated
*/
bool Subsector::getCalibrationStatus( const int period ) const {
    return calibrationStatus[ period ];
}

/*! \brief Returns true if this Subsector, or underlying technologies, are calibrated.
*
* If either the Subsector output, or the output of all the technologies under this Subsector (not including those with zero output) are calibrated, then the calibrationStatus for the sector is set to true.
*
* \author Steve Smith
* \param period Model period
*/
void Subsector::setCalibrationStatus( const int period ) {
    if ( doCalibration[ period ] ) {
        calibrationStatus[ period ] = true;
    } 
	else {
        for (int i=0; i<notech; i++ ) {
            if ( techs[ i ][ period ]->getCalibrationStatus( ) ) {
                calibrationStatus[ period ] = true;
                return;
            }
        }
    }
}

/*! \brief returns Subsector capacity limit.
*
* The capacity limit is in terms of the sector share.
*
* \author Steve Smith
* \param period Model period
* \return Capacity limit for this sub-sector
*/
double Subsector::getCapacityLimit( const int period ) const {
    return capLimit[ period ];
}

/*! \brief sets flag for Subsector capacity limit status.
*
* capLimited is true when the sector has pegged at its capacity limit
*
* \author Steve Smith
* \param value This variable should be renamed and documented.
* \param period Model period
*/
void Subsector::setCapLimitStatus( const bool value, const int period ) {
   capLimited[ period ] = value;
}

/*! \brief returns Subsector capacity limit status.
*
* Status is true when the sector has pegged at its capacity limit for this iteration
*
* \author Steve Smith
* \param period Model period
* \return Boolean capacity limit status
*/
bool Subsector::getCapLimitStatus( const int period ) const {
    return capLimited[ period ];
}

/*! \brief returns Subsector fuel price.
*
* Status is true when the sector has pegged at its capacity limit for this iteration
*
* \author Steve Smith
* \param period Model period
* \return fuel price
*/
double Subsector::getfuelprice(int period) const
{
    return fuelprice[period];
}

/*! \brief returns Subsector CO2 emissions factor.
*
* \author Sonny Kim
* \param period Model period
* \return CO2EmFactor
*/
double Subsector::getCO2EmFactor(int period) const
{
    return CO2EmFactor;
}
/*! \brief returns Subsector fuel price times share
*
* Returns the share-weighted fuel price, which is later summed to get the sector-weighted fuel price (or cost)
*
* \author Sonny Kim
* \param period Model period
* \return share-weighted fuel price
*/
double Subsector::getwtfuelprice(int period) const
{
	double tempShare;
	// base year share
    if (period == 0) {
        tempShare = share[period]; 
    }
	// lagged one period
    else {
        tempShare = share[period-1];
    }
    return tempShare*fuelprice[period];
}

/*! \brief calculate technology shares within Subsector
*
* Calls technology objects to first calculate cost, then their share. Follos this by normalizing shares. 
*
* \author Marshall Weise, Josh Lurz
* \param regionName region name
* \param period model period
* \warning technologies can not independently have fixed outputs at this point
*/
void Subsector::calcTechShares( const GDP* gdp, const int period ) {
    int i=0;
    double sum = 0;
    
    for (i=0;i<notech;i++) {
        // calculate technology cost
        techs[i][period]->calcCost( regionName, sectorName, period );
        // determine shares based on technology costs
        techs[i][period]->calcShare( regionName, gdp, period );
        sum += techs[i][period]->getShare();
    }
    // normalize technology shares to total 100 %
    for (i=0;i<notech;i++) {
        techs[i][period]->normShare(sum);
        // Logit exponential should not be zero or positive when more than one technology
        if(notech>1 && techs[i][period]->getlexp()>=0) {
          // cerr << "Tech for sector " << name << " Logit Exponential is invalid (>= 0)" << endl;
        }
    }
}	


/*! \brief calculate Subsector unnormalized shares
*
* Calculates the un-normalized share for this sector. 
* Also claculates the sector aggregate price (or cost)
*
* \author Sonny Kim, Josh Lurz
* \param regionName region name
* \param period model period
* \param gdp gdp object
* \warning technologies can not independently have fixed outputs
* \warning there is no difference between demand and supply technologies. Control behavior with value of parameter fuelPrefElasticity
*/
void Subsector::calcShare(const int period, const GDP* gdp ) {
   
    // call function to compute technology shares
    calcTechShares( gdp, period );
    // calculate and return Subsector share; uses above price function
    // calc_price() uses normalized technology shares calculated above
    // Logit exponential should not be zero
    
    // compute Subsector weighted average price of technologies
    calcPrice( period );

    // Subsector logit exponential check
    if(lexp[period]==0) cerr << "SubSec Logit Exponential is 0." << endl;
    
    if( subsectorprice[period]==0) {
        share[period] = 0;
    }
    else {
      double scaledGdpPerCapita = gdp->getBestScaledGDPperCap( period );
		share[period] = shrwts[period]*pow(subsectorprice[period],lexp[period])*pow(scaledGdpPerCapita,fuelPrefElasticity[period]);
	}
		
   if (shrwts[period]  > 1e4) {
    cout << "WARNING: Huge shareweight for sub-sector " << name << " : " << shrwts[period] 
         << " in region " << regionName <<endl;
   }
      
   if (share[period] < 0) {
     cerr << "Share is < 0 for " << name << " in " << regionName << endl;
     cerr << "    subsectorprice[period]: " << subsectorprice[period] << endl;
     cerr << "    shrwts[period]: " << shrwts[period] << endl;
   }   
	
}

/*! \brief normalizes Subsector shares
*
* \author Sonny Kim, Josh Lurz
* \param sum sum of sector shares
* \param period model period
* \warning sum must be correct sum of shares
* \pre calc shares must be called first
*/
void Subsector::normShare( const double sum, const int period) {
    if ( sum==0 ) {
        share[period]=0;
    }
    else {
        setShare( share[period] / sum, period );
    }
}

/*!
* \brief normalizes shares to 100% subject to capacity limit.
*
* Used by sector::calcShare() to re-normalize shares, adjusting for capacity limits.
*
* Note that a multiplier is passed, not a divisor. The appropriate multiplier must be calculated by the calling routine.
*
* Sub-sectors that are not subject to a capacity limit get multiplied by mult.
* Capacity limited subsectors are set to their capacity limit.
*
* \author Steve Smith
* \warning The routine assumes that shares are already normalized.
* \param multiplier Multiplier by which to scale shares of all non-capacity limited sub-sectors
* \param period Model period
*/
void Subsector::limitShares( const double multiplier, const int period ) {
   if ( multiplier == 0 ) {
      share[period] = 0;
   }
   else {	
      double capLimitValue = capLimitTransform( capLimit[period], share[period] );
      if ( share[period] >= capLimitValue ) {
         // Only adjust if not already capacity limited
         // need this because can't transform more than once, see capLimitTransform
         if ( !capLimited[ period ] ) {
            setShare( capLimitValue, period );
            setCapLimitStatus( true, period ); // set status to true
         }
      } 
	  else {
         if ( fixedShare[ period ] == 0 ) { // don't change if fixed
            setShare( share[period] * multiplier, period );
         }
      }
   }
}

/*! \brief Transform share to smoothly implement capacity limit.
*
* Function transforms the original share value into one that smoothly approaches the capacity limit.
* Returns the original orgShare when share << capLimit and returns capLimit when orgShare is large by using a logistic transformation.
* 
*
* \author Steve Smith
* \param capLimit capacity limit (share)
* \param orgShare original share for sector
* \return transformed share value
*/
 double Subsector::capLimitTransform( double capLimit, double orgShare ) {
   const double SMALL_NUM = util::getSmallNumber();
   const double exponentValue =  2;
   const double mult =  1.4;
   double newShare = capLimit ;

   if ( capLimit < ( 1 - SMALL_NUM ) ) {
      double factor = exp( pow( mult * orgShare/capLimit , exponentValue ) );
      newShare = orgShare * factor/( 1 + ( orgShare/capLimit ) * factor);
   }
   return newShare;
}

/*! \brief Return the total exogenously fixed technology output for this sector.
*
* \author Steve Smith
* \param period model period
* \pre calc shares must be called first
*/
double Subsector::getFixedOutput( const int period ) const {
    double fixedOutput = 0;
    for ( int i=0 ;i<notech; i++ ) {
        fixedOutput += techs[i][period]->getFixedOutput();
    }
    return fixedOutput;
}

/*!\brief Return the share from this sub-sector that is fixed supply
* Enables communication of fixed share to other classes. 
*This is necessary since, while the amount of fixed supply is available (via getFixedOutput), the total output of a sector is not always known. So this function enables the amount of fixed supply in terms of the sector share to be communicated. 
*
* \author Steve Smith
* \param period Model period
*/
double Subsector::getFixedShare( const int period ) const {
    return fixedShare[ period ];
}

/*! \brief Save the share from this sub-sector that is fixed supply
* Enables communication of fixed share to other classes. See documentation for getFixedShare.
*
* \author Steve Smith
\param period Model period
\param share sector share that is fixed supply
*/
void Subsector::setFixedShare( const int period, const double share ) {
    // option to turn this off during calibration
    // This does not work correctly, shares will not sum to one. -JPL
    // if ( world->getCalibrationSetting() ) {
        fixedShare[ period ] = share;
        if ( share > 1 ) {
            cerr << "Share set to value > 1. Value = " << share << endl;
        }
    // }
}

/*! \brief Set the share from this sub-sector to that saved for fixed supply
* This function changes the share to the share previously saved for the fixed supply.
* This is done instead of using a function to directly set the share in general. 
* Doing this allows the price and calibration routines to operate with an appropriate share.
*
*\author Steve Smith
*\param period Model period
*/
void Subsector::setShareToFixedValue( const int period ) {
   setShare( fixedShare[ period ], period );
}

/*! \brief Reset fixed supply for each technology
* Reset fixed supply to read-in value. This is needed in case the fixed supply had been downscaled to match demand.
* This is done instead of using a function to directly set the share in general. 
* Doing this allows the price and calibration routines to operate with an appropriate share.
*
*\author Steve Smith
*\param period Model period
*/
void Subsector::resetfixedOutput( const int period ) {
    for ( int i=0 ;i<notech; i++ ) {
        techs[ i ][period]->resetfixedOutput(period); // eliminate any previous down-scaleing
    }
}

/*! \brief Scale down fixed supply
* This is use dif the total fixed production is greater than the actual demand. See scalefixedOutput.
*
* \author Steve Smith
* \param period Model period
* \param scaleRatio multiplicative scale factor by which to scale fixed supply
*/
void Subsector::scalefixedOutput( const double scaleRatio, const int period ) {
    // scale fixed technology output down
    for ( int i=0 ;i<notech; i++ ) {
        techs[ i ][ period ]->scalefixedOutput( scaleRatio );
    }
    setFixedShare( period, fixedShare[ period ] * scaleRatio ); 
}

/*! \brief Consistantly adjust share weights after calibration 
* If the sector share weight in the previous period was changed due to calibration, 
* then adjust next few shares so that there is not a big jump in share weights.
*
* Can turn this feature off by setting the scaleYear before the calibration year (e.g., 1975, or even zero)
*
* If scaleYear is set to be the calibration year then shareweights are kept constant
*
* \author Steve Smith
* \param period Model period
* \warning Share weights must be scaled (from sector) before this is called.
*/
void Subsector::interpolateShareWeights( const int period ) {
    const Modeltime* modeltime = scenario->getModeltime();
    
    // if previous period was calibrated, then adjust future shares
     if ( ( period > modeltime->getyr_to_per( 1990 ) ) && calibrationStatus[ period - 1 ] && Configuration::getInstance()->getBool( "CalibrationActive" ) ) {
        // Only scale shareweights if after 1990 and scaleYear is after this period

        int endPeriod = 0;
        if ( scaleYear >= modeltime->getstartyr() ) {
            endPeriod = modeltime->getyr_to_per( scaleYear );
        }
        if  ( endPeriod >= ( period - 1) ) {
            // If begining share weight is zero, then it wasn't changed by calibration so do not scale
           // sjsTEMP. Change this to > zero once other share interps are changed. This was a mistake in the original vers.
             if ( shrwts[ period - 1 ] >= 0 ) {
                shareWeightLinearInterpFn( period - 1, endPeriod );
            }
        }
        
        // Also do this at the technology level if necessary
        if ( notech > 1 ) {
            // First renormalize share weights
            // sjsTEMP. Turn this on once data is updated
          //  normalizeTechShareWeights( period - 1 );

            // Linearlly interpolate shareweights if > 0
            // sjsTEMP. Turn this on once data is updated
         //   techShareWeightLinearInterpFn( period - 1, modeltime->getyr_to_per( modeltime->getendyr() ) );
        }
    }
}

/*! \brief Linearly interpolate share weights between specified endpoints 
* Utility function to linearly scale share weights between two specified points.
*
* \author Steve Smith
* \param beginPeriod Period in which to begin the interpolation.
* \param endPeriod Period in which to end the interpolation.
*/
void Subsector::shareWeightLinearInterpFn( const int beginPeriod,  const int endPeriod ) {
const Modeltime* modeltime = scenario->getModeltime();
double shareIncrement = 0;
    
    int loopPeriod = endPeriod;
    if ( endPeriod > beginPeriod ) {
        shareIncrement = ( shrwts[ endPeriod ] - shrwts[ beginPeriod ] ) / ( endPeriod - beginPeriod );
    } else
    if ( endPeriod == beginPeriod ) {
        // If end period equals the begining period then this is a flag to keep the weights the same, so make increment zero
        // and loop over rest of periods
        loopPeriod = modeltime->getmaxper();  
        shareIncrement = 0;
    }
        
    for ( int period = beginPeriod + 1; period < loopPeriod; period++ ) {
        shrwts[ period ] = shrwts[ period - 1 ] + shareIncrement;
    }

    ILogger& mainLog = ILogger::getLogger( "main_log" );
    mainLog.setLevel( ILogger::DEBUG );
    mainLog << "Shareweights interpolated for subsector " << name << " in sector " << sectorName << " in region " << regionName << endl;

}

/*! \brief Linearly interpolate technology share weights between specified endpoints 
* Utility function to linearly scale technology share weights between two specified points.
*
* \author Steve Smith
* \param beginPeriod Period in which to begin the interpolation.
* \param endPeriod Period in which to end the interpolation.
*/
void Subsector::techShareWeightLinearInterpFn( const int beginPeriod,  const int endPeriod ) {
const Modeltime* modeltime = scenario->getModeltime();
double shareIncrement = 0;
     
    for ( int i=0; i<notech; i++ ) {
        double beginingShareWeight = techs[ i ][ beginPeriod ]->getShareWeight();
        
        // If begining share weight is zero, then it wasn't changed by calibration so do not scale
        if ( beginingShareWeight > 0 ) {
            if ( endPeriod > beginPeriod ) {
                shareIncrement = ( techs[ i ][ endPeriod ]->getShareWeight() - beginingShareWeight );
                shareIncrement /= endPeriod - beginPeriod;
            }
    
            int loopPeriod = endPeriod;
            // If end period equals the begining period then this is a flag to keep the weights the same, so loop over rest of periods
            if ( endPeriod == beginPeriod ) {
                loopPeriod = modeltime->getmaxper();  
                shareIncrement = 0;
            }
            
            for ( int period = beginPeriod + 1; period < loopPeriod; period++ ) {
                 techs[ i ][ period ]->setShareWeight( techs[ i ][ period - 1 ]->getShareWeight() + shareIncrement );
            }
            ILogger& mainLog = ILogger::getLogger( "main_log" );
            mainLog.setLevel( ILogger::DEBUG );
            mainLog << "Shareweights interpolated for technologies in subsector " << name << " in sector " << sectorName << " in region " << regionName << endl;
        }
    }
}

/*! \brief Scales technology share weights so that they equal number of subsectors.
*
* This is needed so that 1) share weights can be easily interpreted (> 1 means favored) and so that
* future share weights can be consistently applied relative to calibrated years.
*
* \author Steve Smith
* \param period Model period
* \warning The routine assumes that all tech outputs are calibrated.
*/
void Subsector::normalizeTechShareWeights( const int period ) {
    
    double shareWeightTotal = 0;
    int numberNonzeroTechs = 0;
    for ( int i=0; i<notech; i++ ) {
        double techShareWeight = techs[ i ][ period ]->getShareWeight();
        shareWeightTotal += techShareWeight;
        if ( techShareWeight > 0 ) {
            numberNonzeroTechs += 1;
        }
    }

    if ( shareWeightTotal == 0 ) {
        ILogger& mainLog = ILogger::getLogger( "main_log" );
        mainLog.setLevel( ILogger::ERROR );
        mainLog << "ERROR: in subsector " << name << " Shareweights sum to zero." << endl;
    } else {
        for ( int i=0; i<notech; i++ ) {
             techs[ i ][ period ]->scaleShareWeight( numberNonzeroTechs / shareWeightTotal );
        }
        ILogger& mainLog = ILogger::getLogger( "main_log" );
        mainLog.setLevel( ILogger::DEBUG );
        mainLog << "Shareweights normalized for technologies in subsector " << name << " in sector " << sectorName << " in region " << regionName << endl;
    }
}

//! Adjusts shares to be consistant with any fixed production 
/*! This routine does two things. 

If this sub-sector has a fixed supply, it sets the share to be consistant with the fixed supply
If this sub-sector does not have a fixed supply, it adjusts the share to be consistant with all the fixed supplies of all other sub-sectors (totalfixedOutput)

\param dmd total demand for all sectors
\param shareRatio amount variable shares need to be adjusted to be consistant with fixed supply
\param totalfixedOutput total fixed supply from all sub-sectors
\param period model period
*/
void Subsector::adjShares( const double demand, double shareRatio, 
                          const double totalfixedOutput, const int period ) {
    double sumSubsectfixedOutput = 0; // total Subsector fixed supply
    double fixedOutput = 0; // fixed supply for each technology
    double varShareTot = 0; // sum of shares without fixed supply
    double subsecdmd; // Subsector demand adjusted with new shares

    // add up the fixed supply and share of non-fixed supply
    for ( int i=0 ;i<notech; i++ ) {
        fixedOutput = techs[i][period]->getFixedOutput();
        sumSubsectfixedOutput += fixedOutput;
        if (fixedOutput == 0) { 
           varShareTot += techs[i][period]->getShare();
        }
    }
    
    // Adjust the share for this Subsector
    // This makes the implicit assumption that the Subsector is either all
    // fixed production or all variable. Would need to amend the logic below
    // to take care of other cases.
    
    // totalfixedOutput is the sector total
    if(totalfixedOutput > 0) {
        if (sumSubsectfixedOutput > 0) {	// This Subsector has a fixed supply
            if ( demand > 0 ) {
                setShare( sumSubsectfixedOutput/demand, period ); 
            }
            else { // no fixed share if no demand
                share[period] = 0; 
            }
        }
        else {	// This Subsector does not have fixed supply 
            if ( demand > 0 ) {
                setShare( share[period] * shareRatio, period ); 
            }
            else {
                share[period] = 0; 
            }  
        } 
    }
    
    // then adjust technology shares to be consistent
    subsecdmd = share[period]*demand; // share is Subsector level
    for (int j=0;j<notech;j++) {
        // adjust tech shares 
        techs[j][period]->adjShares(subsecdmd, sumSubsectfixedOutput, varShareTot, period);
    }
    
}

/*! \brief The demand passed to this function is shared out at the technology level.
* Demand from the "dmd" parameter (could be energy or energy service) is passed to technologies.
*  This is then shared out at the technology level.
*  See also sector::setoutput. 
*
* \author Sonny Kim, Josh Lurz
* \param regionName region name
* \param prodName name of product for this sector
* \param demand Total demand for this product
* \param period Model period
* \pre dmd must be the total demand for this project, so must be called after this has been determined
*/
void Subsector::setoutput( const double demand, const int period, const GDP* gdp ) {
    input[period] = 0; // initialize Subsector total fuel input 
    
    // note that output is in service unit when called from demand sectors
    // multiply dmd by Subsector share go get the total demand to be supplied by this Subsector
    double subsecdmd = share[period]* demand; 
    
    for ( int i=0; i<notech; i++ ) {
        // calculate technology output and fuel input from Subsector output
        techs[i][period]->production( regionName, sectorName, subsecdmd, gdp, period );
        
        // total energy input into Subsector, must call after tech production
        input[period] += techs[i][period]->getInput();
    }
}

/*! \brief Adjusts share weights and Subsector demand to be consistent with calibration value.
* Calibration is performed by scaling share weights to be consistent with the calibration value. 
* Calibration is, therefore, performed as part of the iteration process. 
* Since this can change derivatives, best to turn calibration off when using N-R solver.
*
* This routine adjusts subsector shareweights so that relative shares are correct for each subsector.
* Note that all calibration values are scaled (up or down) according to total sectorDemand 
* -- getting the overall scale correct is the job of the TFE calibration
*
* Routine takes into account fixed supply, which is assumed to take precedence over calibration values
* Note that this routine doesn't notice if the calibration is at the technology or sub-sector level, 
* this is taken care of by routine getTotalCalOutputs.
*
* Routine also calls adjustment to scale technology share weights if necessary.
*
* \author Steve Smith
* \param sectorDemand total demand for this sector
* \param totalfixedOutput total amount of fixed supply for this sector
* \param totalCalOutputs total amount of calibrated outputs for this sector
* \param allFixedOutput flag if all outputs from this sector are calibrated
* \param period Model period
* \warning If calvalue is larger than sector demand nothing is done
* \warning The value of subsecdmd is changed (for sub-sector output calibration)
*/
void Subsector::adjustForCalibration( double sectorDemand, double totalfixedOutput, double totalCalOutputs, const bool allFixedOutput, const int period ) {
   double shareScaleValue = 0;
   double availableDemand;
   double subSectorDemand;

   // total calibrated outputs for this sub-sector
   double calOutputSubsect = getTotalCalOutputs( period );

    // make sure share weights aren't zero or else cann't calibrate
    if ( shrwts[ period ]  == 0 && ( calOutputSubsect > 0 ) ) {
        shrwts[ period ]  = 1;
    }
   
   // Determine available demand that can be shared out (subtract sub-sectors with fixed supply)
   availableDemand = sectorDemand - totalfixedOutput;
   if ( availableDemand < 0 ) {
      availableDemand = 0;
   }
   
   // Next block adjusts calibration values if total cal + fixed demands for this sector 
   // are different from total sector demand passed in.   
   // Do this in all cases, unless calvalues < available demand and all outputs are NOT fixed
   // (if all outputs are not fixed, then the sectors that are not fixed can take up the remaining demand)
   if ( !( ( totalCalOutputs < availableDemand ) && !allFixedOutput ) ) {
     calOutputSubsect = calOutputSubsect * ( availableDemand  / totalCalOutputs );
   }
   
   // Adjust share weights
   subSectorDemand = share[ period ] * sectorDemand;
   if ( subSectorDemand > 0 ) {
      shareScaleValue = calOutputSubsect / subSectorDemand;
      shrwts[ period ]  = shrwts[ period ] * shareScaleValue;
   }
    
   // Check to make sure share weights are not less than zero (and reset if they are)
   if ( shrwts[ period ] < 0 ) {
     cerr << "Share Weight is < 0 in Subsector " << name << endl;
     cerr << "    shrwts[period]: " << shrwts[ period ] << " (reset to 1)" << endl;
     shrwts[ period ] = 1;
   }

   int numberTechs = getNumberAvailTechs( period );
   // Now calibrate technology shares if necessary
   if ( numberTechs > 1 ) {
      for (int j=0;j<notech;j++) {
         // adjust tech shares 
         if ( techs[j][period]->techAvailable( ) ) {
            techs[j][period]->adjustForCalibration( calOutputSubsect );
         }
      }
   }
   
    // Report if share weight gets extremely large
    bool watchSubSector = ( name == "oil" && sectorName == "electricity" && regionName == "Canadaxx");
    if ( debugChecking && (shrwts[ period ] > 1e4 || watchSubSector) ) {
       if ( !watchSubSector ) {
          cout << "In calibration for sub-sector: " << name;
          cout << " in sector: "<< sectorName << " in region: " << regionName << endl;
       } else { cout << " ||" ; }
    }
}
  
/*! \brief returns the total of technologies available this period
*
* Technologies are available if they exist and shareweights are not zero
*
* \author Steve Smith
* \param period Model period
* \return Number of available technologies
*/
int Subsector::getNumberAvailTechs( const int period ) const {
int numberAvailable = 0;

   for ( int i=0; i<notech; i++ ) {
      // calculate technology output and fuel input from Subsector output
      if ( techs[i][period]->techAvailable( ) ) {
         numberAvailable += 1;
      }
   }
   return numberAvailable;
}

/*! \brief returns the total calibrated output from this sector.
*
* Routine adds up calibrated values from both the sub-sector and (if not calibrated at Subsector), technology levels.
* This returns only calibrated outputs, not values otherwise fixed (as fixed or zero share weights)
*
* \author Steve Smith
* \param period Model period
* \return Total calibrated output for this Subsector
*/
double Subsector::getTotalCalOutputs( const int period ) const {
    double sumCalValues = 0;

   if ( doCalibration[ period ] ) {
      sumCalValues += calOutputValue[ period ];
   } 
   else {
   	for ( int i=0; i<notech; i++ ) {
         if ( techs[ i ][ period ]->getCalibrationStatus( ) ) {
            
            if ( debugChecking ) {
              if ( techs[ i ][ period ]->getCalibrationOutput( ) < 0 ) {
                 cerr << "calibration < 0 for tech " << techs[ i ][ period ]->getName() 
                      << " in Subsector " << name << endl;
              }
            }

            sumCalValues += techs[ i ][ period ]->getCalibrationOutput( );
         }
      }
   }
   
   return sumCalValues;
}

/*! \brief returns the total calibrated or fixed input from this sector for the specified good.
*
* Routine adds up calibrated or fixed input values from all technologies.
*
* \author Steve Smith
* \param period Model period
* \param goodName market good to return inputs for. If equal to the value "allInputs" then returns all inputs.
* \param bothVals optional parameter. It true (default) both calibration and fixed values are returned, if false only calInputs
* \return Total calibrated input for this Subsector
*/
double Subsector::getCalAndFixedInputs( const int period, const std::string& goodName, const bool bothVals ) const {
    double sumCalInputValues = 0;

    for ( int i=0; i<notech; i++ ) {
        if ( techHasInput( techs[ i ][ period ], goodName ) || ( goodName == "allInputs" ) ) {
            if ( techs[ i ][ period ]->getCalibrationStatus( ) ) {
                sumCalInputValues += techs[ i ][ period ]->getCalibrationInput( );
            } 
            else if ( techs[ i ][ period ]->ouputFixed( ) && bothVals ) {
                sumCalInputValues += techs[ i ][ period ]->getFixedInput( );
            }
        }
    }
    return sumCalInputValues;
}

/*! \brief returns the total calibrated or fixed input from this sector for the specified good.
*
* Routine adds up calibrated or fixed input values from all technologies.
*
* \author Steve Smith
* \param period Model period
* \param goodName market good to return inputs for. If equal to the value "allInputs" then returns all inputs.
* \param bothVals optional parameter. It true (default) both calibration and fixed values are returned, if false only calInputs
* \return Total calibrated input for this Subsector
*/
double Subsector::getCalAndFixedOutputs( const int period, const std::string& goodName, const bool bothVals ) const {
    double sumCalOutputValues = 0;

    for ( int i=0; i<notech; i++ ) {
        if ( techHasInput( techs[ i ][ period ], goodName ) || ( goodName == "allInputs" ) ) {
            if ( techs[ i ][ period ]->getCalibrationStatus( ) ) {
                sumCalOutputValues += techs[ i ][ period ]->getCalibrationOutput( );
            } 
            else if ( techs[ i ][ period ]->ouputFixed( ) && bothVals ) {
                sumCalOutputValues += techs[ i ][ period ]->getFixedOutput( );
            }
        }
    }
    return sumCalOutputValues;
}

/*! \brief Calculates the input value needed to produce the required output
*
* \author Steve Smith
* \param period Model period
* \param goodName market good to determine the inputs for.
* \param requiredOutput Amount of output to produce
*/
bool Subsector::setImpliedFixedInput( const int period, const std::string& goodName, const double requiredOutput ) {
    Marketplace* marketplace = scenario->getMarketplace();
    bool inputWasChanged = false;
    for ( int i=0; i<notech; i++ ) {
        if ( techHasInput( techs[ i ][ period ], goodName ) ) {
            double inputValue = requiredOutput / techs[ i ][ period ]->getEff();
            if ( !inputWasChanged ) {
                inputWasChanged = true;
                double existingMarketDemand = max( marketplace->getMarketInfo( goodName, regionName , period, "calDemand" ), 0.0 );
                marketplace->setMarketInfo( goodName, regionName, period, "calDemand", existingMarketDemand + inputValue );
            } 
            else {
                ILogger& mainLog = ILogger::getLogger( "main_log" );
                mainLog.setLevel( ILogger::WARNING );
                mainLog << "  WARNING: More than one technology input would have been changed " 
                    << " in sub-sector " << name << " in sector " << sectorName
                    << " in region " << regionName << endl; 
            }
        }
    }
    return inputWasChanged;
}

/*! \brief returns true if inputs are all fixed for this subsector and input good
*
* \author Steve Smith
* \param period Model period
* \param goodName market good to return inputs for. If equal to the value "allInputs" then returns all inputs.
* \return boolean true if inputs of specified good are fixed
*/
bool Subsector::inputsAllFixed( const int period, const std::string& goodName ) const {
    bool allInputsFixed = false;

    // test for each method of fixing output, if none of these are true then demand is not all fixed
    for ( int i=0; i<notech; i++ ) {
        if ( techHasInput( techs[ i ][ period ], goodName ) || ( goodName == "allInputs" ) ) {
            if ( ( techs[ i ][ period ]->getCalibrationStatus( ) ) ) {
                allInputsFixed = true;
            } 
            else if ( techs[ i ][ period ]->ouputFixed( ) != 0 ) {
                allInputsFixed =  true;
            } else if ( shrwts[ period ] == 0) {
                allInputsFixed = true;
            } else {
                return false;
            }
        }
    }

    return true;
}

/*! \brief checks to see if technology demands the specified good
*
* \author Steve Smith
* \warning This routine depends on technologies being named for their fuel type or if fuelname is equal to the good. 
* This works currently for electricity, but will not for other techs. Need to impliment a more robust method of checking calibrations.
* \param goodName market good to check for
* \param pointer to technology to consider
* \return True if the specified technology has goodname as input
* \todo Need a more robust way of doing this check (requires a more fundamental change to the way calibrated inputs and outputs are found)
*/
bool Subsector::techHasInput( const technology* thisTech, const std::string& goodName ) const {
	
	return ( thisTech->getFuelName() == goodName );
	
}

/*! \brief Scales calibrated values for the specified good.
*
* \author Steve Smith
* \param period Model period
* \param goodName market good to return inputs for
* \param scaleValue multipliciative scaler for calibrated values 
* \return Total calibrated input for this Subsector
*/
void Subsector::scaleCalibratedValues( const int period, const std::string& goodName, const double scaleValue ) {

	for ( int i=0; i<notech; i++ ) {
		if ( techHasInput( techs[ i ][ period ], goodName ) ) {
			if ( techs[ i ][ period ]->getCalibrationStatus( ) ) {
				techs[ i ][ period ]->scaleCalibrationInput( scaleValue );
			} 
		}
   }
}

/*! \brief returns true if all output is either fixed or calibrated.
*
* If output is is calibrated, fixed, or share weight is zero for this Subsector or all technologies in this sub-sector returns true.
*
* \author Steve Smith
* \param period Model period
* \return Total calibrated output for this Subsector
*/
bool Subsector::allOuputFixed( const int period ) const {
    bool oneNotFixed = false;
    bool outputFixed = false;

    if ( doCalibration[ period ] ) {
        outputFixed = true;  // this sector has fixed output
    } 
    else  if ( shrwts[ period ] == 0 ) {
        outputFixed = true; // this sector has no output, so is also fixed
    }
    // if not fixed at sub-sector level, then check at the technology level
    else {
        for ( int i=0; i<notech; i++ ) {
            if ( !( techs[ i ][ period ]->ouputFixed( ) ) ) {
                oneNotFixed = true;
            }
        }
    }

    if ( outputFixed ) {
        return true;
    } else {
        return !oneNotFixed;
    }
}

/*! \brief scale calibration values.
*
* Scale calibration values in each technology by specified amount. 
*
* \author Steve Smith
* \param period Model period
* \param scaleFactor Multiplicitive scale factor for each calibration value
*/
void Subsector::scaleCalibrationInput( const int period, const double scaleFactor ) {
    for ( int i=0; i<notech; i++ ) {
        techs[ i ][ period ]->scaleCalibrationInput( scaleFactor );
    }
}

/*! \brief returns share weight for this Subsector
*
* Needed so that share weights can be scaled by sector
*
* \author Steve Smith
* \param period Model period
* \return share weight
*/
double Subsector::getShareWeight( const int period ) const {
    return shrwts[ period ];
}

/*! \brief Scales share weight for this Subsector
*
* \author Steve Smith
* \param period Model period
* \param scaleValue Multipliciatve scale factor for shareweight
*/
void Subsector::scaleShareWeight( const double scaleValue, const int period ) {
    
    if ( scaleValue != 0 ) {
        shrwts[ period ] *= scaleValue;
    }
}
/*! \brief returns share for this Subsector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \pre calcShare
* \return share value
*/
double Subsector::getShare( const int period ) const {
    return share[period];
}

/*! \brief set share for this Subsector with normalization check
*
* Use this function to set the share at any time where shares are supposed to be normalized
*
* \author Steve Smith
* \param shareVal Value to which to set the share.
* \param period Model period
*/
void Subsector::setShare( const double shareVal, const int period ) {
const double tinyNumber = util::getVerySmallNumber();

   share[ period ] = shareVal;
   if ( shareVal > (1 + tinyNumber ) ) {
      cerr << "ERROR - share value set > 1. Val: " << shareVal << endl;
   }
}

//! write Subsector output to database
void Subsector::csvOutputFile() const {
    // function protocol
    void fileoutput3( string var1name,string var2name,string var3name,
        string var4name,string var5name,string uname,vector<double> dout);
    
    const Modeltime* modeltime = scenario->getModeltime();
    const int maxper = modeltime->getmaxper();
    vector<double> temp(maxper);
    
    // function arguments are variable name, double array, db name, table name
    // the function writes all years
    // total Subsector output
    fileoutput3( regionName,sectorName,name," ","production","EJ",output);
    // Subsector price
    fileoutput3( regionName,sectorName,name," ","price","$/GJ(ser)",subsectorprice);
    // Subsector carbon taxes paid
    for( int m = 0; m < maxper; m++ ){
        temp[ m ] = getTotalCarbonTaxPaid( m );
    }
    fileoutput3( regionName, sectorName, name, " ", "C tax paid", "Mil90$", temp );

    for ( int m=0;m<maxper;m++){
        temp[m] = summary[m].get_emissmap_second("CO2");
    }
    fileoutput3( regionName,sectorName,name," ","CO2 emiss","MTC",temp);

    // do for all technologies in the Subsector
    for ( int i=0;i<notech;i++) {
        // sjs -- bad coding here, hard-wired period. But is difficult to do something different with current output structure. This is just for csv file.
        int numberOfGHGs =  techs[ i ][ 2 ]->getNumbGHGs();
        vector<string> ghgNames;
        ghgNames = techs[i][ 2 ]->getGHGNames();		
        for ( int ghgN =0; ghgN <= ( numberOfGHGs - 1 ); ghgN++ ) {
            if ( ghgNames[ ghgN ] != "CO2" ) {
                for ( int m=0;m<maxper;m++) {
                    temp[m] = techs[i][ m ]->get_emissmap_second( ghgNames[ ghgN ] );
                }
                fileoutput3( regionName,sectorName,name,techs[i][ 2 ]->getName(), ghgNames[ ghgN ] + " emiss","Tg",temp);
            }
        }

        // output or demand for each technology
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getOutput();
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"production","EJ",temp);
        // technology share
        if(notech>1) {
            for ( int m=0;m<maxper;m++) {
                temp[m] = techs[i][m]->getShare();
            }
            fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"tech share","%",temp);
        }
        // technology cost
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getTechcost();
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"price","$/GJ",temp);
        
        // ghg tax paid
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getCarbonTaxPaid( regionName, m );
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"C tax paid","90Mil$",temp);
        // technology fuel input
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getInput();
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"fuel consump","EJ",temp);
        // technology efficiency
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getEff();
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"efficiency","%",temp);
        // technology non-energy cost
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getNecost();
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"non-energy cost","$/GJ",temp);
        // technology CO2 emission
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->get_emissmap_second("CO2");
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"CO2 emiss","MTC",temp);
        // technology indirect CO2 emission
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->get_emissmap_second("CO2ind");
        }
        fileoutput3( regionName,sectorName,name,techs[i][ 0 ]->getName(),"CO2 emiss(ind)","MTC",temp);
    }
    
    csvDerivedClassOutput();
}

//! Outputs any variables specific to derived classes
void Subsector::csvDerivedClassOutput() const {
    // do nothing
}

/*! \brief Write supply sector MiniCAM style Subsector output to database.
*
* Writes outputs with titles and units appropriate to supply sectors.
*
* \author Sonny Kim
*/
void Subsector::MCoutputSupplySector() const {
    // function protocol
    void dboutput4(string var1name,string var2name,string var3name,string var4name,
        string uname,vector<double> dout);
    
    const Modeltime* modeltime = scenario->getModeltime();
    const int maxper = modeltime->getmaxper();
    const double CVRT_90 = 2.212; //  convert '75 price to '90 price
    vector<double> temp(maxper);
    
    // total Subsector output
    dboutput4(regionName,"Secondary Energy Prod",sectorName,name,"EJ",output);
    // Subsector price
    dboutput4(regionName,"Price",sectorName,name,"75$/GJ",subsectorprice);
    // for electricity sector only
    if (sectorName == "electricity") {
        for ( int m=0;m<maxper;m++) {
            temp[m] = subsectorprice[m] * CVRT_90 * 0.36;
        }
        dboutput4(regionName,"Price",sectorName+" C/kWh",name,"90C/kWh",temp);
    }
    
    // do for all technologies in the Subsector
    for ( int i=0;i<notech;i++) {
        // technology non-energy cost
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getNecost();
        }
        dboutput4( regionName, "Price NE Cost", sectorName, techs[i][ 0 ]->getName(), "75$/GJ", temp );
        // secondary energy and price output by tech
        // output or demand for each technology
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getOutput();
        }
        dboutput4( regionName, "Secondary Energy Prod", sectorName + "_tech", techs[i][ 0 ]->getName(), "EJ", temp );
        // technology cost
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getTechcost() * CVRT_90;
        }
        dboutput4( regionName, "Price", sectorName + "_tech", techs[i][ 0 ]->getName(), "90$/GJ", temp );
    }
}

/*! \brief Write demand sector MiniCAM style Subsector output to database.
*
* Writes outputs with titles and units appropriate to demand sectors.
* Part B is for demand sector, titles and units are different from Part A
*
* \author Sonny Kim
*/
void Subsector::MCoutputDemandSector() const {
    // function protocol
    void dboutput4(string var1name,string var2name,string var3name,string var4name,
        string uname,vector<double> dout);
    const Modeltime* modeltime = scenario->getModeltime();
    const int maxper = modeltime->getmaxper();
    vector<double> temp(maxper);
    
    // total Subsector output
    dboutput4(regionName,"End-Use Service",sectorName+" by Subsec",name,"Ser Unit",output);
    dboutput4(regionName,"End-Use Service",sectorName+" "+name,"zTotal","Ser Unit",output);
    // Subsector price
    dboutput4(regionName,"Price",sectorName,name+" Tot Cost","75$/Ser",subsectorprice);
    
    // do for all technologies in the Subsector
    for ( int i=0;i<notech;i++) {
        if(notech>1) {  // write out if more than one technology
            // output or demand for each technology
            for ( int m=0;m<maxper;m++) {
                temp[m] = techs[i][m]->getOutput();
            }
            dboutput4(regionName,"End-Use Service",sectorName+" "+name,techs[i][ 0 ]->getName(),"Ser Unit",temp);
            // total technology cost
            for ( int m=0;m<maxper;m++) {
                temp[m] = techs[i][m]->getTechcost();
            }
            dboutput4(regionName,"Price",sectorName+" "+name,techs[i][ 0 ]->getName(),"75$/Ser",temp);
           // technology fuel cost
            for ( int m=0;m<maxper;m++) {
                temp[m] = techs[i][m]->getFuelcost();
            }
            dboutput4( regionName,"Price",sectorName+" "+name+" Fuel Cost",techs[i][ 0 ]->getName(),"75$/Ser",temp);
            // technology non-energy cost
            for ( int m=0;m<maxper;m++) {
                temp[m] = techs[i][m]->getNecost();
            }
            dboutput4( regionName, "Price", sectorName + " " + name + " NE Cost", techs[i][ 0 ]->getName(), "75$/Ser", temp );
        }
    }
}

/*! \brief Write common MiniCAM style Subsector output to database.
*
* Writes outputs that are common to both supply and demand sectors.
*
* \author Sonny Kim
*/
void Subsector::MCoutputAllSectors() const {
    // function protocol
    void dboutput4(string var1name,string var2name,string var3name,string var4name,
        string uname,vector<double> dout);
    const Modeltime* modeltime = scenario->getModeltime();
    const int maxper = modeltime->getmaxper();
    vector<double> temp(maxper);
    
    // Subsector carbon taxes paid
    // Subsector carbon taxes paid
    for( int m = 0; m < maxper; m++ ){
        temp[ m ] = getTotalCarbonTaxPaid( m );
    }
    dboutput4( regionName, "General", "CarbonTaxPaid by subsec", sectorName + name, "$", temp );
    // Subsector share 
    dboutput4(regionName,"Subsec Share",sectorName,name,"100%",share);
    // Subsector emissions for all greenhouse gases
    // fuel consumption by Subsector
    dboutput4( regionName, "Fuel Consumption", sectorName + " by Subsec", name, "EJ", input );
    
    // subsector CO2 emission. How is this different then below?
    for( int i = 0; i < notech; ++i ){
        for ( int m = 0; m < maxper; m++ ) {
            // this gives Subsector total CO2 emissions
            // get CO2 emissions for each technology
            temp[m] = techs[i][m]->get_emissmap_second("CO2");
        }
    }
    dboutput4( regionName, "CO2 Emiss", sectorName, name, "MTC", temp );

    typedef map<string,double>::const_iterator CI;
    map<string,double> temissmap = summary[0].getemission(); // get gas names for period 0
    for (CI gmap=temissmap.begin(); gmap!=temissmap.end(); ++gmap) {
        for ( int m=0;m<maxper;m++) {
            temp[m] = summary[m].get_emissmap_second(gmap->first);
        }
        dboutput4( regionName, "Emissions",  "Subsec-" + sectorName + "_" + name, gmap->first, "MTC", temp );
    }
    
    // do for all technologies in the Subsector
    for ( int i = 0; i < notech; i++ ) {
        const string subsecTechName = name + techs[i][ 0 ]->getName();
        // technology indirect CO2 emission
        for( int m=0;m<maxper;m++) {
            temp[m] = summary[m].get_emindmap_second("CO2");
        }
        dboutput4(regionName,"CO2 Emiss(ind)",sectorName, subsecTechName,"MTC",temp);
        // technology share
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getShare();
        }
        dboutput4(regionName,"Tech Share",sectorName, subsecTechName,"%",temp);

        // ghg tax and storage cost applied to technology if any
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getTotalGHGCost();
        }
        dboutput4(regionName,"Total GHG Cost",sectorName, subsecTechName,"$/gj",temp);

        // ghg tax paid
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getCarbonTaxPaid( regionName, m );
        }
        dboutput4(regionName,"C Tax Paid",sectorName, subsecTechName,"90Mil$",temp);

        // technology fuel input
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getInput();
        }
        dboutput4( regionName,"Fuel Consumption", sectorName + " by Technology " + subsecTechName, techs[i][0]->getFuelName(), "EJ", temp );

        // for 1 or more technologies
        // technology efficiency
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getEff();
        }
        dboutput4(regionName,"Tech Efficiency",sectorName, subsecTechName,"%",temp);
        for ( int m=0;m<maxper;m++) {
            temp[m] = techs[i][m]->getIntensity(m);
        }
        dboutput4(regionName,"Tech Intensity", sectorName, subsecTechName,"In/Out",temp);
    }

    MCDerivedClassOutput( );

}

//! Outputs any variables specific to derived classes
void Subsector::MCDerivedClassOutput( ) const {
    // do nothing
}

//! calculate GHG emissions from annual production of each technology
void Subsector::emission( const int period ){
    /*! \pre period is less than or equal to max period. */
    assert( period <= scenario->getModeltime()->getmaxper() );
    summary[period].clearemiss(); // clear emissions map
    summary[period].clearemfuelmap(); // clear emissions map

    for ( int i = 0; i < notech; i++ ) {
        techs[i][period]->calcEmission( sectorName );
        summary[period].updateemiss( techs[i][period]->getemissmap() );
        summary[period].updateemfuelmap( techs[i][period]->getemfuelmap() );
    }
}

//! calculate indirect GHG emissions from annual production of each technology
void Subsector::indemission( const int period, const vector<Emcoef_ind>& emcoef_ind ) {
    /*! \pre period is less than or equal to max period. */
    assert( period <= scenario->getModeltime()->getmaxper() );
    summary[period].clearemindmap(); // clear emissions map
    for ( int i=0 ;i<notech; i++ ) {
        techs[i][period]->indemission( emcoef_ind );
        summary[period].updateemindmap(techs[i][period]->getemindmap());
    }
}


/*! \brief returns (energy) input to sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return sector input
*/
double Subsector::getInput( const int period ) const {
    /*! \pre period is less than or equal to max period. */
    assert( period <= scenario->getModeltime()->getmaxper() );
    
    return input[period];
}

/*! \brief calculates fuel input and Subsector output.
*
* Sums technology output to get total sector output
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
*/
void Subsector::sumOutput( const int period ) {
    output[period] = 0;
    for ( int i=0 ;i<notech; i++ ) {
        output[period] += techs[i][period]->getOutput();
    }
}

/*! \brief returns Subsector output
*
* output summed every time to ensure consistency
* this is never called for demand sectors!
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return sector output
*/
double Subsector::getOutput( const int period ) {
    /*! \pre period is less than or equal to max period. */
   assert( period <= scenario->getModeltime()->getmaxper() );
   sumOutput( period );
   return output[period];
}

/*! \brief returns total Subsector carbon taxes paid
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return total carbon taxes paid by this sub-sector
*/
double Subsector::getTotalCarbonTaxPaid( const int period ) const {
    /*! \pre period is less than or equal to max period. */
    assert( period <= scenario->getModeltime()->getmaxper() );
    double sum = 0;
    for( unsigned int i = 0; i < techs.size(); ++i ){
        sum += techs[ i ][ period ]->getCarbonTaxPaid( regionName, period );
    }
    return sum;
}

/*! \brief returns gets fuel consumption map for this sub-sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \pre updateSummary
* \todo Sonny or Josh -- is this precondition correct? Please edit (I'm not sure I understand when these functions are valid) 
* \return fuel consumption map
*/
map<string, double> Subsector::getfuelcons( const int period ) const {
    /*! \pre period is less than or equal to max period. */
    assert( period <= scenario->getModeltime()->getmaxper() );
    
    return summary[period].getfuelcons();
}

/*! \brief clears fuel consumption map for this sub-sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
*/
void Subsector::clearfuelcons( const int period ) {
    summary[ period ].clearfuelcons();
}

/*! \brief returns GHG emissions map for this sub-sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return GHG emissions map
*/
map<string, double> Subsector::getemission( const int period ) const {
    return summary[ period ].getemission();
}

/*! \brief returns map of GHG emissions by fuel for this sub-sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return map of GHG emissions by fuel
*/
map<string, double> Subsector::getemfuelmap( const int period ) const {
    return summary[ period ].getemfuelmap();
}

/*! \brief returns map of indirect GHG emissions for this sub-sector
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
* \return map of indirect GHG emissions
*/
map<string, double> Subsector::getemindmap( const int period ) const {
    return summary[ period ].getemindmap();
}

/*! \brief update summaries for reporting
*
* \author Sonny Kim, Josh Lurz
* \param period Model period
*/
void Subsector::updateSummary( const int period ) {
    // clears Subsector fuel consumption map
    summary[period].clearfuelcons();
    
    for ( int i = 0; i < notech; i++ ) {
        summary[period].initfuelcons( techs[i][0]->getFuelName(), techs[i][period]->getInput() );
    }
}

