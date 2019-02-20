#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../Surface.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../MedialAxis.hpp"

#include "FillConcentric.hpp"

namespace Slic3r {
    

void FillConcentric::_fill_surface_single(
    const FillParams                &params, 
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction, 
    ExPolygon                       &expolygon, 
    Polylines                       &polylines_out)
{
    // no rotation is supported for this infill pattern
    BoundingBox bounding_box = expolygon.contour.bounding_box();
    
    coord_t min_spacing = scale_(this->spacing);
    coord_t distance = coord_t(min_spacing / params.density);
    
    if (params.density > 0.9999f && !params.dont_adjust) {
        distance = this->_adjust_solid_spacing(bounding_box.size()(0), distance);
        this->spacing = unscale<double>(distance);
    }

    Polygons loops = (Polygons)expolygon;
    Polygons last  = loops;
    while (! last.empty()) {
        last = offset2(last, -(distance + min_spacing/2), +min_spacing/2);
        loops.insert(loops.end(), last.begin(), last.end());
    }

    // generate paths from the outermost to the innermost, to avoid
    // adhesion problems of the first central tiny loops
    loops = union_pt_chained(loops, false);
    
    // split paths using a nearest neighbor search
    size_t iPathFirst = polylines_out.size();
    Point last_pos(0, 0);
    for (const Polygon &loop : loops) {
        polylines_out.push_back(loop.split_at_index(last_pos.nearest_point_index(loop)));
        last_pos = polylines_out.back().last_point();
    }

    // clip the paths to prevent the extruder from getting exactly on the first point of the loop
    // Keep valid paths only.
    size_t j = iPathFirst;
    for (size_t i = iPathFirst; i < polylines_out.size(); ++ i) {
        polylines_out[i].clip_end(this->loop_clipping);
        if (polylines_out[i].is_valid()) {
            if (j < i)
                polylines_out[j] = std::move(polylines_out[i]);
            ++ j;
        }
    }
    if (j < polylines_out.size())
        polylines_out.erase(polylines_out.begin() + j, polylines_out.end());
    //TODO: return ExtrusionLoop objects to get better chained paths,
    // otherwise the outermost loop starts at the closest point to (0, 0).
    // We want the loops to be split inside the G-code generator to get optimum path planning.
}

class ExtrusionSetRole : public ExtrusionVisitor {
    ExtrusionRole new_role;
public:
    ExtrusionSetRole(ExtrusionRole role) : new_role(role) {}
    void use(ExtrusionPath &path) override { std::cout << "change role from " << path.role() << " to " << new_role << "\n"; path.set_role(new_role); }
    void use(ExtrusionPath3D &path3D) override { path3D.set_role(new_role); }
    void use(ExtrusionMultiPath &multipath) override { for (ExtrusionPath path : multipath.paths) path.set_role(new_role); }
    void use(ExtrusionMultiPath3D &multipath) override { for (ExtrusionPath path : multipath.paths) path.set_role(new_role); }
    void use(ExtrusionLoop &loop) override { for (ExtrusionPath path : loop.paths) path.set_role(new_role); }
    void use(ExtrusionEntityCollection &collection) override { for (ExtrusionEntity *entity : collection.entities) entity->visit(*this); }
};

void FillConcentricWGapFill::fill_surface_extrusion(const Surface *surface, const FillParams &params,
    ExtrusionEntitiesPtr &out) {

    // Perform offset.
    Slic3r::ExPolygons expp = offset_ex(surface->expolygon, float(scale_(0 - 0.5 * this->spacing)));
    // Create the infills for each of the regions.
    Polylines polylines_out;
    for (size_t i = 0; i < expp.size(); ++i) {
        //_fill_surface_single(
        //params,
        //surface->thickness_layers,
        //_infill_direction(surface),
        //expp[i],
        //polylines_out);
        ExPolygon expolygon = expp[i];

        coordf_t init_spacing = this->spacing;

        // no rotation is supported for this infill pattern
        BoundingBox bounding_box = expolygon.contour.bounding_box();

        coord_t min_spacing = scale_(this->spacing);
        coord_t distance = coord_t(min_spacing / params.density);

        if (params.density > 0.9999f && !params.dont_adjust) {
            distance = this->_adjust_solid_spacing(bounding_box.size().x(), distance);
            this->spacing = unscaled(distance);
        }

        ExPolygons gaps;
        Polygons loops = (Polygons)expolygon;
        Polygons last = loops;
        while (!last.empty()) {
            Polygons next_onion = offset2(last, -(distance + min_spacing / 2), +min_spacing / 2);
            loops.insert(loops.end(), next_onion.begin(), next_onion.end());
            append(gaps, diff_ex(
                offset(last, -0.5f * distance),
                offset(next_onion, 0.5f * distance + 10)));  // safety offset
            last = next_onion;
        }

        // generate paths from the outermost to the innermost, to avoid
        // adhesion problems of the first central tiny loops
        //note: useless if we don't apply no_sort flag
        //loops = union_pt_chained(loops, false);


        //get the role
        ExtrusionRole good_role = params.role;
        if (good_role == erNone || good_role == erCustom) {
            good_role = (params.flow->bridge ? erBridgeInfill :
                (surface->is_solid() ?
                ((surface->is_top()) ? erTopSolidInfill : erSolidInfill) :
                erInternalInfill));
        }

        ExtrusionEntityCollection *coll_nosort = new ExtrusionEntityCollection();
        coll_nosort->no_sort = true; //can be sorted inside the pass
        extrusion_entities_append_loops(
            coll_nosort->entities, loops,
            good_role,
            params.flow->mm3_per_mm() * params.flow_mult,
            params.flow->width * params.flow_mult,
            params.flow->height);

        //add gapfills
        if (!gaps.empty() && params.density >= 1) {
            // collapse 
            double min = 0.2 * distance * (1 - INSET_OVERLAP_TOLERANCE);
            double max = 2. * distance;
            ExPolygons gaps_ex = diff_ex(
                offset2_ex(gaps, -min / 2, +min / 2),
                offset2_ex(gaps, -max / 2, +max / 2),
                true);
            ThickPolylines polylines;
            for (const ExPolygon &ex : gaps_ex) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (ex.area() > min*max) {
                    ex.medial_axis(ex, max, min, &polylines, params.flow->height);
                }
            }
            if (!polylines.empty() && good_role != erBridgeInfill) {
                ExtrusionEntityCollection gap_fill = thin_variable_width(polylines, erGapFill, *params.flow);
                //set role if needed
                if (good_role != erSolidInfill) {
                    ExtrusionSetRole set_good_role(good_role);
                    gap_fill.visit(set_good_role);
                }
                //move them into the collection
                coll_nosort->append(std::move(gap_fill.entities));
            }
        }

        if (!coll_nosort->entities.empty())
            out.push_back(coll_nosort);
        else delete coll_nosort;
    }
}

} // namespace Slic3r
