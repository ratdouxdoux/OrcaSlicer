#include "AIModelFeatures.hpp"
#include "AABBMesh.hpp"
#include "AIGeneratedModel.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "Flow.hpp"
#include "Measure.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "Slicing.hpp"
#include "TriangleMeshSlicer.hpp"
#include "GCode/SeamPlacer.hpp"
#include "Geometry/ConvexHull.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tbb/parallel_for.h>

namespace Slic3r::AIPrintAssistant {
namespace {
using json = nlohmann::json;

// Faces within a degree of level print as flat surfaces; tessellated flat faces
// are rarely exactly level.
constexpr double FLAT_TOLERANCE = PI / 180.;
// Edges at least this sharp are corners the seam placer snaps to.
constexpr double SHARP_ANGLE = SeamPlacer::sharp_angle_snapping_threshold;
// PrusaSlicer's hard-coded small-perimeter radius. Orca slows no small perimeters
// while small_perimeter_threshold is 0, its default.
constexpr double DEFAULT_SMALL_PERIMETER_R = 6.5;
// Offset that keeps points off the surface they were sampled from.
constexpr double SURFACE_EPSILON = 0.005;

double round_to(double value, double step) { return std::round(value / step) * step; }
json   vec(const Vec3d& v) { return json::array({round_to(v.x(), 0.01), round_to(v.y(), 0.01), round_to(v.z(), 0.01)}); }
double mm2(double scaled_area) { return scaled_area * SCALING_FACTOR * SCALING_FACTOR; }
double degrees(double radians) { return round_to(radians * 180. / PI, 0.1); }

struct Body
{
    std::string          id;
    indexed_triangle_set its; // world mm
    int                  filament = 1;
    const ModelVolume*   volume   = nullptr;
};

class SolidTest
{
public:
    SolidTest(const std::vector<Body>& parts, const std::vector<Body>& negatives)
    {
        for (const auto& body : parts)
            m_parts.emplace_back(body.its);
        for (const auto& body : negatives)
            m_negatives.emplace_back(body.its);
    }
    bool inside_part(const Vec3d& p, int skip = -1) const { return inside(m_parts, p, skip); }
    bool inside_negative(const Vec3d& p, int skip = -1) const { return inside(m_negatives, p, skip); }
    bool solid(const Vec3d& p) const { return inside_part(p) && !inside_negative(p); }

private:
    static bool inside(const std::vector<AABBMesh>& meshes, const Vec3d& p, int skip)
    {
        // An oblique direction avoids grazing axis-aligned edges.
        static const Vec3d direction = Vec3d(0.31, 0.53, 0.79).normalized();
        for (size_t i = 0; i < meshes.size(); ++i)
            if (int(i) != skip && meshes[i].query_ray_hit(p, direction).is_inside())
                return true;
        return false;
    }
    std::vector<AABBMesh> m_parts, m_negatives;
};

// The printed surface: faces of model parts that are not buried in another part
// or cut away, and faces of negative volumes that lie inside a part, flipped.
struct Surface
{
    indexed_triangle_set its;
    std::vector<Vec3f>   normals;
    std::vector<Vec3i32> neighbors;
    std::vector<char>    visible;
    std::vector<double>  areas;

