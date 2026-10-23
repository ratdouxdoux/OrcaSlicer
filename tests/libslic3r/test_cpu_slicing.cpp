#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/FillRectilinear.hpp"
#include "libslic3r/MixedFilamentLayerTracker.hpp"
#include "libslic3r/MixedFilamentChannelMap.hpp"
#include "libslic3r/LayerGeometryBatch.hpp"
#include "libslic3r/LocalZMaskTimeline.hpp"
#include "libslic3r/Fill/NarrowRoofInfill.hpp"
#include "libslic3r/PaintedWallInset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Support/TreeSupport.hpp"
#include <tbb/task_arena.h>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string>

using namespace Slic3r;
using Catch::Approx;

namespace {
ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    return ExPolygon(Polygon(Points{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}));
}
} // namespace

TEST_CASE("Independent Local-Z passes partition changing material ownership", "[LocalZ][Ownership]")
{
    const auto box = [](double x0, double y0, double x1, double y1) { return rectangle(scale_(x0), scale_(y0), scale_(x1), scale_(y1)); };
    const ExPolygons            small{box(0, 0, 3, 4)}, large{box(0, 0, 8, 4)};
    std::vector<LocalZMaskSlab> input;
    SECTION("unchanged footprint keeps the full thick pass") { input = {{0, .1, .2, small}, {1, .2, .3, small}, {2, .3, .4, small}}; }
    SECTION("growth cannot inherit the earlier material's thickness")
    {
        input = {{0, .1, .2, small}, {1, .2, .3, large}, {2, .3, .4, large}};
    }
    SECTION("shrinking area is completed on its last owning layer")
    {
        input = {{0, .1, .2, large}, {1, .2, .3, small}, {2, .3, .4, small}};
    }
    SECTION("departing island and returning island have separate spans")
    {
        input = {{0, .1, .2, large}, {1, .2, .3, small}, {2, .3, .4, large}};
    }
    SECTION("split and merge with a hole and disconnected island")
    {
        ExPolygon ring = box(0, 0, 8, 4);
        Polygon   hole = box(3, 1, 5, 3).contour;
        hole.reverse();
        ring.holes.push_back(hole);
        input = {{0, .1, .2, large}, {1, .2, .3, {ring, box(10, 0, 12, 4)}}, {2, .3, .4, large}};
    }
    SECTION("no ownership across an empty slab") { input = {{0, .1, .2, large}, {1, .2, .3, {}}, {2, .3, .4, large}}; }
    SECTION("short end pieces keep their actual thickness") { input = {{0, .18, .2, small}, {1, .2, .28, large}, {2, .28, .301, small}}; }
    const auto output = partition_local_z_mask_span(input);
    REQUIRE_FALSE(output.empty());
    double expected_volume = 0.0, actual_volume = 0.0;
    for (const auto& slab : input) {
        expected_volume += area(slab.masks) * (slab.z_hi - slab.z_lo);
        ExPolygons   coverage;
        const double middle = (slab.z_lo + slab.z_hi) / 2;
        for (const auto& span : output)
            if (span.z_lo < middle && span.z_hi > middle)
                append(coverage, span.masks);
        CHECK(area(diff_ex(slab.masks, coverage)) < 1.0);
        CHECK(area(diff_ex(coverage, slab.masks)) < 1.0);
    }
    for (size_t i = 0; i < output.size(); ++i) {
        const auto& span = output[i];
        CHECK(span.z_hi > span.z_lo);
        CHECK(span.z_hi == Approx(input[span.layer_id].z_hi));
        CHECK(area(diff_ex(span.masks, input[span.layer_id].masks)) < 1.0);
        actual_volume += area(span.masks) * (span.z_hi - span.z_lo);
        for (size_t j = 0; j < i; ++j)
            if (std::min(span.z_hi, output[j].z_hi) > std::max(span.z_lo, output[j].z_lo) + EPSILON)
                CHECK(area(intersection_ex(span.masks, output[j].masks)) < 1.0);
    }
    CHECK(actual_volume == Approx(expected_volume));
    if (input[0].masks == input[1].masks && input[1].masks == input[2].masks) {
        REQUIRE(output.size() == 1);
        CHECK(output.front().z_lo == Approx(.1));
        CHECK(output.front().z_hi == Approx(.4));
    }
}

