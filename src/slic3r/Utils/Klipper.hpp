#ifndef slic3r_Klipper_hpp_
#define slic3r_Klipper_hpp_

#include <boost/algorithm/string.hpp>

#include <string>
#include <wx/string.h>
#include <wx/arrstr.h>
#include <boost/optional.hpp>

#include "OctoPrint.hpp"
#include "libslic3r/PrintConfig.hpp"


namespace Slic3r {

class DynamicPrintConfig;

class Klipper : public OctoPrint
{
public:
    Klipper(DynamicPrintConfig *config);
    ~Klipper() override = default;

    const char* get_name() const;
};

}

#endif
