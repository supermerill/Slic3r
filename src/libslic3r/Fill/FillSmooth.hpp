#ifndef slic3r_FillSmooth_hpp_
#define slic3r_FillSmooth_hpp_

#include "../libslic3r.h"

#include "FillBase.hpp"

namespace Slic3r {

class FillSmooth : public Fill
{
public:

    FillSmooth(const PrintRegionConfig* config)
    : config(config)
    {
        nbPass = 2;
        anglePass[0] = 0;
        anglePass[1] = float(M_PI/2);
        anglePass[2] = 0;
        fillPattern[0] = InfillPattern::ipRectilinear;
        fillPattern[1] = InfillPattern::ipRectilinear;
        fillPattern[2] = InfillPattern::ipRectilinear;
        rolePass[0] = erSolidInfill;
        rolePass[1] = erTopSolidInfill;
        rolePass[2] = erSolidInfill;
        percentWidth[0] = 1.0;
        percentWidth[1] = 1.0;
        percentWidth[2] = 1.0;
        percentFlow[0] = 1.0;
        percentFlow[1] = 1.0;
        percentFlow[2] = 0.0;
        double extrusionMult = 1.0;
	if(config)
	{
	  percentFlow[0] *= config->top_infill_flow_ratio;
	  percentFlow[1] *= config->ironing_flow_ratio;
	}
        percentFlow[2] *= extrusionMult;
        has_overlap[0] = false;
        has_overlap[1] = true;
        has_overlap[2] = false;
    }
    virtual Fill* clone() const{ return new FillSmooth(*this); }

    virtual Polylines fill_surface(const Surface *surface, const FillParams &params) override;
    virtual void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override;
    
protected:
    const PrintRegionConfig* config;
    int nbPass=2;
    double percentWidth[3];
    double percentFlow[3];
    float anglePass[3];
    bool has_overlap[3];
    ExtrusionRole rolePass[3];
    InfillPattern fillPattern[3];
    float top_infill_flow_ratio;
    float ironing_flow_ratio;

    void performSingleFill(const int idx, ExtrusionEntityCollection &eecroot, const Surface &srf_source,
        const FillParams &params, const double volume);
    void fillExPolygon(const int idx, ExtrusionEntityCollection &eec, const Surface &srf_to_fill,
        const FillParams &params, const double volume);
};


class FillSmoothTriple : public FillSmooth
{
public:
    FillSmoothTriple(const PrintRegionConfig* config)
    : FillSmooth(config)
    {
        nbPass = 1; //3
        anglePass[0] = 0;
        anglePass[1] = float(M_PI / 2);
        anglePass[2] = float(M_PI / 12); //align with nothing
        fillPattern[0] = InfillPattern::ipHilbertCurve; //ipRectilinear
        fillPattern[1] = InfillPattern::ipConcentric;
        fillPattern[2] = InfillPattern::ipRectilinear;
        rolePass[0] = erTopSolidInfill;//erSolidInfill
        rolePass[1] = erSolidInfill;
        rolePass[2] = erTopSolidInfill;
        percentWidth[0] = 1.4; //0.8
        percentWidth[1] = 1.5;
        percentWidth[2] = 2.8;
        percentFlow[0] = 1; //0.7
        percentFlow[1] = 0.25;
        percentFlow[2] = 0.15;
        double extrusionMult = 1.0; //slight over-extrusion
        percentFlow[0] *= extrusionMult;
        percentFlow[1] *= extrusionMult;
        percentFlow[2] *= extrusionMult;
        has_overlap[0] = true;
        has_overlap[1] = true;
        has_overlap[2] = true;

    }
    virtual Fill* clone() const { return new FillSmoothTriple(*this); }

};

class FillSmoothHilbert : public FillSmooth
{
public:
    FillSmoothHilbert(const PrintRegionConfig* config)
    : FillSmooth(config)
    {
        nbPass = 2;
        anglePass[0] = 0;
        anglePass[1] = float(M_PI / 4);
        anglePass[2] = float(M_PI / 4);
        fillPattern[0] = InfillPattern::ipHilbertCurve; //ipHilbertCurve
        fillPattern[1] = InfillPattern::ipHilbertCurve;
        fillPattern[2] = InfillPattern::ipRectilinear;
        rolePass[0] = erSolidInfill;
        rolePass[1] = erTopSolidInfill;
        rolePass[2] = erTopSolidInfill;
        percentWidth[0] = 1;
        percentWidth[1] = 1.5;
        percentWidth[2] = 1.0;
        percentFlow[0] = 1;
        percentFlow[1] = 0.0;
        percentFlow[2] = 0.0;
        double extrusionMult = 1.0;
        percentFlow[0] *= extrusionMult;
        percentFlow[1] *= extrusionMult;
        percentFlow[2] *= extrusionMult;
        has_overlap[0] = true;
        has_overlap[1] = false;
        has_overlap[2] = true;

    }
    virtual Fill* clone() const { return new FillSmoothHilbert(*this); }

};


} // namespace Slic3r

#endif // slic3r_FillSmooth_hpp_
