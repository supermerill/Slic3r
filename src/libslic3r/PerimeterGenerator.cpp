#include "PerimeterGenerator.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "BridgeDetector.hpp"
#include "Geometry.hpp"
#include <cmath>
#include <cassert>
#include <vector>

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "Polygon.hpp"
#include "Line.hpp"
#include "ClipperUtils.hpp"
#include "SVG.hpp"
#include "polypartition.h"
#include "poly2tri/poly2tri.h"
#include <algorithm>
#include <cassert>
#include <list>

namespace Slic3r {

    PerimeterGeneratorLoops get_all_Childs(PerimeterGeneratorLoop loop) {
        PerimeterGeneratorLoops ret;
        for (PerimeterGeneratorLoop &child : loop.children) {
            ret.push_back(child);
            PerimeterGeneratorLoops vals = get_all_Childs(child);
            ret.insert(ret.end(), vals.begin(), vals.end());
        }
        return ret;
    }

void PerimeterGenerator::process()
{
    // other perimeters
    this->_mm3_per_mm               = this->perimeter_flow.mm3_per_mm();
    coord_t perimeter_width         = this->perimeter_flow.scaled_width();
    coord_t perimeter_spacing       = this->perimeter_flow.scaled_spacing();
    
    // external perimeters
    this->_ext_mm3_per_mm           = this->ext_perimeter_flow.mm3_per_mm();
    coord_t ext_perimeter_width     = this->ext_perimeter_flow.scaled_width();
    coord_t ext_perimeter_spacing   = this->ext_perimeter_flow.scaled_spacing();
    coord_t ext_perimeter_spacing2  = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);
    
    // overhang perimeters
    this->_mm3_per_mm_overhang      = this->overhang_flow.mm3_per_mm();
    
    // solid infill
    coord_t solid_infill_spacing    = this->solid_infill_flow.scaled_spacing();