TEST_CASE("Painted Classic wall insets do not amplify sub-nozzle boundary teeth", "[CPUSlicing][PaintedWall]")
{
    std::ifstream fixture(std::string(TEST_DATA_DIR) + "/painted_wall_outline.txt");
    REQUIRE(fixture.good());
    Points  contour;
    coord_t x, y;
    while (fixture >> x >> y)
        contour.emplace_back(x, y);
    REQUIRE(contour.size() == 390);
    const ExPolygons region{ExPolygon(Polygon(std::move(contour)))};
    const ExPolygons object{rectangle(0, scale_(-6.), scale_(20.), scale_(5.))};
    const float      distance        = scale_(.21);
    const ExPolygons original        = offset_ex(region, -distance);
    const ExPolygons corrected       = inset_painted_wall(region, offset_ex(object, -distance), distance);
    const auto       width_at_middle = [](const ExPolygons& polygons) {
        double length = 0.;
        for (const Polyline& line : intersection_pl(Polylines{Polyline(Points{Point(0, 0), Point::new_scale(20., 0.)})}, polygons))
            length += line.length();
        return unscale<double>(length);
    };
    CHECK(width_at_middle(original) < 3.9);
    CHECK(width_at_middle(corrected) > 4.2);
    CHECK(area(diff_ex(original, corrected)) < 1.);
    CHECK(area(diff_ex(corrected, region)) < 1.);
    // The repaired first inset leaves a normal-sized pocket after inner walls.
    const float      spacing          = scale_(.435 - .08 * (1. - .25 * PI));
    const float      half_min_spacing = scale_((.45 - .08 * (1. - .25 * PI)) * (1. - INSET_OVERLAP_TOLERANCE) / 2.);
    const ExPolygons inside           = offset2_ex(corrected, -spacing - half_min_spacing + 1, half_min_spacing - 1);
    CHECK(width_at_middle(inside) > 3.3);
    // Round joins approximate the true nozzle-radius inset within 5 microns.
    size_t samples = 0;
    for (const ExPolygon& poly : corrected)
        for (const Point& point : poly.contour.points)
            if (point.y() > scale_(-1.) && point.y() < scale_(1.)) {
                double measured = std::numeric_limits<double>::infinity();
                for (const Line& line : region.front().contour.lines())
                    measured = std::min(measured, unscale<double>(line.distance_to(point)));
                CHECK(measured >= .2049);
                CHECK(measured <= .2151);
                ++samples;
            }
    CHECK(samples > 2);
    // Both sides of the same painted boundary keep disjoint nozzle envelopes.
    const ExPolygons other       = diff_ex(object, region);
    const ExPolygons other_inset = inset_painted_wall(other, offset_ex(object, -distance), distance);
    const ExPolygons envelope_a  = offset_ex(corrected, distance - scale_(.006), jtRound, scale_(.001));
    const ExPolygons envelope_b  = offset_ex(other_inset, distance - scale_(.006), jtRound, scale_(.001));
    CHECK(area(intersection_ex(envelope_a, envelope_b)) < 1.);
    // The former miter expansion captured the neighbour's wall despite the
    // requested 0.10 mm guard being smaller than its 0.21 mm inset.
    const Polylines neighbor_lines = to_polylines(other_inset);
    CHECK_FALSE(intersection_pl(neighbor_lines, offset_ex(region, scale_(.10))).empty());
    CHECK(intersection_pl(neighbor_lines, expand_local_z_masks(region, scale_(.10))).empty());
    CHECK(expand_local_z_masks({}, scale_(.10)).empty());
    CHECK(expand_local_z_masks(region, 0.) == region);
    CHECK(expand_local_z_masks(region, -scale_(100.)) == region);
    CHECK(expand_local_z_masks(region, -scale_(100.), false).empty());
}

TEST_CASE("Classic painted inset keeps printable slivers and removes Boolean grid needles", "[CPUSlicing][PaintedWall]")
{
    const auto p = [](double x, double y) { return Point::new_scale(x, y); };
    Points     teeth{p(0, 0), p(1.2, 0)};
    for (int i = 1; i <= 40; ++i)
        teeth.push_back(p(1.2 - (i % 2) * .09, i * .044));
    teeth.push_back(p(0, 40 * .044));
    for (int i = 39; i > 0; --i)
        teeth.push_back(p((i % 2) * .09, i * .044));
    const ExPolygons strip{ExPolygon(Polygon(std::move(teeth)))};
    const ExPolygons object{rectangle(scale_(-10.), scale_(-10.), scale_(20.), scale_(20.))};
    CHECK(offset2_ex(strip, -scale_(.4), scale_(.15)).empty());
    CHECK_FALSE(offset_ex(inset_painted_wall(strip, offset_ex(object, -scale_(.4)), scale_(.4)), scale_(.15)).empty());
    const ExPolygons rib{rectangle(0, 0, scale_(.5), scale_(3.))};
    CHECK(inset_painted_wall(rib, offset_ex(rib, -scale_(.4)), scale_(.4)).empty());

    std::ifstream fixture(std::string(TEST_DATA_DIR) + "/painted_wall_transition.txt");
    REQUIRE(fixture.good());
    Points  contour;
    coord_t x, y;
    while (fixture >> x >> y)
        contour.emplace_back(x, y);
    REQUIRE(contour.size() > 3);
    ExPolygons corrected = inset_painted_wall({ExPolygon(Polygon(std::move(contour)))}, offset_ex(object, -209999.5f), 209999.5f);
    REQUIRE_FALSE(corrected.empty());
    // This transition used to contain a 0.21 mm return path only one integer
    // unit wide. G-code rounding turned it into a double extrusion.
    for (ExPolygon& poly : corrected) {
        poly.contour.round_to_grid(scale_(.001));
        remove_same_neighbor(poly.contour);
        const Points& pts = poly.contour.points;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Vec2d a = (pts[i] - pts[(i + pts.size() - 1) % pts.size()]).cast<double>();
            const Vec2d b = (pts[(i + 1) % pts.size()] - pts[i]).cast<double>();
            CHECK_FALSE((a.dot(b) < 0. && a.x() * b.y() == a.y() * b.x()));
        }
    }
}