    Vec3d vertex(int face, int corner) const { return its.vertices[its.indices[face][corner]].cast<double>(); }
    Vec3d centroid(int face) const { return (vertex(face, 0) + vertex(face, 1) + vertex(face, 2)) / 3.; }
};

// Splits every edge longer than max_edge at its midpoint, the same way in both
// triangles that share it, so the mesh stays closed. A coarse face partly
// covered by another part is then classified piece by piece.
void subdivide(indexed_triangle_set& its, double max_edge)
{
    const float limit = float(max_edge * max_edge);
    for (int pass = 0; pass < 32; ++pass) {
        std::map<std::pair<int, int>, int> midpoints;
        auto key = [](int a, int b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
        for (const auto& t : its.indices)
            for (int e = 0; e < 3; ++e) {
                const int a = t[e], b = t[(e + 1) % 3];
                if ((its.vertices[a] - its.vertices[b]).squaredNorm() > limit && !midpoints.count(key(a, b))) {
                    midpoints[key(a, b)] = int(its.vertices.size());
                    its.vertices.push_back((its.vertices[a] + its.vertices[b]) / 2.f);
                }
            }
        if (midpoints.empty())
            return;
        std::vector<stl_triangle_vertex_indices> next;
        for (const auto& t : its.indices) {
            int m[3], split = 0;
            for (int e = 0; e < 3; ++e) {
                const auto it = midpoints.find(key(t[e], t[(e + 1) % 3]));
                m[e]          = it == midpoints.end() ? -1 : it->second;
                split += m[e] >= 0;
            }
            if (split == 0)
                next.push_back(t);
            else if (split == 3) {
                next.emplace_back(t[0], m[0], m[2]);
                next.emplace_back(m[0], t[1], m[1]);
                next.emplace_back(m[1], t[2], m[2]);
                next.emplace_back(m[0], m[1], m[2]);
            } else if (split == 1) {
                const int e = m[0] >= 0 ? 0 : m[1] >= 0 ? 1 : 2;
                next.emplace_back(t[e], m[e], t[(e + 2) % 3]);
                next.emplace_back(m[e], t[(e + 1) % 3], t[(e + 2) % 3]);
            } else {
                const int u = m[0] < 0 ? 0 : m[1] < 0 ? 1 : 2; // the edge left whole
                const int a = m[(u + 1) % 3], b = m[(u + 2) % 3];
                next.emplace_back(a, t[(u + 2) % 3], b);
                next.emplace_back(t[u], t[(u + 1) % 3], a);
                next.emplace_back(t[u], a, b);
            }
        }
        its.indices = std::move(next);
    }
}

std::vector<Surface> printed_surfaces(const std::vector<Body>& parts, const std::vector<Body>& negatives, const SolidTest& test, double line_width)
{
    std::vector<Surface> result;
    const bool           alone = parts.size() == 1 && negatives.empty();
    // Fine enough to place the edge of a covered area within two lines, and
    // coarse enough to keep large models to a few hundred thousand pieces.
    double area = 0.;
    for (const auto* bodies : {&parts, &negatives})
        for (const auto& body : *bodies)
            for (const auto& t : body.its.indices) {
                const Vec3f a = body.its.vertices[t[0]];
                area += 0.5 * double((body.its.vertices[t[1]] - a).cross(body.its.vertices[t[2]] - a).norm());
            }
    const double max_edge = std::max(2. * line_width, std::sqrt(area / 200000.));
    auto                 add = [&](indexed_triangle_set its, bool negative, int index) {
        Surface s;
        if (negative)
            its_flip_triangles(its);
        if (!alone)
            subdivide(its, max_edge);
        s.its       = std::move(its);
        s.normals   = its_face_normals(s.its);
        s.neighbors = its_face_neighbors(s.its);
        s.visible.assign(s.its.indices.size(), 1);
        s.areas.resize(s.its.indices.size());
        for (int f = 0; f < int(s.its.indices.size()); ++f) {
            s.areas[f]    = 0.5 * (s.vertex(f, 1) - s.vertex(f, 0)).cross(s.vertex(f, 2) - s.vertex(f, 0)).norm();
            if (alone)
                continue;
            const Vec3d c = s.centroid(f), n = s.normals[f].cast<double>();
            if (negative)
                s.visible[f] = test.inside_part(c - n * SURFACE_EPSILON) && !test.inside_negative(c - n * SURFACE_EPSILON, index);
            else
                s.visible[f] = !test.inside_part(c + n * SURFACE_EPSILON, index) && !test.inside_negative(c - n * SURFACE_EPSILON);
        }
        result.push_back(std::move(s));
    };
    for (size_t i = 0; i < parts.size(); ++i)
        add(parts[i].its, false, int(i));
    for (size_t i = 0; i < negatives.size(); ++i)
        add(negatives[i].its, true, int(i));
    return result;
}

// Connected regions of member faces; join decides whether two neighbours connect.
template<class Join> std::vector<std::vector<int>> regions(const Surface& s, const std::vector<char>& member, Join join)
{
    std::vector<std::vector<int>> result;
    std::vector<char>             seen(member.size(), 0);
    for (int start = 0; start < int(member.size()); ++start) {
        if (!member[start] || seen[start])
            continue;
        std::vector<int> region{start}, stack{start};
        seen[start] = 1;
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            for (int e = 0; e < 3; ++e) {
                const int g = s.neighbors[f][e];
                if (g < 0 || seen[g] || !member[g] || !join(f, g))
                    continue;
                seen[g] = 1;
                region.push_back(g);
                stack.push_back(g);
            }
        }
        result.push_back(std::move(region));
    }
    return result;
}

double normal_angle(const Surface& s, int f, int g)
{
    return std::acos(std::clamp(double(s.normals[f].dot(s.normals[g])), -1., 1.));
}

// Half a line, coarser for large features so a grid stays about 1000 cells across.
double footprint_cell(const BoundingBoxf3& box, double line_width)
{
    return std::max(line_width / 2., std::max(box.size().x(), box.size().y()) / 1000.);
}

// The XY outline of faces or edges, marked on a grid. Steep faces project to
// many overlapping slivers, which make polygon unions very slow; a grid is
// robust and fine enough for a modifier. Vertical faces, whose projection has
// no area, still mark the cells along their edges.
class Footprint
{
public:
    Footprint(const BoundingBoxf3& box, double cell)
        : m_cell(cell)
        , m_origin(box.min.head<2>() - Vec2d::Constant(m_cell))
        , m_width(int(box.size().x() / m_cell) + 3)
        , m_height(int(box.size().y() / m_cell) + 3)
        , m_cells(size_t(m_width) * size_t(m_height), 0)
    {}
    void segment(const Vec3d& a, const Vec3d& b)
    {
        const int steps = int((b - a).head<2>().norm() / (m_cell / 2.)) + 1;
        for (int i = 0; i <= steps; ++i)
            mark(a.head<2>() + (b - a).head<2>() * (double(i) / steps));
    }
    void triangle(const Vec3d& a, const Vec3d& b, const Vec3d& c)
    {
        segment(a, b);
        segment(b, c);
        segment(c, a);
        // Cells whose centres lie inside the projected triangle.
        const Vec2d p0 = a.head<2>(), e1 = b.head<2>() - p0, e2 = c.head<2>() - p0;
        const double det = e1.x() * e2.y() - e1.y() * e2.x();
        if (std::abs(det) < 1e-12)
            return;
        const Vec2d lo = p0.cwiseMin(b.head<2>()).cwiseMin(c.head<2>()), hi = p0.cwiseMax(b.head<2>()).cwiseMax(c.head<2>());
        for (int y = cell(lo).y(); y <= cell(hi).y(); ++y)
            for (int x = cell(lo).x(); x <= cell(hi).x(); ++x) {
                const Vec2d  q = m_origin + Vec2d(x + 0.5, y + 0.5) * m_cell - p0;
                const double u = (q.x() * e2.y() - q.y() * e2.x()) / det, v = (e1.x() * q.y() - e1.y() * q.x()) / det;
                if (u >= 0. && v >= 0. && u + v <= 1.)
                    m_cells[size_t(y) * m_width + x] = 1;
            }
    }
    // A convex polygon, such as a triangle clipped to a band; it may collapse
    // to a segment or a point.
    void polygon(const std::vector<Vec3d>& corners)
    {
        if (corners.size() == 1)
            mark(corners.front().head<2>());
        else if (corners.size() == 2)
            segment(corners[0], corners[1]);
        for (size_t i = 1; i + 1 < corners.size(); ++i)
            triangle(corners[0], corners[i], corners[i + 1]);
    }
    // Marks the cells of a footprint whose grid lines up with this one.
    void add(const Footprint& other)
    {
        const int dx = int(std::lround((other.m_origin.x() - m_origin.x()) / m_cell));
        const int dy = int(std::lround((other.m_origin.y() - m_origin.y()) / m_cell));
        for (int y = 0; y < other.m_height; ++y)
            for (int x = 0; x < other.m_width; ++x)
                if (other.m_cells[size_t(y) * other.m_width + x] && x + dx >= 0 && y + dy >= 0 && x + dx < m_width && y + dy < m_height)
                    m_cells[size_t(y + dy) * m_width + x + dx] = 1;
    }
    // Each row's runs of cells become rectangles, overlapping their neighbours
    // slightly so the union never meets exactly coincident edges.
    ExPolygons polygons() const
    {
        Polygons   runs;
        const auto overlap = coord_t(10);
        for (int y = 0; y < m_height; ++y)
            for (int x = 0; x < m_width;) {
                if (!m_cells[size_t(y) * m_width + x]) {
                    ++x;
                    continue;
                }
                int end = x;
                while (end < m_width && m_cells[size_t(y) * m_width + end])
                    ++end;
                const Point lo = Point::new_scale(m_origin.x() + x * m_cell, m_origin.y() + y * m_cell) - Point(overlap, overlap);
                const Point hi = Point::new_scale(m_origin.x() + end * m_cell, m_origin.y() + (y + 1) * m_cell) + Point(overlap, overlap);
                runs.push_back(Polygon({lo, Point(hi.x(), lo.y()), hi, Point(lo.x(), hi.y())}));
                x = end;
            }
        return union_ex(runs);
    }

private:
    Vec2i32 cell(const Vec2d& p) const
    {
        const Vec2d c = (p - m_origin) / m_cell;
        return Vec2i32(std::clamp(int(c.x()), 0, m_width - 1), std::clamp(int(c.y()), 0, m_height - 1));
    }
    void mark(const Vec2d& p)
    {
        const auto c = cell(p);
        m_cells[size_t(c.y()) * m_width + c.x()] = 1;
    }
    double            m_cell;
    Vec2d             m_origin;
    int               m_width, m_height;
    std::vector<char> m_cells;
};

// Features are cut into at most this many bands: enough for a modifier to follow
// a slope within a millimetre or so, few enough to keep reports and meshes light.
constexpr int MAX_BANDS = 64;

// The part of a convex polygon on one side of a level plane.
std::vector<Vec3d> clip_z(const std::vector<Vec3d>& polygon, double z, bool above)
{
    std::vector<Vec3d> result;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vec3d& a    = polygon[i];
        const Vec3d& b    = polygon[(i + 1) % polygon.size()];
        const bool   in_a = above ? a.z() >= z : a.z() <= z;
        const bool   in_b = above ? b.z() >= z : b.z() <= z;
        if (in_a)
            result.push_back(a);
        if (in_a != in_b)
            result.push_back(a + (b - a) * ((z - a.z()) / (b.z() - a.z())));
    }
    return result;
}

// World-mm triangles; an edge is a triangle whose last two corners coincide.
using Triangles = std::vector<std::array<Vec3d, 3>>;

// The outline of triangles band by band, about a layer per band, and the whole
// footprint they add up to. Each triangle is clipped to the bands it crosses, so
// a slope becomes a narrow strip in each band instead of one block.
void set_outline(ModelFeature& feature, const Triangles& triangles, const FeatureParams& p)
{
    const auto&  box  = feature.box;
    const double cell = footprint_cell(box, p.line_width);
    Footprint    whole(box, cell);
    const double height = box.size().z();
    const int    count  = std::clamp(int(std::ceil(height / p.layer_height)), 1, MAX_BANDS);
    const double band   = height / count;
    auto         index  = [&](double z) { return band > 0. ? std::clamp(int((z - box.min.z()) / band), 0, count - 1) : 0; };
    std::vector<std::vector<size_t>> members(count);
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto range = std::minmax({triangles[i][0].z(), triangles[i][1].z(), triangles[i][2].z()});
        for (int k = index(range.first); k <= index(range.second); ++k)
            members[k].push_back(i);
    }
    for (int k = 0; k < count; ++k) {
        const double                    z0 = box.min.z() + k * band, z1 = k + 1 == count ? box.max.z() : box.min.z() + (k + 1) * band;
        std::vector<std::vector<Vec3d>> pieces;
        BoundingBoxf3                   extent;
        for (size_t i : members[k]) {
            const auto& t     = triangles[i];
            auto        piece = clip_z(clip_z({t[0], t[1], t[2]}, z0, true), z1, false);
            for (const auto& corner : piece)
                extent.merge(corner);
            if (!piece.empty())
                pieces.push_back(std::move(piece));
        }
        if (pieces.empty())
            continue;
        // Cells line up with the whole footprint's, so a straight wall gives the
        // same outline in every band.
        BoundingBoxf3 grid = extent;
        grid.min.head<2>() = box.min.head<2>() + ((extent.min.head<2>() - box.min.head<2>()) / cell).array().floor().matrix() * cell;
        Footprint footprint(grid, cell);
        for (const auto& piece : pieces)
            footprint.polygon(piece);
        whole.add(footprint);
        feature.slabs.push_back({extent.min.z(), extent.max.z(), footprint.polygons()});
    }
    feature.footprint = whole.polygons();
}