    // nozzle diameter
    const double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1);
    
    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the ext_perimeter_spacing calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_perimeter_spacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on 
    // internal flow which is unrelated.
    coord_t min_spacing     = (coord_t)( perimeter_spacing      * (1 - INSET_OVERLAP_TOLERANCE) );
    coord_t ext_min_spacing = (coord_t)( ext_perimeter_spacing  * (1 - INSET_OVERLAP_TOLERANCE) );
    
    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != NULL && this->config->overhangs) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by half the nozzle diameter used 
        // in the current layer
        this->_lower_slices_p = offset(*this->lower_slices, double(scale_(+nozzle_diameter/2)));
    }
    
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    int surface_idx = 0;
    Surfaces all_surfaces = this->slices->surfaces;

    //store surface for bridge infill to avoid unsupported perimeters (but the first one, this one is always good)
    
    if (this->config->no_perimeter_unsupported_algo != npuaNone
        && this->lower_slices != NULL && !this->lower_slices->expolygons.empty()) {

        for (surface_idx = 0; surface_idx < all_surfaces.size(); surface_idx++) {
            Surface *surface = &all_surfaces[surface_idx];
            ExPolygons last = union_ex(surface->expolygon.simplify_p(SCALED_RESOLUTION));
            //compute our unsupported surface
            ExPolygons unsupported = diff_ex(last, this->lower_slices->expolygons, true);
            if (!unsupported.empty()) {
                //remove small overhangs
                ExPolygons unsupported_filtered = offset2_ex(unsupported, double(-perimeter_spacing), double(perimeter_spacing));
                if (!unsupported_filtered.empty()) {
                    //to_draw.insert(to_draw.end(), last.begin(), last.end());
                    //extract only the useful part of the lower layer. The safety offset is really needed here.
                    ExPolygons support = diff_ex(last, unsupported, true);
                    if (!unsupported.empty()) {
                        //only consider the part that can be bridged (really, by the bridge algorithm)
                        //first, separate into islands (ie, each ExPlolygon)
                        int numploy = 0;
                        //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                        ExPolygonCollection lower_island(support);
                        BridgeDetector detector(unsupported_filtered,
                            lower_island,
                            perimeter_spacing);
                        if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value))) {
                            ExPolygons bridgeable = union_ex(detector.coverage(-1, true));
                            if (!bridgeable.empty()) {
                                //check if we get everything or just the bridgeable area
                                if (this->config->no_perimeter_unsupported_algo == npuaNoPeri || this->config->no_perimeter_unsupported_algo == npuaFilled) {
                                    //we bridge everything, even the not-bridgeable bits
                                    for (size_t i = 0; i < unsupported_filtered.size();) {
                                        ExPolygon &poly_unsupp = *(unsupported_filtered.begin() + i);
                                        Polygons contour_simplified = poly_unsupp.contour.simplify(perimeter_spacing);
                                        ExPolygon poly_unsupp_bigger = poly_unsupp;
                                        Polygons contour_bigger = offset(poly_unsupp_bigger.contour, perimeter_spacing);
                                        if (contour_bigger.size() == 1) poly_unsupp_bigger.contour = contour_bigger[0];

                                        //check convex, has some bridge, not overhang
                                        if (contour_simplified.size() == 1 && contour_bigger.size() == 1 && contour_simplified[0].concave_points().size() == 0
                                            && intersection_ex(bridgeable, { poly_unsupp }).size() > 0
                                            && diff_ex({ poly_unsupp_bigger }, last, true).size() == 0) {
                                            //ok, keep it
                                            i++;
                                        } else {
                                            unsupported_filtered.erase(unsupported_filtered.begin() + i);
                                        }
                                    }
                                    unsupported_filtered = intersection_ex(last,
                                        offset2_ex(unsupported_filtered, double(-perimeter_spacing / 2), double(perimeter_spacing * 3 / 2)));
                                    if (this->config->no_perimeter_unsupported_algo == npuaFilled) {
                                        for (ExPolygon &expol : unsupported_filtered) {
                                            //check if the holes won't be covered by the upper layer
                                            //TODO: if we want to do that, we must modify the geometry before making perimeters.
                                            //if (this->upper_slices != nullptr && !this->upper_slices->expolygons.empty()) {
                                            //    for (Polygon &poly : expol.holes) poly.make_counter_clockwise();
                                            //    float perimeterwidth = this->config->perimeters == 0 ? 0 : (this->ext_perimeter_flow.scaled_width() + (this->config->perimeters - 1) + this->perimeter_flow.scaled_spacing());
                                            //    std::cout << "test upper slices with perimeterwidth=" << perimeterwidth << "=>" << offset_ex(this->upper_slices->expolygons, -perimeterwidth).size();
                                            //    if (intersection(Polygons() = { expol.holes }, to_polygons(offset_ex(this->upper_slices->expolygons, -this->ext_perimeter_flow.scaled_width() / 2))).empty()) {
                                            //        std::cout << " EMPTY§";
                                            //        expol.holes.clear();
                                            //    } else {
                                            //    }
                                            //    std::cout << "\n";
                                            //} else {
                                                expol.holes.clear();
                                            //}

                                            //detect inside volume
                                            for (size_t surface_idx_other = 0; surface_idx_other < all_surfaces.size(); surface_idx_other++) {
                                                if (surface_idx == surface_idx_other) continue;
                                                if (intersection_ex(ExPolygons() = { expol }, ExPolygons() = { all_surfaces[surface_idx_other].expolygon }).size() > 0) {
                                                    //this means that other_surf was inside an expol holes
                                                    //as we removed them, we need to add a new one
                                                    ExPolygons new_poly = offset2_ex(all_surfaces[surface_idx_other].expolygon, double(-perimeter_spacing * 2), double(perimeter_spacing));
                                                    if (new_poly.size() == 1) {
                                                        all_surfaces[surface_idx_other].expolygon = new_poly[0];
                                                        expol.holes.push_back(new_poly[0].contour);
                                                        expol.holes.back().make_clockwise();
                                                    } else {
                                                        for (size_t idx = 0; idx < new_poly.size(); idx++) {
                                                            Surface new_surf = all_surfaces[surface_idx_other];
                                                            new_surf.expolygon = new_poly[idx];
                                                            all_surfaces.push_back(new_surf);
                                                            expol.holes.push_back(new_poly[idx].contour);
                                                            expol.holes.back().make_clockwise();
                                                        }
                                                        all_surfaces.erase(all_surfaces.begin() + surface_idx_other);
                                                        if (surface_idx_other < surface_idx) {
                                                            surface_idx--;
                                                            surface = &all_surfaces[surface_idx];
                                                        }
                                                        surface_idx_other--;
                                                    }
                                                }
                                            }
                                        }

                                    }
                                    //TODO: add other polys as holes inside this one (-margin)
                                } else if (this->config->no_perimeter_unsupported_algo == npuaBridgesOverhangs || this->config->no_perimeter_unsupported_algo == npuaBridges){
                                    //simplify to avoid most of artefacts from printing lines.
                                    ExPolygons bridgeable_simplified;
                                    for (ExPolygon &poly : bridgeable) {
                                        poly.simplify(perimeter_spacing, &bridgeable_simplified);
                                    }
                                    bridgeable_simplified = offset2_ex(bridgeable_simplified, -ext_perimeter_width, ext_perimeter_width);
                                    //bridgeable_simplified = intersection_ex(bridgeable_simplified, unsupported_filtered);
                                    //offset by perimeter spacing because the simplify may have reduced it a bit.
                                    //it's not dangerous as it will be intersected by 'unsupported' later
                                    //FIXME: add overlap in this->fill_surfaces->append
                                    //FIXME: it overlap inside unsuppported not-bridgeable area!
                                    double overlap = scale_(this->config->get_abs_value("infill_overlap", unscale<double>(perimeter_spacing)));

                                    //bridgeable_simplified = offset2_ex(bridgeable_simplified, (double)-perimeter_spacing, (double)perimeter_spacing * 2);
                                    //ExPolygons unbridgeable = offset_ex(diff_ex(unsupported, bridgeable_simplified), perimeter_spacing * 3 / 2);
                                    //ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                    //unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);


                                    if (this->config->no_perimeter_unsupported_algo == npuaBridges) {
                                        ExPolygons unbridgeable = unsupported_filtered;
                                        for (ExPolygon &expol : unbridgeable)expol.holes.clear();
                                        unbridgeable = diff_ex(unbridgeable, bridgeable_simplified);
                                        unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width*2, ext_perimeter_width*2);
                                        ExPolygons bridges_temp = intersection_ex(last, diff_ex(unsupported_filtered, unbridgeable));
                                        //remove the overhangs section form the surface polygons
                                        last = diff_ex(last, unsupported_filtered);
                                        //ExPolygons no_bridge = diff_ex(offset_ex(unbridgeable, ext_perimeter_width * 3 / 2), last);
                                        //bridges_temp = diff_ex(bridges_temp, no_bridge);
                                        unsupported_filtered = diff_ex(offset_ex(bridges_temp, ext_perimeter_width * 3 / 2), offset_ex(unbridgeable, ext_perimeter_width*2, jtSquare));
                                    } else {
                                        ExPolygons unbridgeable = intersection_ex(unsupported, diff_ex(unsupported_filtered, offset_ex(bridgeable_simplified, ext_perimeter_width / 2)));
                                        unbridgeable = offset2_ex(unbridgeable, -ext_perimeter_width, ext_perimeter_width);
                                        unsupported_filtered = unbridgeable;

                                        ////put the bridge area inside the unsupported_filtered variable
                                        //unsupported_filtered = intersection_ex(last,
                                        //    diff_ex(
                                        //    offset_ex(bridgeable_simplified, (double)perimeter_spacing / 2),
                                        //    unbridgeable
                                        //    )
                                        //    );
                                    }
                                }
                            } else {
                                unsupported_filtered.clear();
                            }
                        } else {
                            unsupported_filtered.clear();
                        }
                    }

                    if (!unsupported_filtered.empty()) {


                        //add this directly to the infill list.
                        // this will avoid to throw wrong offsets into a good polygons
                        this->fill_surfaces->append(
                            unsupported_filtered,
                            stPosInternal | stDensSparse);

                        // store the results
                        last = diff_ex(last, unsupported_filtered, true);
                        //remove "thin air" polygons (note: it assumes that all polygons below will be extruded)
                        for (int i = 0; i < last.size(); i++) {
                            if (intersection_ex(support, ExPolygons() = { last[i] }).empty()) {
                                this->fill_surfaces->append(
                                    ExPolygons() = { last[i] },
                                    stPosInternal | stDensSparse);
                                last.erase(last.begin() + i);
                                i--;
                            }
                        }
                    }
                }
            }
            if (last.size() == 0) {
                all_surfaces.erase(all_surfaces.begin() + surface_idx);
                surface_idx--;
            } else {
                surface->expolygon = last[0];
                for (size_t idx = 1; idx < last.size(); idx++) {
                    Surface new_surf = *surface;
                    new_surf.expolygon = last[idx];
                    all_surfaces.push_back(new_surf);
                }
            }
        }
    }

    surface_idx = 0;
    for (const Surface &surface : all_surfaces) {
        // detect how many perimeters must be generated for this island
        int        loop_number = this->config->perimeters + surface.extra_perimeters - 1;  // 0-indexed loops
        surface_idx++;

        if (this->config->only_one_perimeter_top && this->upper_slices == NULL){
            loop_number = 0;
        }
        
        ExPolygons gaps;
        //this var store infill surface removed from last to not add any more perimeters to it.
        ExPolygons stored;
        ExPolygons last        = union_ex(surface.expolygon.simplify_p(SCALED_RESOLUTION));
        if (loop_number >= 0) {

            // Add perimeters on overhangs : initialization
            ExPolygons overhangs_unsupported;
            if (this->config->extra_perimeters && !last.empty()
                && this->lower_slices != NULL  && !this->lower_slices->expolygons.empty()) {
                overhangs_unsupported = diff_ex(last, this->lower_slices->expolygons);
                if (!overhangs_unsupported.empty()) {
                    //only consider overhangs and let bridges alone
                    //only consider the part that can be bridged (really, by the bridge algorithm)
                    //first, separate into islands (ie, each ExPlolygon)
                    //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                    const ExPolygonCollection lower_island(diff_ex(last, overhangs_unsupported));
                    BridgeDetector detector(overhangs_unsupported,
                        lower_island,
                        perimeter_spacing);
                    if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value))) {
                        const ExPolygons bridgeable = union_ex(detector.coverage(-1, true));
                        if (!bridgeable.empty()) {
                            //simplify to avoid most of artefacts from printing lines.
                            ExPolygons bridgeable_simplified;
                            for (const ExPolygon &poly : bridgeable) {
                                poly.simplify(perimeter_spacing / 2, &bridgeable_simplified);
                            }

                            if (!bridgeable_simplified.empty())
                                bridgeable_simplified = offset_ex(bridgeable_simplified, double(perimeter_spacing) / 1.9);
                            if (!bridgeable_simplified.empty()) {
                                //offset by perimeter spacing because the simplify may have reduced it a bit.
                                overhangs_unsupported = diff_ex(overhangs_unsupported, bridgeable_simplified);
                            }
                        }
                    }
                }
            }

            // In case no perimeters are to be generated, loop_number will equal to -1.            
            std::vector<PerimeterGeneratorLoops> contours(loop_number+1);    // depth => loops
            std::vector<PerimeterGeneratorLoops> holes(loop_number+1);       // depth => loops
            ThickPolylines thin_walls;
            // we loop one time more than needed in order to find gaps after the last perimeter was applied
            for (int i = 0;; ++ i) {  // outer loop is 0

                

                // We can add more perimeters if there are uncovered overhangs
                // improvement for future: find a way to add perimeters only where it's needed.
                bool has_overhang = false;
                if (this->config->extra_perimeters && !last.empty() && !overhangs_unsupported.empty()) {
                    overhangs_unsupported = intersection_ex(overhangs_unsupported, last);
                    if (overhangs_unsupported.size() > 0) {
                        //add fake perimeters here
                        has_overhang = true;
                    }
                }

                // Calculate next onion shell of perimeters.
                //this variable stored the next onion
                ExPolygons next_onion;
                if (i == 0) {
                    // compute next onion, without taking care of thin_walls : destroy too thin areas.
                    if (!this->config->thin_walls)
                        next_onion = offset_ex(last, double( - ext_perimeter_width / 2));


                    // look for thin walls
                    if (this->config->thin_walls) {
                        // the minimum thickness of a single loop is:
                        // ext_width/2 + ext_spacing/2 + spacing/2 + width/2
                        next_onion = offset2_ex(
                            last,
                            -(float)(ext_perimeter_width / 2 + ext_min_spacing / 2 - 1),
                            +(float)(ext_min_spacing / 2 - 1));
                        // detect edge case where a curve can be split in multiple small chunks.
                        ExPolygons no_thin_onion = offset_ex(last, double( - ext_perimeter_width / 2));
                        float div = 2;
                        while (no_thin_onion.size() > 0 && next_onion.size() > no_thin_onion.size() && no_thin_onion.size() + next_onion.size() > 3) {
                            div -= 0.3;
                            if (div == 2) div -= 0.3;
                            //use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                            ExPolygons next_onion_secondTry = offset2_ex(
                                last,
                                -(float)(ext_perimeter_width / 2 + ext_min_spacing / div - 1),
                                +(float)(ext_min_spacing / div - 1));
                            if (next_onion.size() >  next_onion_secondTry.size() * 1.1) {
                                next_onion = next_onion_secondTry;
                            }
                            if (div > 3 || div < 1.2) break;
                        }

                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = (coord_t)scale_(this->config->thin_walls_min_width.get_abs_value(this->ext_perimeter_flow.nozzle_diameter));
                        
                        ExPolygons no_thin_zone = offset_ex(next_onion, double(ext_perimeter_width / 2), jtSquare);
                        // medial axis requires non-overlapping geometry
                        ExPolygons thin_zones = diff_ex(last, no_thin_zone, true);
                        //don't use offset2_ex, because we don't want to merge the zones that have been separated.
                            //a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
                        ExPolygons half_thins = offset_ex(thin_zones, double(-min_width / 2));
                        //simplify them
                        for (ExPolygon &half_thin : half_thins) {
                            half_thin.remove_point_too_near((float)SCALED_RESOLUTION);
                        }
                        //we push the bits removed and put them into what we will use as our anchor
                        if (half_thins.size() > 0) {
                            no_thin_zone = diff_ex(last, offset_ex(half_thins, double(min_width / 2 - SCALED_EPSILON)), true);
                        }
                        // compute a bit of overlap to anchor thin walls inside the print.
                        for (ExPolygon &half_thin : half_thins) {
                            //growing back the polygon
                            ExPolygons thin = offset_ex(half_thin, double(min_width / 2));
                            assert(thin.size() <= 1);
                            if (thin.empty()) continue;
                            coord_t overlap = (coord_t)scale_(this->config->thin_walls_overlap.get_abs_value(this->ext_perimeter_flow.nozzle_diameter));
                            ExPolygons anchor = intersection_ex(offset_ex(half_thin, double(min_width / 2) +
                                (float)(overlap), jtSquare), no_thin_zone, true);
                            ExPolygons bounds = union_ex(thin, anchor, true);
                            for (ExPolygon &bound : bounds) {
                                if (!intersection_ex(thin[0], bound).empty()) {
                                    //be sure it's not too small to extrude reliably
                                    thin[0].remove_point_too_near((coord_t)SCALED_RESOLUTION);
                                    if (thin[0].area() > min_width*(ext_perimeter_width + ext_perimeter_spacing2)) {
                                        bound.remove_point_too_near((coord_t)SCALED_RESOLUTION);
                                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop
                                        Slic3r::MedialAxis ma{ thin[0], ext_perimeter_width + ext_perimeter_spacing2, 
                                            min_width, coord_t(this->layer_height) };
                                        ma.use_bounds(bound)
                                            .use_min_real_width((coord_t)scale_(this->ext_perimeter_flow.nozzle_diameter))
                                            .use_tapers(overlap)
                                            .build(thin_walls);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    //FIXME Is this offset correct if the line width of the inner perimeters differs
                    // from the line width of the infill?
                    coord_t good_spacing = (i == 1) ? ext_perimeter_spacing2 : perimeter_spacing;
                    if (this->config->thin_walls){
                        // This path will ensure, that the perimeters do not overfill, as in 
                        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                        // excessively, creating gaps, which then need to be filled in by the not very 
                        // reliable gap fill algorithm.
                        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                        // the original.
                        next_onion = offset2_ex(last,
                            -(float)(good_spacing + min_spacing / 2 - 1),
                            +(float)(min_spacing / 2 - 1));

                        ExPolygons no_thin_onion = offset_ex(last, double(-good_spacing));
                        float div = 2;
                        while (no_thin_onion.size() > 0 && next_onion.size() > no_thin_onion.size() && no_thin_onion.size() + next_onion.size() > 3) {
                            div -= 0.3;
                            if (div == 2) div -= 0.3;
                            //use a sightly bigger spacing to try to drastically improve the split, that can lead to very thick gapfill
                            ExPolygons next_onion_secondTry = offset2_ex(
                                last,
                                -(float)(good_spacing + min_spacing / div - 1),
                                +(float)(min_spacing / div - 1));
                            if (next_onion.size() >  next_onion_secondTry.size() * 1.1) {
                                next_onion = next_onion_secondTry;
                            }
                            if (div > 3 || div < 1.2) break;
                        }

                    } else {
                        // If "detect thin walls" is not enabled, this paths will be entered, which 
                        // leads to overflows, as in prusa3d/Slic3r GH #32
                        next_onion = offset_ex(last, double( - good_spacing));
                    }
                    // look for gaps
                    if (this->config->gap_fill_speed.value > 0 && this->config->gap_fill 
                        //check if we are going to have an other perimeter
                        && (i <= loop_number || has_overhang || next_onion.empty()))
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        append(gaps, diff_ex(
                            offset(last, -0.5f*good_spacing),
                            offset(next_onion, 0.5f * good_spacing + 10)));  // safety offset
                }

                if (next_onion.empty()) {
                    // Store the number of loops actually generated.
                    loop_number = i - 1;
                    // No region left to be filled in.
                    last.clear();
                    break;
                } else if (i > loop_number) {
                    if (has_overhang) {
                        loop_number++;
                        contours.emplace_back();
                        holes.emplace_back();
                    } else {
                        // If i > loop_number, we were looking just for gaps.
                        break;
                    }
                }

                for (const ExPolygon &expolygon : next_onion) {
                    //TODO: add width here to allow variable width (if we want to extrude a sightly bigger perimeter, see thin wall)
                    contours[i].emplace_back(PerimeterGeneratorLoop(expolygon.contour, i, true, has_overhang));
                    if (! expolygon.holes.empty()) {
                        holes[i].reserve(holes[i].size() + expolygon.holes.size());
                        for (const Polygon &hole : expolygon.holes)
                            holes[i].emplace_back(PerimeterGeneratorLoop(hole, i, false, has_overhang));
                    }
                }
                last = std::move(next_onion);
                    
                //store surface for top infill if only_one_perimeter_top
                if(i==0 && config->only_one_perimeter_top && this->upper_slices != NULL){
                    //split the polygons with top/not_top
                    ExPolygons upper_polygons(this->upper_slices->expolygons);
                    ExPolygons top_polygons = diff_ex(last, (upper_polygons), true);
                    ExPolygons inner_polygons = diff_ex(last, top_polygons, true);
                    // increase a bit the inner space to fill the frontier between last and stored.
                    stored = union_ex(stored, intersection_ex(offset_ex(top_polygons, double(perimeter_spacing / 2)), last));
                    last = intersection_ex(offset_ex(inner_polygons, double(perimeter_spacing / 2)), last);
                }

                

            }
            
            // re-add stored polygons
            last = union_ex(last, stored);

            // nest loops: holes first
            for (int d = 0; d <= loop_number; ++d) {
                PerimeterGeneratorLoops &holes_d = holes[d];
                // loop through all holes having depth == d
                for (int i = 0; i < (int)holes_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = holes_d[i];
                    // find the hole loop that contains this one, if any
                    for (int t = d+1; t <= loop_number; ++t) {
                        for (int j = 0; j < (int)holes[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = holes[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    // if no hole contains this hole, find the contour loop that contains it
                    for (int t = loop_number; t >= 0; --t) {
                        for (int j = 0; j < (int)contours[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    NEXT_LOOP: ;
                }
            }
            // nest contour loops
            for (int d = loop_number; d >= 1; --d) {
                PerimeterGeneratorLoops &contours_d = contours[d];
                // loop through all contours having depth == d
                for (int i = 0; i < (int)contours_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = contours_d[i];
                    // find the contour loop that contains it
                    for (int t = d - 1; t >= 0; -- t) {
                        for (size_t j = 0; j < contours[t].size(); ++ j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                contours_d.erase(contours_d.begin() + i);
                                --i;
                                goto NEXT_CONTOUR;
                            }
                        }
                    }
                    NEXT_CONTOUR: ;
                }
            }
            // at this point, all loops should be in contours[0] (= contours.front() )
            ExtrusionEntityCollection entities;
            if (config->perimeter_loop.value) {
                //onlyone_perimter = >fusion all perimeterLoops
                for (PerimeterGeneratorLoop &loop : contours.front()) {
                    ExtrusionLoop extr_loop = this->_traverse_and_join_loops(loop, get_all_Childs(loop), loop.polygon.points.front());
                    //ExtrusionLoop extr_loop = this->_traverse_and_join_loops_old(loop, loop.polygon.points.front(), true);
                    extr_loop.paths.back().polyline.points.push_back(extr_loop.paths.front().polyline.points.front());
                    entities.append(extr_loop);
                }

                // append thin walls
                if (!thin_walls.empty()) {
                    ExtrusionEntityCollection tw = thin_variable_width
                        (thin_walls, erExternalPerimeter, this->ext_perimeter_flow);

                    entities.append(tw.entities);
                    thin_walls.clear();
                }
            } else {
                entities = this->_traverse_loops(contours.front(), thin_walls);
            }


            // if brim will be printed, reverse the order of perimeters so that
            // we continue inwards after having finished the brim
            // TODO: add test for perimeter order
            if (this->config->external_perimeters_first || 
                (this->layer_id == 0 && this->print_config->brim_width.value > 0))
                    entities.reverse();
            // append perimeters for this slice as a collection
            if (!entities.empty())
                this->loops->append(entities);
        } // for each loop of an island

        // fill gaps
        if (!gaps.empty()) {
            // collapse 
            double min = 0.2 * perimeter_width * (1 - INSET_OVERLAP_TOLERANCE);
            //be sure we don't gapfill where the perimeters are already touching each other (negative spacing).
            min = std::max(min, double(Flow::new_from_spacing(EPSILON, nozzle_diameter, this->layer_height, false).scaled_width()));
            double max = 2. * perimeter_spacing;
            ExPolygons gaps_ex = diff_ex(
                offset2_ex(gaps, double(-min / 2), double(+min / 2)),
                offset2_ex(gaps, double(-max / 2), double(+max / 2)),
                true);
            ThickPolylines polylines;
            for (const ExPolygon &ex : gaps_ex) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (ex.area() > scale_(scale_(this->config->gap_fill_min_area.get_abs_value(unscaled(perimeter_width)*unscaled(perimeter_width))))) {
                    MedialAxis{ ex, coord_t(max*1.1), coord_t(min), coord_t(this->layer_height) }.build(polylines);
                }
            }
            if (!polylines.empty()) {
                ExtrusionEntityCollection gap_fill = thin_variable_width(polylines, 
                    erGapFill, this->solid_infill_flow);
                this->gap_fill->append(gap_fill.entities);
                /*  Make sure we don't infill narrow parts that are already gap-filled
                    (we only consider this surface's gaps to reduce the diff() complexity).
                    Growing actual extrusions ensures that gaps not filled by medial axis
                    are not subtracted from fill surfaces (they might be too short gaps
                    that medial axis skips but infill might join with other infill regions
                    and use zigzag).  */
                //FIXME Vojtech: This grows by a rounded extrusion width, not by line spacing,
                // therefore it may cover the area, but no the volume.
                last = diff_ex(to_polygons(last), gap_fill.polygons_covered_by_width(10.f));
            }
        }

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset = 
            (loop_number < 0) ? 0 :
            (loop_number == 0) ?
            // one loop
                ext_perimeter_spacing / 2 :
                // two or more loops?
                perimeter_spacing / 2;
        // only apply infill overlap if we actually have one perimeter
        coord_t overlap = 0;
        if (inset > 0) {
            overlap = (coord_t)scale_(this->config->get_abs_value("infill_overlap", unscale<coordf_t>(inset + solid_infill_spacing / 2)));
        }
        // simplify infill contours according to resolution
        Polygons not_filled_p;
        for (ExPolygon &ex : last)
            ex.simplify_p(SCALED_RESOLUTION, &not_filled_p);
        ExPolygons not_filled_exp = union_ex(not_filled_p);
        // collapse too narrow infill areas
        coord_t min_perimeter_infill_spacing = (coord_t)( solid_infill_spacing * (1. - INSET_OVERLAP_TOLERANCE) );
        // append infill areas to fill_surfaces
        //auto it_surf = this->fill_surfaces->surfaces.end();
        ExPolygons infill_exp = offset2_ex(not_filled_exp,
            -inset - min_perimeter_infill_spacing / 2 + overlap,
            (float)min_perimeter_infill_spacing / 2);
        this->fill_surfaces->append(infill_exp, stPosInternal | stDensSparse);
            
        if (overlap != 0) {
            ExPolygons polyWithoutOverlap = offset2_ex(
                not_filled_exp,
                -inset - min_perimeter_infill_spacing / 2,
                (float) min_perimeter_infill_spacing / 2);
            this->fill_no_overlap.insert(this->fill_no_overlap.end(), polyWithoutOverlap.begin(), polyWithoutOverlap.end());
        }
    } // for each island
}


ExtrusionEntityCollection PerimeterGenerator::_traverse_loops(
    const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls) const
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntityCollection coll;
    for (PerimeterGeneratorLoops::const_iterator loop = loops.begin();
        loop != loops.end(); ++loop) {
        bool is_external = loop->is_external();
        
        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop->is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }
        
        // detect overhanging/bridging perimeters
        ExtrusionPaths paths;
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance_type.value == zdNone)) {
            // get non-overhang paths by intersecting this loop with the grown lower slices
            extrusion_paths_append(
                paths,
                intersection_pl(loop->polygon, this->_lower_slices_p),
                role,
                is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm,
                is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width,
                (float) this->layer_height);
            
            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            extrusion_paths_append(
                paths,
                diff_pl(loop->polygon, this->_lower_slices_p),
                erOverhangPerimeter,
                this->_mm3_per_mm_overhang,
                this->overhang_flow.width,
                this->overhang_flow.height);
            
            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = (ExtrusionPaths)ExtrusionEntityCollection(paths).chained_path();
        } else {
            ExtrusionPath path(role);
            path.polyline   = loop->polygon.split_at_first_point();
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm;
            path.width      = is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width;
            path.height     = (float) this->layer_height;
            paths.push_back(path);
        }
        
        coll.append(ExtrusionLoop(paths, loop_role));
    }
    
    // append thin walls to the nearest-neighbor search (only for first iteration)
    if (!thin_walls.empty()) {
        ExtrusionEntityCollection tw = thin_variable_width
            (thin_walls, erExternalPerimeter, this->ext_perimeter_flow);
        
        coll.append(tw.entities);
        thin_walls.clear();
    }
    
    // sort entities into a new collection using a nearest-neighbor search,
    // preserving the original indices which are useful for detecting thin walls
    ExtrusionEntityCollection sorted_coll;
    coll.chained_path(&sorted_coll, false, erMixed, &sorted_coll.orig_indices);
    
    // traverse children and build the final collection
    ExtrusionEntityCollection entities;
    for (std::vector<size_t>::const_iterator idx = sorted_coll.orig_indices.begin();
        idx != sorted_coll.orig_indices.end();
        ++idx) {
        
        if (*idx >= loops.size()) {
            // this is a thin wall
            // let's get it from the sorted collection as it might have been reversed
            size_t i = idx - sorted_coll.orig_indices.begin();
            entities.append(*sorted_coll.entities[i]);
            //if thin extrusion is a loop, make it ccw like a normal contour.
            if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entities.entities.back())) {
                loop->make_counter_clockwise();
            }
        } else {
            const PerimeterGeneratorLoop &loop = loops[*idx];
            ExtrusionLoop eloop = *dynamic_cast<ExtrusionLoop*>(coll.entities[*idx]);
            
            ExtrusionEntityCollection children = this->_traverse_loops(loop.children, thin_walls);
            if (loop.is_contour) {
                if (loop.is_overhang && this->layer_id % 2 == 1)
                    eloop.make_clockwise();
                else
                    eloop.make_counter_clockwise();
                entities.append(children.entities);
                entities.append(eloop);
            } else {
                eloop.make_clockwise();
                entities.append(eloop);
                entities.append(children.entities);
            }
        }
    }
    return entities;
}