TEST_CASE("Painted wall correction preserves physical corners holes and thin islands", "[CPUSlicing][PaintedWall]")
{
    const auto p = [](double x, double y) { return Point(scale_(x), scale_(y)); };
    ExPolygon  physical(Polygon(Points{p(0, 0), p(10, 0), p(10, 10), p(6, 10), p(5, 7), p(4, 10), p(0, 10)}));
    Polygon    hole = rectangle(scale_(2.), scale_(2.), scale_(4.), scale_(4.)).contour;
    hole.reverse();
    physical.holes.push_back(hole);
    const ExPolygons object{physical, rectangle(scale_(12.), 0, scale_(12.3), scale_(4.))};
    const float      distance = scale_(.21);
    const ExPolygons expected = offset_ex(object, -distance);
    const ExPolygons whole    = inset_painted_wall(object, expected, distance);
    CHECK(area(diff_ex(whole, expected)) < 1.);
    CHECK(area(diff_ex(expected, whole)) < 1.);
    const ExPolygons painted   = intersection_ex(object, ExPolygons{rectangle(0, 0, scale_(6.5), scale_(11.))});
    const ExPolygons old       = offset_ex(painted, -distance);
    const ExPolygons corrected = inset_painted_wall(painted, expected, distance);
    CHECK(area(diff_ex(old, corrected)) < 1.);
    CHECK(area(diff_ex(diff_ex(corrected, old), expected)) < 1.);
    CHECK(area(intersection_ex(corrected, ExPolygons{ExPolygon(rectangle(scale_(2.), scale_(2.), scale_(4.), scale_(4.)).contour)})) < 1.);
    // An acute convex painted tip keeps its ordinary inset as well.
    const ExPolygons tip{ExPolygon(Polygon(Points{p(1, 1), p(7, 1), p(1, 2)}))};
    const ExPolygons sharp = inset_painted_wall(tip, offset_ex(ExPolygons{rectangle(0, 0, scale_(10.), scale_(10.))}, -distance), distance);
    const ExPolygons sharp_expected = offset_ex(tip, -distance);
    CHECK(area(diff_ex(sharp, sharp_expected)) < 1.);
    CHECK(area(diff_ex(sharp_expected, sharp)) < 1.);
}

TEST_CASE("Roof gap recovery preserves connected paths and unfillable features", "[Infill][RoofPartition][GapStub]")
{
    const auto line = [](std::initializer_list<Point> points, double width) {
        ThickPolyline path;
        path.points = points;
        path.width.assign(2 * (path.points.size() - 1), scale_(width));
        return path;
    };
    const auto          p    = [](double x, double y) { return Point(scale_(x), scale_(y)); };
    const ThickPolyline stub = line({p(5, 5), p(5, 5.08)}, .8);
    ThickPolylines      paths{stub,
                         line({p(6, 2), p(6, 8)}, .5),
                         line({p(7, 5), p(7, 5.08)}, .8),
                         line({p(7, 5.08), p(7, 8)}, .5),
                         line({p(8, 5), p(8.05, 5), p(8.05, 5.05), p(8, 5)}, .8),
                         line({p(1, 5), p(1, 5.08)}, .8)};
    const ExPolygons    fillable{rectangle(scale_(4.), scale_(1.), scale_(9.), scale_(9.))};
    CHECK(reclaim_roof_gap_stubs(paths, [](const ThickPolylines&) { return ExPolygons{}; }, 0.) == 0);
    CHECK(reclaim_roof_gap_stubs(paths, [&](const ThickPolylines&) { return fillable; }, scale_(.02)) == 1);
    REQUIRE(paths.size() == 5);
    CHECK(paths.front().points == Points{p(6, 2), p(6, 8)});
    CHECK(reclaim_roof_gap_stubs(paths, [&](const ThickPolylines&) { return fillable; }, scale_(.02)) == 0);
    // A rejected candidate can prevent a neighbouring candidate from being
    // filled too. Neither may be discarded after the first tentative pass.
    paths = {line({p(4.2, 5), p(4.2, 5.08)}, .8), line({p(4.8, 5), p(4.8, 5.08)}, .8), line({p(6, 6), p(6, 6.08)}, .8)};
    const auto after_retained_gaps = [&](const ThickPolylines& retained) {
        Polygons occupied;
        for (const ThickPolyline& path : retained)
            append(occupied, offset(Polyline(path.points), float(path.width.front() * .5 + 10.)));
        return diff_ex(fillable, occupied);
    };
    CHECK(reclaim_roof_gap_stubs(paths, after_retained_gaps, 0.) == 1);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0].first_point() == p(4.2, 5));
    CHECK(paths[1].first_point() == p(4.8, 5));
    // A stub crossing the infill boundary must not be dropped, even if its
    // centre is inside: this could be a real gap between walls.
    paths = {stub};
    CHECK(reclaim_roof_gap_stubs(
              paths, [&](const ThickPolylines&) { return ExPolygons{rectangle(scale_(4.9), scale_(4.), scale_(6.), scale_(6.))}; }, 0.) ==
          0);
}