// A feature whose outline is drawn later, in parallel with the others: from the
// faces of a surface, or from triangles when there is no surface.
struct Pending
{
    ModelFeature     feature;
    const Surface*   surface = nullptr;
    std::vector<int> faces;
    Triangles        triangles;
};

Pending face_feature(const std::string& kind, const Surface& s, const std::vector<int>& faces)
{
    Pending item{{}, &s, faces, {}};
    double  area = 0.;
    for (int f : faces) {
        for (int corner = 0; corner < 3; ++corner)
            item.feature.box.merge(s.vertex(f, corner));
        area += s.areas[f];
    }
    item.feature.kind                 = kind;
    item.feature.measures["area_mm2"] = round_to(area, 0.01);
    item.feature.significance         = area;
    return item;
}

// Outlines are the costly part of the face pass and independent of each other.
void outline_all(std::vector<Pending>& pending, const FeatureParams& p, std::vector<ModelFeature>& out)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, pending.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            auto& item = pending[i];
            if (item.surface)
                for (int f : item.faces)
                    item.triangles.push_back({item.surface->vertex(f, 0), item.surface->vertex(f, 1), item.surface->vertex(f, 2)});
            set_outline(item.feature, item.triangles, p);
            item.triangles = Triangles();
        }
    });
    for (auto& item : pending)
        out.push_back(std::move(item.feature));
}

// Largest angle between two face normals of a region, approximated from the
// largest face's normal and the normal farthest from it.
double normal_spread(const Surface& s, const std::vector<int>& faces)
{
    const int seed = *std::max_element(faces.begin(), faces.end(), [&](int a, int b) { return s.areas[a] < s.areas[b]; });
    int       farthest = seed;
    for (int f : faces)
        if (normal_angle(s, seed, f) > normal_angle(s, seed, farthest))
            farthest = f;
    double spread = 0.;
    for (int f : faces)
        spread = std::max(spread, normal_angle(s, farthest, f));
    return spread;
}

