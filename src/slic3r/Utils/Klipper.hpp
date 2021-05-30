#ifndef slic3r_Klipper_hpp_
#define slic3r_Klipper_hpp_

#include "OctoPrint.hpp"

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