PerimeterIntersectionPoint
PerimeterGenerator::_get_nearest_point(const PerimeterGeneratorLoops &children, ExtrusionLoop &myPolylines, const coord_t dist_cut, const coord_t max_dist) const {
    //find best points of intersections
    PerimeterIntersectionPoint intersect;
    intersect.distance = 0x7FFFFFFF; // ! assumption on intersect type & max value
    intersect.idx_polyline_outter = -1;
    intersect.idx_children = -1;
    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            if ((myPolylines.paths[idx_poly].role() == erExternalPerimeter || child.is_external() )
                && this->object_config->seam_position.value != SeamPosition::spRandom) {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    //Try to find a point in the far side, aligning them
                    if (dist + dist_cut / 20 < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter <0 || p.y() > intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            } else {
                //first, try to find 2 point near enough
                for (size_t idx_point = 0; idx_point < myPolylines.paths[idx_poly].polyline.points.size(); idx_point++) {
                    const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                    const Point &nearest_p = *child.polygon.closest_point(p);
                    const double dist = nearest_p.distance_to(p);
                    if (dist + SCALED_EPSILON < intersect.distance || 
                        (config->perimeter_loop_seam.value == spRear && (intersect.idx_polyline_outter<0 || p.y() < intersect.outter_best.y())
                            && dist <= max_dist && intersect.distance + dist_cut / 20)) {
                        //ok, copy the idx
                        intersect.distance = (coord_t)nearest_p.distance_to(p);
                        intersect.idx_children = idx_child;
                        intersect.idx_polyline_outter = idx_poly;
                        intersect.outter_best = p;
                        intersect.child_best = nearest_p;
                    }
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            //second, try to check from one of my points
            //don't check the last point, as it's used to go outter, can't use it to go inner.
            for (size_t idx_point = 1; idx_point < myPolylines.paths[idx_poly].polyline.points.size()-1; idx_point++) {
                const Point &p = myPolylines.paths[idx_poly].polyline.points[idx_point];
                Point nearest_p = child.polygon.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                //std::cout << " child point " << idx_point << "/" << (myPolylines[idx_poly].me.polyline.points.size() - 1 )<< " dist = " << unscale(dist) << " < ? " << unscale(intersect.distance)<<" \n";
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = p;
                    intersect.child_best = nearest_p;
                }
            }
        }
    }
    if (intersect.distance <= max_dist) {
        return intersect;
    }

    for (size_t idx_child = 0; idx_child < children.size(); idx_child++) {
        const PerimeterGeneratorLoop &child = children[idx_child];
        for (size_t idx_poly = 0; idx_poly < myPolylines.paths.size(); idx_poly++) {
            if (myPolylines.paths[idx_poly].extruder_id == (unsigned int)-1) continue;
            if (myPolylines.paths[idx_poly].length() < dist_cut + SCALED_RESOLUTION) continue;

            //lastly, try to check from one of his points
            for (size_t idx_point = 0; idx_point < child.polygon.points.size(); idx_point++) {
                const Point &p = child.polygon.points[idx_point];
                Point nearest_p = myPolylines.paths[idx_poly].polyline.point_projection(p);
                coord_t dist = (coord_t)nearest_p.distance_to(p);
                //if no projection, go to next
                if (dist == 0) continue;
                //if (nearest_p.coincides_with_epsilon(myPolylines.paths[idx_poly].polyline.points.back())) {
                //    Line segment(*(myPolylines.paths[idx_poly].polyline.points.end() - 2), myPolylines.paths[idx_poly].polyline.points.back());
                //    dist = (coord_t)segment.point_at(segment.length() - dist_cut).distance_to(p);
                //}
                //std::cout << " my point " << idx_point << " dist=" << unscale(dist) << " <? " << unscale(intersect.distance) << " \n";
                if (dist + SCALED_EPSILON / 2 < intersect.distance) {
                    //ok, copy the idx
                    intersect.distance = dist;
                    intersect.idx_children = idx_child;
                    intersect.idx_polyline_outter = idx_poly;
                    intersect.outter_best = nearest_p;
                    intersect.child_best = p;
                }
            }
        }
    }
    return intersect;
}