void detect_faces(const std::vector<Surface>& surfaces, const FeatureParams& p, double bed_z, std::vector<ModelFeature>& out)
{
    // The slicer grades overhanging walls by how much of the line hangs over
    // air, from fully supported to beyond half a nozzle outside the layer below
    // (PerimeterGenerator::generate_lower_polygons_series).
    const double grading_width  = (p.line_width + p.nozzle_diameter) / 2.;
    const double first_slice_z  = bed_z + p.first_layer_height / 2.;
    const double shallow_limit  = std::atan(p.layer_height / p.line_width);
    const double min_area       = p.line_width * p.line_width;
    auto         unsupported    = [&](double angle_from_vertical) {
        return angle_from_vertical >= PI / 2. - 1e-9 ? 1. : std::min(1., p.layer_height * std::tan(angle_from_vertical) / grading_width);
    };
    std::vector<Pending> pending;
    for (const auto& s : surfaces) {
        const size_t      n = s.its.indices.size();
        std::vector<char> overhang(n, 0), shallow(n, 0), flat(n, 0), wall(n, 0), top(n, 0);
        for (size_t f = 0; f < n; ++f) {
            if (!s.visible[f] || s.areas[f] <= 0.)
                continue;
            const double nz = s.normals[f].z();
            if (nz < 0.) {
                const double highest = std::max({s.vertex(int(f), 0).z(), s.vertex(int(f), 1).z(), s.vertex(int(f), 2).z()});
                overhang[f]          = highest > first_slice_z && unsupported(std::asin(std::min(1., -nz))) >= 0.5;
            } else {
                const double slope = std::acos(std::min(1., nz));
                flat[f]            = slope < FLAT_TOLERANCE;
                shallow[f]         = !flat[f] && slope < shallow_limit;
            }
            wall[f] = std::abs(nz) < 0.5;
            top[f]  = nz >= 0.5;
        }
        const auto any = [](int, int) { return true; };
        for (const auto& faces : regions(s, overhang, any)) {
            auto   item    = face_feature("overhang", s, faces);
            auto&  feature = item.feature;
            double lowest = PI, steepest = 0.;
            for (int f : faces) {
                const double angle = std::asin(std::min(1., double(-s.normals[f].z())));
                lowest             = std::min(lowest, angle);
                steepest           = std::max(steepest, angle);
            }
            if (feature.significance < min_area)
                continue;
            const double fraction                    = unsupported(steepest);
            feature.measures["angle_from_vertical_deg"] = {degrees(lowest), degrees(steepest)};
            feature.measures["unsupported_percent"]     = round_to(100. * fraction, 1.);
            feature.measures["overhang_speed_setting"]  = fraction >= 0.75 ? "overhang_4_4_speed" : "overhang_3_4_speed";
            feature.measures["needs_support"]           = 90. - steepest * 180. / PI < p.support_angle;
            pending.push_back(std::move(item));
        }
        for (const auto& faces : regions(s, shallow, any)) {
            auto   item    = face_feature("shallow_slope", s, faces);
            auto&  feature = item.feature;
            double lowest  = PI / 2., highest = 0.;
            for (int f : faces) {
                const double slope = std::acos(std::min(1., double(s.normals[f].z())));
                lowest             = std::min(lowest, slope);
                highest            = std::max(highest, slope);
            }
            if (feature.significance < min_area)
                continue;
            feature.measures["slope_deg"]           = {degrees(lowest), degrees(highest)};
            feature.measures["widest_step_mm"]      = round_to(p.layer_height / std::tan(lowest), 0.01);
            pending.push_back(std::move(item));
        }
        for (const auto& faces : regions(s, flat, any)) {
            auto  item    = face_feature("flat_top", s, faces);
            auto& feature = item.feature;
            if (feature.significance < min_area)
                continue;
            feature.measures["z_mm"] = round_to(feature.box.max.z(), 0.01);
            pending.push_back(std::move(item));
        }
        for (const auto* orientation : {"wall", "top"}) {
            const auto& member = std::string(orientation) == "wall" ? wall : top;
            const auto grown = regions(s, member, [&](int f, int g) { return normal_angle(s, f, g) < SHARP_ANGLE; });
            for (const auto& faces : grown) {
                const double spread = normal_spread(s, faces);
                if (spread < SHARP_ANGLE)
                    continue;
                auto  item    = face_feature("curved_surface", s, faces);
                auto& feature = item.feature;
                if (feature.significance < min_area)
                    continue;
                feature.measures["orientation"]      = orientation;
                feature.measures["normal_spread_deg"] = degrees(spread);
                pending.push_back(std::move(item));
            }
        }
        // Sharp, mostly vertical edges: where seams hide (concave) and where
        // ringing starts (convex). Chains join edges that share a vertex.
        struct Edge
        {
            int    a, b;
            bool   convex;
            double angle;
        };
        std::vector<Edge> edges;
        for (int f = 0; f < int(n); ++f)
            for (int e = 0; e < 3; ++e) {
                const int g = s.neighbors[f][e];
                if (g <= f || !s.visible[f] || !s.visible[g])
                    continue;
                const double angle = normal_angle(s, f, g);
                if (angle < SHARP_ANGLE)
                    continue;
                const int   a = int(s.its.indices[f][e]), b = int(s.its.indices[f][(e + 1) % 3]);
                const Vec3d d = s.its.vertices[b].cast<double>() - s.its.vertices[a].cast<double>();
                if (std::abs(d.z()) < d.head<2>().norm())
                    continue;
                int opposite = 0;
                for (int c = 0; c < 3; ++c)
                    if (int(s.its.indices[g][c]) != a && int(s.its.indices[g][c]) != b)
                        opposite = int(s.its.indices[g][c]);
                const bool convex = (s.its.vertices[opposite].cast<double>() - s.its.vertices[a].cast<double>())
                                        .dot(s.normals[f].cast<double>()) < 0.;
                edges.push_back({a, b, convex, angle});
            }
        std::vector<int> parent(edges.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> root = [&](int i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
        std::map<std::pair<int, bool>, int> by_vertex;
        for (int i = 0; i < int(edges.size()); ++i)
            for (int v : {edges[i].a, edges[i].b}) {
                auto [it, inserted] = by_vertex.emplace(std::make_pair(v, edges[i].convex), i);
                if (!inserted)
                    parent[root(i)] = root(it->second);
            }
        std::map<int, std::vector<int>> chains;
        for (int i = 0; i < int(edges.size()); ++i)
            chains[root(i)].push_back(i);
        for (const auto& [chain_root, members] : chains) {
            Pending item;
            auto&   feature = item.feature;
            double  length = 0., angle = 0.;
            auto         end    = [&](int i, bool second) { return s.its.vertices[second ? edges[i].b : edges[i].a].cast<double>(); };
            for (int i : members) {
                feature.box.merge(end(i, false));
                feature.box.merge(end(i, true));
                length += (end(i, true) - end(i, false)).norm();
                angle = std::max(angle, edges[i].angle);
            }
            if (length < p.layer_height)
                continue;
            for (int i : members)
                item.triangles.push_back({end(i, false), end(i, true), end(i, true)});
            feature.kind              = "sharp_edge";
            feature.measures          = {{"length_mm", round_to(length, 0.01)},
                                         {"angle_between_faces_deg", degrees(angle)},
                                         {"convex", edges[members.front()].convex}};
            feature.significance      = length;
            pending.push_back(std::move(item));
        }
    }
    outline_all(pending, p, out);
}

// ---------------------------------------------------------------- layers ----

struct SliceLayer
{
    double                  z_lo = 0., z_hi = 0.; // world mm
    ExPolygons              solid;
    std::vector<ExPolygons> parts; // per model part, kept only for material boundaries
};

std::vector<SliceLayer> slice_layers(const ModelObject&        object,
                                     size_t                    instance,
                                     const DynamicPrintConfig& config,
                                     bool                      keep_parts,
                                     const std::function<void()>& cancel)
{
    const auto        box    = object.instance_bounding_box(instance);
    SlicingParameters params = PrintObject::slicing_parameters(config, object, float(box.size().z()), Vec3d::Ones());
    std::vector<coordf_t> profile;
    PrintObject::update_layer_height_profile(object, params, profile);
    const bool precise = config.has("precise_z_height") && config.opt_bool("precise_z_height");
    const auto bounds  = generate_object_layers(params, profile, precise);
    std::vector<SliceLayer> layers(bounds.size() / 2);
    std::vector<float>      zs(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        layers[i].z_lo = box.min.z() + bounds[2 * i];
        layers[i].z_hi = box.min.z() + bounds[2 * i + 1];
        zs[i]          = float((layers[i].z_lo + layers[i].z_hi) / 2.);
    }
    MeshSlicingParamsEx slicing;
    if (config.has("slice_closing_radius"))
        slicing.closing_radius = float(config.opt_float("slice_closing_radius"));
    const auto& inst = *object.instances[instance];
    std::vector<std::vector<ExPolygons>> part_slices, negative_slices;
    for (const auto* volume : object.volumes) {
        if (!volume->is_model_part() && volume->type() != ModelVolumeType::NEGATIVE_VOLUME)
            continue;
        indexed_triangle_set its = volume->mesh().its;
        slicing.trafo            = inst.get_matrix() * volume->get_matrix();
        if (slicing.trafo.rotation().determinant() < 0.)
            its_flip_triangles(its);
        (volume->is_model_part() ? part_slices : negative_slices).push_back(slice_mesh_ex(its, zs, slicing, cancel ? cancel : [] {}));
    }
    for (size_t i = 0; i < layers.size(); ++i) {
        ExPolygons parts, negatives;
        for (const auto& slices : part_slices) {
            append(parts, slices[i]);
            if (keep_parts)
                layers[i].parts.push_back(slices[i]);
        }
        for (const auto& slices : negative_slices)
            append(negatives, slices[i]);
        layers[i].solid = negatives.empty() ? union_ex(parts) : diff_ex(union_ex(parts), negatives);
        if (keep_parts && !negatives.empty())
            for (auto& part : layers[i].parts)
                part = diff_ex(part, negatives);
    }
    return layers;
}

struct Island
{
    size_t    layer;
    double    z_lo, z_hi;
    ExPolygon shape;
};

// Islands of consecutive layers that overlap belong to one feature.
std::vector<std::vector<size_t>> group_islands(const std::vector<Island>& islands)
{
    std::vector<size_t> parent(islands.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    std::map<size_t, std::vector<size_t>> by_layer;
    for (size_t i = 0; i < islands.size(); ++i)
        by_layer[islands[i].layer].push_back(i);
    for (const auto& [layer, members] : by_layer) {
        const auto below = by_layer.find(layer - 1);
        if (layer == 0 || below == by_layer.end())
            continue;
        for (size_t i : members)
            for (size_t j : below->second)
                if (get_extents(islands[i].shape).overlap(get_extents(islands[j].shape)) &&
                    !intersection_ex(ExPolygons{islands[i].shape}, ExPolygons{islands[j].shape}).empty())
                    parent[root(i)] = root(j);
    }
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < islands.size(); ++i)
        groups[root(i)].push_back(i);
    std::vector<std::vector<size_t>> result;
    for (auto& [group_root, members] : groups)
        result.push_back(std::move(members));
    return result;
}

ModelFeature island_feature(const std::string& kind, const std::vector<Island>& islands, const std::vector<size_t>& group)
{
    ModelFeature feature;
    feature.kind  = kind;
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest(), largest = 0.;
    std::set<size_t> layers;
    for (size_t i : group) {
        feature.footprint.push_back(islands[i].shape);
        lo      = std::min(lo, islands[i].z_lo);
        hi      = std::max(hi, islands[i].z_hi);
        largest = std::max(largest, mm2(islands[i].shape.area()));
        layers.insert(islands[i].layer);
    }
    feature.footprint = union_ex(feature.footprint);
    // Consecutive layers share a slab, at most MAX_BANDS of them.
    const size_t                  first = *layers.begin(), per_slab = (*layers.rbegin() - first) / MAX_BANDS + 1;
    std::map<size_t, FeatureSlab> slabs;
    for (size_t i : group) {
        const auto [it, created] = slabs.try_emplace((islands[i].layer - first) / per_slab);
        auto& slab               = it->second;
        slab.z_lo                = created ? islands[i].z_lo : std::min(slab.z_lo, islands[i].z_lo);
        slab.z_hi                = created ? islands[i].z_hi : std::max(slab.z_hi, islands[i].z_hi);
        slab.shape.push_back(islands[i].shape);
    }
    for (auto& [index, slab] : slabs) {
        slab.shape = union_ex(slab.shape);
        feature.slabs.push_back(std::move(slab));
    }
    const auto extents = get_extents(feature.footprint);
    feature.box        = BoundingBoxf3(Vec3d(unscaled(extents.min.x()), unscaled(extents.min.y()), lo),
                                       Vec3d(unscaled(extents.max.x()), unscaled(extents.max.y()), hi));
    feature.measures   = {{"layers", layers.size()}, {"largest_area_mm2", round_to(largest, 0.01)}};
    feature.significance = largest * double(layers.size());
    return feature;
}

void add_islands(const std::string& kind, const std::vector<Island>& islands, std::vector<ModelFeature>& out,
                 const std::function<void(ModelFeature&, const std::vector<size_t>&)>& measure = {})
{
    for (const auto& group : group_islands(islands)) {
        auto feature = island_feature(kind, islands, group);
        if (measure)
            measure(feature, group);
        out.push_back(std::move(feature));
    }
}

// Minimum width of a convex polygon (rotating calipers, brute force).
double min_width(const Polygon& hull)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0; i < hull.points.size(); ++i) {
        const Vec2d a = hull.points[i].cast<double>(), b = hull.points[(i + 1) % hull.points.size()].cast<double>();
        const Vec2d d = (b - a).normalized();
        double      reach = 0.;
        for (const auto& point : hull.points)
            reach = std::max(reach, std::abs(d.x() * (point.y() - a.y()) - d.y() * (point.x() - a.x())));
        best = std::min(best, reach);
    }
    return unscaled(best);
}

