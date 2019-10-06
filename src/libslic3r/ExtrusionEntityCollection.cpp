#include "ExtrusionEntityCollection.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {

ExtrusionEntityCollection::ExtrusionEntityCollection(const ExtrusionPaths &paths)
    : no_sort(false)
{
    this->append(paths);
}

ExtrusionEntityCollection& ExtrusionEntityCollection::operator= (const ExtrusionEntityCollection &other)
{
    this->entities      = other.entities;
    for (size_t i = 0; i < this->entities.size(); ++i)
        this->entities[i] = this->entities[i]->clone();
    this->orig_indices  = other.orig_indices;
    this->no_sort       = other.no_sort;
    return *this;
}

void
ExtrusionEntityCollection::swap(ExtrusionEntityCollection &c)
{
    std::swap(this->entities, c.entities);
    std::swap(this->orig_indices, c.orig_indices);
    std::swap(this->no_sort, c.no_sort);
}

void ExtrusionEntityCollection::clear()
{
	for (size_t i = 0; i < this->entities.size(); ++i)
		delete this->entities[i];
    this->entities.clear();
}

ExtrusionEntityCollection::operator ExtrusionPaths() const
{
    ExtrusionPaths paths;
    for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it) {
        if (const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(*it))
            paths.push_back(*path);
    }
    return paths;
}

ExtrusionEntityCollection*
ExtrusionEntityCollection::clone() const
{
    ExtrusionEntityCollection* coll = new ExtrusionEntityCollection(*this);
    for (size_t i = 0; i < coll->entities.size(); ++i)
        coll->entities[i] = this->entities[i]->clone();
    return coll;
}

void
ExtrusionEntityCollection::reverse()
{
    for (ExtrusionEntitiesPtr::iterator it = this->entities.begin(); it != this->entities.end(); ++it) {
        // Don't reverse it if it's a loop, as it doesn't change anything in terms of elements ordering
        // and caller might rely on winding order
        if (!(*it)->can_reverse()) (*it)->reverse();
    }
    std::reverse(this->entities.begin(), this->entities.end());
}

void
ExtrusionEntityCollection::replace(size_t i, const ExtrusionEntity &entity)
{
    delete this->entities[i];
    this->entities[i] = entity.clone();
}

void
ExtrusionEntityCollection::remove(size_t i)
{
    delete this->entities[i];
    this->entities.erase(this->entities.begin() + i);
}

ExtrusionEntityCollection
ExtrusionEntityCollection::chained_path(bool no_reverse, ExtrusionRole role) const
{
    ExtrusionEntityCollection coll;
    this->chained_path(&coll, no_reverse, role);
    return coll;
}

void
ExtrusionEntityCollection::chained_path(ExtrusionEntityCollection* retval, bool no_reverse, ExtrusionRole role, std::vector<size_t>* orig_indices) const
{
    if (this->entities.empty()) return;
    this->chained_path_from(this->entities.front()->first_point(), retval, no_reverse, role, orig_indices);
}

ExtrusionEntityCollection ExtrusionEntityCollection::chained_path_from(Point start_near, bool no_reverse, ExtrusionRole role) const
{
    ExtrusionEntityCollection coll;
    this->chained_path_from(start_near, &coll, no_reverse, role);
    return coll;
}