ExtrusionLoop
PerimeterGenerator::_extrude_and_cut_loop(const PerimeterGeneratorLoop &loop, const Point entry_point, const Line &direction) const
{

    bool need_to_reverse = false;
    Polyline initial_polyline;
    const coord_t dist_cut = (coord_t)scale_(this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1));


    if (loop.polygon.points.size() < 3) return ExtrusionLoop(elrDefault);
    if (loop.polygon.length() < dist_cut * 2) {
        ExtrusionLoop single_point(elrDefault);
        Polyline poly_point;
        poly_point.append(loop.polygon.centroid());
        single_point.paths.emplace_back(
            loop.is_external() ? erExternalPerimeter : erPerimeter,
            (double)(loop.is_external() ? this->_ext_mm3_per_mm : this->_mm3_per_mm),
            (float)(loop.is_external() ? this->ext_perimeter_flow.width : this->perimeter_flow.width),
            (float)(this->layer_height));
        single_point.paths.back().polyline = poly_point;
        return single_point;
    }
    //std::cout << "idx_closest_from_entry_point : " << loop.polygon.closest_point_index(entry_point) << "/" << loop.polygon.points.size()<<"\n";
    const size_t idx_closest_from_entry_point = loop.polygon.closest_point_index(entry_point);
    //std::cout << "loop.polygon.points[idx_closest_from_entry_point].distance_to(entry_point) : " << unscale(loop.polygon.points[idx_closest_from_entry_point].distance_to(entry_point)) << "\n";
    if (loop.polygon.points[idx_closest_from_entry_point].distance_to(entry_point) > SCALED_EPSILON) {
        //create new Point
        //get first point
        size_t idx_before = -1;
        for (size_t idx_p_a = 0; idx_p_a < loop.polygon.points.size(); ++idx_p_a) {
            Line l(loop.polygon.points[idx_p_a], loop.polygon.points[(idx_p_a + 1 == loop.polygon.points.size()) ? 0 : (idx_p_a + 1)]);
            if (entry_point.distance_to(l) < SCALED_EPSILON) {
                idx_before = idx_p_a;
                break;
            }
        }
        if (idx_before == (size_t)-1) std::cout << "ERROR: _traverse_and_join_loops : idx_before can't be finded to create new point\n";
        initial_polyline = loop.polygon.split_at_index(idx_before);
        initial_polyline.points.push_back(entry_point);
        initial_polyline.points[0] = entry_point;
    } else {
        initial_polyline = loop.polygon.split_at_index(idx_closest_from_entry_point);
    }
   

    //std::vector<PerimeterPolylineNode> myPolylines;
    ExtrusionLoop my_loop;

    //overhang / notoverhang
    {
        bool is_external = loop.is_external();

        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }

        // detect overhanging/bridging perimeters
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance_type.value == zdNone)) {
            ExtrusionPaths paths;
            // get non-overhang paths by intersecting this loop with the grown lower slices
            extrusion_paths_append(
                paths,
                intersection_pl(initial_polyline, this->_lower_slices_p),
                role,
                is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm,
                is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width,
                (float) this->layer_height);

            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            extrusion_paths_append(
                paths,
                diff_pl(initial_polyline, this->_lower_slices_p),
                erOverhangPerimeter,
                this->_mm3_per_mm_overhang,
                this->overhang_flow.width,
                this->overhang_flow.height);

            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = (ExtrusionPaths)ExtrusionEntityCollection(paths).chained_path();
             
            
            if (direction.length() > 0) {
                Polyline direction_polyline;
                for (ExtrusionPath &path : paths) {
                    direction_polyline.points.insert(direction_polyline.points.end(), path.polyline.points.begin(), path.polyline.points.end());
                }
                direction_polyline.clip_start(SCALED_RESOLUTION);
                direction_polyline.clip_end(SCALED_RESOLUTION);
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }
            if (need_to_reverse) {
                std::reverse(paths.begin(), paths.end());
            }
            //search for the first path
            size_t good_idx = 0; 
            for (size_t idx_path = 0; idx_path < paths.size(); idx_path++) {
                const ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) {
                    if (path.polyline.points.back().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                } else {
                    if (path.polyline.points.front().coincides_with_epsilon(initial_polyline.points.front())) {
                        good_idx = idx_path;
                        break;
                    }
                }
            }
            for (size_t idx_path = good_idx; idx_path < paths.size(); idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
            for (size_t idx_path = 0; idx_path < good_idx; idx_path++) {
                ExtrusionPath &path = paths[idx_path];
                if (need_to_reverse) path.reverse();
                my_loop.paths.emplace_back(path);
            }
        } else {

            if (direction.length() > 0) {
                Polyline direction_polyline = initial_polyline;
                direction_polyline.clip_start(SCALED_RESOLUTION);
                direction_polyline.clip_end(SCALED_RESOLUTION);
                coord_t dot = direction.dot(Line(direction_polyline.points.back(), direction_polyline.points.front()));
                need_to_reverse = dot>0;
            }

            ExtrusionPath path(role);
            path.polyline = initial_polyline;
            if (need_to_reverse) path.polyline.reverse();
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm;
            path.width = is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width;
            path.height = (float)(this->layer_height);
            my_loop.paths.emplace_back(path);
        }

    }
    
    return my_loop;
}