void detect_layers(const std::vector<SliceLayer>& layers, const FeatureParams& p, double object_height, std::vector<ModelFeature>& out)
{
    if (layers.empty())
        return;
    const float  w        = float(scale_(p.line_width));
    const double min_area = p.line_width * p.line_width;
    auto         keep     = [&](const ExPolygon& shape) { return mm2(shape.area()) >= min_area; };

    // Bed contact: the first layer, its sharp corners and how tall the part is
    // compared with its narrowest base.
    {
        const auto& first = layers.front();
        ModelFeature feature;
        feature.kind      = "bed_contact";
        feature.footprint = first.solid;
        const auto extents = get_extents(first.solid);
        feature.box = BoundingBoxf3(Vec3d(unscaled(extents.min.x()), unscaled(extents.min.y()), first.z_lo),
                                    Vec3d(unscaled(extents.max.x()), unscaled(extents.max.y()), first.z_hi));
        double area = 0.;
        json   corners = json::array();
        size_t sharp   = 0;
        for (const auto& island : first.solid) {
            area += mm2(island.area());
            Polygon contour = island.contour;
            const auto simplified = contour.simplify(scale_(p.ear_detection));
            if (!simplified.empty())
                contour = simplified.front();
            const auto& pts = contour.points;
            for (size_t i = 0; i < pts.size(); ++i) {
                const Vec2d prev = pts[(i + pts.size() - 1) % pts.size()].cast<double>(), cur = pts[i].cast<double>(),
                            next = pts[(i + 1) % pts.size()].cast<double>();
                const Vec2d u = prev - cur, v = next - cur;
                if (u.x() * v.y() - u.y() * v.x() > 0.) // concave on a CCW contour
                    continue;
                const double angle = std::acos(std::clamp(u.normalized().dot(v.normalized()), -1., 1.)) * 180. / PI;
                if (angle > p.ear_max_angle)
                    continue;
                ++sharp;
                if (corners.size() < 8)
                    corners.push_back({round_to(unscaled(cur.x()), 0.01), round_to(unscaled(cur.y()), 0.01)});
            }
        }
        Points points;
        for (const auto& island : first.solid)
            append(points, island.contour.points);
        const double width = points.size() >= 3 ? min_width(Geometry::convex_hull(points)) : 0.;
        feature.measures   = {{"area_mm2", round_to(area, 0.01)},
                              {"islands", first.solid.size()},
                              {"narrowest_base_mm", round_to(width, 0.01)},
                              {"height_mm", round_to(object_height, 0.01)},
                              {"height_to_base", width > 0. ? round_to(object_height / width, 0.01) : 0.},
                              {"sharp_corners", sharp},
                              {"sharp_corner_points_mm", corners}};
        feature.significance = area;
        out.push_back(std::move(feature));
    }

    std::vector<Island> bridges, floating, thin, fine, small, plates, gaps;
    std::map<size_t, double> bridge_span, bridge_angle;
    const double small_r      = p.small_perimeter_r > 0. ? p.small_perimeter_r : DEFAULT_SMALL_PERIMETER_R;
    const double small_length = scale_(2. * PI * small_r);
    // Layers are independent here; collect per layer in parallel, merge in order.
    struct Found
    {
        std::vector<Island> bridges, floating, thin, fine, small, gaps;
        std::vector<double> spans, angles;
    };
    std::vector<Found> found(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const auto& layer = layers[i];
            auto&       here  = found[i];
            auto        add   = [&](std::vector<Island>& list, const ExPolygons& shapes) {
                for (const auto& shape : shapes)
                    if (keep(shape))
                        list.push_back({i, layer.z_lo, layer.z_hi, shape});
            };
            if (i > 0) {
                // Areas over air. Those that reach less than a line width beyond the
                // layer below are overhanging walls, which the face pass grades.
                const auto& lower  = layers[i - 1].solid;
                const auto  beyond = diff_ex(layer.solid, offset_ex(lower, w));
                for (const auto& island : beyond.empty() ? ExPolygons{} : diff_ex(layer.solid, lower)) {
                    double reach = 0.;
                    for (const auto& part : intersection_ex(ExPolygons{island}, beyond))
                        reach += mm2(part.area());
                    if (reach < min_area)
                        continue;
                    if (intersection_ex(offset_ex(island, w / 2.f), lower).empty()) {
                        here.floating.push_back({i, layer.z_lo, layer.z_hi, island});
                        continue;
                    }
                    // The detector grows the area itself to find anchors on the layer
                    // below; a bridge needs them at both ends.
                    BridgeDetector detector(island, lower, coord_t(w));
                    if (!detector.detect_angle())
                        continue;
                    // Coverage keeps the strips that reach two separate anchors. Most
                    // of a bridge spans like that; a cantilever's lines only run along
                    // its one anchored edge.
                    double spanned = 0.;
                    for (const auto& part : intersection_ex(union_ex(detector.coverage()), ExPolygons{island}))
                        spanned += part.area();
                    if (spanned < 0.5 * island.area())
                        continue;
                    ExPolygon aligned = island;
                    aligned.rotate(-detector.angle);
                    here.spans.push_back(unscaled(get_extents(aligned).size().x()));
                    here.angles.push_back(detector.angle);
                    here.bridges.push_back({i, layer.z_lo, layer.z_hi, island});
                }
            }
            const auto too_fine = diff_ex(layer.solid, opening_ex(layer.solid, w / 2.f));
            add(here.fine, too_fine);
            add(here.thin, diff_ex(diff_ex(layer.solid, opening_ex(layer.solid, w)), offset_ex(too_fine, w / 4.f)));
            for (const auto& island : layer.solid)
                if (island.contour.length() <= small_length)
                    here.small.push_back({i, layer.z_lo, layer.z_hi, island});
            add(here.gaps, diff_ex(closing_ex(layer.solid, w / 2.f), layer.solid));
        }
    });
    for (auto& layer : found) {
        for (size_t k = 0; k < layer.bridges.size(); ++k) {
            bridge_span[bridges.size()]  = layer.spans[k];
            bridge_angle[bridges.size()] = layer.angles[k];
            bridges.push_back(std::move(layer.bridges[k]));
        }
        for (auto [from, to] : {std::make_pair(&layer.floating, &floating), std::make_pair(&layer.thin, &thin),
                                std::make_pair(&layer.fine, &fine), std::make_pair(&layer.small, &small), std::make_pair(&layer.gaps, &gaps)})
            to->insert(to->end(), std::make_move_iterator(from->begin()), std::make_move_iterator(from->end()));
    }
    // Thin plates: a bottom surface that becomes a top surface within fewer
    // layers than the top and bottom shells together.
    std::vector<ExPolygons> bottoms(layers.size()), tops(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        bottoms[i] = i == 0 ? layers[i].solid : diff_ex(layers[i].solid, layers[i - 1].solid);
        tops[i]    = i + 1 == layers.size() ? layers[i].solid : diff_ex(layers[i].solid, layers[i + 1].solid);
    }
    std::map<size_t, double> plate_thickness;
    for (size_t b = 0; b < layers.size(); ++b) {
        if (bottoms[b].empty())
            continue;
        ExPolygons claimed;
        for (size_t t = b; t < layers.size() && int(t - b) < p.shell_layers - 1; ++t) {
            if (tops[t].empty())
                continue;
            // Slivers narrower than two lines, such as a sphere's equator, print
            // as walls rather than top and bottom shells.
            for (const auto& patch : opening_ex(diff_ex(intersection_ex(bottoms[b], tops[t]), claimed), w))
                if (keep(patch)) {
                    plate_thickness[plates.size()] = layers[t].z_hi - layers[b].z_lo;
                    plates.push_back({b, layers[b].z_lo, layers[t].z_hi, patch});
                }
            append(claimed, tops[t]);
        }
    }

    add_islands("bridge", bridges, out, [&](ModelFeature& f, const std::vector<size_t>& group) {
        double span = 0., angle = 0.;
        for (size_t i : group)
            if (bridge_span[i] > span) {
                span  = bridge_span[i];
                angle = bridge_angle[i];
            }
        f.measures["span_mm"]   = round_to(span, 0.01);
        f.measures["direction_deg"] = degrees(angle);
    });
    add_islands("floating", floating, out);
    add_islands("thin_wall", thin, out, [&](ModelFeature& f, const std::vector<size_t>&) {
        f.measures["narrower_than_mm"] = round_to(2. * p.line_width, 0.01);
    });
    add_islands("fine_detail", fine, out, [&](ModelFeature& f, const std::vector<size_t>&) {
        f.measures["narrower_than_mm"] = round_to(p.line_width, 0.01);
    });
    add_islands("small_section", small, out, [&](ModelFeature& f, const std::vector<size_t>& group) {
        double outline = 0.;
        for (size_t i : group)
            outline = std::max(outline, unscaled(small[i].shape.contour.length()));
        f.measures["longest_outline_mm"] = round_to(outline, 0.01);
        f.measures["outline_limit_mm"]   = round_to(2. * PI * small_r, 0.01);
        f.significance                   = f.box.size().z();
    });
    add_islands("thin_plate", plates, out, [&](ModelFeature& f, const std::vector<size_t>& group) {
        double thinnest = std::numeric_limits<double>::max();
        for (size_t i : group)
            thinnest = std::min(thinnest, plate_thickness[i]);
        f.measures["thinnest_mm"]  = round_to(thinnest, 0.01);
        f.measures["shell_layers"] = p.shell_layers;
    });
    add_islands("narrow_gap", gaps, out, [&](ModelFeature& f, const std::vector<size_t>&) {
        f.measures["narrower_than_mm"] = round_to(p.line_width, 0.01);
    });

    // Holes in the slices: open at an end they are holes, capped at both ends
    // enclosed cavities.
    std::vector<Island> holes;
    for (size_t i = 0; i < layers.size(); ++i)
        for (const auto& island : layers[i].solid)
            for (Polygon hole : island.holes) {
                hole.reverse();
                ExPolygon shape(hole);
                if (2. * std::sqrt(mm2(shape.area()) / PI) >= p.line_width)
                    holes.push_back({i, layers[i].z_lo, layers[i].z_hi, std::move(shape)});
            }
    for (const auto& group : group_islands(holes)) {
        size_t first = holes[group.front()].layer, last = first;
        for (size_t i : group) {
            first = std::min(first, holes[i].layer);
            last  = std::max(last, holes[i].layer);
        }
        auto covered = [&](size_t layer, size_t neighbour) {
            ExPolygons shapes;
            for (size_t i : group)
                if (holes[i].layer == layer)
                    shapes.push_back(holes[i].shape);
            double total = 0.;
            for (const auto& shape : shapes)
                total += shape.area();
            double open = 0.;
            for (const auto& shape : diff_ex(shapes, layers[neighbour].solid))
                open += shape.area();
            return open < 0.05 * total;
        };
        const bool capped_below = first > 0 && covered(first, first - 1);
        const bool capped_above = last + 1 < layers.size() && covered(last, last + 1);
        auto       feature      = island_feature(capped_below && capped_above ? "cavity" : "vertical_hole", holes, group);
        double     narrowest = std::numeric_limits<double>::max(), volume = 0.;
        const ExPolygon* narrow = nullptr;
        for (size_t i : group) {
            const double area = mm2(holes[i].shape.area());
            volume += area * (holes[i].z_hi - holes[i].z_lo);
            if (area < narrowest) {
                narrowest = area;
                narrow    = &holes[i].shape;
            }
        }
        if (feature.kind == "cavity") {
            feature.measures["volume_mm3"] = round_to(volume, 0.1);
        } else {
            const double perimeter               = unscaled(narrow->contour.length());
            feature.measures["diameter_mm"]      = round_to(2. * std::sqrt(narrowest / PI), 0.01);
            feature.measures["circularity"]      = round_to(4. * PI * narrowest / (perimeter * perimeter), 0.01);
            feature.measures["depth_mm"]         = round_to(feature.box.size().z(), 0.01);
            feature.measures["open_at_bottom"]   = !capped_below;
            feature.measures["open_at_top"]      = !capped_above;
        }
        out.push_back(std::move(feature));
    }
}