TEST_CASE("Sawtoothed painted roof leaves no isolated wide gap stubs in printable infill", "[Infill][RoofPartition][GapStub]")
{
    std::ifstream fixture(std::string(TEST_DATA_DIR) + "/painted_roof_gap.txt");
    REQUIRE(fixture.good());
    Points  contour;
    coord_t x, y;
    while (fixture >> x >> y)
        contour.emplace_back(x, y);
    REQUIRE(contour.size() == 203);
    ExPolygon      gap(Polygon(std::move(contour)));
    ThickPolylines paths;
    gap.medial_axis(scale_(.2 * .45 * (1. - INSET_OVERLAP_TOLERANCE)), scale_(2. * (.45 - .08 * (1. - .25 * PI))), &paths);
    REQUIRE_FALSE(paths.empty());
    const ExPolygons fillable{rectangle(scale_(13.4), scale_(-1.), scale_(14.8), scale_(3.3))};
    CHECK(reclaim_roof_gap_stubs(paths, [&](const ThickPolylines&) { return fillable; }, scale_(.08 * (1. - .25 * PI))) >= 4);
    // The genuine gap at the bend lies outside the printable roof rectangle.
    CHECK_FALSE(paths.empty());
    CHECK(reclaim_roof_gap_stubs(paths, [&](const ThickPolylines&) { return fillable; }, scale_(.08 * (1. - .25 * PI))) == 0);
}

TEST_CASE("Painted roof detection respects shell depth at steps and the object top", "[Infill][RoofPartition]")
{
    struct TestLayer : Layer
    {
        explicit TestLayer(size_t id) : Layer(id, nullptr, .2, .2 * (id + 1), .2 * id + .1) {}
    };
    std::vector<std::unique_ptr<TestLayer>> layers;
    const ExPolygons                        base{rectangle(0, 0, scale_(20.), scale_(20.))};
    const ExPolygons                        tower{rectangle(scale_(5.), scale_(5.), scale_(15.), scale_(15.))};
    for (size_t i = 0; i < 7; ++i) {
        layers.emplace_back(std::make_unique<TestLayer>(i));
        layers.back()->lslices = i < 4 ? base : tower;
        if (i > 0) {
            layers[i - 1]->upper_layer = layers[i].get();
            layers[i]->lower_layer     = layers[i - 1].get();
        }
    }
    // The step's top is layer 3, so a three-layer shell reaches layer 1, not 0.
    CHECK(area(roof_covered_outline(*layers[0], 3, 0.)) == Approx(area(base)));
    CHECK(area(roof_covered_outline(*layers[1], 3, 0.)) == Approx(area(tower)));
    CHECK(area(roof_covered_outline(*layers[0], 3, .61)) == Approx(area(tower)));
    CHECK(area(roof_covered_outline(*layers[0], 3, .6)) == Approx(area(base)));
    // Reaching the final existing outline must not invent an extra shell layer.
    CHECK(area(roof_covered_outline(*layers[3], 3, 0.)) == Approx(area(tower)));
    CHECK(roof_covered_outline(*layers[4], 3, 0.).empty());
    CHECK(roof_covered_outline(*layers[6], 3, 0.).empty());
}

