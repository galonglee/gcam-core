#ifndef _TRAN_TECHNOLOGY_H_
#define _TRAN_TECHNOLOGY_H_
#if defined(_MSC_VER)
#pragma once
#endif

/*! 
* \file tran_technology.h
* \ingroup Objects
* \brief The transportation technology class header file.
* \author Sonny Kim
* \date $Date$
* \version $Revision$
*/

#include <xercesc/dom/DOMNode.hpp>
#include "technologies/include/technology.h"

class GDP;

/*! 
* \ingroup Objects
* \brief This transportation technology class is based on the MiniCAM description of technology.
* \author Sonny Kim
*/

class TranTechnology : public technology
{
public:
    TranTechnology();
    TranTechnology* clone() const;
    const std::string& getXMLName1D() const;
    static const std::string& getXMLNameStatic1D();
    void calcCost( const std::string& regionName, const std::string& sectorName, const int per );
    void production( const std::string& regionName, const std::string& prodName,double dmd, const GDP* gdp, const int per);    
    double getIntensity(const int per) const;
    double getCalibrationOutput() const;
protected:
    double techChangeCumm; //!< cummulative technical change
    double loadFactor; //!< vechile load factor
    double vehicleOutput; //!< service per vehicle
    double serviceOutput; //!< service by technology
	double baseScaler; //!< constant scaler to scale base output
    bool XMLDerivedClassParse( const std::string nodeName, const xercesc::DOMNode* curr );
    void toInputXMLDerived( std::ostream& out, Tabs* tabs ) const;
    void toOutputXMLDerived( std::ostream& out, Tabs* tabs ) const;
    void toDebugXMLDerived( const int period, std::ostream& out, Tabs* tabs ) const;
private:
    static const std::string XML_NAME; //!< The XML name of this object.
};

#endif // _TRAN_TECHNOLOGY_H_