// Where parts with different filaments meet: the band of one part within a line
// width of the other.
void detect_material_boundaries(const std::vector<SliceLayer>& layers, const std::vector<Body>& parts, const FeatureParams& p,
                                std::vector<ModelFeature>& out)
{
    const float w = float(scale_(p.line_width));
    for (size_t a = 0; a < parts.size(); ++a)
        for (size_t b = a + 1; b < parts.size(); ++b) {
            if (parts[a].filament == parts[b].filament)
                continue;
            std::vector<Island> bands;
            for (size_t i = 0; i < layers.size(); ++i)
                for (const auto& band : intersection_ex(offset_ex(layers[i].parts[a], w), layers[i].parts[b]))
                    if (mm2(band.area()) >= p.line_width * p.line_width)
                        bands.push_back({i, layers[i].z_lo, layers[i].z_hi, band});
            add_islands("material_boundary", bands, out, [&](ModelFeature& f, const std::vector<size_t>&) {
                f.measures["filaments"] = {parts[a].filament, parts[b].filament};
                f.measures["part_ids"]  = {parts[a].id, parts[b].id};
            });
        }
}

// Horizontal holes: circles on vertical faces with air inside and solid around.
void detect_horizontal_holes(const std::vector<Body>& bodies, const SolidTest& test, const FeatureParams& p, double extent,
                             std::vector<ModelFeature>& out)
{
    struct Hole
    {
        Vec3d  center, axis;
        double radius, depth;
        bool   through;
    };
    std::vector<Hole> found;
    const double      step = p.line_width;
    for (const auto& body : bodies) {
        const bool        negative = body.volume && body.volume->type() == ModelVolumeType::NEGATIVE_VOLUME;
        Measure::Measuring measuring(body.its);
        for (int plane = 0; plane < measuring.get_num_of_planes(); ++plane) {
            for (const auto& feature : measuring.get_plane_features(unsigned(plane))) {
                if (feature.get_type() != Measure::SurfaceFeatureType::Circle)
                    continue;
                // Copies: C++17 lambdas cannot capture structured bindings.
                const auto   circle = feature.get_circle();
                const Vec3d  center = std::get<0>(circle), normal = std::get<2>(circle);
                const double radius = std::get<1>(circle);
                if (std::abs(normal.z()) > std::sin(FLAT_TOLERANCE) || 2. * radius < p.line_width)
                    continue;
                // Model part faces point out of the solid and the hole runs into it;
                // a negative volume's cap points out of the hole it cuts.
                const Vec3d axis  = -normal.normalized();
                const Vec3d side  = axis.cross(Vec3d::UnitZ()).normalized();
                auto        core  = [&](double d) { return test.solid(center + axis * d); };
                auto        ring  = [&](double d) {
                    int solid = 0;
                    for (const Vec3d& u : {side, Vec3d(-side), Vec3d(Vec3d::UnitZ()), Vec3d(-Vec3d::UnitZ())})
                        solid += test.solid(center + axis * d + u * (radius + p.line_width));
                    return solid >= 3;
                };
                // A negative cylinder's cap usually sits outside the part; start
                // where the hole enters solid material.
                double start = negative ? 0. : step;
                if (negative)
                    while (start < extent && !ring(start))
                        start += step;
                if (start >= extent || core(start) || !ring(start))
                    continue;
                double depth = start;
                while (depth < extent && !core(depth) && ring(depth))
                    depth += step;
                found.push_back({center + axis * (start - step), axis, radius, depth - start + step, !core(depth)});
            }
        }
    }
    // A through hole shows a circle at each end; keep one.
    std::vector<char> dropped(found.size(), 0);
    for (size_t i = 0; i < found.size(); ++i)
        for (size_t j = i + 1; j < found.size(); ++j) {
            const Vec3d offset = found[j].center - found[i].center;
            if (!dropped[j] && std::abs(found[i].radius - found[j].radius) < 0.02 * found[i].radius &&
                std::abs(found[i].axis.dot(found[j].axis)) > std::cos(FLAT_TOLERANCE) &&
                (offset - found[i].axis * offset.dot(found[i].axis)).norm() < found[i].radius / 4.)
                dropped[j] = 1;
        }
    for (size_t i = 0; i < found.size(); ++i) {
        if (dropped[i])
            continue;
        const auto&  hole = found[i];
        ModelFeature feature;
        feature.kind = "horizontal_hole";
        for (const Vec3d& end : {hole.center, Vec3d(hole.center + hole.axis * hole.depth)})
            for (int axis = 0; axis < 3; ++axis) {
                const double reach = hole.radius * std::sqrt(std::max(0., 1. - hole.axis[axis] * hole.axis[axis]));
                Vec3d        lo = end, hi = end;
                lo[axis] -= reach;
                hi[axis] += reach;
                feature.box.merge(lo);
                feature.box.merge(hi);
            }
        Polygon rectangle({Point::new_scale(feature.box.min.x(), feature.box.min.y()), Point::new_scale(feature.box.max.x(), feature.box.min.y()),
                           Point::new_scale(feature.box.max.x(), feature.box.max.y()), Point::new_scale(feature.box.min.x(), feature.box.max.y())});
        feature.footprint    = {ExPolygon(rectangle)};
        feature.measures     = {{"diameter_mm", round_to(2. * hole.radius, 0.01)},
                                {"axis", vec(hole.axis)},
                                {"depth_mm", round_to(hole.depth, 0.01)},
                                {"through", hole.through}};
        feature.significance = hole.radius;
        out.push_back(std::move(feature));
    }
}