ExtrusionLoop
PerimeterGenerator::_traverse_and_join_loops(const PerimeterGeneratorLoop &loop, const PerimeterGeneratorLoops &children, const Point entry_point) const
{
    //std::cout << " === ==== _traverse_and_join_loops ==== ===\n";
    // other perimeters
    //this->_mm3_per_mm = this->perimeter_flow.mm3_per_mm();
    //coord_t perimeter_width = this->perimeter_flow.scaled_width();
    const coord_t perimeter_spacing = this->perimeter_flow.scaled_spacing();

    //// external perimeters
    //this->_ext_mm3_per_mm = this->ext_perimeter_flow.mm3_per_mm();
    //coord_t ext_perimeter_width = this->ext_perimeter_flow.scaled_width();
    const coord_t ext_perimeter_spacing = this->ext_perimeter_flow.scaled_spacing();
    //coord_t ext_perimeter_spacing2 = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);

    //const coord_t dist_cut = (coord_t)scale_(this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder - 1));
    //TODO change this->external_perimeter_flow.scaled_width() if it's the first one!
    const coord_t max_width_extrusion = this->perimeter_flow.scaled_width();
    ExtrusionLoop my_loop = _extrude_and_cut_loop(loop, entry_point);

    int child_idx = 0;
    //Polylines myPolylines = { myPolyline };
    //iterate on each point ot find the best place to go into the child
    PerimeterGeneratorLoops childs = children;
    while (!childs.empty()) {
        child_idx++;
        PerimeterIntersectionPoint nearest = this->_get_nearest_point(childs, my_loop, this->perimeter_flow.scaled_width(), this->perimeter_flow.scaled_width()* 1.42);
        if (nearest.idx_children == (size_t)-1) {
            //return ExtrusionEntityCollection();
            break;
        } else {
            const PerimeterGeneratorLoop &child = childs[nearest.idx_children];
            //std::cout << "c." << child_idx << " === i have " << my_loop.paths.size() << " paths" << " == cut_path_is_ccw size " << path_is_ccw.size() << "\n";
            //std::cout << "change to child " << nearest.idx_children << " @ " << unscale(nearest.outter_best.x) << ":" << unscale(nearest.outter_best.y)
            //    << ", idxpolyline = " << nearest.idx_polyline_outter << "\n";
            //PerimeterGeneratorLoops less_childs = childs;
            //less_childs.erase(less_childs.begin() + nearest.idx_children);
            //create new node with recursive ask for the inner perimeter & COPY of the points, ready to be cut
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths[nearest.idx_polyline_outter]);

            ExtrusionPath *outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            ExtrusionPath *outer_end = &my_loop.paths[nearest.idx_polyline_outter + 1];
            Line deletedSection;

            //cut our polyline
            //separate them
            size_t nearest_idx_outter = outer_start->polyline.closest_point_index(nearest.outter_best);
            if (outer_start->polyline.points[nearest_idx_outter].coincides_with_epsilon(nearest.outter_best)) {
                if (nearest_idx_outter < outer_start->polyline.points.size() - 1) {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + nearest_idx_outter + 1, outer_start->polyline.points.end());
                }
                if (nearest_idx_outter > 0) {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + nearest_idx_outter);
                }
            } else {
                //get first point
                size_t idx_before = -1;
                for (size_t idx_p_a = 0; idx_p_a < outer_start->polyline.points.size() - 1; ++idx_p_a) {
                    Line l(outer_start->polyline.points[idx_p_a], outer_start->polyline.points[idx_p_a + 1]);
                    if (nearest.outter_best.distance_to(l) < SCALED_EPSILON) {
                        idx_before = idx_p_a;
                        break;
                    }
                }
                if (idx_before == (size_t)-1) {
                    std::cout << "ERROR: idx_before can't be finded\n";
                    continue;
                }

                Points &my_polyline_points = outer_start->polyline.points;
                my_polyline_points.erase(my_polyline_points.begin() + idx_before + 1, my_polyline_points.end());
                my_polyline_points.push_back(nearest.outter_best);

                if (idx_before < outer_end->polyline.points.size()-1)
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.begin() + idx_before + 1);
                else
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin()+1, outer_end->polyline.points.end());
                outer_end->polyline.points.insert(outer_end->polyline.points.begin(), nearest.outter_best);
            }
            Polyline to_reduce = outer_start->polyline;
            if (to_reduce.points.size()>1) to_reduce.clip_end(SCALED_RESOLUTION);
            deletedSection.a = to_reduce.points.back();
            to_reduce = outer_end->polyline;
            if (to_reduce.points.size()>1) to_reduce.clip_start(SCALED_RESOLUTION);
            deletedSection.b = to_reduce.points.front();
            
            //get the inner loop to connect to us.
            ExtrusionLoop child_loop = _extrude_and_cut_loop(child, nearest.child_best, deletedSection);

            const coord_t inner_child_spacing = child.is_external() ? ext_perimeter_spacing : perimeter_spacing;
            const coord_t outer_start_spacing = scale_(outer_start->width - outer_start->height * (1. - 0.25 * PI));
            const coord_t outer_end_spacing = scale_(outer_end->width - outer_end->height * (1. - 0.25 * PI));

            //FIXME: if child_loop has no point or 1 point or not enough space !!!!!!!
            const size_t child_paths_size = child_loop.paths.size();
            if (child_paths_size == 0) continue;
            my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, child_loop.paths.begin(), child_loop.paths.end());
            
            //add paths into my_loop => need to re-get the refs
            outer_start = &my_loop.paths[nearest.idx_polyline_outter];
            outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
            ExtrusionPath *inner_start = &my_loop.paths[nearest.idx_polyline_outter+1];
            ExtrusionPath *inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
            //TRIM
            //choose trim direction
            if (outer_start->polyline.points.size() == 1 && outer_end->polyline.points.size() == 1) {
                //do nothing
            } else if (outer_start->polyline.points.size() == 1) {
                outer_end->polyline.clip_start(outer_end_spacing);
                if (inner_end->polyline.length() > inner_child_spacing)
                    inner_end->polyline.clip_end(inner_child_spacing);
                else
                    inner_end->polyline.clip_end(inner_end->polyline.length() / 2);
            } else if (outer_end->polyline.points.size() == 1) {
                outer_start->polyline.clip_end(outer_start_spacing);
                if (inner_start->polyline.length() > inner_child_spacing)
                    inner_start->polyline.clip_start(inner_child_spacing);
                else
                    inner_start->polyline.clip_start(inner_start->polyline.length()/2);
            } else {
                coord_t length_poly_1 = (coord_t)outer_start->polyline.length();
                coord_t length_poly_2 = (coord_t)outer_end->polyline.length();
                coord_t length_trim_1 = outer_start_spacing / 2;
                coord_t length_trim_2 = outer_end_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    outer_start->polyline.clip_end(length_trim_1);
                } else {
                    outer_start->polyline.points.erase(outer_start->polyline.points.begin() + 1, outer_start->polyline.points.end());
                }
                if (length_poly_2 > length_trim_2) {
                    outer_end->polyline.clip_start(length_trim_2);
                } else {
                    outer_end->polyline.points.erase(outer_end->polyline.points.begin(), outer_end->polyline.points.end() - 1);
                }
                
                length_poly_1 = inner_start->polyline.length();
                length_poly_2 = inner_end->polyline.length();
                length_trim_1 = inner_child_spacing / 2;
                length_trim_2 = inner_child_spacing / 2;
                if (length_poly_1 < length_trim_1) {
                    length_trim_2 = length_trim_1 + length_trim_2 - length_poly_1;
                }
                if (length_poly_2 < length_trim_1) {
                    length_trim_1 = length_trim_1 + length_trim_2 - length_poly_2;
                }
                if (length_poly_1 > length_trim_1) {
                    inner_start->polyline.clip_start(length_trim_1);
                } else {
                    inner_start->polyline.points.erase(
                        inner_start->polyline.points.begin(), 
                        inner_start->polyline.points.end() - 1);
                }
                if (length_poly_2 > length_trim_2) {
                    inner_end->polyline.clip_end(length_trim_2);
                } else {
                    inner_end->polyline.points.erase(
                        inner_end->polyline.points.begin() + 1,
                        inner_end->polyline.points.end());
                }
            }

            //last check to see if we need a reverse
            {
                Line l1(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Line l2(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_inter(0, 0);
                bool is_interect = l1.intersection(l2, &p_inter);
                if (is_interect && p_inter.distance_to(l1) < SCALED_EPSILON && p_inter.distance_to(l2) < SCALED_EPSILON) {
                    //intersection! need to reverse!
                    std::reverse(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1);
                    for (size_t idx = nearest.idx_polyline_outter + 1; idx < nearest.idx_polyline_outter + child_paths_size + 1; idx++) {
                        my_loop.paths[idx].reverse();
                    }
                    outer_start = &my_loop.paths[nearest.idx_polyline_outter];
                    inner_start = &my_loop.paths[nearest.idx_polyline_outter + 1];
                    inner_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size];
                    outer_end = &my_loop.paths[nearest.idx_polyline_outter + child_paths_size + 1];
                }

            }

            //now add extrusionPAths to connect the two loops
            ExtrusionPaths travel_path_begin;// (ExtrusionRole::erNone, 0, outer_start->width, outer_start->height);
            //travel_path_begin.extruder_id = -1;
            ExtrusionPaths travel_path_end;// (ExtrusionRole::erNone, 0, outer_end->width, outer_end->height);
            //travel_path_end.extruder_id = -1;
            double dist_travel = outer_start->polyline.points.back().distance_to(inner_start->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erNone, 0, outer_start->width, outer_start->height);
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm, outer_start->width, outer_start->height);
                travel_path_begin[0].extruder_id = -1;
                travel_path_begin[1].extruder_id = -1;
                travel_path_begin[2].extruder_id = -1;
                Line line(outer_start->polyline.points.back(), inner_start->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel = line.a;
                p_start_travel += p_dist_cut_extrude;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(p_start_travel);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel = line.b;
                p_end_travel -= p_dist_cut_extrude;
                travel_path_begin[2].polyline.append(p_end_travel);
                travel_path_begin[2].polyline.append(inner_start->polyline.points.front());
                //fake travel in the middle
                travel_path_begin[1].polyline.append(p_start_travel);
                travel_path_begin[1].polyline.append(p_end_travel);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_begin.emplace_back(ExtrusionRole::erPerimeter, outer_start->mm3_per_mm * flow_mult, (float)(outer_start->width * flow_mult), outer_start->height);
                travel_path_begin[0].extruder_id = -1;
                travel_path_begin[0].polyline.append(outer_start->polyline.points.back());
                travel_path_begin[0].polyline.append(inner_start->polyline.points.front());
            }
            dist_travel = inner_end->polyline.points.back().distance_to(outer_end->polyline.points.front());
            if (dist_travel > max_width_extrusion*1.5 && this->config->fill_density.value > 0) {
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erNone, 0, outer_end->width, outer_end->height);
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm, outer_end->width, outer_end->height);
                travel_path_end[0].extruder_id = -1;
                travel_path_end[1].extruder_id = -1;
                travel_path_end[2].extruder_id = -1;
                Line line(inner_end->polyline.points.back(), outer_end->polyline.points.front());
                Point p_dist_cut_extrude = (line.b - line.a);
                p_dist_cut_extrude.x() = (coord_t)(p_dist_cut_extrude.x() * ((double)max_width_extrusion) / (line.length() * 2));
                p_dist_cut_extrude.y() = (coord_t)(p_dist_cut_extrude.y() * ((double)max_width_extrusion) / (line.length() * 2));
                //extrude a bit after the turn, to close the loop
                Point p_start_travel_2 = line.a;
                p_start_travel_2 += p_dist_cut_extrude;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(p_start_travel_2);
                //extrude a bit before the final turn, to close the loop
                Point p_end_travel_2 = line.b;
                p_end_travel_2 -= p_dist_cut_extrude;
                travel_path_end[2].polyline.append(p_end_travel_2);
                travel_path_end[2].polyline.append(outer_end->polyline.points.front());
                //fake travel in the middle
                travel_path_end[1].polyline.append(p_start_travel_2);
                travel_path_end[1].polyline.append(p_end_travel_2);
            } else {
                // the path is small enough to extrude all along.
                double flow_mult = 1;
                if (dist_travel > max_width_extrusion && this->config->fill_density.value > 0) {
                    // the path is a bit too long, reduce the extrusion flow.
                    flow_mult = max_width_extrusion / dist_travel;
                }
                travel_path_end.emplace_back(ExtrusionRole::erPerimeter, outer_end->mm3_per_mm * flow_mult, (float)(outer_end->width * flow_mult), outer_end->height);
                travel_path_end[0].extruder_id = -1;
                travel_path_end[0].polyline.append(inner_end->polyline.points.back());
                travel_path_end[0].polyline.append(outer_end->polyline.points.front());
            }
            //check if we add path or reuse bits
            //FIXME
            /*if (outer_start->polyline.points.size() == 1) {
                outer_start->polyline = travel_path_begin.front().polyline;
                travel_path_begin.erase(travel_path_begin.begin());
                outer_start->extruder_id = -1;
            } else if (outer_end->polyline.points.size() == 1) {
                outer_end->polyline = travel_path_end.back().polyline;
                travel_path_end.erase(travel_path_end.end() - 1);
                outer_end->extruder_id = -1;
            }*/
            //add paths into my_loop => after that all ref are wrong!
            for (size_t i = travel_path_end.size() - 1; i < travel_path_end.size(); i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + child_paths_size + 1, travel_path_end[i]);
            }
            for (size_t i = travel_path_begin.size() - 1; i < travel_path_begin.size(); i--) {
                my_loop.paths.insert(my_loop.paths.begin() + nearest.idx_polyline_outter + 1, travel_path_begin[i]);
            }
        }

        //update for next loop
        childs.erase(childs.begin() + nearest.idx_children);
    }

    return my_loop;
}

bool PerimeterGeneratorLoop::is_internal_contour() const
{
    // An internal contour is a contour containing no other contours
    if (! this->is_contour)
                return false;
    for (const PerimeterGeneratorLoop &loop : this->children)
        if (loop.is_contour)
            return false;
        return true;
    }

}