TEST_CASE("Painted roof pockets use straight fill without removing narrow ribs", "[Infill][RoofPartition]")
{
    const ExPolygon  body   = rectangle(0, 0, scale_(20.), scale_(20.));
    const ExPolygon  rib    = rectangle(scale_(30.), 0, scale_(31.2), scale_(10.));
    const ExPolygon  pocket = rectangle(scale_(5.), scale_(5.), scale_(6.), scale_(6.));
    const ExPolygons outline{body, rib};
    ExPolygons       normal, narrow{pocket, rib};
    const ExPolygons broad = broad_roof_support(outline, {}, .42);
    keep_partitioned_roof_infill_straight(normal, narrow, broad);
    CHECK(area(diff_ex(ExPolygons{pocket}, normal)) < 1.0);
    CHECK(area(intersection_ex(ExPolygons{pocket}, narrow)) < 1.0);
    CHECK(area(diff_ex(ExPolygons{rib}, narrow)) < 1.0);
    CHECK(area(intersection_ex(ExPolygons{rib}, normal)) < 1.0);
    ExPolygons covered = normal;
    append(covered, narrow);
    CHECK(area(union_ex(covered)) == Approx(pocket.area() + rib.area()));
    CHECK(broad_roof_support(outline, outline, .42).empty());
    // A hole remains excluded; the correction must never invent extrusion.
    ExPolygon holed = body;
    Polygon   hole  = pocket.contour;
    hole.reverse();
    holed.holes.push_back(hole);
    CHECK(area(intersection_ex(broad_roof_support({holed}, {}, .42), ExPolygons{pocket})) < 1.0);

    // Infill overlaps the inner wall slightly; this must not make a broad roof
    // pocket look like a genuine narrow rib just because it crosses that band.
    const ExPolygon edge_pocket  = rectangle(scale_(.8), scale_(5.), scale_(1.8), scale_(6.));
    const ExPolygon inside_walls = rectangle(scale_(.84), scale_(.84), scale_(19.16), scale_(19.16));
    normal.clear();
    narrow = {edge_pocket, rib};
    keep_partitioned_roof_infill_straight(normal, narrow, broad_roof_support({inside_walls, rib}, {}, .42), .21);
    CHECK(area(diff_ex(ExPolygons{edge_pocket}, normal)) < 1.0);
    CHECK(area(diff_ex(ExPolygons{rib}, narrow)) < 1.0);
}

TEST_CASE("Infill gap filling preserves density boundaries and bridge exclusions", "[CPUSlicing][InfillGap]")
{
    struct TestFill : FillRectilinear
    {
        using Fill::_create_gap_fill;
    };
    TestFill          fill;
    PrintObjectConfig object_config;
    PrintRegionConfig region_config;
    object_config.gap_fill_target.value     = gftEverywhere;
    region_config.filter_out_gap_fill.value = 0;
    fill.print_object_config                = &object_config;
    const ExPolygon gap                     = rectangle(0, 0, scale_(10.), scale_(0.4));
    fill.no_overlap_expolygons              = {gap};
    Surface    surface(stTop, gap);
    FillParams params;
    params.flow           = Flow(0.45f, 0.2f, 0.4f);
    params.config         = &region_config;
    params.extrusion_role = erTopSolidInfill;

    for (float density : {0.f, 0.15f, std::nextafter(1.f, 0.f), std::numeric_limits<float>::quiet_NaN()}) {
        params.density = density;
        ExtrusionEntityCollection out;
        fill._create_gap_fill(&surface, params, &out);
        CHECK(out.entities.empty());
    }
    params.density = 1.f;
    {
        ExtrusionEntityCollection out;
        fill._create_gap_fill(&surface, params, &out);
        REQUIRE_FALSE(out.entities.empty());
        CHECK(out.entities.front()->role() == erGapFill);
    }
    for (ExtrusionRole role : {erBridgeInfill, erInternalBridgeInfill, erOverhangPerimeter}) {
        params.extrusion_role = role;
        ExtrusionEntityCollection out;
        fill._create_gap_fill(&surface, params, &out);
        CHECK(out.entities.empty());
    }
    params.extrusion_role = erTopSolidInfill;
    fill.no_overlap_expolygons.clear();
    ExtrusionEntityCollection empty;
    fill._create_gap_fill(&surface, params, &empty);
    CHECK(empty.entities.empty());
    fill.no_overlap_expolygons          = {gap};
    object_config.gap_fill_target.value = gftNowhere;
    fill._create_gap_fill(&surface, params, &empty);
    CHECK(empty.entities.empty());
    object_config.gap_fill_target.value = gftTopBottom;
    surface.surface_type                = stInternalSolid;
    fill._create_gap_fill(&surface, params, &empty);
    CHECK(empty.entities.empty());
    surface.surface_type = stTop;
    fill._create_gap_fill(&surface, params, &empty);
    CHECK_FALSE(empty.entities.empty());
}