void detect_parts(const ModelObject& object, const Transform3d& instance, const FeatureParams& p, std::vector<ModelFeature>& out)
{
    for (const auto* volume : object.volumes) {
        const Transform3d world = instance * volume->get_matrix();
        auto              add   = [&](const std::string& kind, const indexed_triangle_set& its, json measures) {
            ModelFeature feature;
            feature.kind = kind;
            for (const auto& v : its.vertices)
                feature.box.merge(world * v.cast<double>());
            if (!feature.box.defined)
                return;
            Triangles triangles;
            triangles.reserve(its.indices.size());
            for (const auto& tri : its.indices)
                triangles.push_back({world * its.vertices[tri[0]].cast<double>(), world * its.vertices[tri[1]].cast<double>(),
                                     world * its.vertices[tri[2]].cast<double>()});
            set_outline(feature, triangles, p);
            feature.measures     = std::move(measures);
            feature.measures["volume_id"] = std::to_string(volume->id().id);
            feature.significance = feature.box.size().prod();
            out.push_back(std::move(feature));
        };
        if (volume->text_configuration || volume->emboss_shape)
            add("text", volume->mesh().its,
                {{"text", volume->text_configuration ? volume->text_configuration->text.substr(0, 64) : std::string()},
                 {"mode", !volume->text_configuration ? "svg" : volume->is_model_part() ? "raised" : "engraved"}});
        if (volume->is_model_part() && !volume->mmu_segmentation_facets.empty()) {
            std::vector<indexed_triangle_set> per_state;
            volume->mmu_segmentation_facets.get_facets(*volume, per_state);
            for (size_t state = 1; state < per_state.size(); ++state)
                if (!per_state[state].indices.empty())
                    add("painted_region", per_state[state], {{"filament", state}});
        }
    }
}

// A level overhang between two anchors is a bridge, which needs no support.
// Record how much of each overhang the bridge detector covers.
void mark_bridged(std::vector<ModelFeature>& features, double layer_height)
{
    for (auto& overhang : features) {
        if (overhang.kind != "overhang")
            continue;
        ExPolygons bridged;
        for (const auto& bridge : features)
            if (bridge.kind == "bridge" && bridge.box.max.z() >= overhang.box.min.z() - layer_height &&
                bridge.box.min.z() <= overhang.box.max.z() + layer_height)
                append(bridged, bridge.footprint);
        double total = 0., covered = 0.;
        for (const auto& shape : overhang.footprint)
            total += shape.area();
        for (const auto& shape : intersection_ex(overhang.footprint, bridged))
            covered += shape.area();
        overhang.measures["bridged_percent"] = total > 0. ? round_to(std::min(100., 100. * covered / total), 1.) : 0.;
    }
}

const std::map<std::string, const char*>& definitions()
{
    static const std::map<std::string, const char*> text = {
        {"overhang", "Downward faces where at least half the outer wall line hangs over air, the slicer's two steepest overhang "
                     "speed bands. needs_support compares the slope with support_threshold_angle; bridged_percent is how much of "
                     "it the slicer can bridge instead."},
        {"shallow_slope", "Upward faces so shallow that each layer leaves a step wider than a line: visible stair-stepping."},
        {"flat_top", "Level upward faces: top surfaces, candidates for ironing."},
        {"curved_surface", "Smooth walls or tops that turn at least as far as a sharp corner without one; seams cannot hide there."},
        {"sharp_edge", "Mostly vertical edges where faces meet at a corner the seam placer snaps to. Concave corners hide seams; "
                       "ringing starts after convex ones."},
        {"bed_contact", "The first layer: contact area, sharp corners that brim ears would get, and height compared with the narrowest base."},
        {"bridge", "Areas printed over air with anchors at both ends, found with the slicer's own bridge detector."},
        {"floating", "Areas that start in mid-air with nothing below: they need support."},
        {"thin_wall", "Sections narrower than two outer wall lines: room for one wall loop only."},
        {"fine_detail", "Sections narrower than one outer wall line: Classic walls drop them unless detect_thin_wall is on."},
        {"small_section", "Islands whose outline is shorter than the small-perimeter length: pins, spires and tips."},
        {"thin_plate", "Horizontal sections with fewer layers than the top and bottom shells together."},
        {"vertical_hole", "Holes through the slices, open at one or both ends."},
        {"cavity", "Enclosed voids, capped above and below."},
        {"narrow_gap", "Gaps narrower than one outer wall line, between parts or inside slots; they may fuse."},
        {"horizontal_hole", "Round holes with a horizontal axis; their tops overhang or bridge."},
        {"material_boundary", "Where parts printed with different filaments meet."},
        {"painted_region", "Faces painted with a filament."},
        {"text", "Text and SVG parts made with the text and SVG tools."}};
    return text;
}
} // namespace

const std::vector<std::string>& feature_kinds()
{
    static const std::vector<std::string> kinds = [] {
        std::vector<std::string> result;
        for (const auto& [kind, text] : definitions())
            result.push_back(kind);
        return result;
    }();
    return kinds;
}

FeatureParams feature_params(const DynamicPrintConfig& config)
{
    FeatureParams p;
    // Per-extruder settings use the first extruder's value.
    auto number = [&](const char* key, double fallback) -> double {
        const auto* option = config.option(key);
        if (!option)
            return fallback;
        if (const auto* floats = dynamic_cast<const ConfigOptionFloats*>(option))
            return floats->values.empty() ? fallback : floats->values.front();
        if (const auto* floats = dynamic_cast<const ConfigOptionFloatsNullable*>(option))
            return floats->values.empty() || floats->is_nil(0) ? fallback : floats->values.front();
        if (option->type() == coInt)
            return option->getInt();
        return option->getFloat();
    };
    p.nozzle_diameter    = number("nozzle_diameter", p.nozzle_diameter);
    p.layer_height       = number("layer_height", p.layer_height);
    p.first_layer_height = number("initial_layer_print_height", p.layer_height);
    p.support_angle      = number("support_threshold_angle", p.support_angle);
    p.small_perimeter_r  = number("small_perimeter_threshold", 0.);
    p.ear_max_angle      = number("brim_ears_max_angle", p.ear_max_angle);
    p.ear_detection      = number("brim_ears_detection_length", p.ear_detection);
    p.shell_layers       = int(number("top_shell_layers", 3) + number("bottom_shell_layers", 3));
    if (config.has("outer_wall_line_width")) {
        const auto* width = config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
        const double value = width->get_abs_value(p.nozzle_diameter);
        p.line_width       = value > 0. ? value : Flow::auto_extrusion_width(frExternalPerimeter, float(p.nozzle_diameter));
    }
    return p;
}

std::uint64_t feature_geometry_key(const ModelObject& object, size_t instance, const DynamicPrintConfig& config)
{
    std::uint64_t hash = 14695981039346656037ull;
    auto          feed = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    // Field by field: the struct's padding bytes are indeterminate.
    const auto p = feature_params(config);
    for (double value : {p.layer_height, p.first_layer_height, p.line_width, p.nozzle_diameter, p.support_angle, p.small_perimeter_r,
                         p.ear_max_angle, p.ear_detection, double(p.shell_layers)})
        feed(&value, sizeof(value));
    const auto matrix = object.instances[instance]->get_matrix();
    feed(matrix.data(), sizeof(double) * 16);
    for (const auto* volume : object.volumes) {
        // Modifiers and support volumes change no detected geometry.
        if (!volume->is_model_part() && volume->type() != ModelVolumeType::NEGATIVE_VOLUME)
            continue;
        const auto key    = mesh_fingerprint(volume->mesh().its);
        const auto type   = int(volume->type());
        const auto filler = volume->extruder_id();
        const auto paint  = volume->mmu_segmentation_facets.timestamp();
        const auto part   = volume->get_matrix();
        feed(&key, sizeof(key));
        feed(&type, sizeof(type));
        feed(&filler, sizeof(filler));
        feed(&paint, sizeof(paint));
        feed(part.data(), sizeof(double) * 16);
    }
    for (const auto& value : object.layer_height_profile.get())
        feed(&value, sizeof(value));
    for (const auto& [range, settings] : object.layer_config_ranges) {
        feed(&range, sizeof(range));
        const auto stamp = settings.timestamp();
        feed(&stamp, sizeof(stamp));
    }
    return hash;
}

