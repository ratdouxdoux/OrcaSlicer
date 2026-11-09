#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/GlossPainting.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/filesystem.hpp>
#include <limits>

using namespace Slic3r;
using Catch::Approx;

namespace {
ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    return ExPolygon(
        Polygon{Points{Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)}});
}

struct TemporaryProject
{
    std::string path = (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("gloss-%%%%-%%%%.3mf")).string();
    ~TemporaryProject()
    {
        boost::system::error_code error;
        boost::filesystem::remove(path, error);
    }
};
} // namespace

TEST_CASE("Gloss speed requests respect motion flow and surface caps", "[GlossPainting][fast]")
{
    auto range = gloss_speed_range(200., 10., 300., 500., 150., 20., 12., 0.1, 0.04);
    CHECK(range.max_speed == Approx(120.));
    CHECK(range.min_time() == Approx(100. / 120.));
    CHECK(range.max_time() == Approx(10.));
    CHECK(range.speed_for_time(2.) == Approx(50.));
    CHECK(range.speed_for_time(0.01) == Approx(120.));
    CHECK(range.speed_for_time(10000.) == Approx(10.));
    CHECK(range.speed_for_time(std::numeric_limits<double>::quiet_NaN()) == Approx(120.));
    CHECK(range.speed_for_time(-1.) == Approx(120.));
    CHECK(range.speed_for_time(0.) == Approx(120.));
    range = gloss_speed_range(200., 10., 300., 500., 150., 1., 12., 0.1, 0.2);
    CHECK(range.max_speed == Approx(5.));
    CHECK(range.min_speed == Approx(5.));
    CHECK(range.speed_for_time(1000.) == Approx(5.));
    range = gloss_speed_range(35., 0., 0., 0., 0., 0., 0., 0.1, 0.04);
    CHECK(range.max_speed == Approx(35.));
    CHECK(range.min_speed == Approx(1.));
}

TEST_CASE("Gloss boundaries preserve path order length holes and unpainted spans", "[GlossPainting][fast]")
{
    GlossPaintRegions masks;
    masks[0]     = {rectangle(2., -1., 8., 1.)};
    Polygon hole = rectangle(4., -0.5, 6., 0.5).contour;
    hole.reverse();
    masks[0].front().holes.push_back(hole);
    masks[1]             = {rectangle(7., -1., 9., 1.)}; // First color wins in the overlap.
    const Polyline path  = Polyline::new_scale({{0., 0.}, {0., 0.}, {10., 0.}, {10., 2.}, {0., 2.}, {0., 0.}});
    auto           parts = split_gloss_path(path, masks);
    REQUIRE(parts.size() == 6);
    CHECK(parts[0].color == 0);
    CHECK(parts[1].color == 1);
    CHECK(parts[2].color == 0);
    CHECK(parts[3].color == 1);
    CHECK(parts[4].color == 2);
    CHECK(parts[5].color == 0);
    CHECK(parts.front().polyline.first_point() == path.first_point());
    CHECK(parts.back().polyline.last_point() == path.last_point());
    double length = 0.;
    for (size_t i = 0; i < parts.size(); ++i) {
        length += parts[i].polyline.length();
        if (i > 0)
            CHECK(parts[i - 1].polyline.last_point() == parts[i].polyline.first_point());
    }
    CHECK(length == Approx(path.length()));
    CHECK(unscale<double>(parts[4].polyline.length()) == Approx(1.));
    const auto reversed = split_gloss_path(Polyline::new_scale({{10., 0.}, {0., 0.}}), masks);
    REQUIRE(reversed.size() == 6);
    CHECK(reversed[1].color == 2);
    CHECK(reversed[1].polyline.first_point() == Point::new_scale(9., 0.));
    CHECK(reversed[1].polyline.last_point() == Point::new_scale(8., 0.));
}

TEST_CASE("Unpainted gloss paths retain geometry", "[GlossPainting][fast]")
{
    const Polyline path  = Polyline::new_scale({{0., 0.}, {3., 5.}, {8., 2.}});
    const auto     parts = split_gloss_path(path, GlossPaintRegions{});
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].color == 0);
    CHECK(parts[0].polyline.points == path.points);
    CHECK(split_gloss_path(Polyline{}, GlossPaintRegions{}).empty());
}

TEST_CASE("Gloss paint survives cloning and both 3MF formats independently of filament paint", "[GlossPainting][3mf]")
{
    Model        source;
    ModelObject* object = source.add_object();
    ModelVolume* volume = object->add_volume(TriangleMesh(its_make_cube(10., 10., 2.)));
    object->add_instance();
    TriangleSelector gloss(volume->mesh());
    TriangleSelector filament(volume->mesh());
    for (int facet = 0; facet < int(volume->mesh().facets_count()); ++facet)
        gloss.set_facet(facet, static_cast<EnforcerBlockerType>(facet % GLOSS_PAINT_COLORS + 1));
    filament.set_facet(0, EnforcerBlockerType::Extruder1);
    volume->gloss_facets.set(gloss);
    volume->mmu_segmentation_facets.set(filament);
    object->config.set_key_value("gloss_paint_times", new ConfigOptionFloats{1.5, 3., 5., 9.});

    Model copy = source;
    REQUIRE(copy.objects.front()->volumes.front()->gloss_facets.equals(volume->gloss_facets));
    CHECK_FALSE(model_gloss_data_changed(*object, *copy.objects.front()));
    copy.objects.front()->volumes.front()->gloss_facets.reset();
    CHECK(model_gloss_data_changed(*object, *copy.objects.front()));
    CHECK(source.is_gloss_painted());
    CHECK_FALSE(copy.is_gloss_painted());

    TemporaryProject   file;
    Model              restored;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Full-default vector enums need their dictionaries restored for export,
    // as in the existing mixed-filament serialization fixtures.
    for (const auto& key : config.keys())
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    SECTION("Prusa 3MF")
    {
        REQUIRE(store_3mf(file.path.c_str(), &source, &config, false));
        REQUIRE(load_3mf(file.path.c_str(), config, substitutions, &restored, false));
    }
    SECTION("Orca project 3MF")
    {
        StoreParams params;
        params.path     = file.path.c_str();
        params.model    = &source;
        params.config   = &config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(params));
        PlateDataPtrs        plates;
        std::vector<Preset*> presets;
        bool                 is_bbl = false;
        Semver               version;
        REQUIRE(load_bbs_3mf(file.path.c_str(), &config, &substitutions, &restored, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
        for (auto* plate : plates)
            delete plate;
        for (auto* preset : presets)
            delete preset;
    }
    REQUIRE(restored.objects.size() == 1);
    REQUIRE(restored.objects[0]->volumes.size() == 1);
    CHECK(restored.objects[0]->volumes[0]->gloss_facets.equals(volume->gloss_facets));
    CHECK(restored.objects[0]->volumes[0]->mmu_segmentation_facets.equals(volume->mmu_segmentation_facets));
    const auto* times = restored.objects[0]->config.get().option<ConfigOptionFloats>("gloss_paint_times");
    REQUIRE(times != nullptr);
    CHECK(times->values == std::vector<double>{1.5, 3., 5., 9.});
}