TEST_CASE("Mixed channel snapshots preserve hidden rows and borrowed legacy edits", "[CPUSlicing][PaintPreparation]")
{
    MixedFilamentManager       manager;
    std::vector<MixedFilament> rows(4);
    for (auto& row : rows) {
        row.component_a = 1;
        row.component_b = 2;
    }
    rows[0].deleted = true;
    rows[1].enabled = false;
    manager.set_mixed_filament_legacy_rows(rows, 2, {"#FFFFFF", "#000000"});
    const auto initial = mixed_filament_channel_rows(manager, 2, 7);
    REQUIRE(initial == std::vector<int>{-1, -1, -1, 2, 3, -1, -1});
    auto& borrowed      = manager.mixed_filaments();
    borrowed[2].enabled = false;
    const auto updated  = mixed_filament_channel_rows(manager, 2, 7);
    REQUIRE(updated == std::vector<int>{-1, -1, -1, 3, -1, -1, -1});
    CHECK(initial[3] == 2);
    for (size_t channel = 0; channel < updated.size(); ++channel)
        CHECK(updated[channel] == manager.mixed_index_from_filament_id(unsigned(channel), 2));
    CHECK(mixed_filament_channel_rows(manager, 2, 0).empty());
}

TEST_CASE("Layer geometry batches preserve progress and neighbor halos at boundaries", "[CPUSlicing][PaintPreparation]")
{
    struct Fixture
    {
        std::vector<size_t> points, ends;
    };
    std::vector<Fixture> fixtures{{std::vector<size_t>(64, 1), {64}},
                                  {std::vector<size_t>(65, 1), {64, 65}},
                                  {std::vector<size_t>(66, 1), {64, 66}},
                                  {{200, 1, 1}, {1, 3}},
                                  {{1, 200, 1}, {1, 2, 3}},
                                  {{1, 1, 200}, {2, 3}},
                                  {{0}, {1}}};
    fixtures[2].points[64] = 0;
    for (const auto& fixture : fixtures) {
        size_t begin = 0;
        for (size_t expected_end : fixture.ends) {
            const auto batch = next_layer_geometry_batch(begin, fixture.points.size(), 64, 128,
                                                         [&](size_t i) { return fixture.points.at(i); });
            REQUIRE(batch.end == expected_end);
            CHECK(batch.begin == begin);
            CHECK(batch.neighbor_begin == (begin == 0 ? 0 : begin - 1));
            CHECK(batch.neighbor_end == std::min(expected_end + 1, fixture.points.size()));
            if (batch.input_points > 128)
                CHECK(batch.end == begin + 1);
            for (size_t layer = batch.begin; layer < batch.end; ++layer) {
                CHECK(layer >= batch.neighbor_begin);
                CHECK(layer < batch.neighbor_end);
                if (layer > 0)
                    CHECK(layer - 1 >= batch.neighbor_begin);
                if (layer + 1 < fixture.points.size())
                    CHECK(layer + 1 < batch.neighbor_end);
            }
            begin = batch.end;
        }
        CHECK(begin == fixture.points.size());
    }
}

TEST_CASE("Cheap mixed-component overlap preserves holes, contacts and earliest activation", "[CPUSlicing][PaintPreparation]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const std::vector<ExPolygons> layers{{rectangle(200, 0, 250, 50)},
                                         {ring, rectangle(200, 0, 250, 50)},
                                         {rectangle(25, 25, 75, 75), rectangle(5, 5, 15, 15), rectangle(100, 0, 110, 10),
                                          rectangle(200, 0, 250, 50)},
                                         {rectangle(0, 0, 300, 100)},
                                         {},
                                         {ring},
                                         {ring}};
    MixedFilamentLayerTracker     reference, optimized(true);
    std::vector<int>              expected, actual;
    for (size_t layer = 0; layer < layers.size(); ++layer) {
        reference.track_layer(layers[layer], int(layer), expected);
        optimized.track_layer(layers[layer], int(layer), actual);
        REQUIRE(actual == expected);
        if (layer == 3)
            CHECK(actual == std::vector<int>{3});
    }
}

TEST_CASE("Mixed painted components retain cadence through splits and merges", "[CPUSlicing][MixedFilament]")
{
    // All layers remain alive, as in the segmentation pass that owns the masks.
    const std::vector<ExPolygons>       layers{{rectangle(0, 0, 100, 100)},
                                               {rectangle(0, 0, 40, 100), rectangle(60, 0, 100, 100), rectangle(200, 0, 300, 100)},
                                               {rectangle(0, 0, 300, 100)},
                                               {},
                                               {rectangle(0, 0, 100, 100)}};
    const std::vector<std::vector<int>> expected{{0}, {1, 1, 0}, {2}, {}, {0}};
    MixedFilamentLayerTracker           tracker;
    std::vector<int>                    indices;
    for (size_t i = 0; i < layers.size(); ++i) {
        tracker.track_layer(layers[i], int(i), indices);
        REQUIRE(indices == expected[i]);
    }
}

TEST_CASE("Mixed painted cadence uses polygon overlap including holes", "[CPUSlicing][MixedFilament]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const ExPolygons          first{ring};
    const ExPolygons          second{rectangle(30, 30, 70, 70), rectangle(5, 5, 15, 15), ExPolygon{}};
    MixedFilamentLayerTracker tracker;
    std::vector<int>          indices;
    tracker.track_layer(first, 5, indices);
    tracker.track_layer(second, 6, indices);
    REQUIRE(indices == std::vector<int>{0, 1, -1});
}