FeatureReport detect_model_features(const ModelObject& object, size_t instance, const DynamicPrintConfig& config,
                                    const std::function<void()>& throw_on_cancel)
{
    if (instance >= object.instances.size())
        throw std::runtime_error("That instance no longer exists.");
    FeatureReport report;
    report.object_id    = std::to_string(object.id().id);
    report.instance_id  = std::to_string(object.instances[instance]->id().id);
    report.params       = feature_params(config);
    report.geometry_key = feature_geometry_key(object, instance, config);
    const auto        cancel = [&] {
        if (throw_on_cancel)
            throw_on_cancel();
    };
    const Transform3d inst = object.instances[instance]->get_matrix();
    std::vector<Body> parts, negatives;
    const int         object_filament = object.config.has("extruder") ? std::max(1, object.config.get().opt_int("extruder")) : 1;
    for (const auto* volume : object.volumes) {
        if (!volume->is_model_part() && volume->type() != ModelVolumeType::NEGATIVE_VOLUME)
            continue;
        TriangleMesh mesh = volume->mesh();
        mesh.transform(inst * volume->get_matrix(), true);
        Body body{std::to_string(volume->id().id), std::move(mesh.its), volume->extruder_id() > 0 ? volume->extruder_id() : object_filament,
                  volume};
        (volume->is_model_part() ? parts : negatives).push_back(std::move(body));
    }
    if (parts.empty())
        throw std::runtime_error("The object has no solid parts.");
    const auto box = object.instance_bounding_box(instance);
    SolidTest  test(parts, negatives);
    detect_faces(printed_surfaces(parts, negatives, test, report.params.line_width), report.params, box.min.z(), report.features);
    cancel();
    const bool several_filaments = std::any_of(parts.begin(), parts.end(), [&](const Body& b) { return b.filament != parts.front().filament; });
    const auto layers            = slice_layers(object, instance, config, several_filaments, cancel);
    detect_layers(layers, report.params, box.size().z(), report.features);
    mark_bridged(report.features, report.params.layer_height);
    cancel();
    if (several_filaments)
        detect_material_boundaries(layers, parts, report.params, report.features);
    std::vector<Body> all = parts;
    all.insert(all.end(), negatives.begin(), negatives.end());
    detect_horizontal_holes(all, test, report.params, box.size().norm(), report.features);
    detect_parts(object, inst, report.params, report.features);
    cancel();
    // Largest first within each kind, so IDs and truncation favour what matters.
    std::stable_sort(report.features.begin(), report.features.end(), [](const ModelFeature& a, const ModelFeature& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.significance > b.significance;
    });
    std::map<std::string, int> counters;
    for (auto& feature : report.features)
        feature.id = feature.kind + "-" + std::to_string(++counters[feature.kind]);
    return report;
}

json feature_report_json(const FeatureReport& report, const std::vector<std::string>& kinds, size_t limit)
{
    for (const auto& kind : kinds)
        if (!definitions().count(kind))
            throw std::runtime_error("Unknown feature kind: " + kind + ".");
    json features = json::array(), counts = json::object(), defined = json::object();
    size_t omitted = 0;
    std::map<std::string, size_t> shown;
    for (const auto& feature : report.features) {
        if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), feature.kind) == kinds.end())
            continue;
        counts[feature.kind] = counts.value(feature.kind, 0) + 1;
        defined[feature.kind] = definitions().at(feature.kind);
        if (shown[feature.kind]++ >= limit) {
            ++omitted;
            continue;
        }
        json item = {{"id", feature.id},
                     {"kind", feature.kind},
                     {"box_min_mm", vec(feature.box.min)},
                     {"box_max_mm", vec(feature.box.max)}};
        item.update(feature.measures);
        features.push_back(std::move(item));
    }
    const auto& p = report.params;
    return {{"object_id", report.object_id},
            {"instance_id", report.instance_id},
            {"detected_with",
             {{"layer_height_mm", p.layer_height},
              {"outer_wall_line_width_mm", round_to(p.line_width, 0.001)},
              {"nozzle_diameter_mm", p.nozzle_diameter},
              {"support_threshold_angle_deg", p.support_angle},
              {"shell_layers", p.shell_layers}}},
            {"counts", counts},
            {"features", features},
            {"omitted", omitted},
            {"definitions", defined},
            {"coordinates", "World millimetres, like get_model_context."}};
}

namespace {
ExPolygons grown(const ExPolygons& shapes, double margin)
{
    return margin > 0. ? offset_ex(shapes, float(scale_(margin))) : union_ex(shapes);
}

// Simplified by about a footprint cell, so the grid's steps do not become facets;
// a modifier needs no finer outline.
ExPolygons simplified(const ExPolygons& shapes)
{
    ExPolygons result;
    for (const auto& island : shapes)
        append(result, island.simplify(scale_(0.25)));
    return result;
}

double total_area(const ExPolygons& shapes)
{
    double area = 0.;
    for (const auto& shape : shapes)
        area += shape.area();
    return area;
}

void add_prism(indexed_triangle_set& its, const ExPolygons& shape, double z0, double z1)
{
    auto prism = Emboss::polygons2model(shape, Emboss::ProjectZ(scale_(z1 - z0)));
    for (auto& v : prism.vertices)
        v = Vec3f(float(v.x() * SCALING_FACTOR), float(v.y() * SCALING_FACTOR), float(z0 + v.z() * SCALING_FACTOR));
    its_merge(its, prism);
}

// Prisms band by band. Each band takes the slabs within margin of it, grown by
// margin, so the stack is the feature grown in every direction. Bands are no
// finer than the margin, which blurs finer steps anyway, and neighbours that
// barely differ share a prism, so a straight wall stays one piece.
void add_stack(indexed_triangle_set& its, const std::vector<FeatureSlab>& slabs, double lo, double hi, double margin)
{
    const int    count = std::clamp(int(std::ceil((hi - lo) / std::max(margin, 0.1) - 1e-9)), 1, MAX_BANDS);
    const double band  = (hi - lo) / count;
    struct Prism
    {
        int        last_band;
        double     z0, z1, area;
        ExPolygons shape;
    };
    std::vector<Prism> prisms;
    for (int k = 0; k < count; ++k) {
        // The same expression for a band's top and the next band's bottom, so
        // neighbouring prisms meet exactly.
        const double z0 = lo + k * band, z1 = k + 1 == count ? hi : lo + (k + 1) * band;
        ExPolygons   shapes;
        for (const auto& slab : slabs)
            if (slab.z_hi >= z0 - margin && slab.z_lo <= z1 + margin)
                append(shapes, slab.shape);
        // Compared before simplifying, which picks different corners from
        // nearly equal outlines.
        auto shape = shapes.empty() ? ExPolygons{} : grown(shapes, margin);
        if (shape.empty())
            continue;
        const double area = total_area(shape);
        if (!prisms.empty() && prisms.back().last_band == k - 1) {
            auto&        last    = prisms.back();
            const double changed = total_area(diff_ex(shape, last.shape)) + total_area(diff_ex(last.shape, shape));
            if (changed <= 0.01 * std::max(last.area, area)) {
                append(last.shape, std::move(shape));
                last.shape     = union_ex(last.shape);
                last.area      = total_area(last.shape);
                last.z1        = z1;
                last.last_band = k;
                continue;
            }
        }
        prisms.push_back({k, z0, z1, area, std::move(shape)});
    }
    for (const auto& prism : prisms)
        add_prism(its, simplified(prism.shape), prism.z0, prism.z1);
}
} // namespace

indexed_triangle_set feature_modifier_mesh(const ModelFeature& feature, bool follow, double margin)
{
    if (!feature.box.defined)
        throw std::runtime_error("This feature has no geometry.");
    margin = std::max(0., margin);
    // A level feature without a margin still gets a printable thickness.
    const double pad = std::max(0., 0.1 - feature.box.size().z() - 2. * margin) / 2.;
    const double z0 = feature.box.min.z() - margin - pad, z1 = feature.box.max.z() + margin + pad;
    indexed_triangle_set its;
    if (follow && !feature.slabs.empty())
        add_stack(its, feature.slabs, z0, z1, margin);
    else if (follow && !feature.footprint.empty())
        add_prism(its, simplified(grown(feature.footprint, margin)), z0, z1);
    else {
        const Vec3d lo = feature.box.min - Vec3d(margin, margin, margin + pad);
        const Vec3d size = feature.box.max + Vec3d(margin, margin, margin + pad) - lo;
        its              = its_make_cube(size.x(), size.y(), size.z());
        for (auto& v : its.vertices)
            v += lo.cast<float>();
    }
    if (its.indices.empty())
        throw std::runtime_error("The modifier mesh is empty.");
    return its;
}

} // namespace Slic3r::AIPrintAssistant