void ExtrusionEntityCollection::chained_path_from(Point start_near, ExtrusionEntityCollection* retval, bool no_reverse, ExtrusionRole role, std::vector<size_t>* orig_indices) const
{
    if (this->no_sort) {
        *retval = *this;
        return;
    }
    retval->entities.reserve(this->entities.size());
    retval->orig_indices.reserve(this->entities.size());
    
    // if we're asked to return the original indices, build a map
    std::map<ExtrusionEntity*,size_t> indices_map;
    
    ExtrusionEntitiesPtr my_paths;
    for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it) {
        if (role != erMixed) {
            // The caller wants only paths with a specific extrusion role.
            ExtrusionRole role2 = (*it)->role();
            if (role != role2) {
                // This extrusion entity does not match the role asked.
                assert(role2 != erMixed);
                continue;
            }
        }

        ExtrusionEntity* entity = (*it)->clone();
        my_paths.push_back(entity);
        if (orig_indices != NULL) indices_map[entity] = it - this->entities.begin();
    }
    
    Points endpoints;
    for (ExtrusionEntitiesPtr::iterator it = my_paths.begin(); it != my_paths.end(); ++it) {
        endpoints.push_back((*it)->first_point());
        if (no_reverse || !(*it)->can_reverse()) {
            endpoints.push_back((*it)->first_point());
        } else {
            endpoints.push_back((*it)->last_point());
        }
    }
    
    while (!my_paths.empty()) {
        // find nearest point
        int start_index = start_near.nearest_point_index(endpoints);
        int path_index = start_index/2;
        ExtrusionEntity* entity = my_paths.at(path_index);
        // never reverse loops, since it's pointless for chained path and callers might depend on orientation
        if (start_index % 2 && !no_reverse && entity->can_reverse()) {
            entity->reverse();
        }
        retval->entities.push_back(my_paths.at(path_index));
        if (orig_indices != NULL) orig_indices->push_back(indices_map[entity]);
        my_paths.erase(my_paths.begin() + path_index);
        endpoints.erase(endpoints.begin() + 2*path_index, endpoints.begin() + 2*path_index + 2);
        start_near = retval->entities.back()->last_point();
    }
}

void ExtrusionEntityCollection::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it)
        (*it)->polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionEntityCollection::polygons_covered_by_spacing(Polygons &out, const float scaled_epsilon) const
{
    for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it)
        (*it)->polygons_covered_by_spacing(out, scaled_epsilon);
}

/* Recursively count paths and loops contained in this collection */
size_t
ExtrusionEntityCollection::items_count() const
{
    return CountEntities().count(*this);
}

void
CountEntities::use(const ExtrusionEntityCollection &coll) {
    for (const ExtrusionEntity* entity : coll.entities) {
        entity->visit(*this);
    }
}
//
//void
//ExtrusionEntityCollection::flatten(ExtrusionEntityCollection* retval, bool preserve_ordering) const
//{
//    if (this->no_sort && preserve_ordering) {
//        /// if we want to preserve ordering and we can't sort, break out the unsorted ones first.
//        ExtrusionEntityCollection *unsortable = new ExtrusionEntityCollection();
//        unsortable->no_sort = true;
//        unsortable->orig_indices = this->orig_indices;
//        retval->append(*unsortable);
//        for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it) {
//            if ((*it)->is_collection()) {
//                ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(*it);
//                collection->flatten(unsortable, preserve_ordering);
//            } else {
//                unsortable->append(**it);
//            }
//        }
//    } else {
//        for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it) {
//            if ((*it)->is_collection()) {
//                ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(*it);
//                retval->append(collection->flatten().entities);
//            } else {
//                retval->append(**it);
//            }
//        }
//    }
//}

/* Returns a single vector of chained (new) pointers to all non-collection items contained in this one */
ExtrusionEntityCollection
ExtrusionEntityCollection::flatten(bool preserve_ordering) const
{
    //ExtrusionEntityCollection coll;
    //this->flatten(&coll, preserve_ordering);
    //return coll;
    return FlatenEntities(preserve_ordering).flatten(*this);

}
void
FlatenEntities::use(const ExtrusionEntityCollection &coll) {
    if (coll.no_sort && preserve_ordering) {
        FlatenEntities unsortable(coll, preserve_ordering);
        for (const ExtrusionEntity* entity : coll.entities) {
            entity->visit(unsortable);
        }
        to_fill.append(std::move(unsortable.to_fill));
    } else {
        for (const ExtrusionEntity* entity : coll.entities) {
            entity->visit(*this);
        }
    }
}
ExtrusionEntityCollection&&
FlatenEntities::flatten(const ExtrusionEntityCollection &to_flatten) && {
    use(to_flatten);
    return std::move(to_fill);
}


double
ExtrusionEntityCollection::min_mm3_per_mm() const
{
    double min_mm3_per_mm = std::numeric_limits<double>::max();
    for (ExtrusionEntitiesPtr::const_iterator it = this->entities.begin(); it != this->entities.end(); ++it)
        min_mm3_per_mm = std::min(min_mm3_per_mm, (*it)->min_mm3_per_mm());
    return min_mm3_per_mm;
}

}