TEST_CASE("Mixed painted channels have independent activation runs", "[CPUSlicing][MixedFilament]")
{
    const ExPolygons          masks{rectangle(0, 0, 100, 100)};
    MixedFilamentLayerTracker first, second;
    std::vector<int>          first_indices, second_indices;
    first.track_layer(masks, 0, first_indices);
    second.track_layer(masks, 2, second_indices);
    first.track_layer(masks, 3, first_indices);
    second.track_layer(masks, 3, second_indices);
    REQUIRE(first_indices == std::vector<int>{3});
    REQUIRE(second_indices == std::vector<int>{1});
}

TEST_CASE("Read-only extrusion flattening borrows leaves without changing order", "[CPUSlicing][GCode]")
{
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{0, 0}, {100, 0}, {100, 100}});
    ExtrusionEntityCollection inner;
    inner.no_sort = true;
    inner.append(ExtrusionLoop(path));
    inner.append(ExtrusionMultiPath(path));
    ExtrusionEntityCollection source;
    source.append(path);
    source.append(inner);
    source.append(ExtrusionEntityCollection{});
    source.append(path);

    const auto  leaves = source.flatten_refs();
    const auto* nested = static_cast<const ExtrusionEntityCollection*>(source.entities[1]);
    REQUIRE(leaves == std::vector<const ExtrusionEntity*>{source.entities[0], nested->entities[0], nested->entities[1], source.entities[3]});
    const auto copied = source.flatten(false);
    REQUIRE(leaves.size() == copied.entities.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        CHECK(leaves[i] != copied.entities[i]);
        CHECK(leaves[i]->role() == copied.entities[i]->role());
        CHECK(leaves[i]->is_loop() == copied.entities[i]->is_loop());
        Points original_points, copied_points;
        leaves[i]->collect_points(original_points);
        copied.entities[i]->collect_points(copied_points);
        CHECK(original_points == copied_points);
    }
    REQUIRE(ExtrusionEntityCollection{}.flatten_refs().empty());
}

namespace {
struct CpuReuseEnvironment
{
    const std::array<const char*, 3>          names{"ORCA_WALL_SUPPORT", "ORCA_SHELL_VOLUME", "ORCA_SHELL_PROJECTION"};
    std::array<std::optional<std::string>, 3> previous;
    explicit CpuReuseEnvironment(const char* mode)
    {
        for (size_t i = 0; i < names.size(); ++i) {
            if (const char* value = std::getenv(names[i]))
                previous[i] = value;
            set(names[i], mode);
        }
    }
    ~CpuReuseEnvironment()
    {
        for (size_t i = 0; i < names.size(); ++i)
            set(names[i], previous[i] ? previous[i]->c_str() : nullptr);
    }
    static void set(const char* name, const char* value)
    {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }
};
} // namespace
TEST_CASE("Shell reuse preserves fill geometry for adjoining material regions", "[CPUSlicing][ShellReuse]")
{
    auto slice = [](const char* mode, bool interfaces) {
        CpuReuseEnvironment environment(mode);
        auto                config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.set("layer_height", 0.2);
        config.set("initial_layer_print_height", 0.2);
        config.set("interface_shells", interfaces);
        config.set_key_value("ensure_vertical_shell_thickness", new ConfigOptionEnum<EnsureVerticalShellThickness>(evstAll));
        config.set("top_shell_layers", 5);
        config.set("bottom_shell_layers", 4);
        config.set("enable_prime_tower", false);
        config.set("single_extruder_multi_material", false);
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        Model model;
        auto* object = model.add_object();
        auto* left   = object->add_volume(make_cube(8., 10., 4.));
        left->config.set("extruder", 1);
        auto* right = object->add_volume(make_cube(8., 10., 3.));
        right->translate(Vec3d(8., 0., 0.));
        right->config.set("extruder", 2);
        for (const char* option : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"}) {
            left->config.set(option, 1);
            right->config.set(option, 2);
        }
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        object->ensure_on_bed();
        Print print;
        print.is_BBL_printer() = false;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        std::vector<Polygons> result;
        REQUIRE(print.get_object(0)->num_printing_regions() == 2);
        for (const Layer* layer : print.get_object(0)->layers())
            for (const LayerRegion* region : layer->regions())
                for (SurfaceType type : {stInternal, stInternalVoid, stInternalSolid})
                    result.push_back(to_polygons(region->fill_surfaces.filter_by_type(type)));
        return result;
    };
    for (bool interfaces : {false, true}) {
        const auto reference = slice("cpu", interfaces);
        const auto reused    = slice("validate", interfaces);
        REQUIRE(reused == reference);
    }
}

TEST_CASE("Tree overhang geometry and labels agree across worker counts", "[CPUSlicing][TreeOverhang]")
{
    struct Snapshot
    {
        std::vector<ExPolygons>       overhangs, cantilevers;
        std::vector<std::vector<int>> types;
        bool                          sharp_tail, cantilever;
        double                        max_cantilever;
    };
    struct FilterEnvironment
    {
        std::optional<std::string> previous;
        explicit FilterEnvironment(const char* mode)
        {
            if (const char* value = std::getenv("ORCA_TREE_OVERHANG_FILTER"))
                previous = value;
            CpuReuseEnvironment::set("ORCA_TREE_OVERHANG_FILTER", mode);
        }
        ~FilterEnvironment() { CpuReuseEnvironment::set("ORCA_TREE_OVERHANG_FILTER", previous ? previous->c_str() : nullptr); }
    };
    auto detect = [](int workers, bool critical_only, const char* mode, bool cancel = false) {
        FilterEnvironment environment(mode);
        auto              config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.set("layer_height", 0.2);
        config.set("initial_layer_print_height", 0.2);
        config.set("enable_support", true);
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
        config.set("support_remove_small_overhang", false);
        config.set("support_critical_regions_only", critical_only);
        config.set("max_bridge_length", 10.0);
        config.set("enable_prime_tower", false);
        config.set("single_extruder_multi_material", false);
        config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
        config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
        Model model;
        auto* object  = model.add_object();
        auto  add_box = [&](double x, double z, double width, double height, int extruder) {
            auto* volume = object->add_volume(make_cube(width, 10., height));
            volume->translate(Vec3d(x, 0., z));
            volume->config.set("extruder", extruder);
            for (const char* key : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"})
                volume->config.set(key, extruder);
            return volume;
        };
        add_box(0., 0., 4., 3., 1);
        add_box(20., 0., 4., 3., 1);
        add_box(0., 3., 30., 1., 1);
        add_box(30., 3., 4., 1., 2);
        add_box(42., 1., 3., 1., 1); // Floating component exercises sharp-tail propagation.
        add_box(12., 0., 3., 5., 1)->set_type(ModelVolumeType::SUPPORT_ENFORCER);
        add_box(27., 0., 2., 5., 1)->set_type(ModelVolumeType::SUPPORT_BLOCKER);
        object->add_instance()->set_offset(Vec3d(100., 100., 0.));
        Print print;
        print.is_BBL_printer() = false;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        auto* printed = print.get_object(0);
        // Repeat detection on finished perimeters/surfaces with fresh derived state.
        for (Layer* layer : printed->layers()) {
            layer->cantilevers.clear();
            layer->sharp_tails.clear();
            layer->sharp_tails_height.clear();
        }
        REQUIRE(printed->num_printing_regions() >= 2);
        TreeSupport     support(*printed, printed->slicing_parameters());
        tbb::task_arena arena(workers);
        if (cancel) {
            std::atomic<int> callbacks{0};
            support.throw_on_cancel = [&] {
                if (callbacks.fetch_add(1) == 2)
                    print.cancel();
                if (print.canceled())
                    throw CanceledException();
            };
            REQUIRE_THROWS_AS(arena.execute([&] { support.detect_overhangs(); }), CanceledException);
            CHECK(support.overhang_types.empty()); // No partial pointer map is published.
            return Snapshot{};
        }
        arena.execute([&] { support.detect_overhangs(); });
        Snapshot result;
        for (const Layer* layer : printed->layers()) {
            result.overhangs.push_back(layer->loverhangs);
            result.cantilevers.push_back(layer->cantilevers);
            std::vector<int> types;
            for (const auto& polygon : layer->loverhangs) {
                const auto found = support.overhang_types.find(&polygon);
                REQUIRE(found != support.overhang_types.end());
                types.push_back(int(found->second));
            }
            result.types.push_back(std::move(types));
        }
        result.sharp_tail     = support.has_sharp_tails;
        result.cantilever     = support.has_cantilever;
        result.max_cantilever = support.max_cantilever_dist;
        REQUIRE(std::any_of(result.overhangs.begin(), result.overhangs.end(), [](const auto& p) { return !p.empty(); }));
        REQUIRE(result.cantilever);
        REQUIRE(result.max_cantilever > scale_(3.));
        return result;
    };
    for (bool critical_only : {false, true}) {
        const auto reference = detect(1, critical_only, "serial");
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto parallel = detect(4, critical_only, repeat == 0 ? "parallel" : "validate");
            CHECK(parallel.overhangs == reference.overhangs);
            CHECK(parallel.cantilevers == reference.cantilevers);
            CHECK(parallel.types == reference.types);
            CHECK(parallel.sharp_tail == reference.sharp_tail);
            CHECK(parallel.cantilever == reference.cantilever);
            CHECK(parallel.max_cantilever == reference.max_cantilever);
        }
    }
    detect(4, false, "parallel", true);
}
